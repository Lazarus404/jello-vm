// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_JIT_INTERNAL_H
#define JELLO_JIT_INTERNAL_H

/* Interpreter and profiler integration hooks. */

#include <jello/internal/jit_impl.h>

/* -- compiling one function to x64 -- */
typedef struct jello_jit_code {
  jit_exec_buf buf;
  size_t entry_off;
  size_t body_off;
  uint32_t* loop_anchor_pcs;
  size_t* loop_anchor_offs;
  uint32_t nloop_anchors;
} jello_jit_code;

typedef jello_exec_status (*jello_jit_entry_fn)(jello_vm* vm, uint8_t* frame_base, jello_numeric_result* out);

int jello_jit_compile_function(const jello_bc_module* m, uint32_t fi, jello_jit_code* out_code);
void jello_jit_code_free(jello_jit_code* code);

#endif /* JELLO_JIT_INTERNAL_H */
