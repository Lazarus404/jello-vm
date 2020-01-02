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

void proc_slot_give(proc_pool* pool, uint8_t* slot) {
  memcpy(free_link(pool, slot), &pool->free, sizeof pool->free);
  pool->free = slot;
}
