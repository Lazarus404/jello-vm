// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The layout of jdlo_ctx, shared by the native-call instruction and the jdl_* accessors. */

#ifndef JELLO_JDLL_INTERNAL_H
#define JELLO_JDLL_INTERNAL_H

#include <jello/internal/vm_internal.h>
#include <jello/jdll.h>

struct jdlo_ctx {
  jello_vm* vm;
  const jello_bc_module* m;
  reg_frame* caller_rf;
  const jello_insn* ins; /* imports[ins->imm]: arity, arg_kinds; b = first arg register */
  const jello_import_sig* sig;

  int returned;
  int failed;
  jello_trap_code trap; /**< The trap a failed built-in export asks for; TYPE_MISMATCH when 0. */
  int fail_said;        /**< Whether jdl_fail gave a message, which is then the VM's trap detail. */
  jello_type_kind ret_kind_given; /* which jdl_return_* was actually called */
  union {
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    jello_value dyn;
    struct {
      const uint8_t* data;
      uint32_t len;
    } bytes;
  } ret;
  
  uint8_t* ret_bytes;
  uint32_t ret_bytes_len;
  const jello_jdll_handle_class* handle_class;
  void* handle_payload;
  uint32_t handle_bytes;
};

extern const jello_jdll_api jello_jdll_host_api;
extern const jello_jdll_manifest vm_builtin_manifest;

int vm_builtin_export(const jello_jdll_export* e);
void jdl_release_ret(jdlo_ctx* c);

#endif /* JELLO_JDLL_INTERNAL_H */
