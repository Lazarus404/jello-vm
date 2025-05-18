// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * arm64 JIT emitter; compiles a function's JIR to native code with its prologue, trap and return
 * exits, and emits the enter, bridge and lazy stubs. */

#include <jello/internal/jit_emit.h>
#include <jello/internal/jit_arm64.h>
#include <jello.h>
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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
  if(buf->mem && !buf->finalized && buf->limit - buf->len >= 4u) {
    memcpy(buf->mem + buf->len, &word, 4);
    buf->len += 4;
    return;
  }
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

static void emit_mov64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mov_x_rr((uint8_t)dst, (uint8_t)src));
}

static void emit_call_fn(jit_exec_buf* buf, uintptr_t fn) {
  emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, 16));
  emit_u32(buf, a64_str_x_uimm(A64_LR, A64_SP, 8));
  emit_mov_x_imm64(buf, A64_IP0, (uint64_t)fn);
  emit_u32(buf, a64_blr_x(A64_IP0));
  emit_u32(buf, a64_ldr_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, 16));
#ifdef JELLO_JIT_SCRIBBLE_CALLS
  emit_mov_x_imm64(buf, A64_IP0, 0x5c5c5c5c5c5c5c5cull);
  for(uint8_t r = 1; r < A64_IP0; r++) emit_mov64(buf, r, A64_IP0);
  emit_mov64(buf, A64_IP1, A64_IP0);
#endif
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
  emit_mov32_imm(buf, A64_X0, imm);
}

static void emit_mov_x_imm64_reg(jit_exec_buf* buf, unsigned dst, uint64_t imm) {
  emit_mov_x_imm64(buf, (uint8_t)dst, imm);
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

static void emit_store_imm32_to_vm_field(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  emit_mov_w_imm32(buf, A64_TMP1, imm);
  emit_str_w_vm(buf, A64_TMP1, disp);
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

static void emit_store_out32(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  emit_u32(buf, a64_str_w_uimm((uint8_t)src, (uint8_t)JIT_REG_OUT, disp));
}

static void emit_store_out32_imm(jit_exec_buf* buf, uint32_t disp, uint32_t imm) {
  emit_mov32_imm(buf, A64_TMP1, imm);
  emit_store_out32(buf, disp, A64_TMP1);
}

static void emit_store_out64(jit_exec_buf* buf, uint32_t disp, unsigned src) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, (uint8_t)JIT_REG_OUT, disp));
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

static void emit_add_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_sub_w_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sub_w_rr((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_alu_x_imm32(jit_exec_buf* buf, uint8_t op_ext, unsigned dst, uint32_t imm) {
  if(op_ext == 0x00 && imm <= 0xFFFu) emit_u32(buf, a64_add_x_imm((uint8_t)dst, (uint8_t)dst, imm));
  else {
    emit_mov_w_imm32(buf, A64_TMP0, imm);
    if(op_ext == 0x00) emit_u32(buf, a64_add_x_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
    else emit_u32(buf, a64_sub_x_rr((uint8_t)dst, (uint8_t)dst, A64_TMP0));
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

static void emit_test32(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_w_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_test64(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_x_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_setcc_from_flags(jit_exec_buf* buf, uint8_t cc_opcode, unsigned dst) {
  emit_u32(buf, a64_cset_w((uint8_t)dst, cc_opcode == JIT_SETL ? A64_LT : A64_EQ));
}

static void emit_mul_x_rr(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_mul_x((uint8_t)dst, (uint8_t)dst, (uint8_t)src));
}

static void emit_sext32to64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sxtw_x_w((uint8_t)dst, (uint8_t)src));
}

static void emit_sext16(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_sxth_w((uint8_t)dst, (uint8_t)src));
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

static void emit_store16(jit_exec_buf* buf, unsigned src, unsigned base, uint32_t disp) {
  assert(disp_encodes(disp, 2));
  emit_u32(buf, a64_strh_w_uimm((uint8_t)src, (uint8_t)base, disp));
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

static void emit_fmov32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fmov_s(pool_dreg_i(dst), pool_dreg_i(src)));
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

static void emit_scvtf_d(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_d_x(pool_dreg_i(xmm), (uint8_t)gpr));
  else emit_u32(buf, a64_scvtf_d_w(pool_dreg_i(xmm), (uint8_t)gpr));
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

static void emit_fbin32(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t dd = pool_dreg_i(dst), dn = pool_dreg_i(dst), dm = pool_dreg_i(src);
  if(op == 0x58) emit_u32(buf, a64_fadd_s(dd, dn, dm));
  else if(op == 0x5C) emit_u32(buf, a64_fsub_s(dd, dn, dm));
  else if(op == 0x59) emit_u32(buf, a64_fmul_s(dd, dn, dm));
  else if(op == 0x5E) emit_u32(buf, a64_fdiv_s(dd, dn, dm));
}

static void emit_fmov64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fmov_d(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_fbin64(jit_exec_buf* buf, uint8_t op, unsigned dst, unsigned src) {
  uint8_t dd = pool_dreg_i(dst), dn = pool_dreg_i(dst), dm = pool_dreg_i(src);
  if(op == 0x58) emit_u32(buf, a64_fadd_d(dd, dn, dm));
  else if(op == 0x5C) emit_u32(buf, a64_fsub_d(dd, dn, dm));
  else if(op == 0x59) emit_u32(buf, a64_fmul_d(dd, dn, dm));
  else if(op == 0x5E) emit_u32(buf, a64_fdiv_d(dd, dn, dm));
}

static void emit_fcmp32(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_fcmp_s(pool_dreg_i(a), pool_dreg_i(b)));
}

static void emit_fcmp64(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_fcmp_d(pool_dreg_i(a), pool_dreg_i(b)));
}

static void emit_int_to_f32(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_s_x(pool_dreg_i(xmm), (uint8_t)gpr));
  else emit_u32(buf, a64_scvtf_s_w(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_int_to_f64(jit_exec_buf* buf, unsigned xmm, unsigned gpr, int wide) {
  if(wide) emit_u32(buf, a64_scvtf_d_x(pool_dreg_i(xmm), (uint8_t)gpr));
  else emit_u32(buf, a64_scvtf_d_w(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_f32_to_int(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  if(wide) emit_u32(buf, a64_fcvtzs_x_s((uint8_t)gpr, pool_dreg_i(xmm)));
  else emit_u32(buf, a64_fcvtzs_w_s((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_f64_to_int(jit_exec_buf* buf, unsigned gpr, unsigned xmm, int wide) {
  if(wide) emit_u32(buf, a64_fcvtzs_x_d((uint8_t)gpr, pool_dreg_i(xmm)));
  else emit_u32(buf, a64_fcvtzs_w_d((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_f32_to_f64(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fcvt_d_s(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_f64_to_f32(jit_exec_buf* buf, unsigned dst, unsigned src) {
  emit_u32(buf, a64_fcvt_s_d(pool_dreg_i(dst), pool_dreg_i(src)));
}

static void emit_fmov_from32(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  emit_u32(buf, a64_fmov_s_x(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_fmov_to32(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  emit_u32(buf, a64_fmov_x_s((uint8_t)gpr, pool_dreg_i(xmm)));
}

static void emit_fmov_from64(jit_exec_buf* buf, unsigned xmm, unsigned gpr) {
  emit_u32(buf, a64_fmov_d_x(pool_dreg_i(xmm), (uint8_t)gpr));
}

static void emit_fmov_to64(jit_exec_buf* buf, unsigned gpr, unsigned xmm) {
  emit_u32(buf, a64_fmov_x_d((uint8_t)gpr, pool_dreg_i(xmm)));
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
  emit_store_vm32_imm(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
  emit_store_vm32_imm(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  size_t at = emit_b_placeholder(ctx->buf);
  jit_push_stack_overflow_jump(ctx, at);
}

static size_t emit_branch_if_native_room(jit_exec_buf* buf) {
  emit_load_vm64(buf, A64_X9, (uint32_t)offsetof(jello_vm, native_stack_limit));
  emit_u32(buf, 0xEB2063FFu | ((uint32_t)A64_X9 << 16)); // cmp sp, x9, uxtx
  return emit_b_cond_placeholder(buf, JIT_JAE);          // HS: at or above the limit
}

static void emit_native_stack_check(emit_ctx* ctx) {
  size_t fits = emit_branch_if_native_room(ctx->buf);
  emit_jmp_stack_overflow(ctx);
  jit_patch_jump(ctx->buf, fits, ctx->buf->len);
}

static void emit_bridge_when_deep(emit_ctx* ctx, unsigned target) {
  jit_exec_buf* buf = ctx->buf;
  size_t fits = emit_branch_if_native_room(buf);
  emit_mov64_imm(buf, target, (uint64_t)(uintptr_t)&ctx->entries[ctx->m->nfuncs]);
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)target, (uint8_t)target, 0));
  jit_patch_jump(buf, fits, buf->len);
}

static void emit_jcc_trap(emit_ctx* ctx, uint8_t cc, uint8_t kind) {
  size_t skip = emit_b_cond_placeholder(ctx->buf, (uint8_t)(cc ^ 1u));
  if(kind == JIT_TRAP_CALLEE) {
    emit_store_vm32_imm(ctx->buf, JIT_TRAP_SITE_OFF, ctx->cur_bc_pc + 1u);
  } else if(kind != JIT_TRAP_PASSTHROUGH) {
    emit_store_vm32_imm(ctx->buf, (uint32_t)offsetof(jello_vm, trap_func_index), ctx->fi);
    emit_store_vm32_imm(ctx->buf, (uint32_t)offsetof(jello_vm, trap_pc), ctx->cur_bc_pc);
  }
  size_t at = emit_b_placeholder(ctx->buf);
  jit_patch_jump(ctx->buf, skip, ctx->buf->len);
  jit_push_trap_jump(ctx, at, kind);
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
  emit_b_cond_placeholder(ctx->buf, JIT_JNE);
  emit_cmp32_imm(ctx->buf, lhs_gpr, (uint32_t)INT32_MIN);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  jit_patch_jump(ctx->buf, jne, ok);
}

static void emit_divmod_guard_i64(emit_ctx* ctx, unsigned div_gpr, unsigned lhs_gpr) {
  emit_test64(ctx->buf, div_gpr, div_gpr);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_ARITH);
  emit_cmp_x_eq_neg1(ctx->buf, div_gpr);
  size_t jne = ctx->buf->len;
  emit_b_cond_placeholder(ctx->buf, JIT_JNE);
  emit_mov64_imm(ctx->buf, A64_TMP0, (uint64_t)INT64_MIN);
  emit_cmp64(ctx->buf, lhs_gpr, A64_TMP0);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_ARITH);
  size_t ok = ctx->buf->len;
  jit_patch_jump(ctx->buf, jne, ok);
}

static void emit_save_fp_pool(jit_exec_buf* buf) {
  for(uint8_t d = 8; d < 16; d = (uint8_t)(d + 2u))
    emit_u32(buf, a64_stp_d_pre(d, (uint8_t)(d + 1u), A64_SP, -2));
}

static void emit_restore_fp_pool(jit_exec_buf* buf) {
  for(uint8_t d = 14; d >= 8; d = (uint8_t)(d - 2u))
    emit_u32(buf, a64_ldp_d_post(d, (uint8_t)(d + 1u), A64_SP, 2));
}

void jit_emit_prologue(jit_exec_buf* buf, size_t* out_body_off, int any_armed) {
  emit_u32(buf, a64_stp_x_pre(A64_FP, A64_LR, A64_SP, -2));
  emit_u32(buf, a64_mov_x_rr(A64_FP, A64_SP));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, -2));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, -2));
  emit_save_fp_pool(buf);
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_VM, A64_X0));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_BASE, A64_X1));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_OUT, A64_X2));
  *out_body_off = buf->len;
  if(any_armed) {
    emit_sp_adjust(buf, 0, 16);
    emit_store64_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_u32(buf, a64_add_x_imm(A64_TMP1, A64_SP, 0));
    emit_u32(buf, 0xF2400C1Fu | ((uint32_t)A64_TMP1 << 5)); // tst x16, #0xf
    emit_u32(buf, a64_b_cond(A64_EQ, 2));
    emit_u32(buf, 0xD4200000u); // brk #0
  }
}

static void emit_restore_callee_saved(jit_exec_buf* buf, int any_armed) {
  if(any_armed) {
    emit_load64_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_restore_fp_pool(buf);
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post(A64_FP, A64_LR, A64_SP, 2));
}

static void emit_store_out(emit_ctx* ctx, jello_type_kind ret_kind, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  emit_store_out32_imm(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                       (uint32_t)JELLO_NUM_VOID);
  if(ret_kind == JELLO_T_BOOL) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out32_imm(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                         (uint32_t)JELLO_NUM_BOOL);
    emit_store_out32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out32_imm(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                         (uint32_t)JELLO_NUM_I32);
    emit_store_out32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_I64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out32_imm(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                         (uint32_t)JELLO_NUM_I64);
    emit_store_out64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F32) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out32_imm(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                         (uint32_t)JELLO_NUM_F32);
    emit_store_out32(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_F64) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out32_imm(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                         (uint32_t)JELLO_NUM_F64);
    emit_store_out64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_BYTES || ret_kind == JELLO_T_STRUCT || ret_kind == JELLO_T_OBJECT) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out32_imm(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                         (uint32_t)JELLO_NUM_PTR);
    emit_store_out64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  } else if(ret_kind == JELLO_T_DYNAMIC) {
    int r = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);
    emit_store_out32_imm(buf, (uint32_t)offsetof(jello_numeric_result, kind),
                         (uint32_t)JELLO_NUM_DYN);
    emit_store_out64(buf, (uint32_t)offsetof(jello_numeric_result, as), (unsigned)r);
  }
}

void jit_emit_ok_exit(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)vm_pop_frame);
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_OK);
  emit_ret_insn_a64(buf);
}

static void emit_reload_jit_reg_base(jit_exec_buf* buf) {
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_current_frame_mem);
  emit_mov64(buf, (unsigned)JIT_REG_BASE, A64_X0);
}

