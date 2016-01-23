// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void build_fib_module(jello_type_entry* types, jello_type_id* r0, jello_type_id* r1,
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
  e1[1].op = (uint8_t)JOP_JMP_IF; e1[1].a = 1; e1[1].imm = 6;
  e1[2].op = (uint8_t)JOP_SUB_I32_IMM; e1[2].a = 2; e1[2].b = 0; e1[2].imm = 1;
  e1[3].op = (uint8_t)JOP_CALL; e1[3].a = 3; e1[3].b = 2; e1[3].c = 1; e1[3].imm = 1;
  e1[4].op = (uint8_t)JOP_SUB_I32_IMM; e1[4].a = 4; e1[4].b = 0; e1[4].imm = 2;
  e1[5].op = (uint8_t)JOP_CALL; e1[5].a = 5; e1[5].b = 4; e1[5].c = 1; e1[5].imm = 1;
  e1[6].op = (uint8_t)JOP_ADD_I32; e1[6].a = 6; e1[6].b = 3; e1[6].c = 5;
  e1[7].op = (uint8_t)JOP_RET; e1[7].a = 6;
  e1[8].op = (uint8_t)JOP_RET; e1[8].a = 0;

  funcs[0].nregs = 2; funcs[0].cap_start = 0; funcs[0].reg_types = r0; funcs[0].ninsns = 3; funcs[0].insns = e0;
  funcs[1].nregs = 7; funcs[1].cap_start = 0; funcs[1].reg_types = r1; funcs[1].ninsns = 9; funcs[1].insns = e1;

  memset(m, 0, sizeof(*m));
  m->ntypes = 2; m->types = types; m->nfuncs = 2; m->funcs = funcs; m->entry = 0;
}

static void test_module_create_destroy_roundtrip(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_fib_module(types, r0, r1, e0, e1, funcs, &m, 10);

  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm != NULL);
  jello_jit_module_destroy(jm);
  jello_jit_module_destroy(NULL); /* must be a safe no-op */
  printf("test_module_create_destroy_roundtrip: PASS\n");
}

static void build_one_func_module(jello_type_entry* types, jello_type_id* r,
    jello_insn* insns, uint32_t ninsns, uint32_t nregs, uint32_t nargs,
    jello_bc_function* f, jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  f->name_atom = JELLO_BC_NO_NAME;
  f->nargs = nargs;
  f->nregs = nregs;
  f->cap_start = nregs;
  f->reg_types = r;
  f->ninsns = ninsns;
  f->insns = insns;
  memset(m, 0, sizeof(*m));
  m->ntypes = 1; m->types = types; m->nfuncs = 1; m->funcs = f; m->entry = 0;
}

static void test_reload_analyze_unchanged(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r_a[2] = {0, 0}, r_b[2] = {0, 0};
  jello_insn insns_a[2], insns_b[2];
  memset(insns_a, 0, sizeof(insns_a));
  memset(insns_b, 0, sizeof(insns_b));
  insns_a[0].op = insns_b[0].op = (uint8_t)JOP_ADD_I32_IMM;
  insns_a[0].a = insns_b[0].a = 1; insns_a[0].b = insns_b[0].b = 0;
  insns_a[0].imm = insns_b[0].imm = 5;
  insns_a[1].op = insns_b[1].op = (uint8_t)JOP_RET;
  insns_a[1].a = insns_b[1].a = 1;

  jello_bc_function fa, fb;
  jello_bc_module ma, mb;
  build_one_func_module(types_a, r_a, insns_a, 2, 2, 1, &fa, &ma);
  build_one_func_module(types_b, r_b, insns_b, 2, 2, 1, &fb, &mb);

  assert(jello_reload_analyze_function(&ma, &fa, &mb, &fb) == JELLO_RELOAD_FN_UNCHANGED);
  printf("test_reload_analyze_unchanged: PASS\n");
}

static void test_reload_analyze_changed(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r_a[2] = {0, 0}, r_b[2] = {0, 0};
  jello_insn insns_a[2], insns_b[2];
  memset(insns_a, 0, sizeof(insns_a));
  memset(insns_b, 0, sizeof(insns_b));
  insns_a[0].op = insns_b[0].op = (uint8_t)JOP_ADD_I32_IMM;
  insns_a[0].a = insns_b[0].a = 1; insns_a[0].b = insns_b[0].b = 0;
  insns_a[0].imm = 5;
  insns_b[0].imm = 6; /* the only difference -- same signature, different body */
  insns_a[1].op = insns_b[1].op = (uint8_t)JOP_RET;
  insns_a[1].a = insns_b[1].a = 1;

  jello_bc_function fa, fb;
  jello_bc_module ma, mb;
  build_one_func_module(types_a, r_a, insns_a, 2, 2, 1, &fa, &ma);
  build_one_func_module(types_b, r_b, insns_b, 2, 2, 1, &fb, &mb);

  assert(jello_reload_analyze_function(&ma, &fa, &mb, &fb) == JELLO_RELOAD_FN_CHANGED);
  printf("test_reload_analyze_changed: PASS\n");
}

