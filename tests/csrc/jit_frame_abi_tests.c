// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* -- jit_frame_abi_tests -- */

static jello_bc_module build_noop_void_module(jello_type_entry* type_void,
                                              jello_type_id* reg_types,
                                              jello_insn* insns,
                                              jello_bc_function* func) {
  type_void->kind = JELLO_T_VOID;
  memset(&type_void->as, 0, sizeof(type_void->as));

  reg_types[0] = 0; /* register 0 -> types[0] (VOID) */

  insns[0].op = (uint8_t)JOP_RET;
  insns[0].a = 0;
  insns[0].b = 0;
  insns[0].c = 0;
  insns[0].imm = 0;

  func->nregs = 1;
  func->cap_start = 0;
  func->reg_types = reg_types;
  func->ninsns = 1;
  func->insns = insns;

  jello_bc_module m;
  memset(&m, 0, sizeof(m));
  m.ntypes = 1;
  m.types = type_void;
  m.nfuncs = 1;
  m.funcs = func;
  m.entry = 0;
  return m;
}

static void test_noop_function_compiles(jello_jit_code* code, const jello_bc_module* m) {
  int ok = jello_jit_compile_function(m, 0, code);
  assert(ok == 1);
  assert(code->buf.mem != NULL);
  assert(code->entry_off == 0);
  assert(code->body_off > code->entry_off);
  assert(code->body_off < code->buf.cap);
  printf("test_noop_function_compiles: PASS\n");
}

static void test_emitted_bytes_match_golden_sequence(const jello_jit_code* code) {
  static const uint8_t expected_prologue[] = {
    0x41, 0x54,             /* push r12 */
    0x41, 0x55,             /* push r13 */
    0x49, 0x89, 0xFC,       /* mov r12, rdi */
    0x49, 0x89, 0xF5,       /* mov r13, rsi */
  };
  static const uint8_t expected_epilogue_before_addr[] = {
    0xC7, 0x02, 0x00, 0x00, 0x00, 0x00, /* mov dword [rdx], 0 */
    0x4C, 0x89, 0xE7,                   /* mov rdi, r12 */
    0x48, 0x83, 0xEC, 0x08,             /* sub rsp, 8 */
    0x48, 0xB8,                         /* movabs rax, imm64 -- imm64 bytes follow, checked separately */
  };
  static const uint8_t expected_epilogue_after_addr[] = {
    0xFF, 0xD0,             /* call rax */
    0x48, 0x83, 0xC4, 0x08, /* add rsp, 8 */
    0x41, 0x5D,             /* pop r13 */
    0x41, 0x5C,             /* pop r12 */
    0xC3,                   /* ret */
  };

  assert(code->body_off == sizeof(expected_prologue));
  assert(memcmp(code->buf.mem, expected_prologue, sizeof(expected_prologue)) == 0);

  size_t off = code->body_off;
  assert(memcmp(code->buf.mem + off, expected_epilogue_before_addr, sizeof(expected_epilogue_before_addr)) == 0);
  off += sizeof(expected_epilogue_before_addr);

  uint64_t embedded_addr;
  memcpy(&embedded_addr, code->buf.mem + off, sizeof(embedded_addr));
  uint64_t real_addr;
  void (*pop_frame_fn)(jello_vm*) = vm_pop_frame;
  memcpy(&real_addr, &pop_frame_fn, sizeof(real_addr));
  assert(embedded_addr == real_addr);
  off += sizeof(embedded_addr);

  assert(memcmp(code->buf.mem + off, expected_epilogue_after_addr, sizeof(expected_epilogue_after_addr)) == 0);
  off += sizeof(expected_epilogue_after_addr);
  assert(off == sizeof(expected_prologue) + sizeof(expected_epilogue_before_addr) + sizeof(embedded_addr) + sizeof(expected_epilogue_after_addr));

  printf("test_emitted_bytes_match_golden_sequence: PASS\n");
}

#if defined(__x86_64__)
static void test_noop_function_called_through_private_abi(jello_jit_code* code, const jello_bc_module* m) {
  jello_vm* vm = jello_vm_create();
  assert(vm != NULL);

  atomic_store(&vm->in_exec, 1);
  vm_frame_reset(vm);
  vm->running_module = m;
  int pushed = vm_push_frame(vm, m, 0, 0, 0, 0, 0);
  assert(pushed == 1);
  uint32_t frames_before = vm->call_frames_len;

  uint8_t* frame_base = jit_frame_entry_base(vm);
  jello_jit_entry_fn entry = (jello_jit_entry_fn)(void*)(code->buf.mem + code->entry_off);

  jello_numeric_result out;
  out.kind = JELLO_NUM_I32; /* deliberately not VOID -- proves the epilogue itself writes it */
  entry(vm, frame_base, &out);

  atomic_store(&vm->in_exec, 0);

  assert(out.kind == JELLO_NUM_VOID);
  assert(vm->call_frames_len == frames_before - 1u);
  assert(vm->call_frames_len == 0u);

  jello_vm_destroy(vm);
  printf("test_noop_function_called_through_private_abi: PASS\n");
}
#else
static void test_noop_function_called_through_private_abi(jello_jit_code* code, const jello_bc_module* m) {
  (void)code;
  (void)m;
  printf("test_noop_function_called_through_private_abi: SKIPPED (host is not x86_64 -- "
         "emitted bytes are real x64 machine code and must not be executed here)\n");
}
#endif

int main(void) {
  jello_type_entry type_void;
  jello_type_id reg_types[1];
  jello_insn insns[1];
  jello_bc_function func;
  jello_bc_module m = build_noop_void_module(&type_void, reg_types, insns, &func);

  jello_jit_code code;
  test_noop_function_compiles(&code, &m);
  test_emitted_bytes_match_golden_sequence(&code);
  test_noop_function_called_through_private_abi(&code, &m);
  jello_jit_code_free(&code);

  printf("jit_frame_abi_tests: all tests passed\n");
  return 0;
}