static size_t emit_jump_if_no_caller_tmp0(jit_exec_buf* buf) {
  emit_load_vm32(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_load_vm64(buf, A64_TMP1, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov32_imm(buf, A64_X14, 1u);
  emit_sub32(buf, A64_TMP0, A64_X14);
  emit_mov64_imm(buf, A64_X14, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_TMP0, A64_TMP0);
  emit_mul64(buf, A64_TMP0, A64_X14);
  emit_add64(buf, A64_TMP0, A64_TMP1);
  emit_load8(buf, A64_X14, A64_TMP0, (uint32_t)offsetof(call_frame, has_caller));
  return emit_cbz_w_placeholder(buf, A64_X14);
}

static void emit_trap_tail(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  size_t j_boundary = emit_jump_if_no_caller_tmp0(buf);

  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_pop_frame);
  emit_reload_jit_reg_base(buf);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  if(ctx->survive_any_armed) {
    emit_load64_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_ret_insn_a64(buf);

  size_t boundary = buf->len;
  jit_patch_jump(buf, j_boundary, boundary);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_trap_pop_frame);
  emit_restore_callee_saved(buf, ctx->survive_any_armed);
  emit_mov_w0_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  emit_ret_insn_a64(buf);
}

void jit_emit_trap_exit(emit_ctx* ctx, uintptr_t trap_fn) {
  jit_exec_buf* buf = ctx->buf;
  if(trap_fn) {
    emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, trap_fn);
  }
  emit_trap_tail(ctx);
}

static void emit_ret_insn(emit_ctx* ctx, uint8_t ret_vreg) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind rk = ctx->ret_kind;
  int wide = jello_slot_size(rk) == 8u; // pointers too, not only I64/F64
  int ret_gpr = jit_reg_get_gpr(ctx->ra, ret_vreg, 1);

  if(wide) emit_mov64(buf, A64_TMP2, (unsigned)ret_gpr);
  else emit_mov32(buf, A64_TMP2, (unsigned)ret_gpr);
  jit_reg_bind(ctx->ra, ret_vreg, A64_TMP2);
  ret_gpr = A64_TMP2;

  size_t j_boundary = emit_jump_if_no_caller_tmp0(buf);

  emit_mov64(buf, A64_X13, A64_TMP0);
  emit_load32(buf, A64_X14, A64_X13, (uint32_t)offsetof(call_frame, caller_dst));
  emit_mul32_imm(buf, A64_X14, REG_SLOT_BYTES);
  emit_uxtw_x_w(buf, A64_X14, A64_X14);

  if(wide) emit_mov64(buf, A64_X15, (unsigned)ret_gpr);
  else emit_mov32(buf, A64_X15, (unsigned)ret_gpr);

  emit_load32(buf, A64_X9, A64_X13, (uint32_t)offsetof(call_frame, pop_work));
  size_t j_try = emit_cbnz_w_placeholder(buf, A64_X9);
  emit_load64(buf, A64_X9, A64_X13, (uint32_t)offsetof(call_frame, spill_slots));
  emit_test64(buf, A64_X9, A64_X9);
  size_t j_spill = emit_b_cond_placeholder(buf, JIT_JNE); // NE: spill slots to free
  emit_load32(buf, A64_X9, A64_X13,
              (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, size));
  emit_load_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_sub32(buf, A64_X10, A64_X9);
  emit_store_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_load_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_alu32_imm(buf, JIT_ALU_SUB, A64_X10, 1u);
  emit_store_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_u32(buf, a64_sub_x_imm(A64_X13, A64_X13, (uint32_t)sizeof(call_frame)));
  emit_load64(buf, (unsigned)JIT_REG_BASE, A64_X13,
              (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));
  size_t j_popped = emit_b_placeholder(buf);
  jit_patch_jump(buf, j_try, buf->len);
  jit_patch_jump(buf, j_spill, buf->len);

  emit_sp_adjust(buf, 0, 16);
  emit_store64_sp(buf, A64_X14, 0);
  emit_store64_sp(buf, A64_X15, 8);

  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)vm_pop_frame);
  emit_reload_jit_reg_base(buf);

  emit_load64_sp(buf, A64_X14, 0);
  emit_load64_sp(buf, A64_X15, 8);
  emit_sp_adjust(buf, 1, 16);
  jit_patch_jump(buf, j_popped, buf->len);
  emit_mov64(buf, A64_TMP1, (unsigned)JIT_REG_BASE);
  emit_add64(buf, A64_TMP1, A64_X14);
  if(wide) emit_store64(buf, A64_X15, A64_TMP1, 0);
  else emit_store32(buf, A64_X15, A64_TMP1, 0);

  if(ctx->survive_any_armed) {
    emit_load64_sp(buf, (unsigned)JIT_REG_SURVIVE, 0);
    emit_sp_adjust(buf, 1, 16);
  }
  emit_ret_insn_a64(buf);

  size_t boundary = buf->len;
  jit_patch_jump(buf, j_boundary, boundary);

  emit_store_out(ctx, rk, ret_vreg);
  size_t at = emit_b_placeholder(buf);
  jit_push_ret_jump(ctx, at);
}

static void emit_fill_call_frame(jit_exec_buf* buf, const jello_bc_function* f, uint32_t fi,
                                 int has_pointer, uint32_t total, uint32_t caller_dst) {
  emit_load_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_load_vm64(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames));
  emit_uxtw_x_w(buf, A64_X12, A64_X10);
  emit_mov64_imm(buf, A64_X11, sizeof(call_frame));
  emit_mul64(buf, A64_X12, A64_X11);
  emit_add64(buf, A64_X9, A64_X12);

  if(f) emit_mov64_imm(buf, A64_X11, (uint64_t)(uintptr_t)f);
  else emit_load64(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, f));
  emit_store64(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, f));
  unsigned fi_reg = A64_X14;
  if(f) {
    emit_mov32_imm(buf, A64_X11, fi);
    fi_reg = A64_X11;
  }
  emit_store32(buf, fi_reg, A64_X9, (uint32_t)offsetof(call_frame, func_index));
  emit_store32(buf, 31u, A64_X9, (uint32_t)offsetof(call_frame, pc)); // wzr
  _Static_assert(sizeof(((call_frame*)0)->has_caller) == 1 &&
                     sizeof(((call_frame*)0)->has_pointer) == 1,
                 "the frame's flags are stored as bytes");
  emit_mov32_imm(buf, A64_X11, 1u);
  emit_store8(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, has_caller));
  if(f) emit_mov32_imm(buf, A64_X11, (uint32_t)has_pointer);
  else emit_load32(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, has_pointer));
  emit_store8(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, has_pointer));
  emit_mov32_imm(buf, A64_X11, caller_dst);
  emit_store32(buf, A64_X11, A64_X9, (uint32_t)offsetof(call_frame, caller_dst));

  emit_load64(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, kind));
  emit_store64(buf, A64_X11, A64_X9,
               (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, kind));
  emit_load64(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, wide));
  emit_store64(buf, A64_X11, A64_X9,
               (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, wide));
  if(f) emit_mov32_imm(buf, A64_X11, total);
  else emit_load32(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
  emit_store32(buf, A64_X11, A64_X9,
               (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, size));
  emit_store64(buf, A64_X15, A64_X9,
               (uint32_t)offsetof(call_frame, rf) + (uint32_t)offsetof(reg_frame, mem));

  emit_alu32_imm(buf, JIT_ALU_ADD, A64_X10, 1u);
  emit_store_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
}

static void emit_copy_args(emit_ctx* ctx, const jello_bc_function* kinds_f, uint32_t kinds_first,
                           uint32_t arg_first, uint32_t nargs) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov64(buf, A64_X12, (unsigned)JIT_REG_BASE);
  emit_mov64(buf, (unsigned)JIT_REG_BASE, A64_X15);
  for(uint32_t ai = 0; ai < nargs; ai++) {
    uint32_t src_disp = (arg_first + ai) * REG_SLOT_BYTES;
    uint32_t dst_disp = ai * REG_SLOT_BYTES;
    if(jello_slot_size(vm_reg_kind(ctx->m, kinds_f, kinds_first + ai)) == 8u) {
      emit_load64(buf, A64_X9, A64_X12, src_disp);
      emit_str_x_base(buf, A64_X9, dst_disp);
    } else {
      emit_load32(buf, A64_X9, A64_X12, src_disp);
      emit_str_w_base(buf, A64_X9, dst_disp);
    }
  }
}

static void emit_call_jit_code(jit_exec_buf* buf, unsigned target) {
  emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, 16));
  emit_u32(buf, a64_str_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_blr_x((uint8_t)target));
  emit_u32(buf, a64_ldr_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, 16));
}

static void emit_after_jit_call(emit_ctx* ctx, uint8_t dst) {
  emit_load_vm32(ctx->buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test32(ctx->buf, A64_X9, A64_X9);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_CALLEE);
  jit_reg_state_discard_currents(ctx->ra);
  (void)jit_reg_get_gpr(ctx->ra, dst, 1);
}

