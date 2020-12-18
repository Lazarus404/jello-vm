// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * JIT module state; the per-module code blob, per-function compile records, lazy compilation
 * in mixed mode, the crossings between interpreter and JIT code, and hot reload. */

#ifndef JELLO_JIT_INTERNAL_H
#define JELLO_JIT_INTERNAL_H

#include <jello/internal/jit_impl.h>

typedef struct jello_jit_code {
  jit_exec_buf buf;
  size_t entry_off;
  size_t body_off;
  uint32_t* loop_anchor_pcs;
  size_t* loop_anchor_offs;
  uint32_t nloop_anchors;
} jello_jit_code;

typedef enum jello_jit_decline {
  JIT_DECLINE_NONE = 0,
  JIT_DECLINE_IR_BUILD, /**< The bytecode has no JIR form. */
  JIT_DECLINE_VERIFY,   /**< The JIR failed jello_jit_ir_verify. */
  JIT_DECLINE_SHAPE,    /**< jit_ir_shape_ok refused it. */
  JIT_DECLINE_SURVIVE,  /**< Call-survive planning failed. */
  JIT_DECLINE_EMIT,     /**< Emission failed; an op the backend cannot emit, a full buffer, or
                         * (x64) a call-patch list that could not grow. */
  JIT_DECLINE_OOM,
} jello_jit_decline;

static inline const char* jello_jit_decline_name(jello_jit_decline d) {
  switch(d) {
    case JIT_DECLINE_NONE: return "none";
    case JIT_DECLINE_IR_BUILD: return "ir-build";
    case JIT_DECLINE_VERIFY: return "verify";
    case JIT_DECLINE_SHAPE: return "shape";
    case JIT_DECLINE_SURVIVE: return "survive";
    case JIT_DECLINE_EMIT: return "emit";
    case JIT_DECLINE_OOM: return "oom";
  }
  return "unknown";
}

typedef struct jello_jit_func_code {
  size_t blob_off, entry_off, body_off, size;
  uint32_t* loop_anchor_pcs;
  size_t* loop_anchor_offs;
  uint32_t nloop_anchors;
  uint8_t compiled;
  uint8_t tried;          /**< Compiled or declined; set once. */
  uint8_t decline_reason; /**< A jello_jit_decline; NONE when compiled. */
  uint16_t decline_op;    /**< Bytecode opcode at decline_pc. */
  uint32_t decline_pc;    /**< UINT32_MAX when no single instruction is to blame. */
  uint32_t calls;         /**< Calls counted towards JIT_COMPILE_AFTER_CALLS while not tried. */
} jello_jit_func_code;

typedef struct jello_jit_call_patch {
  uint32_t caller_fi;
  uint32_t callee_fi;
  size_t site_off;
} jello_jit_call_patch;

#define JIT_COMPILE_AFTER_CALLS 1u

typedef struct jello_jit_module {
  jit_exec_buf blob;
  jello_jit_func_code* funcs;
  uint32_t nfuncs;
  int prepared;
  jello_jit_call_patch* patches;
  uint32_t npatch;
  uint32_t ncompiled;
  size_t bridge_off, enter_off, lazy_off;
  const void** entries;
  const jello_bc_module* m;
} jello_jit_module;

typedef void(JELLO_JIT_ABI* jello_jit_enter_fn)(jello_vm* vm, uint8_t* frame_base,
                                                jello_numeric_result* out, const void* body);

extern size_t jit_append_window_bytes;

typedef jello_exec_status(JELLO_JIT_ABI* jello_jit_entry_fn)(jello_vm* vm, uint8_t* frame_base,
                                                             jello_numeric_result* out);

int jello_jit_compile_function(const jello_bc_module* m, uint32_t fi, jello_jit_code* out_code);
void jello_jit_code_free(jello_jit_code* code);

extern _Atomic uint32_t jello_jit_module_prepare_count;

int jello_jit_module_prepare(const jello_bc_module* m, jello_jit_module* out);
void jello_jit_module_free(jello_jit_module* jm);
int jello_jit_try_enter(jello_vm* vm, jello_jit_module* jm, jello_numeric_result* out, jello_exec_status* out_st);

extern uint32_t jit_try_enter_count;

int jello_jit_emit_function_into(const jello_bc_module* m, uint32_t fi, jit_exec_buf* buf,
    jello_jit_func_code* out_fc, jello_jit_call_patch** patches, uint32_t* npatch, uint32_t* patch_cap,
    int allow_calls);

void jello_jit_patch_call_rel32(jit_exec_buf* buf, size_t at, int32_t rel);
void jello_jit_patch_call(jit_exec_buf* buf, size_t site_off, size_t callee_off);

typedef enum jello_reload_fn_status {
  JELLO_RELOAD_FN_UNCHANGED,          /* same signature, same body hash -- no-op */
  JELLO_RELOAD_FN_CHANGED,            /* same signature, different body hash -- recompile + patch */
  JELLO_RELOAD_FN_SIGNATURE_CONFLICT, /* return kind, arg count, or an arg kind differs -- refused */
  JELLO_RELOAD_FN_NEW,                /* Sec 8's own concern: name has no match in the old module */
  JELLO_RELOAD_FN_REMOVED,            /* Sec 8's own concern: old name has no match in the new module */
} jello_reload_fn_status;

jello_reload_fn_status jello_reload_analyze_function(const jello_bc_module* old_m, const jello_bc_function* old_f,
    const jello_bc_module* new_m, const jello_bc_function* new_f);

int jello_reload_struct_sig_changed(const jello_bc_module* old_m, const jello_bc_module* new_m, uint32_t struct_sig_id);
int jello_reload_function_uses_struct_sig(const jello_bc_module* m, const jello_bc_function* f, uint32_t struct_sig_id);

int jello_jit_reload_pause_all(jello_vm* vm);
void jello_jit_reload_resume_all(jello_vm* vm);

typedef struct jello_jit_reload_report {
  uint32_t n_unchanged, n_changed, n_new, n_removed, n_signature_conflicts, n_type_conflicts;
  jello_reload_fn_status* fn_status; /* [new_m->nfuncs], owned by this report */
} jello_jit_reload_report;

int jello_jit_module_reload(jello_vm* vm, jello_jit_module* jm, const jello_bc_module* old_m,
    const jello_bc_module* new_m, int allow_conflicts, jello_jit_reload_report* out_report);
void jello_jit_reload_report_free(jello_jit_reload_report* report);

#endif /* JELLO_JIT_INTERNAL_H */
