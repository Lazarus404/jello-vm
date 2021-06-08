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

typedef struct proc_buffer {
  jdl_refs refs;
  uint32_t length;
  _Alignas(8) uint8_t data[];
} proc_buffer;

#define D (uint8_t)JELLO_T_DYNAMIC
#define I (uint8_t)JELLO_T_I32
#define B (uint8_t)JELLO_T_BOOL
#define L (uint8_t)JELLO_T_I64
#define F (uint8_t)JELLO_T_F32
#define G (uint8_t)JELLO_T_F64
#define Y (uint8_t)JELLO_T_BYTES
