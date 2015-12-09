// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void build_call_direct_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
                                     jello_insn* e0, jello_insn* e1, jello_bc_function* funcs,
                                     jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = 4;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_ADD_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 1;
  e1[1].op = (uint8_t)JOP_RET; e1[1].a = 1;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 2; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 2; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 1;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}

#if defined(__x86_64__)
static void build_call_self_sum_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
                                       jello_insn* e0, jello_insn* e1, jello_bc_function* funcs,
                                       jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  for(uint32_t i = 0; i < 5; i++) r1[i] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = 10;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_EQ_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 0;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 4;
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1;
  e1[4].op = (uint8_t)JOP_ADD_I32; e1[4].a = 4; e1[4].b = 0; e1[4].c = 3;
  e1[5].op = (uint8_t)JOP_RET; e1[5].a = 4;
  e1[6].op = (uint8_t)JOP_CONST_I32; e1[6].a = 4; e1[6].imm = 0;
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 4;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 5; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 8; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}
#endif

static void test_module_prepare_produces_one_blob(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);
  assert(jm.prepared == 1);
  assert(jm.blob.mem != NULL);
  assert(jm.nfuncs == 2);
  assert(jm.funcs[0].size > 0);
  assert(jm.funcs[1].size > 0);
  assert(jm.funcs[0].body_off == 14);
  assert(jm.funcs[1].body_off == 14);
  assert(jm.funcs[1].blob_off > jm.funcs[0].blob_off);
  assert(jm.funcs[1].blob_off < jm.blob.cap);
  jello_jit_module_free(&jm);
  printf("test_module_prepare_produces_one_blob: PASS\n");
}

static void test_call_rel32_patch_golden(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);

  size_t f0_start = jm.funcs[0].blob_off;
  size_t f0_end = f0_start + jm.funcs[0].size;
  size_t callee_target = jm.funcs[1].blob_off + jm.funcs[1].body_off;

  int found = 0;
  for(size_t i = f0_start; i + 5 <= f0_end; i++) {
    if(jm.blob.mem[i] != 0xE8) continue;
    int32_t rel;
    memcpy(&rel, jm.blob.mem + i + 1, 4);
    size_t end = i + 5;
    size_t target = (size_t)((int64_t)end + (int64_t)rel);
    if(target == callee_target) {
      found = 1;
      break;
    }
  }
  assert(found == 1);
  jello_jit_module_free(&jm);
  printf("test_call_rel32_patch_golden: PASS\n");
}

static void test_module_prepare_rejects_call_in_single_compile(void) {
  jello_type_entry types[1];
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  jello_type_id r[2];
  r[0] = 0; r[1] = 0;
  jello_insn ins[2];
  ins[0].op = (uint8_t)JOP_CALL; ins[0].a = 0; ins[0].b = 0; ins[0].imm = 0;
  ins[1].op = (uint8_t)JOP_RET; ins[1].a = 0;
  jello_bc_function func;
  func.nregs = 2; func.cap_start = 0; func.reg_types = r; func.ninsns = 2; func.insns = ins;
  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1; m.types = types; m.nfuncs = 1; m.funcs = &func; m.entry = 0;

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  assert(jello_jit_compile_function(&m, 0, &code) == 0);
  jello_jit_code_free(&code);
  printf("test_module_prepare_rejects_call_in_single_compile: PASS\n");
}

static void test_module_prepare_rejects_call_native(void) {
  jello_type_entry types[1];
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  jello_type_id r[2];
  r[0] = 0; r[1] = 0;

  jello_insn ins[3];
  ins[0].op = (uint8_t)JOP_CONST_I32; ins[0].a = 0; ins[0].imm = 0;
  ins[1].op = (uint8_t)JOP_CALL_NATIVE; ins[1].a = 1; ins[1].b = 0; ins[1].c = 0; ins[1].imm = 0;
  ins[2].op = (uint8_t)JOP_RET; ins[2].a = 1;

  jello_bc_function func;
  func.nregs = 2; func.cap_start = 0; func.reg_types = r; func.ninsns = 3; func.insns = ins;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1; m.types = types; m.nfuncs = 1; m.funcs = &func; m.entry = 0;

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 0);
  assert(jm.prepared == 0);
  jello_jit_module_free(&jm);
  printf("test_module_prepare_rejects_call_native: PASS\n");
}

static void test_module_prepare_accepts_pointer_callee(void) {
  jello_type_entry types[2];
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BYTES;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  jello_type_id r0[2];
  r0[0] = 1; r0[1] = 1;
  jello_type_id r1[1];
  r1[0] = 1;

  jello_insn e0[2];
  e0[0].op = (uint8_t)JOP_CALL; e0[0].a = 1; e0[0].b = 0; e0[0].c = 1; e0[0].imm = 1;
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 1;

  jello_insn e1[1];
  e1[0].op = (uint8_t)JOP_RET; e1[0].a = 0;

  jello_bc_function funcs[2];
  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].nregs = 1; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 1; funcs[1].insns = e1;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 2; m.types = types; m.nfuncs = 2; m.funcs = funcs; m.entry = 0;

  jello_jit_module jm;
  memset(&jm, 0, sizeof(jm));
  assert(jello_jit_module_prepare(&m, &jm) == 1);
  assert(jm.prepared == 1);
  assert(jm.funcs[0].size > 0);
  assert(jm.funcs[1].size > 0);
  jello_jit_module_free(&jm);
  printf("test_module_prepare_accepts_pointer_callee: PASS\n");
}

