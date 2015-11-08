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

static int bytes_contain(const uint8_t* hay, size_t hay_len,
                         const uint8_t* needle, size_t needle_len) {
  if(needle_len > hay_len) return 0;
  for(size_t i = 0; i + needle_len <= hay_len; i++) {
    if(memcmp(hay + i, needle, needle_len) == 0) return 1;
  }
  return 0;
}

static void test_emitted_bytes_match_golden_sequence(const jello_jit_code* code) {
  static const uint8_t expected_prologue[] = {
    0x41, 0x54,             /* push r12 */
    0x41, 0x55,             /* push r13 */
    0x53,                   /* push rbx */
    0x49, 0x89, 0xFC,       /* mov r12, rdi */
    0x49, 0x89, 0xF5,       /* mov r13, rsi */
    0x48, 0x89, 0xD3,       /* mov rbx, rdx */
  };
  static const uint8_t void_kind_write[] = {
    0xC7, 0x83, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  static const uint8_t mov_rdi_r12[] = { 0x4C, 0x89, 0xE7 };
  static const uint8_t call_rax[] = { 0xFF, 0xD0 };
  static const uint8_t restore_pops[] = { 0x5B, 0x41, 0x5D, 0x41, 0x5C };
  static const uint8_t mov_eax_ok[] = { 0xB8, 0x00, 0x00, 0x00, 0x00 };
  static const uint8_t sub_rsp8[] = { 0x48, 0x83, 0xEC, 0x08 };
  static const uint8_t add_rsp8[] = { 0x48, 0x83, 0xC4, 0x08 };

  assert(code->body_off == 14);
  assert(code->body_off == sizeof(expected_prologue));
  assert(memcmp(code->buf.mem, expected_prologue, sizeof(expected_prologue)) == 0);

  size_t ep_end = code->body_off;
  int found_ret = 0;
  for(size_t i = code->body_off; i < code->buf.len; i++) {
    if(code->buf.mem[i] == 0xC3) {
      ep_end = i + 1;
      found_ret = 1;
      break;
    }
  }
  assert(found_ret);

  const uint8_t* ep = code->buf.mem + code->body_off;
  size_t ep_len = ep_end - code->body_off;

  assert(bytes_contain(ep, ep_len, void_kind_write, sizeof(void_kind_write)));
  assert(bytes_contain(ep, ep_len, mov_rdi_r12, sizeof(mov_rdi_r12)));
  assert(bytes_contain(ep, ep_len, call_rax, sizeof(call_rax)));
  assert(bytes_contain(ep, ep_len, restore_pops, sizeof(restore_pops)));
  assert(bytes_contain(ep, ep_len, mov_eax_ok, sizeof(mov_eax_ok)));
  assert(!bytes_contain(ep, ep_len, sub_rsp8, sizeof(sub_rsp8)));
  assert(!bytes_contain(ep, ep_len, add_rsp8, sizeof(add_rsp8)));

  int found_movabs = 0;
  for(size_t i = 0; i + 10 <= ep_len; i++) {
    if(ep[i] == 0x48 && ep[i + 1] == 0xB8) {
      uint64_t embedded;
      memcpy(&embedded, ep + i + 2, 8);
      uint64_t real;
      void (*pop_frame_fn)(jello_vm*) = vm_pop_frame;
      memcpy(&real, &pop_frame_fn, sizeof(real));
      assert(embedded == real);
      found_movabs = 1;
      break;
    }
  }
  assert(found_movabs);

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
  out.kind = JELLO_NUM_I32;
  jello_exec_status st = entry(vm, frame_base, &out);
  assert(st == JELLO_EXEC_OK);

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
  printf("test_noop_function_called_through_private_abi: SKIPPED (host is not x86_64)\n");
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
