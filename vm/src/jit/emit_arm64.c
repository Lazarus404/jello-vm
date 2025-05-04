// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
#include <jello/internal/jit_arm64.h>
#include <jello.h>
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* -- AArch64 encoding -- */

enum {
  A64_X0 = 0,
  A64_X1 = 1,
  A64_X2 = 2,
  A64_X3 = 3,
  A64_X4 = 4,
  A64_X5 = 5,
  A64_X9 = 9,
  A64_X10 = 10,
  A64_X11 = 11,
  A64_X12 = 12,
  A64_X13 = 13,
  A64_X14 = 14,
  A64_X15 = 15,
  A64_LAYOUT = 27,
  A64_FP = 29,
  A64_LR = 30,
  A64_SP = 31,
  A64_IP0 = 16,
  A64_IP1 = 17,
  A64_TMP0 = 15,
  A64_TMP1 = 16,
  A64_TMP2 = 17,
  A64_D_TMP0 = 16,
  A64_D_TMP1 = 17, /**< FP scratch outside the d8-d15 pool. */
  A64_NO_GPR = 32, /**< emit_disp_base's `data` for an FP register. */
};

static void emit_u32(jit_exec_buf* buf, uint32_t word) {
  jit_exec_buf_write(buf, &word, 4);
}

static uint8_t pool_dreg_i(unsigned pool_idx) {
  return (uint8_t)(8u + pool_idx);
}

static void emit_mov_x_imm64(jit_exec_buf* buf, uint8_t rd, uint64_t imm) {
  emit_u32(buf, a64_movz_x(rd, (uint16_t)(imm & 0xFFFFu), 0));
  if(imm > 0xFFFFu)
    emit_u32(buf, a64_movk_x(rd, (uint16_t)((imm >> 16) & 0xFFFFu), 16));
  if(imm > 0xFFFFFFFFu)
    emit_u32(buf, a64_movk_x(rd, (uint16_t)((imm >> 32) & 0xFFFFu), 32));
  if(imm > 0xFFFFFFFFFFFFu)
    emit_u32(buf, a64_movk_x(rd, (uint16_t)((imm >> 48) & 0xFFFFu), 48));
}

static uint64_t fn_addr_bits_fn(void* fn) {
  uint64_t bits;
  memcpy(&bits, &fn, sizeof(bits));
  return bits;
}

static void emit_mov64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mov_x_rr((uint8_t)dst, (uint8_t)src));
}

static void emit_call_fn(jit_exec_buf* buf, uintptr_t fn) {
  emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, 16));
  emit_u32(buf, a64_str_x_uimm(A64_LR, A64_SP, 8));
  emit_mov_x_imm64(buf, A64_IP0, fn_addr_bits_fn(fn));
  emit_u32(buf, a64_blr_x(A64_IP0));
  emit_u32(buf, a64_ldr_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, 16));
}

static int placeholder_written(const jit_exec_buf* buf, size_t at) { return at + 4u <= buf->len; }

static int32_t off_insns_at(size_t at, size_t target) {
  return (int32_t)((int64_t)target - (int64_t)at) / 4;
}

static size_t emit_b_placeholder(jit_exec_buf* buf) {
  size_t at = buf->len;
  emit_u32(buf, a64_b(0));
  return at;
}

static void patch_b(jit_exec_buf* buf, size_t at, size_t target) {
  int32_t off = off_insns_at(at, target);
  uint32_t word = a64_b(off);
  memcpy(buf->mem + at, &word, 4);
}

void jit_patch_jump(jit_exec_buf* buf, size_t at, size_t target) {
  if(!placeholder_written(buf, at)) return;
  uint32_t off = (uint32_t)off_insns_at(at, target);
  uint32_t word;
  memcpy(&word, buf->mem + at, 4);
  if((word >> 26) == 0x5u) word = (word & 0xFC000000u) | (off & 0x03FFFFFFu);
  else word = (word & 0xFF00001Fu) | ((off & 0x7FFFFu) << 5);
  memcpy(buf->mem + at, &word, 4);
}

static size_t emit_b_cond_placeholder(jit_exec_buf* buf, uint8_t cc) {
  size_t at = buf->len;
  a64_cond cond;
  switch(cc) {
    case JIT_JE: cond = A64_EQ; break;
    case JIT_JNE: cond = A64_NE; break;
    case JIT_JB: cond = A64_LO; break;
    case JIT_JAE: cond = A64_HS; break;
    case JIT_JBE: cond = A64_LS; break;
    case JIT_JA: cond = A64_HI; break;
    case JIT_JL: cond = A64_LT; break;
    case JIT_JG: cond = A64_GT; break;
    case JIT_SETL: cond = A64_LT; break;
    default: cond = A64_EQ; break;
  }
  emit_u32(buf, a64_b_cond(cond, 0));
  return at;
}

static void patch_b_cond(jit_exec_buf* buf, size_t at, size_t target) {
  int32_t off = off_insns_at(at, target);
  uint32_t old = *(uint32_t*)(buf->mem + at);
  uint32_t cond = old & 0xFu;
  uint32_t word = 0x54000000u | (((uint32_t)off & 0x7FFFFu) << 5) | cond;
  memcpy(buf->mem + at, &word, 4);
}

static size_t emit_cbz_w_placeholder(jit_exec_buf* buf, unsigned reg) {
  size_t at = buf->len;
  emit_u32(buf, a64_cbz_w((uint8_t)reg, 0));
  return at;
}

static size_t emit_cbnz_w_placeholder(jit_exec_buf* buf, unsigned reg) {
  size_t at = buf->len;
  emit_u32(buf, a64_cbnz_w((uint8_t)reg, 0));
  return at;
}

static void patch_cbz_w(jit_exec_buf* buf, size_t at, size_t target) {
  int32_t off = off_insns_at(at, target);
  uint32_t old = *(uint32_t*)(buf->mem + at);
  uint32_t rt = old & 0x1Fu;
  uint32_t word = 0x34000000u | (((uint32_t)off & 0x7FFFFu) << 5) | rt;
  memcpy(buf->mem + at, &word, 4);
}

static void patch_cbnz_w(jit_exec_buf* buf, size_t at, size_t target) {
  int32_t off = off_insns_at(at, target);
  uint32_t old = *(uint32_t*)(buf->mem + at);
  uint32_t rt = old & 0x1Fu;
  uint32_t word = 0x35000000u | (((uint32_t)off & 0x7FFFFu) << 5) | rt;
  memcpy(buf->mem + at, &word, 4);
}

static size_t emit_bl_placeholder(jit_exec_buf* buf) {
  size_t at = buf->len;
  emit_u32(buf, a64_bl(0));
  return at;
}

static void patch_bl(jit_exec_buf* buf, size_t at, int32_t rel_unused) {
  (void)rel_unused;
  (void)at;
  (void)buf;
}

static void emit_ret_insn_a64(jit_exec_buf* buf) {
  emit_u32(buf, a64_ret());
}

static void emit_mov_w_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mov_w_rr((uint8_t)dst, (uint8_t)src));
}

static void emit_mov32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mov_w_rr((uint8_t)dst, (uint8_t)src));
}

static void emit_uxtw_x_w(jit_exec_buf* buf, unsigned xd, unsigned ws) {
  emit_u32(buf, a64_uxtw_x_w((uint8_t)xd, (uint8_t)ws));
}

static void emit_mov_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mov_x_rr((uint8_t)dst, (uint8_t)src));
}

static void emit_mov_w_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_u32(buf, a64_movz_w((uint8_t)dst, (uint16_t)(imm & 0xFFFFu), 0));
  if(imm > 0xFFFFu)
    emit_u32(buf, a64_movk_w((uint8_t)dst, (uint16_t)(imm >> 16), 16));
}

static void emit_mov32_imm(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_u32(buf, a64_movz_w((uint8_t)dst, (uint16_t)(imm & 0xFFFFu), 0));
  if(imm > 0xFFFFu) emit_u32(buf, a64_movk_w((uint8_t)dst, (uint16_t)(imm >> 16), 16));
}

static void emit_mov_w0_imm32(jit_exec_buf* buf, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_X0, imm);
}

static void emit_mov_x_imm64_reg(jit_exec_buf* buf, unsigned dst, uint64_t imm) {
  emit_mov_x_imm64(buf, (uint8_t)dst, imm);
}

static void emit_store_out_kind_imm32(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_X9, imm);
  emit_u32(buf, a64_str_w_uimm(A64_X9, (uint8_t)JIT_REG_OUT, disp));
}

static void emit_store_out_as_w(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)JIT_REG_OUT, disp));
}

static void emit_store_out_as_x(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)JIT_REG_OUT, disp));
}

static void emit_ldr_x_vm(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, (uint8_t)JIT_REG_VM, disp));
}

static void emit_ldr_w_vm(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  emit_u32(buf, a64_ldr_w_uimm((uint8_t)dst, (uint8_t)JIT_REG_VM, disp));
}

static void emit_str_w_vm(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)JIT_REG_VM, disp));
}

static void emit_str_x_vm(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)JIT_REG_VM, disp));
}

static void emit_store_imm32_to_vm_field(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_TMP1, imm);
  emit_str_w_vm(buf, A64_TMP1, disp);
}

static void emit_ldr_x_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  if(disp <= 32760u && (disp & 7u) == 0)
    emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, (uint8_t)base, disp));
  else {
    emit_mov_w_imm32(buf, A64_IP0, disp);
    emit_u32(buf, a64_add_x_rr(A64_IP0, (uint8_t)base, A64_IP0));
    emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, A64_IP0, 0));
  }
}

static void emit_ldr_w_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  if(disp <= 16380u && (disp & 3u) == 0)
    emit_u32(buf, a64_ldr_w_uimm((uint8_t)dst, (uint8_t)base, disp));
  else {
    emit_mov_w_imm32(buf, A64_IP0, disp);
    emit_u32(buf, a64_add_x_rr(A64_IP0, (uint8_t)base, A64_IP0));
    emit_u32(buf, a64_ldr_w_uimm((uint8_t)dst, A64_IP0, 0));
  }
}

