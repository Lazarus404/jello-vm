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

static int resume(jello_process* p) {
  p->parked = PROC_PARK_NONE;
  return !p->off;
}

static int unpark(jello_process* p) {
  if(p->parked == PROC_PARK_NONE || p->parked == PROC_PARK_NATIVE) return 0;
  return resume(p);
}

static void wake(proc_sched* s, jello_process* p) {
  pthread_mutex_lock(&p->mu);
  int queue = unpark(p);
  pthread_mutex_unlock(&p->mu);
  if(queue) make_ready_next(s, p);
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

static void inbox_free(jello_process* p, vm_parcel* dropped) {
  for(uint32_t i = 0; i < p->inbox_len; i++) vm_parcel_join(dropped, proc_inbox_at(p, i));
  free(p->inbox);
  p->inbox = NULL;
  p->inbox_len = 0;
  p->inbox_cap = 0;
}

void proc_release(jello_process* p) {
  if(atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) != 1) return;
  pthread_mutex_destroy(&p->mu);
  vm_parcel dropped = vm_parcel_empty();
  inbox_free(p, &dropped);
  vm_parcel_free(&dropped);
  vm_parcel_free(&p->exit);
  free(p);
}

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

static int deliver(jello_process* p, vm_parcel* msg) {
  if(p->inbox_len == p->inbox_cap) {
    uint32_t cap = p->inbox_cap ? p->inbox_cap * 2u : 4u;
    vm_parcel* ring = (vm_parcel*)malloc(sizeof(vm_parcel) * cap);
    if(!ring) abort();
    for(uint32_t i = 0; i < p->inbox_len; i++) ring[i] = *proc_inbox_at(p, i);
    free(p->inbox);
    p->inbox = ring;
    p->inbox_cap = cap;
    p->inbox_head = 0;
  }
  vm_parcel_move(proc_inbox_at(p, p->inbox_len++), msg);
  return p->parked == PROC_PARK_RECEIVE && unpark(p);
}

static void fire_due(proc_sched* s, uint64_t now) {
  while(s->ntimers && s->timers[0]->due <= now) {
    proc_timer* t = s->timers[0];
    timer_disarm(s, t);
    jello_process* p = t->target;
    int deadline = t == &p->deadline;
    pthread_mutex_lock(&p->mu);
    int queue = deadline ? unpark(p) : deliver(p, &t->msg);
    pthread_mutex_unlock(&p->mu);
    if(queue) make_ready(s, p);
    vm_parcel delivered = vm_parcel_empty(); /* What timer_drop finds once deliver took it. */
    if(!deadline) timer_drop(t, &delivered);
  }
}

static uint64_t timers_due(const proc_sched* s) {
  uint64_t due = atomic_load_explicit(&s->next_due, memory_order_relaxed);
  if(!due) return 0;
  uint64_t now = proc_now();
  return due <= now ? now : 0;
}

static void fire_at(proc_sched* s, uint64_t now) {
  lock(s);
  fire_due(s, now);
  unlock(s);
}

static void fire_if_due(proc_sched* s) {
  uint64_t now = timers_due(s);
  if(now) fire_at(s, now);
}

static void reap(proc_sched* s, jello_process* p) {
#ifdef PROC_TSAN
  __tsan_destroy_fiber(p->fiber);
#endif
  vm_destroy_attached(p->vm);
  p->vm = NULL;
  pthread_mutex_lock(&s->pool_lock);
  proc_slot_give(p->pool, p->slot);
  pthread_mutex_unlock(&s->pool_lock);
  proc_release(p);
}

static void switched(proc_sched* s, proc_worker* w) {
  jello_process* p = w->prev;
  if(!p) return;
  w->prev = NULL;
  switch(w->left) {
    case PROC_LEFT_PARKED: {
      pthread_mutex_lock(&p->mu);
      p->off = 0;
      int woken = p->parked == PROC_PARK_NONE;
      pthread_mutex_unlock(&p->mu);
      if(woken) make_ready(s, p);
      break;
    }
    case PROC_LEFT_YIELDED: make_ready(s, p); break;
    default: reap(s, p); break;
  }
}

