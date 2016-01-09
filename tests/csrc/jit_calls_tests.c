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
static void build_fib_survive_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
                                     jello_insn* e0, jello_insn* e1, jello_bc_function* funcs,
                                     jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 1; r1[2] = 0; r1[3] = 0; r1[4] = 0; r1[5] = 0; r1[6] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = n;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_LT_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 2;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 6; /* -> pc 8 (base case) */
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1; /* r3 = fib(n-1) -- survive candidate */
  e1[4].op = (uint8_t)JOP_SUB_I32_IMM; e1[4].a = 4; e1[4].b = 0; e1[4].imm = 2;
  e1[5].op = (uint8_t)JOP_CALL; e1[5].a = 5; e1[5].b = 4; e1[5].c = 1; e1[5].imm = 1; /* r5 = fib(n-2) */
  e1[6].op = (uint8_t)JOP_ADD_I32; e1[6].a = 6; e1[6].b = 3; e1[6].c = 5; /* consumer: r3 + r5 */
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 6;
  e1[8].op = (uint8_t)JOP_RET; e1[8].a = 0; /* base case: n < 2 */

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 7; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 9; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}

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


static int32_t fib_ref(int32_t n) {
  return (n < 2) ? n : fib_ref(n - 1) + fib_ref(n - 2);
}

static void build_call_direct_survive_module(jello_type_entry* types,
    jello_type_id* r0, jello_type_id* r1, jello_type_id* r2,
    jello_insn* e0, jello_insn* e1, jello_insn* e2,
    jello_bc_function* funcs, jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 0;
  r2[0] = 0; r2[1] = 0; r2[2] = 0; r2[3] = 0; r2[4] = 0;

  e0[0].op = (uint8_t)JOP_MUL_I32_IMM; e0[0].a = 1; e0[0].b = 0; e0[0].imm = 2; /* leaf(x) = 2x */
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 1;

  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 0; e1[0].imm = n;
  e1[1].op = (uint8_t)JOP_CALL; e1[1].a = 1; e1[1].b = 0; e1[1].c = 1; e1[1].imm = 2; /* -> func[2] */
  e1[2].op = (uint8_t)JOP_RET; e1[2].a = 1;

  e2[0].op = (uint8_t)JOP_CALL; e2[0].a = 1; e2[0].b = 0; e2[0].c = 1; e2[0].imm = 0; /* r1 = leaf(n) -- candidate */
  e2[1].op = (uint8_t)JOP_SUB_I32_IMM; e2[1].a = 2; e2[1].b = 0; e2[1].imm = 1; /* r2 = n-1 */
  e2[2].op = (uint8_t)JOP_CALL; e2[2].a = 3; e2[2].b = 2; e2[2].c = 1; e2[2].imm = 0; /* r3 = leaf(n-1) */
  e2[3].op = (uint8_t)JOP_ADD_I32; e2[3].a = 4; e2[3].b = 1; e2[3].c = 3; /* consumer: r1 + r3 */
  e2[4].op = (uint8_t)JOP_RET; e2[4].a = 4;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].nregs = 2; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 3; funcs[1].insns = e1;
  funcs[2].nregs = 5; funcs[2].cap_start = 0; funcs[2].reg_types = r2; funcs[2].ninsns = 5; funcs[2].insns = e2;

  memset(m, 0, sizeof(*m));
  m->ntypes = 1;
  m->types = types;
  m->nfuncs = 3;
  m->funcs = funcs;
  m->entry = 1;
}

static void test_call_direct_survive_correct(void) {
  jello_type_entry types[1];
  jello_type_id r0[2], r1[2], r2[5];
  jello_insn e0[2], e1[3], e2[5];
  jello_bc_function funcs[3];
  jello_bc_module m;
  build_call_direct_survive_module(types, r0, r1, r2, e0, e1, e2, funcs, &m, 5);

  jit_call_survive_armed_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == 2 * 5 + 2 * 4); /* 18 */
  assert(jit_call_survive_armed_count > 0);
  printf("test_call_direct_survive_correct: PASS (result=%d, armed=%u)\n",
      out.as.i32, jit_call_survive_armed_count);
}