static void emit_str_w_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  if(disp <= 16380u && (disp & 3u) == 0)
    emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)base, disp));
  else {
    emit_mov_w_imm32(buf, A64_IP0, disp);
    emit_u32(buf, a64_add_x_rr(A64_IP0, (uint8_t)base, A64_IP0));
    emit_u32(buf, a64_str_w_uimm((uint8_t)src, A64_IP0, 0));
  }
}

static void emit_str_x_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  if(disp <= 32760u && (disp & 7u) == 0)
    emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)base, disp));
  else {
    emit_mov_w_imm32(buf, A64_IP0, disp);
    emit_u32(buf, a64_add_x_rr(A64_IP0, (uint8_t)base, A64_IP0));
    emit_u32(buf, a64_str_x_uimm((uint8_t)src, A64_IP0, 0));
  }
}

static void emit_mov64_imm(jit_exec_buf* buf, unsigned dst, uint64_t imm) {
  emit_mov_x_imm64(buf, (uint8_t)dst, imm);
}

static void emit_load_vm64(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, (uint8_t)JIT_REG_VM, disp));
}

static void emit_load_vm32(jit_exec_buf* buf, unsigned dst, uint32_t disp) {
  emit_u32(buf, a64_ldr_w_uimm((uint8_t)dst, (uint8_t)JIT_REG_VM, disp));
}

static void emit_store_vm32(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)JIT_REG_VM, disp));
}

static void emit_store_vm32_imm(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  emit_mov32_imm(buf, A64_TMP1, imm);
  emit_store_vm32(buf, A64_TMP1, disp);
}

static int disp_encodes(uint32_t disp, uint32_t size) {
  return disp % size == 0 && disp / size <= 0xFFFu;
}

static unsigned emit_disp_base(jit_exec_buf* buf, unsigned base, unsigned data, unsigned scratch,
                               uint32_t* disp, uint32_t size) {
  if(disp_encodes(*disp, size)) return base;
  assert(scratch != base && scratch != data);
  emit_mov32_imm(buf, scratch, *disp);
  emit_u32(buf, a64_add_x_rr((uint8_t)scratch, (uint8_t)base, (uint8_t)scratch));
  *disp = 0;
  return scratch;
}

static void emit_load64(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 8));
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_load32(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 4));
  emit_u32(buf, a64_ldr_w_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_store32(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 4));
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_store64(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 8));
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_str_w_base(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)JIT_REG_BASE, disp));
}

static void emit_str_x_base(jit_exec_buf* buf, unsigned src, uint32_t disp) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)JIT_REG_BASE, disp));
}

static void emit_add_w_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_add_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_add_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_sub_w_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_alu_w_rr(jit_exec_buf* buf, uint8_t opcode, unsigned dst, unsigned src) {
  (void)opcode;
  if(opcode == 0x01) emit_u32(buf, a64_add_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x29) emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x21) emit_u32(buf, a64_and_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x09) emit_u32(buf, a64_orr_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x31) emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_alu_x_rr(jit_exec_buf* buf, uint8_t opcode, unsigned dst, unsigned src) {
  if(opcode == 0x01) emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x29) emit_u32(buf, a64_sub_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x21) emit_u32(buf, a64_and_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x09) emit_u32(buf, a64_orr_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(opcode == 0x31) emit_u32(buf, a64_eor_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_alu_w_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  if(op_ext == 0x00 && imm <= 0xFFFu) emit_u32(buf, a64_add_w_imm((uint8_t)dst, (uint8_t)dst, imm));
  else if(op_ext == 0x05 && imm <= 0xFFFu) emit_u32(buf, a64_sub_w_imm((uint8_t)dst, (uint8_t)dst, imm));
  else if(op_ext == 0x04) {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    emit_u32(buf, a64_and_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  } else if(op_ext == 0x06) {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  } else {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    if(op_ext == 0x00) emit_u32(buf, a64_add_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
    else emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  }
}

static void emit_alu_x_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  if(op_ext == 0x00 && imm <= 0xFFFu) emit_u32(buf, a64_add_x_imm((uint8_t)dst, (uint8_t)dst, imm));
  else {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    if(op_ext == 0x00) emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
    else emit_u32(buf, a64_sub_x_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  }
}

static void emit_neg_w(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_neg_w((uint8_t)dst, (uint8_t)dst));
}

static void emit_neg_x(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_neg_x((uint8_t)dst, (uint8_t)dst));
}

static void emit_cmp_w_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_subs_w_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_cmp_w_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  if(imm <= 0xFFFu) emit_u32(buf, a64_cmp_w_imm((uint8_t)dst, (uint16_t)imm));
  else {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    emit_u32(buf, a64_subs_w_rr(0x1F, (uint8_t)dst, A64_TMP0));
  }
}

