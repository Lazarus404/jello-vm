// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Processes: switching native stacks, the pooled slots that hold a process's native stack and
 * frame stack, and the schedulers, one OS thread each, that run processes in turn. */

#ifndef JELLO_PROC_INTERNAL_H
#define JELLO_PROC_INTERNAL_H

#include <jello.h>
#include <jello/internal/vm_internal.h>
#include <jello/jdll.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#endif
#endif

#ifdef PROC_TSAN
#endif

typedef struct proc_pool {
  uint32_t native_bytes; /**< The native stack's size; a multiple of 16. */
} proc_pool;

typedef struct proc_timer {
  uint64_t due;                 /**< When it fires, in proc_now's nanoseconds. */
  _Atomic uint32_t refs;        /**< A sending timer's: the heap's while armed, and its handles'. */
} proc_timer;

#if defined(_WIN32)
#else
#endif

typedef struct jello_process {
  pthread_mutex_t mu;  /**< Guards its inbox, `parked`, `off`, `done` and writes to `stopping`. */
  _Atomic uint32_t refs;
} jello_process;

typedef struct proc_reply {
  jdl_refs refs;   /**< The handles holding it, in any process. */
} proc_reply;

#endif /* JELLO_PROC_INTERNAL_H */