static void emit_frame_tops(jit_exec_buf* buf, uint32_t total) {
  emit_load_vm32(buf, A64_X11, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_mov64(buf, A64_X12, A64_X11);
  emit_alu32_imm(buf, JIT_ALU_ADD, A64_X12, total);
}

static int emit_call_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins, jello_jit_ir_op op) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t callee_fi = (op == JIR_CALL_SELF) ? ctx->fi : ins->imm;
  uint32_t caller_dst = ins->a;
  uint32_t arg_first = ins->b;
  uint32_t nargs = ins->c;
  const jello_bc_function* callee_f = &ctx->m->funcs[callee_fi];
  if(!ctx->entries) return 0;

  jit_reg_state_discard_currents(ctx->ra);

  emit_load_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, jit_layout_by_func_len));
  emit_cmp32_imm(buf, A64_X10, callee_fi);
  size_t j_indexed = emit_b_cond_placeholder(buf, JIT_JA); // HI: callee_fi < len
  emit_jmp_stack_overflow(ctx);
  jit_patch_jump(buf, j_indexed, buf->len);
  emit_load_vm64(buf, A64_LAYOUT, (uint32_t)offsetof(jello_vm, jit_layout_by_func));
  uint32_t layout_disp = callee_fi * (uint32_t)sizeof(frame_layout*);
  unsigned layout_base = emit_disp_base(buf, A64_LAYOUT, A64_LAYOUT, A64_TMP0, &layout_disp, 8);
  emit_load64(buf, A64_LAYOUT, layout_base, layout_disp);
  emit_test64(buf, A64_LAYOUT, A64_LAYOUT);
  size_t j_no_layout = emit_b_cond_placeholder(buf, JIT_JNE);
  emit_jmp_stack_overflow(ctx);
  jit_patch_jump(buf, j_no_layout, buf->len);

  uint32_t total = callee_f->nregs * REG_SLOT_BYTES;
  int has_pointer = vm_func_has_pointer(ctx->m, callee_f);

  emit_load_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_load_vm32(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_limit));
  emit_cmp32(buf, A64_X10, A64_TMP0);
  size_t j_frames = emit_b_cond_placeholder(buf, JIT_JB); // LO: below the frame limit
  emit_jmp_stack_overflow(ctx);
  jit_patch_jump(buf, j_frames, buf->len);
  emit_frame_tops(buf, total);
  emit_load_vm32(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, frame_stack_limit));
  emit_cmp32(buf, A64_X12, A64_TMP0);
  size_t j_grow = emit_b_cond_placeholder(buf, JIT_JA); // HI: past what is committed
  emit_load_vm32(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_cap));
  emit_cmp32(buf, A64_X10, A64_X9);
  size_t j_room = emit_b_cond_placeholder(buf, JIT_JB); // LO: a free slot
  jit_patch_jump(buf, j_grow, buf->len);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64(buf, A64_X1, A64_X12);
  emit_call_fn(buf, (uintptr_t)jit_try_grow_push);
  emit_test32(buf, A64_X0, A64_X0);
  size_t j_grown = emit_b_cond_placeholder(buf, JIT_JNE);
  emit_jmp_stack_overflow(ctx);
  jit_patch_jump(buf, j_grown, buf->len);
  emit_frame_tops(buf, total);
  jit_patch_jump(buf, j_room, buf->len);

  emit_store_vm32(buf, A64_X12, (uint32_t)offsetof(jello_vm, frame_stack_top));
  emit_load_vm64(buf, A64_X9, (uint32_t)offsetof(jello_vm, frame_stack));
  emit_uxtw_x_w(buf, A64_X15, A64_X11);
  emit_add64(buf, A64_X15, A64_X9);

  if(has_pointer) {
    emit_mov64(buf, A64_X0, A64_X15);
    emit_load64(buf, A64_X1, A64_LAYOUT, (uint32_t)offsetof(frame_layout, init));
    emit_mov32_imm(buf, A64_X2, total);
    emit_call_fn(buf, (uintptr_t)memcpy);
    emit_mov64(buf, A64_X15, A64_X0);
  }

  emit_fill_call_frame(buf, callee_f, callee_fi, has_pointer, total, caller_dst);
  emit_copy_args(ctx, callee_f, 0, arg_first, nargs);

  emit_mov64_imm(buf, A64_IP0, (uint64_t)(uintptr_t)&ctx->entries[callee_fi]);
  emit_u32(buf, a64_ldr_x_uimm(A64_IP0, A64_IP0, 0));
  emit_bridge_when_deep(ctx, A64_IP0);
  emit_call_jit_code(buf, A64_IP0);
  emit_after_jit_call(ctx, ins->a);
  return 1;
}

static void emit_obj_kind_guard(jit_exec_buf* buf, unsigned reg, uint32_t kind, size_t* slow,
                                size_t* nslow) {
  emit_mov32_imm(buf, A64_X9, 7u);
  emit_test64(buf, reg, A64_X9);
  slow[(*nslow)++] = emit_b_cond_placeholder(buf, JIT_JNE); // NE: not a pointer
  emit_test64(buf, reg, reg);
  slow[(*nslow)++] = emit_b_cond_placeholder(buf, JIT_JE); // EQ: NULL
  emit_load8(buf, A64_X9, reg, (uint32_t)offsetof(jello_obj_header, kind));
  emit_cmp32_imm(buf, A64_X9, kind);
  slow[(*nslow)++] = emit_b_cond_placeholder(buf, JIT_JNE);
}

static int emit_callr_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
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
    emit_load64(buf, A64_X13, (unsigned)JIT_REG_BASE, callee_disp);
    emit_obj_kind_guard(buf, A64_X13, (uint32_t)JELLO_OBJ_FUNCTION, slow, &nslow);
    emit_load32(buf, A64_X14, A64_X13, (uint32_t)offsetof(jello_function, func_index));
    emit_load_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, jit_layout_by_func_len));
    emit_cmp32(buf, A64_X10, A64_X14);
    slow[nslow++] = emit_b_cond_placeholder(buf, JIT_JBE); // LS: no layout index
    emit_load_vm64(buf, A64_LAYOUT, (uint32_t)offsetof(jello_vm, jit_layout_by_func));
    emit_uxtw_x_w(buf, A64_X9, A64_X14);
    emit_lsl_x_imm3(buf, A64_X9, A64_X9);
    emit_add64(buf, A64_LAYOUT, A64_X9);
    emit_load64(buf, A64_LAYOUT, A64_LAYOUT, 0);
    emit_test64(buf, A64_LAYOUT, A64_LAYOUT);
    slow[nslow++] = emit_b_cond_placeholder(buf, JIT_JE);
    emit_load_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames_len));
    emit_load_vm32(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, call_frames_limit));
    emit_cmp32(buf, A64_X10, A64_TMP0);
    slow[nslow++] = emit_b_cond_placeholder(buf, JIT_JAE); // HS
    emit_load_vm32(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_cap));
    emit_cmp32(buf, A64_X10, A64_X9);
    slow[nslow++] = emit_b_cond_placeholder(buf, JIT_JAE);
    emit_load_vm32(buf, A64_X11, (uint32_t)offsetof(jello_vm, frame_stack_top));
    emit_load32(buf, A64_X12, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
    emit_u32(buf, a64_add_w_rr(A64_X12, A64_X12, A64_X11));
    emit_load_vm32(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, frame_stack_limit));
    emit_cmp32(buf, A64_X12, A64_TMP0);
    slow[nslow++] = emit_b_cond_placeholder(buf, JIT_JA); // HI: does not fit

    emit_store_vm32(buf, A64_X12, (uint32_t)offsetof(jello_vm, frame_stack_top));
    emit_load_vm64(buf, A64_X9, (uint32_t)offsetof(jello_vm, frame_stack));
    emit_uxtw_x_w(buf, A64_X15, A64_X11);
    emit_add64(buf, A64_X15, A64_X9);
    emit_load32(buf, A64_X9, A64_LAYOUT, (uint32_t)offsetof(frame_layout, has_pointer));
    emit_test32(buf, A64_X9, A64_X9);
    size_t j_no_ptr = emit_b_cond_placeholder(buf, JIT_JE);
    emit_mov64(buf, A64_X0, A64_X15);
    emit_load64(buf, A64_X1, A64_LAYOUT, (uint32_t)offsetof(frame_layout, init));
    emit_load32(buf, A64_X2, A64_LAYOUT, (uint32_t)offsetof(frame_layout, total));
    emit_call_fn(buf, (uintptr_t)memcpy);
    emit_mov64(buf, A64_X15, A64_X0);
    emit_load64(buf, A64_X13, (unsigned)JIT_REG_BASE, callee_disp);
    emit_load32(buf, A64_X14, A64_X13, (uint32_t)offsetof(jello_function, func_index));
    jit_patch_jump(buf, j_no_ptr, buf->len);

    emit_fill_call_frame(buf, NULL, 0, 0, 0, caller_dst);

    emit_load32(buf, A64_X10, A64_X13, (uint32_t)offsetof(jello_function, ncaps));
    emit_test32(buf, A64_X10, A64_X10);
    size_t j_no_caps = emit_b_cond_placeholder(buf, JIT_JE);
    emit_load64(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(frame_layout, f));
    emit_load32(buf, A64_X11, A64_X11, (uint32_t)offsetof(jello_bc_function, cap_start));
    emit_uxtw_x_w(buf, A64_X11, A64_X11);
    emit_lsl_x_imm3(buf, A64_X11, A64_X11);
    emit_add64(buf, A64_X11, A64_X15);
    emit_u32(buf, a64_add_x_imm(A64_X12, A64_X13, (uint32_t)offsetof(jello_function, captures)));
    size_t loop = buf->len;
    emit_u32(buf, a64_ldr_x_uimm(A64_X9, A64_X12, 0));
    emit_u32(buf, a64_add_x_imm(A64_X12, A64_X12, 8));
    emit_u32(buf, a64_str_x_uimm(A64_X9, A64_X11, 0));
    emit_u32(buf, a64_add_x_imm(A64_X11, A64_X11, 8));
    emit_alu32_imm(buf, JIT_ALU_SUB, A64_X10, 1u);
    emit_test32(buf, A64_X10, A64_X10);
    emit_u32(buf, a64_b_cond(A64_NE, off_insns_at(buf->len, loop)));
    jit_patch_jump(buf, j_no_caps, buf->len);

    emit_copy_args(ctx, caller_f, arg_first, arg_first, nargs);
    emit_mov64_imm(buf, A64_X9, (uint64_t)(uintptr_t)ctx->entries);
    emit_uxtw_x_w(buf, A64_X14, A64_X14);
    emit_lsl_x_imm3(buf, A64_X14, A64_X14);
    emit_add64(buf, A64_X9, A64_X14);
    emit_load64(buf, A64_LAYOUT, A64_X9, 0);
    j_call = emit_b_placeholder(buf);
  }

  for(size_t i = 0; i < nslow; i++) jit_patch_jump(buf, slow[i], buf->len);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov32_imm(buf, A64_X1, caller_dst);
  emit_mov32_imm(buf, A64_X2, ins->b);
  emit_mov32_imm(buf, A64_X3, arg_first);
  emit_mov32_imm(buf, A64_X4, nargs);
  emit_call_fn(buf, (uintptr_t)jit_callr_push);
  emit_mov64(buf, A64_LAYOUT, A64_X0);
  emit_test64(buf, A64_LAYOUT, A64_LAYOUT);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // EQ: refused
  emit_reload_jit_reg_base(buf);
  if(ctx->entries) {
    jit_patch_jump(buf, j_call, buf->len);
    emit_bridge_when_deep(ctx, A64_LAYOUT);
  }
  emit_call_jit_code(buf, A64_LAYOUT);
  emit_after_jit_call(ctx, ins->a);
  return 1;
}

static void emit_try_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov32_imm(buf, A64_X1, (uint32_t)((int32_t)ins->bc_pc + 1 + (int32_t)ins->imm));
  emit_mov32_imm(buf, A64_X2, ins->a);
  emit_call_fn(buf, (uintptr_t)vm_try_push);
  emit_test32(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // EQ: no memory
}

static void emit_endtry_insn(emit_ctx* ctx) {
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(ctx->buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(ctx->buf, (uintptr_t)vm_try_pop);
}

static void emit_throw_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov32_imm(buf, A64_X1, ins->a);
  emit_mov32_imm(buf, A64_X2, ctx->fi);
  emit_mov32_imm(buf, A64_X3, ctx->cur_bc_pc);
  emit_call_fn(buf, (uintptr_t)jit_throw);
  size_t at = emit_b_placeholder(buf);
  jit_push_trap_jump(ctx, at, JIT_TRAP_PASSTHROUGH);
}

