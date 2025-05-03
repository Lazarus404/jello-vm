// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Jello processes and their schedulers. A process is a VM on a pooled slot, sharing its parent's
 * heap, module, frame layouts and JIT code, running on its own native stack. JIT code calls only
 * above JIT_NATIVE_RESERVE from the stack's bottom, and the interpreter enters JIT code only
 * above PROC_JIT_ENTRY_MARGIN more, as on a thread's stack.
 *
 * A main process's processes run on N schedulers, each an OS thread: scheduler 0 is the main
 * process's own, and the only one to run it. A process runs until its window of reductions ends
 * with another waiting on its scheduler and none idle, then goes to the back of that one's run
 * queue, or until it parks to wait for a message, for another process to finish, or for a time.
 * A scheduler with nothing to run takes half of another's run queue.
 *
 * No lock is held across a switch. A process switching away is resumed by nobody until its stack
 * is saved: whatever runs next on its scheduler queues it, if it yielded or was woken while it
 * parked, or frees it, if it finished, since nothing can free the stack it is running on. A
 * process switched out leaves its VM quiescent, and its frames are complete at every safepoint
 * and native call. A collection reads no inbox, timer or reply, so it takes no scheduler's
 * lock. */

#include <jello/internal.h>
#include <jello/internal/platform.h>
#include <jello/internal/proc_internal.h>

#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PROC_CLEANUP_REDUCTIONS 100000u
#define PROC_JIT_ENTRY_MARGIN (16u * 1024u)
#define PROC_NEXT_STREAK 16u
#define PROC_SPIN_NS 20000u

uint64_t proc_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t proc_deadline(int32_t ms) { return ms < 0 ? 0 : proc_now() + (uint64_t)ms * 1000000u; }