static void test_reload_analyze_signature_conflict_arg_count(void) {
  jello_type_entry types_a[1], types_b[1];
  jello_type_id r_a[2] = {0, 0}, r_b[3] = {0, 0, 0};
  jello_insn insns_a[2], insns_b[2];
  memset(insns_a, 0, sizeof(insns_a));
  memset(insns_b, 0, sizeof(insns_b));
  insns_a[0].op = (uint8_t)JOP_ADD_I32_IMM; insns_a[0].a = 1; insns_a[0].b = 0; insns_a[0].imm = 5;
  insns_a[1].op = (uint8_t)JOP_RET; insns_a[1].a = 1;
  insns_b[0].op = (uint8_t)JOP_ADD_I32; insns_b[0].a = 2; insns_b[0].b = 0; insns_b[0].c = 1;
  insns_b[1].op = (uint8_t)JOP_RET; insns_b[1].a = 2;

  jello_bc_function fa, fb;
  jello_bc_module ma, mb;
  build_one_func_module(types_a, r_a, insns_a, 2, 2, 1, &fa, &ma);
  build_one_func_module(types_b, r_b, insns_b, 2, 3, 2, &fb, &mb); /* nargs 1 -> 2 */

  assert(jello_reload_analyze_function(&ma, &fa, &mb, &fb) == JELLO_RELOAD_FN_SIGNATURE_CONFLICT);
  printf("test_reload_analyze_signature_conflict_arg_count: PASS\n");
}

static void test_reload_analyze_signature_conflict_ret_kind(void) {
  jello_type_entry types_a[2], types_b[2];
  jello_type_id r_a[1] = {0}, r_b[1] = {1};
  jello_insn insns_a[1], insns_b[1];
  memset(insns_a, 0, sizeof(insns_a));
  memset(insns_b, 0, sizeof(insns_b));
  insns_a[0].op = insns_b[0].op = (uint8_t)JOP_RET;
  insns_a[0].a = insns_b[0].a = 0;

  types_a[0].kind = JELLO_T_I32; memset(&types_a[0].as, 0, sizeof(types_a[0].as));
  types_a[1].kind = JELLO_T_BOOL; memset(&types_a[1].as, 0, sizeof(types_a[1].as));
  types_b[0].kind = JELLO_T_I32; memset(&types_b[0].as, 0, sizeof(types_b[0].as));
  types_b[1].kind = JELLO_T_BOOL; memset(&types_b[1].as, 0, sizeof(types_b[1].as));

  jello_bc_function fa, fb;
  jello_bc_module ma, mb;
  fa.name_atom = fb.name_atom = JELLO_BC_NO_NAME;
  fa.nargs = fb.nargs = 0;
  fa.nregs = fb.nregs = 1;
  fa.cap_start = fb.cap_start = 1;
  fa.reg_types = r_a; fb.reg_types = r_b; /* I32 vs BOOL return */
  fa.ninsns = fb.ninsns = 1;
  fa.insns = insns_a; fb.insns = insns_b;
  memset(&ma, 0, sizeof(ma)); ma.ntypes = 2; ma.types = types_a; ma.nfuncs = 1; ma.funcs = &fa;
  memset(&mb, 0, sizeof(mb)); mb.ntypes = 2; mb.types = types_b; mb.nfuncs = 1; mb.funcs = &fb;

  assert(jello_reload_analyze_function(&ma, &fa, &mb, &fb) == JELLO_RELOAD_FN_SIGNATURE_CONFLICT);
  printf("test_reload_analyze_signature_conflict_ret_kind: PASS\n");
}

static void test_module_create_rejects_empty_module(void) {
  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.nfuncs = 0;
  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm == NULL);
  printf("test_module_create_rejects_empty_module: PASS\n");
}

#if defined(__x86_64__)

