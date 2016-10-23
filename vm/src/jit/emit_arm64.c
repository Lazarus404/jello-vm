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
  A64_X0 = 0, A64_X1 = 1, A64_X2 = 2, A64_X3 = 3, A64_X4 = 4, A64_X5 = 5,
  A64_X9 = 9, A64_X10 = 10, A64_X11 = 11, A64_X12 = 12,
  A64_X13 = 13, A64_X14 = 14, A64_X15 = 15,
  A64_LAYOUT = 27,
  A64_FP = 29, A64_LR = 30, A64_SP = 31,
  A64_IP0 = 16, A64_IP1 = 17,
  A64_TMP0 = 15, A64_TMP1 = 16, A64_TMP2 = 17,
};

static void emit_u32(jit_exec_buf* buf, uint32_t word) {
  jit_exec_buf_write(buf, &word, 4);
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

static void emit_call_fn(jit_exec_buf* buf, void* fn) {
  emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, 16));
  emit_u32(buf, a64_str_x_uimm(A64_LR, A64_SP, 8));
  emit_mov_x_imm64(buf, A64_IP0, fn_addr_bits_fn(fn));
  emit_u32(buf, a64_blr_x(A64_IP0));
  emit_u32(buf, a64_ldr_x_uimm(A64_LR, A64_SP, 8));
  emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, 16));
}

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