#if defined(__x86_64__)

static jello_exec_status run_module_exec(const jello_bc_module* m, jello_numeric_result* out) {
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_exec_status st = jello_vm_exec_numeric(vm, m, out);
  if(st != JELLO_EXEC_OK)
    fprintf(stderr, "run_module_exec: st=%d trap=%d\n", (int)st, (int)jello_vm_last_trap_code(vm));
  jello_vm_destroy(vm);
  return st;
}

static void test_call_self_recursive_sum(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[5];
  jello_insn e0[3], e1[8];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_call_self_sum_module(types, r0, r1, e0, e1, funcs, &m);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  vm_warm_jit_layouts(vm, &m);
  assert(vm->jit_layout_by_func_len == 2);
  assert(vm->jit_layout_by_func[0] != NULL);
  assert(vm->jit_layout_by_func[1] != NULL);
  jello_vm_destroy(vm);

  jit_try_enter_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = run_module_exec(&m, &out);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 55);
  assert(jit_try_enter_count == 1);
  printf("test_call_self_recursive_sum: PASS\n");
}

static void test_call_direct_executes(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 5);
  printf("test_call_direct_executes: PASS\n");
}

static void build_call_self_trap_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
    jello_insn* e0, jello_insn* e1, jello_bc_function* funcs, jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  for(uint32_t i = 0; i < 5; i++) r1[i] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = 2;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_EQ_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 0;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 5;
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1;
  e1[4].op = (uint8_t)JOP_CONST_I32; e1[4].a = 4; e1[4].imm = 0;
  e1[5].op = (uint8_t)JOP_RET; e1[5].a = 4;
  e1[6].op = (uint8_t)JOP_CONST_I32; e1[6].a = 2; e1[6].imm = 1;
  e1[7].op = (uint8_t)JOP_CONST_I32; e1[7].a = 3; e1[7].imm = 0;
  e1[8].op = (uint8_t)JOP_DIV_I32; e1[8].a = 4; e1[8].b = 2; e1[8].c = 3;
  e1[9].op = (uint8_t)JOP_RET; e1[9].a = 4;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 5; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 10; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}

static void test_nested_jit_trap_propagates(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[5];
  jello_insn e0[3], e1[10];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_call_self_trap_module(types, r0, r1, e0, e1, funcs, &m);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);
  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_ARITHMETIC);
  jello_vm_destroy(vm);
  printf("test_nested_jit_trap_propagates: PASS\n");
}

static void test_wx_icache_prepare_exec_repeated(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2];
  jello_insn e0[3], e1[2];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_call_direct_module(types, r0, r1, e0, e1, funcs, &m);

  for(int i = 0; i < 50; i++) {
    jello_jit_module jm;
    memset(&jm, 0, sizeof(jm));
    assert(jello_jit_module_prepare(&m, &jm) == 1);
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
    assert(out.as.i32 == 5);
    jello_jit_module_free(&jm);
  }
  printf("test_wx_icache_prepare_exec_repeated: PASS\n");
}

static void test_nested_ret_does_not_hit_boundary_epilogue(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[5];
  jello_insn e0[3], e1[8];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_call_self_sum_module(types, r0, r1, e0, e1, funcs, &m);
  e0[0].imm = 1;

  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 1);
  printf("test_nested_ret_does_not_hit_boundary_epilogue: PASS\n");
}

#endif

int main(void) {
  test_module_prepare_produces_one_blob();
  test_call_rel32_patch_golden();
  test_module_prepare_rejects_call_in_single_compile();
  test_module_prepare_rejects_call_native();
  test_module_prepare_accepts_pointer_callee();
#if defined(__x86_64__)
  test_call_self_recursive_sum();
  test_call_direct_executes();
  test_nested_jit_trap_propagates();
  test_wx_icache_prepare_exec_repeated();
  test_nested_ret_does_not_hit_boundary_epilogue();
#else
  printf("test_call_self_recursive_sum: SKIPPED (host is not x86_64)\n");
  printf("test_call_direct_executes: SKIPPED (host is not x86_64)\n");
  printf("test_nested_jit_trap_propagates: SKIPPED (host is not x86_64)\n");
  printf("test_wx_icache_prepare_exec_repeated: SKIPPED (host is not x86_64)\n");
  printf("test_nested_ret_does_not_hit_boundary_epilogue: SKIPPED (host is not x86_64)\n");
#endif
  printf("jit_calls_tests: all tests passed\n");
  return 0;
}