static int host_due(const proc_worker* w) { return w->run_until && proc_now() >= w->run_until; }

static void switch_to(proc_sched* s, proc_worker* w, jello_process* me, jello_process* next,
                      uint8_t left) {
  w->switches++;
  w->prev = left == PROC_LEFT_NONE ? NULL : me;
  w->left = left;
  if(next) next->worker = w;
#ifdef PROC_TSAN
  __tsan_switch_to_fiber(next ? next->fiber : w->fiber, 0);
#endif
  proc_stack_switch(me ? &me->sp : &w->idle_sp, next ? next->sp : w->idle_sp);
  switched(s, me ? me->worker : w);
}

static void run_next(proc_sched* s, jello_process* p, uint8_t left) {
  proc_worker* w = p->worker;
  jello_process* next = host_due(w) ? NULL : take(s, w);
  if(!next && w == s->workers) {
    next = s->main;
    s->main_idle = 1;
  }
  switch_to(s, w, p, next, left);
}

static int stalled(const proc_sched* s) {
  return atomic_load(&s->idle) + 1u == s->started &&
         !atomic_load_explicit(&s->next_due, memory_order_relaxed) && !atomic_load(&s->outside) &&
         !any_ready(s) && !atomic_load(&s->pinned_ready);
}

static jello_trap_code main_wait(proc_sched* s) {
  jello_process* m = s->main;
  proc_worker* w = s->workers;
  for(;;) {
    jello_process* next = host_due(w) ? NULL : take(s, w);
    if(next == m) return JELLO_TRAP_NONE;
    if(next) {
      switch_to(s, w, m, next, PROC_LEFT_NONE);
      if(!s->main_idle) return JELLO_TRAP_NONE;
      s->main_idle = 0;
      continue;
    }
    uint64_t now = timers_due(s);
    if(now) {
      fire_at(s, now);
      continue;
    }
    if(w->run_until) return JELLO_TRAP_NONE;
    idle_lock(s);
    atomic_store(&s->main_waiting, 1);
    jello_trap_code trap = JELLO_TRAP_NONE;
    if(!atomic_load(&s->pinned_ready) && !any_ready(s)) {
      if(stalled(s)) trap = JELLO_TRAP_DEADLOCK;
      else
        wait_until(&s->main_cv, &s->idle_lock,
                   atomic_load_explicit(&s->next_due, memory_order_relaxed));
    }
    atomic_store(&s->main_waiting, 0);
    idle_unlock(s);
    if(trap != JELLO_TRAP_NONE) return trap;
  }
}

static jello_trap_code park(proc_sched* s, jello_vm* vm, uint8_t why, int locked) {
  jello_process* p = vm->proc;
  int stopped = why != PROC_PARK_NATIVE && (p->stopping || p->cleaning);
  if(!stopped) {
    p->parked = why;
    p->off = p != s->main;
  }
  pthread_mutex_unlock(&p->mu);
  if(locked) unlock(s);
  if(stopped) return JELLO_TRAP_STOPPED;
  vm_go_quiescent(vm);
  jello_trap_code trap = JELLO_TRAP_NONE;
  if(p != s->main) {
    run_next(s, p, PROC_LEFT_PARKED);
  } else if((trap = main_wait(s)) != JELLO_TRAP_NONE) {
    pthread_mutex_lock(&p->mu);
    p->parked = PROC_PARK_NONE;
    pthread_mutex_unlock(&p->mu);
  }
  vm_leave_quiescent(vm);
  return p->stopping ? JELLO_TRAP_STOPPED : trap;
}

static void sibling_unlink(jello_process* p) {
  if(p->sibling_prev) p->sibling_prev->sibling_next = p->sibling_next;
  else p->owner->children = p->sibling_next;
  if(p->sibling_next) p->sibling_next->sibling_prev = p->sibling_prev;
  p->owner = NULL;
}

static int stop(proc_sched* s, jello_process* p, proc_exit why) {
  pthread_mutex_lock(&p->mu);
  int running = !p->done;
  int queue = 0;
  if(running) {
    if(!p->stopping) p->stopping = (uint8_t)why;
    queue = unpark(p);
  }
  pthread_mutex_unlock(&p->mu);
  if(queue) make_ready(s, p);
  return running;
}

