// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
#include <jello.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* -- x86-64 byte encoding -- */

enum {
  X64_RAX = 0, X64_RCX = 1, X64_RDX = 2, X64_RBX = 3,
  X64_RSP = 4, X64_RBP = 5, X64_RSI = 6, X64_RDI = 7,
  X64_R8 = 8, X64_R9 = 9, X64_R10 = 10, X64_R11 = 11, X64_R14 = 14,
};

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

static void emit_alu_r32_imm8(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint8_t imm) {
  uint8_t bytes[4];
  size_t n = 0;
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x83;
  bytes[n++] = (uint8_t)(0xC0u | ((op_ext & 7u) << 3) | (dst & 7u));
  bytes[n++] = imm;
  jit_exec_buf_write(buf, bytes, n);
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

static void emit_ret(jit_exec_buf* buf) {
  uint8_t byte = 0xC3;
  jit_exec_buf_write(buf, &byte, 1);
}

static uint64_t fn_addr_bits_fn(void* fn) {
  uint64_t bits;
  memcpy(&bits, &fn, sizeof(bits));
  return bits;
}

static void emit_call_fn(jit_exec_buf* buf, void* fn);
static void emit_imul_r64_r64(jit_exec_buf* buf, unsigned dst, unsigned src);
static void emit_imul_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm);

static void emit_call_fn(jit_exec_buf* buf, void* fn) {
  emit_movabs_rax(buf, fn_addr_bits_fn(fn));
  emit_call_rax(buf);
}