static void emit_cmp_x_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_subs_x_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_add64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_sub32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_alu32(jit_exec_buf* buf, jit_alu op, unsigned dst, unsigned src) {
  if(op == JIT_ALU_ADD) emit_u32(buf, a64_add_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(op == JIT_ALU_SUB) emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(op == JIT_ALU_AND) emit_u32(buf, a64_and_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(op == JIT_ALU_OR) emit_u32(buf, a64_orr_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(op == JIT_ALU_XOR) emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_alu64(jit_exec_buf* buf, jit_alu op, unsigned dst, unsigned src) {
  if(op == JIT_ALU_ADD) emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(op == JIT_ALU_SUB) emit_u32(buf, a64_sub_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(op == JIT_ALU_AND) emit_u32(buf, a64_and_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(op == JIT_ALU_OR) emit_u32(buf, a64_orr_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
  else if(op == JIT_ALU_XOR) emit_u32(buf, a64_eor_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_alu32_imm(jit_exec_buf* buf, jit_alu op, unsigned dst, uint32_t imm) {
  if(op == JIT_ALU_ADD && imm <= 0xFFFu)
    emit_u32(buf, a64_add_w_imm((uint8_t)dst, (uint8_t)dst, imm));
  else if(op == JIT_ALU_SUB && imm <= 0xFFFu)
    emit_u32(buf, a64_sub_w_imm((uint8_t)dst, (uint8_t)dst, imm));
  else if(op == JIT_ALU_AND) {
    emit_mov32_imm(buf, A64_TMP0, imm);
    emit_u32(buf, a64_and_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  } else if(op == JIT_ALU_XOR) {
    emit_mov32_imm(buf, A64_TMP0, imm);
    emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  } else {
    emit_mov32_imm(buf, A64_TMP0, imm);
    if(op == JIT_ALU_ADD) emit_u32(buf, a64_add_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
    else emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  }
}

static void emit_alu64_imm(jit_exec_buf* buf, jit_alu op, unsigned dst, uint32_t imm) {
  if(op == JIT_ALU_ADD && imm <= 0xFFFu)
    emit_u32(buf, a64_add_x_imm((uint8_t)dst, (uint8_t)dst, imm));
  else {
    emit_mov32_imm(buf, A64_TMP0, imm);
    if(op == JIT_ALU_ADD) emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
    else emit_u32(buf, a64_sub_x_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
  }
}

static void emit_neg32(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_neg_w((uint8_t)dst, (uint8_t)dst));
}

static void emit_neg64(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_neg_x((uint8_t)dst, (uint8_t)dst));
}

static void emit_cmp32(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_subs_w_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_cmp32_imm(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  if(imm <= 0xFFFu) emit_u32(buf, a64_cmp_w_imm((uint8_t)dst, (uint16_t)imm));
  else {
    emit_mov32_imm(buf, A64_TMP0, imm);
    emit_u32(buf, a64_subs_w_rr(0x1F, (uint8_t)dst, A64_TMP0));
  }
}

static void emit_cmp64(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_subs_x_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_cmp_x_eq_neg1(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_cmn_x_imm((uint8_t)reg, 1));
}

static void emit_tst_w_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_w_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_tst_x_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_x_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_test32(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_w_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_test64(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_x_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_setcc_from_flags(jit_exec_buf* buf, uint8_t cc_opcode, unsigned dst) {
  uint8_t cond;
  switch(cc_opcode) {
    case 0x94: cond = 0; break;
    case 0x92: cond = 0xB; break;
    case 0x9C: cond = 0xB; break;
    default: cond = 0; break;
  }
  emit_u32(buf, a64_cset_w((uint8_t)dst, cond));
}

static void emit_sxtw_x_w(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sxtw_x_w((uint8_t)dst, (uint8_t)src));
}

static void emit_sxth_w(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sxth_w((uint8_t)dst, (uint8_t)src));
}

static void emit_eor_w_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_TMP0, imm);
  emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_mul_w_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mul_w((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_mul_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mul_x((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_mul_w_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_TMP0, imm);
  emit_u32(buf, a64_mul_w((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_xor32_imm(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_mov32_imm(buf, A64_TMP0, imm);
  emit_u32(buf, a64_eor_w_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_mul32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mul_w((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_mul64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mul_x((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_mul32_imm(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_mov32_imm(buf, A64_TMP0, imm);
  emit_u32(buf, a64_mul_w((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_lsl_x_imm3(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_lsl_x_imm((uint8_t)dst, (uint8_t)src, 3));
}

static void emit_lslv_w(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_lslv_w((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_asrv_w(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_asrv_w((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_lslv_x(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_lslv_x((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_asrv_x(jit_exec_buf* buf, unsigned dst) {
  emit_u32(buf, a64_asrv_x((uint8_t)dst, (uint8_t)dst, A64_TMP0));
}

static void emit_ldrb_w_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  emit_u32(buf, a64_ldrb_w_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_ldrh_w_disp(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  emit_u32(buf, a64_ldrh_w_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_strb_w_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  emit_u32(buf, a64_strb_w_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_strh_w_disp(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  emit_u32(buf, a64_strh_w_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_rev_w(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_rev_w((uint8_t)reg, (uint8_t)reg));
}

static void emit_rev_x(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_rev_x((uint8_t)reg, (uint8_t)reg));
}

static void emit_rev16_w(jit_exec_buf* buf, unsigned reg, uint8_t imm8) {
  (void)imm8;
  emit_u32(buf, a64_rev16_w((uint8_t)reg, (uint8_t)reg));
}

static void emit_fmov_s(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fmov_s(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_load8(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 1));
  emit_u32(buf, a64_ldrb_w_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_load16(jit_exec_buf* buf, unsigned dst, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 2));
  emit_u32(buf, a64_ldrh_w_uimm((uint8_t)dst, (uint8_t)base, disp));
}

static void emit_store8(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 1));
  emit_u32(buf, a64_strb_w_uimm((uint8_t)src, (uint8_t)base, disp));
}

static void emit_bswap32(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_rev_w((uint8_t)reg, (uint8_t)reg));
}

static void emit_bswap64(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_rev_x((uint8_t)reg, (uint8_t)reg));
}

static void emit_bswap16(jit_exec_buf* buf, unsigned reg, uint8_t imm8) {
  (void)imm8;
  emit_u32(buf, a64_rev16_w((uint8_t)reg, (uint8_t)reg));
}

static void emit_fmov_s_from_s0(jit_exec_buf* buf, unsigned dst_pool) {
  emit_u32(buf, a64_fmov_s(pool_dreg_i(dst_pool), 0));
}

static void emit_fmov_s_to_s0(jit_exec_buf* buf, unsigned src_pool) {
  emit_u32(buf, a64_fmov_s(0, pool_dreg_i(src_pool)));
}

static void emit_fcvtzs_w_from_s0(jit_exec_buf* buf, unsigned dst_gpr) {
  emit_u32(buf, a64_fcvtzs_w_s((uint8_t)dst_gpr, 0));
}

static void emit_scvtf_s_to_s0(jit_exec_buf* buf, unsigned src_gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_s_x(0, (uint8_t)src_gpr));
  else emit_u32(buf, a64_scvtf_s_w(0, (uint8_t)src_gpr));
}

static void emit_fbin_s(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t dd = pool_dreg_i(dst), dn = pool_dreg_i(dst), dm = pool_dreg_i(src);
  if(op == 0x58) emit_u32(buf, a64_fadd_s(dd, dn, dm));
  else if(op == 0x5C) emit_u32(buf, a64_fsub_s(dd, dn, dm));
  else if(op == 0x59) emit_u32(buf, a64_fmul_s(dd, dn, dm));
  else if(op == 0x5E) emit_u32(buf, a64_fdiv_s(dd, dn, dm));
}

static void emit_fmov_d(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fmov_d(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_fbin_d(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t dd = pool_dreg_i(dst), dn = pool_dreg_i(dst), dm = pool_dreg_i(src);
  if(op == 0x58) emit_u32(buf, a64_fadd_d(dd, dn, dm));
  else if(op == 0x5C) emit_u32(buf, a64_fsub_d(dd, dn, dm));
  else if(op == 0x59) emit_u32(buf, a64_fmul_d(dd, dn, dm));
  else if(op == 0x5E) emit_u32(buf, a64_fdiv_d(dd, dn, dm));
}

static void emit_fcmp_s(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_fcmp_s(pool_dreg_i(a), pool_dreg_i(b)));
}

static void emit_fcmp_d(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_fcmp_d(pool_dreg_i(a), pool_dreg_i(b)));
}

static void emit_scvtf_s(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_s_x(pool_dreg_i(xmm), (uint8_t)gpr));
  else emit_u32(buf, a64_scvtf_s_w(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_scvtf_d(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_d_x(pool_dreg_i(xmm), (uint8_t)gpr));
  else emit_u32(buf, a64_scvtf_d_w(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_fcvtzs_w_s(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  if(wide) emit_u32(buf, a64_fcvtzs_x_s((uint8_t)gpr, pool_dreg_i(xmm)));
  else emit_u32(buf, a64_fcvtzs_w_s((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_fcvtzs_w_d(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  if(wide) emit_u32(buf, a64_fcvtzs_x_d((uint8_t)gpr, pool_dreg_i(xmm)));
  else emit_u32(buf, a64_fcvtzs_w_d((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_fcvt_d_s(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fcvt_d_s(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_fcvt_s_d(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fcvt_s_d(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_fmov_s_w(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  emit_u32(buf, a64_fmov_s_x(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_fmov_w_s(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  emit_u32(buf, a64_fmov_x_s((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_fmov_d_x(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  emit_u32(buf, a64_fmov_d_x(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_fmov_x_d(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  emit_u32(buf, a64_fmov_x_d((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_str_x_sp(jit_exec_buf* buf, unsigned src, uint8_t disp) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, A64_SP, disp));
}

static void emit_ldr_x_sp(jit_exec_buf* buf, unsigned dst, uint8_t disp) {
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, A64_SP, disp));
}

static void emit_store64_sp(jit_exec_buf* buf, unsigned src, uint8_t disp) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, A64_SP, disp));
}

static void emit_load64_sp(jit_exec_buf* buf, unsigned dst, uint8_t disp) {
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, A64_SP, disp));
}

static void emit_sp_adjust(jit_exec_buf* buf, int is_add, uint8_t imm8) {
  if(is_add) emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, imm8));
  else emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, imm8));
}

static void emit_ldr_s_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  if(disp <= 16380u && (disp & 3u) == 0)
    emit_u32(buf, a64_ldr_s_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
  else {
    emit_mov_w_imm32(buf, A64_IP0, disp);
    emit_u32(buf, a64_add_x_rr(A64_IP0, (uint8_t)base, A64_IP0));
    emit_u32(buf, a64_ldr_s_uimm(pool_dreg_i(xmm), A64_IP0, 0));
  }
}

static void emit_str_s_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  if(disp <= 16380u && (disp & 3u) == 0)
    emit_u32(buf, a64_str_s_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
  else {
    emit_mov_w_imm32(buf, A64_IP0, disp);
    emit_u32(buf, a64_add_x_rr(A64_IP0, (uint8_t)base, A64_IP0));
    emit_u32(buf, a64_str_s_uimm(pool_dreg_i(xmm), A64_IP0, 0));
  }
}

static void emit_ldr_d_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  if(disp <= 32760u && (disp & 7u) == 0)
    emit_u32(buf, a64_ldr_d_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
  else {
    emit_mov_w_imm32(buf, A64_IP0, disp);
    emit_u32(buf, a64_add_x_rr(A64_IP0, (uint8_t)base, A64_IP0));
    emit_u32(buf, a64_ldr_d_uimm(pool_dreg_i(xmm), A64_IP0, 0));
  }
}

static void emit_str_d_disp(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  if(disp <= 32760u && (disp & 7u) == 0)
    emit_u32(buf, a64_str_d_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
  else {
    emit_mov_w_imm32(buf, A64_IP0, disp);
    emit_u32(buf, a64_add_x_rr(A64_IP0, (uint8_t)base, A64_IP0));
    emit_u32(buf, a64_str_d_uimm(pool_dreg_i(xmm), A64_IP0, 0));
  }
}

static void emit_fload32(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 4));
  emit_u32(buf, a64_ldr_s_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
}

static void emit_fstore32(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 4));
  emit_u32(buf, a64_str_s_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
}

static void emit_fload64(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 8));
  emit_u32(buf, a64_ldr_d_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
}

static void emit_fstore64(jit_exec_buf* buf, unsigned xmm, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 8));
  emit_u32(buf, a64_str_d_uimm(pool_dreg_i(xmm), (uint8_t)base, disp));
}

static void emit_load_u32_slot_to_w1(jit_exec_buf* buf, uint8_t vreg) {
  uint32_t off = (uint32_t)vreg * REG_SLOT_BYTES;
  emit_u32(buf, a64_ldr_w_uimm(A64_X1, (uint8_t)JIT_REG_BASE, off));
}

static void emit_jmp_stack_overflow(emit_ctx* ctx) {
  emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
  emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  size_t at = emit_b_placeholder(ctx->buf);
  if(ctx->stack_overflow_jump_count < ctx->stack_overflow_jump_cap)
    ctx->stack_overflow_jumps[ctx->stack_overflow_jump_count++] = at;
}

static void patch_stack_overflow_jumps(emit_ctx* ctx, size_t so_off) {
  for(size_t i = 0; i < ctx->stack_overflow_jump_count; i++) {
    size_t at = ctx->stack_overflow_jumps[i];
    patch_b(ctx->buf, at, so_off);
  }
}

static void emit_jcc_trap(emit_ctx* ctx, uint8_t cc, uint8_t kind) {
  size_t skip = emit_b_cond_placeholder(ctx->buf, (uint8_t)(cc ^ 1u), 0);
  if(kind != JIT_TRAP_PASSTHROUGH) {
    emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
    emit_store_imm32_to_vm_field(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  }
  size_t at = emit_b_placeholder(ctx->buf);
  patch_b_cond(ctx->buf, skip, ctx->buf->len);
  if(ctx->trap_jump_count < ctx->trap_jump_cap) {
    ctx->trap_jumps[ctx->trap_jump_count].at = at;
    ctx->trap_jumps[ctx->trap_jump_count].kind = kind;
    ctx->trap_jump_count++;
  }
}

static void patch_trap_jumps(emit_ctx* ctx, const size_t* trap_off_by_kind) {
  for(size_t i = 0; i < ctx->trap_jump_count; i++) {
    size_t at = ctx->trap_jumps[i].at;
    size_t trap_off = trap_off_by_kind[ctx->trap_jumps[i].kind];
    uint32_t word;
    memcpy(&word, ctx->buf->mem + at, 4);
    if((word >> 26) == 0x5u) patch_b(ctx->buf, at, trap_off);
    else patch_b_cond(ctx->buf, at, trap_off);
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

static int resolve_target_bc(uint32_t bc_pc, int32_t imm, uint32_t ninsns, uint32_t* out) {
  int64_t t = (int64_t)(int32_t)bc_pc + 1 + (int64_t)imm;
  if(t < 0 || t >= (int64_t)ninsns) return 0;
  *out = (uint32_t)t;
  return 1;
}

static void emit_divmod_guard_i32(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_tst_w_rr(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  emit_cmp_w_imm32(ctx->buf, div_gpr, (uint32_t)-1);
  size_t jne = ctx->buf->len;
  emit_b_cond_placeholder(ctx->buf, 0x85, 0);
  emit_cmp_w_imm32(ctx->buf, lhs_gpr, (uint32_t)INT32_MIN);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  patch_b_cond(ctx->buf, jne, ok);
}

static void emit_divmod_guard_i64(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_tst_x_rr(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  emit_cmp_x_eq_neg1(ctx->buf, div_gpr);
  size_t jne = ctx->buf->len;
  emit_b_cond_placeholder(ctx->buf, 0x85, 0);
  emit_mov_x_imm64_reg(ctx->buf, A64_TMP0, (uint64_t)INT64_MIN);
  emit_cmp_x_rr(ctx->buf, lhs_gpr, A64_TMP0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  patch_b_cond(ctx->buf, jne, ok);
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

static void emit_prologue(jit_exec_buf* buf, size_t* out_body_off, int any_armed) {
  emit_u32(buf, a64_stp_x_pre(A64_FP, A64_LR, A64_SP, -2));
  emit_u32(buf, a64_mov_x_rr(A64_FP, A64_SP));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, -2));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, -2));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_VM, A64_X0));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_BASE, A64_X1));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_OUT, A64_X2));
  *out_body_off = buf->len;
  if(any_armed) {
    emit_sp_adjust(buf, 0, 16);
    emit_str_x_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
#ifndef NDEBUG
    emit_mov_x_rr(buf, A64_X0, A64_SP);
    emit_call_fn(buf, (void*)jit_debug_assert_rsp_aligned16);
#endif
  }
}

static void emit_restore_callee_saved(jit_exec_buf* buf, int any_armed) {
  if(any_armed) {
    emit_ldr_x_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post(A64_FP, A64_LR, A64_SP, 2));
}

static void emit_store_out(emit_ctx* ctx, jello_type_kind ret_kind, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_VOID);
  if(ret_kind == JELLO_T_BOOL) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_BOOL);
    emit_store_out_as_w(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_I32);
    emit_store_out_as_w(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_I64);
    emit_store_out_as_x(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_F32);
    emit_store_out_as_w(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_F64);
    emit_store_out_as_x(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_BYTES || ret_kind == JELLO_T_STRUCT || ret_kind == JELLO_T_OBJECT) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_PTR);
    emit_store_out_as_x(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_DYNAMIC) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out_kind_imm32(buf, (uint32_t)offsetof(jello_numeric_result, kind), (uint32_t)JELLO_NUM_DYN);
    emit_store_out_as_x(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  }
}

static void emit_epilogue_ok_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_OK);
  emit_ret_insn_a64(buf);
}

static void emit_reload_jit_reg_base(jit_exec_buf* buf) {
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)jit_current_frame_mem);
  emit_mov_x_rr(buf, (unsigned)JIT_REG_BASE, A64_X0);
}

static void emit_trap_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_TMP1, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_w_imm32(buf, A64_X14, 1u);
  emit_sub_w_rr(buf, A64_TMP0, A64_X14);
  emit_mov_x_imm64_reg(buf, A64_X14, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_TMP0, A64_TMP0);
  emit_mul_x_rr(buf, A64_TMP0, A64_X14);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);
  emit_ldrb_w_disp(buf, A64_X14, A64_TMP0, (uint32_t)offsetof(call_frame, has_caller));
  size_t j_boundary = emit_cbz_w_placeholder(buf, A64_X14);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_reload_jit_reg_base(buf);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  if(ctx->survive_any_armed) {
    emit_ldr_x_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_ret_insn_a64(buf);

  size_t boundary = buf->len;
  patch_cbz_w(buf, j_boundary, boundary);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  emit_ret_insn_a64(buf);
}

static void emit_epilogue_trap_generic(emit_ctx* ctx, void* trap_fn) {
  jit_exec_buf* buf = ctx->buf;
  if(trap_fn) {
    emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, trap_fn);
  }
  emit_trap_tail(ctx);
}

static void emit_epilogue_stack_overflow(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)jit_trap_stack_overflow);
  emit_trap_tail(ctx);
}

static void emit_ret_insn(emit_ctx* ctx, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind rk = ctx->ret_kind;
  int wide = (rk == JELLO_T_I64 || rk == JELLO_T_F64);
  int ret_gpr = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);

  if(wide) emit_mov_x_rr(buf, A64_TMP2, (unsigned)ret_gpr);
  else emit_mov_w_rr(buf, A64_TMP2, (unsigned)ret_gpr);
  jit_reg_bind(ctx->ra, ret_vreg, A64_TMP2);
  ret_gpr = A64_TMP2;

  emit_ldr_w_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_TMP1, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_w_imm32(buf, A64_X14, 1u);
  emit_sub_w_rr(buf, A64_TMP0, A64_X14);
  emit_mov_x_imm64_reg(buf, A64_X14, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_TMP0, A64_TMP0);
  emit_mul_x_rr(buf, A64_TMP0, A64_X14);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);
  emit_ldrb_w_disp(buf, A64_X14, A64_TMP0, (uint32_t)offsetof(call_frame, has_caller));
  size_t j_boundary = emit_cbz_w_placeholder(buf, A64_X14);

  emit_ldr_w_disp(buf, A64_X14, A64_TMP0, (uint32_t)offsetof(call_frame, caller_dst));
  emit_mul_w_imm32(buf, A64_X14, REG_SLOT_BYTES);
  emit_uxtw_x_w(buf, A64_X14, A64_X14);

  if(wide) emit_mov_x_rr(buf, A64_X15, (unsigned)ret_gpr);
  else emit_mov_w_rr(buf, A64_X15, (unsigned)ret_gpr);

  emit_sp_adjust(buf, 0, 16);
  emit_str_x_sp(buf, A64_X14, 0);
  emit_str_x_sp(buf, A64_X15, 8);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)jit_current_frame_mem);
  emit_mov_x_rr(buf, (unsigned)JIT_REG_BASE, A64_X0);

  emit_ldr_x_sp(buf, A64_X14, 0);
  emit_ldr_x_sp(buf, A64_X15, 8);
  emit_sp_adjust(buf, 1, 16);
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)JIT_REG_BASE);
  emit_add_x_rr(buf, A64_TMP1, A64_X14);
  if(wide) emit_str_x_disp(buf, A64_X15, A64_TMP1, 0);
  else emit_str_w_disp(buf, A64_X15, A64_TMP1, 0);

  if(ctx->survive_any_armed) {
    emit_ldr_x_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16); /* add rsp, 16 */
  }
  emit_ret_insn_a64(buf);

  size_t boundary = buf->len;
  patch_cbz_w(buf, j_boundary, boundary );

  emit_store_out(ctx, rk, ret_vreg);
  size_t at = emit_b_placeholder(buf);
  push_ret_jump(ctx, at);
}


static int emit_call_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins, jello_jit_ir_op op) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t callee_fi = (op == JIR_CALL_SELF) ? ctx->fi : ins->imm;
  uint32_t caller_dst = ins->a;
  uint32_t arg_first = ins->b;
  uint32_t nargs = ins->c;
  const jello_bc_function* callee_f = &ctx->m->funcs[callee_fi];

  jit_reg_state_discard_currents(ctx->ra);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_w_imm32(buf, A64_X1, callee_fi);
  emit_call_fn(buf, (void*)jit_layout_ptr_or_null);
  emit_mov_x_rr(buf, A64_LAYOUT, A64_X0);
  emit_tst_x_rr(buf, A64_LAYOUT, A64_LAYOUT);
  size_t j_no_layout = emit_b_cond_placeholder(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_layout = buf->len;
  patch_b_cond(buf, j_no_layout, cont_layout );
  emit_ldr_w_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));

  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_cmp_w_imm32(buf, A64_X10, JIT_CALL_FRAMES_MAX);
  size_t j_frames = emit_b_cond_placeholder(buf, 0x82, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_frames = buf->len;
  patch_b_cond(buf, j_frames, cont_frames );

  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov_w_rr(buf, A64_X12, A64_X10);
  emit_add_w_rr(buf, A64_X12, A64_X11);
  emit_mov_w_imm32(buf, A64_X9, JIT_FRAME_STACK_MAX);
  emit_cmp_w_rr(buf, A64_X12, A64_X9);
  size_t j_max = emit_b_cond_placeholder(buf, 0x86, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_max = buf->len;
  patch_b_cond(buf, j_max, cont_max );

  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, frame_stack_cap));
  emit_cmp_w_rr(buf, A64_X12, A64_X9);
  size_t j_cap_ok = emit_b_cond_placeholder(buf, 0x86, 0);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_rr(buf, A64_X1, A64_X11);
  emit_call_fn(buf, (void*)jit_try_grow_frame_stack);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  size_t j_grow_ok = emit_b_cond_placeholder(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_grow = buf->len;
  patch_b_cond(buf, j_grow_ok, cont_grow );
  emit_reload_jit_reg_base(buf);

  size_t cap_ok = buf->len;
  patch_b_cond(buf, j_cap_ok, cap_ok );

  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_cap));
  emit_cmp_w_rr(buf, A64_X10, A64_X9);
  size_t j_cf_ok = emit_b_cond_placeholder(buf, 0x82, 0);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)jit_try_grow_call_frames);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  size_t j_cf_grow = emit_b_cond_placeholder(buf, 0x85, 0);
  emit_jmp_stack_overflow(ctx);
  size_t cont_cf = buf->len;
  patch_b_cond(buf, j_cf_grow, cont_cf );
  size_t cf_ok = buf->len;
  patch_b_cond(buf, j_cf_ok, cf_ok );

  emit_ldr_w_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));

  emit_ldr_x_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, frame_stack));
  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_uxtw_x_w(buf, A64_X15, A64_X10);
  emit_add_x_rr(buf, A64_X9, A64_X15);
  emit_mov_x_rr(buf, A64_X15, A64_X9);

  emit_ldr_w_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, has_pointer));
  emit_tst_w_rr(buf, A64_X11, A64_X11);
  size_t j_no_ptr = emit_b_cond_placeholder(buf, 0x84, 0); /* JE: has_pointer == 0 */
  emit_mov_x_rr(buf, A64_X0, A64_X15);
  emit_ldr_w_disp(buf, A64_X1, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
  emit_ldr_x_disp(buf, A64_X2, A64_LAYOUT, (uint32_t)offsetof(frame_layout, kind));
  emit_mov_w_imm32(buf, A64_X3, callee_f->nregs);
  emit_call_fn(buf, (void*)jit_frame_zero_and_null_init);
  emit_ldr_x_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, frame_stack));
  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_uxtw_x_w(buf, A64_X10, A64_X10);
  emit_add_x_rr(buf, A64_X9, A64_X10);
  emit_mov_x_rr(buf, A64_X15, A64_X9);
  size_t after_zero = buf->len;
  patch_b_cond(buf, j_no_ptr, after_zero );

  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_ldr_w_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
  emit_add_w_rr(buf, A64_X10, A64_X11);
  emit_str_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));

  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames));
  emit_uxtw_x_w(buf, A64_X10, A64_X10);
  emit_mov_x_imm64_reg(buf, A64_X11, sizeof(call_frame));
  emit_mul_x_rr(buf, A64_X10, A64_X11);
  emit_add_x_rr(buf, A64_X9, A64_X10);

  emit_mov_x_imm64_reg(buf, A64_X11, (uint64_t)(uintptr_t)callee_f);
  emit_str_x_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, f));
  emit_mov_w_imm32(buf, A64_X11, callee_fi);
  emit_str_w_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, func_index));
  emit_mov_w_imm32(buf, A64_X11, 0u);
  emit_str_w_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, pc));
  emit_mov_w_imm32(buf, A64_X11, 1u);
  emit_str_w_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, has_caller));
  emit_ldr_w_disp(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, has_pointer));
  emit_str_w_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, has_pointer));
  emit_mov_w_imm32(buf, A64_X11, caller_dst);
  emit_str_w_disp(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, caller_dst));

  emit_ldr_x_disp(buf, A64_X10, A64_LAYOUT, (uint32_t)offsetof(frame_layout, kind));
  emit_str_x_disp(buf, A64_X10, A64_X9, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, kind));
  emit_ldr_x_disp(buf, A64_X10, A64_LAYOUT, (uint32_t)offsetof(frame_layout, wide));
  emit_str_x_disp(buf, A64_X10, A64_X9, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, wide));
  emit_ldr_w_disp(buf, A64_X10, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
  emit_str_w_disp(buf, A64_X10, A64_X9, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, size));
  emit_str_x_disp(buf, A64_X15, A64_X9, (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));

  emit_ldr_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_mov_w_imm32(buf, A64_X11, 1u);
  emit_add_w_rr(buf, A64_X10, A64_X11);
  emit_str_w_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));

  emit_mov_x_rr(buf, A64_X12, (unsigned)JIT_REG_BASE);
  emit_mov_x_rr(buf, (unsigned)JIT_REG_BASE, A64_X15);
  for(uint32_t ai = 0; ai < nargs; ai++) {
    uint32_t src_disp = (arg_first + ai) * REG_SLOT_BYTES;
    uint32_t dst_disp = ai * REG_SLOT_BYTES;
    jello_type_kind ak = vm_reg_kind(ctx->m, callee_f, ai);
    if(jello_slot_size(ak) == 8u) {
      emit_ldr_x_disp(buf, A64_X9, A64_X12, src_disp);
      emit_str_x_base(buf, A64_X9, dst_disp);
    } else {
      emit_ldr_w_disp(buf, A64_X9, A64_X12, src_disp);
      emit_str_w_base(buf, A64_X9, dst_disp);
    }
  }

  emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, 16));
  emit_u32(buf, a64_str_x_uimm(A64_LR, A64_SP, 8));
  size_t call_at = emit_bl_placeholder(buf);
  emit_u32(buf, a64_ldr_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, 16));
  if(!push_call_patch(ctx, callee_fi, call_at)) return 0;

  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_tst_w_rr(buf, A64_X9, A64_X9);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_PASSTHROUGH); /* JNE: trap_code != NONE */

  jit_reg_state_discard_currents(ctx->ra);
  (void)jit_reg_get_gpr(ctx->ra, ins->a, 1);
  return 1;
}