static size_t emit_b_cond_placeholder(jit_exec_buf* buf, uint8_t x64_cc, int32_t rel_unused) {
  (void)rel_unused;
  size_t at = buf->len;
  uint32_t word;
  switch(x64_cc) {
    case 0x84: word = a64_b_eq(0); break;
    case 0x85: word = a64_b_ne(0); break;
    case 0x82: word = a64_b_lo(0); break;
    case 0x83: word = a64_b_hs(0); break;
    case 0x86: word = a64_b_ls(0); break;
    case 0x87: word = a64_b_hi(0); break;
    case 0x8C: word = a64_b_lt(0); break;
    case 0x9C: word = a64_b_lt(0); break;
    default: word = a64_b_eq(0); break;
  }
  emit_u32(buf, word);
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

static void patch_cbz_w(jit_exec_buf* buf, size_t at, size_t target) {
  int32_t off = off_insns_at(at, target);
  uint32_t old = *(uint32_t*)(buf->mem + at);
  uint32_t rt = old & 0x1Fu;
  uint32_t word = 0x34000000u | (((uint32_t)off & 0x7FFFFu) << 5) | rt;
  memcpy(buf->mem + at, &word, 4);
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

static void emit_cmp_x_eq_neg1(jit_exec_buf* buf, unsigned reg) {
  emit_u32(buf, a64_cmn_x_imm((uint8_t)reg, 1));
}

static void emit_tst_w_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
  emit_u32(buf, a64_ands_w_rr(0x1F, (uint8_t)a, (uint8_t)b));
}

static void emit_tst_x_rr(jit_exec_buf* buf, unsigned a, unsigned b) {
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

static void emit_str_x_sp(jit_exec_buf* buf, unsigned src, uint8_t disp) {
  emit_u32(buf, a64_str_x_uimm((uint8_t)src, A64_SP, disp));
}

static void emit_ldr_x_sp(jit_exec_buf* buf, unsigned dst, uint8_t disp) {
  emit_u32(buf, a64_ldr_x_uimm((uint8_t)dst, A64_SP, disp));
}

static void emit_sp_adjust(jit_exec_buf* buf, int is_add, uint8_t imm8) {
  if(is_add) emit_u32(buf, a64_add_x_imm(A64_SP, A64_SP, imm8));
  else emit_u32(buf, a64_sub_x_imm(A64_SP, A64_SP, imm8));
}

typedef struct jit_branch_patch {
  size_t at;
  uint32_t bc_tgt;
  uint8_t is_jcc;
} jit_branch_patch;

typedef enum jit_trap_kind {
  JIT_TRAP_ARITH = 0,
  JIT_TRAP_NULL,
  JIT_TRAP_BOUNDS,
  JIT_TRAP_TYPE,
  JIT_TRAP_PASSTHROUGH,
  JIT_TRAP_KIND_COUNT,
} jit_trap_kind;

typedef struct jit_trap_jump {
  size_t at;
  uint8_t kind; /* jit_trap_kind */
} jit_trap_jump;

typedef struct emit_ctx {
  jit_exec_buf* buf;
  jit_reg_state* ra;
  const jello_bc_module* m;
  const jello_bc_function* f;
  uint32_t fi;
  int allow_calls;
  jit_trap_jump* trap_jumps;
  size_t trap_jump_cap;
  size_t trap_jump_count;
  jit_branch_patch* branches;
  size_t branch_cap;
  size_t branch_count;
  size_t* ret_jumps;
  size_t ret_jump_cap;
  size_t ret_jump_count;
  size_t* stack_overflow_jumps;
  size_t stack_overflow_jump_cap;
  size_t stack_overflow_jump_count;
  jello_jit_call_patch** call_patches;
  uint32_t* call_patch_count;
  uint32_t call_patch_cap;
  jello_type_kind ret_kind;
  int survive_any_armed;
} emit_ctx;

static void emit_jcc_trap(emit_ctx* ctx, uint8_t cc, uint8_t kind) {
  size_t at = emit_b_cond_placeholder(ctx->buf, cc, 0);
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
    if(ctx->trap_jumps[i].kind == 255) patch_b(ctx->buf, at, trap_off);
    else patch_b_cond(ctx->buf, at, trap_off);
  }
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
  (void)allow_calls;
  switch(op) {
    case JIR_NOP: case JIR_RET: case JIR_MOV:
    case JIR_CONST_I8_IMM: case JIR_CONST_I32: case JIR_CONST_I64:
    case JIR_CONST_F16: case JIR_CONST_F32: case JIR_CONST_F64: case JIR_CONST_BOOL:
    case JIR_BIN_I32: case JIR_BIN_I64:
    case JIR_ADD_I32_IMM: case JIR_SUB_I32_IMM: case JIR_MUL_I32_IMM:
    case JIR_NEG_I32: case JIR_NEG_I64: case JIR_NOT_BOOL:
    case JIR_CMP_I32: case JIR_CMP_I64:
    case JIR_EQ_I32_IMM: case JIR_LT_I32_IMM:
      return 1;
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
    emit_mov_x_rr(buf, A64_X9, A64_SP);
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




/* -- Object ops -- */

static int emit_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  switch((jello_jit_ir_op)ins->op) {
    case JIR_NOP:
      break;
    case JIR_MOV: {
      jello_type_kind dk = vm_reg_kind(ctx->m, ctx->f, ins->a);
      if(dk == JELLO_T_I64 || dk == JELLO_T_F64) {
        int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
        int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
        emit_mov_x_rr(ctx->buf, (unsigned)d, (unsigned)s);
        jit_reg_store_home_gpr64(ctx->ra, ins->a, d);
      } else {
        int s = jit_reg_get_gpr(ctx->ra, ins->b, 1);
        int d = jit_reg_alloc_gpr(ctx->ra, ins->a, 0);
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
    ok = emit_insn(&ctx, ins);
  insn_done:
    if(splan.is_consumer[i] && jit_reg_survive_is_held(&ra)) jit_reg_survive_release(&ra);
    jit_reg_unpin_all(&ra);
  }

  size_t epilogue_ok_off = 0;
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
    patch_trap_jumps(&ctx, trap_off_by_kind);
    for(size_t i = 0; i < ctx.ret_jump_count; i++) {
      size_t at = ctx.ret_jumps[i];
      patch_b(buf, at, epilogue_ok_off );
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
