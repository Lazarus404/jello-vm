// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(__aarch64__)
enum { k_jit_body_off = 28 };

static int insn_word_present(const uint8_t* ep, size_t ep_len, uint32_t mask, uint32_t value) {
  for(size_t i = 0; i + 4 <= ep_len; i++) {
    uint32_t w = 0;
    memcpy(&w, ep + i, 4);
    if((w & mask) == value) return 1;
  }
  return 0;
}

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

static void test_golden_12b_add_i32(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func;
  for(uint32_t i = 0; i < 3; i++) reg_types[i] = 0;
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 5;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].imm = 3;
  insns[2].op = (uint8_t)JOP_ADD_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2;
  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 3, 4);

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 1);
  assert(code.body_off == (size_t)k_jit_body_off);
  const uint8_t* ep = code.buf.mem + code.body_off;
  size_t ep_len = code.buf.len - code.body_off;
  assert(insn_word_present(ep, ep_len, 0xFF000000u, 0x0B000000u));
  jello_jit_code_free(&code);
  printf("test_golden_13b_add_i32: PASS\n");
}

static void test_golden_12c_back_edge_branch(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[1];
  jello_insn insns[2];
  jello_bc_function func;
  reg_types[0] = 0;
  insns[0].op = (uint8_t)JOP_JMP; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;
  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 1, 2);

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 1);
  const uint8_t* ep = code.buf.mem + code.body_off;
  size_t ep_len = code.buf.len - code.body_off;
  assert(insn_word_present(ep, ep_len, 0xFC000000u, 0x14000000u) ||
         insn_word_present(ep, ep_len, 0xFF000000u, 0x54000000u));
  jello_jit_code_free(&code);
  printf("test_golden_13c_back_edge_branch: PASS\n");
}

#else
static void test_golden_12b_add_i32(void) {
  printf("test_golden_13b_add_i32: SKIPPED (host is not aarch64)\n");
}
static void test_golden_12c_back_edge_branch(void) {
  printf("test_golden_13c_back_edge_branch: SKIPPED (host is not aarch64)\n");
}
#endif

int main(void) {
  test_golden_12b_add_i32();
  test_golden_12c_back_edge_branch();
  printf("chapter13_jit_golden_tests: all tests passed\n");
  return 0;
}