static void emit_f16_binop(emit_ctx* ctx, const jello_jit_ir_insn* ins, uint8_t sse_op) {
  jit_exec_buf* buf = ctx->buf;
  emit_sp_adjust(buf, 0, 16);
  emit_load_u32_slot_to_w1(buf, ins->b);
  emit_mov_w_rr(buf, A64_X0, A64_X1);
  emit_call_fn(buf, (void*)vm_f16_to_f32);
  emit_u32(buf, a64_str_s_uimm(0, A64_SP, 0)); /* spill raw s0 (first value) */
  emit_load_u32_slot_to_w1(buf, ins->c);
  emit_mov_w_rr(buf, A64_X0, A64_X1);
  emit_call_fn(buf, (void*)vm_f16_to_f32); /* raw s0 now holds the second value */
  emit_u32(buf, a64_ldr_s_uimm(9, A64_SP, 0)); /* raw s9 = reload first value */
  if(sse_op == 0x58) emit_u32(buf, a64_fadd_s(9, 9, 0));
  else if(sse_op == 0x5C) emit_u32(buf, a64_fsub_s(9, 9, 0));
  else if(sse_op == 0x59) emit_u32(buf, a64_fmul_s(9, 9, 0));
  else emit_u32(buf, a64_fdiv_s(9, 9, 0));
  emit_u32(buf, a64_fmov_s(0, 9));
  emit_call_fn(buf, (void*)vm_f32_to_f16);
  emit_sp_adjust(buf, 1, 16);
  jit_reg_state_clobber_call(ctx->ra);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bin_f32(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, 0);
  int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  unsigned di = (unsigned)(dx - (int)JIT_PREG_XMM0);
  emit_fmov_s(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_fbin_s(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_fbin_s(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_fbin_s(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_fbin_s(ctx->buf, 0x5E, di, ri); break;
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
  emit_fmov_d(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_fbin_d(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_fbin_d(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_fbin_d(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_fbin_d(ctx->buf, 0x5E, di, ri); break;
    default: return;
  }
  jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
}

static void emit_cmp_float(emit_ctx* ctx, const jello_jit_ir_insn* ins, int is_f64) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, is_f64);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, is_f64);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  if(is_f64) emit_fcmp_d(ctx->buf, li, ri);
  else emit_fcmp_s(ctx->buf, li, ri);
  int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x92, (unsigned)d);
  emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
}


static int emit_conv(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jir_conv)ins->imm) {
    case JIR_CONV_TRUNC_I8: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_w_imm32(ctx->buf, 0x04, (unsigned)d, 0xFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_TRUNC_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_w_imm32(ctx->buf, 0x04, (unsigned)d, 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_sxth_w(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_I32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_sxtw_x_w(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_scvtf_s(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_s(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_scvtf_d(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 0);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_d(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_fcvt_d_s(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0), (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F32_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_fcvt_s_d(ctx->buf, (unsigned)(dx - (int)JIT_PREG_XMM0), (unsigned)(sx - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dx);
      return 1;
    }
    case JIR_CONV_F64_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 1);
      emit_scvtf_d(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F64: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_d(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_scvtf_s(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0), (unsigned)s, 1);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      return 1;
    }
    case JIR_CONV_I64_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_s(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov_x_rr(ctx->buf, A64_X0, (unsigned)s);
      emit_call_fn(ctx->buf, (void*)vm_f16_to_f32);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_fmov_s_from_s0(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_fmov_s_to_s0(ctx->buf, (unsigned)(sx - (int)JIT_PREG_XMM0));
      emit_call_fn(ctx->buf, (void*)vm_f32_to_f16);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, A64_X0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_scvtf_s_to_s0(ctx->buf, (unsigned)s, 0);
      emit_call_fn(ctx->buf, (void*)vm_f32_to_f16);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, A64_X0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_I32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov_x_rr(ctx->buf, A64_X0, (unsigned)s);
      emit_call_fn(ctx->buf, (void*)vm_f16_to_f32);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_from_s0(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    default:
      return 0;
  }
}


/* -- Bytes non-allocating ops -- */

static void emit_bytes_null_check(emit_ctx* ctx, unsigned bytes_gpr) {
  emit_tst_x_rr(ctx->buf, bytes_gpr, bytes_gpr);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
}

static void emit_bytes_len(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_bytes_null_check(ctx, (unsigned)bp);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_ldr_w_disp(buf, (unsigned)dst, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bytes_get_u8(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_bytes_null_check(ctx, (unsigned)bp);
  int idx = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_ldr_w_disp(buf, A64_TMP0, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp_w_rr(buf, (unsigned)idx, A64_TMP0);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS); /* JAE: idx >= length */
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)bp);
  emit_mov_w_rr(buf, A64_TMP2, (unsigned)idx);
  emit_add_x_rr(buf, A64_TMP1, A64_TMP2);
  emit_alu_x_imm32(buf, 0x00, A64_TMP1, (uint32_t)offsetof(jello_bytes, data));
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_ldrb_w_disp(buf, (unsigned)dst, A64_TMP1, 0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bytes_set_u8(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_bytes_null_check(ctx, (unsigned)bp);
  int idx = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_ldr_w_disp(buf, A64_TMP0, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp_w_rr(buf, (unsigned)idx, A64_TMP0);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS); /* JAE: idx >= length */
  int val = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_cmp_w_imm32(buf, (unsigned)val, 255u);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); /* JA: val > 255 */
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)bp);
  emit_mov_w_rr(buf, A64_TMP2, (unsigned)idx);
  emit_add_x_rr(buf, A64_TMP1, A64_TMP2);
  emit_alu_x_imm32(buf, 0x00, A64_TMP1, (uint32_t)offsetof(jello_bytes, data));
  emit_strb_w_disp(buf, (unsigned)val, A64_TMP1, 0);
}

static void emit_bytes_eq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);

  emit_cmp_x_rr(buf, (unsigned)b, (unsigned)c);
  size_t j_same = emit_b_cond_placeholder(buf, 0x84, 0); /* JE -> true */

  emit_tst_x_rr(buf, (unsigned)b, (unsigned)b);
  size_t j_b_null = emit_b_cond_placeholder(buf, 0x84, 0); /* -> false */
  emit_tst_x_rr(buf, (unsigned)c, (unsigned)c);
  size_t j_c_null = emit_b_cond_placeholder(buf, 0x84, 0); /* -> false */

  emit_ldr_w_disp(buf, A64_TMP0, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  emit_ldr_w_disp(buf, A64_TMP1, (unsigned)c, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp_w_rr(buf, A64_TMP0, A64_TMP1);
  size_t j_len_ne = emit_b_cond_placeholder(buf, 0x85, 0); /* JNE -> false */

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)b);
  emit_alu_x_imm32(buf, 0x00, A64_TMP0, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)c);
  emit_alu_x_imm32(buf, 0x00, A64_TMP1, (uint32_t)offsetof(jello_bytes, data));
  emit_ldr_w_disp(buf, A64_TMP2, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, A64_TMP0);
  emit_mov_x_rr(buf, A64_X1, A64_TMP1);
  emit_mov_x_rr(buf, A64_X2, A64_TMP2);
  emit_call_fn(buf, (void*)memcmp);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  emit_setcc_from_flags(buf, 0x94, (unsigned)dst); /* dst = (memcmp == 0) */
  emit_mov_w_rr(buf, (unsigned)dst, (unsigned)dst);
  size_t j_done1 = emit_b_placeholder(buf);

  size_t false_lbl = buf->len;
  patch_b_cond(buf, j_b_null, false_lbl );
  patch_b_cond(buf, j_c_null, false_lbl );
  patch_b_cond(buf, j_len_ne, false_lbl );
  emit_mov_w_imm32(buf, (unsigned)dst, 0u);
  size_t j_done2 = emit_b_placeholder(buf);

  size_t true_lbl = buf->len;
  patch_b_cond(buf, j_same, true_lbl );
  emit_mov_w_imm32(buf, (unsigned)dst, 1u);

  size_t done_lbl = buf->len;
  patch_b(buf, j_done1, done_lbl );
  patch_b(buf, j_done2, done_lbl );

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
  emit_bytes_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_null_check(ctx, (unsigned)c);
  int op_kind = bytes_bitop_kind(ins->imm);

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)b);
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_rr(buf, A64_X1, A64_TMP0);
  emit_mov_x_rr(buf, A64_X2, A64_TMP1);
  emit_mov_w_imm32(buf, A64_X3, (uint32_t)op_kind);
  emit_call_fn(buf, (void*)jit_bytes_bitop);
  emit_tst_x_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_PASSTHROUGH); /* NULL: trap_code already set */
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
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
  emit_ldr_w_disp(buf, A64_TMP0, (unsigned)bytes_gpr, (uint32_t)offsetof(jello_bytes, length));
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off_gpr);
  emit_alu_x_imm32(buf, 0x00, A64_TMP1, width);
  emit_cmp_x_rr(buf, A64_TMP1, A64_TMP0);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); /* JA: off + width > length */
}

static void emit_bytes_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width; int be, is_float;
  bytes_rw_info((jello_jir_bytes_rw)ins->imm, &width, &be, &is_float);
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_bytes_null_check(ctx, (unsigned)bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_bounds_check(ctx, bp, off, width);

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)bp);
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);
  emit_alu_x_imm32(buf, 0x00, A64_TMP0, (uint32_t)offsetof(jello_bytes, data));

  if(!is_float) {
    int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
    if(width == 2) emit_ldrh_w_disp(buf, (unsigned)dst, A64_TMP0, 0);
    else if(width == 4) emit_ldr_w_disp(buf, (unsigned)dst, A64_TMP0, 0);
    else emit_ldr_x_disp(buf, (unsigned)dst, A64_TMP0, 0);
    if(be) {
      if(width == 8) emit_rev_x(buf, (unsigned)dst);
      else if(width == 4) emit_rev_w(buf, (unsigned)dst);
      else emit_rev16_w(buf, (unsigned)dst, 8);
    }
    if(width == 8) jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
    else jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
  } else {
    if(width == 4) emit_ldr_w_disp(buf, A64_TMP1, A64_TMP0, 0);
    else emit_ldr_x_disp(buf, A64_TMP1, A64_TMP0, 0);
    if(be) { if(width == 8) emit_rev_x(buf, A64_TMP1); else emit_rev_w(buf, A64_TMP1); }
    int dst = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, width == 8);
    if(width == 4) {
      emit_fmov_s_w(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), A64_TMP1);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dst);
    } else {
      emit_fmov_d_x(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), A64_TMP1);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dst);
    }
  }
}

