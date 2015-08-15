// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static jello_bc_function make_src(uint32_t nregs, uint32_t ninsns) {
  jello_bc_function f;
  memset(&f, 0, sizeof(f));
  f.nregs = nregs;
  f.ninsns = ninsns;
  f.reg_types = NULL;
  f.insns = NULL;
  f.cap_start = 0;
  return f;
}

static void test_verify_accepts_well_formed_ir(void) {
  jello_bc_function src = make_src(/*nregs=*/2, /*ninsns=*/2);
  jello_jit_ir_insn insns[2] = {
    { (uint16_t)JIR_MOV, 0, 1, 0, 0, 0 },
    { (uint16_t)JIR_RET, 0, 0, 0, 0, 1 },
  };
  jello_jit_ir_function f;
  f.nregs = 2;
  f.ninsns = 2;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NUMERIC_FAST;

  assert(jello_jit_ir_verify(&f, &src) == 1);
  printf("test_verify_accepts_well_formed_ir: PASS\n");
}

static void test_verify_accepts_safepoint_sharing_bc_pc(void) {
  jello_bc_function src = make_src(/*nregs=*/2, /*ninsns=*/1);
  jello_jit_ir_insn insns[2] = {
    { (uint16_t)JIR_SAFEPOINT, 0, 0, 0, 0, 0 },
    { (uint16_t)JIR_CALL_SELF, 0, 0, 0, 0, 0 },
  };
  jello_jit_ir_function f;
  f.nregs = 2;
  f.ninsns = 2;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NONE;

  assert(jello_jit_ir_verify(&f, &src) == 1);
  printf("test_verify_accepts_safepoint_sharing_bc_pc: PASS\n");
}

static void test_verify_rejects_out_of_range_register(void) {
  jello_bc_function src = make_src(/*nregs=*/2, /*ninsns=*/1);
  jello_jit_ir_insn insns[1] = {
    { (uint16_t)JIR_MOV, 5, 0, 0, 0, 0 },
  };
  jello_jit_ir_function f;
  f.nregs = 2;
  f.ninsns = 1;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NONE;

  assert(jello_jit_ir_verify(&f, &src) == 0);
  printf("test_verify_rejects_out_of_range_register: PASS\n");
}

static void test_verify_ignores_non_register_slots_for_bounds_check(void) {
  jello_bc_function src = make_src(/*nregs=*/3, /*ninsns=*/1);
  jello_jit_ir_insn insns[1] = {
    { (uint16_t)JIR_BITS_WRITE, 0, 1, 32, 0, 0 },
  };
  jello_jit_ir_function f;
  f.nregs = 3;
  f.ninsns = 1;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NONE;

  assert(jello_jit_ir_verify(&f, &src) == 1);
  printf("test_verify_ignores_non_register_slots_for_bounds_check: PASS\n");
}

static void test_verify_rejects_fabricated_bc_pc(void) {
  jello_bc_function src = make_src(/*nregs=*/2, /*ninsns=*/1);
  jello_jit_ir_insn insns[1] = {
    { (uint16_t)JIR_RET, 0, 0, 0, 0, 7 },
  };
  jello_jit_ir_function f;
  f.nregs = 2;
  f.ninsns = 1;
  f.insns = insns;
  f.nslow = 0;
  f.flags = JELLO_JIR_FLAG_NONE;

  assert(jello_jit_ir_verify(&f, &src) == 0);
  printf("test_verify_rejects_fabricated_bc_pc: PASS\n");
}

int main(void) {
  test_verify_accepts_well_formed_ir();
  test_verify_accepts_safepoint_sharing_bc_pc();
  test_verify_rejects_out_of_range_register();
  test_verify_ignores_non_register_slots_for_bounds_check();
  test_verify_rejects_fabricated_bc_pc();
  printf("chapter9_jir_tests: all tests passed\n");
  return 0;
}
