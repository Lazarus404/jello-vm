// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_I32 = 0, T_LIST_I32 = 1, T_BOOL = 2, T_DYN = 3, T_LIST_BOOL = 4, NTYPES = 5 };

static jello_type_entry g_types[NTYPES];

static void init_types(void) {
  memset(g_types, 0, sizeof(g_types));
  g_types[T_I32].kind = JELLO_T_I32;
  g_types[T_LIST_I32].kind = JELLO_T_LIST;
  g_types[T_LIST_I32].as.unary.elem = T_I32;
  g_types[T_BOOL].kind = JELLO_T_BOOL;
  g_types[T_DYN].kind = JELLO_T_DYNAMIC;
  g_types[T_LIST_BOOL].kind = JELLO_T_LIST;
  g_types[T_LIST_BOOL].as.unary.elem = T_BOOL;
}

static jello_insn mk(jello_op op, uint8_t a, uint8_t b, uint8_t c, uint32_t imm) {
  jello_insn i;
  memset(&i, 0, sizeof(i));
  i.op = (uint8_t)op;
  i.a = a;
  i.b = b;
  i.c = c;
  i.imm = imm;
  return i;
}

static void init_module(jello_bc_module* m, jello_bc_function* f, const jello_type_id* reg_types, uint32_t nregs,
                        const jello_insn* insns, uint32_t ninsns) {
  memset(f, 0, sizeof(*f));
  f->name_atom = JELLO_BC_NO_NAME;
  f->nregs = nregs;
  f->cap_start = nregs;
  f->reg_types = reg_types;
  f->ninsns = ninsns;
  f->insns = insns;
  memset(m, 0, sizeof(*m));
  m->ntypes = NTYPES;
  m->types = g_types;
  m->nfuncs = 1;
  m->funcs = f;
  m->entry = 0;
}

static int validate_all(const jello_bc_module* m, const char** msg) {
  const jello_bc_function* f = &m->funcs[0];
  for(uint32_t pc = 0; pc < f->ninsns; pc++) {
    jello_bc_result r = jello_bc_validate_insn(m, f->reg_types, &f->insns[pc], f->nregs, pc, f->ninsns, m->nfuncs);
    if(r.err != JELLO_BC_OK) {
      *msg = r.msg;
      return 0;
    }
  }
  return 1;
}

#define BUILD_N 300000

static void run_build_and_sum(int jit) {
  if(jit) unsetenv("JELLO_JIT_DISABLE");
  else setenv("JELLO_JIT_DISABLE", "1", 1);

  static const jello_type_id regs[7] = {T_LIST_I32, T_I32, T_I32, T_BOOL, T_LIST_I32, T_I32, T_I32};
  jello_insn code[18];
  code[0] = mk(JOP_LIST_NIL, 0, 0, 0, 0);
  code[1] = mk(JOP_CONST_I32, 1, 0, 0, 0);
  code[2] = mk(JOP_CONST_I32, 2, 0, 0, BUILD_N);
  code[3] = mk(JOP_CONST_I32, 5, 0, 0, 0);
  code[4] = mk(JOP_LT_I32, 3, 1, 2, 0);
  code[5] = mk(JOP_NOT_BOOL, 3, 3, 0, 0);
  code[6] = mk(JOP_JMP_IF, 3, 0, 0, 3u);
  code[7] = mk(JOP_ADD_I32_IMM, 1, 1, 0, 1u);
  code[8] = mk(JOP_LIST_CONS, 0, 1, 0, 0);
  code[9] = mk(JOP_JMP, 0, 0, 0, (uint32_t)-6);
  code[10] = mk(JOP_MOV, 4, 0, 0, 0);
  code[11] = mk(JOP_LIST_IS_NIL, 3, 4, 0, 0);
  code[12] = mk(JOP_JMP_IF, 3, 0, 0, 4u);
  code[13] = mk(JOP_LIST_HEAD, 6, 4, 0, 0);
  code[14] = mk(JOP_ADD_I32, 5, 5, 6, 0);
  code[15] = mk(JOP_LIST_TAIL, 4, 4, 0, 0);
  code[16] = mk(JOP_JMP, 0, 0, 0, (uint32_t)-6);
  code[17] = mk(JOP_RET, 5, 0, 0, 0);

  jello_bc_module m;
  jello_bc_function f;
  init_module(&m, &f, regs, 7, code, 18);
  const char* msg = NULL;
  if(!validate_all(&m, &msg)) {
    fprintf(stderr, "build_and_sum: unexpected validation failure: %s\n", msg);
    abort();
  }

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);
  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  int32_t want = (int32_t)(uint32_t)(((uint64_t)BUILD_N * (BUILD_N + 1u)) / 2u);
  assert(out.as.i32 == want);

  uint64_t live = 0;
  uint32_t collections = 0;
  double pause_ms = 0.0;
  jello_vm_gc_stats(vm, &live, &collections, &pause_ms);
  assert(collections > 0);

  printf("build_and_sum(%s): PASS (sum=%d collections=%u)\n", jit ? "jit" : "interp", out.as.i32, collections);
  jello_vm_destroy(vm);
  unsetenv("JELLO_JIT_DISABLE");
}

