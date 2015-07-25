// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* -- shared-heap threading + stop-the-world correctness -- */

#include <jello/internal.h>
#include <jello/jdll.h>

#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void watchdog_handler(int sig) {
  (void)sig;
  fprintf(stderr, "WATCHDOG: test took too long, aborting\n");
  fflush(stderr);
  _exit(99);
}

static double now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* -- fixture 1: two joined VMs, one allocating, one blocked in a native call -- */

#define ALLOC_ROUNDS 4000
#define SLEEP_MS 60

typedef struct alloc_thread_arg {
  jello_vm* vm;
  uint32_t collections_seen;
} alloc_thread_arg;

static void* alloc_thread_main(void* argp) {
  alloc_thread_arg* arg = (alloc_thread_arg*)argp;
  for(int i = 0; i < ALLOC_ROUNDS; i++) {
    jello_bytes* b = jello_bytes_new(arg->vm, 300);
    (void)b;
  }
  uint32_t live = 0, collections = 0;
  double pause_ms = 0.0;
  jello_vm_gc_stats(arg->vm, &live, &collections, &pause_ms);
  arg->collections_seen = collections;
  return NULL;
}

static jello_bc_module* build_sleep_module(int ms) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));

  static const char* names[] = {"test_native", "sleep_ms"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 2u);
  atoms[0] = names[0];
  atoms[1] = names[1];
  m->natoms = 2;
  m->atoms = atoms;

  jello_type_entry* types = (jello_type_entry*)calloc(2, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  m->ntypes = 2;
  m->types = types;

  jello_import_sig* imports = (jello_import_sig*)calloc(1, sizeof(jello_import_sig));
  imports[0].library_atom = 0;
  imports[0].export_atom = 1;
  imports[0].ret_kind = (uint8_t)JELLO_T_BOOL;
  imports[0].arity = 1;
  imports[0].arg_kinds[0] = (uint8_t)JELLO_T_I32;

  jello_jdll_handle* h = jello_jdll_load("test_native");
  if(!h) { fprintf(stderr, "build_sleep_module: failed to load test_native.jdll (check JELLO_JDLL_PATH)\n"); abort(); }
  const jello_jdll_export* exp = jello_jdll_find_export(h, "sleep_ms");
  if(!exp) { fprintf(stderr, "build_sleep_module: test_native has no sleep_ms export\n"); abort(); }
  imports[0].resolved = exp;
  m->nimports = 1;
  m->imports = imports;
  jello_jdll_handle** handles = (jello_jdll_handle**)malloc(sizeof(jello_jdll_handle*));
  handles[0] = h;
  m->jdll_handles = handles;
  m->njdll_handles = 1;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 2u);
  reg_types[0] = 0; /* r0: I32 */
  reg_types[1] = 1; /* r1: Bool */

  /* 0: CONST_I32 r0 = ms
   * 1: CALL_NATIVE r1 = sleep_ms(r0)
   * 2: RET r1
   */
  jello_insn* insns = (jello_insn*)calloc(3, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = (uint32_t)ms;
  insns[1].op = (uint8_t)JOP_CALL_NATIVE; insns[1].a = 1; insns[1].b = 0; insns[1].c = 1; insns[1].imm = 0;
  insns[2].op = (uint8_t)JOP_RET; insns[2].a = 1;

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 2;
  funcs[0].cap_start = 2;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = 3;
  funcs[0].insns = insns;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

typedef struct blocking_thread_arg {
  jello_vm* vm;
  const jello_bc_module* m;
  int ok;
} blocking_thread_arg;

static void* blocking_thread_main(void* argp) {
  blocking_thread_arg* arg = (blocking_thread_arg*)argp;
  jello_numeric_result out;
  jello_exec_status st = jello_vm_exec_numeric(arg->vm, arg->m, &out);
  arg->ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_BOOL && out.as.b == 1);
  return NULL;
}

