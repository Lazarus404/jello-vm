// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__)
static const char* trap_code_name(jello_trap_code tc) {
  switch(tc) {
    case JELLO_TRAP_NONE: return "NONE";
    case JELLO_TRAP_ARITHMETIC: return "ARITHMETIC";
    case JELLO_TRAP_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case JELLO_TRAP_NULL_DEREF: return "NULL_DEREF";
    case JELLO_TRAP_BOUNDS: return "BOUNDS";
    case JELLO_TRAP_TYPE_MISMATCH: return "TYPE_MISMATCH";
    default: return "UNKNOWN";
  }
}

static void print_numeric_result(const jello_numeric_result* r) {
  switch(r->kind) {
    case JELLO_NUM_VOID: printf("RESULT kind=VOID\n"); break;
    case JELLO_NUM_BOOL: printf("RESULT kind=BOOL value=%d\n", r->as.b); break;
    case JELLO_NUM_I32: printf("RESULT kind=I32 value=%d\n", r->as.i32); break;
    case JELLO_NUM_I64: printf("RESULT kind=I64 value=%lld\n", (long long)r->as.i64); break;
    case JELLO_NUM_F32: printf("RESULT kind=F32 value=%.9g\n", (double)r->as.f32); break;
    case JELLO_NUM_F64: printf("RESULT kind=F64 value=%.17g\n", r->as.f64); break;
  }
}
#endif

int main(int argc, char** argv) {
  if(argc != 2) {
    fprintf(stderr, "usage: jit_numeric_run <path.jlo>\n");
    return 1;
  }

#if !defined(__x86_64__)
  printf("JIT_SKIP\n");
  return 0;
#else
  const char* in_path = argv[1];
  FILE* f = fopen(in_path, "rb");
  if(!f) {
    fprintf(stderr, "%s: cannot open\n", in_path);
    return 1;
  }
  if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
  long size = ftell(f);
  if(size < 0) { fclose(f); return 1; }
  fseek(f, 0, SEEK_SET);
  uint8_t* data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
  if(!data) { fclose(f); return 1; }
  size_t n = fread(data, 1, (size_t)size, f);
  fclose(f);

  jello_bc_module* m = NULL;
  jello_bc_result rr = jello_bc_read(data, n, &m);
  free(data);
  if(rr.err) {
    fprintf(stderr, "%s: %s (offset %zu)\n", in_path, rr.msg, rr.offset);
    return 1;
  }

  jello_jit_code code;
  memset(&code, 0, sizeof(code));
  if(!jello_jit_compile_function(m, m->entry, &code)) {
    jello_bc_free(m);
    printf("JIT_SKIP\n");
    return 0;
  }

  jello_vm* vm = jello_vm_create();
  if(!vm) {
    jello_jit_code_free(&code);
    jello_bc_free(m);
    fprintf(stderr, "%s: out of memory creating VM\n", in_path);
    return 1;
  }

  atomic_store(&vm->in_exec, 1);
  vm_frame_reset(vm);
  vm->running_module = m;
  if(!vm_push_frame(vm, m, m->entry, 0, 0, 0, 0)) {
    atomic_store(&vm->in_exec, 0);
    jello_jit_code_free(&code);
    jello_vm_destroy(vm);
    jello_bc_free(m);
    fprintf(stderr, "%s: vm_push_frame failed\n", in_path);
    return 1;
  }

  jello_jit_entry_fn entry =
      (jello_jit_entry_fn)(void*)(code.buf.mem + code.entry_off);
  jello_numeric_result result;
  jello_exec_status st = entry(vm, jit_frame_entry_base(vm), &result);

  atomic_store(&vm->in_exec, 0);

  if(st == JELLO_EXEC_OK) {
    print_numeric_result(&result);
  } else {
    printf("TRAP code=%s\n", trap_code_name(jello_vm_last_trap_code(vm)));
  }

  jello_jit_code_free(&code);
  jello_vm_destroy(vm);
  jello_bc_free(m);
  return 0;
#endif
}