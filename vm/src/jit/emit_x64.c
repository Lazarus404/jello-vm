// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
#include <string.h>

/* -- x86-64 byte encoding -- */

enum {
  X64_RAX = 0, X64_RCX = 1, X64_RDX = 2, X64_RBX = 3,
  X64_RSP = 4, X64_RBP = 5, X64_RSI = 6, X64_RDI = 7,
};

static void emit_push_r64(jit_exec_buf* buf, unsigned reg) {
  uint8_t bytes[2];
  bytes[0] = (uint8_t)(0x40 | ((reg >= 8) ? 0x01 : 0)); /* REX.B */
  bytes[1] = (uint8_t)(0x50 + (reg & 7u));
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_pop_r64(jit_exec_buf* buf, unsigned reg) {
  uint8_t bytes[2];
  bytes[0] = (uint8_t)(0x40 | ((reg >= 8) ? 0x01 : 0)); /* REX.B */
  bytes[1] = (uint8_t)(0x58 + (reg & 7u));
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_mov_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  bytes[0] = (uint8_t)(0x48 | ((dst >= 8) ? 0x01 : 0) /* REX.B: r/m field */
                            | ((src >= 8) ? 0x04 : 0) /* REX.R: reg field */);
  bytes[1] = 0x89;
  bytes[2] = (uint8_t)(0xC0 | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_mov_mem32_imm32(jit_exec_buf* buf, unsigned dst_reg, uint32_t imm) {
  uint8_t bytes[6];
  bytes[0] = 0xC7;
  bytes[1] = (uint8_t)(0x00 | (dst_reg & 7u)); /* mod=00, reg=000 (/0 = MOV), rm=dst_reg */
  memcpy(&bytes[2], &imm, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_rsp_adjust(jit_exec_buf* buf, int is_add, uint8_t imm8) {
  uint8_t bytes[4];
  bytes[0] = 0x48; /* REX.W */
  bytes[1] = 0x83;
  bytes[2] = (uint8_t)(0xC0 | (is_add ? 0x00u : 0x28u) | (unsigned)X64_RSP); /* /0=add, /5=sub, rm=RSP */
  bytes[3] = imm8;
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_movabs_rax(jit_exec_buf* buf, uint64_t imm) {
  uint8_t bytes[10];
  bytes[0] = 0x48;
  bytes[1] = 0xB8;
  memcpy(&bytes[2], &imm, 8);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static uint64_t fn_addr_bits(void (*fn)(jello_vm*)) {
  uint64_t bits;
  memcpy(&bits, &fn, sizeof(bits));
  return bits;
}

static void emit_call_rax(jit_exec_buf* buf) {
  uint8_t bytes[2] = { 0xFF, 0xD0 };
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_ret(jit_exec_buf* buf) {
  uint8_t byte = 0xC3;
  jit_exec_buf_write(buf, &byte, 1);
}

/* -- entry_off + body_off -- */
static void emit_prologue(jit_exec_buf* buf, size_t* out_body_off) {
  emit_push_r64(buf, (unsigned)JIT_REG_VM);
  emit_push_r64(buf, (unsigned)JIT_REG_BASE);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_VM, X64_RDI);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_BASE, X64_RSI);
  *out_body_off = buf->len;
}

static void emit_epilogue_void(jit_exec_buf* buf, void (*vm_pop_frame_fn)(jello_vm*)) {
  emit_mov_mem32_imm32(buf, X64_RDX, (uint32_t)JELLO_NUM_VOID);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_rsp_adjust(buf, 0, 8); /* sub rsp, 8 -- realigns RSP to 16 for the call below */
  emit_movabs_rax(buf, fn_addr_bits(vm_pop_frame_fn));
  emit_call_rax(buf);
  emit_rsp_adjust(buf, 1, 8); /* add rsp, 8 -- undo the alignment padding */
  emit_pop_r64(buf, (unsigned)JIT_REG_BASE);
  emit_pop_r64(buf, (unsigned)JIT_REG_VM);
  emit_ret(buf);
}

#define JIT_NOOP_BUF_CAP 256u

int jello_jit_compile_function(const jello_bc_module* m, uint32_t fi, jello_jit_code* out_code) {
  out_code->buf.mem = NULL;
  out_code->buf.cap = 0;
  out_code->buf.len = 0;
  out_code->buf.finalized = 0;
  out_code->entry_off = 0;
  out_code->body_off = 0;

  if(fi >= m->nfuncs) return 0;
  const jello_bc_function* f = &m->funcs[fi];

  jello_jit_ir_function* ir = jello_jit_ir_build(m, fi);
  if(!ir) return 0;
  int well_formed = jello_jit_ir_verify(ir, f);

  int shape_ok = well_formed && ir->ninsns == 1u && ir->insns[0].op == (uint16_t)JIR_RET;
  jello_type_kind ret_kind = JELLO_T_VOID;
  if(shape_ok) {
    ret_kind = vm_reg_kind(m, f, ir->insns[0].a);
    shape_ok = (ret_kind == JELLO_T_VOID);
  }
  jello_jit_ir_free(ir);
  if(!shape_ok) return 0;

  jit_exec_buf buf = jit_alloc_exec_buf(JIT_NOOP_BUF_CAP);
  if(!buf.mem) return 0;

  size_t body_off = 0;
  emit_prologue(&buf, &body_off);
  emit_epilogue_void(&buf, vm_pop_frame);

  if(!jit_finalize_exec_buf(&buf)) {
    jit_free_exec_buf(&buf);
    return 0;
  }

  out_code->buf = buf;
  out_code->entry_off = 0;
  out_code->body_off = body_off;
  return 1;
}

void jello_jit_code_free(jello_jit_code* code) {
  jit_free_exec_buf(&code->buf);
}