static void test_shared_heap_alloc_and_blocking_call(void) {
  printf("== fixture: shared heap, one allocating thread + one blocking-native-call thread ==\n");

  jello_vm* vm_a = jello_vm_create();
  assert(vm_a);
  jello_vm* vm_b = jello_vm_create_joined(vm_a);
  assert(vm_b);

  jello_bc_module* m = build_sleep_module(SLEEP_MS);

  alloc_thread_arg a_arg = {vm_a, 0};
  blocking_thread_arg b_arg = {vm_b, m, 0};

  double t0 = now_ms();
  pthread_t ta, tb;
  assert(pthread_create(&ta, NULL, alloc_thread_main, &a_arg) == 0);
  assert(pthread_create(&tb, NULL, blocking_thread_main, &b_arg) == 0);
  assert(pthread_join(ta, NULL) == 0);
  assert(pthread_join(tb, NULL) == 0);
  double elapsed = now_ms() - t0;

  printf("  collections_seen=%u blocking_call_ok=%d elapsed_ms=%.1f\n",
         a_arg.collections_seen, b_arg.ok, elapsed);

  assert(b_arg.ok && "blocking native call must still return the correct value");
  assert(a_arg.collections_seen > 0 && "allocation loop must have triggered at least one real collection");
  assert(elapsed < (double)(SLEEP_MS * 8) && "threads must not have serialised behind one another");

  free((void*)m->types);
  free((void*)m->atoms);
  free((void*)m->imports);
  jello_jdll_unload(((jello_jdll_handle**)m->jdll_handles)[0]);
  free(m->jdll_handles);
  free((void*)m->funcs[0].reg_types);
  free((void*)m->funcs[0].insns);
  free((void*)m->funcs);
  free(m);

  jello_vm_destroy(vm_b);
  jello_vm_destroy(vm_a);
  printf("  PASS\n");
}

/* -- fixture 2: the backward-JMP safepoint check actually fires -- */

#define LOOP_ITERATIONS 1000000

static jello_bc_module* build_counting_loop_module(int32_t iterations) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  jello_type_entry* types = (jello_type_entry*)calloc(2, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BOOL;
  m->ntypes = 2;
  m->types = types;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 2u);
  reg_types[0] = 0; /* r0: I32 counter */
  reg_types[1] = 1; /* r1: Bool loop condition */

  /* 0: CONST_I32 r0 = 0
   * 1: LT_I32_IMM r1 = r0 < iterations
   * 2: ADD_I32_IMM r0 = r0 + 1
   * 3: JMP_IF r1, -3   (back to 1)
   * 4: RET r0
   */
  jello_insn* insns = (jello_insn*)calloc(5, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CONST_I32; insns[0].a = 0; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_LT_I32_IMM; insns[1].a = 1; insns[1].b = 0; insns[1].imm = (uint32_t)iterations;
  insns[2].op = (uint8_t)JOP_ADD_I32_IMM; insns[2].a = 0; insns[2].b = 0; insns[2].imm = 1;
  insns[3].op = (uint8_t)JOP_JMP_IF; insns[3].a = 1; insns[3].imm = (uint32_t)(int32_t)-3;
  insns[4].op = (uint8_t)JOP_RET; insns[4].a = 0;

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 2;
  funcs[0].cap_start = 2;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = 5;
  funcs[0].insns = insns;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

typedef struct loop_thread_arg {
  jello_vm* vm;
  const jello_bc_module* m;
  int ok;
} loop_thread_arg;

static void* loop_thread_main(void* argp) {
  loop_thread_arg* arg = (loop_thread_arg*)argp;
  jello_numeric_result out;
  jello_exec_status st = jello_vm_exec_numeric(arg->vm, arg->m, &out);
  arg->ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_I32 && out.as.i32 == LOOP_ITERATIONS + 1);
  return NULL;
}

typedef struct collector_thread_arg {
  jello_vm* vm;
  atomic_int* stop;
} collector_thread_arg;

static void* collector_thread_main(void* argp) {
  collector_thread_arg* arg = (collector_thread_arg*)argp;
  struct timespec pause = {0, 200 * 1000};
  while(!atomic_load(arg->stop)) {
    for(int i = 0; i < 200; i++) {
      jello_bytes* b = jello_bytes_new(arg->vm, 64);
      (void)b;
    }
    jello_gc_collect(arg->vm);
    nanosleep(&pause, NULL);
  }
  return NULL;
}

