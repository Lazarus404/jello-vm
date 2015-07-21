// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

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
  const char* fail_msg;
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
};

#endif /* JELLO_JDLL_INTERNAL_H */