static void lock(proc_sched* s) { pthread_mutex_lock(s->lock); }
static void unlock(proc_sched* s) { pthread_mutex_unlock(s->lock); }
static void idle_lock(proc_sched* s) { pthread_mutex_lock(&s->idle_lock); }
static void idle_unlock(proc_sched* s) { pthread_mutex_unlock(&s->idle_lock); }
static void pause_briefly(void) {
#if defined(__aarch64__)
  __asm__ __volatile__("isb" ::: "memory");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

static void queue_lock(proc_worker* w) {
  uint32_t spins = 0;
  while(atomic_exchange_explicit(&w->mu, 1, memory_order_acquire)) {
    while(atomic_load_explicit(&w->mu, memory_order_relaxed)) {
      if(++spins % 64u == 0) sched_yield();
      else pause_briefly();
    }
  }
}

static void queue_unlock(proc_worker* w) { atomic_store_explicit(&w->mu, 0, memory_order_release); }

static void wait_until(pthread_cond_t* cv, pthread_mutex_t* mu, uint64_t due) {
  if(!due) {
    pthread_cond_wait(cv, mu);
    return;
  }
  uint64_t now = proc_now();
  if(due <= now) return;
#if defined(__APPLE__) || defined(_WIN32)
  uint64_t ns = due - now;
  struct timespec rel = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
  pthread_cond_timedwait_relative_np(cv, mu, &rel);
#else
  struct timespec at = {(time_t)(due / 1000000000u), (long)(due % 1000000000u)};
  pthread_cond_timedwait(cv, mu, &at);
#endif
}

static _Thread_local proc_worker* this_worker;
static _Thread_local const proc_sched* this_sched;

static void be_worker(proc_worker* w) {
  this_worker = w;
  this_sched = w->sched;
}

static proc_worker* home(proc_sched* s, const jello_process* p) {
  if(p->pinned) return s->workers;
  return this_sched == s ? this_worker : p->worker;
}

static int any_ready(const proc_sched* s) {
  for(uint32_t i = 0; i < s->nworkers; i++) {
    if(atomic_load(&s->workers[i].len)) return 1;
  }
  return 0;
}

static void wake_one(proc_sched* s, pthread_cond_t* cv) {
  idle_lock(s);
  pthread_cond_signal(cv);
  idle_unlock(s);
}

static void unpark_worker(proc_worker* w) {
  pthread_mutex_lock(&w->park_mu);
  w->woken = 1;
  pthread_cond_signal(&w->park_cv);
  pthread_mutex_unlock(&w->park_mu);
}

static void wake_idle(proc_sched* s) {
  for(uint32_t i = 0; i < PROC_IDLE_WORDS; i++) {
    uint64_t bits = atomic_load_explicit(&s->idle_mask[i], memory_order_relaxed);
    while(bits) {
      uint64_t bit = bits & (~bits + 1u);
      uint64_t was = atomic_fetch_and(&s->idle_mask[i], ~bit);
      if(was & bit) {
        unpark_worker(&s->workers[i * 64u + (uint32_t)__builtin_ctzll(bit)]);
        return;
      }
      bits = was & ~bit;
    }
  }
}

static void queue_append(proc_worker* w, jello_process* first, jello_process* last, uint32_t n) {
  last->queued = NULL;
  if(w->tail) w->tail->queued = first;
  else w->head = first;
  w->tail = last;
  if(n) atomic_fetch_add(&w->len, n);
}

static void make_ready(proc_sched* s, jello_process* p) {
  proc_worker* w = home(s, p);
  queue_lock(w);
  queue_append(w, p, p, !p->pinned);
  if(p->pinned) atomic_fetch_add(&s->pinned_ready, 1);
  queue_unlock(w);
  if(!p->pinned && atomic_load(&s->idle) && !atomic_load(&s->spinning)) wake_idle(s);
  else if(atomic_exchange(&s->main_waiting, 0)) wake_one(s, &s->main_cv);
}

static void make_ready_next(proc_sched* s, jello_process* p) {
  if(this_sched != s || s->nworkers == 1 || p->pinned) {
    make_ready(s, p);
    return;
  }
  jello_process* last = this_worker->next;
  this_worker->next = p;
  if(last) make_ready(s, last);
}

static int queue_waits(const proc_sched* s, proc_worker* w) {
  return atomic_load_explicit(&w->len, memory_order_relaxed) ||
         (w == s->workers && atomic_load_explicit(&s->pinned_ready, memory_order_relaxed));
}

static jello_process* take_own(proc_sched* s, proc_worker* w) {
  jello_process* p = w->next;
  int waits = queue_waits(s, w);
  if(p && (!waits || w->streak < PROC_NEXT_STREAK)) {
    w->next = NULL;
    w->streak = waits ? w->streak + 1u : 0;
    return p;
  }
  w->streak = 0;
  if(!waits) return NULL;
  queue_lock(w);
  p = w->head;
  if(p) {
    w->head = p->queued;
    if(!w->head) w->tail = NULL;
    if(p->pinned) atomic_fetch_sub_explicit(&s->pinned_ready, 1, memory_order_relaxed);
    else atomic_fetch_sub_explicit(&w->len, 1, memory_order_relaxed);
  }
  queue_unlock(w);
  if(p) return p;
  /* Its queue was taken meanwhile. */
  p = w->next;
  w->next = NULL;
  return p;
}

static jello_process* steal(proc_sched* s, proc_worker* w) {
  uint32_t n = s->nworkers;
  uint32_t self = (uint32_t)(w - s->workers);
  for(uint32_t i = 1; i < n; i++) {
    proc_worker* v = &s->workers[(self + i) % n];
    if(!atomic_load_explicit(&v->len, memory_order_relaxed)) continue;
    queue_lock(v);
    uint32_t want = (atomic_load_explicit(&v->len, memory_order_relaxed) + 1u) / 2u;
    jello_process* first = NULL;
    jello_process* last = NULL;
    jello_process* kept = NULL; /* The last pinned process passed over. */
    uint32_t got = 0;
    for(jello_process* p = v->head; p && got < want; p = kept ? kept->queued : v->head) {
      if(p->pinned) {
        kept = p;
        continue;
      }
      if(kept) kept->queued = p->queued;
      else v->head = p->queued;
      if(v->tail == p) v->tail = kept;
      if(last) last->queued = p;
      else first = p;
      last = p;
      got++;
    }
    if(got) atomic_fetch_sub_explicit(&v->len, got, memory_order_relaxed);
    queue_unlock(v);
    if(!got) continue;
    if(got > 1) {
      queue_lock(w);
      queue_append(w, first->queued, last, got - 1u);
      queue_unlock(w);
    }
    first->queued = NULL;
    return first;
  }
  return NULL;
}

static jello_process* take(proc_sched* s, proc_worker* w) {
  jello_process* p = take_own(s, w);
  return p ? p : steal(s, w);
}

static void timer_place(proc_sched* s, proc_timer* t, uint32_t i) {
  s->timers[i] = t;
  t->at = i;
}

static void timer_settle(proc_sched* s, uint32_t i) {
  proc_timer* t = s->timers[i];
  while(i > 0 && s->timers[(i - 1u) / 2u]->due > t->due) {
    timer_place(s, s->timers[(i - 1u) / 2u], i);
    i = (i - 1u) / 2u;
  }
  for(;;) {
    uint32_t c = 2u * i + 1u;
    if(c >= s->ntimers) break;
    if(c + 1u < s->ntimers && s->timers[c + 1u]->due < s->timers[c]->due) c++;
    if(s->timers[c]->due >= t->due) break;
    timer_place(s, s->timers[c], i);
    i = c;
  }
  timer_place(s, t, i);
}

static void publish_next_due(proc_sched* s) {
  atomic_store_explicit(&s->next_due, s->ntimers ? s->timers[0]->due : 0, memory_order_relaxed);
}

static void timers_changed(proc_sched* s) {
  publish_next_due(s);
  atomic_thread_fence(memory_order_seq_cst);
  if(atomic_load(&s->idle)) wake_idle(s);
  idle_lock(s);
  if(atomic_load(&s->main_waiting)) pthread_cond_signal(&s->main_cv);
  idle_unlock(s);
}

static void timer_arm(proc_sched* s, proc_timer* t, uint64_t due) {
  if(s->ntimers == s->timers_cap) {
    s->timers_cap = s->timers_cap ? s->timers_cap * 2u : 16u;
    s->timers = (proc_timer**)realloc(s->timers, sizeof(proc_timer*) * s->timers_cap);
    if(!s->timers) abort();
  }
  t->due = due;
  timer_place(s, t, s->ntimers++);
  timer_settle(s, t->at);
  if(t->at == 0) timers_changed(s);
}

static void timer_disarm(proc_sched* s, proc_timer* t) {
  uint32_t i = t->at;
  t->at = PROC_TIMER_IDLE;
  proc_timer* last = s->timers[--s->ntimers];
  if(last != t) {
    timer_place(s, last, i);
    timer_settle(s, i);
  }
  if(i == 0) publish_next_due(s);
}

static void live_unlink(proc_sched* s, jello_process* p) {
  if(p->live_prev) p->live_prev->live_next = p->live_next;
  else s->live = p->live_next;
  if(p->live_next) p->live_next->live_prev = p->live_prev;
}

static void join_unlink(jello_process* p) {
  jello_process* target = p->joining;
  if(p->join_prev) p->join_prev->join_next = p->join_next;
  else target->joiners = p->join_next;
  if(p->join_next) p->join_next->join_prev = p->join_prev;
  p->joining = NULL;
}

void proc_retain(jello_process* p) { atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed); }