void jit_emit_catch_dispatch(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_load_vm32(buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_cmp32_imm(buf, A64_X9, (uint32_t)JELLO_TRAP_INTERNAL_UNWIND);
  size_t not_unwind = emit_b_cond_placeholder(buf, JIT_JNE);
  emit_load_vm32(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_load_vm32(buf, A64_X10, (uint32_t)offsetof(jello_vm, unwind_len));
  emit_cmp32(buf, A64_X9, A64_X10);
  size_t not_here = emit_b_cond_placeholder(buf, JIT_JNE);
  emit_store_vm32_imm(buf, (uint32_t)offsetof(jello_vm, trap_code), (uint32_t)JELLO_TRAP_NONE);
  emit_store_vm32_imm(buf, JIT_TRAP_SITE_OFF, 0u);
  emit_reload_jit_reg_base(buf);
  emit_load_vm32(buf, A64_X9, (uint32_t)offsetof(jello_vm, call_frames_len));
  emit_load_vm64(buf, A64_X10, (uint32_t)offsetof(jello_vm, call_frames));
  emit_mov32_imm(buf, A64_X11, 1u);
  emit_sub32(buf, A64_X9, A64_X11);
  emit_mov64_imm(buf, A64_X11, sizeof(call_frame));
  emit_uxtw_x_w(buf, A64_X15, A64_X9);
  emit_mul64(buf, A64_X15, A64_X11);
  emit_add64(buf, A64_X15, A64_X10);
  emit_load32(buf, A64_X9, A64_X15, (uint32_t)offsetof(call_frame, pc));
  for(uint32_t i = 0; i < ctx->ncatch; i++) {
    emit_cmp32_imm(buf, A64_X9, ctx->catch_pcs[i]);
    size_t at = emit_b_cond_placeholder(buf, JIT_JE);
    jit_push_branch(ctx, at, ctx->catch_pcs[i]);
  }
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_unwind_lost);
  jit_patch_jump(buf, not_unwind, buf->len);
  jit_patch_jump(buf, not_here, buf->len);
  emit_trap_tail(ctx);
}

static void emit_f16_binop(emit_ctx* ctx, const jello_jit_ir_insn* ins, uint8_t sse_op) {
  jit_exec_buf* buf = ctx->buf;
  emit_sp_adjust(buf, 0, 16);
  emit_load_u32_slot_to_w1(buf, ins->b);
  emit_mov32(buf, A64_X0, A64_X1);
  emit_call_fn(buf, (uintptr_t)vm_f16_to_f32);
  emit_u32(buf, a64_str_s_uimm(0, A64_SP, 0)); // str s0, [sp]
  emit_load_u32_slot_to_w1(buf, ins->c);
  emit_mov32(buf, A64_X0, A64_X1);
  emit_call_fn(buf, (uintptr_t)vm_f16_to_f32); // s0 = second operand
  emit_u32(buf, a64_ldr_s_uimm(9, A64_SP, 0)); // ldr s9, [sp]
  if(sse_op == 0x58) emit_u32(buf, a64_fadd_s(9, 9, 0));
  else if(sse_op == 0x5C) emit_u32(buf, a64_fsub_s(9, 9, 0));
  else if(sse_op == 0x59) emit_u32(buf, a64_fmul_s(9, 9, 0));
  else emit_u32(buf, a64_fdiv_s(9, 9, 0));
  emit_u32(buf, a64_fmov_s(0, 9));
  emit_call_fn(buf, (uintptr_t)vm_f32_to_f16);
  emit_sp_adjust(buf, 1, 16);
  jit_reg_state_clobber_call(ctx->ra);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov32(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_bin_f32(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, 0);
  int dx = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  unsigned di = (unsigned)(dx - (int)JIT_PREG_XMM0);
  emit_fmov32(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_fbin32(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_fbin32(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_fbin32(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_fbin32(ctx->buf, 0x5E, di, ri); break;
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
  emit_fmov64(ctx->buf, di, li);
  switch((jello_jir_bin)ins->imm) {
    case JIR_BIN_ADD: emit_fbin64(ctx->buf, 0x58, di, ri); break;
    case JIR_BIN_SUB: emit_fbin64(ctx->buf, 0x5C, di, ri); break;
    case JIR_BIN_MUL: emit_fbin64(ctx->buf, 0x59, di, ri); break;
    case JIR_BIN_DIV: emit_fbin64(ctx->buf, 0x5E, di, ri); break;
    default: return;
  }
  jit_reg_store_home_xmm64(ctx->ra, ins->a, dx);
}

static void emit_cmp_float(emit_ctx* ctx, const jello_jit_ir_insn* ins, int is_f64) {
  int lx = jit_reg_get_xmm(ctx->ra, ins->b, 1, is_f64);
  int rx = jit_reg_get_xmm(ctx->ra, ins->c, 1, is_f64);
  unsigned li = (unsigned)(lx - (int)JIT_PREG_XMM0);
  unsigned ri = (unsigned)(rx - (int)JIT_PREG_XMM0);
  if(is_f64) emit_fcmp64(ctx->buf, li, ri);
  else emit_fcmp32(ctx->buf, li, ri);
  int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  a64_cond cond = (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? A64_EQ : A64_MI;
  emit_u32(ctx->buf, a64_cset_w((uint8_t)d, cond));
  jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
}

static void emit_load_f64(jit_exec_buf* buf, uint8_t dd, double x) {
  uint64_t bits;
  memcpy(&bits, &x, sizeof bits);
  emit_mov_x_imm64(buf, A64_TMP1, bits);
  emit_u32(buf, a64_fmov_d_x(dd, A64_TMP1));
}

static void emit_float_conv_check(emit_ctx* ctx, uint8_t v, int is_f64, int to_i64) {
  jit_exec_buf* buf = ctx->buf;
  if(!is_f64) {
    emit_u32(buf, a64_fcvt_d_s(A64_D_TMP0, v));
    v = A64_D_TMP0;
  }
  emit_load_f64(buf, A64_D_TMP1, to_i64 ? VM_I64_FROM_FLOAT_LO : VM_I32_FROM_FLOAT_LO);
  emit_u32(buf, a64_fcmp_d(A64_D_TMP1, v));
  emit_jcc_trap(ctx, JIT_JAE, JIT_TRAP_ARITH); // HS: lo >= v, or unordered
  emit_load_f64(buf, A64_D_TMP1, to_i64 ? VM_I64_FROM_FLOAT_HI : VM_I32_FROM_FLOAT_HI);
  emit_u32(buf, a64_fcmp_d(v, A64_D_TMP1));
  emit_jcc_trap(ctx, JIT_JAE, JIT_TRAP_ARITH); // HS: v >= hi, or unordered
}

static int emit_conv(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jir_conv)ins->imm) {
    case JIR_CONV_TRUNC_I8: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu32_imm(ctx->buf, JIT_ALU_AND, (unsigned)d, 0xFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_TRUNC_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu32_imm(ctx->buf, JIT_ALU_AND, (unsigned)d, 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_sext16(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_I32_FROM_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_SEXT_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_sext32to64(ctx->buf, (unsigned)d, (unsigned)s);
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
      emit_float_conv_check(ctx, pool_dreg_i((unsigned)(sx - (int)JIT_PREG_XMM0)), 0, 0);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_f32_to_int(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 0);
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
      emit_float_conv_check(ctx, pool_dreg_i((unsigned)(sx - (int)JIT_PREG_XMM0)), 1, 0);
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
      emit_float_conv_check(ctx, pool_dreg_i((unsigned)(sx - (int)JIT_PREG_XMM0)), 1, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_fcvtzs_w_d(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
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
      emit_float_conv_check(ctx, pool_dreg_i((unsigned)(sx - (int)JIT_PREG_XMM0)), 0, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_f32_to_int(ctx->buf, (unsigned)d, (unsigned)(sx - (int)JIT_PREG_XMM0), 1);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      return 1;
    }
    case JIR_CONV_F32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov64(ctx->buf, A64_X0, (unsigned)s);
      emit_call_fn(ctx->buf, (uintptr_t)vm_f16_to_f32);
      int x = jit_reg_alloc_xmm(ctx->ra, ins->a, 0, 0);
      emit_fmov_s_from_s0(ctx->buf, (unsigned)(x - (int)JIT_PREG_XMM0));
      jit_reg_store_home_xmm32(ctx->ra, ins->a, x);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_F32: {
      int sx = jit_reg_get_xmm(ctx->ra, ins->b, 1, 0);
      emit_fmov_s_to_s0(ctx->buf, (unsigned)(sx - (int)JIT_PREG_XMM0));
      emit_call_fn(ctx->buf, (uintptr_t)vm_f32_to_f16);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, A64_X0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_F16_FROM_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_scvtf_s_to_s0(ctx->buf, (unsigned)s, 0);
      emit_call_fn(ctx->buf, (uintptr_t)vm_f32_to_f16);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, A64_X0);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      jit_reg_state_clobber_call(ctx->ra);
      return 1;
    }
    case JIR_CONV_I32_FROM_F16: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      emit_mov64(ctx->buf, A64_X0, (unsigned)s);
      emit_call_fn(ctx->buf, (uintptr_t)vm_f16_to_f32);
      emit_float_conv_check(ctx, 0, 0, 0);
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

static void emit_null_check(emit_ctx* ctx, unsigned gpr) {
  emit_test64(ctx->buf, gpr, gpr);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_NULL);
}

static void emit_buffer_check(emit_ctx* ctx, unsigned gpr) {
  emit_null_check(ctx, gpr);
  emit_load32(ctx->buf, A64_TMP1, gpr, (uint32_t)offsetof(jello_bytes, h.type_id));
  emit_cmp32_imm(ctx->buf, A64_TMP1, JELLO_BYTES_BUFFER);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_TYPE); // NE: no buffer
}

static void emit_bounds_trap(emit_ctx* ctx, unsigned ptr, unsigned idx, uint32_t len_off) {
  emit_load32(ctx->buf, A64_TMP0, ptr, len_off);
  emit_cmp32(ctx->buf, idx, A64_TMP0);
  emit_jcc_trap(ctx, JIT_JAE, JIT_TRAP_BOUNDS); // HS: idx >= length, unsigned
}

static void emit_indexed_addr(emit_ctx* ctx, unsigned ptr, unsigned idx, uint32_t len_off,
                              uint32_t data_off, uint8_t shift) {
  jit_exec_buf* buf = ctx->buf;
  emit_bounds_trap(ctx, ptr, idx, len_off);
  emit_load64(buf, A64_TMP1, ptr, data_off);
  emit_u32(buf, a64_add_x_uxtw(A64_TMP1, A64_TMP1, (uint8_t)idx, shift));
}

static void emit_tag_cmp(jit_exec_buf* buf, unsigned src, uint32_t tag) {
  emit_u32(buf, a64_and_w_imm7(A64_TMP1, (uint8_t)src));
  emit_cmp32_imm(buf, A64_TMP1, tag);
}

static void emit_null_flags(emit_ctx* ctx, uint8_t r) {
  int s = jit_reg_get_gpr(ctx->ra, r, 1);
  if(vm_reg_kind(ctx->m, ctx->f, r) == JELLO_T_DYNAMIC) {
    emit_tag_cmp(ctx->buf, (unsigned)s, (uint32_t)JELLO_TAG_NULL);
  } else {
    emit_test64(ctx->buf, (unsigned)s, (unsigned)s);
  }
}

static void emit_bytes_len(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_ldr_w_disp(buf, (unsigned)dst, (unsigned)bp, (uint32_t)offsetof(jello_bytes, length));
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
  emit_ldrb_w_disp(buf, (unsigned)dst, A64_TMP1, 0);
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
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // HI: val > 255
  emit_store8(buf, (unsigned)val, A64_TMP1, 0);
}

static void emit_bytes_eq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);

  emit_cmp64(buf, (unsigned)b, (unsigned)c);
  size_t j_same = emit_b_cond_placeholder(buf, JIT_JE); // EQ: same object, true

  emit_test64(buf, (unsigned)b, (unsigned)b);
  size_t j_b_null = emit_b_cond_placeholder(buf, JIT_JE); // null: false
  emit_test64(buf, (unsigned)c, (unsigned)c);
  size_t j_c_null = emit_b_cond_placeholder(buf, JIT_JE); // null: false

  emit_load32(buf, A64_TMP0, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  emit_load32(buf, A64_TMP1, (unsigned)c, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp32(buf, A64_TMP0, A64_TMP1);
  size_t j_len_ne = emit_b_cond_placeholder(buf, JIT_JNE); // NE: lengths differ, false

  emit_load64(buf, A64_TMP0, (unsigned)b, (uint32_t)offsetof(jello_bytes, data));
  emit_load64(buf, A64_TMP1, (unsigned)c, (uint32_t)offsetof(jello_bytes, data));
  emit_load32(buf, A64_TMP2, (unsigned)b, (uint32_t)offsetof(jello_bytes, length));
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, A64_TMP0);
  emit_mov64(buf, A64_X1, A64_TMP1);
  emit_mov64(buf, A64_X2, A64_TMP2);
  emit_call_fn(buf, (uintptr_t)memcmp);
  emit_test32(buf, A64_X0, A64_X0);
  emit_setcc_from_flags(buf, JIT_SETE, (unsigned)dst); // dst = (memcmp == 0)
  emit_mov32(buf, (unsigned)dst, (unsigned)dst);
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
  emit_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_null_check(ctx, (unsigned)c);
  int op_kind = bytes_bitop_kind(ins->imm);

  emit_mov64(buf, A64_TMP0, (unsigned)b);
  emit_mov64(buf, A64_TMP1, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64(buf, A64_X1, A64_TMP0);
  emit_mov64(buf, A64_X2, A64_TMP1);
  emit_mov32_imm(buf, A64_X3, (uint32_t)op_kind);
  emit_call_fn(buf, (uintptr_t)jit_bytes_bitop);
  emit_test64(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // NULL: the helper set trap_code
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, A64_X0);
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
  emit_load32(buf, A64_TMP0, (unsigned)bytes_gpr, (uint32_t)offsetof(jello_bytes, length));
  emit_mov32(buf, A64_TMP1, (unsigned)off_gpr);
  emit_alu64_imm(buf, JIT_ALU_ADD, A64_TMP1, width);
  emit_cmp64(buf, A64_TMP1, A64_TMP0);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // HI: off + width > length
}

static void emit_bytes_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width; int be, is_float;
  bytes_rw_info((jello_jir_bytes_rw)ins->imm, &width, &be, &is_float);
  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_bytes_bounds_check(ctx, bp, off, width);

  emit_load64(buf, A64_TMP0, (unsigned)bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov32(buf, A64_TMP1, (unsigned)off);
  emit_add64(buf, A64_TMP0, A64_TMP1);

  if(!is_float) {
    int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
    if(width == 2) emit_load16(buf, (unsigned)dst, A64_TMP0, 0);
    else if(width == 4) emit_load32(buf, (unsigned)dst, A64_TMP0, 0);
    else emit_load64(buf, (unsigned)dst, A64_TMP0, 0);
    if(be) {
      if(width == 8) emit_bswap64(buf, (unsigned)dst);
      else if(width == 4) emit_bswap32(buf, (unsigned)dst);
      else emit_bswap16(buf, (unsigned)dst, 8);
    }
    if(width == 8) jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
    else jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
  } else {
    if(width == 4) emit_load32(buf, A64_TMP1, A64_TMP0, 0);
    else emit_load64(buf, A64_TMP1, A64_TMP0, 0);
    if(be) {
      if(width == 8) emit_bswap64(buf, A64_TMP1);
      else emit_bswap32(buf, A64_TMP1);
    }
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
  emit_buffer_check(ctx, (unsigned)bp);
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

  emit_load64(buf, A64_TMP0, (unsigned)bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov32(buf, A64_TMP1, (unsigned)off);
  emit_add64(buf, A64_TMP0, A64_TMP1);

  if(width == 2) emit_strh_w_disp(buf, A64_TMP2, A64_TMP0, 0);
  else if(width == 4) emit_str_w_disp(buf, A64_TMP2, A64_TMP0, 0);
  else emit_str_x_disp(buf, A64_TMP2, A64_TMP0, 0);
}

static void emit_bytes_blit(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int dst_bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_buffer_check(ctx, (unsigned)dst_bp);
  int src_bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)src_bp);
  int off = jit_reg_get_gpr(ctx->ra, ins->c, 1);

  emit_load32(buf, A64_TMP0, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, length));
  emit_mov32(buf, A64_TMP1, (unsigned)off);
  emit_add64(buf, A64_TMP1, A64_TMP0);
  emit_load32(buf, A64_TMP2, (unsigned)dst_bp, (uint32_t)offsetof(jello_bytes, length));
  emit_cmp64(buf, A64_TMP1, A64_TMP2);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // HI: off + src->length > dst->length

  emit_load64(buf, A64_TMP0, (unsigned)dst_bp, (uint32_t)offsetof(jello_bytes, data));
  emit_mov32(buf, A64_TMP1, (unsigned)off);
  emit_add64(buf, A64_TMP0, A64_TMP1);
  emit_load64(buf, A64_TMP1, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, data));
  emit_load32(buf, A64_TMP2, (unsigned)src_bp, (uint32_t)offsetof(jello_bytes, length));

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, A64_TMP0);
  emit_mov64(buf, A64_X1, A64_TMP1);
  emit_mov64(buf, A64_X2, A64_TMP2);
  emit_call_fn(buf, (uintptr_t)memmove);
}

static void emit_bits_write(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_buffer_check(ctx, (unsigned)bp);
  int val = jit_reg_get_gpr(ctx->ra, ins->b, 1);

  emit_mov64(buf, A64_TMP0, (unsigned)bp);
  emit_load32(buf, A64_TMP1, A64_TMP0, (uint32_t)offsetof(jello_bytes, length));
  emit_lsl_x_imm3(buf, A64_TMP1, A64_TMP1);
  emit_mov64_imm(buf, A64_IP1, end_bit);
  emit_cmp64(buf, A64_IP1, A64_TMP1);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // HI: end_bit > length * 8

  emit_mov32(buf, A64_TMP2, (unsigned)val);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, A64_TMP0);
  emit_mov32_imm(buf, A64_X1, width);
  emit_mov32_imm(buf, A64_X2, bit_off);
  emit_mov32(buf, A64_X3, A64_TMP2);
  emit_call_fn(buf, (uintptr_t)jit_bits_write);
}

static void emit_bytes_freeze(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  const uint32_t mark = (uint32_t)offsetof(jello_bytes, h.type_id);
  int bp = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  emit_null_check(ctx, (unsigned)bp);
  emit_load32(buf, A64_TMP1, (unsigned)bp, mark);
  emit_cmp32_imm(buf, A64_TMP1, JELLO_BYTES_BUFFER);
  size_t shared = emit_b_cond_placeholder(buf, JIT_JNE); // NE: no buffer
  emit_mov32_imm(buf, A64_TMP1, 0);
  emit_store32(buf, A64_TMP1, (unsigned)bp, mark);
  jit_patch_jump(buf, shared, buf->len);
}

static void emit_bytes_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int len = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov32(buf, A64_TMP0, (unsigned)len);
  emit_mov32_imm(buf, A64_TMP1, (uint32_t)INT32_MAX);
  emit_cmp32(buf, A64_TMP0, A64_TMP1);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS); // HI: len > INT32_MAX
  emit_load_vm32(buf, A64_TMP1, (uint32_t)offsetof(jello_vm, max_bytes_len));
  emit_test32(buf, A64_TMP1, A64_TMP1);
  size_t limit_skip = emit_b_cond_placeholder(buf, JIT_JE); // EQ: max_bytes_len == 0, disabled
  emit_cmp32(buf, A64_TMP0, A64_TMP1);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_LIMIT); // HI: len > max_bytes_len
  jit_patch_jump(buf, limit_skip, buf->len);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov32(buf, A64_X1, A64_TMP0);
  emit_call_fn(buf, (uintptr_t)vm_buffer_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bytes_concat2(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int b = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)b);
  int c = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_null_check(ctx, (unsigned)c);
  emit_mov64(buf, A64_TMP0, (unsigned)b);
  emit_mov64(buf, A64_TMP1, (unsigned)c);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64(buf, A64_X1, A64_TMP0);
  emit_mov64(buf, A64_X2, A64_TMP1);
  emit_call_fn(buf, (uintptr_t)jit_bytes_concat2);
  emit_test64(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // NULL: the helper set trap_code (BOUNDS)
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_bits_read(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t end_bit = (uint64_t)bit_off + (uint64_t)width;

  int bp = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)bp);

  emit_mov64(buf, A64_TMP0, (unsigned)bp);
  emit_load32(buf, A64_TMP1, A64_TMP0, (uint32_t)offsetof(jello_bytes, length));
  emit_lsl_x_imm3(buf, A64_TMP1, A64_TMP1);
  emit_mov64_imm(buf, A64_IP1, end_bit);
  emit_cmp64(buf, A64_IP1, A64_TMP1);
  emit_jcc_trap(ctx, JIT_JA, JIT_TRAP_BOUNDS);

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, A64_TMP0);
  emit_mov32_imm(buf, A64_X1, width);
  emit_mov32_imm(buf, A64_X2, bit_off);
  emit_call_fn(buf, (uintptr_t)jit_bits_read);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

/* -- Struct ops -- */
static int is_xmm_field_kind(jello_type_kind k) {
  return k == JELLO_T_F32 || k == JELLO_T_F64;
}

static void emit_load_slot(emit_ctx* ctx, uint8_t r, jello_type_kind k, unsigned base,
                           uint32_t disp) {
  jit_exec_buf* buf = ctx->buf;
  uint32_t width = (uint32_t)jello_slot_size(k);
  if(is_xmm_field_kind(k)) {
    int dst = jit_reg_alloc_xmm(ctx->ra, r, 0, width == 8);
    base = emit_disp_base(buf, base, A64_NO_GPR, A64_TMP0, &disp, width);
    if(width == 8) {
      emit_fload64(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), base, disp);
      jit_reg_store_home_xmm64(ctx->ra, r, dst);
    } else {
      emit_fload32(buf, (unsigned)(dst - (int)JIT_PREG_XMM0), base, disp);
      jit_reg_store_home_xmm32(ctx->ra, r, dst);
    }
  } else {
    int dst = jit_reg_alloc_gpr(ctx->ra, r, 0);
    base = emit_disp_base(buf, base, (unsigned)dst, A64_TMP0, &disp, width);
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
    base = emit_disp_base(buf, base, A64_NO_GPR, A64_TMP0, &disp, width);
    if(width == 8) emit_fstore64(buf, (unsigned)(val - (int)JIT_PREG_XMM0), base, disp);
    else emit_fstore32(buf, (unsigned)(val - (int)JIT_PREG_XMM0), base, disp);
  } else {
    int val = jit_reg_get_gpr(ctx->ra, r, 1);
    base = emit_disp_base(buf, base, (unsigned)val, A64_TMP0, &disp, width);
    if(width == 8) emit_store64(buf, (unsigned)val, base, disp);
    else emit_store32(buf, (unsigned)val, base, disp);
  }
}

static void emit_struct_new(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_id tid = ctx->f->reg_types[ins->a];
  uint32_t struct_sig_id = ctx->m->types[tid].as.strct.struct_sig_id;

  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov32_imm(buf, A64_X2, struct_sig_id);
  emit_call_fn(buf, (uintptr_t)jello_struct_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
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
  emit_load_slot(ctx, ins->a, emit_array_elem_addr(ctx, ins->b, ins->c), A64_TMP1, 0);
}

static void emit_array_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_store_slot(ctx, ins->a, emit_array_elem_addr(ctx, ins->b, ins->c), A64_TMP1, 0);
}

static jello_type_kind emit_globals_base(emit_ctx* ctx, uint32_t g) {
  emit_load_vm64(ctx->buf, A64_TMP1, (uint32_t)offsetof(jello_vm, globals));
  return ctx->m->types[ctx->m->global_types[g]].kind;
}

static void emit_global_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_load_slot(ctx, ins->a, emit_globals_base(ctx, ins->imm), A64_TMP1, ins->imm * 8u);
}