static void test_safepoint_at_loop_backedge(void) {
  printf("== fixture: backward-JMP safepoint check under concurrent collection ==\n");

  jello_vm* vm_loop = jello_vm_create();
  assert(vm_loop);
  jello_vm* vm_collector = jello_vm_create_joined(vm_loop);
  assert(vm_collector);

  jello_bc_module* m = build_counting_loop_module(LOOP_ITERATIONS);

  atomic_int stop;
  atomic_init(&stop, 0);
  collector_thread_arg c_arg = {vm_collector, &stop};
  loop_thread_arg l_arg = {vm_loop, m, 0};

  pthread_t tc, tl;
  assert(pthread_create(&tc, NULL, collector_thread_main, &c_arg) == 0);
  assert(pthread_create(&tl, NULL, loop_thread_main, &l_arg) == 0);
  assert(pthread_join(tl, NULL) == 0);
  atomic_store(&stop, 1);
  assert(pthread_join(tc, NULL) == 0);

  printf("  loop_ok=%d\n", l_arg.ok);
  assert(l_arg.ok && "counting loop must finish with the correct result while collections run concurrently");

  free((void*)m->types);
  free((void*)m->funcs[0].reg_types);
  free((void*)m->funcs[0].insns);
  free((void*)m->funcs);
  free(m);

  jello_vm_destroy(vm_collector);
  jello_vm_destroy(vm_loop);
  printf("  PASS\n");
}

/* -- fixture 3: the std JDLL's file I/O, get_env, argv -- */

typedef struct insn_buf {
  jello_insn* items;
  uint32_t len;
  uint32_t cap;
} insn_buf;

static void ib_push(insn_buf* b, jello_op op, uint8_t a, uint8_t bb, uint8_t cc, int32_t imm) {
  if(b->len == b->cap) {
    b->cap = b->cap ? b->cap * 2u : 16u;
    b->items = (jello_insn*)realloc(b->items, sizeof(jello_insn) * (size_t)b->cap);
  }
  jello_insn* in = &b->items[b->len++];
  in->op = (uint8_t)op;
  in->a = a;
  in->b = bb;
  in->c = cc;
  in->imm = (uint32_t)imm;
}

static void emit_bytes_literal(insn_buf* b, uint8_t dst, uint8_t idx_reg, uint8_t byte_reg,
                                uint8_t len_reg, const char* s) {
  uint32_t len = (uint32_t)strlen(s);
  ib_push(b, JOP_CONST_I32, len_reg, 0, 0, (int32_t)len);
  ib_push(b, JOP_BYTES_NEW, dst, len_reg, 0, 0);
  for(uint32_t i = 0; i < len; i++) {
    ib_push(b, JOP_CONST_I32, idx_reg, 0, 0, (int32_t)i);
    ib_push(b, JOP_CONST_I32, byte_reg, 0, 0, (int32_t)(unsigned char)s[i]);
    ib_push(b, JOP_BYTES_SET_U8, byte_reg, dst, idx_reg, 0);
  }
}

static jello_bc_module* build_std_file_roundtrip_module(jello_jdll_handle* h, const char* path, const char* data) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));

  static const char* names[] = {"std", "file_write", "file_read"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 3u);
  atoms[0] = names[0]; atoms[1] = names[1]; atoms[2] = names[2];
  m->natoms = 3;
  m->atoms = atoms;

  jello_type_entry* types = (jello_type_entry*)calloc(3, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BYTES;
  types[2].kind = JELLO_T_BOOL;
  m->ntypes = 3;
  m->types = types;

  jello_import_sig* imports = (jello_import_sig*)calloc(2, sizeof(jello_import_sig));
  imports[0].library_atom = 0; imports[0].export_atom = 1; /* file_write */
  imports[0].ret_kind = (uint8_t)JELLO_T_BOOL; imports[0].arity = 2;
  imports[0].arg_kinds[0] = (uint8_t)JELLO_T_BYTES; imports[0].arg_kinds[1] = (uint8_t)JELLO_T_BYTES;
  imports[1].library_atom = 0; imports[1].export_atom = 2; /* file_read */
  imports[1].ret_kind = (uint8_t)JELLO_T_BYTES; imports[1].arity = 1;
  imports[1].arg_kinds[0] = (uint8_t)JELLO_T_BYTES;

  const jello_jdll_export* exp_write = jello_jdll_find_export(h, "file_write");
  const jello_jdll_export* exp_read = jello_jdll_find_export(h, "file_read");
  if(!exp_write || !exp_read) { fprintf(stderr, "std.jdll missing file_write/file_read\n"); abort(); }
  imports[0].resolved = exp_write;
  imports[1].resolved = exp_read;
  m->nimports = 2;
  m->imports = imports;
  m->jdll_handles = NULL; /* not owned by this module -- main() unloads "std" once */
  m->njdll_handles = 0;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 9u);
  reg_types[0] = 1; reg_types[1] = 1; reg_types[2] = 0; reg_types[3] = 0; reg_types[4] = 0;
  reg_types[5] = 2; reg_types[6] = 1; reg_types[7] = 0; reg_types[8] = 2;

  insn_buf ib = {0};
  emit_bytes_literal(&ib, 0, 2, 3, 4, path);
  emit_bytes_literal(&ib, 1, 2, 3, 4, data);
  ib_push(&ib, JOP_CALL_NATIVE, 5, 0, 2, 0); /* r5 = file_write(r0, r1) */
  ib_push(&ib, JOP_CALL_NATIVE, 6, 0, 1, 1); /* r6 = file_read(r0) */
  ib_push(&ib, JOP_CONST_I32, 2, 0, 0, 0);
  ib_push(&ib, JOP_BYTES_GET_U8, 7, 6, 2, 0); /* r7 = read_back[0] */
  ib_push(&ib, JOP_EQ_I32_IMM, 8, 7, 0, (int32_t)(unsigned char)data[0]); /* r8 = (r7 == data[0]) */
  ib_push(&ib, JOP_RET, 8, 0, 0, 0);

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 9;
  funcs[0].cap_start = 9;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = ib.len;
  funcs[0].insns = ib.items;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static void free_std_module(jello_bc_module* m) {
  free((void*)m->types);
  free((void*)m->atoms);
  free((void*)m->imports);
  free((void*)m->funcs[0].reg_types);
  free((void*)m->funcs[0].insns);
  free((void*)m->funcs);
  free(m);
}

