// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_impl.h>

#undef NDEBUG
#include <assert.h>
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

static void test_prologue_unchanged(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[1];
  jello_insn insns[1];
  jello_bc_function func;

  types[0].kind = JELLO_T_VOID;
  memset(&types[0].as, 0, sizeof(types[0].as));
  reg_types[0] = 0;
  insns[0].op = (uint8_t)JOP_RET;
  insns[0].a = 0;
  func.nregs = 1;
  func.cap_start = 0;
  func.reg_types = reg_types;
  func.ninsns = 1;
  func.insns = insns;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = &func;
  m.entry = 0;

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 1);
  assert(code.body_off == 14);
  jello_jit_code_free(&code);
  printf("test_prologue_unchanged: PASS\n");
}

static void test_jmp_forward_patches(void) {
  jello_type_entry types[1];
  jello_type_id reg_types[2];
  jello_insn insns[5];
  jello_bc_function func;

  init_i32_reg_types(reg_types, 2);
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 1;
  insns[1].op = (uint8_t)JOP_JMP; insns[1].imm = 1;
  insns[2].op = (uint8_t)JOP_CONST_I32; insns[2].a = 0; insns[2].imm = 99;
  insns[3].op = (uint8_t)JOP_CONST_I32; insns[3].a = 1; insns[3].imm = 7;
  insns[4].op = (uint8_t)JOP_RET; insns[4].a = 1;

  jello_bc_module m = make_i32_module(types, reg_types, insns, &func, 2, 5);

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 1);
  assert(code.body_off == 14);

  int found_e9 = 0;
  for(size_t i = code.body_off; i + 5 <= code.buf.len; i++) {
    if(code.buf.mem[i] != 0xE9) continue;
    int32_t rel = 0;
    memcpy(&rel, code.buf.mem + i + 1, 4);
    size_t end = i + 5;
    size_t tgt = (size_t)((int64_t)end + (int64_t)rel);
    if(tgt > i && tgt < code.buf.len) {
      found_e9 = 1;
      break;
    }
  }
  assert(found_e9 == 1);
  jello_jit_code_free(&code);
  printf("test_jmp_forward_patches: PASS\n");
}

#if defined(__x86_64__)
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

static void test_hot_loop_execute(void) {
  jello_type_entry types[2];
  jello_type_id reg_types[3];
  jello_insn insns[7];
  jello_bc_function func;

  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  types[1].kind = JELLO_T_BOOL;
  memset(&types[1].as, 0, sizeof(types[1].as));
  reg_types[0] = 0;
  reg_types[1] = 0;
  reg_types[2] = 1;

  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_CONST_I32; insns[1].a = 1; insns[1].imm = 1;
  insns[2].op = (uint8_t)JOP_ADD_I32; insns[2].a = 0; insns[2].b = 0; insns[2].c = 1;
  insns[3].op = (uint8_t)JOP_MOV; insns[3].a = 1; insns[3].b = 0;
  insns[4].op = (uint8_t)JOP_LT_I32_IMM; insns[4].a = 2; insns[4].b = 0; insns[4].imm = 10;
  insns[5].op = (uint8_t)JOP_JMP_IF; insns[5].a = 2; insns[5].imm = (uint32_t)-5;
  insns[6].op = (uint8_t)JOP_RET; insns[6].a = 0;

  func.nregs = 3;
  func.cap_start = 0;
  func.reg_types = reg_types;
  func.ninsns = 7;
  func.insns = insns;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 2;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = &func;
  m.entry = 0;

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = run_jit_entry(&m, &out);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 10);
  printf("test_hot_loop_execute: PASS\n");
}

static void test_jmp_if_taken_execute(void) {
  jello_type_entry types[2];
  jello_type_id reg_types[3];
  jello_insn insns[6];
  jello_bc_function func;

  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  types[1].kind = JELLO_T_BOOL;
  memset(&types[1].as, 0, sizeof(types[1].as));
  reg_types[0] = 1;
  reg_types[1] = 0;
  reg_types[2] = 0;

  insns[0].op = (uint8_t)JOP_CONST_BOOL; insns[0].a = 0; insns[0].imm = 1;
  insns[1].op = (uint8_t)JOP_JMP_IF; insns[1].a = 0; insns[1].imm = 2;
  insns[2].op = (uint8_t)JOP_CONST_I32; insns[2].a = 1; insns[2].imm = 1;
  insns[3].op = (uint8_t)JOP_RET; insns[3].a = 1;
  insns[4].op = (uint8_t)JOP_CONST_I32; insns[4].a = 2; insns[4].imm = 2;
  insns[5].op = (uint8_t)JOP_RET; insns[5].a = 2;

  func.nregs = 3;
  func.cap_start = 0;
  func.reg_types = reg_types;
  func.ninsns = 6;
  func.insns = insns;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 2;
  m.types = types;
  m.nfuncs = 1;
  m.funcs = &func;
  m.entry = 0;

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = run_jit_entry(&m, &out);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 2);
  printf("test_jmp_if_taken_execute: PASS\n");
}
#endif

int main(void) {
  test_prologue_unchanged();
  test_jmp_forward_patches();
#if defined(__x86_64__)
  test_hot_loop_execute();
  test_jmp_if_taken_execute();
#else
  printf("test_hot_loop_execute: SKIPPED (host is not x86_64)\n");
  printf("test_jmp_if_taken_execute: SKIPPED (host is not x86_64)\n");
#endif
  printf("jit_cfg_tests: all tests passed\n");
  return 0;
}