static void emit_global_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  emit_store_slot(ctx, ins->a, emit_globals_base(ctx, ins->imm), A64_TMP1, ins->imm * 8u);
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

static void emit_typed_helper_args(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_current_rf_ptr_into_r11(buf);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov64_imm(buf, A64_X2, (uint64_t)(uintptr_t)ctx->f);
  emit_mov64(buf, A64_X3, A64_X15);
  emit_mov32_imm(buf, A64_X4, r);
}

static void emit_store_from_boxed_x27(emit_ctx* ctx, uint8_t dst) {
  jit_exec_buf* buf = ctx->buf;
  emit_typed_helper_args(ctx, dst);
  emit_mov64(buf, A64_X5, A64_LAYOUT);
  emit_call_fn(buf, (uintptr_t)vm_store_from_boxed);
  emit_test32(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_HELPER); // 0: the helper set trap_code
}

static void emit_unbox_x27_into(emit_ctx* ctx, uint8_t dst) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind k = vm_reg_kind(ctx->m, ctx->f, dst);
  if(k == JELLO_T_DYNAMIC) {
    int r = jit_reg_alloc_gpr(ctx->ra, dst, 0);
    emit_mov64(buf, (unsigned)r, A64_LAYOUT);
    jit_reg_store_home_gpr64(ctx->ra, dst, r);
    return;
  }
  int tag = vm_kind_immediate_tag(k);
  int f64 = k == JELLO_T_F64;
  if(!f64 && tag < 0) {
    emit_store_from_boxed_x27(ctx, dst);
    if(k == JELLO_T_F32) (void)jit_reg_get_xmm(ctx->ra, dst, 1, 0);
    else (void)jit_reg_get_gpr(ctx->ra, dst, 1);
    return;
  }
  size_t slow[3];
  size_t nslow = 0;
  int p = f64 ? jit_reg_alloc_xmm(ctx->ra, dst, 0, 1) : jit_reg_alloc_gpr(ctx->ra, dst, 0);
  unsigned pi = f64 ? (unsigned)(p - (int)JIT_PREG_XMM0) : (unsigned)p;
  if(f64) {
    emit_obj_kind_guard(buf, A64_LAYOUT, (uint32_t)JELLO_OBJ_BOX_F64, slow, &nslow);
    emit_fload64(buf, pi, A64_LAYOUT, (uint32_t)offsetof(jello_box_f64, value));
    jit_reg_store_home_xmm64(ctx->ra, dst, p);
  } else {
    emit_tag_cmp(buf, A64_LAYOUT, (uint32_t)tag);
    slow[nslow++] = emit_b_cond_placeholder(buf, JIT_JNE); // NE: another tag
    emit_u32(buf, a64_ubfx_x((uint8_t)pi, A64_LAYOUT, 3u, vm_kind_immediate_bits(k)));
    jit_reg_store_home_gpr32(ctx->ra, dst, p);
  }
  size_t j_done = emit_b_placeholder(buf);
  for(size_t i = 0; i < nslow; i++) jit_patch_jump(buf, slow[i], buf->len);
  emit_store_from_boxed_x27(ctx, dst);
  if(f64) jit_spill_load_xmm64(buf, pi, dst);
  else jit_spill_load_gpr64(buf, pi, dst);
  jit_patch_jump(buf, j_done, buf->len);
}