static void emit_bytes_write(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width; int be, is_float;
  bytes_rw_info((jello_jir_bytes_rw)ins->imm, &width, &be, &is_float);
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_bytes_null_check(ctx, (unsigned)bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_bounds_check(ctx, bp, off, width);

  if(!is_float) {
    int val = jit_reg_get_gpr(ctx->ra, ins->a, 1);
    emit_mov_x_rr(buf, A64_TMP2, (unsigned)val);
  } else {
    int val = jit_reg_get_xmm(ctx->ra, ins->a, 1, width == 8);
    if(width == 4) emit_fmov_w_s(buf, A64_TMP2, (unsigned)(val - (int)JIT_PREG_XMM0));
    else emit_fmov_x_d(buf, A64_TMP2, (unsigned)(val - (int)JIT_PREG_XMM0));
  }
  if(be) {
    if(width == 8) emit_rev_x(buf, A64_TMP2);
    else if(width == 4) emit_rev_w(buf, A64_TMP2);
    else emit_rev16_w(buf, A64_TMP2, 8);
  }

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)bp);
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);
  emit_alu_x_imm32(buf, 0x00, A64_TMP0, (uint32_t)offsetof(jello_bytes, data));

  if(width == 2) emit_strh_w_disp(buf, A64_TMP2, A64_TMP0, 0);
  else if(width == 4) emit_str_w_disp(buf, A64_TMP2, A64_TMP0, 0);
  else emit_str_x_disp(buf, A64_TMP2, A64_TMP0, 0);
}

