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

static void patch_trap_jumps(emit_ctx* ctx, size_t trap_off) {
  for(size_t i = 0; i < ctx->trap_jump_count; i++) {
    size_t at = ctx->trap_jumps[i];
    uint8_t op = ctx->buf->mem[at];
    if(op == 0xE9) patch_jmp_rel32(ctx->buf, at, (int32_t)(trap_off - (at + 5)));
    else patch_jcc_rel32(ctx->buf, at, (int32_t)(trap_off - (at + 6)));
  }
}

static int jir_op_allowed(jello_jit_ir_op op) {
  switch(op) {
    case JIR_NOP: case JIR_RET: case JIR_MOV:
    case JIR_CONST_I8_IMM: case JIR_CONST_I32: case JIR_CONST_I64:
    case JIR_CONST_BOOL:
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