static void emit_box_into_x0(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  jello_type_kind k = vm_reg_kind(ctx->m, ctx->f, r);
  int tag = vm_kind_immediate_tag(k);
  if(k == JELLO_T_DYNAMIC) {
    jit_spill_load_gpr64(buf, A64_X0, r);
  } else if(tag >= 0) {
    emit_load32(buf, A64_X0, (unsigned)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES);
    if(k == JELLO_T_BOOL) {
      emit_test32(buf, A64_X0, A64_X0);
      emit_u32(buf, a64_cset_w(A64_X0, A64_NE));
    }
    emit_lsl_x_imm3(buf, A64_X0, A64_X0);
    emit_u32(buf, a64_add_x_imm(A64_X0, A64_X0, (uint32_t)tag));
  } else if(k == JELLO_T_F64) {
    emit_u32(buf, a64_ldr_d_uimm(0, (uint8_t)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES));
    emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_call_fn(buf, (uintptr_t)jello_box_f64_new);
  } else {
    emit_typed_helper_args(ctx, r);
    emit_call_fn(buf, (uintptr_t)vm_box_from_typed);
  }
}

static void emit_handler_call(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov32_imm(buf, A64_X2, ctx->fi);
  emit_mov32_imm(buf, A64_X3, ctx->cur_bc_pc);
  emit_call_fn(buf, (uintptr_t)jit_run_handler);
  emit_test32(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_PASSTHROUGH); // NE: trapped, located by jit_run_handler
}

static void emit_enum_guard(emit_ctx* ctx, unsigned ep, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_null_check(ctx, ep);
  emit_load8(buf, A64_TMP1, ep, (uint32_t)offsetof(jello_obj_header, kind));
  emit_cmp32_imm(buf, A64_TMP1, (uint32_t)JELLO_OBJ_ENUM);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_TYPE);
  emit_load32(buf, A64_TMP1, ep, (uint32_t)offsetof(jello_obj_header, type_id));
  emit_cmp32_imm(buf, A64_TMP1, ctx->f->reg_types[r]);
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
  emit_u32(buf, a64_add_x_uxtw(A64_TMP1, (uint8_t)ep, (uint8_t)ip, 3));
  jit_reg_state_discard_currents(ctx->ra);
  emit_load64(buf, A64_LAYOUT, A64_TMP1, (uint32_t)offsetof(jello_enum, fields));
  emit_unbox_x27_into(ctx, ins->a);
}

static void emit_to_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_box_into_x0(ctx, ins->b);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_x_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_from_dyn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(ctx->buf, A64_LAYOUT, (unsigned)src);
  emit_unbox_x27_into(ctx, ins->a);
}

static void emit_is_enum(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov64(buf, A64_X0, (unsigned)src);
  emit_mov32_imm(buf, A64_X1, ins->imm);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)vm_dynamic_is_enum);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov32(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_kindof(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_mov_x_rr(buf, A64_X0, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)vm_kindof_dynamic);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_physeq(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  jit_reg_state_discard_currents(ctx->ra);
  emit_sp_adjust(buf, 0, 16);
  emit_box_into_x0(ctx, ins->b);
  emit_store64_sp(buf, A64_X0, 0);
  emit_box_into_x0(ctx, ins->c);
  emit_load64_sp(buf, A64_X10, 0);
  emit_sp_adjust(buf, 1, 16);

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
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov32_imm(buf, A64_X1, tid);
  emit_call_fn(buf, (uintptr_t)jello_object_new);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static void emit_helper_trap_check(emit_ctx* ctx) {
  emit_load_vm32(ctx->buf, A64_X9, (uint32_t)offsetof(jello_vm, trap_code));
  emit_test32(ctx->buf, A64_X9, A64_X9);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_HELPER); // NE: trap_code set
}

static void emit_obj_has_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)o);
  jit_reg_state_discard_currents(ctx->ra);
  if(!ctx->m->proto_enabled || ins->imm == JELLO_ATOM___PROTO__) {
    emit_mov64(buf, A64_X0, (unsigned)o);
    emit_mov32_imm(buf, A64_X1, ins->imm);
    emit_call_fn(buf, (uintptr_t)jello_object_has);
  } else {
    emit_mov64(buf, A64_X2, (unsigned)o);
    emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
    emit_mov32_imm(buf, A64_X3, ins->imm);
    emit_call_fn(buf, (uintptr_t)jit_obj_chain_has);
    emit_helper_trap_check(ctx);
  }
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov_w_rr(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static int obj_get_keeps(const emit_ctx* ctx, uint8_t dst) {
  return vm_reg_kind(ctx->m, ctx->f, dst) == JELLO_T_DYNAMIC;
}

static void emit_obj_set_f64_call(emit_ctx* ctx, uint8_t r) {
  jit_exec_buf* buf = ctx->buf;
  emit_u32(buf, a64_ldr_d_uimm(0, (uint8_t)JIT_REG_BASE, (uint32_t)r * REG_SLOT_BYTES));
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov64(buf, A64_X2, A64_LAYOUT);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_f64);
  emit_helper_trap_check(ctx);
}

static int obj_atom_links(const emit_ctx* ctx, uint32_t atom) {
  return ctx->m->proto_enabled && atom == JELLO_ATOM___PROTO__;
}

#define OBJ_INLINE_LINKS 8u

static size_t emit_obj_probe(jit_exec_buf* buf, uint32_t atom) {
  emit_load32(buf, A64_TMP1, A64_LAYOUT, (uint32_t)offsetof(jello_object, cap));
  emit_u32(buf, a64_sub_w_imm(A64_TMP1, A64_TMP1, 1u));    // the mask
  emit_mov32_imm(buf, A64_X10, vm_atom_hash(atom) - 1u); // the loop steps first
  emit_load64(buf, A64_X11, A64_LAYOUT, (uint32_t)offsetof(jello_object, states));
  emit_load64(buf, A64_X12, A64_LAYOUT, (uint32_t)offsetof(jello_object, keys));
  size_t loop = buf->len;
  emit_u32(buf, a64_add_w_imm(A64_X10, A64_X10, 1u));
  emit_u32(buf, a64_and_w_rr(A64_X10, A64_X10, A64_TMP1));
  emit_u32(buf, a64_add_x_uxtw(A64_TMP2, A64_X11, A64_X10, 0));
  emit_load8(buf, A64_X13, A64_TMP2, 0);
  size_t miss = emit_cbz_w_placeholder(buf, A64_X13);
  emit_cmp32_imm(buf, A64_X13, JELLO_OBJ_SLOT_TOMB);
  emit_u32(buf, a64_b_cond(A64_EQ, off_insns_at(buf->len, loop)));
  emit_u32(buf, a64_add_x_uxtw(A64_TMP2, A64_X12, A64_X10, 2));
  emit_load32(buf, A64_X14, A64_TMP2, 0);
  emit_cmp32_imm(buf, A64_X14, atom);
  emit_u32(buf, a64_b_cond(A64_NE, off_insns_at(buf->len, loop)));
  return miss;
}

static void emit_obj_probed_value_addr(jit_exec_buf* buf) {
  emit_load64(buf, A64_TMP2, A64_LAYOUT, (uint32_t)offsetof(jello_object, vals));
  emit_u32(buf, a64_add_x_uxtw(A64_TMP2, A64_TMP2, A64_X10, 3));
}

static void emit_obj_probed_state_set(jit_exec_buf* buf, uint32_t state) {
  emit_mov32_imm(buf, A64_TMP1, state);
  emit_u32(buf, a64_add_x_uxtw(A64_TMP2, A64_X11, A64_X10, 0));
  emit_store8(buf, A64_TMP1, A64_TMP2, 0);
}