static void build_call_direct_branch_decline_module(jello_type_entry* types,
    jello_type_id* r0, jello_type_id* r1, jello_type_id* r2,
    jello_insn* e0, jello_insn* e1, jello_insn* e2,
    jello_bc_function* funcs, jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 0;
  r2[0] = 0; r2[1] = 0; r2[2] = 0; r2[3] = 0; r2[4] = 1; r2[5] = 0;

  e0[0].op = (uint8_t)JOP_ADD_I32_IMM; e0[0].a = 1; e0[0].b = 0; e0[0].imm = 100; /* leaf(x) = x+100 */
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 1;

  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 0; e1[0].imm = n;
  e1[1].op = (uint8_t)JOP_CALL; e1[1].a = 1; e1[1].b = 0; e1[1].c = 1; e1[1].imm = 2; /* -> func[2] */
  e1[2].op = (uint8_t)JOP_RET; e1[2].a = 1;

  e2[0].op = (uint8_t)JOP_CALL; e2[0].a = 1; e2[0].b = 0; e2[0].c = 1; e2[0].imm = 0; /* r1 = leaf(n) -- candidate */
  e2[1].op = (uint8_t)JOP_SUB_I32_IMM; e2[1].a = 2; e2[1].b = 0; e2[1].imm = 1; /* r2 = n-1 */
  e2[2].op = (uint8_t)JOP_CALL; e2[2].a = 3; e2[2].b = 2; e2[2].c = 1; e2[2].imm = 0; /* r3 = leaf(n-1) */
  e2[3].op = (uint8_t)JOP_EQ_I32_IMM; e2[3].a = 4; e2[3].b = 0; e2[3].imm = 0; /* r4 = (n == 0), unrelated to r1 */
  e2[4].op = (uint8_t)JOP_JMP_IF; e2[4].a = 4; e2[4].imm = 1; /* -> pc 6 -- a branch sits between call2 and the would-be consumer */
  e2[5].op = (uint8_t)JOP_ADD_I32; e2[5].a = 5; e2[5].b = 1; e2[5].c = 3; /* would-be consumer -- must not be reached across the branch */
  e2[6].op = (uint8_t)JOP_RET; e2[6].a = 5;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].nregs = 2; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 3; funcs[1].insns = e1;
  funcs[2].nregs = 6; funcs[2].cap_start = 0; funcs[2].reg_types = r2; funcs[2].ninsns = 7; funcs[2].insns = e2;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 3;
  m->funcs = funcs;
  m->entry = 1;
}

static void test_call_survive_declines_across_branch(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[2], r2[6];
  jello_insn e0[2], e1[3], e2[7];
  jello_bc_function funcs[3];
  jello_bc_module m;
  build_call_direct_branch_decline_module(types, r0, r1, r2, e0, e1, e2, funcs, &m, 5);

  jit_call_survive_armed_count = 0;
  jit_call_survive_decline_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == (5 + 100) + (4 + 100)); /* 209 -- correct regardless of survive */
  assert(jit_call_survive_decline_count > 0);
  printf("test_call_survive_declines_across_branch: PASS (result=%d, armed=%u, declined=%u)\n",
      out.as.i32, jit_call_survive_armed_count, jit_call_survive_decline_count);
}

static void test_call_self_survive_fib_correct(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_fib_survive_module(types, r0, r1, e0, e1, funcs, &m, 15);

  jit_call_survive_armed_count = 0;
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  assert(run_module_exec(&m, &out) == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == fib_ref(15));
  assert(jit_call_survive_armed_count > 0);
  printf("test_call_self_survive_fib_correct: PASS (fib(15)=%d, armed=%u)\n",
      out.as.i32, jit_call_survive_armed_count);
}

