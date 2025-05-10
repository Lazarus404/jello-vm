// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
#include <jello.h>
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* -- x86-64 byte encoding -- */

enum {
  X64_RAX = 0,
  X64_RCX = 1,
  X64_RDX = 2,
  X64_RSP = 4,
  X64_RBP = 5,
  X64_RSI = 6,
  X64_RDI = 7,
  X64_R8 = 8,
  X64_R9 = 9,
  X64_R10 = 10,
  X64_R11 = 11,
  X64_R14 = 14,
  X64_STAGE = X64_RBP,
};

typedef struct x64_insn {
  uint8_t bytes[16];
  size_t len;
} x64_insn;

static void x64_byte(x64_insn* i, uint8_t b) { i->bytes[i->len++] = b; }

static void x64_u32(x64_insn* i, uint32_t v) {
  memcpy(i->bytes + i->len, &v, sizeof v);
  i->len += sizeof v;
}

static void x64_u64(x64_insn* i, uint64_t v) {
  memcpy(i->bytes + i->len, &v, sizeof v);
  i->len += sizeof v;
}

static unsigned x64_byte_reg(unsigned reg) {
  // REX for r8b-r15b, and for spl/bpl/sil/dil, which are ah/ch/dh/bh without it
  return reg >= 4 ? X64_REX : 0;
}

static x64_insn x64_op(uint8_t prefix, unsigned flags, uint16_t opcode, unsigned reg, unsigned rm) {
  x64_insn i = {{0}, 0};
  unsigned rex = ((flags & X64_W) ? 0x08u : 0) | (reg >= 8 ? 0x04u : 0) | (rm >= 8 ? 0x01u : 0);
  if(prefix) x64_byte(&i, prefix);
  if(rex || (flags & X64_REX)) x64_byte(&i, (uint8_t)(0x40u | rex));
  if(opcode > 0xFFu) x64_byte(&i, (uint8_t)(opcode >> 8));
  x64_byte(&i, (uint8_t)opcode);
  return i;
}

static x64_insn x64_reg(uint8_t prefix, unsigned flags, uint16_t opcode, unsigned reg,
                        unsigned rm) {
  x64_insn i = x64_op(prefix, flags, opcode, reg, rm);
  x64_byte(&i, (uint8_t)(0xC0u | ((reg & 7u) << 3) | (rm & 7u)));
  return i;
}

static x64_insn x64_mem(uint8_t prefix, unsigned flags, uint16_t opcode, unsigned reg,
                        unsigned base, uint32_t disp) {
  x64_insn i = x64_op(prefix, flags, opcode, reg, base);
  x64_byte(&i, (uint8_t)(0x80u | ((reg & 7u) << 3) | (base & 7u)));
  if((base & 7u) == X64_RSP) x64_byte(&i, 0x24);
  x64_u32(&i, disp);
  return i;
}

static x64_insn x64_mem8(uint8_t prefix, unsigned flags, uint16_t opcode, unsigned reg,
                         unsigned base, uint8_t disp) {
  x64_insn i = x64_op(prefix, flags, opcode, reg, base);
  x64_byte(&i, (uint8_t)(0x40u | ((reg & 7u) << 3) | (base & 7u))); // mod=01, rm=RSP -> SIB
  if((base & 7u) == X64_RSP) x64_byte(&i, 0x24);                     // SIB: base=RSP, index=none
  x64_byte(&i, disp);
  return i;
}

static void x64_emit(jit_exec_buf* buf, x64_insn i) { jit_exec_buf_write(buf, i.bytes, i.len); }

static void x64_rr(jit_exec_buf* buf, uint8_t prefix, unsigned flags, uint16_t opcode,
                   unsigned reg, unsigned rm) {
  x64_emit(buf, x64_reg(prefix, flags, opcode, reg, rm));
}

static void x64_rm(jit_exec_buf* buf, uint8_t prefix, unsigned flags, uint16_t opcode,
                   unsigned reg, unsigned base, uint32_t disp) {
  x64_emit(buf, x64_mem(prefix, flags, opcode, reg, base, disp));
}

static void emit_push_r64(jit_exec_buf* buf, unsigned reg) {
  if(reg >= 8u) {
    uint8_t bytes[2] = { 0x41, (uint8_t)(0x50 + (reg & 7u)) };
    jit_exec_buf_write(buf, bytes, sizeof(bytes));
  } else {
    uint8_t byte = (uint8_t)(0x50 + reg);
    jit_exec_buf_write(buf, &byte, 1);
  }
}

static void emit_pop_r64(jit_exec_buf* buf, unsigned reg) {
  if(reg >= 8u) {
    uint8_t bytes[2] = { 0x41, (uint8_t)(0x58 + (reg & 7u)) };
    jit_exec_buf_write(buf, bytes, sizeof(bytes));
  } else {
    uint8_t byte = (uint8_t)(0x58 + reg);
    jit_exec_buf_write(buf, &byte, 1);
  }
}

