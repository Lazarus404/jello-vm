// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The built-in `vm` library: processes for Jello code. A process is held as a handle to its
 * record. Unlike a `.jdll` export, these run with their VM running rather than quiescent, since
 * they allocate, park and change scheduler state; parking makes the VM quiescent itself. */

#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>

#include <stdlib.h>
#include <string.h>

static void retain_payload(void* payload) { proc_retain((jello_process*)payload); }

static void release_timer(void* payload) { proc_timer_release((proc_timer*)payload); }

static void retain_timer(void* payload) {
  atomic_fetch_add_explicit(&((proc_timer*)payload)->refs, 1, memory_order_relaxed);
}

static const jello_jdll_handle_class proc_timer_class = {"timer", release_timer, retain_timer};

static uint64_t due_in(int32_t ms) { return proc_deadline(ms < 0 ? 0 : ms); }

typedef struct proc_buffer {
  jdl_refs refs;
  uint32_t length;
  _Alignas(8) uint8_t data[];
} proc_buffer;

static void free_buffer(void* payload) {
  if(jdl_release(payload)) free(payload);
}

static const jello_jdll_handle_class proc_buffer_class = {"buffer", free_buffer, jdl_retain};

static uint64_t load_bits(const void* at, uint32_t width) {
  switch(width) {
    case 1: return __atomic_load_n((const uint8_t*)at, __ATOMIC_RELAXED);
    case 2: return __atomic_load_n((const uint16_t*)at, __ATOMIC_RELAXED);
    case 4: return __atomic_load_n((const uint32_t*)at, __ATOMIC_RELAXED);
    default: return __atomic_load_n((const uint64_t*)at, __ATOMIC_RELAXED);
  }
}

static void store_bits(void* at, uint32_t width, uint64_t v) {
  switch(width) {
    case 1: __atomic_store_n((uint8_t*)at, (uint8_t)v, __ATOMIC_RELAXED); break;
    case 2: __atomic_store_n((uint16_t*)at, (uint16_t)v, __ATOMIC_RELAXED); break;
    case 4: __atomic_store_n((uint32_t*)at, (uint32_t)v, __ATOMIC_RELAXED); break;
    default: __atomic_store_n((uint64_t*)at, v, __ATOMIC_RELAXED); break;
  }
}

#define D (uint8_t)JELLO_T_DYNAMIC
#define I (uint8_t)JELLO_T_I32
#define B (uint8_t)JELLO_T_BOOL
#define L (uint8_t)JELLO_T_I64
#define F (uint8_t)JELLO_T_F32
#define G (uint8_t)JELLO_T_F64
#define Y (uint8_t)JELLO_T_BYTES
