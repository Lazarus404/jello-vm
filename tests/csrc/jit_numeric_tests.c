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

#if defined(__x86_64__) || defined(__aarch64__)
static jello_exec_status run_jit_entry(const jello_bc_module* m,
                                       jello_numeric_result* out) {
  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(m, 0, &code) == 1);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  atomic_store(&vm->in_exec, 1);
  vm_frame_reset(vm);
  vm->running_module = m;
  assert(vm_push_frame(vm, m, 0, 0, 0, 0, 0) == 1);

  jello_jit_entry_fn entry =
      (jello_jit_entry_fn)(void*)(code.buf.mem + code.entry_off);
  jello_exec_status st = entry(vm, jit_frame_entry_base(vm), out);

  atomic_store(&vm->in_exec, 0);
  jello_jit_code_free(&code);
  jello_vm_destroy(vm);
  return st;
}
#endif

static void test_add_i32(void) {
#if defined(__x86_64__) || defined(__aarch64__)
  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func;

  init_i32_reg_types(reg_types, 3);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 5;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].imm = 3;
  insns[2].op = (uint8_t)JOP_ADD_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 3, 4);

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = run_jit_entry(&m, &out);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 8);
  printf("test_add_i32: PASS\n");
#else
  printf("test_add_i32: SKIPPED (host is not x86_64 or aarch64)\n");
#endif
}

static void test_div_i32_by_zero_trap(void) {
#if defined(__x86_64__) || defined(__aarch64__)
  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func;

  init_i32_reg_types(reg_types, 3);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 5;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].imm = 0;
  insns[2].op = (uint8_t)JOP_DIV_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 3, 4);

  jello_numeric_result out;
  jello_exec_status st = run_jit_entry(&m, &out);
  assert(st == JELLO_EXEC_TRAP);
  printf("test_div_i32_by_zero_trap: PASS\n");
#else
  printf("test_div_i32_by_zero_trap: SKIPPED (host is not x86_64 or aarch64)\n");
#endif
}

static void test_div_i32_min_neg1_trap(void) {
#if defined(__x86_64__) || defined(__aarch64__)
  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func;

  init_i32_reg_types(reg_types, 3);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = (uint32_t)INT32_MIN;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].imm = (uint32_t)-1;
  insns[2].op = (uint8_t)JOP_DIV_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 3, 4);

  jello_numeric_result out;
  jello_exec_status st = run_jit_entry(&m, &out);
  assert(st == JELLO_EXEC_TRAP);
  printf("test_div_i32_min_neg1_trap: PASS\n");
#else
  printf("test_div_i32_min_neg1_trap: SKIPPED (host is not x86_64 or aarch64)\n");
#endif
}

static void test_accept_jmp_function(void) {
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
  assert(jello_jit_compile_function(&m, 0, &code) == 1);
  assert(code.buf.mem != NULL);
  jello_jit_code_free(&code);
  printf("test_accept_jmp_function: PASS\n");
}

static void test_reject_call_function(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[2];
  jello_insn insns[2];
  jello_bc_function func;

  init_i32_reg_types(reg_types, 2);
  insns[0].op = (uint8_t)JOP_CALL; insns[0].a = 0; insns[0].b = 0; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 2, 2);

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 0);
  assert(code.buf.mem == NULL);
  printf("test_reject_call_function: PASS\n");
}

static void test_reload_count_const_add_add(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[1];
  jello_insn insns[4];
  jello_bc_function func;

  init_i32_reg_types(reg_types, 1);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 1;
  insns[1].op = (uint8_t)JOP_ADD_I32; insns[1].a = 0; insns[1].b = 0; insns[1].c = 0;
  insns[2].op = (uint8_t)JOP_ADD_I32; insns[2].a = 0; insns[2].b = 0; insns[2].c = 0;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 0;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 1, 4);

  jit_reg_reload_count = 0;
  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 1);
  assert(jit_reg_reload_count == 0);
  jello_jit_code_free(&code);
  printf("test_reload_count_const_add_add: PASS (reload_count=%u)\n", jit_reg_reload_count);
}

int main(void) {
  test_add_i32();
  test_div_i32_by_zero_trap();
  test_div_i32_min_neg1_trap();
  test_accept_jmp_function();
  test_reject_call_function();
  test_reload_count_const_add_add();
  printf("jit_numeric_tests: all tests passed\n");
  return 0;
}
