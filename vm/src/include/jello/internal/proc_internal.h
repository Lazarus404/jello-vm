// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Processes: switching native stacks, the pooled slots that hold a process's native stack and
 * frame stack, and the schedulers, one OS thread each, that run processes in turn. */

#ifndef JELLO_PROC_INTERNAL_H
#define JELLO_PROC_INTERNAL_H

#include <jello.h>
#include <jello/internal/tls.h>
#include <jello/internal/vm_internal.h>
#include <jello/jdll.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PROC_TSAN 1
#endif
#endif

#ifdef PROC_TSAN
#endif

#define PROC_NATIVE_STACK_BYTES (256u * 1024u)
#define PROC_FRAME_STACK_BYTES (256u * 1024u)
#define PROC_MAX_NATIVE_THREADS 256u

typedef struct proc_pool {
  uint32_t native_bytes; /**< The native stack's size; a multiple of 16. */
  uint32_t frame_bytes;  /**< The frame stack's size; a multiple of 16. */
  uint32_t guard_bytes;  /**< The inaccessible page below each slab. */
  uint8_t* free;         /**< Given-back slots, linked through a word at their native top. */
  uint8_t* fresh;        /**< The current slab's first slot never handed out. */
  uint8_t* fresh_end;    /**< The current slab's end. */
  uint8_t** slabs;       /**< Every slab it has mapped, for proc_pool_free. */
  uint32_t nslabs;
} proc_pool;

proc_pool proc_pool_make(uint32_t native_bytes, uint32_t frame_bytes);
void proc_slot_give(proc_pool* pool, uint8_t* slot);
void proc_pool_free(proc_pool* pool);

static inline uint8_t* proc_slot_mid(const proc_pool* pool, uint8_t* slot) {
  return slot + pool->native_bytes;
}

void proc_stack_switch(void** save, void* load);

typedef struct proc_sched proc_sched;

typedef enum proc_exit {
  PROC_STOPPED = 1,       /**< It was stopped. */
} proc_exit;

enum {
  PROC_PARK_NONE,
  PROC_PARK_RECEIVE,
  PROC_PARK_ALL,
  PROC_PARK_NATIVE,
};

#define PROC_TIMER_IDLE UINT32_MAX

typedef struct proc_timer {
  uint64_t due;                 /**< When it fires, in proc_now's nanoseconds. */
  uint32_t at;                  /**< Its index in the heap, or PROC_TIMER_IDLE. */
  _Atomic uint32_t refs;        /**< A sending timer's: the heap's while armed, and its handles'. */
  struct jello_process* target; /**< Who it wakes, or whom it sends to. */
  vm_parcel msg;                /**< What it sends; empty for a parked process's deadline. */
  struct proc_timer* prev;      /**< In its target's list of sends, while armed. */
  struct proc_timer* next;
} proc_timer;

typedef struct proc_native_call {
  jdll_export_fn fn;
  jdlo_ctx* ctx;
  struct jello_process* p;
  struct proc_native_call* next;
} proc_native_call;

typedef struct proc_poller {
#if defined(_WIN32)
#else
  int fd;
#if defined(__linux__)
#endif
#endif
} proc_poller;

#if defined(_WIN32)
#else
#endif
void proc_poller_wake(proc_poller* pl);
void proc_poller_close(proc_poller* pl);

#define PROC_POLL_CHUNK 1024u
#define PROC_POLL_CHUNKS 1024u
#define PROC_POLL_FDS (PROC_POLL_CHUNK * PROC_POLL_CHUNKS)

static inline int32_t proc_poll_index(int fd) {
#if defined(_WIN32)
  uint32_t at = (uint32_t)fd >> 2;
#else
  uint32_t at = (uint32_t)fd;
#endif
  return fd < 0 || at >= PROC_POLL_FDS ? -1 : (int32_t)at;
}