static void build_fib_survive_nested_trap_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
    jello_insn* e0, jello_insn* e1, jello_bc_function* funcs, jello_bc_module* m, uint32_t n) {
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  memset(&types[0].as, 0, sizeof(types[0].as));
  memset(&types[1].as, 0, sizeof(types[1].as));

  r0[0] = 0; r0[1] = 0;
  r1[0] = 0; r1[1] = 1; r1[2] = 0; r1[3] = 0; r1[4] = 0; r1[5] = 0; r1[6] = 0;

  e0[0].op = (uint8_t)JOP_CONST_I32; e0[0].a = 0; e0[0].imm = n;
  e0[1].op = (uint8_t)JOP_CALL; e0[1].a = 1; e0[1].b = 0; e0[1].c = 1; e0[1].imm = 1;
  e0[2].op = (uint8_t)JOP_RET; e0[2].a = 1;

  e1[0].op = (uint8_t)JOP_LT_I32_IMM; e1[0].a = 1; e1[0].b = 0; e1[0].imm = 2;
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 6; /* -> base at pc 8 */
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1;
  e1[4].op = (uint8_t)JOP_SUB_I32_IMM; e1[4].a = 4; e1[4].b = 0; e1[4].imm = 2;
  e1[5].op = (uint8_t)JOP_CALL; e1[5].a = 5; e1[5].b = 4; e1[5].c = 1; e1[5].imm = 1;
  e1[6].op = (uint8_t)JOP_ADD_I32; e1[6].a = 6; e1[6].b = 3; e1[6].c = 5;
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 6;
  e1[8].op = (uint8_t)JOP_EQ_I32_IMM; e1[8].a = 1; e1[8].b = 0; e1[8].imm = 0;
  e1[9].op = (uint8_t)JOP_JMP_IF; e1[9].a = 1; e1[9].imm = 2; /* n==0 -> trap at pc 12 */
  e1[10].op = (uint8_t)JOP_RET; e1[10].a = 0; /* n==1 base */
  e1[11].op = (uint8_t)JOP_CONST_I32; e1[11].a = 2; e1[11].imm = 1;
  e1[12].op = (uint8_t)JOP_CONST_I32; e1[12].a = 3; e1[12].imm = 0;
  e1[13].op = (uint8_t)JOP_DIV_I32; e1[13].a = 4; e1[13].b = 2; e1[13].c = 3;
  e1[14].op = (uint8_t)JOP_RET; e1[14].a = 4;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 7; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 15; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2;
  m->types = types;
  m->nfuncs = 2;
  m->funcs = funcs;
  m->entry = 0;
}

static void test_nested_survive_trap_propagates(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[15];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_fib_survive_nested_trap_module(types, r0, r1, e0, e1, funcs, &m, 2);

  jit_call_survive_armed_count = 0;
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);
  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_ARITHMETIC);
  assert(jit_call_survive_armed_count > 0);
  jello_vm_destroy(vm);
  printf("test_nested_survive_trap_propagates: PASS (armed=%u)\n", jit_call_survive_armed_count);
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
  test_call_self_survive_fib_correct();
  test_call_direct_survive_correct();
  test_call_survive_declines_across_branch();
  test_nested_survive_trap_propagates();
  test_nested_ret_does_not_hit_boundary_epilogue();
#else
  printf("test_call_self_recursive_sum: SKIPPED (host is not x86_64)\n");
  printf("test_call_direct_executes: SKIPPED (host is not x86_64)\n");
  printf("test_nested_jit_trap_propagates: SKIPPED (host is not x86_64)\n");
  printf("test_wx_icache_prepare_exec_repeated: SKIPPED (host is not x86_64)\n");
  printf("test_call_self_survive_fib_correct: SKIPPED (host is not x86_64)\n");
  printf("test_call_direct_survive_correct: SKIPPED (host is not x86_64)\n");
  printf("test_call_survive_declines_across_branch: SKIPPED (host is not x86_64)\n");
  printf("test_nested_survive_trap_propagates: SKIPPED (host is not x86_64)\n");
  printf("test_nested_ret_does_not_hit_boundary_epilogue: SKIPPED (host is not x86_64)\n");
#endif
  printf("jit_calls_tests: all tests passed\n");
  return 0;
}