static void test_head_of_nil_traps(void) {
  setenv("JELLO_JIT_DISABLE", "1", 1);
  static const jello_type_id regs[2] = {T_LIST_I32, T_I32};
  jello_insn code[3];
  code[0] = mk(JOP_LIST_NIL, 0, 0, 0, 0);
  code[1] = mk(JOP_LIST_HEAD, 1, 0, 0, 0);
  code[2] = mk(JOP_RET, 1, 0, 0, 0);

  jello_bc_module m;
  jello_bc_function f;
  init_module(&m, &f, regs, 2, code, 3);
  const char* msg = NULL;
  assert(validate_all(&m, &msg));

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric(vm, &m, &out);
  assert(st == JELLO_EXEC_TRAP);
  assert(jello_vm_last_trap_code(vm) == JELLO_TRAP_NULL_DEREF);
  assert(jello_vm_last_trap_pc(vm) == 1);
  printf("head_of_nil_traps: PASS\n");
  jello_vm_destroy(vm);
  unsetenv("JELLO_JIT_DISABLE");
}

static void expect_reject(const char* name, const jello_type_id* regs, uint32_t nregs, jello_insn ins,
                          const char* want) {
  jello_insn code[2];
  code[0] = ins;
  code[1] = mk(JOP_RET, 0, 0, 0, 0);
  jello_bc_module m;
  jello_bc_function f;
  init_module(&m, &f, regs, nregs, code, 2);
  const char* msg = NULL;
  if(validate_all(&m, &msg)) {
    fprintf(stderr, "%s: expected rejection [%s], module validated\n", name, want);
    abort();
  }
  if(strcmp(msg, want) != 0) {
    fprintf(stderr, "%s: expected [%s] got [%s]\n", name, want, msg);
    abort();
  }
  printf("%s: PASS (%s)\n", name, msg);
}

static void test_validation_rejects(void) {
  static const jello_type_id head_bool[2] = {T_LIST_I32, T_BOOL};
  expect_reject("head_into_wrong_type", head_bool, 2, mk(JOP_LIST_HEAD, 1, 0, 0, 0), "list_head dst type mismatch");

  static const jello_type_id cons_bool[3] = {T_LIST_I32, T_BOOL, T_LIST_I32};
  expect_reject("cons_wrong_head", cons_bool, 3, mk(JOP_LIST_CONS, 0, 1, 2, 0), "list_cons head type mismatch");

  static const jello_type_id cons_mixed[3] = {T_LIST_I32, T_I32, T_LIST_BOOL};
  expect_reject("cons_mixed_lists", cons_mixed, 3, mk(JOP_LIST_CONS, 0, 1, 2, 0), "list_cons list type mismatch");

  static const jello_type_id tail_dyn[2] = {T_LIST_I32, T_DYN};
  expect_reject("tail_from_dynamic", tail_dyn, 2, mk(JOP_LIST_TAIL, 0, 1, 0, 0), "list_tail src must be List");

  static const jello_type_id head_dyn[2] = {T_I32, T_DYN};
  expect_reject("head_from_dynamic", head_dyn, 2, mk(JOP_LIST_HEAD, 0, 1, 0, 0), "list_head src must be List");

  static const jello_type_id nil_i32[1] = {T_I32};
  expect_reject("nil_into_i32", nil_i32, 1, mk(JOP_LIST_NIL, 0, 0, 0, 0), "list_nil dst must be List");
}

int main(void) {
  init_types();
  run_build_and_sum(0);
#if defined(__x86_64__) || defined(__aarch64__)
  run_build_and_sum(1);
#endif
  test_head_of_nil_traps();
  test_validation_rejects();
  printf("list_tests: all passed\n");
  return 0;
}
