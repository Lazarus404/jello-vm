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

static void queue_append(proc_worker* w, jello_process* first, jello_process* last, uint32_t n) {
  last->queued = NULL;
  if(w->tail) w->tail->queued = first;
  else w->head = first;
  w->tail = last;
  if(n) atomic_fetch_add(&w->len, n);
}

static void timer_place(proc_sched* s, proc_timer* t, uint32_t i) {
  s->timers[i] = t;
  t->at = i;
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

void proc_timer_release(proc_timer* t) {
  if(atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1) free(t);
}

static uint64_t timers_due(const proc_sched* s) {
  uint64_t due = atomic_load_explicit(&s->next_due, memory_order_relaxed);
  if(!due) return 0;
  uint64_t now = proc_now();
  return due <= now ? now : 0;
}

static void sibling_unlink(jello_process* p) {
  if(p->sibling_prev) p->sibling_prev->sibling_next = p->sibling_next;
  else p->owner->children = p->sibling_next;
  if(p->sibling_next) p->sibling_next->sibling_prev = p->sibling_prev;
  p->owner = NULL;
}

static uint32_t scheduler_count(void) {
  const char* env = getenv("JELLO_SCHEDULERS");
  char* end = NULL;
  long n = env && *env ? strtol(env, &end, 10) : 0;
  if(n < 1 || *end) n = jello_cpu_count();
  return n > (long)PROC_MAX_SCHEDULERS ? PROC_MAX_SCHEDULERS : (uint32_t)n;
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
