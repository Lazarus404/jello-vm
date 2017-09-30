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
#define PROC_NEXT_STREAK 16u

uint64_t proc_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void proc_retain(jello_process* p) { atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed); }

void proc_timer_release(proc_timer* t) {
  if(atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1) free(t);
}