typedef struct proc_poll_desc {
  pthread_mutex_t mu; /**< Guards the rest; taken before any process's `mu`. */
  uint8_t open;       /**< Set from proc_poll_open until proc_poll_close. */
  uint8_t ready;      /**< What it became ready for, with no one waiting, since a wait last found
                       * it so. */
  int fd;             /**< The descriptor it is the record of, while `open`. */
} proc_poll_desc;

proc_poll_desc* proc_polls_find(proc_sched* s, int fd);

#define PROC_MAX_SCHEDULERS 256u
#define PROC_IDLE_WORDS (PROC_MAX_SCHEDULERS / 64u)
#define PROC_CACHE_LINE 128

typedef struct proc_worker {
  _Alignas(PROC_CACHE_LINE) proc_sched* sched;
  _Atomic uint32_t mu;        /**< A spin lock guarding its run queue: a mutex would sleep in the
                               * kernel under the contention of schedulers taking from it. */
  struct jello_process* head; /**< Its run queue, first in first out, through `queued`; only
                               * scheduler 0's holds the pinned processes, which only it runs. */
  struct jello_process* tail;
  _Atomic uint32_t len; /**< How many are in its run queue, pinned ones aside. */
  _Alignas(PROC_CACHE_LINE) struct jello_process* next;
  uint32_t streak;    /**< How many it has run in a row from `next` while its run queue waited. */
  uint64_t run_until; /**< Only scheduler 0's: when the proc_run_for driving it is to return, or 0 while none is. */
  void* idle_sp;      /**< Its thread's own stack while it runs a process; unused by scheduler 0. */
  struct jello_process* prev;
  uint8_t left;               /**< A PROC_LEFT_* value, for `prev`. */
  uint64_t switches;          /**< Its switches from one stack to another, for tests. */
  pthread_mutex_t park_mu;    /**< Guards `woken`, for waiting on `park_cv`. */
  pthread_cond_t park_cv;     /**< Where it waits for work, unless it is scheduler 0. */
  uint8_t woken;              /**< Set by whatever took its bit in `idle_mask`, to end its wait. */
  pthread_t thread;
#ifdef PROC_TSAN
  void* fiber; /**< ThreadSanitizer's name for its thread's own stack. */
#endif
} proc_worker;

typedef struct jello_process {
  pthread_mutex_t mu;  /**< Guards its inbox, `parked`, `off`, `done` and writes to `stopping`. */
  struct jello_vm* vm; /**< NULL once its VM is freed; nothing runs before it finishes. */
  proc_sched* sched;   /**< Read only while it has yet to finish, as it may be freed after. */
  proc_worker* worker; /**< The scheduler running it, or the last to, or scheduler 0. */
  proc_pool* pool;
  uint8_t* slot; /**< NULL for the main process. */
  void* sp;      /**< Its native stack pointer while it is not running. */
#ifdef PROC_TSAN
  void* fiber; /**< ThreadSanitizer's name for its stack. */
#endif
  const struct jello_function* body; /**< What it runs, a root of its VM until it starts. */
  struct jello_process* queued;      /**< The next in its run queue. */
  struct jello_process* live_next;
  struct jello_process* owner;    /**< The process whose finishing stops it, unless detached. */
  vm_parcel* inbox; /**< Messages not yet received, a ring of inbox_cap, a power of two. */
  uint32_t inbox_head;
  uint32_t inbox_len;
  uint32_t inbox_cap;
  _Atomic uint32_t refs;
  uint8_t parked; /**< A PROC_PARK_* value; still set when woken because nothing could run. */
  uint8_t off;
  _Atomic uint8_t stopping; /**< Set under `mu` by proc_stop, to the first reason it was given. */
  _Atomic uint8_t done;     /**< Set, once it has finished, after how it did is recorded. */
  uint8_t cleaning; /**< Its own: set while its cleanups run, when a stop waits, a park traps. */
  uint8_t pinned;   /**< Run only by scheduler 0: the main process, and those spawned on main. */
  struct proc_socket* sockets; /**< The sockets it owns, closed as it finishes; see net.c. */
  proc_timer deadline;         /**< The deadline of its park, while it has one. */
  proc_timer* sends;           /**< Its armed sending timers. */
  jello_value* cleanups; /**< Its own: what onExit registered, run last first as it finishes. */
  uint32_t ncleanups;
  uint32_t cleanup_windows;    /**< Reduction windows the cleanup running has used. */
  jello_trap_code trap;        /**< Why it trapped, once it has finished. */
  jello_value exit_value;
  vm_parcel exit;
} jello_process;