static void test_std_jdll_file_roundtrip(jello_jdll_handle* std_h) {
  printf("== fixture: std JDLL file_write/file_read round-trip ==\n");
  const char* path = "/tmp/jello_chapter7_std_test.tmp";
  const char* data = "hello, chapter 7";

  jello_vm* vm = jello_vm_create();
  jello_bc_module* m = build_std_file_roundtrip_module(std_h, path, data);

  jello_numeric_result out;
  jello_exec_status st = jello_vm_exec_numeric(vm, m, &out);
  int ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_BOOL && out.as.b == 1);
  printf("  round_trip_ok=%d\n", ok);
  assert(ok && "file_write then file_read must round-trip the same first byte");

  remove(path);
  free_std_module(m);
  jello_vm_destroy(vm);
  printf("  PASS\n");
}

static jello_bc_module* build_std_get_env_module(jello_jdll_handle* h, const char* name, char expected_first_char) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));

  static const char* names[] = {"std", "get_env"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 2u);
  atoms[0] = names[0]; atoms[1] = names[1];
  m->natoms = 2;
  m->atoms = atoms;

  jello_type_entry* types = (jello_type_entry*)calloc(3, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  types[1].kind = JELLO_T_BYTES;
  types[2].kind = JELLO_T_BOOL;
  m->ntypes = 3;
  m->types = types;

  jello_import_sig* imports = (jello_import_sig*)calloc(1, sizeof(jello_import_sig));
  imports[0].library_atom = 0; imports[0].export_atom = 1;
  imports[0].ret_kind = (uint8_t)JELLO_T_BYTES; imports[0].arity = 1;
  imports[0].arg_kinds[0] = (uint8_t)JELLO_T_BYTES;

  const jello_jdll_export* exp = jello_jdll_find_export(h, "get_env");
  if(!exp) { fprintf(stderr, "std.jdll missing get_env\n"); abort(); }
  imports[0].resolved = exp;
  m->nimports = 1;
  m->imports = imports;
  m->jdll_handles = NULL;
  m->njdll_handles = 0;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 7u);
  reg_types[0] = 1; reg_types[1] = 0; reg_types[2] = 0; reg_types[3] = 0;
  reg_types[4] = 1; reg_types[5] = 0; reg_types[6] = 2;

  insn_buf ib = {0};
  emit_bytes_literal(&ib, 0, 1, 2, 3, name);
  ib_push(&ib, JOP_CALL_NATIVE, 4, 0, 1, 0); /* r4 = get_env(r0) */
  ib_push(&ib, JOP_CONST_I32, 1, 0, 0, 0);
  ib_push(&ib, JOP_BYTES_GET_U8, 5, 4, 1, 0); /* r5 = r4[0] */
  ib_push(&ib, JOP_EQ_I32_IMM, 6, 5, 0, (int32_t)(unsigned char)expected_first_char);
  ib_push(&ib, JOP_RET, 6, 0, 0, 0);

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 7;
  funcs[0].cap_start = 7;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = ib.len;
  funcs[0].insns = ib.items;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static jello_bc_module* build_std_argv_count_module(jello_jdll_handle* h) {
  jello_bc_module* m = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));

  static const char* names[] = {"std", "argv_count"};
  const char** atoms = (const char**)malloc(sizeof(char*) * 2u);
  atoms[0] = names[0]; atoms[1] = names[1];
  m->natoms = 2;
  m->atoms = atoms;

  jello_type_entry* types = (jello_type_entry*)calloc(1, sizeof(jello_type_entry));
  types[0].kind = JELLO_T_I32;
  m->ntypes = 1;
  m->types = types;

  jello_import_sig* imports = (jello_import_sig*)calloc(1, sizeof(jello_import_sig));
  imports[0].library_atom = 0; imports[0].export_atom = 1;
  imports[0].ret_kind = (uint8_t)JELLO_T_I32; imports[0].arity = 0;

  const jello_jdll_export* exp = jello_jdll_find_export(h, "argv_count");
  if(!exp) { fprintf(stderr, "std.jdll missing argv_count\n"); abort(); }
  imports[0].resolved = exp;
  m->nimports = 1;
  m->imports = imports;
  m->jdll_handles = NULL;
  m->njdll_handles = 0;

  jello_type_id* reg_types = (jello_type_id*)malloc(sizeof(jello_type_id) * 1u);
  reg_types[0] = 0;

  jello_insn* insns = (jello_insn*)calloc(2, sizeof(jello_insn));
  insns[0].op = (uint8_t)JOP_CALL_NATIVE; insns[0].a = 0; insns[0].b = 0; insns[0].c = 0; insns[0].imm = 0;
  insns[1].op = (uint8_t)JOP_RET; insns[1].a = 0;

  jello_bc_function* funcs = (jello_bc_function*)calloc(1, sizeof(jello_bc_function));
  funcs[0].nregs = 1;
  funcs[0].cap_start = 1;
  funcs[0].reg_types = reg_types;
  funcs[0].ninsns = 2;
  funcs[0].insns = insns;
  m->nfuncs = 1;
  m->funcs = funcs;
  m->entry = 0;
  return m;
}