static size_t emit_call_rel32(jit_exec_buf* buf, int32_t rel) {
  size_t at = buf->len;
  uint8_t bytes[5];
  bytes[0] = 0xE8;
  memcpy(&bytes[1], &rel, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
  return at;
}

static void patch_call_rel32(jit_exec_buf* buf, size_t at, int32_t rel) {
  memcpy(buf->mem + at + 1, &rel, 4);
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


typedef struct jit_branch_patch {
  size_t at;
  uint32_t bc_tgt;
  uint8_t is_jcc;
} jit_branch_patch;

typedef struct emit_ctx {
  jit_exec_buf* buf;
  jit_reg_state* ra;
  const jello_bc_module* m;
  const jello_bc_function* f;
  uint32_t fi;
  int allow_calls;
  size_t* trap_jumps;
  size_t trap_jump_cap;
  size_t trap_jump_count;
  size_t* trap_passthrough_jumps;
  size_t trap_passthrough_jump_cap;
  size_t trap_passthrough_jump_count;
  jit_branch_patch* branches;
  size_t branch_cap;
  size_t branch_count;
  size_t* ret_jumps;
  size_t ret_jump_cap;
  size_t ret_jump_count;
  size_t* caller_ret_jumps;
  size_t caller_ret_jump_cap;
  size_t caller_ret_jump_count;
  size_t* stack_overflow_jumps;
  size_t stack_overflow_jump_cap;
  size_t stack_overflow_jump_count;
  jello_jit_call_patch** call_patches;
  uint32_t* call_patch_count;
  uint32_t call_patch_cap;
  jello_type_kind ret_kind;
} emit_ctx;

static void emit_jmp_stack_overflow(emit_ctx* ctx) {
  size_t at = emit_jmp_rel32(ctx->buf, 0);
  if(ctx->stack_overflow_jump_count < ctx->stack_overflow_jump_cap)
    ctx->stack_overflow_jumps[ctx->stack_overflow_jump_count++] = at;
}

static int push_call_patch(emit_ctx* ctx, uint32_t callee_fi, size_t site_off) {
  if(!ctx->call_patches || !ctx->call_patch_count) return 1;
  if(*ctx->call_patch_count >= ctx->call_patch_cap) {
    uint32_t new_cap = ctx->call_patch_cap ? ctx->call_patch_cap * 2u : 16u;
    jello_jit_call_patch* p = (jello_jit_call_patch*)realloc(*ctx->call_patches,
        (size_t)new_cap * sizeof(jello_jit_call_patch));
    if(!p) return 0;
    *ctx->call_patches = p;
    ctx->call_patch_cap = new_cap;
  }
  jello_jit_call_patch* patch = &(*ctx->call_patches)[*ctx->call_patch_count];
  patch->caller_fi = ctx->fi;
  patch->callee_fi = callee_fi;
  patch->site_off = site_off;
  (*ctx->call_patch_count)++;
  return 1;
}

static void patch_stack_overflow_jumps(emit_ctx* ctx, size_t so_off) {
  for(size_t i = 0; i < ctx->stack_overflow_jump_count; i++) {
    size_t at = ctx->stack_overflow_jumps[i];
    patch_jmp_rel32(ctx->buf, at, (int32_t)(so_off - (at + 5)));
  }
}

static void emit_jcc_trap(emit_ctx* ctx, uint8_t cc) {
  size_t at = emit_jcc_rel32(ctx->buf, cc, 0);
  if(ctx->trap_jump_count < ctx->trap_jump_cap) ctx->trap_jumps[ctx->trap_jump_count++] = at;
}

static void patch_trap_jumps(emit_ctx* ctx, size_t trap_off) {
  for(size_t i = 0; i < ctx->trap_jump_count; i++) {
    size_t at = ctx->trap_jumps[i];
    uint8_t op = ctx->buf->mem[at];
    if(op == 0xE9) patch_jmp_rel32(ctx->buf, at, (int32_t)(trap_off - (at + 5)));
    else patch_jcc_rel32(ctx->buf, at, (int32_t)(trap_off - (at + 6)));
  }
}

static void emit_jcc_trap_passthrough(emit_ctx* ctx, uint8_t cc) {
  size_t at = emit_jcc_rel32(ctx->buf, cc, 0);
  if(ctx->trap_passthrough_jump_count < ctx->trap_passthrough_jump_cap)
    ctx->trap_passthrough_jumps[ctx->trap_passthrough_jump_count++] = at;
}

static void patch_trap_passthrough_jumps(emit_ctx* ctx, size_t trap_off) {
  for(size_t i = 0; i < ctx->trap_passthrough_jump_count; i++) {
    size_t at = ctx->trap_passthrough_jumps[i];
    uint8_t op = ctx->buf->mem[at];
    if(op == 0xE9) patch_jmp_rel32(ctx->buf, at, (int32_t)(trap_off - (at + 5)));
    else patch_jcc_rel32(ctx->buf, at, (int32_t)(trap_off - (at + 6)));
  }
}

static int push_branch(emit_ctx* ctx, size_t at, uint32_t bc_tgt, uint8_t is_jcc) {
  if(ctx->branch_count >= ctx->branch_cap) return 0;
  ctx->branches[ctx->branch_count].at = at;
  ctx->branches[ctx->branch_count].bc_tgt = bc_tgt;
  ctx->branches[ctx->branch_count].is_jcc = is_jcc;
  ctx->branch_count++;
  return 1;
}

static int push_ret_jump(emit_ctx* ctx, size_t at) {
  if(ctx->ret_jump_count >= ctx->ret_jump_cap) return 0;
  ctx->ret_jumps[ctx->ret_jump_count++] = at;
  return 1;
}

static int push_caller_ret_jump(emit_ctx* ctx, size_t at) {
  if(ctx->caller_ret_jump_count >= ctx->caller_ret_jump_cap) return 0;
  ctx->caller_ret_jumps[ctx->caller_ret_jump_count++] = at;
  return 1;
}

static int resolve_target_bc(uint32_t bc_pc, int32_t imm, uint32_t ninsns, uint32_t* out) {
  int64_t t = (int64_t)(int32_t)bc_pc + 1 + (int64_t)imm;
  if(t < 0 || t >= (int64_t)ninsns) return 0;
  *out = (uint32_t)t;
  return 1;
}

static void emit_divmod_guard_i32(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_test_r32_r32(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, 0x84);
  emit_cmp_r32_imm32(ctx->buf, div_gpr, (uint32_t)-1);
  size_t jne = ctx->buf->len;
  emit_jcc_rel32(ctx->buf, 0x85, 0);
  emit_cmp_r32_imm32(ctx->buf, lhs_gpr, (uint32_t)INT32_MIN);
  emit_jcc_trap(ctx, 0x84);
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

static void emit_cmp_r64_imm8(jit_exec_buf* buf, unsigned reg, int8_t imm) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((reg >= 8) ? 0x01u : 0));
  bytes[1] = 0x83;
  bytes[2] = (uint8_t)(0xF8u | (reg & 7u));
  bytes[3] = (uint8_t)imm;
  jit_exec_buf_write(buf, bytes, 4);
}

static void emit_divmod_guard_i64(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_test_r64_r64(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, 0x84);
  emit_cmp_r64_imm8(ctx->buf, div_gpr, (int8_t)-1);
  size_t jne = ctx->buf->len;
  emit_jcc_rel32(ctx->buf, 0x85, 0);
  emit_mov_r64_imm64(ctx->buf, X64_RAX, (uint64_t)INT64_MIN);
  emit_cmp_r64_r64(ctx->buf, lhs_gpr, X64_RAX);
  emit_jcc_trap(ctx, 0x84);
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
      return 1;
    case JIR_CALL_SELF:
    case JIR_CALL_DIRECT:
      return allow_calls;
    default:
      return 0;
  }
}

