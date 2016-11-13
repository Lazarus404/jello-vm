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

static int jit_call_site_targets(const uint8_t* mem, size_t start, size_t end, size_t target) {
  for(size_t i = start; i + 4 <= end; i++) {
    uint32_t w = 0;
    memcpy(&w, mem + i, 4);
    if((w & 0xFC000000u) != 0x94000000u) continue;
    int32_t off = (int32_t)(w & 0x03FFFFFFu);
    if(off & 0x02000000) off |= (int32_t)~0x03FFFFFFu;
    if(i + (size_t)off * 4u == target) return 1;
  }
  return 0;
}

static void test_golden_12d_bl_call_patch(void) {
  jello_type_entry types[1];
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2];
  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 0;
  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = 4;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;
  e1[0].op = (uint8_t)JOP_ADD_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 1;
  e1[1].op = (uint8_t)JOP_RET; e1[1].a = 1;
  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 2; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 2; funcs[1].insns = e1;
  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1; m.types = types; m.nfuncs = 2; m.funcs = funcs; m.entry = 0;

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);
  size_t f0_start = jm.funcs[0].blob_off;
  size_t f0_end = f0_start + jm.funcs[0].size;
  size_t callee_target = jm.funcs[1].blob_off + jm.funcs[1].body_off;
  assert(jit_call_site_targets(jm.blob.mem, f0_start, f0_end, callee_target) == 1);
  jello_jit_module_free(&jm);
  printf("test_golden_13d_bl_call_patch: PASS\n");
}

#else
static void test_golden_12b_add_i32(void) {
  printf("test_golden_13b_add_i32: SKIPPED (host is not aarch64)\n");
}
static void test_golden_12c_back_edge_branch(void) {
  printf("test_golden_13c_back_edge_branch: SKIPPED (host is not aarch64)\n");
}
static void test_golden_12d_bl_call_patch(void) {
  printf("test_golden_13d_bl_call_patch: SKIPPED (host is not aarch64)\n");
}
#endif

int main(void) {
  test_golden_12b_add_i32();
  test_golden_12c_back_edge_branch();
  test_golden_12d_bl_call_patch();
  printf("chapter13_jit_golden_tests: all tests passed\n");
  return 0;
}