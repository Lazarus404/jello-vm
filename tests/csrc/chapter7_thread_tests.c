// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* -- shared-heap threading + stop-the-world correctness -- */

#include <jello/internal.h>

#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void watchdog_handler(int sig) {
  (void)sig;
  fprintf(stderr, "WATCHDOG: test took too long, aborting\n");
  fflush(stderr);
  _exit(99);
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

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  signal(SIGALRM, watchdog_handler);
  alarm(45);
  test_safepoint_at_loop_backedge();

  printf("chapter7_thread_tests: all fixtures passed\n");
  return 0;
}