static int callee_has_pointer(const jello_bc_module* m, const jello_bc_function* f) {
  for(uint32_t r = 0; r < f->nregs; r++) {
    jello_type_kind k = vm_reg_kind(m, f, r);
    if(k == JELLO_T_BYTES || k == JELLO_T_STRUCT || k == JELLO_T_OBJECT || k == JELLO_T_DYNAMIC)
      return 1;
  }
  return 0;
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
      if(callee_has_pointer(m, &m->funcs[callee])) return 0;
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

static void emit_prologue(jit_exec_buf* buf, size_t* out_body_off) {
  emit_push_r64(buf, (unsigned)JIT_REG_VM);
  emit_push_r64(buf, (unsigned)JIT_REG_BASE);
  emit_push_r64(buf, (unsigned)JIT_REG_OUT);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_VM, X64_RDI);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_BASE, X64_RSI);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_OUT, X64_RDX);
  *out_body_off = buf->len;
}

static void emit_restore_callee_saved(jit_exec_buf* buf) {
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
  }
}

static void emit_epilogue_ok_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_restore_callee_saved(buf);
  emit_mov_eax_imm32(buf, (uint32_t)JELLO_EXEC_OK);
  emit_ret(buf);
}

static void emit_trap_tail_x64(emit_ctx* ctx);

static void emit_epilogue_trap(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)jit_trap_arithmetic);
  emit_trap_tail_x64(ctx);
}

static void emit_epilogue_stack_overflow(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)jit_trap_stack_overflow);
  emit_trap_tail_x64(ctx);
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

static void emit_trap_tail_x64(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_R8, X64_RAX);
  emit_imul_r64_r64(buf, X64_R8, X64_RDX);
  emit_add_r64_r64(buf, X64_R8, X64_RCX);
  emit_movzx_r32_m8_r64_disp(buf, X64_RDX, X64_R8, (uint32_t)offsetof(call_frame, has_caller));
  emit_test_r32_r32(buf, X64_RDX, X64_RDX);
  size_t j_boundary = emit_jcc_rel32(buf, 0x84, 0);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_reload_jit_reg_base_x64(buf);
  emit_mov_eax_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  emit_ret(buf);

  size_t boundary = buf->len;
  patch_jcc_rel32(buf, j_boundary, (int32_t)(boundary - (j_boundary + 6)));
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_restore_callee_saved(buf);
  emit_mov_eax_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  emit_ret(buf);
}