static void emit_obj_get_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)o);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_LAYOUT, (unsigned)o);

  int chained = ctx->m->proto_enabled && !obj_atom_links(ctx, ins->imm);
  if(chained) emit_mov32_imm(buf, A64_X5, OBJ_INLINE_LINKS);
  size_t probe = buf->len;
  size_t miss = emit_obj_probe(buf, ins->imm);
  if(obj_get_keeps(ctx, ins->a)) {
    emit_cmp32_imm(buf, A64_X13, JELLO_OBJ_SLOT_OWNED);
    size_t plain = emit_b_cond_placeholder(buf, JIT_JNE);
    emit_obj_probed_state_set(buf, JELLO_OBJ_SLOT_OCCUPIED);
    jit_patch_jump(buf, plain, buf->len);
  }
  emit_obj_probed_value_addr(buf);
  emit_load64(buf, A64_LAYOUT, A64_TMP2, 0);
  size_t j_done = emit_b_placeholder(buf);
  jit_patch_jump(buf, miss, buf->len);
  if(chained) {
    size_t last = emit_cbz_w_placeholder(buf, A64_X5);
    emit_load64(buf, A64_X9, A64_LAYOUT, (uint32_t)offsetof(jello_object, proto));
    emit_test64(buf, A64_X9, A64_X9);
    size_t end = emit_b_cond_placeholder(buf, JIT_JE); // EQ: no prototype
    emit_u32(buf, a64_sub_w_imm(A64_X5, A64_X5, 1u));
    emit_mov64(buf, A64_LAYOUT, A64_X9);
    emit_u32(buf, a64_b(off_insns_at(buf->len, probe)));
    jit_patch_jump(buf, last, buf->len);
    jit_patch_jump(buf, end, buf->len);
  }

  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov64(buf, A64_X2, A64_LAYOUT);
  emit_mov32_imm(buf, A64_X3, ins->imm);
  emit_mov32_imm(buf, A64_X4, (uint32_t)obj_get_keeps(ctx, ins->a));
  emit_call_fn(buf, (uintptr_t)jit_obj_chain_get);
  emit_mov64(buf, A64_LAYOUT, A64_X0); // x27 = the value; o is dead

  emit_helper_trap_check(ctx);

  jit_patch_jump(buf, j_done, buf->len);
  emit_unbox_x27_into(ctx, ins->a);
}

static void emit_obj_set_atom(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)o);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_LAYOUT, (unsigned)o);
  if(vm_reg_kind(ctx->m, ctx->f, ins->a) == JELLO_T_F64) {
    size_t miss = emit_obj_probe(buf, ins->imm);
    emit_cmp32_imm(buf, A64_X13, JELLO_OBJ_SLOT_OWNED);
    size_t unowned = emit_b_cond_placeholder(buf, JIT_JNE);
    emit_obj_probed_value_addr(buf);
    emit_load64(buf, A64_TMP2, A64_TMP2, 0);
    emit_u32(buf, a64_ldr_d_uimm(0, (uint8_t)JIT_REG_BASE, (uint32_t)ins->a * REG_SLOT_BYTES));
    emit_u32(buf, a64_str_d_uimm(0, A64_TMP2, (uint32_t)offsetof(jello_box_f64, value)));
    size_t j_done = emit_b_placeholder(buf);
    jit_patch_jump(buf, miss, buf->len);
    jit_patch_jump(buf, unowned, buf->len);
    emit_mov32_imm(buf, A64_X3, ins->imm);
    emit_obj_set_f64_call(ctx, ins->a);
    jit_patch_jump(buf, j_done, buf->len);
    return;
  }
  emit_box_into_x0(ctx, ins->a);
  int inline_set = !obj_atom_links(ctx, ins->imm);
  size_t j_done = 0;
  if(inline_set) {
    size_t miss = emit_obj_probe(buf, ins->imm);
    emit_cmp32_imm(buf, A64_X13, JELLO_OBJ_SLOT_SEALED);
    size_t sealed = emit_b_cond_placeholder(buf, JIT_JE); // EQ: the helper traps
    emit_obj_probed_value_addr(buf);
    emit_store64(buf, A64_X0, A64_TMP2, 0);
    emit_obj_probed_state_set(buf, JELLO_OBJ_SLOT_OCCUPIED);
    j_done = emit_b_placeholder(buf);
    jit_patch_jump(buf, miss, buf->len);
    jit_patch_jump(buf, sealed, buf->len);
  }

  emit_mov64(buf, A64_X4, A64_X0);
  emit_mov32_imm(buf, A64_X3, ins->imm);
  emit_mov64(buf, A64_X2, A64_LAYOUT);
  emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_direct);
  emit_helper_trap_check(ctx);
  if(inline_set) jit_patch_jump(buf, j_done, buf->len);
}

static void emit_obj_atom_operands(emit_ctx* ctx, const jello_jit_ir_insn* ins, int* o, int* atom) {
  *o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)*o);
  *atom = jit_reg_get_gpr(ctx->ra, ins->c, 1);
  emit_cmp32_imm(ctx->buf, (unsigned)*atom, ctx->m->natoms);
  emit_jcc_trap(ctx, JIT_JAE, JIT_TRAP_BOUNDS); // HS: atom_id >= natoms
}

static void emit_obj_get(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o, atom;
  emit_obj_atom_operands(ctx, ins, &o, &atom);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_LAYOUT, (unsigned)o);
  emit_mov32(buf, A64_X13, (unsigned)atom);

  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov64(buf, A64_X2, A64_LAYOUT);
  emit_mov32(buf, A64_X3, A64_X13);
  emit_mov32_imm(buf, A64_X4, (uint32_t)obj_get_keeps(ctx, ins->a));
  emit_call_fn(buf, (uintptr_t)jit_obj_chain_get);
  emit_mov64(buf, A64_LAYOUT, A64_X0);

  emit_helper_trap_check(ctx);

  emit_unbox_x27_into(ctx, ins->a);
}

static void emit_obj_set(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o, atom;
  emit_obj_atom_operands(ctx, ins, &o, &atom);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_LAYOUT, (unsigned)o);
  if(vm_reg_kind(ctx->m, ctx->f, ins->a) == JELLO_T_F64) {
    emit_mov32(buf, A64_X3, (unsigned)atom);
    emit_obj_set_f64_call(ctx, ins->a);
    return;
  }
  emit_sp_adjust(buf, 0, 16);
  emit_mov32(buf, A64_X13, (unsigned)atom);
  emit_store64_sp(buf, A64_X13, 0);
  emit_box_into_x0(ctx, ins->a);
  emit_load64_sp(buf, A64_X5, 0);
  emit_sp_adjust(buf, 1, 16);

  emit_mov64(buf, A64_X4, A64_X0);
  emit_mov32(buf, A64_X3, A64_X5);
  emit_mov64(buf, A64_X2, A64_LAYOUT);
  emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_obj_set_direct);
  emit_helper_trap_check(ctx);
}

static void emit_obj_has(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o, atom;
  emit_obj_atom_operands(ctx, ins, &o, &atom);
  if(!ctx->m->proto_enabled) {
    emit_mov64(buf, A64_X0, (unsigned)o);
    emit_mov32(buf, A64_X1, (unsigned)atom);
    jit_reg_state_discard_currents(ctx->ra);
    emit_call_fn(buf, (uintptr_t)jello_object_has);
  } else {
    jit_reg_state_discard_currents(ctx->ra);
    emit_mov64(buf, A64_LAYOUT, (unsigned)o);
    emit_mov32(buf, A64_X13, (unsigned)atom);
    emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
    emit_mov64_imm(buf, A64_X1, (uint64_t)(uintptr_t)ctx->m);
    emit_mov64(buf, A64_X2, A64_LAYOUT);
    emit_mov32(buf, A64_X3, A64_X13);
    emit_call_fn(buf, (uintptr_t)jit_obj_chain_has);
    emit_helper_trap_check(ctx);
  }
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov32(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_remove(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o, atom;
  emit_obj_atom_operands(ctx, ins, &o, &atom);
  emit_mov64(buf, A64_X1, (unsigned)o);
  emit_mov32(buf, A64_X2, (unsigned)atom);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)jit_obj_remove);
  emit_helper_trap_check(ctx);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov32(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr32(ctx->ra, ins->a, dst);
}

static void emit_obj_clear(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int o = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)o);
  emit_mov64(buf, A64_X1, (unsigned)o);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  jit_reg_state_discard_currents(ctx->ra);
  emit_call_fn(buf, (uintptr_t)jit_obj_clear);
  emit_helper_trap_check(ctx);
}

static void emit_obj_copy(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_exec_buf* buf = ctx->buf;
  int src = jit_reg_get_gpr(ctx->ra, ins->b, 1);
  emit_null_check(ctx, (unsigned)src);
  emit_mov64(buf, A64_TMP0, (unsigned)src);
  jit_reg_state_discard_currents(ctx->ra);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_mov64(buf, A64_X1, A64_TMP0);
  emit_call_fn(buf, (uintptr_t)jello_object_copy);
  int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
  emit_mov64(buf, (unsigned)dst, A64_X0);
  jit_reg_store_home_gpr64(ctx->ra, ins->a, dst);
}

static size_t emit_buf_cap(uint32_t ninsns, int allow_calls) {
  size_t per = allow_calls ? JIT_EMIT_CALL_INSN_BUDGET : JIT_EMIT_PER_INSN_BUDGET;
  size_t cap = 256u + (size_t)ninsns * per + 512u;
  if(cap < JIT_EMIT_BUF_MIN_CAP) cap = JIT_EMIT_BUF_MIN_CAP;
  return cap;
}

void jello_jit_patch_call_rel32(jit_exec_buf* buf, size_t at, int32_t rel) {
  patch_bl(buf, at, rel);
}

void jello_jit_code_free(jello_jit_code* code) {
  free(code->loop_anchor_pcs);
  free(code->loop_anchor_offs);
  code->loop_anchor_pcs = NULL;
  code->loop_anchor_offs = NULL;
  code->nloop_anchors = 0;
  jit_free_exec_buf(&code->buf);
}

static void emit_jmp_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  jit_push_branch(ctx, emit_b_placeholder(ctx->buf), jit_branch_target(ctx, ins));
}

static void emit_jmp_if_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  uint32_t tgt = jit_branch_target(ctx, ins);
  int cond = jit_reg_get_gpr(ctx->ra, ins->a, 1);
  jit_push_branch(ctx, emit_cbnz_w_placeholder(ctx->buf, (unsigned)cond), tgt);
}

static void emit_safepoint_insn(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  _Static_assert(offsetof(jello_heap, stop_requested) % 4 == 0 &&
                     offsetof(jello_heap, stop_requested) / 4 <= 0xFFF,
                 "ldr w encodes the offset");
  emit_load_vm64(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, heap));
  emit_load32(buf, A64_TMP0, A64_TMP0, (uint32_t)offsetof(jello_heap, stop_requested));
  size_t no_stop = emit_cbz_w_placeholder(buf, A64_TMP0);
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)vm_safepoint_check);
  jit_patch_jump(buf, no_stop, buf->len);
  jit_reg_state_clobber_call(ctx->ra);
  emit_load_vm32(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, reductions));
  emit_u32(buf, a64_subs_w_imm(A64_TMP0, A64_TMP0, 1));
  emit_store_vm32(buf, A64_TMP0, (uint32_t)offsetof(jello_vm, reductions));
  size_t in_window = emit_b_cond_placeholder(buf, JIT_JG); // GT: reductions left
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)vm_reductions_out);
  emit_test32(buf, A64_X0, A64_X0);
  emit_jcc_trap(ctx, JIT_JNE, JIT_TRAP_HELPER); // NE: out of fuel
  jit_patch_jump(buf, in_window, buf->len);
}

