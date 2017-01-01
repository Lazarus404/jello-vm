// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_impl.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_i32_type(jello_type_entry* types) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
}

/* -- Test 1: interpreter-only -- */
static void test_interpreter_trap_pc(void) {
  setenv("JELLO_JIT_DISABLE", "1", 1);

  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func;
  set_i32_type(types);

  reg_types[0] = 0; reg_types[1] = 0; reg_types[2] = 0;
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].b = 0; insns[0].c = 0; insns[0].imm = 5;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].b = 0; insns[1].c = 0; insns[1].imm = 0;
  insns[2].op = (uint8_t)JOP_DIV_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1; insns[2].imm = 0;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2; insns[3].b = 0; insns[3].c = 0; insns[3].imm = 0;

  func.name_atom = JELLO_BC_NO_NAME;
  func.nargs = 0;
  func.nregs = 3;
  func.cap_start = 3;
  func.reg_types = reg_types;
  func.ninsns = 4;
  func.insns = insns;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = &func;
  m.entry = 0;

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);

  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_ARITHMETIC);
  assert(jello_vm_last_trap_func_index(vm) == 0);
  assert(jello_vm_last_trap_pc(vm) == 2);
  printf("test_interpreter_trap_pc: PASS (func_index=%u pc=%u)\n",
      jello_vm_last_trap_func_index(vm), jello_vm_last_trap_pc(vm));

  jello_vm_destroy(vm);
  unsetenv("JELLO_JIT_DISABLE");
}

#if defined(__x86_64__) || defined(__aarch64__)

/* -- Test 2: JIT single-function -- */
static void test_jit_single_function_trap_pc(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[3];
  jello_insn insns[4];
  jello_bc_function func;
  set_i32_type(types);

  reg_types[0] = 0; reg_types[1] = 0; reg_types[2] = 0;
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].b = 0; insns[0].c = 0; insns[0].imm = 5;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].b = 0; insns[1].c = 0; insns[1].imm = 0;
  insns[2].op = (uint8_t)JOP_DIV_I32; insns[2].a = 2; insns[2].b = 0; insns[2].c = 1; insns[2].imm = 0;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 2; insns[3].b = 0; insns[3].c = 0; insns[3].imm = 0;

  func.name_atom = JELLO_BC_NO_NAME;
  func.nargs = 0;
  func.nregs = 3;
  func.cap_start = 3;
  func.reg_types = reg_types;
  func.ninsns = 4;
  func.insns = insns;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = &func;
  m.entry = 0;

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);

  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_ARITHMETIC);
  assert(jello_vm_last_trap_func_index(vm) == 0);
  assert(jello_vm_last_trap_pc(vm) == 2);

  printf("test_jit_single_function_trap_pc: PASS (func_index=%u pc=%u)\n",
      jello_vm_last_trap_func_index(vm), jello_vm_last_trap_pc(vm));
  jello_vm_destroy(vm);
}

/* -- Test 3: JIT-to-JIT direct call, callee traps -- */
static void test_jit_to_jit_callee_trap_pc(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[3];
  jello_insn e0[3], e1[3];
  jello_bc_function funcs[2];
  set_i32_type(types);

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 0; r1[2] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].b = 0; e0[0].c = 0; e0[0].imm = 7;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1; /* -> func[1] */
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1; e0[2].b = 0; e0[2].c = 0; e0[2].imm = 0;
  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 1; e1[0].b = 0; e1[0].c = 0; e1[0].imm = 0;
  e1[1].op = (uint8_t)JOP_DIV_I32; e1[1].a = 2; e1[1].b = 0; e1[1].c = 1; e1[1].imm = 0;
  e1[2].op = (uint8_t)JOP_RET; e1[2].a = 2; e1[2].b = 0; e1[2].c = 0; e1[2].imm = 0;

  funcs[0].name_atom = JELLO_BC_NO_NAME;
  funcs[0].nargs = 0;
  funcs[0].nregs = 2;
  funcs[0].cap_start = 2;
  funcs[0].reg_types = r0;
  funcs[0].ninsns = 3;
  funcs[0].insns = e0;

  funcs[1].name_atom = JELLO_BC_NO_NAME;
  funcs[1].nargs = 1;
  funcs[1].nregs = 3;
  funcs[1].cap_start = 3;
  funcs[1].reg_types = r1;
  funcs[1].ninsns = 3;
  funcs[1].insns = e1;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1;
  m.types = types;
  m.nfuncs = 2;
  m.funcs = funcs;
  m.entry = 0;

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);

  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_ARITHMETIC);
  assert(jello_vm_last_trap_func_index(vm) == 1); /* the callee, not the entry (0) */
  assert(jello_vm_last_trap_pc(vm) == 1);

  printf("test_jit_to_jit_callee_trap_pc: PASS (func_index=%u pc=%u)\n",
      jello_vm_last_trap_func_index(vm), jello_vm_last_trap_pc(vm));
  jello_vm_destroy(vm);
}

#else

static void test_jit_single_function_trap_pc(void) {
  printf("test_jit_single_function_trap_pc: SKIPPED (host is not x86_64 or aarch64)\n");
}
static void test_jit_to_jit_callee_trap_pc(void) {
  printf("test_jit_to_jit_callee_trap_pc: SKIPPED (host is not x86_64 or aarch64)\n");
}

#endif

int main(void) {
  test_interpreter_trap_pc();
  test_jit_single_function_trap_pc();
  test_jit_to_jit_callee_trap_pc();
  printf("trap_pc_tests: all tests passed\n");
  return 0;
}