static void emit_mov_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  bytes[0] = (uint8_t)(0x48 | ((dst >= 8) ? 0x01 : 0) /* REX.B: r/m field */
                            | ((src >= 8) ? 0x04 : 0) /* REX.R: reg field */);
  bytes[1] = 0x89;
  bytes[2] = (uint8_t)(0xC0 | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_mov_r32_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  size_t n = 0;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  uint8_t bytes[6];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = (uint8_t)(0xB8u + (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r64_imm64(jit_exec_buf* buf, unsigned dst, uint64_t imm) {
  uint8_t bytes[10];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = (uint8_t)(0xB8u + (dst & 7u));
  memcpy(&bytes[2], &imm, 8);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_mov_dword_rbx_disp_imm32(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  uint8_t bytes[10];
  bytes[0] = 0xC7;
  bytes[1] = 0x83;
  memcpy(&bytes[2], &disp, 4);
  memcpy(&bytes[6], &imm, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_mov_dword_rbx_disp_r32(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  uint8_t bytes[10];
  size_t n = 0;
  if(src >= 8) bytes[n++] = 0x44;
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x83u | ((src & 7u) << 3));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_qword_rbx_disp_r64(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0)); /* REX.W always */
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x83u | ((src & 7u) << 3));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_alu_r32_r32(jit_exec_buf* buf, uint8_t opcode, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  size_t n = 0;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = opcode;
  bytes[n++] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_alu_r64_r64(jit_exec_buf* buf, uint8_t opcode, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = opcode;
  bytes[2] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_alu_r32_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  uint8_t bytes[7];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x81;
  bytes[n++] = (uint8_t)(0xC0u | ((op_ext & 7u) << 3) | (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  x64_rr(buf, 0, X64_W, 0x89, src, dst);
}

static void emit_mov32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  x64_rr(buf, 0, 0, 0x89, src, dst);
}

static void emit_mov32_imm(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  x64_insn i = x64_op(0, 0, (uint16_t)(0xB8u + (dst & 7u)), 0, dst);
  x64_u32(&i, imm);
  x64_emit(buf, i);
}

static void emit_mov64_imm(jit_exec_buf* buf, unsigned dst, uint64_t imm) {
  x64_insn i = x64_op(0, X64_W, (uint16_t)(0xB8u + (dst & 7u)), 0, dst);
  x64_u64(&i, imm);
  x64_emit(buf, i);
}

static void emit_alu32(jit_exec_buf* buf, jit_alu op, unsigned dst, unsigned src) {
  x64_rr(buf, 0, 0, (uint16_t)((op << 3) | 0x01u), src, dst);
}

static void emit_alu64(jit_exec_buf* buf, jit_alu op, unsigned dst, unsigned src) {
  x64_rr(buf, 0, X64_W, (uint16_t)((op << 3) | 0x01u), src, dst);
}

static void emit_alu32_imm(jit_exec_buf* buf, jit_alu op, unsigned dst, uint32_t imm) {
  x64_insn i = x64_reg(0, 0, 0x81, op, dst);
  x64_u32(&i, imm);
  x64_emit(buf, i);
}

static void emit_alu_r32_imm8(jit_exec_buf* buf, jit_alu op, unsigned dst, uint8_t imm) {
  x64_insn i = x64_reg(0, 0, 0x83, op, dst);
  x64_byte(&i, imm);
  x64_emit(buf, i);
}

static void emit_neg_r32(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[3];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xF7;
  bytes[n++] = (uint8_t)(0xD8u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cmp_r32_r32(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[3];
  size_t n = 0;
  if((a >= 8) || (b >= 8)) bytes[n++] = (uint8_t)(0x40u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[n++] = 0x3B;
  bytes[n++] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cmp_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  uint8_t bytes[7];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x81;
  bytes[n++] = (uint8_t)(0xF8u | (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_neg32(jit_exec_buf* buf, unsigned dst) {
  x64_rr(buf, 0, 0, 0xF7, 3, dst);
}

static void emit_cmp32(jit_exec_buf* buf, unsigned a, unsigned b) {
  // cmp a32, b32 (3B /r): flags from a - b
  x64_rr(buf, 0, 0, 0x3B, a, b);
}

static void emit_cmp32_imm(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  int32_t v = (int32_t)imm;
  int short_imm = v >= -128 && v <= 127;
  x64_insn i = x64_reg(0, 0, short_imm ? 0x83 : 0x81, 7, dst);
  if(short_imm) x64_byte(&i, (uint8_t)imm);
  else x64_u32(&i, imm);
  x64_emit(buf, i);
}

static void emit_cmp_m32_r64_disp_imm8(jit_exec_buf* buf, unsigned base, uint32_t disp,
                                       int8_t imm) {
  x64_insn i = disp <= 127u ? x64_mem8(0, 0, 0x83, 7, base, (uint8_t)disp)
                            : x64_mem(0, 0, 0x83, 7, base, disp);
  x64_byte(&i, (uint8_t)imm);
  x64_emit(buf, i);
}

static void emit_setcc(jit_exec_buf* buf, uint8_t cc_opcode, unsigned dst) {
  uint8_t bytes[4];
  size_t n = 0;
  if(dst >= 4) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = cc_opcode;
  bytes[n++] = (uint8_t)(0xC0u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movzx_r32_r8(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  size_t n = 0;
  if(dst >= 8 || src >= 4) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xB6;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_setcc_from_flags(jit_exec_buf* buf, uint8_t cc_opcode, unsigned dst) {
  emit_setcc(buf, cc_opcode, dst);
  emit_movzx_r32_r8(buf, dst, dst);
}

static void emit_cdq(jit_exec_buf* buf) {
  uint8_t b = 0x99;
  jit_exec_buf_write(buf, &b, 1);
}

static void emit_cqo(jit_exec_buf* buf) {
  uint8_t bytes[2] = { 0x48, 0x99 };
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_idiv_r32(jit_exec_buf* buf, unsigned divisor) {
  uint8_t bytes[3];
  size_t n = 0;
  if(divisor >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xF7;
  bytes[n++] = (uint8_t)(0xF8u | (divisor & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_idiv_r64(jit_exec_buf* buf, unsigned divisor) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((divisor >= 8) ? 0x01u : 0));
  bytes[1] = 0xF7;
  bytes[2] = (uint8_t)(0xF8u | (divisor & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_shl_cl_r32(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[3];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xD3;
  bytes[n++] = (uint8_t)(0xE0u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_shr_cl_r32(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[3];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xD3;
  bytes[n++] = (uint8_t)(0xF8u | (dst & 7u)); /* SAR (signed), matching interpreter >> */
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_shl_cl_r64(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0xD3;
  bytes[2] = (uint8_t)(0xE0u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_shr_cl_r64(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0xD3;
  bytes[2] = (uint8_t)(0xF8u | (dst & 7u)); /* SAR */
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_test_r32_r32(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[3];
  size_t n = 0;
  if((a >= 8) || (b >= 8)) bytes[n++] = (uint8_t)(0x40u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[n++] = 0x85;
  bytes[n++] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_test32(jit_exec_buf* buf, unsigned a, unsigned b) {
  x64_rr(buf, 0, 0, 0x85, a, b);
}

static size_t emit_jmp_rel32(jit_exec_buf* buf, int32_t rel) {
  size_t at = buf->len;
  uint8_t bytes[5];
  bytes[0] = 0xE9;
  memcpy(&bytes[1], &rel, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
  return at;
}

static size_t emit_jcc_rel32(jit_exec_buf* buf, uint8_t cc, int32_t rel) {
  size_t at = buf->len;
  uint8_t bytes[6];
  bytes[0] = 0x0F;
  bytes[1] = (uint8_t)(0x80u | (cc & 0xFu));
  memcpy(&bytes[2], &rel, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
  return at;
}

static void patch_jcc_rel32(jit_exec_buf* buf, size_t at, int32_t rel) {
  memcpy(buf->mem + at + 2, &rel, 4);
}

static void patch_jmp_rel32(jit_exec_buf* buf, size_t at, int32_t rel) {
  memcpy(buf->mem + at + 1, &rel, 4);
}

static void emit_mov_eax_imm32(jit_exec_buf* buf, uint32_t imm) {
  uint8_t bytes[5];
  bytes[0] = 0xB8;
  memcpy(&bytes[1], &imm, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_neg_r64(jit_exec_buf* buf, unsigned dst) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0xF7;
  bytes[2] = (uint8_t)(0xD8u | (dst & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_movsxd_r64_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[1] = 0x63;
  bytes[2] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_cmp_r64_r64(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[1] = 0x3B;
  bytes[2] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_xor_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_alu_r32_imm32(buf, 0x06, dst, imm);
}

static void emit_movss_xmm_xmm(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_binss_xmm_xmm(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = op;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movsd_xmm_xmm(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_binsd_xmm_xmm(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = op;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_ucomiss(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[5];
  size_t n = 0;
  if((a >= 8) || (b >= 8)) bytes[n++] = (uint8_t)(0x40u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2E;
  bytes[n++] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_ucomisd(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0x66;
  if((a >= 8) || (b >= 8)) bytes[n++] = (uint8_t)(0x40u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2E;
  bytes[n++] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvtsi2ss(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if(wide || xmm >= 8 || gpr >= 8)
    bytes[n++] = (uint8_t)(0x40u | (wide ? 0x08u : 0) | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2A;
  bytes[n++] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvtsi2sd(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if(wide || xmm >= 8 || gpr >= 8)
    bytes[n++] = (uint8_t)(0x40u | (wide ? 0x08u : 0) | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2A;
  bytes[n++] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvttss2si(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if(wide || gpr >= 8 || xmm >= 8)
    bytes[n++] = (uint8_t)(0x40u | (wide ? 0x08u : 0) | ((gpr >= 8) ? 0x04u : 0) | ((xmm >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2C;
  bytes[n++] = (uint8_t)(0xC0u | ((gpr & 7u) << 3) | (xmm & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvttsd2si(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if(wide || gpr >= 8 || xmm >= 8)
    bytes[n++] = (uint8_t)(0x40u | (wide ? 0x08u : 0) | ((gpr >= 8) ? 0x04u : 0) | ((xmm >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x2C;
  bytes[n++] = (uint8_t)(0xC0u | ((gpr & 7u) << 3) | (xmm & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvtss2sd(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x5A;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_cvtsd2ss(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x5A;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movabs_rax(jit_exec_buf* buf, uint64_t imm) {
  uint8_t bytes[10];
  bytes[0] = 0x48;
  bytes[1] = 0xB8;
  memcpy(&bytes[2], &imm, 8);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

static void emit_call_rax(jit_exec_buf* buf) {
  uint8_t bytes[2] = { 0xFF, 0xD0 };
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
}

void jit_patch_jump(jit_exec_buf* buf, size_t at, size_t target) {
  size_t rel_at = buf->mem[at] == 0x0F ? at + 2u : at + 1u;
  int32_t rel = (int32_t)(target - (rel_at + 4u));
  memcpy(buf->mem + rel_at, &rel, 4);
}

static size_t emit_jcc_rel8(jit_exec_buf* buf, uint8_t cc) {
  size_t at = buf->len;
  uint8_t bytes[2] = {(uint8_t)(0x70u | (cc & 0xFu)), 0};
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
  return at;
}

static void patch_jcc_rel8_here(jit_exec_buf* buf, size_t at) {
  size_t rel = buf->len - (at + 2);
  assert(rel <= 127u);
  buf->mem[at + 1] = (uint8_t)rel;
}

static void patch_jump_here(jit_exec_buf* buf, size_t at) { jit_patch_jump(buf, at, buf->len); }

static void emit_neg64(jit_exec_buf* buf, unsigned dst) {
  x64_rr(buf, 0, X64_W, 0xF7, 3, dst);
}

static void emit_cmp64(jit_exec_buf* buf, unsigned a, unsigned b) {
  // cmp a64, b64 (REX.W 3B /r): flags from a - b
  x64_rr(buf, 0, X64_W, 0x3B, a, b);
}

static void emit_xor32_imm(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_alu32_imm(buf, JIT_ALU_XOR, dst, imm);
}

static void emit_fmov32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  x64_rr(buf, 0xF3, 0, 0x0F10, dst, src);
}

static void emit_fbin32(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  x64_rr(buf, 0xF3, 0, (uint16_t)(0x0F00u | op), dst, src);
}

static void emit_fcmp64(jit_exec_buf* buf, unsigned a, unsigned b) {
  x64_rr(buf, 0x66, 0, 0x0F2E, a, b);
}

static void emit_int_to_f32(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  x64_rr(buf, 0xF3, wide ? X64_W : 0, 0x0F2A, xmm, gpr);
}

static void emit_f32_to_f64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  x64_rr(buf, 0xF3, 0, 0x0F5A, dst, src);
}

static void emit_f64_to_f32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  x64_rr(buf, 0xF2, 0, 0x0F5A, dst, src);
}

static void emit_ff_r64(jit_exec_buf* buf, uint8_t ext, unsigned reg) {
  x64_rr(buf, 0, 0, 0xFF, ext, reg);
}

static void emit_call_r64(jit_exec_buf* buf, unsigned reg) { emit_ff_r64(buf, 2, reg); }

static void emit_jmp_r64(jit_exec_buf* buf, unsigned reg) { emit_ff_r64(buf, 4, reg); }

static void emit_ret(jit_exec_buf* buf) {
  uint8_t byte = 0xC3;
  jit_exec_buf_write(buf, &byte, 1);
}

static uint64_t fn_addr_bits_fn(void* fn) {
  uint64_t bits;
  memcpy(&bits, &fn, sizeof(bits));
  return bits;
}
static void emit_imul_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src);
static void emit_imul_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm);


static void emit_call_fn(jit_exec_buf* buf, uintptr_t fn) {
  emit_mov64_imm(buf, X64_RAX, (uint64_t)fn);
  emit_call_r64(buf, X64_RAX);
#ifdef JELLO_JIT_SCRIBBLE_CALLS
  static const unsigned scribbled[] = {X64_RCX, X64_RDX, X64_RSI, X64_RDI,
                                       X64_R8,  X64_R9,  X64_R10, X64_R11};
  for(size_t i = 0; i < sizeof scribbled / sizeof scribbled[0]; i++)
    emit_mov64_imm(buf, scribbled[i], 0x5c5c5c5c5c5c5c5cull);
#endif
}

static size_t emit_call_rel32(jit_exec_buf* buf, int32_t rel) {
  size_t at = buf->len;
  uint8_t bytes[5];
  bytes[0] = 0xE8;
  memcpy(&bytes[1], &rel, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
  return at;
}

static void emit_mov_r64_from_r12_disp(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x49u | ((dst >= 8) ? 0x04u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | 0x04u);
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r32_from_r12_disp(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x41u | ((dst >= 8) ? 0x04u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | 0x04u);
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r32_to_r12_disp(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x41u | ((src >= 8) ? 0x04u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | 0x04u);
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r64_to_r12_disp(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x49u | ((src >= 8) ? 0x04u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | 0x04u);
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_imm32_to_vm_field(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  uint8_t bytes[12];
  size_t n = 0;
  bytes[n++] = 0x41;
  bytes[n++] = 0xC7;
  bytes[n++] = 0x84;
  bytes[n++] = 0x24;
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r64_from_r64_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((dst >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r32_from_r64_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  if((dst >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r32_to_r64_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  if((src >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r64_to_r64_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r32_to_r13_disp(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x41u | ((src >= 8) ? 0x04u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r64_to_r13_disp(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_add_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[1] = 0x01;
  bytes[2] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_add_r32_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  size_t n = 0;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x01;
  bytes[n++] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_load_vm64(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  x64_rm(buf, 0, X64_W, 0x8B, dst, (unsigned)JIT_REG_VM, disp);
}

static void emit_load_vm32(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  x64_rm(buf, 0, 0, 0x8B, dst, (unsigned)JIT_REG_VM, disp);
}

static void emit_cmp_vm(jit_exec_buf* buf, unsigned a, uint32_t disp, int wide) {
  x64_rm(buf, 0, wide ? X64_W : 0, 0x3B, a, (unsigned)JIT_REG_VM, disp);
}

static void emit_sub_vm32_imm8(jit_exec_buf* buf, uint32_t disp, int8_t imm) {
  x64_insn i = x64_mem(0, 0, 0x83, 5, (unsigned)JIT_REG_VM, disp);
  x64_byte(&i, (uint8_t)imm);
  x64_emit(buf, i);
}

static void emit_store_vm32(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  x64_rm(buf, 0, 0, 0x89, src, (unsigned)JIT_REG_VM, disp);
}

static void emit_store_vm32_imm(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  x64_insn i = x64_mem(0, 0, 0xC7, 0, (unsigned)JIT_REG_VM, disp);
  x64_u32(&i, imm);
  x64_emit(buf, i);
}

static void emit_load64(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  x64_rm(buf, 0, X64_W, 0x8B, dst, base, disp);
}

static void emit_load32(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  x64_rm(buf, 0, 0, 0x8B, dst, base, disp);
}

static void emit_store32(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  x64_rm(buf, 0, 0, 0x89, src, base, disp);
}

static void emit_store64(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  x64_rm(buf, 0, X64_W, 0x89, src, base, disp);
}

static void emit_add64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_alu64(buf, JIT_ALU_ADD, dst, src);
}

static void emit_jmp_stack_overflow(emit_ctx* ctx) {
  emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
  emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  size_t at = emit_jmp_rel32(ctx->buf, 0);
  if(ctx->stack_overflow_jump_count < ctx->stack_overflow_jump_cap)
    ctx->stack_overflow_jumps[ctx->stack_overflow_jump_count++] = at;
}

static size_t emit_branch_if_native_room(jit_exec_buf* buf) {
  emit_cmp_vm(buf, X64_RSP, (uint32_t)offsetof(jello_vm, native_stack_limit), 1);
  return emit_jcc_rel32(buf, JIT_JAE, 0); // JAE: at or above the limit
}

static void emit_native_stack_check(emit_ctx* ctx) {
  size_t fits = emit_branch_if_native_room(ctx->buf);
  emit_jmp_stack_overflow(ctx);
  patch_jump_here(ctx->buf, fits);
}

static void emit_bridge_when_deep(emit_ctx* ctx, unsigned target) {
  jit_exec_buf* buf = ctx->buf;
  size_t fits = emit_branch_if_native_room(buf);
  emit_mov64_imm(buf, target, (uint64_t)(uintptr_t)&ctx->entries[ctx->m->nfuncs]);
  emit_load64(buf, target, target, 0);
  patch_jump_here(buf, fits);
}

static void emit_jcc_trap(emit_ctx* ctx, uint8_t cc, uint8_t kind) {
  size_t skip = emit_jcc_rel32(ctx->buf, (uint8_t)(cc ^ 1u), 0);
  if(kind == JIT_TRAP_CALLEE) {
    emit_store_vm32_imm(ctx->buf, JIT_TRAP_SITE_OFF, ctx->cur_bc_pc + 1u);
  } else if(kind != JIT_TRAP_PASSTHROUGH) {
    emit_store_vm32_imm(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
    emit_store_vm32_imm(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  }
  size_t at = emit_jmp_rel32(ctx->buf, 0);
  patch_jcc_rel32(ctx->buf, skip, (int32_t)(ctx->buf->len - (skip + 6)));
  if(ctx->trap_jump_count < ctx->trap_jump_cap) {
    ctx->trap_jumps[ctx->trap_jump_count].at = at;
    ctx->trap_jumps[ctx->trap_jump_count].kind = kind;
    ctx->trap_jump_count++;
  }
}

static int resolve_target_bc(uint32_t bc_pc, int32_t imm, uint32_t ninsns, uint32_t* out) {
  int64_t t = (int64_t)(int32_t)bc_pc + 1 + (int64_t)imm;
  if(t < 0 || t >= (int64_t)ninsns) return 0;
  *out = (uint32_t)t;
  return 1;
}

static void emit_divmod_guard_i32(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_test32(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_ARITH);
  emit_cmp32_imm(ctx->buf, div_gpr, (uint32_t)-1);
  size_t jne = ctx->buf->len;
  emit_jcc_rel32(ctx->buf, JIT_JNE, 0);
  emit_cmp32_imm(ctx->buf, lhs_gpr, (uint32_t)INT32_MIN);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  patch_jcc_rel32(ctx->buf, jne, (int32_t)(ok - (jne + 6)));
}

static void emit_test_r64_r64(jit_exec_buf* buf, unsigned a, unsigned b) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[1] = 0x85;
  bytes[2] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_test64(jit_exec_buf* buf, unsigned a, unsigned b) {
  x64_rr(buf, 0, X64_W, 0x85, a, b);
}

static void emit_cmp_r64_imm8(jit_exec_buf* buf, unsigned reg, int8_t imm) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((reg >= 8) ? 0x01u : 0));
  bytes[1] = 0x83;
  bytes[2] = (uint8_t)(0xF8u | (reg & 7u));
  bytes[3] = (uint8_t)imm;
  jit_exec_buf_write(buf, bytes, 4);
}

static void emit_divmod_guard_i64(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_test64(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_ARITH);
  emit_cmp_r64_imm8(ctx->buf, div_gpr, (int8_t)-1);
  size_t jne = ctx->buf->len;
  emit_jcc_rel32(ctx->buf, JIT_JNE, 0);
  // INT64_MIN needs a register operand; RAX is reloaded from lhs for the idiv anyway
  emit_mov64_imm(ctx->buf, X64_RAX, (uint64_t)INT64_MIN);
  emit_cmp64(ctx->buf, lhs_gpr, X64_RAX);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  patch_jcc_rel32(ctx->buf, jne, (int32_t)(ok - (jne + 6)));
}

static int jir_op_allowed(jello_jit_ir_op op, int allow_calls) {
  switch(op) {
    case JIR_NOP: case JIR_RET: case JIR_MOV:
    case JIR_JMP: case JIR_JMP_IF: case JIR_SAFEPOINT:
    case JIR_CONST_I8_IMM: case JIR_CONST_I32: case JIR_CONST_I64:
    case JIR_CONST_F16: case JIR_CONST_F32: case JIR_CONST_F64: case JIR_CONST_BOOL:
    case JIR_BIN_I32: case JIR_BIN_I64: case JIR_BIN_F16: case JIR_BIN_F32: case JIR_BIN_F64:
    case JIR_ADD_I32_IMM: case JIR_SUB_I32_IMM: case JIR_MUL_I32_IMM:
    case JIR_NEG_I32: case JIR_NEG_I64: case JIR_NEG_F32: case JIR_NEG_F64: case JIR_NOT_BOOL:
    case JIR_CMP_I32: case JIR_CMP_I64: case JIR_CMP_F32: case JIR_CMP_F64:
    case JIR_EQ_I32_IMM: case JIR_LT_I32_IMM: case JIR_CONV:
    case JIR_BYTES_NEW: case JIR_BYTES_LEN: case JIR_BYTES_GET_U8: case JIR_BYTES_SET_U8:
    case JIR_BYTES_EQ: case JIR_BYTES_CONCAT2: case JIR_BYTES_BITOP: case JIR_BYTES_READ:
    case JIR_BYTES_WRITE: case JIR_BYTES_BLIT: case JIR_BITS_WRITE: case JIR_BITS_READ:
    case JIR_STRUCT_NEW: case JIR_STRUCT_GET: case JIR_STRUCT_SET:
    case JIR_CONST_NULL: case JIR_CONST_ATOM:
    case JIR_TO_DYN: case JIR_FROM_DYN: case JIR_KINDOF: case JIR_PHYSEQ:
    case JIR_OBJ_NEW: case JIR_OBJ_HAS_ATOM: case JIR_OBJ_GET_ATOM: case JIR_OBJ_SET_ATOM:
    case JIR_OBJ_GET: case JIR_OBJ_SET: case JIR_OBJ_HAS: case JIR_OBJ_REMOVE:
    case JIR_OBJ_CLEAR: case JIR_OBJ_COPY:
      return 1;
    case JIR_CALL_SELF:
    case JIR_CALL_DIRECT:
      return allow_calls;
    default:
      return 0;
  }
}

static int ir_shape_ok(const jello_jit_ir_function* ir, const jello_bc_module* m,
                       const jello_bc_function* f, uint32_t fi, jello_type_kind* out_ret_kind,
                       int allow_calls) {
  if(ir->nslow > 0) return 0;
  int saw_ret = 0;
  jello_type_kind ret_kind = JELLO_T_VOID;
  for(uint32_t i = 0; i < ir->ninsns; i++) {
    jello_jit_ir_op op = (jello_jit_ir_op)ir->insns[i].op;
    if(!jir_op_allowed(op, allow_calls)) return 0;
    if(op == JIR_CALL_SELF || op == JIR_CALL_DIRECT) {
      uint32_t callee = (op == JIR_CALL_SELF) ? fi : ir->insns[i].imm;
      if(!allow_calls) return 0;
      if(callee >= m->nfuncs) return 0;
    }
    if(op == JIR_JMP || op == JIR_JMP_IF) {
      uint32_t tgt = 0;
      if(!resolve_target_bc(ir->insns[i].bc_pc, (int32_t)ir->insns[i].imm, f->ninsns, &tgt))
        return 0;
    }
    if(op == JIR_RET) {
      jello_type_kind k = vm_reg_kind(m, f, ir->insns[i].a);
      if(k == JELLO_T_F16) return 0;
      if(saw_ret && k != ret_kind) return 0;
      ret_kind = k;
      saw_ret = 1;
    }
  }
  if(!saw_ret) return 0;
  *out_ret_kind = ret_kind;
  return 1;
}

static void emit_rsp_adjust_local(jit_exec_buf* buf, int is_add, uint8_t imm8);
static void emit_store_r64_to_mem_rsp(jit_exec_buf* buf, unsigned src, uint8_t disp);
static void emit_mov_r64_from_mem_rsp(jit_exec_buf* buf, unsigned dst, uint8_t disp);


static void emit_sp_adjust(jit_exec_buf* buf, int is_add, uint8_t imm8) {
  x64_insn i = x64_reg(0, X64_W, 0x83, is_add ? 0u : 5u, X64_RSP);
  x64_byte(&i, imm8);
  x64_emit(buf, i);
}

static void emit_store64_sp(jit_exec_buf* buf, unsigned src, uint8_t disp) {
  x64_emit(buf, x64_mem8(0, X64_W, 0x89, src, X64_RSP, disp));
}

void jit_emit_prologue(jit_exec_buf* buf, size_t* out_body_off, int any_armed) {
  emit_push_r64(buf, (unsigned)JIT_REG_VM);
  emit_push_r64(buf, (unsigned)JIT_REG_BASE);
  emit_push_r64(buf, (unsigned)JIT_REG_OUT);
  emit_push_r64(buf, X64_R14);
  emit_push_r64(buf, X64_STAGE);
  emit_mov64(buf, (unsigned)JIT_REG_VM, X64_RDI);
  emit_mov64(buf, (unsigned)JIT_REG_BASE, X64_RSI);
  emit_mov64(buf, (unsigned)JIT_REG_OUT, X64_RDX);
  *out_body_off = buf->len;
  if(any_armed) {
    emit_sp_adjust(buf, 0, 16); // sub rsp, 16
    emit_store64_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    // Every C helper call needs RSP 16-byte aligned; a misaligned one stops here, at a UD2.
    static const uint8_t check[] = {0x48, 0xF7, 0xC4, 0x0F, 0x00, 0x00, 0x00, // test rsp, 0xf
                                    0x74, 0x02,                               // je +2
                                    0x0F, 0x0B};                              // ud2
    jit_exec_buf_write(buf, check, sizeof(check));
  }
}

static void emit_load64_sp(jit_exec_buf* buf, unsigned dst, uint8_t disp) {
  x64_emit(buf, x64_mem8(0, X64_W, 0x8B, dst, X64_RSP, disp));
}

static void emit_restore_callee_saved(jit_exec_buf* buf, int any_armed) {
  if(any_armed) {
    emit_mov_r64_from_mem_rsp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_rsp_adjust_local(buf, 1, 16); /* add rsp, 16 */
  }
  emit_pop_r64(buf, (unsigned)JIT_REG_OUT);
  emit_pop_r64(buf, (unsigned)JIT_REG_BASE);
  emit_pop_r64(buf, (unsigned)JIT_REG_VM);
}

static void emit_store_out(emit_ctx* ctx, jello_type_kind ret_kind, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_VOID);
  if(ret_kind == JELLO_T_BOOL) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_BOOL);
    emit_mov_dword_rbx_disp_r32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_I32);
    emit_mov_dword_rbx_disp_r32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_I64);
    emit_mov_qword_rbx_disp_r64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_F32);
    emit_mov_dword_rbx_disp_r32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_F64);
    emit_mov_qword_rbx_disp_r64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_BYTES || ret_kind == JELLO_T_STRUCT || ret_kind == JELLO_T_OBJECT) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_PTR);
    emit_mov_qword_rbx_disp_r64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_DYNAMIC) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_mov_dword_rbx_disp_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_DYN);
    emit_mov_qword_rbx_disp_r64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  }
}

static void emit_movzx_r32_m8_r64_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  if((dst >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xB6;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movzx_r32_m16_r64_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  if((dst >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xB7;
  bytes[n++] = (uint8_t)(0x80u | ((dst & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r8_to_r64_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  uint8_t bytes[7];
  size_t n = 0;
  if((src >= 4) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x88;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r16_to_r64_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = 0x66;
  if((src >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_bswap_r32(jit_exec_buf* buf, unsigned reg) {
  uint8_t bytes[3];
  size_t n = 0;
  if(reg >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x0F;
  bytes[n++] = (uint8_t)(0xC8u | (reg & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_bswap_r64(jit_exec_buf* buf, unsigned reg) {
  uint8_t bytes[3];
  bytes[0] = (uint8_t)(0x48u | ((reg >= 8) ? 0x01u : 0));
  bytes[1] = 0x0F;
  bytes[2] = (uint8_t)(0xC8u | (reg & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_rol16_imm8(jit_exec_buf* buf, unsigned reg, uint8_t imm8) {
  uint8_t bytes[5];
  size_t n = 0;
  bytes[n++] = 0x66;
  if(reg >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0xC1;
  bytes[n++] = (uint8_t)(0xC0u | (reg & 7u));
  bytes[n++] = imm8;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movd_xmm_r32(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0x66;
  if((xmm >= 8) || (gpr >= 8)) bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x6E;
  bytes[n++] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movd_r32_xmm(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  uint8_t bytes[6];
  size_t n = 0;
  bytes[n++] = 0x66;
  if((xmm >= 8) || (gpr >= 8)) bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x7E;
  bytes[n++] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movq_xmm_r64(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  uint8_t bytes[6];
  bytes[0] = 0x66;
  bytes[1] = (uint8_t)(0x48u | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[2] = 0x0F;
  bytes[3] = 0x6E;
  bytes[4] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, 5);
}

static void emit_movq_r64_xmm(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  uint8_t bytes[6];
  bytes[0] = 0x66;
  bytes[1] = (uint8_t)(0x48u | ((xmm >= 8) ? 0x04u : 0) | ((gpr >= 8) ? 0x01u : 0));
  bytes[2] = 0x0F;
  bytes[3] = 0x7E;
  bytes[4] = (uint8_t)(0xC0u | ((xmm & 7u) << 3) | (gpr & 7u));
  jit_exec_buf_write(buf, bytes, 5);
}

static void emit_sub_r32_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[3];
  size_t n = 0;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((src >= 8) ? 0x04u : 0) | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x29;
  bytes[n++] = (uint8_t)(0xC0u | ((src & 7u) << 3) | (dst & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_reload_jit_reg_base_x64(jit_exec_buf* buf) {
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_R8, X64_RAX);
  emit_imul_r64_r64(buf, X64_R8, X64_RDX);
  emit_add_r64_r64(buf, X64_R8, X64_RCX);
  emit_mov_r64_from_r64_disp(buf, (unsigned)JIT_REG_BASE, X64_R8,
      (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));
}

void jit_emit_ok_exit(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_pop_frame));
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov32_imm(buf, X64_RAX, (uint32_t)JELLO_EXEC_OK);
  emit_ret(buf);
}


static void emit_sub32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_alu32(buf, JIT_ALU_SUB, dst, src);
}

static void emit_mul64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  x64_rr(buf, 0, X64_W, 0x0FAF, dst, src);
}

static void emit_load8(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  x64_rm(buf, 0, 0, 0x0FB6, dst, base, disp);
}

static void emit_top_call_frame_into_r8(jit_exec_buf* buf) {
  emit_load_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_load_vm64(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov32_imm(buf, X64_RDX, 1u);
  emit_sub32(buf, X64_RAX, X64_RDX);
  emit_mov64_imm(buf, X64_RDX, sizeof(call_frame));
  emit_mov64(buf, X64_R8, X64_RAX);
  emit_mul64(buf, X64_R8, X64_RDX);
  emit_add64(buf, X64_R8, X64_RCX);
}

static size_t emit_jump_if_no_caller_r8(jit_exec_buf* buf) {
  emit_top_call_frame_into_r8(buf);
  emit_load8(buf, X64_RDX, X64_R8, (uint32_t)offsetof(call_frame, has_caller));
  emit_test32(buf, X64_RDX, X64_RDX);
  return emit_jcc_rel32(buf, JIT_JE, 0);
}

static void emit_reload_jit_reg_base(jit_exec_buf* buf) {
  emit_top_call_frame_into_r8(buf);
  emit_load64(buf, (unsigned)JIT_REG_BASE, X64_R8,
              (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));
}

static void emit_trap_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  size_t j_boundary = emit_jump_if_no_caller_r8(buf);

  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_pop_frame);
  emit_reload_jit_reg_base(buf);
  emit_mov32_imm(buf, X64_RAX, (uint32_t)JELLO_EXEC_TRAP);
  // Undo this activation's R15 save, or `ret` pops it as the return address.
  if(ctx->survive_any_armed) {
    emit_load64_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_ret(buf);

  size_t boundary = buf->len;
  jit_patch_jump(buf, j_boundary, boundary);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_pop_frame);
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov32_imm(buf, X64_RAX, (uint32_t)JELLO_EXEC_TRAP);
  emit_ret(buf);
}

void jit_emit_trap_exit(emit_ctx* ctx, uintptr_t trap_fn) {
  jit_exec_buf* buf = ctx->buf;
  if(trap_fn) {
    emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, trap_fn);
  }
  emit_trap_tail(ctx);
}

static void emit_load16(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  x64_rm(buf, 0, 0, 0x0FB7, dst, base, disp);
}

static void emit_store8(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  x64_rm(buf, 0, x64_byte_reg(src), 0x88, src, base, disp);
}

static void emit_store_imm8_to_r64_disp(jit_exec_buf* buf, unsigned base, uint32_t disp,
                                        uint8_t imm) {
  x64_insn i = x64_mem(0, 0, 0xC6, 0, base, disp);
  x64_byte(&i, imm);
  x64_emit(buf, i);
}

static void emit_bswap32(jit_exec_buf* buf, unsigned reg) {
  x64_emit(buf, x64_op(0, 0, (uint16_t)(0x0FC8u + (reg & 7u)), 0, reg));
}

static void emit_bswap64(jit_exec_buf* buf, unsigned reg) {
  x64_emit(buf, x64_op(0, X64_W, (uint16_t)(0x0FC8u + (reg & 7u)), 0, reg));
}

static void emit_bswap16(jit_exec_buf* buf, unsigned reg, uint8_t imm8) {
  x64_insn i = x64_reg(0x66, 0, 0xC1, 0, reg);
  x64_byte(&i, imm8);
  x64_emit(buf, i);
}

static void emit_fmov_from64(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  x64_rr(buf, 0x66, X64_W, 0x0F6E, xmm, gpr);
}

static void emit_mul32_imm(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  // imul dst32, dst32, imm32 (69 /r): dst is in both reg and rm, so r8-r15 need REX.R and REX.B
  x64_insn i = x64_reg(0, 0, 0x69, dst, dst);
  x64_u32(&i, imm);
  x64_emit(buf, i);
}

static void emit_alu64_imm(jit_exec_buf* buf, jit_alu op, unsigned dst, uint32_t imm) {
  x64_insn i = x64_reg(0, X64_W, 0x81, op, dst);
  x64_u32(&i, imm);
  x64_emit(buf, i);
}

static void emit_ret_insn(emit_ctx* ctx, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind rk = ctx->ret_kind;
  int wide = (rk == JELLO_T_I64 || rk == JELLO_T_F64);
  int ret_gpr = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);

  if(wide) emit_mov64(buf, X64_R9, (unsigned)ret_gpr);
  else emit_mov32(buf, X64_R9, (unsigned)ret_gpr);
  jit_reg_bind(ctx->ra, ret_vreg, X64_R9);
  ret_gpr = X64_R9;

  size_t j_boundary = emit_jump_if_no_caller_r8(buf);

  emit_load32(buf, X64_R10, X64_R8, (uint32_t)offsetof(call_frame, caller_dst));

  if(wide) emit_mov64(buf, X64_R11, (unsigned)ret_gpr);
  else emit_mov32(buf, X64_R11, (unsigned)ret_gpr);

  emit_load32(buf, X64_RAX, X64_R8, (uint32_t)offsetof(call_frame, pop_work));
  emit_test32(buf, X64_RAX, X64_RAX);
  size_t j_try = emit_jcc_rel32(buf, JIT_JNE, 0);
  emit_load64(buf, X64_RAX, X64_R8, (uint32_t)offsetof(call_frame, spill_slots));
  emit_test64(buf, X64_RAX, X64_RAX);
  size_t j_spill = emit_jcc_rel32(buf, JIT_JNE, 0);
  emit_load32(buf, X64_RCX, X64_R8,
              (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, size));
  emit_load_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_sub32(buf, X64_RAX, X64_RCX);
  emit_store_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_load_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_alu32_imm(buf, JIT_ALU_SUB, X64_RAX, 1u); // sub eax, 1
  emit_store_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_alu64_imm(buf, JIT_ALU_SUB, X64_R8, (uint32_t)sizeof(call_frame)); // sub r8, imm32
  emit_load64(buf, (unsigned)JIT_REG_BASE, X64_R8,
              (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));
  size_t j_popped = emit_jmp_rel32(buf, 0);
  patch_jump_here(buf, j_try);
  patch_jump_here(buf, j_spill);

  emit_push_r64(buf, X64_R10);
  emit_push_r64(buf, X64_R11);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_pop_frame));
  emit_pop_r64(buf, X64_R11);
  emit_pop_r64(buf, X64_R10);

  emit_reload_jit_reg_base(buf);
  patch_jump_here(buf, j_popped);
  emit_mul32_imm(buf, X64_R10, REG_SLOT_BYTES);
  emit_mov64(buf, X64_R8, (unsigned)JIT_REG_BASE);
  emit_add64(buf, X64_R8, X64_R10);
  if(wide) emit_store64(buf, X64_R11, X64_R8, 0);
  else emit_store32(buf, X64_R11, X64_R8, 0);

  if(ctx->survive_any_armed) {
    emit_load64_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16); // add rsp, 16
  }
  emit_ret(buf);

  size_t boundary = buf->len;
  jit_patch_jump(buf, j_boundary, boundary);

  emit_store_out(ctx, rk, ret_vreg);
  size_t at = emit_jmp_rel32(buf, 0);
  jit_push_ret_jump(ctx, at);
}

static void emit_rsp_adjust_local(jit_exec_buf* buf, int is_add, uint8_t imm8);

static void emit_call_jit_code(jit_exec_buf* buf, unsigned target) {
  emit_sp_adjust(buf, 0, 8);
  emit_call_r64(buf, target);
  emit_sp_adjust(buf, 1, 8);
}

static void emit_after_jit_call(emit_ctx* ctx, uint8_t dst) {
  emit_load_vm32(ctx->buf, X64_RAX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test32(ctx->buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_CALLEE);
  jit_reg_state_discard_currents(ctx->ra);
  (void)jit_reg_get_gpr(ctx->ra, dst, 1);
}

static void emit_fill_call_frame(jit_exec_buf* buf, const jello_bc_function* f, uint32_t fi,
                                 int has_pointer, uint32_t total, uint32_t caller_dst) {
  emit_load_vm32(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_load_vm64(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov32(buf, X64_R8, X64_RCX);
  emit_mov64_imm(buf, X64_RDX, sizeof(call_frame));
  emit_mul64(buf, X64_R8, X64_RDX);
  emit_add64(buf, X64_RAX, X64_R8);

  if(f) emit_mov64_imm(buf, X64_RDX, (uint64_t)(uintptr_t)f);
  else emit_load64(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, f));
  emit_store64(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, f));
  unsigned fi_reg = X64_R9;
  if(f) {
    emit_mov32_imm(buf, X64_RDX, fi);
    fi_reg = X64_RDX;
  }
  emit_store32(buf, fi_reg, X64_RAX, (uint32_t)offsetof(call_frame, func_index));
  emit_mov32_imm(buf, X64_RDX, 0u);
  emit_store32(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, pc));
  emit_store_imm8_to_r64_disp(buf, X64_RAX, (uint32_t)offsetof(call_frame, has_caller), 1u);
  if(f) {
    emit_store_imm8_to_r64_disp(buf, X64_RAX, (uint32_t)offsetof(call_frame, has_pointer),
                                (uint8_t)has_pointer);
  } else {
    emit_load32(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, has_pointer));
    emit_store8(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, has_pointer));
  }
  emit_mov32_imm(buf, X64_RDX, caller_dst);
  emit_store32(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, caller_dst));

  emit_load64(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, kind));
  emit_store64(buf, X64_RDX, X64_RAX,
               (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, kind));
  emit_load64(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, wide));
  emit_store64(buf, X64_RDX, X64_RAX,
               (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, wide));
  if(f) emit_mov32_imm(buf, X64_RDX, total);
  else emit_load32(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, total));
  emit_store32(buf, X64_RDX, X64_RAX,
               (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, size));
  emit_store64(buf, X64_R11, X64_RAX,
               (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));

  emit_alu32_imm(buf, JIT_ALU_ADD, X64_RCX, 1u); // add ecx, 1
  emit_store_vm32(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
}

static void emit_copy_args(emit_ctx* ctx, const jello_bc_function* kinds_f, uint32_t kinds_first,
                           uint32_t arg_first, uint32_t nargs) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov64(buf, X64_R8, (unsigned)JIT_REG_BASE);
  emit_mov64(buf, (unsigned)JIT_REG_BASE, X64_R11);
  for(uint32_t ai = 0; ai < nargs; ai++) {
    uint32_t src_disp = (arg_first + ai) * REG_SLOT_BYTES;
    uint32_t dst_disp = ai * REG_SLOT_BYTES;
    if(jello_slot_size(vm_reg_kind(ctx->m, kinds_f, kinds_first + ai)) == 8u) {
      emit_load64(buf, X64_RAX, X64_R8, src_disp);
      emit_store64(buf, X64_RAX, (unsigned)JIT_REG_BASE, dst_disp);
    } else {
      emit_load32(buf, X64_RAX, X64_R8, src_disp);
      emit_store32(buf, X64_RAX, (unsigned)JIT_REG_BASE, dst_disp);
    }
  }
}

static void emit_frame_tops(jit_exec_buf* buf, uint32_t total) {
  emit_load_vm32(buf, X64_RCX, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov32(buf, X64_R8, X64_RCX);
  emit_alu32_imm(buf, JIT_ALU_ADD, X64_R8, total); // add r8d, total
}

static int emit_call_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins, jello_jit_ir_op op) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t callee_fi = (op == JIR_CALL_SELF) ? ctx->fi : ins->imm;
  if(!ctx->entries) return 0;
  uint32_t caller_dst = ins->a;
  uint32_t arg_first = ins->b;
  uint32_t nargs = ins->c;
  const jello_bc_function* callee_f = &ctx->m->funcs[callee_fi];

  jit_reg_state_discard_currents(ctx->ra);

  emit_load_vm32(buf, X64_RCX, (uint32_t)offsetof(jello_vm, jit_layout_by_func_len));
  emit_cmp32_imm(buf, X64_RCX, callee_fi);
  size_t j_indexed = emit_jcc_rel32(buf, JIT_JA, 0); // JA: callee_fi < len
  emit_jmp_stack_overflow(ctx);
  patch_jump_here(buf, j_indexed);
  emit_load_vm64(buf, X64_STAGE, (uint32_t)offsetof(jello_vm, jit_layout_by_func));
  emit_load64(buf, X64_STAGE, X64_STAGE, callee_fi * (uint32_t)sizeof(frame_layout*));
  emit_test64(buf, X64_STAGE, X64_STAGE);
  size_t j_no_layout = emit_jcc_rel32(buf, JIT_JNE, 0);
  emit_jmp_stack_overflow(ctx);
  patch_jump_here(buf, j_no_layout);

  uint32_t total = callee_f->nregs * REG_SLOT_BYTES;
  int has_pointer = vm_func_has_pointer(ctx->m, callee_f);

  emit_load_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_cmp_vm(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_limit), 0);
  size_t j_frames = emit_jcc_rel32(buf, JIT_JB, 0); // JB: below the frame limit
  emit_jmp_stack_overflow(ctx);
  patch_jump_here(buf, j_frames);
  emit_frame_tops(buf, total);
  emit_cmp_vm(buf, X64_R8, (uint32_t)offsetof(jello_vm, frame_stack_limit), 0);
  size_t j_grow = emit_jcc_rel32(buf, JIT_JA, 0); // JA: past what is committed
  emit_cmp_vm(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_cap), 0);
  size_t j_room = emit_jcc_rel32(buf, JIT_JB, 0); // JB: a free slot
  patch_jump_here(buf, j_grow);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov32(buf, X64_RSI, X64_R8);
  emit_call_fn(buf, (uintptr_t)jit_try_grow_push);
  emit_test32(buf, X64_RAX, X64_RAX);
  size_t j_grown = emit_jcc_rel32(buf, JIT_JNE, 0);
  emit_jmp_stack_overflow(ctx);
  patch_jump_here(buf, j_grown);
  emit_frame_tops(buf, total);
  patch_jump_here(buf, j_room);

  emit_store_vm32(buf, X64_R8, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_load_vm64(buf, X64_R11, (uint32_t)offsetof(jello_vm, frame_stack));
  emit_add64(buf, X64_R11, X64_RCX);

  if(has_pointer) {
    emit_mov64(buf, X64_RDI, X64_R11);
    emit_load64(buf, X64_RSI, X64_STAGE, (uint32_t)offsetof(frame_layout, init));
    emit_mov32_imm(buf, X64_RDX, total);
    emit_call_fn(buf, (uintptr_t)JIT_C(memcpy));
    emit_mov64(buf, X64_R11, X64_RAX);
  }

  emit_fill_call_frame(buf, callee_f, callee_fi, has_pointer, total, caller_dst);
  emit_copy_args(ctx, callee_f, 0, arg_first, nargs);

  emit_mov64_imm(buf, X64_RAX, (uint64_t)(uintptr_t)&ctx->entries[callee_fi]);
  emit_load64(buf, X64_RAX, X64_RAX, 0);
  emit_bridge_when_deep(ctx, X64_RAX);
  emit_call_jit_code(buf, X64_RAX);
  emit_after_jit_call(ctx, ins->a);
  return 1;
}

static void emit_imul_r32_r32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[4];
  size_t n = 0;
  if((dst >= 8) || (src >= 8)) bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xAF;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_imul_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  bytes[0] = (uint8_t)(0x48u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[1] = 0x0F;
  bytes[2] = 0xAF;
  bytes[3] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, 4);
}

static void emit_imul_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  uint8_t bytes[8];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x45;
  bytes[n++] = 0x69;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_obj_kind_guard(jit_exec_buf* buf, unsigned reg, uint32_t kind, size_t* slow,
                                size_t* nslow) {
  emit_mov32(buf, X64_RAX, reg);
  emit_alu32_imm(buf, JIT_ALU_AND, X64_RAX, 7u);         // and eax, 7
  slow[(*nslow)++] = emit_jcc_rel32(buf, JIT_JNE, 0); // not a pointer
  emit_test64(buf, reg, reg);
  slow[(*nslow)++] = emit_jcc_rel32(buf, JIT_JE, 0); // NULL
  emit_load8(buf, X64_RAX, reg, (uint32_t)offsetof(jello_obj_header, kind));
  emit_cmp32_imm(buf, X64_RAX, kind);
  slow[(*nslow)++] = emit_jcc_rel32(buf, JIT_JNE, 0);
}

static void emit_shift_r64_imm8(jit_exec_buf* buf, uint8_t ext, unsigned dst, uint8_t imm8) {
  x64_insn i = x64_reg(0, X64_W, 0xC1, ext & 7u, dst);
  x64_byte(&i, imm8);
  x64_emit(buf, i);
}

static void emit_shl_r64_imm8(jit_exec_buf* buf, unsigned dst, uint8_t imm8) {
  emit_shift_r64_imm8(buf, 4, dst, imm8);
}

static void emit_callr_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t caller_dst = ins->a;
  uint32_t callee_disp = ins->b * REG_SLOT_BYTES;
  uint32_t arg_first = ins->imm;
  uint32_t nargs = ins->c;
  const jello_bc_function* caller_f = &ctx->m->funcs[ctx->fi];
  jit_reg_state_discard_currents(ctx->ra);
  if(!ctx->entries) emit_native_stack_check(ctx);

  size_t slow[8];
  size_t nslow = 0;
  size_t j_call = 0;
  if(ctx->entries) {
    // R10 = the Function, R9 = its function index, X64_STAGE = its frame_layout.
    emit_load64(buf, X64_R10, (unsigned)JIT_REG_BASE, callee_disp);
    emit_obj_kind_guard(buf, X64_R10, (uint32_t)JELLO_OBJ_FUNCTION, slow, &nslow);
    emit_load32(buf, X64_R9, X64_R10, (uint32_t)offsetof(jello_function, func_index));
    emit_load_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, jit_layout_by_func_len));
    emit_cmp32(buf, X64_RAX, X64_R9);
    slow[nslow++] = emit_jcc_rel32(buf, JIT_JBE, 0); // JBE: no layout index
    emit_load_vm64(buf, X64_STAGE, (uint32_t)offsetof(jello_vm, jit_layout_by_func));
    emit_mov64(buf, X64_RAX, X64_R9);
    emit_shl_r64_imm8(buf, X64_RAX, 3);
    emit_add64(buf, X64_STAGE, X64_RAX);
    emit_load64(buf, X64_STAGE, X64_STAGE, 0);
    emit_test64(buf, X64_STAGE, X64_STAGE);
    slow[nslow++] = emit_jcc_rel32(buf, JIT_JE, 0);
    emit_load_vm32(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
    emit_cmp_vm(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_limit), 0);
    slow[nslow++] = emit_jcc_rel32(buf, JIT_JAE, 0); // JAE
    emit_load_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_cap));
    emit_cmp32(buf, X64_RCX, X64_RAX);
    slow[nslow++] = emit_jcc_rel32(buf, JIT_JAE, 0);
    emit_load_vm32(buf, X64_RCX, (uint32_t)offsetof(jello_vm, frame_stack_top));
    emit_load32(buf, X64_R8, X64_STAGE, (uint32_t)offsetof(frame_layout, total));
    emit_add64(buf, X64_R8, X64_RCX);
    emit_cmp_vm(buf, X64_R8, (uint32_t)offsetof(jello_vm, frame_stack_limit), 0);
    slow[nslow++] = emit_jcc_rel32(buf, JIT_JA, 0); // JA: does not fit

    // The push cannot fail from here; R11 = the new frame's registers.
    emit_store_vm32(buf, X64_R8, (uint32_t)offsetof(jello_vm, frame_stack_top));
    emit_load_vm64(buf, X64_R11, (uint32_t)offsetof(jello_vm, frame_stack));
    emit_add64(buf, X64_R11, X64_RCX);
    emit_load32(buf, X64_RAX, X64_STAGE, (uint32_t)offsetof(frame_layout, has_pointer));
    emit_test32(buf, X64_RAX, X64_RAX);
    size_t j_no_ptr = emit_jcc_rel32(buf, JIT_JE, 0);
    emit_mov64(buf, X64_RDI, X64_R11);
    emit_load64(buf, X64_RSI, X64_STAGE, (uint32_t)offsetof(frame_layout, init));
    emit_load32(buf, X64_RDX, X64_STAGE, (uint32_t)offsetof(frame_layout, total));
    emit_call_fn(buf, (uintptr_t)JIT_C(memcpy));
    emit_mov64(buf, X64_R11, X64_RAX);
    emit_load64(buf, X64_R10, (unsigned)JIT_REG_BASE, callee_disp);
    emit_load32(buf, X64_R9, X64_R10, (uint32_t)offsetof(jello_function, func_index));
    patch_jump_here(buf, j_no_ptr);

    emit_fill_call_frame(buf, NULL, 0, 0, 0, caller_dst);

    // The captures, from RDI into the registers from cap_start at RSI, EDX at a time.
    emit_load32(buf, X64_RDX, X64_R10, (uint32_t)offsetof(jello_function, ncaps));
    emit_test32(buf, X64_RDX, X64_RDX);
    size_t j_no_caps = emit_jcc_rel32(buf, JIT_JE, 0);
    emit_load64(buf, X64_RSI, X64_STAGE, (uint32_t)offsetof(frame_layout, f));
    emit_load32(buf, X64_RSI, X64_RSI, (uint32_t)offsetof(jello_bc_function, cap_start));
    emit_shl_r64_imm8(buf, X64_RSI, 3);
    emit_add64(buf, X64_RSI, X64_R11);
    emit_mov64(buf, X64_RDI, X64_R10);
    emit_alu64_imm(buf, JIT_ALU_ADD, X64_RDI, (uint32_t)offsetof(jello_function, captures));
    size_t loop = buf->len;
    emit_load64(buf, X64_RAX, X64_RDI, 0);
    emit_store64(buf, X64_RAX, X64_RSI, 0);
    emit_alu64_imm(buf, JIT_ALU_ADD, X64_RDI, 8u);
    emit_alu64_imm(buf, JIT_ALU_ADD, X64_RSI, 8u);
    emit_alu32_imm(buf, JIT_ALU_SUB, X64_RDX, 1u); // sub edx, 1
    emit_jcc_rel32(buf, JIT_JNE, (int32_t)((int64_t)loop - (int64_t)(buf->len + 6)));
    patch_jump_here(buf, j_no_caps);

    // The arguments, at the caller's widths, which validation makes the callee's.
    emit_copy_args(ctx, caller_f, arg_first, arg_first, nargs);
    emit_mov64_imm(buf, X64_RAX, (uint64_t)(uintptr_t)ctx->entries);
    emit_shl_r64_imm8(buf, X64_R9, 3);
    emit_add64(buf, X64_RAX, X64_R9);
    emit_load64(buf, X64_STAGE, X64_RAX, 0);
    j_call = emit_jmp_rel32(buf, 0);
  }

  for(size_t i = 0; i < nslow; i++)
    patch_jump_here(buf, slow[i]);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov32_imm(buf, X64_RSI, caller_dst);
  emit_mov32_imm(buf, X64_RDX, ins->b);
  emit_mov32_imm(buf, X64_RCX, arg_first);
  emit_mov32_imm(buf, X64_R8, nargs);
  emit_call_fn(buf, (uintptr_t)jit_callr_push);
  emit_test64(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // JE: refused
  // The target waits in X64_STAGE while JIT_REG_BASE moves to the callee's frame.
  emit_mov64(buf, X64_STAGE, X64_RAX);
  emit_reload_jit_reg_base(buf);
  if(ctx->entries) {
    patch_jump_here(buf, j_call);
    emit_bridge_when_deep(ctx, X64_STAGE);
  }
  emit_call_jit_code(buf, X64_STAGE);
  emit_after_jit_call(ctx, ins->a);
}

static void emit_try_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov32_imm(buf, X64_RSI, (uint32_t)((int32_t)ins->bc_pc + 1 + (int32_t)ins->imm));
  emit_mov32_imm(buf, X64_RDX, ins->a);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_try_push));
  emit_test32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // JE: no memory
}

static void emit_endtry_insn(emit_ctx* ctx) {
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(ctx->buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_try_pop));
}

static void emit_throw_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov32_imm(buf, X64_RSI, ins->a);
  emit_mov32_imm(buf, X64_RDX, ctx->fi);
  emit_mov32_imm(buf, X64_RCX, ctx->cur_bc_pc);
  emit_call_fn(buf, (uintptr_t)jit_throw);
  size_t at = emit_jmp_rel32(buf, 0);
  jit_push_trap_jump(ctx, at, JIT_TRAP_PASSTHROUGH);
}

void jit_emit_catch_dispatch(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_load_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_cmp32_imm(buf, X64_RAX, (uint32_t)JELLO_TRAP_INTERNAL_UNWIND);
  size_t not_unwind = emit_jcc_rel32(buf, JIT_JNE, 0);
  emit_load_vm32(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_load_vm32(buf, X64_RCX, (uint32_t)offsetof(jello_vm, unwind_len));
  emit_cmp32(buf, X64_RAX, X64_RCX);
  size_t not_here = emit_jcc_rel32(buf, JIT_JNE, 0);
  emit_store_vm32_imm(buf, (uint32_t)offsetof(jello_vm, trap_code), (uint32_t)JELLO_TRAP_NONE);
  emit_store_vm32_imm(buf, JIT_TRAP_SITE_OFF, 0u);
  emit_reload_jit_reg_base(buf); // leaves the top call_frame's address in R8
  emit_load32(buf, X64_RAX, X64_R8, (uint32_t)offsetof(call_frame, pc));
  for(uint32_t i = 0; i < ctx->ncatch; i++) {
    emit_cmp32_imm(buf, X64_RAX, ctx->catch_pcs[i]);
    size_t at = emit_jcc_rel32(buf, JIT_JE, 0);
    jit_push_branch(ctx, at, ctx->catch_pcs[i]);
  }
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_unwind_lost);
  patch_jump_here(buf, not_unwind);
  patch_jump_here(buf, not_here);
  emit_trap_tail(ctx);
}

static void emit_mul32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  x64_rr(buf, 0, 0, 0x0FAF, dst, src);
}

static void emit_load_u32_slot_to_edi(jit_exec_buf* buf, uint8_t vreg) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[8];
  bytes[0] = 0x41; /* REX.B: base R13 */
  bytes[1] = 0x8B;
  bytes[2] = (uint8_t)(0x80u | (X64_RDI << 3) | 5u);
  memcpy(&bytes[3], &disp, 4);
  jit_exec_buf_write(buf, bytes, 7);
}

static void emit_rsp_adjust_local(jit_exec_buf* buf, int is_add, uint8_t imm8) {
  uint8_t bytes[4];
  bytes[0] = 0x48;
  bytes[1] = 0x83;
  bytes[2] = (uint8_t)(0xC0u | (is_add ? 0x00u : 0x28u) | (unsigned)X64_RSP);
  bytes[3] = imm8;
  jit_exec_buf_write(buf, bytes, 4);
}

static void emit_movss_xmm_mem_rsp(jit_exec_buf* buf, unsigned xmm, uint8_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if(xmm >= 8) bytes[n++] = 0x44;
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x44u | ((xmm & 7u) << 3)); /* mod=01, rm=RSP -> SIB */
  bytes[n++] = 0x24; /* SIB: base=RSP, index=none */
  bytes[n++] = disp;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movss_xmm_from_mem_rsp(jit_exec_buf* buf, unsigned xmm, uint8_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if(xmm >= 8) bytes[n++] = 0x44;
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x44u | ((xmm & 7u) << 3));
  bytes[n++] = 0x24;
  bytes[n++] = disp;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_r64_to_mem_rsp(jit_exec_buf* buf, unsigned src, uint8_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((src >= 8) ? 0x04u : 0));
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x44u | ((src & 7u) << 3));
  bytes[n++] = 0x24;
  bytes[n++] = disp;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_mov_r64_from_mem_rsp(jit_exec_buf* buf, unsigned dst, uint8_t disp) {
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((dst >= 8) ? 0x04u : 0));
  bytes[n++] = 0x8B;
  bytes[n++] = (uint8_t)(0x44u | ((dst & 7u) << 3));
  bytes[n++] = 0x24;
  bytes[n++] = disp;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_f16_binop(emit_ctx* ctx, const jello_jit_ir_insn* ins, uint8_t sse_op) {
  // Every XMM register is caller-saved, so the first operand waits on the stack.
  emit_sp_adjust(ctx->buf, 0, 16); // sub rsp, 16
  emit_load_u32_slot_to_edi(ctx->buf, ins->b);
  emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f16_to_f32));
  emit_movss_xmm_mem_rsp(ctx->buf, 0, 0); // movss [rsp], xmm0
  emit_load_u32_slot_to_edi(ctx->buf, ins->c);
  emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f16_to_f32));
  emit_movss_xmm_from_mem_rsp(ctx->buf, 1, 0); // movss xmm1, [rsp]
  emit_fbin32(ctx->buf, sse_op, 1, 0);
  emit_fmov32(ctx->buf, 0, 1);
  emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f32_to_f16));
  emit_sp_adjust(ctx->buf, 1, 16); // add rsp, 16
  jit_reg_state_clobber_call(ctx->ra);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(ctx->buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bin_f32(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, 0);
  int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  unsigned di = (unsigned)(dx - (int)JIT_PREG_XMM0);
  emit_movss_xmm_xmm(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_binss_xmm_xmm(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_binss_xmm_xmm(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_binss_xmm_xmm(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_binss_xmm_xmm(ctx->buf, 0x5E, di, ri); break;
    default: return;
  }
  jit_reg_store_home_xmm32(ctx->ra, ins->a, dx);
}

static void emit_bin_f64(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, 1);
  int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  unsigned di = (unsigned)(dx - (int)JIT_PREG_XMM0);
  emit_movsd_xmm_xmm(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_binsd_xmm_xmm(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_binsd_xmm_xmm(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_binsd_xmm_xmm(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_binsd_xmm_xmm(ctx->buf, 0x5E, di, ri); break;
    default: return;
  }
  jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
}

static void emit_cmp_float(emit_ctx* ctx, const jello_jit_ir_insn* ins, int is_f64) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, is_f64);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, is_f64);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  if(is_f64) emit_ucomisd(ctx->buf, li, ri);
  else emit_ucomiss(ctx->buf, li, ri);
  int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_setcc(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x92, (unsigned)d);
  emit_movzx_r32_r8(ctx->buf, (unsigned)d, (unsigned)d);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
}

static void emit_movsx_r32_r16(jit_exec_buf* buf, unsigned dst, unsigned src) {
  uint8_t bytes[5];
  size_t n = 0;
  if(dst >= 8 || src >= 8)
    bytes[n++] = (uint8_t)(0x40u | ((dst >= 8) ? 0x04u : 0) | ((src >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0xBF;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u));
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_load_f64(jit_exec_buf* buf, unsigned dd, double x) {
  uint64_t bits;
  memcpy(&bits, &x, sizeof bits);
  emit_mov64_imm(buf, X64_RAX, bits);
  emit_fmov_from64(buf, dd, X64_RAX);
}

static void emit_float_conv_check(emit_ctx* ctx, unsigned v, int is_f64, int to_i64) {
  jit_exec_buf* buf = ctx->buf;
  if(!is_f64) {
    emit_f32_to_f64(buf, 7, v);
    v = 7;
  }
  emit_load_f64(buf, 6, to_i64 ? VM_I64_FROM_FLOAT_LO : VM_I32_FROM_FLOAT_LO);
  emit_fcmp64(buf, v, 6);
  emit_jcc_trap(ctx, JIT_JBE, JIT_TRAP_ARITH); // JBE: v <= lo, or unordered
  emit_load_f64(buf, 6, to_i64 ? VM_I64_FROM_FLOAT_HI : VM_I32_FROM_FLOAT_HI);
  emit_fcmp64(buf, 6, v);
  emit_jcc_trap(ctx, JIT_JBE, JIT_TRAP_ARITH); // JBE: hi <= v, or unordered
}

static int emit_conv(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jir_conv)ins->imm) {
    case JIR_CONV_TRUNC_I8: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_r32_imm32(ctx->buf, 0x04, (unsigned)d, 0xFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_TRUNC_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_r32_imm32(ctx->buf, 0x04, (unsigned)d, 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_movsx_r32_r16(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_I32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_movsxd_r64_r32(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_int_to_f32(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_float_conv_check(ctx, (unsigned)(sx - (int)JIT_PREG_XMM0), 0, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttss2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_cvtsi2sd(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      emit_float_conv_check(ctx, (unsigned)(sx - (int)JIT_PREG_XMM0), 1, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttsd2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_f32_to_f64(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0),
                      (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_f64_to_f32(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0),
                      (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F64_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_cvtsi2sd(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      emit_float_conv_check(ctx, (unsigned)(sx - (int)JIT_PREG_XMM0), 1, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttsd2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_int_to_f32(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_float_conv_check(ctx, (unsigned)(sx - (int)JIT_PREG_XMM0), 0, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttss2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov64(ctx->buf, X64_RDI, (unsigned)s);
      emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f16_to_f32));
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_fmov32(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_fmov32(ctx->buf, 0, (unsigned)(sx - (int)JIT_PREG_XMM0));
      emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f32_to_f16));
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, X64_RAX);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_int_to_f32(ctx->buf, 0, (unsigned)s, 0);
      emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f32_to_f16));
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, X64_RAX);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_I32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov64(ctx->buf, X64_RDI, (unsigned)s);
      emit_call_fn(ctx->buf, (uintptr_t)JIT_C(vm_f16_to_f32));
      emit_float_conv_check(ctx, 0, 0, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttss2si(ctx->buf, (unsigned)d, 0, 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    default:
      return 0;
  }
}

static void emit_alu_r64_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  uint8_t bytes[7];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x48u | ((dst >= 8) ? 0x01u : 0));
  bytes[n++] = 0x81;
  bytes[n++] = (uint8_t)(0xC0u | ((op_ext & 7u) << 3) | (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_null_check(emit_ctx* ctx, unsigned gpr) {
  emit_test64(ctx->buf, gpr, gpr);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_NULL);
}

static void emit_buffer_check(emit_ctx* ctx, unsigned gpr) {
  emit_null_check(ctx, gpr);
  emit_load32(ctx->buf, X64_RAX, gpr, (uint32_t)offsetof(jello_bytes, h.type_id));
  emit_cmp32_imm(ctx->buf, X64_RAX, JELLO_BYTES_BUFFER);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_TYPE); // JNE: no buffer
}

static void emit_bounds_trap(emit_ctx* ctx, unsigned ptr, unsigned idx, uint32_t len_off) {
  emit_load32(ctx->buf, X64_RAX, ptr, len_off);
  emit_cmp32(ctx->buf, idx, X64_RAX);
  emit_jcc_trap(ctx, JIT_JAE, JIT_TRAP_BOUNDS); // JAE: idx >= length
}

static void emit_indexed_addr(emit_ctx* ctx, unsigned ptr, unsigned idx, uint32_t len_off,
                              uint32_t data_off, uint8_t shift) {
  jit_exec_buf* buf = ctx->buf;
  emit_bounds_trap(ctx, ptr, idx, len_off);
  emit_load64(buf, X64_RCX, ptr, data_off);
  emit_mov32(buf, X64_RDX, idx);
  if(shift) emit_shl_r64_imm8(buf, X64_RDX, shift);
  emit_add64(buf, X64_RCX, X64_RDX);
}

static void emit_bytes_len(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_from_r64_disp(buf, (unsigned)dst, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bytes_get_u8(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int idx = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_indexed_addr(ctx, (unsigned)bp, (unsigned)idx, (uint32_t)offsetof(jello_bytes, length),
                    (uint32_t)offsetof(jello_bytes, data), 0);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_movzx_r32_m8_r64_disp(buf, (unsigned)dst, X64_RCX, 0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bytes_set_u8(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_buffer_check(ctx, (unsigned)bp);
  int idx = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_indexed_addr(ctx, (unsigned)bp, (unsigned)idx, (uint32_t)offsetof(jello_bytes, length),
                    (uint32_t)offsetof(jello_bytes, data), 0);
  int val = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_cmp32_imm(buf, (unsigned)val, 255u);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // JA: val > 255
  emit_store8(buf, (unsigned)val, X64_RCX, 0);
}

static void emit_bytes_eq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);

  emit_cmp_r64_r64(buf, (unsigned)b, (unsigned)c);
  size_t j_same = emit_jcc_rel32(buf, 0x84, 0); /* JE -> true */

  emit_test_r64_r64(buf, (unsigned)b, (unsigned)b);
  size_t j_b_null = emit_jcc_rel32(buf, 0x84, 0); /* -> false */
  emit_test_r64_r64(buf, (unsigned)c, (unsigned)c);
  size_t j_c_null = emit_jcc_rel32(buf, 0x84, 0); /* -> false */

  emit_load32(buf, X64_RAX, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  emit_load32(buf, X64_RCX, (unsigned)c, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp32(buf, X64_RAX, X64_RCX);
  size_t j_len_ne = emit_jcc_rel32(buf, JIT_JNE, 0); // JNE -> false

  emit_load64(buf, X64_RAX, (unsigned)b, (uint32_t)offsetof(jello_bytes, data));
  emit_load64(buf, X64_RCX, (unsigned)c, (uint32_t)offsetof(jello_bytes, data));
  emit_load32(buf, X64_RDX, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, X64_RAX);
  emit_mov64(buf, X64_RSI, X64_RCX);
  emit_call_fn(buf, (uintptr_t)JIT_C(memcmp));
  emit_test32(buf, X64_RAX, X64_RAX);
  emit_setcc_from_flags(buf, JIT_SETE, (unsigned)dst); // dst = (memcmp == 0)
  size_t j_done1 = emit_jmp_rel32(buf, 0);

  size_t false_lbl = buf->len;
  patch_jcc_rel32(buf, j_b_null, (int32_t)(false_lbl - (j_b_null + 6)));
  patch_jcc_rel32(buf, j_c_null, (int32_t)(false_lbl - (j_c_null + 6)));
  patch_jcc_rel32(buf, j_len_ne, (int32_t)(false_lbl - (j_len_ne + 6)));
  emit_mov_r32_imm32(buf, (unsigned)dst, 0u);
  size_t j_done2 = emit_jmp_rel32(buf, 0);

  size_t true_lbl = buf->len;
  patch_jcc_rel32(buf, j_same, (int32_t)(true_lbl - (j_same + 6)));
  emit_mov_r32_imm32(buf, (unsigned)dst, 1u);

  size_t done_lbl = buf->len;
  patch_jmp_rel32(buf, j_done1, (int32_t)(done_lbl - (j_done1 + 5)));
  patch_jmp_rel32(buf, j_done2, (int32_t)(done_lbl - (j_done2 + 5)));

  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static int bytes_bitop_kind(uint32_t imm) {
  jello_jir_bin op = (jello_jir_bin)imm;
  if(op == JIR_BIN_AND) return 0;
  if(op == JIR_BIN_OR) return 1;
  return 2; /* JIR_BIN_XOR */
}

static void emit_bytes_bitop(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_null_check(ctx, (unsigned)c);
  int op_kind = bytes_bitop_kind(ins->imm);

  emit_mov64(buf, X64_RAX, (unsigned)b);
  emit_mov64(buf, X64_RCX, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov64(buf, X64_RSI, X64_RAX);
  emit_mov64(buf, X64_RDX, X64_RCX);
  emit_mov32_imm(buf, X64_RCX, (uint32_t)op_kind);
  emit_call_fn(buf, (uintptr_t)jit_bytes_bitop);
  emit_test64(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // NULL: trap_code already set
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void bytes_rw_info(jello_jir_bytes_rw rw, uint32_t* width, int* be, int* is_float) {
  switch(rw) {
    case JIR_BRW_U16_LE: *width = 2; *be = 0; *is_float = 0; return;
    case JIR_BRW_U16_BE: *width = 2; *be = 1; *is_float = 0; return;
    case JIR_BRW_U32_LE: *width = 4; *be = 0; *is_float = 0; return;
    case JIR_BRW_U32_BE: *width = 4; *be = 1; *is_float = 0; return;
    case JIR_BRW_I32_LE: *width = 4; *be = 0; *is_float = 0; return;
    case JIR_BRW_I32_BE: *width = 4; *be = 1; *is_float = 0; return;
    case JIR_BRW_F32_LE: *width = 4; *be = 0; *is_float = 1; return;
    case JIR_BRW_F32_BE: *width = 4; *be = 1; *is_float = 1; return;
    case JIR_BRW_I64_LE: *width = 8; *be = 0; *is_float = 0; return;
    case JIR_BRW_I64_BE: *width = 8; *be = 1; *is_float = 0; return;
    case JIR_BRW_F64_LE: *width = 8; *be = 0; *is_float = 1; return;
    case JIR_BRW_F64_BE: *width = 8; *be = 1; *is_float = 1; return;
  }
  *width = 4; *be = 0; *is_float = 0;
}

static void emit_bytes_bounds_check(emit_ctx* ctx, int bytes_gpr, int off_gpr, uint32_t width) {
  jit_exec_buf* buf = ctx->buf;
  emit_load32(buf, X64_RAX, (unsigned)bytes_gpr, (uint32_t)offsetof(jello_bytes, length));
  emit_mov32(buf, X64_RCX, (unsigned)off_gpr);
  emit_alu64_imm(buf, JIT_ALU_ADD, X64_RCX, width);
  emit_cmp64(buf, X64_RCX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // JA: off + width > length
}

static void emit_bytes_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width; int be, is_float;
  bytes_rw_info((jello_jir_bytes_rw)ins->imm, &width, &be, &is_float);
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_bounds_check(ctx, bp, off, width);

  emit_load64(buf, X64_RAX, (unsigned)bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov32(buf, X64_RCX, (unsigned)off);
  emit_add64(buf, X64_RAX, X64_RCX);

  if(!is_float) {
    int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
    if(width == 2) emit_load16(buf, (unsigned)dst, X64_RAX, 0);
    else if(width == 4) emit_load32(buf, (unsigned)dst, X64_RAX, 0);
    else emit_load64(buf, (unsigned)dst, X64_RAX, 0);
    if(be) {
      if(width == 8) emit_bswap64(buf, (unsigned)dst);
      else if(width == 4) emit_bswap32(buf, (unsigned)dst);
      else emit_bswap16(buf, (unsigned)dst, 8);
    }
    if(width == 8) jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
    else jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
  } else {
    if(width == 4) emit_load32(buf, X64_RCX, X64_RAX, 0);
    else emit_load64(buf, X64_RCX, X64_RAX, 0);
    if(be) {
      if(width == 8) emit_bswap64(buf, X64_RCX);
      else emit_bswap32(buf, X64_RCX);
    }
    int dst = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, width == 8);
    if(width == 4) {
      emit_movd_xmm_r32(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), X64_RCX);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dst);
    } else {
      emit_movq_xmm_r64(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), X64_RCX);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dst);
    }
  }
}

static void emit_bytes_write(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width; int be, is_float;
  bytes_rw_info((jello_jir_bytes_rw)ins->imm, &width, &be, &is_float);
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_buffer_check(ctx, (unsigned)bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_bounds_check(ctx, bp, off, width);

  if(!is_float) {
    int val = jit_reg_get_gpr(ctx->ra, ins->a, 1);
    emit_mov_r64_r64(buf, X64_RCX, (unsigned)val);
  } else {
    int val = jit_reg_get_xmm(ctx->ra, ins->a, 1, width == 8);
    if(width == 4) emit_movd_r32_xmm(buf, X64_RCX, (unsigned)(val - (int)JIT_PREG_XMM0));
    else emit_movq_r64_xmm(buf, X64_RCX, (unsigned)(val - (int)JIT_PREG_XMM0));
  }
  if(be) {
    if(width == 8) emit_bswap_r64(buf, X64_RCX);
    else if(width == 4) emit_bswap_r32(buf, X64_RCX);
    else emit_rol16_imm8(buf, X64_RCX, 8);
  }

  emit_load64(buf, X64_RAX, (unsigned)bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov32(buf, X64_RDX, (unsigned)off);
  emit_add64(buf, X64_RAX, X64_RDX);

  if(width == 2) emit_store_r16_to_r64_disp(buf, X64_RCX, X64_RAX, 0);
  else if(width == 4) emit_store_r32_to_r64_disp(buf, X64_RCX, X64_RAX, 0);
  else emit_store_r64_to_r64_disp(buf, X64_RCX, X64_RAX, 0);
}

static void emit_bytes_blit(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int dst_bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_buffer_check(ctx, (unsigned)dst_bp);
  int src_bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)src_bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);

  emit_load32(buf, X64_RAX, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, length));
  emit_mov32(buf, X64_RCX, (unsigned)off);
  emit_add64(buf, X64_RCX, X64_RAX);
  emit_load32(buf, X64_RDX, (unsigned)dst_bp, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp64(buf, X64_RCX, X64_RDX);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // JA: off + src->length > dst->length

  // RAX = dst data + off, RCX = src data, RDX = src length
  emit_load64(buf, X64_RAX, (unsigned)dst_bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov32(buf, X64_RCX, (unsigned)off);
  emit_add64(buf, X64_RAX, X64_RCX);
  emit_load64(buf, X64_RCX, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, data));
  emit_load32(buf, X64_RDX, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, length));

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, X64_RAX);
  emit_mov64(buf, X64_RSI, X64_RCX);
  emit_call_fn(buf, (uintptr_t)JIT_C(memmove));
}

static void emit_bits_write(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_buffer_check(ctx, (unsigned)bp);
  int val = jit_reg_get_gpr(ctx->ra, ins->b, 1);

  emit_load32(buf, X64_RAX, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
  emit_mov32_imm(buf, X64_RCX, 3u);
  emit_shl_cl_r64(buf, X64_RAX);
  emit_mov64_imm(buf, X64_RDX, end_bit);
  emit_cmp64(buf, X64_RDX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // JA: end_bit > length*8

  emit_mov64(buf, X64_RAX, (unsigned)bp);
  emit_mov32(buf, X64_RCX, (unsigned)val);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, X64_RAX);
  emit_mov32_imm(buf, X64_RSI, width);
  emit_mov32_imm(buf, X64_RDX, bit_off);
  emit_call_fn(buf, (uintptr_t)jit_bits_write);
}

static void emit_bytes_freeze(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  const uint32_t mark = (uint32_t)offsetof(jello_bytes, h.type_id);
  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_null_check(ctx, (unsigned)bp);
  emit_load32(buf, X64_RAX, (unsigned)bp, mark);
  emit_cmp32_imm(buf, X64_RAX, JELLO_BYTES_BUFFER);
  size_t shared = emit_jcc_rel8(buf, JIT_JNE); // JNE: no buffer
  emit_mov32_imm(buf, X64_RAX, 0);
  emit_store32(buf, X64_RAX, (unsigned)bp, mark);
  patch_jcc_rel8_here(buf, shared);
}

static void emit_bytes_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int len = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov32(buf, X64_RAX, (unsigned)len);
  // A negative length, read unsigned, is past INT32_MAX, as in op_bytes_new.
  emit_cmp32_imm(buf, X64_RAX, (uint32_t)INT32_MAX);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // JA: len > INT32_MAX
  // As in op_bytes_new, a max_bytes_len of 0 means no limit.
  emit_load_vm32(buf, X64_RCX, (uint32_t)offsetof(jello_vm, max_bytes_len));
  emit_test32(buf, X64_RCX, X64_RCX);
  size_t limit_skip = emit_jcc_rel32(buf, JIT_JE, 0); // JE: max_bytes_len == 0
  emit_cmp32(buf, X64_RAX, X64_RCX);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_LIMIT); // JA: len > max_bytes_len
  patch_jump_here(buf, limit_skip);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov32(buf, X64_RSI, X64_RAX);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_buffer_new));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bytes_concat2(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_null_check(ctx, (unsigned)c);
  emit_mov64(buf, X64_RAX, (unsigned)b);
  emit_mov64(buf, X64_RCX, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov64(buf, X64_RSI, X64_RAX);
  emit_mov64(buf, X64_RDX, X64_RCX);
  emit_call_fn(buf, (uintptr_t)jit_bytes_concat2);
  emit_test64(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // NULL: trap_code already set (BOUNDS)
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bits_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);

  emit_load32(buf, X64_RAX, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
  emit_mov32_imm(buf, X64_RCX, 3u);
  emit_shl_cl_r64(buf, X64_RAX);
  emit_mov64_imm(buf, X64_RDX, end_bit);
  emit_cmp64(buf, X64_RDX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS);

  emit_mov64(buf, X64_RAX, (unsigned)bp);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, X64_RAX);
  emit_mov32_imm(buf, X64_RSI, width);
  emit_mov32_imm(buf, X64_RDX, bit_off);
  emit_call_fn(buf, (uintptr_t)jit_bits_read);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_movss_xmm_from_r64_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  uint8_t bytes[9];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((xmm >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x80u | ((xmm & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movss_to_r64_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  uint8_t bytes[9];
  size_t n = 0;
  bytes[n++] = 0xF3;
  if((xmm >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x80u | ((xmm & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movsd_xmm_from_r64_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  uint8_t bytes[9];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((xmm >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x80u | ((xmm & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_movsd_to_r64_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  uint8_t bytes[9];
  size_t n = 0;
  bytes[n++] = 0xF2;
  if((xmm >= 8) || (base >= 8)) bytes[n++] = (uint8_t)(0x40u | ((xmm >= 8) ? 0x04u : 0) | ((base >= 8) ? 0x01u : 0));
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x80u | ((xmm & 7u) << 3) | (base & 7u));
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_fload32(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  x64_rm(buf, 0xF3, 0, 0x0F10, xmm, base, disp);
}

static void emit_fstore32(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  x64_rm(buf, 0xF3, 0, 0x0F11, xmm, base, disp);
}

static void emit_fload64(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  x64_rm(buf, 0xF2, 0, 0x0F10, xmm, base, disp);
}

static void emit_fstore64(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  x64_rm(buf, 0xF2, 0, 0x0F11, xmm, base, disp);
}

/* -- Struct ops -- */
static int is_xmm_field_kind(jello_type_kind k) {
  return k == JELLO_T_F32 || k == JELLO_T_F64;
}

static void emit_struct_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id tid = ctx->f->reg_types[ins->a];
  uint32_t struct_sig_id = ctx->m->types[tid].as.strct.struct_sig_id;

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov32_imm(buf, X64_RDX, struct_sig_id);
  emit_call_fn(buf, (uintptr_t)JIT_C(jello_struct_new));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_load_slot(emit_ctx* ctx, uint8_t r, jello_type_kind k, unsigned base,
                           uint32_t disp) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = (uint32_t)jello_slot_size(k);
  if(is_xmm_field_kind(k)) {
    int dst = jit_reg_alloc_xmm(ctx->ra, r, 0, width == 8);
    if(width == 8) {
      emit_fload64(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), base, disp);
      jit_reg_store_home_xmm64(ctx->ra, r, dst);
    } else {
      emit_fload32(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), base, disp);
      jit_reg_store_home_xmm32(ctx->ra, r, dst);
    }
  } else {
    int dst = jit_reg_alloc_gpr(ctx->ra, r, 0);
    if(width == 8) {
      emit_load64(buf, (unsigned)dst, base, disp);
      jit_reg_store_home_gpr64(ctx->ra, r, dst);
    } else {
      emit_load32(buf, (unsigned)dst, base, disp);
      jit_reg_store_home_gpr32(ctx->ra, r, dst);
    }
  }
}

static void emit_store_slot(emit_ctx* ctx, uint8_t r, jello_type_kind k, unsigned base,
                            uint32_t disp) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = (uint32_t)jello_slot_size(k);
  if(is_xmm_field_kind(k)) {
    int val = jit_reg_get_xmm(ctx->ra, r, 1, width == 8);
    if(width == 8) emit_fstore64(buf, (unsigned)(val - (int)JIT_PREG_XMM0), base, disp);
    else emit_fstore32(buf, (unsigned)(val - (int)JIT_PREG_XMM0), base, disp);
  } else {
    int val = jit_reg_get_gpr(ctx->ra, r, 1);
    if(width == 8) emit_store64(buf, (unsigned)val, base, disp);
    else emit_store32(buf, (unsigned)val, base, disp);
  }
}

static int emit_struct_field(emit_ctx* ctx, uint8_t r, uint32_t field, jello_type_kind* kind,
                             uint32_t* disp) {
  const jello_struct_sig* sig =
      &ctx->m->struct_sigs[ctx->m->types[ctx->f->reg_types[r]].as.strct.struct_sig_id];
  *kind = ctx->m->types[sig->field_type[field]].kind;
  *disp = (uint32_t)offsetof(jello_struct, fields) + sig->field_offset[field];
  int sp = jit_reg_get_gpr(ctx->ra, r, 1);
  emit_null_check(ctx, (unsigned)sp);
  return sp;
}

static void emit_struct_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jello_type_kind kind;
  uint32_t disp;
  int sp = emit_struct_field(ctx, ins->b, ins->imm, &kind, &disp);
  emit_load_slot(ctx, ins->a, kind, (unsigned)sp, disp);
}

static void emit_struct_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jello_type_kind kind;
  uint32_t disp;
  int sp = emit_struct_field(ctx, ins->b, ins->imm, &kind, &disp);
  emit_store_slot(ctx, ins->a, kind, (unsigned)sp, disp);
}

static jello_type_kind emit_array_elem_addr(emit_ctx* ctx, uint8_t arr, uint8_t idx) {
  jello_type_kind kind = ctx->m->types[ctx->m->types[ctx->f->reg_types[arr]].as.unary.elem].kind;
  int ap = jit_reg_get_gpr(ctx->ra, arr, 1);
  emit_null_check(ctx, (unsigned)ap);
  int ip = jit_reg_get_gpr(ctx->ra, idx, 1);
  emit_indexed_addr(ctx, (unsigned)ap, (unsigned)ip, (uint32_t)offsetof(jello_array, length),
                    (uint32_t)offsetof(jello_array, data), jello_slot_size(kind) == 8 ? 3 : 2);
  return kind;
}

static void emit_array_len(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int ap = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)ap);
  emit_load_slot(ctx, ins->a, JELLO_T_I32, (unsigned)ap, (uint32_t)offsetof(jello_array, length));
}

static void emit_array_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_load_slot(ctx, ins->a, emit_array_elem_addr(ctx, ins->b, ins->c), X64_RCX, 0);
}

static void emit_array_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_store_slot(ctx, ins->a, emit_array_elem_addr(ctx, ins->b, ins->c), X64_RCX, 0);
}

static jello_type_kind emit_globals_base(emit_ctx* ctx, uint32_t g) {
  emit_load_vm64(ctx->buf, X64_RCX, (uint32_t)offsetof(jello_vm, globals));
  return ctx->m->types[ctx->m->global_types[g]].kind;
}

static void emit_global_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_load_slot(ctx, ins->a, emit_globals_base(ctx, ins->imm), X64_RCX, ins->imm * 8u);
}

static void emit_global_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_store_slot(ctx, ins->a, emit_globals_base(ctx, ins->imm), X64_RCX, ins->imm * 8u);
}

/* -- the Dynamic boundary -- */
static void emit_current_rf_ptr_into_r11(jit_exec_buf* buf) {
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_R11, X64_RAX);
  emit_imul_r64_r64(buf, X64_R11, X64_RDX);
  emit_add_r64_r64(buf, X64_R11, X64_RCX);
  emit_alu_r64_imm32(buf, 0x00, X64_R11, (uint32_t)offsetof(call_frame, rf));
}

static void emit_typed_helper_args(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_current_rf_ptr_into_r11(buf);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov64_imm(buf, X64_RDX, (uint64_t)(uintptr_t)ctx->f);
  emit_mov64(buf, X64_RCX, X64_R11);
  emit_mov32_imm(buf, X64_R8, r);
}

static void emit_store_from_boxed_stage(emit_ctx* ctx, uint8_t dst) {
  jit_exec_buf* buf = ctx->buf;
  emit_typed_helper_args(ctx, dst);
  emit_mov64(buf, X64_R9, X64_STAGE);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_store_from_boxed));
  emit_test32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // 0: trap_code already set
}

static void emit_tag_cmp(jit_exec_buf* buf, unsigned src, uint32_t tag) {
  emit_mov32(buf, X64_RAX, src);
  emit_alu_r32_imm8(buf, JIT_ALU_AND, X64_RAX, 7u); // and eax, 7
  emit_cmp32_imm(buf, X64_RAX, tag);
}

static void emit_unbox_stage_into(emit_ctx* ctx, uint8_t dst) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind k = vm_reg_kind(ctx->m, ctx->f, dst);
  if(k == JELLO_T_DYNAMIC) {
    int r = jit_reg_alloc_gpr(ctx->ra, dst, 0);
    emit_mov64(buf, (unsigned)r, X64_STAGE);
    jit_reg_store_home_gpr64(ctx->ra, dst, r);
    return;
  }
  int tag = vm_kind_immediate_tag(k);
  int f64 = k == JELLO_T_F64;
  if(!f64 && tag < 0) {
    emit_store_from_boxed_stage(ctx, dst);
    if(k == JELLO_T_F32) (void)jit_reg_get_xmm(ctx->ra, dst, 1, 0);
    else (void)jit_reg_get_gpr(ctx->ra, dst, 1);
    return;
  }
  // Both paths leave the value in p, bound to `dst`; the slow path reloads it from home.
  size_t slow[3];
  size_t nslow = 0;
  int p = f64 ? jit_reg_alloc_xmm(ctx->ra, dst, 0, 1) : jit_reg_alloc_gpr(ctx->ra, dst, 0);
  unsigned pi = f64 ? (unsigned)(p - (int)JIT_PREG_XMM0) : (unsigned)p;
  if(f64) {
    emit_obj_kind_guard(buf, X64_STAGE, (uint32_t)JELLO_OBJ_BOX_F64, slow, &nslow);
    emit_fload64(buf, pi, X64_STAGE, (uint32_t)offsetof(jello_box_f64, value));
    jit_reg_store_home_xmm64(ctx->ra, dst, p);
  } else {
    uint32_t bits = vm_kind_immediate_bits(k);
    emit_tag_cmp(buf, X64_STAGE, (uint32_t)tag);
    slow[nslow++] = emit_jcc_rel32(buf, JIT_JNE, 0); // JNE: another tag
    emit_mov64(buf, pi, X64_STAGE);
    emit_shift_r64_imm8(buf, 5, pi, 3); // shr
    if(bits == 32u) emit_mov32(buf, pi, pi);
    else emit_alu32_imm(buf, JIT_ALU_AND, pi, (1u << bits) - 1u); // and
    jit_reg_store_home_gpr32(ctx->ra, dst, p);
  }
  size_t j_done = emit_jmp_rel32(buf, 0);
  for(size_t i = 0; i < nslow; i++) {
    patch_jump_here(buf, slow[i]);
  }
  emit_store_from_boxed_stage(ctx, dst);
  if(f64) jit_spill_load_xmm64(buf, pi, dst);
  else jit_spill_load_gpr64(buf, pi, dst);
  patch_jump_here(buf, j_done);
}

static void emit_box_into_rax(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind k = vm_reg_kind(ctx->m, ctx->f, r);
  int tag = vm_kind_immediate_tag(k);
  if(k == JELLO_T_DYNAMIC) {
    jit_spill_load_gpr64(buf, X64_RAX, r);
  } else if(tag >= 0) {
    emit_load32(buf, X64_RAX, (unsigned)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES);
    if(k == JELLO_T_BOOL) {
      emit_test32(buf, X64_RAX, X64_RAX);
      emit_setcc_from_flags(buf, JIT_SETNE, X64_RAX); // setne
    }
    emit_shl_r64_imm8(buf, X64_RAX, 3);
    emit_alu64_imm(buf, JIT_ALU_OR, X64_RAX, (uint32_t)tag); // or rax, tag
  } else if(k == JELLO_T_F64) {
    emit_fload64(buf, 0, (unsigned)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES);
    emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, (uintptr_t)JIT_C(jello_box_f64_new));
  } else {
    emit_typed_helper_args(ctx, r);
    emit_call_fn(buf, (uintptr_t)JIT_C(vm_box_from_typed));
  }
}

static void emit_handler_call(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov32_imm(buf, X64_RDX, ctx->fi);
  emit_mov32_imm(buf, X64_RCX, ctx->cur_bc_pc);
  emit_call_fn(buf, (uintptr_t)jit_run_handler);
  emit_test32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_PASSTHROUGH); // JNE: trapped, attributed by jit_run_handler
}

static void emit_enum_guard(emit_ctx* ctx, unsigned ep, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_null_check(ctx, ep);
  emit_load8(buf, X64_RAX, ep, (uint32_t)offsetof(jello_obj_header, kind));
  emit_cmp32_imm(buf, X64_RAX, (uint32_t)JELLO_OBJ_ENUM);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_TYPE);
  emit_load32(buf, X64_RAX, ep, (uint32_t)offsetof(jello_obj_header, type_id));
  emit_cmp32_imm(buf, X64_RAX, ctx->f->reg_types[r]);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_TYPE);
}

static void emit_enum_tag(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int ep = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_enum_guard(ctx, (unsigned)ep, ins->b);
  emit_load_slot(ctx, ins->a, JELLO_T_I32, (unsigned)ep, (uint32_t)offsetof(jello_enum, tag));
}

static void emit_enum_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int ep = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_enum_guard(ctx, (unsigned)ep, ins->b);
  int ip = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bounds_trap(ctx, (unsigned)ep, (unsigned)ip, (uint32_t)offsetof(jello_enum, nfields));
  emit_mov32(buf, X64_RDX, (unsigned)ip);
  emit_shl_r64_imm8(buf, X64_RDX, 3);
  emit_add64(buf, X64_RDX, (unsigned)ep);
  jit_reg_state_discard_currents(ctx->ra);
  emit_load64(buf, X64_STAGE, X64_RDX, (uint32_t)offsetof(jello_enum, fields));
  emit_unbox_stage_into(ctx, ins->a);
}

static void emit_to_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_box_into_rax(ctx, ins->b);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_from_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(ctx->buf, X64_STAGE, (unsigned)src);
  emit_unbox_stage_into(ctx, ins->a);
}

static void emit_is_enum(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov64(buf, X64_RDI, (unsigned)src);
  emit_mov32_imm(buf, X64_RSI, ins->imm);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_dynamic_is_enum));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_null_flags(emit_ctx* ctx, uint8_t r) {
  int s = jit_reg_get_gpr(ctx->ra, r, 1);
  if(vm_reg_kind(ctx->m, ctx->f, r) == JELLO_T_DYNAMIC) {
    emit_tag_cmp(ctx->buf, (unsigned)s, (uint32_t)JELLO_TAG_NULL);
  } else {
    emit_test64(ctx->buf, (unsigned)s, (unsigned)s);
  }
}

static void emit_kindof(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)JIT_C(vm_kindof_dynamic));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_physeq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_sp_adjust(buf, 0, 16); // sub rsp, 16
  emit_box_into_rax(ctx, ins->b);
  emit_store64_sp(buf, X64_RAX, 0);
  emit_box_into_rax(ctx, ins->c);
  emit_load64_sp(buf, X64_RCX, 0);
  emit_sp_adjust(buf, 1, 16); // add rsp, 16

  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_cmp_r64_r64(buf, X64_RCX, X64_RAX);
  emit_setcc(buf, 0x94, (unsigned)dst);
  emit_movzx_r32_r8(buf, (unsigned)dst, (unsigned)dst);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/* -- Object ops -- */
static void emit_obj_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id tid = ctx->f->reg_types[ins->a];
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov32_imm(buf, X64_RSI, tid);
  emit_call_fn(buf, (uintptr_t)JIT_C(jello_object_new));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_helper_trap_check(emit_ctx* ctx) {
  emit_load_vm32(ctx->buf, X64_RCX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test32(ctx->buf, X64_RCX, X64_RCX);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_HELPER); // JNE: trap_code set
}

static void emit_obj_has_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)o);
  jit_reg_state_discard_currents(ctx->ra);
  if(!ctx->m->proto_enabled || ins->imm == JELLO_ATOM___PROTO__) {
    // No prototype chain to follow: `o`'s own slots answer.
    emit_mov64(buf, X64_RDI, (unsigned)o);
    emit_mov32_imm(buf, X64_RSI, ins->imm);
    emit_call_fn(buf, (uintptr_t)JIT_C(jello_object_has));
    emit_mov64(buf, X64_STAGE, X64_RAX);
  } else {
    emit_mov64(buf, X64_RDX, (unsigned)o);
    emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
    emit_mov64_imm(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
    emit_mov32_imm(buf, X64_RCX, ins->imm);
    emit_call_fn(buf, (uintptr_t)jit_obj_chain_has);
    emit_mov64(buf, X64_STAGE, X64_RAX);
    emit_helper_trap_check(ctx);
  }
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov32(buf, (unsigned)dst, X64_STAGE);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static int obj_get_keeps(const emit_ctx* ctx, uint8_t dst) {
  return vm_reg_kind(ctx->m, ctx->f, dst) == JELLO_T_DYNAMIC;
}

static void emit_obj_set_f64_call(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_fload64(buf, 0, (unsigned)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES);
  emit_mov64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov64(buf, X64_RDX, X64_STAGE);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_f64);
  emit_helper_trap_check(ctx);
}

static int obj_atom_links(const emit_ctx* ctx, uint32_t atom) {
  return ctx->m->proto_enabled && atom == JELLO_ATOM___PROTO__;
}

#define OBJ_INLINE_LINKS 8u

static size_t emit_obj_probe(jit_exec_buf* buf, uint32_t atom) {
  emit_load32(buf, X64_R8, X64_STAGE, (uint32_t)offsetof(jello_object, cap));
  emit_alu32_imm(buf, JIT_ALU_SUB, X64_R8, 1u);                   // the mask
  emit_mov32_imm(buf, X64_R9, vm_atom_hash(atom) - 1u); // the loop steps first
  emit_load64(buf, X64_R10, X64_STAGE, (uint32_t)offsetof(jello_object, states));
  emit_load64(buf, X64_R11, X64_STAGE, (uint32_t)offsetof(jello_object, keys));
  size_t loop = buf->len;
  emit_alu32_imm(buf, JIT_ALU_ADD, X64_R9, 1u);
  emit_alu32(buf, JIT_ALU_AND, X64_R9, X64_R8);
  emit_mov64(buf, X64_RDI, X64_R10);
  emit_add64(buf, X64_RDI, X64_R9);
  emit_load8(buf, X64_RDX, X64_RDI, 0);
  emit_test32(buf, X64_RDX, X64_RDX);
  size_t miss = emit_jcc_rel32(buf, JIT_JE, 0);
  emit_cmp32_imm(buf, X64_RDX, JELLO_OBJ_SLOT_TOMB);
  (void)emit_jcc_rel32(buf, JIT_JE, (int32_t)(loop - (buf->len + 6)));
  emit_mov64(buf, X64_RDI, X64_R9);
  emit_shl_r64_imm8(buf, X64_RDI, 2);
  emit_add64(buf, X64_RDI, X64_R11);
  emit_load32(buf, X64_RSI, X64_RDI, 0);
  emit_cmp32_imm(buf, X64_RSI, atom);
  (void)emit_jcc_rel32(buf, JIT_JNE, (int32_t)(loop - (buf->len + 6)));
  return miss;
}

static void emit_obj_probed_value_addr(jit_exec_buf* buf) {
  emit_mov64(buf, X64_RDI, X64_R9);
  emit_shl_r64_imm8(buf, X64_RDI, 3);
  emit_load64(buf, X64_RSI, X64_STAGE, (uint32_t)offsetof(jello_object, vals));
  emit_add64(buf, X64_RDI, X64_RSI);
}

static void emit_obj_probed_state_set(jit_exec_buf* buf, uint8_t state) {
  emit_mov64(buf, X64_RDI, X64_R10);
  emit_add64(buf, X64_RDI, X64_R9);
  emit_store_imm8_to_r64_disp(buf, X64_RDI, 0, state);
}

static void emit_obj_get_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)o);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, X64_STAGE, (unsigned)o);

  int chained = ctx->m->proto_enabled && !obj_atom_links(ctx, ins->imm);
  if(chained) emit_mov32_imm(buf, X64_RCX, OBJ_INLINE_LINKS);
  size_t probe = buf->len;
  size_t miss = emit_obj_probe(buf, ins->imm);
  if(obj_get_keeps(ctx, ins->a)) {
    emit_cmp32_imm(buf, X64_RDX, JELLO_OBJ_SLOT_OWNED);
    size_t plain = emit_jcc_rel8(buf, JIT_JNE);
    emit_obj_probed_state_set(buf, JELLO_OBJ_SLOT_OCCUPIED);
    patch_jcc_rel8_here(buf, plain);
  }
  emit_obj_probed_value_addr(buf);
  emit_load64(buf, X64_STAGE, X64_RDI, 0);
  size_t j_done = emit_jmp_rel32(buf, 0);
  patch_jump_here(buf, miss);
  if(chained) {
    emit_test32(buf, X64_RCX, X64_RCX);
    size_t last = emit_jcc_rel32(buf, JIT_JE, 0);
    emit_load64(buf, X64_RAX, X64_STAGE, (uint32_t)offsetof(jello_object, proto));
    emit_test64(buf, X64_RAX, X64_RAX);
    size_t end = emit_jcc_rel32(buf, JIT_JE, 0); // no prototype
    emit_alu32_imm(buf, JIT_ALU_SUB, X64_RCX, 1u);
    emit_mov64(buf, X64_STAGE, X64_RAX);
    (void)emit_jmp_rel32(buf, (int32_t)(probe - (buf->len + 5)));
    patch_jump_here(buf, last);
    patch_jump_here(buf, end);
  }

  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_PASSTHROUGH); /* JNE: trap_code != NONE */

  emit_current_rf_ptr_into_r11(buf);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_imm64(buf, X64_RDX, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_r64_r64(buf, X64_RCX, X64_R11);
  emit_mov_r32_imm32(buf, X64_R8, ins->a);
  emit_mov_r64_r64(buf, X64_R9, X64_R14);
  emit_call_fn(buf, (void*)vm_store_from_boxed);
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_PASSTHROUGH);

  jello_type_kind dst_kind = vm_reg_kind(ctx->m, ctx->f, ins->a);
  if(dst_kind == JELLO_T_F32) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 0);
  else if(dst_kind == JELLO_T_F64) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 1);
  else (void)jit_reg_get_gpr(ctx->ra, ins->a, 1);
}

static void emit_obj_set_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_R14, (unsigned)o);
  emit_current_rf_ptr_into_r11(buf);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_imm64(buf, X64_RDX, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_r64_r64(buf, X64_RCX, X64_R11);
  emit_mov_r32_imm32(buf, X64_R8, ins->a);
  emit_call_fn(buf, (void*)vm_box_from_typed);

  emit_mov_r64_imm64(buf, X64_RDI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_r64(buf, X64_RSI, X64_R14);
  emit_mov_r32_imm32(buf, X64_RDX, ins->imm);
  emit_mov_r64_r64(buf, X64_RCX, X64_RAX);
  emit_call_fn(buf, (void*)jit_obj_set_direct);
}

static void emit_obj_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_r32_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS); /* JAE: atom_id >= natoms */
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_R14, (unsigned)o);
  emit_mov_r32_r32(buf, X64_R9, (unsigned)atom);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_r64(buf, X64_RDX, X64_R14);
  emit_mov_r32_r32(buf, X64_RCX, X64_R9);
  emit_call_fn(buf, (void*)jit_obj_chain_get);
  emit_mov_r64_r64(buf, X64_R14, X64_RAX);

  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_PASSTHROUGH);

  emit_current_rf_ptr_into_r11(buf);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_imm64(buf, X64_RDX, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_r64_r64(buf, X64_RCX, X64_R11);
  emit_mov_r32_imm32(buf, X64_R8, ins->a);
  emit_mov_r64_r64(buf, X64_R9, X64_R14);
  emit_call_fn(buf, (void*)vm_store_from_boxed);
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_PASSTHROUGH);

  jello_type_kind dst_kind = vm_reg_kind(ctx->m, ctx->f, ins->a);
  if(dst_kind == JELLO_T_F32) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 0);
  else if(dst_kind == JELLO_T_F64) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 1);
  else (void)jit_reg_get_gpr(ctx->ra, ins->a, 1);
}

static void emit_obj_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_r32_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_R14, (unsigned)o);
  emit_rsp_adjust_local(buf, 0, 16); /* sub rsp, 16 */
  emit_mov_r32_r32(buf, X64_R9, (unsigned)atom);
  emit_store_r64_to_mem_rsp(buf, X64_R9, 0);
  emit_current_rf_ptr_into_r11(buf);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_imm64(buf, X64_RSI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_imm64(buf, X64_RDX, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_r64_r64(buf, X64_RCX, X64_R11);
  emit_mov_r32_imm32(buf, X64_R8, ins->a);
  emit_call_fn(buf, (void*)vm_box_from_typed);
  emit_mov_r64_from_mem_rsp(buf, X64_RDX, 0);
  emit_rsp_adjust_local(buf, 1, 16); /* add rsp, 16 */

  emit_mov_r64_imm64(buf, X64_RDI, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_r64_r64(buf, X64_RSI, X64_R14);
  emit_mov_r64_r64(buf, X64_RCX, X64_RAX);
  emit_call_fn(buf, (void*)jit_obj_set_direct);
}

static void emit_obj_has(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_r32_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  emit_mov_r64_r64(buf, X64_RAX, (unsigned)o);
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)atom);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, X64_RAX);
  emit_mov_r32_r32(buf, X64_RSI, X64_RCX);
  emit_call_fn(buf, (void*)jello_object_has);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_remove(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_r32_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  emit_mov_r64_r64(buf, X64_RAX, (unsigned)o);
  emit_mov_r32_r32(buf, X64_RCX, (unsigned)atom);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, X64_RAX);
  emit_mov_r32_r32(buf, X64_RSI, X64_RCX);
  emit_call_fn(buf, (void*)jello_object_remove);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r32_r32(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_clear(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  emit_mov_r64_r64(buf, X64_RAX, (unsigned)o);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, X64_RAX);
  emit_call_fn(buf, (void*)jello_object_clear);
}

static void emit_obj_copy(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_test_r64_r64(buf, (unsigned)src, (unsigned)src);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  emit_mov_r64_r64(buf, X64_RAX, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_r64(buf, X64_RSI, X64_RAX);
  emit_call_fn(buf, (void*)jello_object_copy);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_r64_r64(buf, (unsigned)dst, X64_RAX);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static int emit_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jit_ir_op)ins->op) {
    case JIR_NOP:
      break;
    case JIR_MOV: {
      jello_type_kind dk = vm_reg_kind(ctx->m, ctx->f, ins->a);
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      if(jello_slot_size(dk) == 8u) {
        emit_mov_r64_r64(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      } else {
        emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      }
      break;
    }
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_I64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_imm64(ctx->buf, (unsigned)d, (uint64_t)ctx->m->const_i64[ins->imm]);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F16: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm & 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      uint64_t bits;
      memcpy(&bits, &ctx->m->const_f64[ins->imm], 8);
      emit_mov_r64_imm64(ctx->buf, (unsigned)d, bits);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_BOOL: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm ? 1u : 0u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_BIN_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu_r32_r32(ctx->buf, 0x01, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu_r32_r32(ctx->buf, 0x29, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_imul_r32_r32(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu_r32_r32(ctx->buf, 0x21, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu_r32_r32(ctx->buf, 0x09, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu_r32_r32(ctx->buf, 0x31, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_DIV: case JIR_BIN_MOD: {
          emit_divmod_guard_i32(ctx, (unsigned)rhs, (unsigned)lhs);
          jit_reg_unbind_preg(ctx->ra, X64_RAX);
          jit_reg_unbind_preg(ctx->ra, X64_RDX);
          emit_mov_r32_r32(ctx->buf, X64_RAX, (unsigned)lhs);
          emit_cdq(ctx->buf);
          emit_idiv_r32(ctx->buf, (unsigned)rhs);
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD) emit_mov_r32_r32(ctx->buf, (unsigned)dst, X64_RDX);
          else emit_mov_r32_r32(ctx->buf, (unsigned)dst, X64_RAX);
          break;
        }
        case JIR_BIN_SHL: case JIR_BIN_SHR: {
          int cnt = jit_reg_get_gpr(ctx->ra, ins->c, 1);
          emit_mov_r32_r32(ctx->buf, X64_RCX, (unsigned)cnt);
          emit_alu_r32_imm8(ctx->buf, 0x04, X64_RCX, (ins->imm == JIR_BIN_SHL) ? 31 : 31);
          if((jello_jir_bin)ins->imm == JIR_BIN_SHL) emit_shl_cl_r32(ctx->buf, (unsigned)dst);
          else emit_shr_cl_r32(ctx->buf, (unsigned)dst);
          break;
        }
        default: return 0;
      }
      jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
      break;
    }
    case JIR_BIN_I64: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_r64(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu_r64_r64(ctx->buf, 0x01, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu_r64_r64(ctx->buf, 0x29, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_imul_r64_r64(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu_r64_r64(ctx->buf, 0x21, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu_r64_r64(ctx->buf, 0x09, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu_r64_r64(ctx->buf, 0x31, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_DIV: case JIR_BIN_MOD: {
          emit_divmod_guard_i64(ctx, (unsigned)rhs, (unsigned)lhs);
          emit_mov_r64_r64(ctx->buf, X64_RAX, (unsigned)lhs);
          emit_cqo(ctx->buf);
          emit_idiv_r64(ctx->buf, (unsigned)rhs);
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD) emit_mov_r64_r64(ctx->buf, (unsigned)dst, X64_RDX);
          else emit_mov_r64_r64(ctx->buf, (unsigned)dst, X64_RAX);
          break;
        }
        case JIR_BIN_SHL: case JIR_BIN_SHR: {
          int cnt = jit_reg_get_gpr(ctx->ra, ins->c, 1);
          emit_mov_r32_r32(ctx->buf, X64_RCX, (unsigned)cnt);
          emit_alu_r32_imm8(ctx->buf, 0x04, X64_RCX, 63);
          if((jello_jir_bin)ins->imm == JIR_BIN_SHL) emit_shl_cl_r64(ctx->buf, (unsigned)dst);
          else emit_shr_cl_r64(ctx->buf, (unsigned)dst);
          break;
        }
        default: return 0;
      }
      jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
      break;
    }
    case JIR_BIN_F16:
      if((jello_jir_bin)ins->imm == JIR_BIN_ADD) emit_f16_binop(ctx, ins, 0x58);
      else if((jello_jir_bin)ins->imm == JIR_BIN_SUB) emit_f16_binop(ctx, ins, 0x5C);
      else if((jello_jir_bin)ins->imm == JIR_BIN_MUL) emit_f16_binop(ctx, ins, 0x59);
      else return 0;
      break;
    case JIR_BIN_F32:
      emit_bin_f32(ctx, ins);
      break;
    case JIR_BIN_F64:
      emit_bin_f64(ctx, ins);
      break;
    case JIR_ADD_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_r32_imm32(ctx->buf, 0x00, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_SUB_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_r32_imm32(ctx->buf, 0x05, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_MUL_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_imul_r32_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg_r32(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_r64(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg_r64(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_xor_r32_imm32(ctx->buf, (unsigned)d, 0x80000000u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_r64(ctx->buf, (unsigned)d, (unsigned)s);
      emit_mov_r64_imm64(ctx->buf, X64_RAX, 0x8000000000000000ULL);
      emit_alu_r64_r64(ctx->buf, 0x31, (unsigned)d, X64_RAX);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NOT_BOOL: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_xor_r32_imm32(ctx->buf, (unsigned)d, 1u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_r32_r32(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x9C, (unsigned)d);
      emit_movzx_r32_r8(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_EQ_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_r32_imm32(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc(ctx->buf, 0x94, (unsigned)d);
      emit_movzx_r32_r8(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I64: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_r64_r64(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x9C, (unsigned)d);
      emit_movzx_r32_r8(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_LT_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_r32_imm32(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc(ctx->buf, 0x9C, (unsigned)d);
      emit_movzx_r32_r8(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_F32:
      emit_cmp_float(ctx, ins, 0);
      break;
    case JIR_CMP_F64:
      emit_cmp_float(ctx, ins, 1);
      break;
    case JIR_CONV:
      if(!emit_conv(ctx, ins)) return 0;
      break;
    case JIR_BYTES_NEW:
      emit_bytes_new(ctx, ins);
      break;
    case JIR_BYTES_LEN:
      emit_bytes_len(ctx, ins);
      break;
    case JIR_BYTES_GET_U8:
      emit_bytes_get_u8(ctx, ins);
      break;
    case JIR_BYTES_SET_U8:
      emit_bytes_set_u8(ctx, ins);
      break;
    case JIR_BYTES_EQ:
      emit_bytes_eq(ctx, ins);
      break;
    case JIR_BYTES_CONCAT2:
      emit_bytes_concat2(ctx, ins);
      break;
    case JIR_BYTES_BITOP:
      emit_bytes_bitop(ctx, ins);
      break;
    case JIR_BYTES_READ:
      emit_bytes_read(ctx, ins);
      break;
    case JIR_BYTES_WRITE:
      emit_bytes_write(ctx, ins);
      break;
    case JIR_BYTES_BLIT:
      emit_bytes_blit(ctx, ins);
      break;
    case JIR_BITS_WRITE:
      emit_bits_write(ctx, ins);
      break;
    case JIR_BITS_READ:
      emit_bits_read(ctx, ins);
      break;
    case JIR_STRUCT_NEW:
      emit_struct_new(ctx, ins);
      break;
    case JIR_STRUCT_GET:
      emit_struct_get(ctx, ins);
      break;
    case JIR_STRUCT_SET:
      emit_struct_set(ctx, ins);
      break;
    case JIR_CONST_NULL: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r64_imm64(ctx->buf, (unsigned)d, (uint64_t)JELLO_TAG_NULL);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_ATOM: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_TO_DYN:
      emit_to_dyn(ctx, ins);
      break;
    case JIR_FROM_DYN:
      emit_from_dyn(ctx, ins);
      break;
    case JIR_KINDOF:
      emit_kindof(ctx, ins);
      break;
    case JIR_PHYSEQ:
      emit_physeq(ctx, ins);
      break;
    case JIR_OBJ_NEW:
      emit_obj_new(ctx, ins);
      break;
    case JIR_OBJ_HAS_ATOM:
      emit_obj_has_atom(ctx, ins);
      break;
    case JIR_OBJ_GET_ATOM:
      emit_obj_get_atom(ctx, ins);
      break;
    case JIR_OBJ_SET_ATOM:
      emit_obj_set_atom(ctx, ins);
      break;
    case JIR_OBJ_GET:
      emit_obj_get(ctx, ins);
      break;
    case JIR_OBJ_SET:
      emit_obj_set(ctx, ins);
      break;
    case JIR_OBJ_HAS:
      emit_obj_has(ctx, ins);
      break;
    case JIR_OBJ_REMOVE:
      emit_obj_remove(ctx, ins);
      break;
    case JIR_OBJ_CLEAR:
      emit_obj_clear(ctx, ins);
      break;
    case JIR_OBJ_COPY:
      emit_obj_copy(ctx, ins);
      break;
    case JIR_RET:
      break;
    default:
      return 0;
  }
  return 1;
}

static size_t emit_buf_cap(uint32_t ninsns, int allow_calls) {
  size_t per = allow_calls ? JIT_EMIT_CALL_INSN_BUDGET : JIT_EMIT_PER_INSN_BUDGET;
  size_t cap = 256u + (size_t)ninsns * per + 512u;
  if(cap < JIT_EMIT_BUF_MIN_CAP) cap = JIT_EMIT_BUF_MIN_CAP;
  return cap;
}

int jello_jit_emit_function_into(const jello_bc_module* m, uint32_t fi, jit_exec_buf* buf,
    jello_jit_func_code* out_fc, jello_jit_call_patch** patches, uint32_t* npatch, uint32_t* patch_cap,
    int allow_calls) {
  if(fi >= m->nfuncs) return 0;
  const jello_bc_function* f = &m->funcs[fi];

  jello_jit_ir_function* ir = jello_jit_ir_build(m, fi);
  if(!ir) return 0;
  if(!jello_jit_ir_verify(ir, f)) { jello_jit_ir_free(ir); return 0; }

  jello_type_kind ret_kind = JELLO_T_VOID;
  if(!ir_shape_ok(ir, m, f, fi, &ret_kind, allow_calls)) { jello_jit_ir_free(ir); return 0; }

  uint8_t* is_target = (uint8_t*)calloc(f->ninsns, 1);
  uint8_t* discarded = (uint8_t*)calloc(f->ninsns, 1);
  size_t* native_off = (size_t*)malloc(sizeof(size_t) * (size_t)f->ninsns);
  uint8_t* is_loop_anchor = (uint8_t*)calloc(f->ninsns, 1);
  if(!is_target || !discarded || !native_off || !is_loop_anchor) {
    free(is_target); free(discarded); free(native_off); free(is_loop_anchor);
    jello_jit_ir_free(ir);
    return 0;
  }
  for(uint32_t i = 0; i < f->ninsns; i++) native_off[i] = (size_t)-1;

  uint32_t nloop_anchors = 0;
  for(uint32_t i = 0; i < ir->ninsns; i++) {
    jello_jit_ir_op op = (jello_jit_ir_op)ir->insns[i].op;
    if(op != JIR_JMP && op != JIR_JMP_IF) continue;
    uint32_t tgt = 0;
    if(!resolve_target_bc(ir->insns[i].bc_pc, (int32_t)ir->insns[i].imm, f->ninsns, &tgt)) {
      free(is_target); free(discarded); free(native_off); free(is_loop_anchor);
      jello_jit_ir_free(ir);
      return 0;
    }
    is_target[tgt] = 1;
    if(tgt <= ir->insns[i].bc_pc && !is_loop_anchor[tgt]) {
      is_loop_anchor[tgt] = 1;
      nloop_anchors++;
    }
  }

  jit_survive_plan splan;
  if(!jit_survive_plan_build(ir, m, f, is_target, &splan)) {
    free(is_target); free(discarded); free(native_off); free(is_loop_anchor);
    jello_jit_ir_free(ir);
    return 0;
  }

  size_t blob_start = buf->len;
  jit_trap_jump trap_jumps[256];
  jit_branch_patch branches[256];
  size_t ret_jumps[64];
  size_t caller_ret_jumps[64];
  size_t stack_overflow_jumps[64];
  emit_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.buf = buf;
  ctx.m = m;
  ctx.f = f;
  ctx.fi = fi;
  ctx.allow_calls = allow_calls;
  ctx.trap_jumps = trap_jumps;
  ctx.trap_jump_cap = 256;
  ctx.branches = branches;
  ctx.branch_cap = 256;
  ctx.ret_jumps = ret_jumps;
  ctx.ret_jump_cap = 64;
  ctx.caller_ret_jumps = caller_ret_jumps;
  ctx.caller_ret_jump_cap = 64;
  ctx.stack_overflow_jumps = stack_overflow_jumps;
  ctx.stack_overflow_jump_cap = 64;
  ctx.call_patches = patches;
  ctx.call_patch_count = npatch;
  ctx.call_patch_cap = patch_cap ? *patch_cap : 0;
  ctx.ret_kind = ret_kind;
  ctx.survive_any_armed = splan.any_armed;

  jit_reg_state ra;
  jit_reg_emit_ctx re;
  re.buf = buf;
  jit_reg_state_init(&ra, ir->nregs, re);
  ctx.ra = &ra;

  size_t body_off = 0;
  emit_prologue(buf, &body_off, splan.any_armed);

  int ok = 1;
  for(uint32_t i = 0; i < ir->ninsns && ok; i++) {
    const jello_jit_ir_insn* ins = &ir->insns[i];
    uint32_t bc = ins->bc_pc;
    ctx.cur_bc_pc = bc;
    if(bc < f->ninsns && is_target[bc] && !discarded[bc]) {
      jit_reg_state_discard_currents(&ra);
      discarded[bc] = 1;
    }
    if(bc < f->ninsns && native_off[bc] == (size_t)-1) native_off[bc] = buf->len;

    jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
    if(op == JIR_RET) {
      emit_ret_insn(&ctx, ins->a);
      goto insn_done;
    }
    if(op == JIR_JMP) {
      uint32_t tgt = 0;
      if(!resolve_target_bc(bc, (int32_t)ins->imm, f->ninsns, &tgt)) { ok = 0; break; }
      size_t at = emit_jmp_rel32(buf, 0);
      if(!push_branch(&ctx, at, tgt, 0)) ok = 0;
      goto insn_done;
    }
    if(op == JIR_JMP_IF) {
      uint32_t tgt = 0;
      if(!resolve_target_bc(bc, (int32_t)ins->imm, f->ninsns, &tgt)) { ok = 0; break; }
      int cond = jit_reg_get_gpr(&ra, ins->a, 1);
      emit_test_r32_r32(buf, (unsigned)cond, (unsigned)cond);
      size_t at = emit_jcc_rel32(buf, 0x85, 0);
      if(!push_branch(&ctx, at, tgt, 1)) ok = 0;
      goto insn_done;
    }
    if(op == JIR_SAFEPOINT) {
      emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
      emit_call_fn(buf, (void*)vm_safepoint_check);
      jit_reg_state_clobber_call(&ra);
      emit_mov_r64_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, fuel_limit));
      emit_test_r64_r64(buf, X64_RAX, X64_RAX);
      size_t fuel_skip = emit_jcc_rel32(buf, 0x84, 0); /* JE: fuel_limit == 0, disabled */
      emit_mov_r64_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, fuel_remaining));
      emit_test_r64_r64(buf, X64_RAX, X64_RAX);
      emit_jcc_trap(&ctx, 0x84, JIT_TRAP_FUEL); /* JE: fuel_remaining == 0 -- trap-PC captured centrally, Sec 4.3 */
      emit_alu_r64_imm32(buf, 0x05 /* SUB */, X64_RAX, 1);
      emit_store_r64_to_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, fuel_remaining));
      patch_jcc_rel32(buf, fuel_skip, (int32_t)(buf->len - (fuel_skip + 6)));
      goto insn_done;
    }
    if(allow_calls && (op == JIR_CALL_SELF || op == JIR_CALL_DIRECT)) {
      if(splan.armed[i]) jit_reg_survive_hold(&ra, splan.candidate[i]);
      ok = emit_call_insn(&ctx, ins, op);
      goto insn_done;
    }
    ok = emit_insn(&ctx, ins);
  insn_done:
    if(splan.is_consumer[i] && jit_reg_survive_is_held(&ra)) jit_reg_survive_release(&ra);
    jit_reg_unpin_all(&ra);
  }

  size_t epilogue_ok_off = 0;
  size_t stack_overflow_off = 0;
  size_t trap_off_by_kind[JIT_TRAP_KIND_COUNT];
  if(ok) {
    epilogue_ok_off = buf->len;
    emit_epilogue_ok_tail(&ctx);
    size_t caller_ret_off = buf->len;
    emit_caller_ret_tail(&ctx);
    for(size_t i = 0; i < ctx.caller_ret_jump_count; i++) {
      size_t at = ctx.caller_ret_jumps[i];
      patch_jcc_rel32(buf, at, (int32_t)(caller_ret_off - (at + 6)));
    }
    trap_off_by_kind[JIT_TRAP_ARITH] = buf->len;
    emit_epilogue_trap_generic(&ctx, (void*)jit_trap_arithmetic);
    trap_off_by_kind[JIT_TRAP_NULL] = buf->len;
    emit_epilogue_trap_generic(&ctx, (void*)jit_trap_null_deref);
    trap_off_by_kind[JIT_TRAP_BOUNDS] = buf->len;
    emit_epilogue_trap_generic(&ctx, (void*)jit_trap_bounds);
    trap_off_by_kind[JIT_TRAP_TYPE] = buf->len;
    emit_epilogue_trap_generic(&ctx, (void*)jit_trap_type_mismatch);
    trap_off_by_kind[JIT_TRAP_PASSTHROUGH] = buf->len;
    emit_epilogue_trap_generic(&ctx, NULL);
    trap_off_by_kind[JIT_TRAP_FUEL] = buf->len;
    emit_epilogue_trap_generic(&ctx, (void*)jit_trap_fuel);
    trap_off_by_kind[JIT_TRAP_LIMIT] = buf->len;
    emit_epilogue_trap_generic(&ctx, (void*)jit_trap_limit);
    stack_overflow_off = buf->len;
    emit_epilogue_stack_overflow(&ctx);
    patch_trap_jumps(&ctx, trap_off_by_kind);
    patch_stack_overflow_jumps(&ctx, stack_overflow_off);
    for(size_t i = 0; i < ctx.ret_jump_count; i++) {
      size_t at = ctx.ret_jumps[i];
      patch_jmp_rel32(buf, at, (int32_t)(epilogue_ok_off - (at + 5)));
    }
    for(size_t i = 0; i < ctx.branch_count; i++) {
      size_t at = ctx.branches[i].at;
      uint32_t tgt = ctx.branches[i].bc_tgt;
      if(tgt >= f->ninsns || native_off[tgt] == (size_t)-1) { ok = 0; break; }
      size_t tgt_off = native_off[tgt];
      if(ctx.branches[i].is_jcc)
        patch_jcc_rel32(buf, at, (int32_t)(tgt_off - (at + 6)));
      else
        patch_jmp_rel32(buf, at, (int32_t)(tgt_off - (at + 5)));
    }
  }

  if(ok && nloop_anchors > 0) {
    out_fc->loop_anchor_pcs = (uint32_t*)malloc(sizeof(uint32_t) * nloop_anchors);
    out_fc->loop_anchor_offs = (size_t*)malloc(sizeof(size_t) * nloop_anchors);
    if(!out_fc->loop_anchor_pcs || !out_fc->loop_anchor_offs) {
      free(out_fc->loop_anchor_pcs);
      free(out_fc->loop_anchor_offs);
      out_fc->loop_anchor_pcs = NULL;
      out_fc->loop_anchor_offs = NULL;
      ok = 0;
    } else {
      uint32_t w = 0;
      for(uint32_t pc = 0; pc < f->ninsns; pc++) {
        if(!is_loop_anchor[pc]) continue;
        out_fc->loop_anchor_pcs[w] = pc;
        out_fc->loop_anchor_offs[w] = native_off[pc] - blob_start;
        w++;
      }
      out_fc->nloop_anchors = w;
    }
  } else {
    out_fc->loop_anchor_pcs = NULL;
    out_fc->loop_anchor_offs = NULL;
    out_fc->nloop_anchors = 0;
  }

  jit_reg_state_free(&ra);
  jit_survive_plan_free(&splan);
  jello_jit_ir_free(ir);
  free(is_target);
  free(discarded);
  free(native_off);
  free(is_loop_anchor);

  if(!ok) {
    free(out_fc->loop_anchor_pcs);
    free(out_fc->loop_anchor_offs);
    out_fc->loop_anchor_pcs = NULL;
    out_fc->loop_anchor_offs = NULL;
    out_fc->nloop_anchors = 0;
    buf->len = blob_start;
    return 0;
  }

  out_fc->blob_off = blob_start;
  out_fc->entry_off = 0;
  out_fc->body_off = body_off - blob_start;
  out_fc->size = buf->len - blob_start;
  return 1;
}

int jello_jit_compile_function(const jello_bc_module* m, uint32_t fi, jello_jit_code* out_code) {
  out_code->buf.mem = NULL;
  out_code->buf.cap = 0;
  out_code->buf.len = 0;
  out_code->buf.finalized = 0;
  out_code->buf.patch_open = 0;
  out_code->entry_off = 0;
  out_code->body_off = 0;
  out_code->loop_anchor_pcs = NULL;
  out_code->loop_anchor_offs = NULL;
  out_code->nloop_anchors = 0;

  if(fi >= m->nfuncs) return 0;
  jello_jit_ir_function* ir = jello_jit_ir_build(m, fi);
  if(!ir) return 0;
  uint32_t ninsns = ir->ninsns;
  jello_jit_ir_free(ir);

  jit_exec_buf buf = jit_alloc_exec_buf(emit_buf_cap(ninsns, 0));
  if(!buf.mem) return 0;

  jello_jit_func_code fc;
  memset(&fc, 0, sizeof(fc));
  if(!jello_jit_emit_function_into(m, fi, &buf, &fc, NULL, NULL, NULL, 0)) {
    jit_free_exec_buf(&buf);
    return 0;
  }

  if(!jit_finalize_exec_buf(&buf)) {
    free(fc.loop_anchor_pcs);
    free(fc.loop_anchor_offs);
    jit_free_exec_buf(&buf);
    return 0;
  }

  out_code->buf = buf;
  out_code->entry_off = fc.entry_off;
  out_code->body_off = fc.body_off;
  out_code->loop_anchor_pcs = fc.loop_anchor_pcs;
  out_code->loop_anchor_offs = fc.loop_anchor_offs;
  out_code->nloop_anchors = fc.nloop_anchors;
  return 1;
}

void jello_jit_patch_call_rel32(jit_exec_buf* buf, size_t at, int32_t rel) {
  patch_call_rel32(buf, at, rel);
}

void jello_jit_patch_call(jit_exec_buf* buf, size_t site_off, size_t callee_off) {
  patch_call_rel32(buf, site_off, (int32_t)(callee_off - (site_off + 5)));
}

void jello_jit_code_free(jello_jit_code* code) {
  free(code->loop_anchor_pcs);
  free(code->loop_anchor_offs);
  code->loop_anchor_pcs = NULL;
  code->loop_anchor_offs = NULL;
  code->nloop_anchors = 0;
  jit_free_exec_buf(&code->buf);
}