static void build_three_leaf_module(jello_type_entry* types, jello_type_id* r0,
    jello_type_id* r1, jello_type_id* r2, jello_insn* e0, jello_insn* e1, jello_insn* e2,
    jello_bc_function* funcs, jello_bc_module* m) {
  types[0].kind = JELLO_T_I32;
  memset(&types[0].as, 0, sizeof(types[0].as));
  r0[0] = 0; r1[0] = 0; r2[0] = 0;

  e0[0].op = (uint8_t)JOP_CALL; e0[0].a = 0; e0[0].b = 0; e0[0].c = 0; e0[0].imm = 1; /* -> func[1] */
  e0[1].op = (uint8_t)JOP_RET; e0[1].a = 0;
  e1[0].op = (uint8_t)JOP_CONST_I32; e1[0].a = 0; e1[0].imm = 100;
  e1[1].op = (uint8_t)JOP_RET; e1[1].a = 0;
  e2[0].op = (uint8_t)JOP_CONST_I32; e2[0].a = 0; e2[0].imm = 200;
  e2[1].op = (uint8_t)JOP_RET; e2[1].a = 0;

  funcs[0].name_atom = funcs[1].name_atom = funcs[2].name_atom = JELLO_BC_NO_NAME;
  funcs[0].nargs = 0; funcs[0].nregs = 1; funcs[0].cap_start = 1; funcs[0].reg_types = r0; funcs[0].ninsns = 2; funcs[0].insns = e0;
  funcs[1].nargs = 0; funcs[1].nregs = 1; funcs[1].cap_start = 1; funcs[1].reg_types = r1; funcs[1].ninsns = 2; funcs[1].insns = e1;
  funcs[2].nargs = 0; funcs[2].nregs = 1; funcs[2].cap_start = 1; funcs[2].reg_types = r2; funcs[2].ninsns = 2; funcs[2].insns = e2;

  memset(m, 0, sizeof(*m));
  m->ntypes = 1; m->types = types; m->nfuncs = 3; m->funcs = funcs; m->entry = 0;
}

static void test_begin_end_patch_redirects_live_call(void) {
  jello_type_entry types[1];
  jello_type_id r0[1], r1[1], r2[1];
  jello_insn e0[2], e1[2], e2[2];
  jello_bc_function funcs[3];
  jello_bc_module m;
  build_three_leaf_module(types, r0, r1, r2, e0, e1, e2, funcs, &m);

  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm != NULL);

  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, jm, &m, &out);
  assert(st == JELLO_EXEC_OK && out.kind == JELLO_NUM_I32 && out.as.i32 == 100);

  size_t f0_start = jm->funcs[0].blob_off;
  size_t f0_end = f0_start + jm->funcs[0].size;
  size_t f1_entry = jm->funcs[1].blob_off + jm->funcs[1].body_off;
  size_t f2_entry = jm->funcs[2].blob_off + jm->funcs[2].body_off;
  size_t call_site = (size_t)-1;
  for(size_t i = f0_start; i + 5 <= f0_end; i++) {
    if(jm->blob.mem[i] != 0xE8) continue;
    int32_t rel;
    memcpy(&rel, jm->blob.mem + i + 1, 4);
    size_t target = (size_t)((int64_t)(i + 5) + (int64_t)rel);
    if(target == f1_entry) { call_site = i; break; }
  }
  assert(call_site != (size_t)-1);

  assert(jm->blob.finalized == 1);
  assert(jit_exec_buf_begin_patch(&jm->blob) == 1);
  int32_t new_rel = (int32_t)(f2_entry - (call_site + 5));
  jello_jit_patch_call_rel32(&jm->blob, call_site, new_rel);
  assert(jit_exec_buf_end_patch(&jm->blob) == 1);
  assert(jm->blob.finalized == 1); /* untouched by either call, per Sec 4's own contract */

  memset(&out, 0, sizeof(out));
  st = jello_vm_exec_numeric_with_jit(vm, jm, &m, &out);
  assert(st == JELLO_EXEC_OK && out.kind == JELLO_NUM_I32 && out.as.i32 == 200);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_begin_end_patch_redirects_live_call: PASS\n");
}

static int32_t fib_ref(int32_t n) {
  return (n < 2) ? n : fib_ref(n - 1) + fib_ref(n - 2);
}

static void test_persistent_handle_no_per_call_recompile(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_fib_module(types, r0, r1, e0, e1, funcs, &m, 15);

  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);

  jello_jit_module_prepare_count = 0;
  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm != NULL);
  assert(jello_jit_module_prepare_count == 1);

  int32_t expected = fib_ref(15);
  for(int i = 0; i < 20; i++) {
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, jm, &m, &out);
    assert(st == JELLO_EXEC_OK);
    assert(out.kind == JELLO_NUM_I32);
    assert(out.as.i32 == expected);
  }
  assert(jello_jit_module_prepare_count == 1);

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_persistent_handle_no_per_call_recompile: PASS (prepare_count=%u)\n",
      jello_jit_module_prepare_count);
}