static void emit_bytes_blit(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int dst_bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_bytes_null_check(ctx, (unsigned)dst_bp);
  int src_bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_bytes_null_check(ctx, (unsigned)src_bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);

  emit_ldr_w_disp(buf, A64_TMP0, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, length));
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off);
  emit_add_x_rr(buf, A64_TMP1, A64_TMP0);
  emit_ldr_w_disp(buf, A64_TMP2, (unsigned)dst_bp, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp_x_rr(buf, A64_TMP1, A64_TMP2);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); /* JA: off + src->length > dst->length */

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)dst_bp);
  emit_mov_w_rr(buf, A64_TMP1, (unsigned)off);
  emit_add_x_rr(buf, A64_TMP0, A64_TMP1);
  emit_alu_x_imm32(buf, 0x00, A64_TMP0, (uint32_t)offsetof(jello_bytes, data));
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)src_bp);
  emit_alu_x_imm32(buf, 0x00, A64_TMP1, (uint32_t)offsetof(jello_bytes, data));
  emit_ldr_w_disp(buf, A64_TMP2, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, length));

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, A64_TMP0);
  emit_mov_x_rr(buf, A64_X1, A64_TMP1);
  emit_mov_x_rr(buf, A64_X2, A64_TMP2);
  emit_call_fn(buf, (void*)memmove);
}

static void emit_bits_write(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_bytes_null_check(ctx, (unsigned)bp);
  int val = jit_reg_get_gpr(ctx->ra, ins->b, 1);

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)bp);
  emit_ldr_w_disp(buf, A64_TMP1, A64_TMP0, (uint32_t)offsetof(jello_bytes, length));
  emit_lsl_x_imm3(buf, A64_TMP1, A64_TMP1);
  emit_mov_x_imm64_reg(buf, A64_IP1, end_bit);
  emit_cmp_x_rr(buf, A64_IP1, A64_TMP1);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS); /* JA: end_bit > length*8 */

  emit_mov_w_rr(buf, A64_TMP2, (unsigned)val);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, A64_TMP0);
  emit_mov_w_imm32(buf, A64_X1, width);
  emit_mov_w_imm32(buf, A64_X2, bit_off);
  emit_mov_w_rr(buf, A64_X3, A64_TMP2);
  emit_call_fn(buf, (void*)jit_bits_write);
}

