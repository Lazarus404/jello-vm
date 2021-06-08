// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Process slots carved from pooled slabs, and the first frame of a process's native stack. */

#include <jello/internal/platform.h>
#include <jello/internal/proc_internal.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PROC_SLAB_SLOTS 64u

static uint8_t* free_link(const proc_pool* pool, uint8_t* slot) {
  return proc_slot_mid(pool, slot) - sizeof(uint8_t*);
}

static size_t slab_bytes(const proc_pool* pool) {
  return pool->guard_bytes + PROC_SLAB_SLOTS * ((size_t)pool->native_bytes + pool->frame_bytes);
}

proc_pool proc_pool_make(uint32_t native_bytes, uint32_t frame_bytes) {
  uint32_t page = (uint32_t)jello_page_size();
  proc_pool p = {
      native_bytes - page / 2u, frame_bytes + page / 2u, page, NULL, NULL, NULL, NULL, 0};
  return p;
}

void proc_slot_give(proc_pool* pool, uint8_t* slot) {
  memcpy(free_link(pool, slot), &pool->free, sizeof pool->free);
  pool->free = slot;
}

void proc_pool_free(proc_pool* pool) {
  for(uint32_t i = 0; i < pool->nslabs; i++) jello_pages_unmap(pool->slabs[i], slab_bytes(pool));
  free(pool->slabs);
  pool->slabs = NULL;
  pool->nslabs = 0;
  pool->free = pool->fresh = pool->fresh_end = NULL;
}

void* proc_stack_prepare(uint8_t* bottom, uint8_t* top, void (*entry)(void*), void* arg) {
  void (*start)(void) = proc_stack_start;
#if defined(_WIN32) && defined(__aarch64__)
  uint64_t frame[24] = {0};
  memcpy(&frame[0], &entry, sizeof entry);
  memcpy(&frame[1], &arg, sizeof arg);
  memcpy(&frame[11], &start, sizeof start);
#elif defined(_WIN32) && defined(__x86_64__)
  uint64_t frame[32] = {0};
  memcpy(&frame[25], &arg, sizeof arg);
  memcpy(&frame[26], &entry, sizeof entry);
  memcpy(&frame[31], &start, sizeof start);
#elif defined(__aarch64__)
  (void)bottom;
  uint64_t frame[20] = {0};
  memcpy(&frame[0], &entry, sizeof entry);
  memcpy(&frame[1], &arg, sizeof arg);
  memcpy(&frame[11], &start, sizeof start);
#elif defined(__x86_64__)
  (void)bottom;
  uint64_t frame[7] = {0};
  memcpy(&frame[2], &arg, sizeof arg);
  memcpy(&frame[3], &entry, sizeof entry);
  memcpy(&frame[6], &start, sizeof start);
#endif
#ifdef _WIN32
  uint8_t* limit = (uint8_t*)((uintptr_t)(top - 1) & ~(uintptr_t)(jello_page_size() - 1u));
  memcpy(&frame[20], &top, sizeof top);
  memcpy(&frame[21], &limit, sizeof limit);
  memcpy(&frame[22], &bottom, sizeof bottom);
#endif
  uint8_t* sp = top - sizeof frame;
  memcpy(sp, frame, sizeof frame);
  return sp;
}