static void emit_caller_ret_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind rk = ctx->ret_kind;
  int wide = (rk == JELLO_T_I64 || rk == JELLO_T_F64);

  emit_mov_r32_from_r64_disp(buf, X64_R10, X64_R8, (uint32_t)offsetof(call_frame, caller_dst));
  emit_imul_r32_imm32(buf, X64_R10, REG_SLOT_BYTES);
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 2u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_imul_r64_r64(buf, X64_RAX, X64_RDX);
  emit_add_r64_r64(buf, X64_RAX, X64_RCX);
  emit_mov_r64_from_r64_disp(buf, X64_R11, X64_RAX, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));
  emit_add_r64_r64(buf, X64_R11, X64_R10);
  if(wide) emit_store_r64_to_r64_disp(buf, X64_R9, X64_R11, 0);
  else emit_store_r32_to_r64_disp(buf, X64_R9, X64_R11, 0);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_reload_jit_reg_base_x64(buf);
  emit_ret(buf);
}

static void emit_ret_insn(emit_ctx* ctx, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind rk = ctx->ret_kind;
  int wide = (rk == JELLO_T_I64 || rk == JELLO_T_F64);
  int ret_gpr = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);

  if(wide) emit_mov_r64_r64(buf, X64_R9, (unsigned)ret_gpr);
  else emit_mov_r32_r32(buf, X64_R9, (unsigned)ret_gpr);
  jit_reg_bind(ctx->ra, ret_vreg, X64_R9);
  ret_gpr = X64_R9;

  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_sub_r32_r32(buf, X64_RAX, X64_RDX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_R8, X64_RAX);
  emit_imul_r64_r64(buf, X64_R8, X64_RDX);
  emit_add_r64_r64(buf, X64_R8, X64_RCX);
  emit_movzx_r32_m8_r64_disp(buf, X64_RDX, X64_R8, (uint32_t)offsetof(call_frame, has_caller));
  emit_test_r32_r32(buf, X64_RDX, X64_RDX);
  size_t j_caller = emit_jcc_rel32(buf, 0x85, 0);
  push_caller_ret_jump(ctx, j_caller);

  emit_store_out(ctx, rk, ret_vreg);
  size_t at = emit_jmp_rel32(buf, 0);
  push_ret_jump(ctx, at);
}

static void emit_rsp_adjust_local(jit_exec_buf* buf, int is_add, uint8_t imm8);

