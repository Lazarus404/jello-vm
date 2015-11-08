// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_impl.h>

#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static jello_bc_module make_i32_module(jello_type_entry* types,
                                       jello_type_id* reg_types,
                                       jello_insn* insns,
                                       jello_bc_function* func,
                                       uint32_t nregs,
                                       uint32_t ninsns) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  func->nregs = nregs;
  func->cap_start = 0;
  func->reg_types = reg_types;
  func->ninsns = ninsns;
  func->insns = insns;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = func;
  m.entry = 0;
  return m;
}

static void init_i32_reg_types(jello_type_id* reg_types, uint32_t nregs) {
  for(uint32_t i = 0; i < nregs; i++) reg_types[i] = 0;
}

static void test_reject_jmp_function(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[1];
  jello_insn insns[2];
  jello_bc_function func;

  init_i32_reg_types(reg_types, 1);
  insns[0].op = (uint8_t)JOP_JMP; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 1, 2);

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 0);
  assert(code.buf.mem == NULL);
  printf("test_reject_jmp_function: PASS\n");
}

int main(void) {
  test_reject_jmp_function();
  printf("jit_numeric_tests: all tests passed\n");
  return 0;
}