static void emit_bytes_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int len = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_w_rr(buf, A64_TMP0, (unsigned)len);
  emit_ldr_w_vm(buf, A64_TMP1, (uint32_t)offsetof(jello_vm, max_bytes_len));
  emit_tst_w_rr(buf, A64_TMP1, A64_TMP1);
  size_t limit_skip = emit_b_cond_placeholder(buf, 0x84, 0); /* EQ: max_bytes_len == 0, disabled */
  emit_cmp_w_rr(buf, A64_TMP0, A64_TMP1);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_LIMIT); /* HI: len > max_bytes_len -- trap-PC captured centrally, Sec 4.3 */
  patch_b_cond(buf, limit_skip, buf->len);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_w_rr(buf, A64_X1, A64_TMP0);
  emit_call_fn(buf, (void*)jello_bytes_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bytes_concat2(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_bytes_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_null_check(ctx, (unsigned)c);
  emit_mov_x_rr(buf, A64_TMP0, (unsigned)b);
  emit_mov_x_rr(buf, A64_TMP1, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_rr(buf, A64_X1, A64_TMP0);
  emit_mov_x_rr(buf, A64_X2, A64_TMP1);
  emit_call_fn(buf, (void*)jit_bytes_concat2);
  emit_tst_x_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_PASSTHROUGH); /* NULL: trap_code already set (BOUNDS) */
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bits_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_bytes_null_check(ctx, (unsigned)bp);

  emit_mov_x_rr(buf, A64_TMP0, (unsigned)bp);
  emit_ldr_w_disp(buf, A64_TMP1, A64_TMP0, (uint32_t)offsetof(jello_bytes, length));
  emit_lsl_x_imm3(buf, A64_TMP1, A64_TMP1);
  emit_mov_x_imm64_reg(buf, A64_IP1, end_bit);
  emit_cmp_x_rr(buf, A64_IP1, A64_TMP1);
  emit_jcc_trap(ctx, 0x87, JIT_TRAP_BOUNDS);

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, A64_TMP0);
  emit_mov_w_imm32(buf, A64_X1, width);
  emit_mov_w_imm32(buf, A64_X2, bit_off);
  emit_call_fn(buf, (void*)jit_bits_read);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
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
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_w_imm32(buf, A64_X2, struct_sig_id);
  emit_call_fn(buf, (void*)jello_struct_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_struct_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id struct_tid = ctx->f->reg_types[ins->b];
  uint32_t sig_id = ctx->m->types[struct_tid].as.strct.struct_sig_id;
  const jello_struct_sig* sig = &ctx->m->struct_sigs[sig_id];
  uint32_t field_off = sig->field_offset[ins->imm];
  jello_type_kind fk = ctx->m->types[sig->field_type[ins->imm]].kind;
  uint32_t width = (uint32_t)jello_slot_size(fk);
  uint32_t data_off = (uint32_t)offsetof(jello_struct, fields) + field_off;

  int sp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)sp, (unsigned)sp);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);

  if(is_xmm_field_kind(fk)) {
    int dst = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, width == 8);
    if(width == 8) {
      emit_ldr_d_disp(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), (unsigned)sp, data_off);
      jit_reg_store_home_xmm64(ctx->ra, ins->a, dst);
    } else {
      emit_ldr_s_disp(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), (unsigned)sp, data_off);
      jit_reg_store_home_xmm32(ctx->ra, ins->a, dst);
    }
  } else {
    int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
    if(width == 8) {
      emit_ldr_x_disp(buf, (unsigned)dst, (unsigned)sp, data_off);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
    } else {
      emit_ldr_w_disp(buf, (unsigned)dst, (unsigned)sp, data_off);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
    }
  }
}

static void emit_struct_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id struct_tid = ctx->f->reg_types[ins->b];
  uint32_t sig_id = ctx->m->types[struct_tid].as.strct.struct_sig_id;
  const jello_struct_sig* sig = &ctx->m->struct_sigs[sig_id];
  uint32_t field_off = sig->field_offset[ins->imm];
  jello_type_kind fk = ctx->m->types[sig->field_type[ins->imm]].kind;
  uint32_t width = (uint32_t)jello_slot_size(fk);
  uint32_t data_off = (uint32_t)offsetof(jello_struct, fields) + field_off;

  int sp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)sp, (unsigned)sp);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);

  if(is_xmm_field_kind(fk)) {
    int val = jit_reg_get_xmm(ctx->ra, ins->a, 1, width == 8);
    if(width == 8) emit_str_d_disp(buf, (unsigned)(val - (int)JIT_PREG_XMM0), (unsigned)sp, data_off);
    else emit_str_s_disp(buf, (unsigned)(val - (int)JIT_PREG_XMM0), (unsigned)sp, data_off);
  } else {
    int val = jit_reg_get_gpr(ctx->ra, ins->a, 1);
    if(width == 8) emit_str_x_disp(buf, (unsigned)val, (unsigned)sp, data_off);
    else emit_str_w_disp(buf, (unsigned)val, (unsigned)sp, data_off);
  }
}

/* -- the Dynamic boundary -- */
static void emit_current_rf_ptr_into_r11(jit_exec_buf* buf) {
  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_ldr_x_vm(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov_w_imm32(buf, A64_X11, 1u);
  emit_sub_w_rr(buf, A64_X9, A64_X11);
  emit_mov_x_imm64_reg(buf, A64_X11, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_X15, A64_X9);
  emit_mul_x_rr(buf, A64_X15, A64_X11);
  emit_add_x_rr(buf, A64_X15, A64_X10);
  emit_alu_x_imm32(buf, 0x00, A64_X15, (uint32_t)offsetof(call_frame, rf));
}

static void emit_to_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_current_rf_ptr_into_r11(buf);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_X15);
  emit_mov_w_imm32(buf, A64_X4, ins->b);
  emit_call_fn(buf, (void*)vm_box_from_typed);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_from_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X13, (unsigned)src);
  emit_current_rf_ptr_into_r11(buf);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_X15);
  emit_mov_w_imm32(buf, A64_X4, ins->a);
  emit_mov_x_rr(buf, A64_X5, A64_X13);
  emit_call_fn(buf, (void*)vm_store_from_boxed);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_PASSTHROUGH); /* 0: trap_code already set (TYPE_MISMATCH) */

  jello_type_kind dst_kind = vm_reg_kind(ctx->m, ctx->f, ins->a);
  if(dst_kind == JELLO_T_F32) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 0);
  else if(dst_kind == JELLO_T_F64) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 1);
  else (void)jit_reg_get_gpr(ctx->ra, ins->a, 1);
}

static void emit_kindof(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_x_rr(buf, A64_X0, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (void*)vm_kindof_dynamic);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_physeq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_current_rf_ptr_into_r11(buf);
  emit_mov_x_rr(buf, A64_LAYOUT, A64_X15);

  emit_sp_adjust(buf, 0, 16); /* sub rsp, 16 */
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_LAYOUT);
  emit_mov_w_imm32(buf, A64_X4, ins->b);
  emit_call_fn(buf, (void*)vm_box_from_typed);
  emit_str_x_sp(buf, A64_X0, 0);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_LAYOUT);
  emit_mov_w_imm32(buf, A64_X4, ins->c);
  emit_call_fn(buf, (void*)vm_box_from_typed);
  emit_ldr_x_sp(buf, A64_X10, 0);
  emit_sp_adjust(buf, 1, 16); /* add rsp, 16 */

  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_cmp_x_rr(buf, A64_X10, A64_X0);
  emit_setcc_from_flags(buf, 0x94, (unsigned)dst);
  emit_mov_w_rr(buf, (unsigned)dst, (unsigned)dst);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/* -- Object ops -- */
static void emit_obj_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id tid = ctx->f->reg_types[ins->a];
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_w_imm32(buf, A64_X1, tid);
  emit_call_fn(buf, (void*)jello_object_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_obj_has_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)o);
  emit_mov_w_imm32(buf, A64_X1, ins->imm);
  emit_call_fn(buf, (void*)jello_object_has);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_get_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X2, A64_LAYOUT);
  emit_mov_w_imm32(buf, A64_X3, ins->imm);
  emit_call_fn(buf, (void*)jit_obj_chain_get);
  emit_mov_x_rr(buf, A64_LAYOUT, A64_X0); /* v -- o no longer needed */

  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_tst_w_rr(buf, A64_X9, A64_X9);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_PASSTHROUGH); /* JNE: trap_code != NONE */

  emit_current_rf_ptr_into_r11(buf);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_X15);
  emit_mov_w_imm32(buf, A64_X4, ins->a);
  emit_mov_x_rr(buf, A64_X5, A64_LAYOUT);
  emit_call_fn(buf, (void*)vm_store_from_boxed);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_PASSTHROUGH);

  jello_type_kind dst_kind = vm_reg_kind(ctx->m, ctx->f, ins->a);
  if(dst_kind == JELLO_T_F32) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 0);
  else if(dst_kind == JELLO_T_F64) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 1);
  else (void)jit_reg_get_gpr(ctx->ra, ins->a, 1);
}

static void emit_obj_set_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);
  emit_current_rf_ptr_into_r11(buf);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_X15);
  emit_mov_w_imm32(buf, A64_X4, ins->a);
  emit_call_fn(buf, (void*)vm_box_from_typed);

  emit_mov_x_rr(buf, A64_X4, A64_X0);
  emit_mov_x_imm64_reg(buf, A64_X0, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X1, A64_LAYOUT);
  emit_mov_w_imm32(buf, A64_X2, ins->imm);
  emit_mov_x_rr(buf, A64_X3, A64_X4);
  emit_call_fn(buf, (void*)jit_obj_set_direct);
}

static void emit_obj_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_w_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS); /* JAE: atom_id >= natoms */
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);
  emit_mov_w_rr(buf, A64_X13, (unsigned)atom);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X2, A64_LAYOUT);
  emit_mov_w_rr(buf, A64_X3, A64_X13);
  emit_call_fn(buf, (void*)jit_obj_chain_get);
  emit_mov_x_rr(buf, A64_LAYOUT, A64_X0);

  emit_ldr_w_vm(buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_tst_w_rr(buf, A64_X9, A64_X9);
  emit_jcc_trap(ctx, 0x85, JIT_TRAP_PASSTHROUGH);

  emit_current_rf_ptr_into_r11(buf);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_X15);
  emit_mov_w_imm32(buf, A64_X4, ins->a);
  emit_mov_x_rr(buf, A64_X5, A64_LAYOUT);
  emit_call_fn(buf, (void*)vm_store_from_boxed);
  emit_tst_w_rr(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_PASSTHROUGH);

  jello_type_kind dst_kind = vm_reg_kind(ctx->m, ctx->f, ins->a);
  if(dst_kind == JELLO_T_F32) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 0);
  else if(dst_kind == JELLO_T_F64) (void)jit_reg_get_xmm(ctx->ra, ins->a, 1, 1);
  else (void)jit_reg_get_gpr(ctx->ra, ins->a, 1);
}