static void test_std_jdll_get_env_and_argv(jello_jdll_handle* std_h) {
  printf("== fixture: std JDLL get_env + argv ==\n");
  setenv("JELLO_CHAPTER7_TEST_VAR", "chapter7", 1);

  jello_vm* vm = jello_vm_create();
  jello_bc_module* m_env = build_std_get_env_module(std_h, "JELLO_CHAPTER7_TEST_VAR", 'c');
  jello_numeric_result out;
  jello_exec_status st = jello_vm_exec_numeric(vm, m_env, &out);
  int env_ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_BOOL && out.as.b == 1);
  printf("  get_env_ok=%d\n", env_ok);
  assert(env_ok && "get_env must return the value this test just set via setenv");
  free_std_module(m_env);

  jello_bc_module* m_argv = build_std_argv_count_module(std_h);
  st = jello_vm_exec_numeric(vm, m_argv, &out);
  int argv_ok = (st == JELLO_EXEC_OK && out.kind == JELLO_NUM_I32 && out.as.i32 >= 1);
  printf("  argv_count=%d argv_ok=%d\n", (st == JELLO_EXEC_OK) ? out.as.i32 : -1, argv_ok);
  assert(argv_ok && "argv_count must be at least 1 (argv[0], the process's own path)");
  free_std_module(m_argv);

  jello_vm_destroy(vm);
  printf("  PASS\n");
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  signal(SIGALRM, watchdog_handler);
  alarm(45);
  test_shared_heap_alloc_and_blocking_call();
  test_safepoint_at_loop_backedge();

  jello_jdll_handle* std_h = jello_jdll_load("std");
  if(!std_h) { fprintf(stderr, "main: failed to load std.jdll (check JELLO_JDLL_PATH)\n"); return 1; }
  test_std_jdll_file_roundtrip(std_h);
  test_std_jdll_get_env_and_argv(std_h);
  jello_jdll_unload(std_h);

  printf("chapter7_thread_tests: all fixtures passed\n");
  return 0;
}