static int emit_call_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins, jello_jit_ir_op op) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t callee_fi = (op == JIR_CALL_SELF) ? ctx->fi : ins->imm;
  uint32_t caller_dst = ins->a;
  uint32_t arg_first = ins->b;
  uint32_t nargs = ins->c;
  const jello_bc_function* callee_f = &ctx->m->funcs[callee_fi];

  jit_reg_state_discard_currents(ctx->ra);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r32_imm32(buf, X64_RSI, callee_fi);
  emit_call_fn(buf, (void*)jit_layout_ptr_or_null);
  emit_mov_r64_r64(buf, X64_R14, X64_RAX);
  emit_test_r64_r64(buf, X64_R14, X64_R14);
  size_t j_no_layout = emit_jcc_rel32(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_layout = buf->len;
  patch_jcc_rel32(buf, j_no_layout, (int32_t)(cont_layout - (j_no_layout + 6)));
  emit_mov_r32_from_r64_disp(buf, X64_RDX, X64_R14, (uint32_t)offsetof(frame_layout, total));

  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_cmp_r32_imm32(buf, X64_RCX, JIT_CALL_FRAMES_MAX);
  size_t j_frames = emit_jcc_rel32(buf, 0x82, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_frames = buf->len;
  patch_jcc_rel32(buf, j_frames, (int32_t)(cont_frames - (j_frames + 6)));

  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov_r32_r32(buf, X64_R8, X64_RCX);
  emit_add_r32_r32(buf, X64_R8, X64_RDX);
  emit_mov_r32_imm32(buf, X64_RAX, JIT_FRAME_STACK_MAX);
  emit_cmp_r32_r32(buf, X64_R8, X64_RAX);
  size_t j_max = emit_jcc_rel32(buf, 0x86, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_max = buf->len;
  patch_jcc_rel32(buf, j_max, (int32_t)(cont_max - (j_max + 6)));

  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, frame_stack_cap));
  emit_cmp_r32_r32(buf, X64_R8, X64_RAX);
  size_t j_cap_ok = emit_jcc_rel32(buf, 0x86, 0);

  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_mov_r64_r64(buf, X64_RSI, X64_RDX);
  emit_call_fn(buf, (void*)jit_try_grow_frame_stack);
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  size_t j_grow_ok = emit_jcc_rel32(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_grow = buf->len;
  patch_jcc_rel32(buf, j_grow_ok, (int32_t)(cont_grow - (j_grow_ok + 6)));

  size_t cap_ok = buf->len;
  patch_jcc_rel32(buf, j_cap_ok, (int32_t)(cap_ok - (j_cap_ok + 6)));

  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames_cap));
  emit_cmp_r32_r32(buf, X64_RCX, X64_RAX);
  size_t j_cf_ok = emit_jcc_rel32(buf, 0x82, 0);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)jit_try_grow_call_frames);
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  size_t j_cf_grow = emit_jcc_rel32(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_cf = buf->len;
  patch_jcc_rel32(buf, j_cf_grow, (int32_t)(cont_cf - (j_cf_grow + 6)));
  size_t cf_ok = buf->len;
  patch_jcc_rel32(buf, j_cf_ok, (int32_t)(cf_ok - (j_cf_ok + 6)));

  emit_mov_r32_from_r64_disp(buf, X64_RDX, X64_R14, (uint32_t)offsetof(frame_layout, total));
  emit_mov_r64_r64(buf, X64_R10, X64_R14);

  emit_mov_r64_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, frame_stack));
  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov_r32_r32(buf, X64_R11, X64_RCX);
  emit_add_r64_r64(buf, X64_RAX, X64_R11);
  emit_mov_r64_r64(buf, X64_R11, X64_RAX);

  emit_add_r32_r32(buf, X64_RCX, X64_RDX);
  emit_store_r32_to_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, frame_stack_top));

  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r64_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_r32_r32(buf, X64_R8, X64_RCX);
  emit_mov_r64_imm64(buf, X64_RDX, sizeof(call_frame));
  emit_mov_r64_r64(buf, X64_RCX, X64_R8);
  emit_imul_r64_r64(buf, X64_RCX, X64_RDX);
  emit_add_r64_r64(buf, X64_RAX, X64_RCX);

  emit_mov_r64_imm64(buf, X64_RDX, (uint64_t)(uintptr_t)callee_f);
  emit_store_r64_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, f));
  emit_mov_r32_imm32(buf, X64_RDX, callee_fi);
  emit_store_r32_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, func_index));
  emit_mov_r32_imm32(buf, X64_RDX, 0u);
  emit_store_r32_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, pc));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_store_r32_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, has_caller));
  emit_mov_r32_imm32(buf, X64_RDX, 0u);
  emit_store_r32_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, has_pointer));
  emit_mov_r32_imm32(buf, X64_RDX, caller_dst);
  emit_store_r32_to_r64_disp(buf, X64_RDX, X64_RAX, (uint32_t)offsetof(call_frame, caller_dst));

  emit_mov_r64_from_r64_disp(buf, X64_RCX, X64_R10, (uint32_t)offsetof(frame_layout, kind));
  emit_store_r64_to_r64_disp(buf, X64_RCX, X64_RAX, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, kind));
  emit_mov_r64_from_r64_disp(buf, X64_RCX, X64_R10, (uint32_t)offsetof(frame_layout, wide));
  emit_store_r64_to_r64_disp(buf, X64_RCX, X64_RAX, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, wide));
  emit_mov_r32_from_r64_disp(buf, X64_RCX, X64_R10, (uint32_t)offsetof(frame_layout, total));
  emit_store_r32_to_r64_disp(buf, X64_RCX, X64_RAX, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, size));
  emit_store_r64_to_r64_disp(buf, X64_R11, X64_RAX, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));

  emit_mov_r32_from_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_r32_imm32(buf, X64_RDX, 1u);
  emit_add_r32_r32(buf, X64_RCX, X64_RDX);
  emit_store_r32_to_r12_disp(buf, X64_RCX, (uint32_t)offsetof(jello_vm, call_frames_len));

  emit_mov_r64_r64(buf, X64_R8, (unsigned)JIT_REG_BASE);
  emit_mov_r64_r64(buf, (unsigned)JIT_REG_BASE, X64_R11);
  for(uint32_t ai = 0; ai < nargs; ai++) {
    uint32_t src_disp = (arg_first + ai) * REG_SLOT_BYTES;
    uint32_t dst_disp = ai * REG_SLOT_BYTES;
    jello_type_kind ak = vm_reg_kind(ctx->m, callee_f, ai);
    if(jello_slot_size(ak) == 8u) {
      emit_mov_r64_from_r64_disp(buf, X64_RAX, X64_R8, src_disp);
      emit_store_r64_to_r13_disp(buf, X64_RAX, dst_disp);
    } else {
      emit_mov_r32_from_r64_disp(buf, X64_RAX, X64_R8, src_disp);
      emit_store_r32_to_r13_disp(buf, X64_RAX, dst_disp);
    }
  }

  emit_rsp_adjust_local(buf, 0, 8);
  size_t call_at = emit_call_rel32(buf, 0);
  emit_rsp_adjust_local(buf, 1, 8);
  if(!push_call_patch(ctx, callee_fi, call_at)) return 0;

  emit_mov_r32_from_r12_disp(buf, X64_RAX, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test_r32_r32(buf, X64_RAX, X64_RAX);
  emit_jcc_trap_passthrough(ctx, 0x85);

  jit_reg_state_discard_currents(ctx->ra);
  (void)jit_reg_get_gpr(ctx->ra, ins->a, 1);
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

static void emit_f16_binop(emit_ctx* ctx, const jello_jit_ir_insn* ins, uint8_t sse_op) {
  emit_rsp_adjust_local(ctx->buf, 0, 16); /* sub rsp, 16 */
  emit_load_u32_slot_to_edi(ctx->buf, ins->b);
  emit_call_fn(ctx->buf, (void*)vm_f16_to_f32);
  emit_movss_xmm_mem_rsp(ctx->buf, 0, 0); /* movss [rsp], xmm0 */
  emit_load_u32_slot_to_edi(ctx->buf, ins->c);
  emit_call_fn(ctx->buf, (void*)vm_f16_to_f32);
  emit_movss_xmm_from_mem_rsp(ctx->buf, 1, 0); /* movss xmm1, [rsp] */
  emit_binss_xmm_xmm(ctx->buf, sse_op, 1, 0);
  emit_movss_xmm_xmm(ctx->buf, 0, 1);
  emit_call_fn(ctx->buf, (void*)vm_f32_to_f16);
  emit_rsp_adjust_local(ctx->buf, 1, 16); /* add rsp, 16 */
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
      emit_cvtsi2ss(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
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
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttsd2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_cvtss2sd(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0), (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_cvtsd2ss(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0), (unsigned)(sx - (int)JIT_PREG_XMM0));
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
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttsd2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_cvtsi2ss(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cvttss2si(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov_r64_r64(ctx->buf, X64_RDI, (unsigned)s);
      emit_call_fn(ctx->buf, (void*)vm_f16_to_f32);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_movss_xmm_xmm(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_movss_xmm_xmm(ctx->buf, 0, (unsigned)(sx - (int)JIT_PREG_XMM0));
      emit_call_fn(ctx->buf, (void*)vm_f32_to_f16);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, X64_RAX);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_cvtsi2ss(ctx->buf, 0, (unsigned)s, 0);
      emit_call_fn(ctx->buf, (void*)vm_f32_to_f16);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_r32_r32(ctx->buf, (unsigned)d, X64_RAX);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_I32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov_r64_r64(ctx->buf, X64_RDI, (unsigned)s);
      emit_call_fn(ctx->buf, (void*)vm_f16_to_f32);
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

static int emit_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jit_ir_op)ins->op) {
    case JIR_NOP:
      break;
    case JIR_MOV: {
      jello_type_kind dk = vm_reg_kind(ctx->m, ctx->f, ins->a);
      if(dk == JELLO_T_I64 || dk == JELLO_T_F64) {
        int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
        int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
        emit_mov_r64_r64(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      } else {
        int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
        int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
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

  size_t blob_start = buf->len;
  size_t trap_jumps[256];
  size_t trap_passthrough_jumps[64];
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
  ctx.trap_passthrough_jumps = trap_passthrough_jumps;
  ctx.trap_passthrough_jump_cap = 64;
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

  jit_reg_state ra;
  jit_reg_emit_ctx re;
  re.buf = buf;
  jit_reg_state_init(&ra, ir->nregs, re);
  ctx.ra = &ra;

  size_t body_off = 0;
  emit_prologue(buf, &body_off);

  int ok = 1;
  for(uint32_t i = 0; i < ir->ninsns && ok; i++) {
    const jello_jit_ir_insn* ins = &ir->insns[i];
    uint32_t bc = ins->bc_pc;
    if(bc < f->ninsns && is_target[bc] && !discarded[bc]) {
      jit_reg_state_discard_currents(&ra);
      discarded[bc] = 1;
    }
    if(bc < f->ninsns && native_off[bc] == (size_t)-1) native_off[bc] = buf->len;

    jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
    if(op == JIR_RET) {
      emit_ret_insn(&ctx, ins->a);
      continue;
    }
    if(op == JIR_JMP) {
      uint32_t tgt = 0;
      if(!resolve_target_bc(bc, (int32_t)ins->imm, f->ninsns, &tgt)) { ok = 0; break; }
      size_t at = emit_jmp_rel32(buf, 0);
      if(!push_branch(&ctx, at, tgt, 0)) ok = 0;
      continue;
    }
    if(op == JIR_JMP_IF) {
      uint32_t tgt = 0;
      if(!resolve_target_bc(bc, (int32_t)ins->imm, f->ninsns, &tgt)) { ok = 0; break; }
      int cond = jit_reg_get_gpr(&ra, ins->a, 1);
      emit_test_r32_r32(buf, (unsigned)cond, (unsigned)cond);
      size_t at = emit_jcc_rel32(buf, 0x85, 0);
      if(!push_branch(&ctx, at, tgt, 1)) ok = 0;
      continue;
    }
    if(op == JIR_SAFEPOINT) {
      emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
      emit_call_fn(buf, (void*)vm_safepoint_check);
      jit_reg_state_clobber_call(&ra);
      continue;
    }
    if(allow_calls && (op == JIR_CALL_SELF || op == JIR_CALL_DIRECT)) {
      ok = emit_call_insn(&ctx, ins, op);
      continue;
    }
    ok = emit_insn(&ctx, ins);
    jit_reg_unpin_all(&ra);
  }

  size_t epilogue_ok_off = 0;
  size_t trap_off = 0;
  size_t trap_passthrough_off = 0;
  size_t stack_overflow_off = 0;
  if(ok) {
    epilogue_ok_off = buf->len;
    emit_epilogue_ok_tail(&ctx);
    size_t caller_ret_off = buf->len;
    emit_caller_ret_tail(&ctx);
    for(size_t i = 0; i < ctx.caller_ret_jump_count; i++) {
      size_t at = ctx.caller_ret_jumps[i];
      patch_jcc_rel32(buf, at, (int32_t)(caller_ret_off - (at + 6)));
    }
    trap_off = buf->len;
    emit_epilogue_trap(&ctx);
    stack_overflow_off = buf->len;
    emit_epilogue_stack_overflow(&ctx);
    trap_passthrough_off = buf->len;
    emit_trap_tail_x64(&ctx);
    patch_trap_jumps(&ctx, trap_off);
    patch_trap_passthrough_jumps(&ctx, trap_passthrough_off);
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

void jello_jit_code_free(jello_jit_code* code) {
  free(code->loop_anchor_pcs);
  free(code->loop_anchor_offs);
  code->loop_anchor_pcs = NULL;
  code->loop_anchor_offs = NULL;
  code->nloop_anchors = 0;
  jit_free_exec_buf(&code->buf);
}