struct proc_sched {
  pthread_mutex_t* lock;     /**< Guards the process tree, joins, replies, timers and `ending`. */
  pthread_mutex_t idle_lock;
  pthread_cond_t main_cv;
  _Atomic uint32_t pinned_ready;
  uint8_t main_idle;
  _Atomic uint8_t main_waiting; /**< Whether scheduler 0 waits for work, the main process parked. */
  _Atomic uint8_t shutdown;     /**< Set once every process has finished, to end the threads. */
  _Atomic uint32_t idle;        /**< Schedulers other than 0 waiting for work. */
  _Atomic uint64_t idle_mask[PROC_IDLE_WORDS];
  _Atomic uint32_t spinning;
  _Atomic uint32_t started;  /**< Schedulers whose threads have started, 0 included. */
  uint32_t nworkers;
  proc_worker* workers; /**< Scheduler 0, then the others. */
  jello_process* main;  /**< The main process; never finishes while the schedulers run. */
  uint8_t ending;       /**< In proc_end_all, which stops processes spawned meanwhile too. */
  jello_process* live;  /**< The unfinished processes, the main process aside. */
  proc_timer** timers;  /**< A binary min-heap by due time. */
  uint32_t ntimers;
  _Atomic uint64_t next_due; /**< The first timer's due time, or 0 when there is none. */
  uint64_t epoch;            /**< When it started, by proc_now. */

  _Alignas(64) _Atomic uint32_t outside;
  _Alignas(64) pthread_mutex_t native_lock;

  pthread_cond_t native_cv;      /**< Where its idle threads wait for a call. */
  proc_native_call* native_head; /**< Calls queued for it, first in first out. */
  proc_native_call* native_tail;
  uint32_t native_queued; /**< How many calls are queued. */
  uint32_t native_idle;   /**< Its threads waiting for a call. */
  uint32_t native_nthreads;
  pthread_t native_threads[PROC_MAX_NATIVE_THREADS];
  uint8_t native_shutdown; /**< Set once every process has finished, to end its threads. */

  pthread_mutex_t poll_lock; /**< Guards `polling` and adding chunks to the poller's table. */
  proc_poller poller;        /**< Opened, and its thread started, by the first proc_poll_open. */
  pthread_t poll_thread;
  uint8_t polling; /**< Set once the poller is open and its thread started. */
  proc_poll_desc* _Atomic poll_chunks[PROC_POLL_CHUNKS];

  proc_pool pool;            /**< Slots of the default sizes. */
  pthread_mutex_t pool_lock; /**< Guards every pool its processes' slots come from. */
};
void proc_retain(jello_process* p);
uint64_t proc_now(void);
void proc_timer_release(proc_timer* t);

static inline void proc_call_quiescent(struct jello_vm* vm, jdll_export_fn fn, jdlo_ctx* ctx) {
  vm_go_quiescent(vm);
  fn(ctx);
  vm_leave_quiescent(vm);
}

typedef struct proc_reply {
  jdl_refs refs;   /**< The handles holding it, in any process. */
  vm_parcel value; /**< What was sent, once `sent`, for each holder to read a copy of. */
} proc_reply;

#endif /* JELLO_PROC_INTERNAL_H */
