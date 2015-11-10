// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

/* -- x86-64 byte encoding -- */

enum {
  X64_RAX = 0, X64_RCX = 1, X64_RDX = 2, X64_RBX = 3,
  X64_RSP = 4, X64_RBP = 5, X64_RSI = 6, X64_RDI = 7,
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
  /* Opcode 0x89: ModRM.reg=src, ModRM.rm=RBX -- high src needs REX.R (0x44). */
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
  /* CMP r32, r/m32 (0x3B): flags from a - b */
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
  /* REX required for r8-r15 and for sil/dil/bpl/spl (regs 4-7). */
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

static void emit_jcc_rel32(jit_exec_buf* buf, uint8_t cc, int32_t rel) {
  uint8_t bytes[6];
  bytes[0] = 0x0F;
  bytes[1] = (uint8_t)(0x80u | (cc & 0xFu));
  memcpy(&bytes[2], &rel, 4);
  jit_exec_buf_write(buf, bytes, sizeof(bytes));
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
  /* CMP r64, r/m64 (0x3B): flags from a - b */
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(0x48u | ((a >= 8) ? 0x04u : 0) | ((b >= 8) ? 0x01u : 0));
  bytes[1] = 0x3B;
  bytes[2] = (uint8_t)(0xC0u | ((a & 7u) << 3) | (b & 7u));
  jit_exec_buf_write(buf, bytes, 3);
}

static void emit_xor_r32_imm32(jit_exec_buf* buf, unsigned dst, uint32_t imm) {
  emit_alu_r32_imm32(buf, 0x06, dst, imm);
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

static void emit_call_fn(jit_exec_buf* buf, void* fn) {
  emit_movabs_rax(buf, fn_addr_bits_fn(fn));
  emit_call_rax(buf);
}

typedef struct emit_ctx {
  jit_exec_buf* buf;
  jit_reg_state* ra;
  const jello_bc_module* m;
  const jello_bc_function* f;
  size_t* trap_jumps;
  size_t trap_jump_cap;
  size_t trap_jump_count;
} emit_ctx;

static void emit_jcc_trap(emit_ctx* ctx, uint8_t cc) {
  size_t at = ctx->buf->len;
  emit_jcc_rel32(ctx->buf, cc, 0);
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
  /* RAX is free scratch here -- DIV/MOD reloads it from lhs next. */
  emit_mov_r64_imm64(ctx->buf, X64_RAX, (uint64_t)INT64_MIN);
  emit_cmp_r64_r64(ctx->buf, lhs_gpr, X64_RAX);
  emit_jcc_trap(ctx, 0x84);
  size_t ok = ctx->buf->len;
  patch_jcc_rel32(ctx->buf, jne, (int32_t)(ok - (jne + 6)));
}

static int jir_op_allowed(jello_jit_ir_op op) {
  switch(op) {
    case JIR_NOP: case JIR_RET: case JIR_MOV:
    case JIR_CONST_I8_IMM: case JIR_CONST_I32: case JIR_CONST_I64:
    case JIR_CONST_BOOL:
    case JIR_BIN_I32: case JIR_BIN_I64:
    case JIR_ADD_I32_IMM: case JIR_SUB_I32_IMM: case JIR_MUL_I32_IMM:
    case JIR_NEG_I32: case JIR_NEG_I64: case JIR_NOT_BOOL:
    case JIR_CMP_I32: case JIR_CMP_I64:
    case JIR_EQ_I32_IMM: case JIR_LT_I32_IMM: case JIR_CONV:
      return 1;
    default:
      return 0;
  }
}

static int ir_shape_ok(const jello_jit_ir_function* ir, const jello_bc_module* m,
                       const jello_bc_function* f, jello_type_kind* out_ret_kind) {
  if(ir->nslow > 0) return 0;
  int saw_ret = 0;
  for(uint32_t i = 0; i < ir->ninsns; i++) {
    jello_jit_ir_op op = (jello_jit_ir_op)ir->insns[i].op;
    if(!jir_op_allowed(op)) return 0;
    if(op == JIR_RET) {
      if(saw_ret) return 0;
      saw_ret = 1;
      *out_ret_kind = vm_reg_kind(m, f, ir->insns[i].a);
      if(*out_ret_kind == JELLO_T_F16) return 0;
    }
  }
  return saw_ret;
}

/* -- entry_off + body_off -- */
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

static void emit_epilogue_ok(emit_ctx* ctx, jello_type_kind ret_kind, uint8_t ret_vreg) {
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
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_restore_callee_saved(buf);
  emit_mov_eax_imm32(buf, (uint32_t)JELLO_EXEC_OK);
  emit_ret(buf);
}

static void emit_epilogue_trap(emit_ctx* ctx) {
  jit_exec_buf* buf = ctx->buf;
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)jit_trap_arithmetic);
  emit_mov_r64_r64(buf, X64_RDI, (unsigned)JIT_REG_VM);
  emit_call_fn(buf, (void*)vm_pop_frame);
  emit_restore_callee_saved(buf);
  emit_mov_eax_imm32(buf, (uint32_t)JELLO_EXEC_TRAP);
  emit_ret(buf);
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
  if(dst >= 8) bytes[n++] = 0x41;
  bytes[n++] = 0x69;
  bytes[n++] = (uint8_t)(0xC0u | ((dst & 7u) << 3) | (dst & 7u));
  memcpy(&bytes[n], &imm, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
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

static size_t emit_buf_cap(uint32_t ninsns) {
  size_t cap = 256u + (size_t)ninsns * JIT_EMIT_PER_INSN_BUDGET + 512u;
  if(cap < JIT_EMIT_BUF_MIN_CAP) cap = JIT_EMIT_BUF_MIN_CAP;
  return cap;
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
  jello_type_kind ret_kind = JELLO_T_VOID;
  if(!ir_shape_ok(ir, m, f, &ret_kind)) { jello_jit_ir_free(ir); return 0; }

  jit_exec_buf buf = jit_alloc_exec_buf(emit_buf_cap(ir->ninsns));
  if(!buf.mem) { jello_jit_ir_free(ir); return 0; }

  size_t trap_jumps[256];
  emit_ctx ctx;
  ctx.buf = &buf;
  ctx.m = m;
  ctx.f = f;
  ctx.trap_jumps = trap_jumps;
  ctx.trap_jump_cap = 256;
  ctx.trap_jump_count = 0;

  jit_reg_state ra;
  jit_reg_emit_ctx re;
  re.buf = &buf;
  jit_reg_state_init(&ra, ir->nregs, re);
  ctx.ra = &ra;

  size_t body_off = 0;
  emit_prologue(&buf, &body_off);
  uint8_t ret_vreg = 0;
  int ok = 1;
  for(uint32_t i = 0; i < ir->ninsns && ok; i++) {
    const jello_jit_ir_insn* ins = &ir->insns[i];
    if(ins->op == (uint16_t)JIR_RET) { ret_vreg = ins->a; continue; }
    ok = emit_insn(&ctx, ins);
    jit_reg_unpin_all(&ra);
  }

  if(ok) {
    emit_epilogue_ok(&ctx, ret_kind, ret_vreg);
    size_t trap_off = buf.len;
    emit_epilogue_trap(&ctx);
    patch_trap_jumps(&ctx, trap_off);
  }

  jit_reg_state_free(&ra);
  jello_jit_ir_free(ir);

  if(!ok || !jit_finalize_exec_buf(&buf)) {
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