static void timer_drop(proc_timer* t, vm_parcel* dropped) {
  if(t->prev) t->prev->next = t->next;
  else t->target->sends = t->next;
  if(t->next) t->next->prev = t->prev;
  vm_parcel_join(dropped, &t->msg);
  proc_timer_release(t);
}

void proc_timer_release(proc_timer* t) {
  if(atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1) free(t);
}

static int timer_cancel(proc_sched* s, proc_timer* t, vm_parcel* dropped) {
  if(t->at == PROC_TIMER_IDLE) return 0;
  timer_disarm(s, t);
  timer_drop(t, dropped);
  return 1;
}

static uint64_t timers_due(const proc_sched* s) {
  uint64_t due = atomic_load_explicit(&s->next_due, memory_order_relaxed);
  if(!due) return 0;
  uint64_t now = proc_now();
  return due <= now ? now : 0;
}

static int stalled(const proc_sched* s) {
  return atomic_load(&s->idle) + 1u == s->started &&
         !atomic_load_explicit(&s->next_due, memory_order_relaxed) && !atomic_load(&s->outside) &&
         !any_ready(s) && !atomic_load(&s->pinned_ready);
}

static void sibling_unlink(jello_process* p) {
  if(p->sibling_prev) p->sibling_prev->sibling_next = p->sibling_next;
  else p->owner->children = p->sibling_next;
  if(p->sibling_next) p->sibling_next->sibling_prev = p->sibling_prev;
  p->owner = NULL;
}

static int spin(proc_sched* s) {
  if(2u * atomic_load(&s->spinning) >= s->nworkers - atomic_load(&s->idle)) return 0;
  atomic_fetch_add(&s->spinning, 1);
  int seen = 0;
  uint64_t until = proc_now() + PROC_SPIN_NS;
  for(uint32_t i = 1; !(seen = any_ready(s)); i++) {
    if(i % 32u == 0 && (timers_due(s) || proc_now() >= until)) break;
    pause_briefly();
  }
  if(atomic_fetch_sub(&s->spinning, 1) == 1 && seen && atomic_load(&s->idle)) wake_idle(s);
  return seen;
}