int jit_emit_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jit_ir_op)ins->op) {
    case JIR_NOP: break;
    case JIR_MOV: {
      jello_type_kind dk = vm_reg_kind(ctx->m, ctx->f, ins->a);
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      if(jello_slot_size(dk) == 8u) {
        emit_mov64(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      } else {
        emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      }
      break;
    }
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32_imm(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_I64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov64_imm(ctx->buf, (unsigned)d, (uint64_t)ctx->m->const_i64[ins->imm]);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F16: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32_imm(ctx->buf, (unsigned)d, ins->imm & 0xFFFFu);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F32: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32_imm(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_F64: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      uint64_t bits;
      memcpy(&bits, &ctx->m->const_f64[ins->imm], 8);
      emit_mov64_imm(ctx->buf, (unsigned)d, bits);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_BOOL: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32_imm(ctx->buf, (unsigned)d, ins->imm ? 1u : 0u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_BIN_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int dst = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu32(ctx->buf, JIT_ALU_ADD, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu32(ctx->buf, JIT_ALU_SUB, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_mul32(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu32(ctx->buf, JIT_ALU_AND, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu32(ctx->buf, JIT_ALU_OR, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu32(ctx->buf, JIT_ALU_XOR, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_DIV:
        case JIR_BIN_MOD: {
          emit_divmod_guard_i32(ctx, (unsigned)rhs, (unsigned)lhs);
          emit_u32(ctx->buf, a64_sdiv_w(A64_TMP0, (uint8_t)lhs, (uint8_t)rhs));
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD) {
            emit_u32(ctx->buf, a64_msub_w((uint8_t)dst, A64_TMP0, (uint8_t)rhs, (uint8_t)lhs));
          } else {
            emit_mov32(ctx->buf, (uint8_t)dst, A64_TMP0);
          }
          break;
        }
        case JIR_BIN_SHL:
        case JIR_BIN_SHR: {
          emit_mov32(ctx->buf, A64_TMP0, (unsigned)rhs);
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
      emit_mov64(ctx->buf, (unsigned)dst, (unsigned)lhs);
      switch((jello_jir_bin)ins->imm) {
        case JIR_BIN_ADD: emit_alu64(ctx->buf, JIT_ALU_ADD, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_SUB: emit_alu64(ctx->buf, JIT_ALU_SUB, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_MUL: emit_mul64(ctx->buf, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_AND: emit_alu64(ctx->buf, JIT_ALU_AND, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_OR: emit_alu64(ctx->buf, JIT_ALU_OR, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_XOR: emit_alu64(ctx->buf, JIT_ALU_XOR, (unsigned)dst, (unsigned)rhs); break;
        case JIR_BIN_DIV:
        case JIR_BIN_MOD: {
          emit_divmod_guard_i64(ctx, (unsigned)rhs, (unsigned)lhs);
          emit_u32(ctx->buf, a64_sdiv_x(A64_TMP0, (uint8_t)lhs, (uint8_t)rhs));
          if((jello_jir_bin)ins->imm == JIR_BIN_MOD) {
            emit_u32(ctx->buf, a64_msub_x((uint8_t)dst, A64_TMP0, (uint8_t)rhs, (uint8_t)lhs));
          } else {
            emit_mov64(ctx->buf, (uint8_t)dst, A64_TMP0);
          }
          break;
        }
        case JIR_BIN_SHL:
        case JIR_BIN_SHR: {
          emit_mov32(ctx->buf, A64_TMP0, (unsigned)rhs);
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
    case JIR_BIN_F32: emit_bin_f32(ctx, ins); break;
    case JIR_BIN_F64: emit_bin_f64(ctx, ins); break;
    case JIR_ADD_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu32_imm(ctx->buf, JIT_ALU_ADD, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_SUB_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_alu32_imm(ctx->buf, JIT_ALU_SUB, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_MUL_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_mul32_imm(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg32(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_I64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov64(ctx->buf, (unsigned)d, (unsigned)s);
      emit_neg64(ctx->buf, (unsigned)d);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F32: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_xor32_imm(ctx->buf, (unsigned)d, 0x80000000u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NEG_F64: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov64(ctx->buf, (unsigned)d, (unsigned)s);
      emit_mov64_imm(ctx->buf, A64_TMP0, 0x8000000000000000ULL);
      emit_alu64(ctx->buf, JIT_ALU_XOR, (unsigned)d, A64_TMP0);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NOT_BOOL: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)s);
      emit_xor32_imm(ctx->buf, (unsigned)d, 1u);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I32: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp32(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? JIT_SETE : JIT_SETL,
                            (unsigned)d);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_EQ_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp32_imm(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc_from_flags(ctx->buf, JIT_SETE, (unsigned)d);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_I64: {
      int lhs = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int rhs = jit_reg_get_gpr(ctx->ra, ins->c, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp64(ctx->buf, (unsigned)lhs, (unsigned)rhs);
      emit_setcc_from_flags(ctx->buf, (jello_jir_cmp)ins->imm == JIR_CMP_EQ ? JIT_SETE : JIT_SETL,
                            (unsigned)d);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_LT_I32_IMM: {
      int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_cmp32_imm(ctx->buf, (unsigned)s, ins->imm);
      emit_setcc_from_flags(ctx->buf, JIT_SETL, (unsigned)d);
      emit_mov32(ctx->buf, (unsigned)d, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CMP_F32: emit_cmp_float(ctx, ins, 0); break;
    case JIR_CMP_F64: emit_cmp_float(ctx, ins, 1); break;
    case JIR_CONV:
      if(!emit_conv(ctx, ins)) return 0;
      break;
    case JIR_BYTES_NEW: emit_bytes_new(ctx, ins); break;
    case JIR_BYTES_LEN: emit_bytes_len(ctx, ins); break;
    case JIR_BYTES_GET_U8: emit_bytes_get_u8(ctx, ins); break;
    case JIR_BYTES_SET_U8: emit_bytes_set_u8(ctx, ins); break;
    case JIR_BYTES_EQ: emit_bytes_eq(ctx, ins); break;
    case JIR_BYTES_CONCAT2: emit_bytes_concat2(ctx, ins); break;
    case JIR_BYTES_BITOP: emit_bytes_bitop(ctx, ins); break;
    case JIR_BYTES_READ: emit_bytes_read(ctx, ins); break;
    case JIR_BYTES_WRITE: emit_bytes_write(ctx, ins); break;
    case JIR_BYTES_BLIT: emit_bytes_blit(ctx, ins); break;
    case JIR_BITS_WRITE: emit_bits_write(ctx, ins); break;
    case JIR_BYTES_FREEZE: emit_bytes_freeze(ctx, ins); break;
    case JIR_BITS_READ: emit_bits_read(ctx, ins); break;
    case JIR_STRUCT_NEW: emit_struct_new(ctx, ins); break;
    case JIR_STRUCT_GET: emit_struct_get(ctx, ins); break;
    case JIR_STRUCT_SET: emit_struct_set(ctx, ins); break;
    case JIR_CONST_NULL: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      uint64_t null_bits =
          vm_reg_kind(ctx->m, ctx->f, ins->a) == JELLO_T_DYNAMIC ? (uint64_t)JELLO_TAG_NULL : 0u;
      emit_mov64_imm(ctx->buf, (unsigned)d, null_bits);
      jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      break;
    }
    case JIR_CONST_ATOM: {
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_mov32_imm(ctx->buf, (unsigned)d, ins->imm);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_TO_DYN: emit_to_dyn(ctx, ins); break;
    case JIR_FROM_DYN: emit_from_dyn(ctx, ins); break;
    case JIR_KINDOF: emit_kindof(ctx, ins); break;
    case JIR_IS_ENUM: emit_is_enum(ctx, ins); break;
    case JIR_PHYSEQ: emit_physeq(ctx, ins); break;
    case JIR_IS_NULL: {
      emit_null_flags(ctx, ins->b);
      int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
      emit_setcc_from_flags(ctx->buf, JIT_SETE, (unsigned)d);
      jit_reg_store_home_gpr32(ctx->ra, ins->a, d);
      break;
    }
    case JIR_NULL_CHECK:
      emit_null_flags(ctx, ins->a);
      emit_jcc_trap(ctx, JIT_JE, JIT_TRAP_NULL);
      break;
    case JIR_OBJ_NEW: emit_obj_new(ctx, ins); break;
    case JIR_OBJ_HAS_ATOM: emit_obj_has_atom(ctx, ins); break;
    case JIR_OBJ_GET_ATOM: emit_obj_get_atom(ctx, ins); break;
    case JIR_OBJ_SET_ATOM: emit_obj_set_atom(ctx, ins); break;
    case JIR_OBJ_GET: emit_obj_get(ctx, ins); break;
    case JIR_OBJ_SET: emit_obj_set(ctx, ins); break;
    case JIR_OBJ_HAS: emit_obj_has(ctx, ins); break;
    case JIR_OBJ_REMOVE: emit_obj_remove(ctx, ins); break;
    case JIR_OBJ_CLEAR: emit_obj_clear(ctx, ins); break;
    case JIR_OBJ_COPY: emit_obj_copy(ctx, ins); break;
    case JIR_ARRAY_LEN: emit_array_len(ctx, ins); break;
    case JIR_ARRAY_GET: emit_array_get(ctx, ins); break;
    case JIR_ARRAY_SET: emit_array_set(ctx, ins); break;
    case JIR_ENUM_TAG: emit_enum_tag(ctx, ins); break;
    case JIR_ENUM_GET: emit_enum_get(ctx, ins); break;
    case JIR_GLOBAL_GET: emit_global_get(ctx, ins); break;
    case JIR_GLOBAL_SET: emit_global_set(ctx, ins); break;
    case JIR_SLOW:
    case JIR_CALL_NATIVE: emit_handler_call(ctx); break;
    case JIR_RET: emit_ret_insn(ctx, ins->a); break;
    case JIR_JMP: emit_jmp_insn(ctx, ins); break;
    case JIR_JMP_IF: emit_jmp_if_insn(ctx, ins); break;
    case JIR_SAFEPOINT: emit_safepoint_insn(ctx); break;
    case JIR_CALLR: return emit_callr_insn(ctx, ins);
    case JIR_TRY: emit_try_insn(ctx, ins); break;
    case JIR_ENDTRY: emit_endtry_insn(ctx); break;
    case JIR_THROW: emit_throw_insn(ctx, ins); break;
    case JIR_CALL_SELF:
    case JIR_CALL_DIRECT: return emit_call_insn(ctx, ins, (jello_jit_ir_op)ins->op);
    default: return 0;
  }
  return 1;
}

int jello_jit_emit_mixed_stubs(jit_exec_buf* buf, size_t* enter_off, size_t* bridge_off,
                               size_t* lazy_off) {
  *enter_off = buf->len;
  emit_u32(buf, a64_stp_x_pre(A64_FP, A64_LR, A64_SP, -2));
  emit_u32(buf, a64_mov_x_rr(A64_FP, A64_SP));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, -2));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, -2));
  emit_u32(buf, a64_stp_x_pre((uint8_t)JIT_REG_SURVIVE, A64_IP0, A64_SP, -2));
  emit_save_fp_pool(buf);
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_VM, A64_X0));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_BASE, A64_X1));
  emit_u32(buf, a64_mov_x_rr((uint8_t)JIT_REG_OUT, A64_X2));
  emit_u32(buf, a64_blr_x(A64_X3));
  emit_restore_fp_pool(buf);
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_SURVIVE, A64_IP0, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_BASE, A64_LAYOUT, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post((uint8_t)JIT_REG_OUT, (uint8_t)JIT_REG_VM, A64_SP, 2));
  emit_u32(buf, a64_ldp_x_post(A64_FP, A64_LR, A64_SP, 2));
  emit_ret_insn_a64(buf);

  *bridge_off = buf->len;
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_run_interp_top);
  emit_reload_jit_reg_base(buf);
  emit_ret_insn_a64(buf);

  *lazy_off = buf->len;
  emit_mov64(buf, A64_X0, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (uintptr_t)jit_lazy_target);
  emit_u32(buf, a64_br_x(A64_X0));
  return !buf->overflowed;
}