static void test_persistent_handle_matches_fresh_path(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2];
  jello_bc_module m;

  for(uint32_t n = 0; n <= 12; n++) {
    build_fib_module(types, r0, r1, e0, e1, funcs, &m, n);

    jello_vm* vm_fresh = jello_vm_create();
    jello_numeric_result out_fresh;
    memset(&out_fresh, 0, sizeof(out_fresh));
    jello_exec_status st_fresh = jello_vm_exec_numeric(vm_fresh, &m, &out_fresh);
    jello_vm_destroy(vm_fresh);

    jello_vm* vm_handle = jello_vm_create();
    jello_jit_module* jm = jello_jit_module_create(&m);
    assert(jm != NULL);
    jello_numeric_result out_handle;
    memset(&out_handle, 0, sizeof(out_handle));
    jello_exec_status st_handle = jello_vm_exec_numeric_with_jit(vm_handle, jm, &m, &out_handle);
    jello_jit_module_destroy(jm);
    jello_vm_destroy(vm_handle);

    assert(st_fresh == st_handle);
    assert(out_fresh.kind == out_handle.kind);
    assert(memcmp(&out_fresh.as, &out_handle.as, sizeof(out_fresh.as)) == 0);
  }
  printf("test_persistent_handle_matches_fresh_path: PASS\n");
}

static void test_persistent_handle_null_falls_back_correctly(void) {
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_fib_module(types, r0, r1, e0, e1, funcs, &m, 8);

  jello_vm* vm = jello_vm_create();
  jello_numeric_result out;
  memset(&out, 0, sizeof(out));
  jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, NULL, &m, &out);
  jello_vm_destroy(vm);

  assert(st == JELLO_EXEC_OK);
  assert(out.kind == JELLO_NUM_I32);
  assert(out.as.i32 == fib_ref(8));
  printf("test_persistent_handle_null_falls_back_correctly: PASS\n");
}

#if defined(__x86_64__) || defined(__aarch64__)
static void test_wx_icache_repeated_create_destroy_then_persistent(void) {
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);
  for(int iter = 0; iter < 50; iter++) {
    jello_type_entry types[2];
    jello_type_id r0[2], r1[7];
    jello_insn e0[3], e1[9];
    jello_bc_function funcs[2];
    jello_bc_module m;
    build_fib_module(types, r0, r1, e0, e1, funcs, &m, 10);

    jello_jit_module* jm = jello_jit_module_create(&m);
    assert(jm != NULL);

    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, jm, &m, &out);
    assert(st == JELLO_EXEC_OK);
    assert(out.kind == JELLO_NUM_I32);
    assert(out.as.i32 == fib_ref(10));

    jello_jit_module_destroy(jm);
  }
  jello_type_entry types[2];
  jello_type_id r0[2], r1[7];
  jello_insn e0[3], e1[9];
  jello_bc_function funcs[2];
  jello_bc_module m;
  build_fib_module(types, r0, r1, e0, e1, funcs, &m, 15);

  jello_jit_module* jm = jello_jit_module_create(&m);
  assert(jm != NULL);

  int32_t expected = fib_ref(15);
  for(int i = 0; i < 20; i++) {
    jello_numeric_result out;
    memset(&out, 0, sizeof(out));
    jello_exec_status st = jello_vm_exec_numeric_with_jit(vm, jm, &m, &out);
    assert(st == JELLO_EXEC_OK);
    assert(out.kind == JELLO_NUM_I32);
    assert(out.as.i32 == expected);
  }

  jello_jit_module_destroy(jm);
  jello_vm_destroy(vm);
  printf("test_wx_icache_repeated_create_destroy_then_persistent: PASS\n");
}
#endif

#endif

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  test_reload_analyze_unchanged();
  test_reload_analyze_changed();
  test_reload_analyze_signature_conflict_arg_count();
  test_reload_analyze_signature_conflict_ret_kind();
  test_module_create_destroy_roundtrip();
  test_module_create_rejects_empty_module();
#if defined(__x86_64__)
  test_persistent_handle_no_per_call_recompile();
  test_persistent_handle_matches_fresh_path();
  test_persistent_handle_null_falls_back_correctly();
  test_begin_end_patch_redirects_live_call();
#else
  printf("test_persistent_handle_no_per_call_recompile: SKIPPED (host is not x86_64)\n");
  printf("test_persistent_handle_matches_fresh_path: SKIPPED (host is not x86_64)\n");
  printf("test_persistent_handle_null_falls_back_correctly: SKIPPED (host is not x86_64)\n");
  printf("test_begin_end_patch_redirects_live_call: SKIPPED (host is not x86_64)\n");
#endif
#if defined(__x86_64__)
  test_wx_icache_repeated_create_destroy_then_persistent();
#endif
  printf("jit_reload_tests: all tests passed\n");
  return 0;
}
