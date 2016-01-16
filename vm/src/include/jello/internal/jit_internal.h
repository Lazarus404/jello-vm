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

typedef struct jello_jit_func_code {
  size_t blob_off, entry_off, body_off, size;
  uint32_t* loop_anchor_pcs;
  size_t* loop_anchor_offs;
  uint32_t nloop_anchors;
} jello_jit_func_code;

typedef struct jello_jit_call_patch {
  uint32_t caller_fi;
  uint32_t callee_fi;
  size_t site_off;
} jello_jit_call_patch;

typedef struct jello_jit_module {
  jit_exec_buf blob;
  jello_jit_func_code* funcs;
  uint32_t nfuncs;
  int prepared;
} jello_jit_module;

typedef jello_exec_status (*jello_jit_entry_fn)(jello_vm* vm, uint8_t* frame_base, jello_numeric_result* out);

int jello_jit_compile_function(const jello_bc_module* m, uint32_t fi, jello_jit_code* out_code);
void jello_jit_code_free(jello_jit_code* code);

extern uint32_t jello_jit_module_prepare_count;

int jello_jit_module_prepare(const jello_bc_module* m, jello_jit_module* out);
void jello_jit_module_free(jello_jit_module* jm);
int jello_jit_try_enter(jello_vm* vm, jello_jit_module* jm, jello_numeric_result* out, jello_exec_status* out_st);

extern uint32_t jit_try_enter_count;

int jello_jit_emit_function_into(const jello_bc_module* m, uint32_t fi, jit_exec_buf* buf,
    jello_jit_func_code* out_fc, jello_jit_call_patch** patches, uint32_t* npatch, uint32_t* patch_cap,
    int allow_calls);

void jello_jit_patch_call_rel32(jit_exec_buf* buf, size_t at, int32_t rel);

#endif /* JELLO_JIT_INTERNAL_H */