static void run_cleanups(jello_process* p) {
  jello_vm* vm = p->vm;
  p->cleaning = 1;
  for(uint32_t i = p->ncleanups; i-- > 0;) {
    vm_frame_unwind(vm);
    p->cleanup_windows = 0;
    jello_numeric_result ignored;
    vm_exec_function(vm, (const jello_function*)jello_as_ptr(p->cleanups[i]), NULL, 0, &ignored);
  }
  vm_frame_unwind(vm);
  p->cleaning = 0;
  p->ncleanups = 0;
  free(p->cleanups);
  p->cleanups = NULL;
}

static void finish(proc_sched* s, jello_process* p, jello_exec_status st) {
  jello_vm* vm = p->vm;
  p->trap = st == JELLO_EXEC_OK ? JELLO_TRAP_NONE : vm->trap_code;
  p->exit_value = p->trap == JELLO_TRAP_UNCAUGHT_THROW ? vm->thrown : jello_make_null();
  if(p->ncleanups) run_cleanups(p);
  if(p->sockets) proc_sockets_end(p);
  vm_parcel_pack(vm, p->exit_value, 1, &p->exit);
  p->exit_value = jello_make_null();
  vm_go_quiescent(vm);
  vm_parcel dropped = vm_parcel_empty();
  lock(s);
  pthread_mutex_lock(&p->mu);
  inbox_free(p, &dropped);
  p->closed = 1;
  pthread_mutex_unlock(&p->mu);
  while(p->sends) timer_cancel(s, p->sends, &dropped);
  unlock(s);
  vm_parcel_free(&dropped);
  lock(s);
  pthread_mutex_lock(&p->mu);
  atomic_store_explicit(&p->done, 1, memory_order_release);
  pthread_mutex_unlock(&p->mu);
  while(p->joiners) {
    jello_process* j = p->joiners;
    join_unlink(j);
    wake(s, j);
  }
  live_unlink(s, p);
  if(p->owner) sibling_unlink(p);
  for(jello_process* c = p->children; c; c = c->sibling_next) {
    c->owner = NULL;
    stop(s, c, PROC_PARENT_EXITED);
  }
  p->children = NULL;
  if(!s->live) {
    pthread_mutex_lock(&s->main->mu);
    int queue = s->main->parked == PROC_PARK_ALL && unpark(s->main);
    pthread_mutex_unlock(&s->main->mu);
    if(queue) make_ready(s, s->main);
  }
  unlock(s);
}