static void park_worker(proc_sched* s, proc_worker* w) {
  uint32_t i = (uint32_t)(w - s->workers);
  uint64_t bit = 1ull << (i % 64u);
  _Atomic uint64_t* word = &s->idle_mask[i / 64u];
  atomic_fetch_or(word, bit);
  atomic_fetch_add(&s->idle, 1);
  uint64_t due = atomic_load(&s->next_due);
  if(!any_ready(s) && !atomic_load(&s->shutdown) && !(due && due <= proc_now())) {
    if(atomic_load(&s->main_waiting) && stalled(s)) wake_one(s, &s->main_cv);
    pthread_mutex_lock(&w->park_mu);
    if(!w->woken) wait_until(&w->park_cv, &w->park_mu, due);
    w->woken = 0;
    pthread_mutex_unlock(&w->park_mu);
  }
  atomic_fetch_and(word, ~bit);
  atomic_fetch_sub(&s->idle, 1);
}

static uint32_t scheduler_count(void) {
  const char* env = getenv("JELLO_SCHEDULERS");
  char* end = NULL;
  long n = env && *env ? strtol(env, &end, 10) : 0;
  if(n < 1 || *end) n = jello_cpu_count();
  return n > (long)PROC_MAX_SCHEDULERS ? PROC_MAX_SCHEDULERS : (uint32_t)n;
}

static jello_process* record_new(void) {
  jello_process* p = (jello_process*)calloc(1, sizeof(jello_process));
  if(!p) return NULL;
  if(pthread_mutex_init(&p->mu, NULL) != 0) {
    free(p);
    return NULL;
  }
  atomic_init(&p->refs, 1);
  p->exit_value = jello_make_null();
  p->exit = vm_parcel_empty();
  p->deadline.at = PROC_TIMER_IDLE;
  p->deadline.target = p;
  p->deadline.msg = vm_parcel_empty();
  return p;
}

static proc_sched* main_sched(const jello_vm* vm) {
  proc_sched* s = vm->proc ? vm->proc->sched : NULL;
  return s && vm->proc == s->main ? s : NULL;
}

static proc_sched* sched_new(jello_vm* vm, jello_process* m) {
  proc_sched* s = (proc_sched*)calloc(1, sizeof(proc_sched));
  uint32_t n = scheduler_count();
  proc_worker* workers =
      s ? (proc_worker*)jello_aligned_alloc(PROC_CACHE_LINE, n * sizeof(proc_worker)) : NULL;
  if(!workers) {
    free(s);
    return NULL;
  }
  s->lock = &vm->heap->proc_lock;
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
#if !defined(__APPLE__) && !defined(_WIN32)
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
  if(pthread_cond_init(&s->main_cv, &attr) != 0 || pthread_cond_init(&s->native_cv, NULL) != 0 ||
     pthread_mutex_init(&s->idle_lock, NULL) != 0 || pthread_mutex_init(&s->pool_lock, NULL) != 0 ||
     pthread_mutex_init(&s->native_lock, NULL) != 0 || pthread_mutex_init(&s->poll_lock, NULL) != 0)
    abort();
  memset(workers, 0, n * sizeof(proc_worker));
  s->workers = workers;
  s->nworkers = n;
  s->started = 1;
  for(uint32_t i = 0; i < n; i++) {
    workers[i].sched = s;
    if(pthread_mutex_init(&workers[i].park_mu, NULL) != 0 ||
       pthread_cond_init(&workers[i].park_cv, &attr) != 0)
      abort();
  }
  pthread_condattr_destroy(&attr);
  be_worker(workers);
  m->pinned = 1;
  s->pool = proc_pool_make(PROC_NATIVE_STACK_BYTES, PROC_FRAME_STACK_BYTES);
  s->main = m;
  s->epoch = proc_now();
  return s;
}

static int others_wait(const proc_sched* s, const jello_process* p) {
  proc_worker* w = p->worker;
  if(w == s->workers && atomic_load_explicit(&s->pinned_ready, memory_order_relaxed)) return 1;
  if(w->next) return 1;
  return atomic_load_explicit(&w->len, memory_order_relaxed) &&
         !atomic_load_explicit(&s->idle, memory_order_relaxed);
}

void proc_leave_to_host(jello_vm* vm) {
  proc_sched* s = main_sched(vm);
  if(!s || this_worker != s->workers) return;
  jello_process* p = s->workers->next;
  s->workers->next = NULL;
  if(p) make_ready(s, p);
}

int proc_on_main(const jello_vm* vm) {
  const jello_process* p = vm->proc;
  return !p || p->worker == p->sched->workers;
}

static int expired(uint64_t deadline) { return deadline && proc_now() >= deadline; }

static void deadline_arm(proc_sched* s, jello_process* p, uint64_t deadline) {
  if(!deadline) return;
  lock(s);
  timer_arm(s, &p->deadline, deadline);
  unlock(s);
}

static void deadline_disarm(proc_sched* s, jello_process* p) {
  if(p->deadline.at != PROC_TIMER_IDLE) timer_disarm(s, &p->deadline);
}