static void emit_obj_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_w_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_LAYOUT, (unsigned)o);
  emit_sp_adjust(buf, 0, 16); /* sub rsp, 16 */
  emit_mov_w_rr(buf, A64_X13, (unsigned)atom);
  emit_str_x_sp(buf, A64_X13, 0);
  emit_current_rf_ptr_into_r11(buf);

  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_imm64_reg(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_imm64_reg(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov_x_rr(buf, A64_X3, A64_X15);
  emit_mov_w_imm32(buf, A64_X4, ins->a);
  emit_call_fn(buf, (void*)vm_box_from_typed);
  emit_ldr_x_sp(buf, A64_X5, 0);
  emit_sp_adjust(buf, 1, 16); /* add rsp, 16 */

  emit_mov_x_rr(buf, A64_X3, A64_X0);
  emit_mov_x_imm64_reg(buf, A64_X0, (uint64_t)(uintptr_t)ctx->m);
  emit_mov_x_rr(buf, A64_X1, A64_LAYOUT);
  emit_mov_w_rr(buf, A64_X2, A64_X5);
  emit_call_fn(buf, (void*)jit_obj_set_direct);
}

static void emit_obj_has(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_w_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  emit_mov_x_rr(buf, A64_X0, (unsigned)o);
  emit_mov_w_rr(buf, A64_X1, (unsigned)atom);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (void*)jello_object_has);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_remove(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  int atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp_w_imm32(buf, (unsigned)atom, ctx->m->natoms);
  emit_jcc_trap(ctx, 0x83, JIT_TRAP_BOUNDS);
  emit_mov_x_rr(buf, A64_X0, (unsigned)o);
  emit_mov_w_rr(buf, A64_X1, (unsigned)atom);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (void*)jello_object_remove);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_clear(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)o, (unsigned)o);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  emit_mov_x_rr(buf, A64_X0, (unsigned)o);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (void*)jello_object_clear);
}

static void emit_obj_copy(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_tst_x_rr(buf, (unsigned)src, (unsigned)src);
  emit_jcc_trap(ctx, 0x84, JIT_TRAP_NULL);
  emit_mov_x_rr(buf, A64_TMP0, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov_x_rr(buf, A64_X1, A64_TMP0);
  emit_call_fn(buf, (void*)jello_object_copy);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
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
        emit_mov_x_rr(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      } else {
        emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      }
      break;
    }
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_I64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_x_imm64_reg(ctx->buf, (unsigned)d, (uint64_t)ctx->m->const_i64[ins->imm]);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F16: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm & 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      uint64_t bits;
      memcpy(&bits, &ctx->m->const_f64[ins->imm], 8);
      emit_mov_x_imm64_reg(ctx->buf, (unsigned)d, bits);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_BOOL: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm ? 1u : 0u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_BIN_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu_w_rr(ctx->buf, 0x01, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu_w_rr(ctx->buf, 0x29, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_mul_w_rr(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu_w_rr(ctx->buf, 0x21, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu_w_rr(ctx->buf, 0x09, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu_w_rr(ctx->buf, 0x31, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_DIV: case JIR_BIN_MOD: {
          emit_divmod_guard_i32(ctx, (unsigned)rhs, (unsigned)lhs);
          emit_u32(ctx->buf, a64_sdiv_w(A64_TMP0, (uint8_t)lhs, (uint8_t)rhs));
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD) {
            emit_u32(ctx->buf, a64_msub_w((uint8_t)dst, A64_TMP0, (uint8_t)rhs, (uint8_t)lhs));
          } else {
            emit_mov_w_rr(ctx->buf, (uint8_t)dst, A64_TMP0);
          }
          break;
        }
        case JIR_BIN_SHL: case JIR_BIN_SHR: {
          int cnt = jit_reg_get_gpr(ctx->ra, ins->c, 1);
          emit_mov_w_rr(ctx->buf, A64_TMP0, (unsigned)cnt);
          emit_u32(ctx->buf, a64_and_w_imm31(A64_TMP0, A64_TMP0));
          if((jello_jir_bin)ins->imm == JIR_BIN_SHL) emit_lslv_w(ctx->buf, (unsigned)dst);
          else emit_asrv_w(ctx->buf, (unsigned)dst);
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
      emit_mov_x_rr(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu_x_rr(ctx->buf, 0x01, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu_x_rr(ctx->buf, 0x29, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_mul_x_rr(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu_x_rr(ctx->buf, 0x21, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu_x_rr(ctx->buf, 0x09, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu_x_rr(ctx->buf, 0x31, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_DIV: case JIR_BIN_MOD: {
          emit_divmod_guard_i64(ctx, (unsigned)rhs, (unsigned)lhs);
          emit_u32(ctx->buf, a64_sdiv_x(A64_TMP0, (uint8_t)lhs, (uint8_t)rhs));
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD) {
            emit_u32(ctx->buf, a64_msub_x((uint8_t)dst, A64_TMP0, (uint8_t)rhs, (uint8_t)lhs));
          } else {
            emit_mov_x_rr(ctx->buf, (uint8_t)dst, A64_TMP0);
          }
          break;
        }
        case JIR_BIN_SHL: case JIR_BIN_SHR: {
          int cnt = jit_reg_get_gpr(ctx->ra, ins->c, 1);
          emit_mov_w_rr(ctx->buf, A64_TMP0, (unsigned)cnt);
          emit_u32(ctx->buf, a64_and_x_imm63(A64_TMP0, A64_TMP0));
          if((jello_jir_bin)ins->imm == JIR_BIN_SHL) emit_lslv_x(ctx->buf, (unsigned)dst);
          else emit_asrv_x(ctx->buf, (unsigned)dst);
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
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_w_imm32(ctx->buf, 0x00, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_SUB_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu_w_imm32(ctx->buf, 0x05, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_MUL_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_mul_w_imm32(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg_w(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_x_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg_x(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_eor_w_imm32(ctx->buf, (unsigned)d, 0x80000000u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_x_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_mov_x_imm64_reg(ctx->buf, A64_TMP0, 0x8000000000000000ULL);
      emit_alu_x_rr(ctx->buf, 0x31, (unsigned)d, A64_TMP0);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NOT_BOOL: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)s);
      emit_eor_w_imm32(ctx->buf, (unsigned)d, 1u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_w_rr(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x9C, (unsigned)d);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_EQ_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_w_imm32(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc_from_flags(ctx->buf, 0x94, (unsigned)d);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I64: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_x_rr(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? 0x94 : 0x9C, (unsigned)d);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_LT_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp_w_imm32(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc_from_flags(ctx->buf, 0x9C, (unsigned)d);
      emit_mov_w_rr(ctx->buf, (unsigned)d, (unsigned)d);
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
      emit_mov_x_imm64_reg(ctx->buf, (unsigned)d, (uint64_t)JELLO_TAG_NULL);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_ATOM: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov_w_imm32(ctx->buf, (unsigned)d, ins->imm);
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
      size_t at = emit_b_placeholder(buf);
      if(!push_branch(&ctx, at, tgt, 0)) ok = 0;
      goto insn_done;
    }
    if(op == JIR_JMP_IF) {
      uint32_t tgt = 0;
      if(!resolve_target_bc(bc, (int32_t)ins->imm, f->ninsns, &tgt)) { ok = 0; break; }
      int cond = jit_reg_get_gpr(&ra, ins->a, 1);
      size_t at = emit_cbnz_w_placeholder(buf, (unsigned)cond);
      if(!push_branch(&ctx, at, tgt, 2)) ok = 0;
      goto insn_done;
    }
    if(op == JIR_SAFEPOINT) {
      emit_mov_x_rr(buf, A64_X0, (unsigned)JIT_REG_VM);
      emit_call_fn(buf, (void*)vm_safepoint_check);
      jit_reg_state_clobber_call(&ra);
      emit_ldr_x_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, fuel_limit));
      emit_tst_x_rr(buf, A64_TMP0, A64_TMP0);
      size_t fuel_skip = emit_b_cond_placeholder(buf, 0x84, 0); /* EQ: fuel_limit == 0, disabled */
      emit_ldr_x_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, fuel_remaining));
      emit_tst_x_rr(buf, A64_TMP0, A64_TMP0);
      emit_jcc_trap(&ctx, 0x84, JIT_TRAP_FUEL); /* EQ: fuel_remaining == 0 -- trap-PC captured centrally, Sec 4.3 */
      emit_u32(buf, a64_sub_x_imm(A64_TMP0, A64_TMP0, 1));
      emit_str_x_vm(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, fuel_remaining));
      patch_b_cond(buf, fuel_skip, buf->len);
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
      patch_b(buf, at, epilogue_ok_off );
    }
    for(size_t i = 0; i < ctx.branch_count; i++) {
      size_t at = ctx.branches[i].at;
      uint32_t tgt = ctx.branches[i].bc_tgt;
      if(tgt >= f->ninsns || native_off[tgt] == (size_t)-1) { ok = 0; break; }
      size_t tgt_off = native_off[tgt];
      if(ctx.branches[i].is_jcc == 2)
        patch_cbnz_w(buf, at, tgt_off);
      else if(ctx.branches[i].is_jcc)
        patch_b_cond(buf, at, tgt_off);
      else
        patch_b(buf, at, tgt_off);
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
  patch_bl(buf, at, rel);
}

void jello_jit_patch_call(jit_exec_buf* buf, size_t site_off, size_t callee_off) {
  int64_t delta = (int64_t)callee_off - (int64_t)site_off;
  assert((delta & 3) == 0);
  int32_t off_insns = (int32_t)(delta / 4);
  assert(off_insns >= -67108864 && off_insns <= 67108863);
  uint32_t word = a64_bl(off_insns);
  assert((!buf->finalized || buf->patch_open) &&
      "Patched a finalized jit_exec_buf outside an open jit_exec_buf_begin_patch/end_patch window");
  memcpy(buf->mem + site_off, &word, 4);
}

void jello_jit_code_free(jello_jit_code* code) {
  free(code->loop_anchor_pcs);
  free(code->loop_anchor_offs);
  code->loop_anchor_pcs = NULL;
  code->loop_anchor_offs = NULL;
  code->nloop_anchors = 0;
  jit_free_exec_buf(&code->buf);
}