static void proc_main(void* arg) {
  jello_process* p = (jello_process*)arg;
  proc_sched* s = p->sched;
  switched(s, p->worker);
  vm_leave_quiescent(p->vm);
  jello_exec_status st = JELLO_EXEC_TRAP;
  if(p->stopping) p->vm->trap_code = JELLO_TRAP_STOPPED;
  else st = vm_exec_function(p->vm, p->body, NULL, 0, &p->result);
  finish(s, p, st);
  run_next(s, p, PROC_LEFT_FINISHED);
  abort(); /* A finished process is never resumed. */
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

static void worker_loop(proc_worker* w) {
  proc_sched* s = w->sched;
  be_worker(w);
  for(;;) {
    jello_process* p = take(s, w);
    if(p) {
      switch_to(s, w, NULL, p, PROC_LEFT_NONE);
      continue;
    }
    uint64_t now = timers_due(s);
    if(now) {
      fire_at(s, now);
      continue;
    }
    if(spin(s)) continue;
    if(atomic_load(&s->shutdown)) return;
    park_worker(s, w);
  }
}

static void* worker_main(void* arg) {
  proc_worker* w = (proc_worker*)arg;
#ifdef PROC_TSAN
  w->fiber = __tsan_get_current_fiber();
#endif
  worker_loop(w);
  return NULL;
}

static uint32_t scheduler_count(void) {
  const char* env = getenv("JELLO_SCHEDULERS");
  char* end = NULL;
  long n = env && *env ? strtol(env, &end, 10) : 0;
  if(n < 1 || *end) n = jello_cpu_count();
  return n > (long)PROC_MAX_SCHEDULERS ? PROC_MAX_SCHEDULERS : (uint32_t)n;
}

static void start_workers(proc_sched* s) {
  while(!s->start_failed && s->started < s->nworkers) {
    proc_worker* w = &s->workers[s->started];
    if(pthread_create(&w->thread, NULL, worker_main, w) != 0) {
      s->start_failed = 1;
      return;
    }
    atomic_fetch_add(&s->started, 1);
  }
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

jello_process* proc_self(jello_vm* vm) {
  if(vm->proc) return vm->proc;
  jello_process* m = record_new();
  proc_sched* s = m ? sched_new(vm, m) : NULL;
  if(!s) {
    if(m) proc_release(m);
    return NULL;
  }
  m->vm = vm;
  m->sched = s;
  m->worker = s->workers;
#ifdef PROC_TSAN
  m->fiber = __tsan_get_current_fiber();
#endif
  vm->proc = m;
  return m;
}

static const jello_function* copy_start(jello_vm* pvm, jello_vm* vm, jello_trap_code* trap,
                                        struct proc_socket_list** sockets) {
  const jello_bc_module* m = vm->running_module;
  pvm->running_module = m;
  if(m && m->nglobals) {
    pvm->globals = (uint64_t*)malloc(sizeof(uint64_t) * m->nglobals);
    if(!pvm->globals) return NULL;
    memcpy(pvm->globals, vm->globals, sizeof(uint64_t) * m->nglobals);
    pvm->owns_globals = 1;
  }
  *trap = vm_copy_start(vm, pvm->space, &pvm->gc_transient_root, pvm->globals, sockets);
  return *trap ? NULL : (const jello_function*)pvm->gc_transient_root;
}

jello_process* proc_spawn(jello_vm* vm, const jello_function* body, proc_pool* pool, unsigned flags,
                          jello_trap_code* trap) {
  *trap = JELLO_TRAP_NONE;
  jello_process* self = proc_self(vm);
  if(!self) return NULL;
  proc_sched* s = self->sched;
  if(!pool) pool = &s->pool;
  if(pool->native_bytes < JIT_NATIVE_RESERVE + PROC_JIT_ENTRY_MARGIN) return NULL;
  jello_process* p = record_new();
  uint8_t* slot = NULL;
  uint8_t* mid = NULL;
  if(p) {
    pthread_mutex_lock(&s->pool_lock);
    slot = proc_slot_take(pool);
    pthread_mutex_unlock(&s->pool_lock);
    if(slot) mid = proc_slot_mid(pool, slot);
  }
  jello_gc_push_root(vm, (void*)body);
  jello_vm* pvm =
      slot ? vm_create_attached(vm, mid, pool->frame_bytes, VM_DEFAULT_CALL_FRAMES, 1) : NULL;
  if(pvm) pvm->gc_transient_root = (void*)body;
  struct proc_socket_list* sockets = NULL;
  if(pvm && !(body = copy_start(pvm, vm, trap, &sockets))) {
    vm_destroy_attached(pvm);
    pvm = NULL;
  }
  jello_gc_pop_roots(vm, 1);
  if(!pvm) {
    if(slot) {
      pthread_mutex_lock(&s->pool_lock);
      proc_slot_give(pool, slot);
      pthread_mutex_unlock(&s->pool_lock);
    }
    if(p) proc_release(p);
    return NULL;
  }
  pvm->active_jit = vm->active_jit;
  pvm->max_bytes_len = vm->max_bytes_len;
  vm_inherit_fuel(pvm, vm);
  pvm->native_stack_limit = (uintptr_t)slot + JIT_NATIVE_RESERVE;
  pvm->jit_stack_floor = pvm->native_stack_limit + PROC_JIT_ENTRY_MARGIN;
  vm_share_layouts(pvm, vm);
  pvm->proc = p;
  if(sockets) proc_sockets_take(p, &sockets);
  vm_go_quiescent(pvm);
  atomic_store(&pvm->in_exec, 1);
  p->vm = pvm;
  p->sched = s;
  p->worker = s->workers;
  p->pool = pool;
  p->slot = slot;
  p->body = body;
  p->pinned = (flags & PROC_SPAWN_ON_MAIN) != 0;
  p->sp = proc_stack_prepare(slot, mid, proc_main, p);
#ifdef PROC_TSAN
  p->fiber = __tsan_create_fiber(0);
#endif
  lock(s);
  p->stopping = s->ending ? PROC_STOPPED : 0;
  if(!(flags & PROC_SPAWN_DETACHED)) {
    p->owner = self;
    p->sibling_next = self->children;
    if(self->children) self->children->sibling_prev = p;
    self->children = p;
  }
  p->live_next = s->live;
  if(s->live) s->live->live_prev = p;
  s->live = p;
  unlock(s);
  if(atomic_load_explicit(&s->started, memory_order_relaxed) < s->nworkers) {
    idle_lock(s);
    start_workers(s);
    idle_unlock(s);
  }
  proc_retain(p); /* The caller's, taken before it can run, finish and drop the scheduler's. */
  make_ready_next(s, p);
  return p;
}

static void yield(proc_sched* s, jello_vm* vm) {
  jello_process* p = vm->proc;
  proc_worker* w = p->worker;
  jello_process* next = NULL;
  if(host_due(w)) {
    next = s->main;
    s->main_idle = 1;
  } else if(!(next = take_own(s, w))) {
    return;
  }
  vm_go_quiescent(vm);
  switch_to(s, w, p, next, PROC_LEFT_YIELDED);
  vm_leave_quiescent(vm);
}

static int others_wait(const proc_sched* s, const jello_process* p) {
  proc_worker* w = p->worker;
  if(w == s->workers && atomic_load_explicit(&s->pinned_ready, memory_order_relaxed)) return 1;
  if(w->next) return 1;
  return atomic_load_explicit(&w->len, memory_order_relaxed) &&
         !atomic_load_explicit(&s->idle, memory_order_relaxed);
}

static int window_turn(jello_vm* vm, int may_yield) {
  jello_process* p = vm->proc;
  proc_sched* s = p->sched;
  fire_if_due(s);
  if(may_yield && (host_due(p->worker) || others_wait(s, p))) yield(s, vm);
  return p->stopping;
}

int proc_window_end(jello_vm* vm, int trapped) {
  jello_process* p = vm->proc;
  int stops = p->stopping;
  if(p->cleaning) stops = ++p->cleanup_windows >= PROC_CLEANUP_REDUCTIONS / VM_REDUCTIONS;
  if(window_turn(vm, !trapped && !stops) && !p->cleaning) stops = 1;
  if(!stops) return trapped;
  vm->trap_code = JELLO_TRAP_STOPPED;
  return -1;
}

int proc_run_for(jello_vm* vm, uint32_t ms) {
  proc_sched* s = main_sched(vm);
  if(!s || this_worker != s->workers) return 0;
  if(atomic_load(&vm->in_exec)) return 1;
  atomic_store(&vm->in_exec, 1);
  vm_safepoint_check(vm);
  vm_go_quiescent(vm);
  proc_worker* w = s->workers;
  w->run_until = proc_now() + (uint64_t)ms * 1000000u + 1u;
  main_wait(s);
  w->run_until = 0;
  proc_leave_to_host(vm);
  vm_leave_quiescent(vm);
  atomic_store(&vm->in_exec, 0);
  lock(s);
  int live = s->live != NULL;
  unlock(s);
  return live;
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

static jello_process* running(jello_vm* vm) {
  jello_process* p = proc_self(vm);
  if(!p) abort();
  return p;
}

static void deadline_arm(proc_sched* s, jello_process* p, uint64_t deadline) {
  if(!deadline) return;
  lock(s);
  timer_arm(s, &p->deadline, deadline);
  unlock(s);
}

static void deadline_disarm(proc_sched* s, jello_process* p) {
  if(p->deadline.at != PROC_TIMER_IDLE) timer_disarm(s, &p->deadline);
}

jello_trap_code proc_receive(jello_vm* vm, jello_value* out) {
  jello_process* p = running(vm);
  pthread_mutex_lock(&p->mu);
  while(!p->inbox_len) {
    jello_trap_code trap = park(p->sched, vm, PROC_PARK_RECEIVE, 0);
    if(trap) return trap;
    pthread_mutex_lock(&p->mu);
  }
  vm_parcel msg = *proc_inbox_at(p, 0);
  p->inbox_head = (p->inbox_head + 1u) & (p->inbox_cap - 1u);
  p->inbox_len--;
  pthread_mutex_unlock(&p->mu);
  *out = vm_parcel_take(vm, &msg);
  return JELLO_TRAP_NONE;
}

void proc_on_exit(jello_vm* vm, jello_value fn) {
  jello_process* p = running(vm);
  p->cleanups = (jello_value*)realloc(p->cleanups, sizeof(jello_value) * (p->ncleanups + 1u));
  if(!p->cleanups) abort();
  p->cleanups[p->ncleanups++] = fn;
}

static void native_done(proc_sched* s, jello_process* p) {
  pthread_mutex_lock(&p->mu);
  int queue = resume(p);
  pthread_mutex_unlock(&p->mu);
  if(queue) make_ready(s, p);
}

static void* native_main(void* arg) {
  proc_sched* s = (proc_sched*)arg;
  pthread_mutex_lock(&s->native_lock);
  for(;;) {
    proc_native_call* c = s->native_head;
    if(c) {
      s->native_head = c->next;
      if(!s->native_head) s->native_tail = NULL;
      s->native_queued--;
      pthread_mutex_unlock(&s->native_lock);
      jello_process* p = c->p;
      c->fn(c->ctx);
      native_done(s, p);
      atomic_fetch_sub(&s->outside, 1);
      pthread_mutex_lock(&s->native_lock);
      continue;
    }
    if(s->native_shutdown) break;
    s->native_idle++;
    pthread_cond_wait(&s->native_cv, &s->native_lock);
    s->native_idle--;
  }
  pthread_mutex_unlock(&s->native_lock);
  return NULL;
}

static int native_queue(proc_sched* s, proc_native_call* c) {
  pthread_mutex_lock(&s->native_lock);
  if(s->native_idle <= s->native_queued && s->native_nthreads < PROC_MAX_NATIVE_THREADS) {
    if(pthread_create(&s->native_threads[s->native_nthreads], NULL, native_main, s) == 0)
      s->native_nthreads++;
  }
  if(!s->native_nthreads) {
    pthread_mutex_unlock(&s->native_lock);
    return 0;
  }
  c->next = NULL;
  if(s->native_tail) s->native_tail->next = c;
  else s->native_head = c;
  s->native_tail = c;
  s->native_queued++;
  pthread_cond_signal(&s->native_cv);
  pthread_mutex_unlock(&s->native_lock);
  return 1;
}

static void natives_end(proc_sched* s) {
  pthread_mutex_lock(&s->native_lock);
  s->native_shutdown = 1;
  pthread_cond_broadcast(&s->native_cv);
  pthread_mutex_unlock(&s->native_lock);
  for(uint32_t i = 0; i < s->native_nthreads; i++) pthread_join(s->native_threads[i], NULL);
}

static jello_trap_code native_end(const jello_process* p) {
  return p->stopping && !p->cleaning ? JELLO_TRAP_STOPPED : JELLO_TRAP_NONE;
}

jello_trap_code proc_call_blocking(jello_vm* vm, jdll_export_fn fn, jdlo_ctx* ctx) {
  jello_process* p = vm->proc;
  proc_sched* s = p ? p->sched : NULL;
  int here = !s;
  if(!here && p == s->main) {
    lock(s);
    here = !s->live;
    unlock(s);
  }
  if(here) {
    proc_call_quiescent(vm, fn, ctx);
    return s ? native_end(p) : JELLO_TRAP_NONE;
  }
  proc_native_call c = {fn, ctx, p, NULL};
  atomic_fetch_add(&s->outside, 1);
  pthread_mutex_lock(&p->mu);
  if(!native_queue(s, &c)) {
    pthread_mutex_unlock(&p->mu);
    atomic_fetch_sub(&s->outside, 1);
    proc_call_quiescent(vm, fn, ctx);
    return native_end(p);
  }
  if(park(s, vm, PROC_PARK_NATIVE, 0) == JELLO_TRAP_DEADLOCK) abort(); /* Not while it counts. */
  return native_end(p);
}
