// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <limits.h>
#include <string.h>

/* -- cold-path numeric opcode handlers -- */

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

static void vm_store_bool(reg_frame* rf, uint32_t r, int b) {
  vm_store_u32(rf, r, b ? 1u : 0u);
}

/* -- F16 <-> F32 bit conversion -- */
static uint16_t vm_load_f16_bits(reg_frame* rf, uint32_t r) {
  return (uint16_t)vm_load_u32(rf, r);
}
static void vm_store_f16_bits(reg_frame* rf, uint32_t r, uint16_t bits) {
  vm_store_u32(rf, r, (uint32_t)bits);
}

static float f16_bits_to_f32(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  uint32_t exp = (uint32_t)(h >> 10) & 0x1Fu;
  uint32_t mant = (uint32_t)(h & 0x3FFu);
  uint32_t bits;
  if(exp == 0u) {
    if(mant == 0u) {
      bits = sign;
    } else {
      int e = -1;
      uint32_t m = mant;
      do {
        m <<= 1u;
        e++;
      } while((m & 0x400u) == 0u);
      m &= 0x3FFu;
      bits = sign | ((uint32_t)(127 - 15 - e) << 23) | (m << 13);
    }
  } else if(exp == 0x1Fu) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
  }
  return vm_f32_from_bits(bits);
}

static uint16_t f32_to_f16_bits(float f) {
  uint32_t bits;
  memcpy(&bits, &f, sizeof bits);
  uint32_t sign = (bits >> 16) & 0x8000u;
  uint32_t biased_exp = (bits >> 23) & 0xFFu;
  uint32_t mant = bits & 0x7FFFFFu;

  if(biased_exp == 0xFFu) {
    return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));
  }
  int32_t exp = (int32_t)biased_exp - 127 + 15;
  if(exp <= 0) return (uint16_t)sign;
  if(exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);
  return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

/* -- I32: bitwise + shift + div/mod + negate -- */

op_result op_div_i32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t b = vm_load_u32(&fr->rf, ins->c);
  if(b == 0u) {
    ctx->vm->trap_code = JELLO_TRAP_ARITHMETIC;
    return OP_TRAP;
  }
  uint32_t a = vm_load_u32(&fr->rf, ins->b);
  if(b == (uint32_t)(uint32_t)-1 && a == (uint32_t)INT32_MIN) {
    ctx->vm->trap_code = JELLO_TRAP_ARITHMETIC;
    return OP_TRAP;
  }
  jello_type_kind k = vm_reg_kind(ctx->m, fr->f, ins->a);
  vm_store_u32_masked(&fr->rf, ins->a, (uint32_t)((int32_t)a / (int32_t)b), k);
  return OP_CONTINUE;
}

op_result op_mod_i32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t b = vm_load_u32(&fr->rf, ins->c);
  if(b == 0u) {
    ctx->vm->trap_code = JELLO_TRAP_ARITHMETIC;
    return OP_TRAP;
  }
  uint32_t a = vm_load_u32(&fr->rf, ins->b);
  if(b == (uint32_t)(uint32_t)-1 && a == (uint32_t)INT32_MIN) {
    ctx->vm->trap_code = JELLO_TRAP_ARITHMETIC;
    return OP_TRAP;
  }
  jello_type_kind k = vm_reg_kind(ctx->m, fr->f, ins->a);
  vm_store_u32_masked(&fr->rf, ins->a, (uint32_t)((int32_t)a % (int32_t)b), k);
  return OP_CONTINUE;
}

#define DEF_BIN_I32_MASKED(NAME, EXPR)                                            \
  op_result NAME(exec_ctx* ctx, const jello_insn* ins) {                          \
    call_frame* fr = current_frame(ctx);                                          \
    uint32_t a = vm_load_u32(&fr->rf, ins->b);                                    \
    uint32_t b = vm_load_u32(&fr->rf, ins->c);                                    \
    jello_type_kind k = vm_reg_kind(ctx->m, fr->f, ins->a);                       \
    vm_store_u32_masked(&fr->rf, ins->a, (uint32_t)(EXPR), k);                    \
    return OP_CONTINUE;                                                           \
  }

DEF_BIN_I32_MASKED(op_shl_i32, (int32_t)a << (int32_t)(b & 31u))
DEF_BIN_I32_MASKED(op_shr_i32, (int32_t)a >> (int32_t)(b & 31u))
DEF_BIN_I32_MASKED(op_bitand_i32, a & b)
DEF_BIN_I32_MASKED(op_bitor_i32, a | b)
DEF_BIN_I32_MASKED(op_bitxor_i32, a ^ b)

/* Unsigned subtraction from zero avoids the signed-overflow UB that `-x`
 * would hit at INT32_MIN. */
op_result op_neg_i32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t x = vm_load_u32(&fr->rf, ins->b);
  jello_type_kind k = vm_reg_kind(ctx->m, fr->f, ins->a);
  vm_store_u32_masked(&fr->rf, ins->a, (uint32_t)0u - x, k);
  return OP_CONTINUE;
}

/* -- I64: full arithmetic + bitwise + shift + comparisons -- */

#define DEF_BIN_I64(NAME, EXPR)                                \
  op_result NAME(exec_ctx* ctx, const jello_insn* ins) {       \
    call_frame* fr = current_frame(ctx);                       \
    int64_t a = vm_load_i64(&fr->rf, ins->b);                  \
    int64_t b = vm_load_i64(&fr->rf, ins->c);                  \
    vm_store_i64(&fr->rf, ins->a, (int64_t)(EXPR));            \
    return OP_CONTINUE;                                        \
  }

DEF_BIN_I64(op_add_i64, (uint64_t)a + (uint64_t)b)
DEF_BIN_I64(op_sub_i64, (uint64_t)a - (uint64_t)b)
DEF_BIN_I64(op_mul_i64, (uint64_t)a * (uint64_t)b)
DEF_BIN_I64(op_bitand_i64, (uint64_t)a & (uint64_t)b)
DEF_BIN_I64(op_bitor_i64, (uint64_t)a | (uint64_t)b)
DEF_BIN_I64(op_bitxor_i64, (uint64_t)a ^ (uint64_t)b)
DEF_BIN_I64(op_shl_i64, (uint64_t)a << (int)((uint64_t)b & 63u))
DEF_BIN_I64(op_shr_i64, a >> (int)((uint64_t)b & 63u))

op_result op_div_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int64_t b = vm_load_i64(&fr->rf, ins->c);
  if(b == 0) {
    ctx->vm->trap_code = JELLO_TRAP_ARITHMETIC;
    return OP_TRAP;
  }
  int64_t a = vm_load_i64(&fr->rf, ins->b);
  if(b == -1 && a == INT64_MIN) {
    ctx->vm->trap_code = JELLO_TRAP_ARITHMETIC;
    return OP_TRAP;
  }
  vm_store_i64(&fr->rf, ins->a, a / b);
  return OP_CONTINUE;
}

op_result op_mod_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int64_t b = vm_load_i64(&fr->rf, ins->c);
  if(b == 0) {
    ctx->vm->trap_code = JELLO_TRAP_ARITHMETIC;
    return OP_TRAP;
  }
  int64_t a = vm_load_i64(&fr->rf, ins->b);
  if(b == -1 && a == INT64_MIN) {
    ctx->vm->trap_code = JELLO_TRAP_ARITHMETIC;
    return OP_TRAP;
  }
  vm_store_i64(&fr->rf, ins->a, a % b);
  return OP_CONTINUE;
}

op_result op_neg_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int64_t x = vm_load_i64(&fr->rf, ins->b);
  vm_store_i64(&fr->rf, ins->a, (int64_t)(0ull - (uint64_t)x));
  return OP_CONTINUE;
}

op_result op_eq_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int64_t a = vm_load_i64(&fr->rf, ins->b);
  int64_t b = vm_load_i64(&fr->rf, ins->c);
  vm_store_bool(&fr->rf, ins->a, a == b);
  return OP_CONTINUE;
}

op_result op_lt_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int64_t a = vm_load_i64(&fr->rf, ins->b);
  int64_t b = vm_load_i64(&fr->rf, ins->c);
  vm_store_bool(&fr->rf, ins->a, a < b);
  return OP_CONTINUE;
}

/* -- F16 (via F32 promotion) -- */

#define DEF_BIN_F16(NAME, EXPR)                                              \
  op_result NAME(exec_ctx* ctx, const jello_insn* ins) {                     \
    call_frame* fr = current_frame(ctx);                                     \
    float a = f16_bits_to_f32(vm_load_f16_bits(&fr->rf, ins->b));            \
    float b = f16_bits_to_f32(vm_load_f16_bits(&fr->rf, ins->c));            \
    vm_store_f16_bits(&fr->rf, ins->a, f32_to_f16_bits((float)(EXPR)));      \
    return OP_CONTINUE;                                                      \
  }

DEF_BIN_F16(op_add_f16, a + b)
DEF_BIN_F16(op_sub_f16, a - b)
DEF_BIN_F16(op_mul_f16, a * b)

/* -- F32 -- */

#define DEF_BIN_F32(NAME, EXPR)                            \
  op_result NAME(exec_ctx* ctx, const jello_insn* ins) {   \
    call_frame* fr = current_frame(ctx);                   \
    float a = vm_load_f32(&fr->rf, ins->b);                \
    float b = vm_load_f32(&fr->rf, ins->c);                \
    vm_store_f32(&fr->rf, ins->a, (float)(EXPR));          \
    return OP_CONTINUE;                                    \
  }

DEF_BIN_F32(op_add_f32, a + b)
DEF_BIN_F32(op_sub_f32, a - b)
DEF_BIN_F32(op_mul_f32, a * b)
/* IEEE-754 division by zero yields +-inf/NaN, not undefined behaviour, so
 * unlike DIV_I32/DIV_I64 (Sec 2.6.2), this needs no added trap. */
DEF_BIN_F32(op_div_f32, a / b)

op_result op_neg_f32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  float x = vm_load_f32(&fr->rf, ins->b);
  vm_store_f32(&fr->rf, ins->a, -x);
  return OP_CONTINUE;
}

op_result op_neg_f64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  double x = vm_load_f64(&fr->rf, ins->b);
  vm_store_f64(&fr->rf, ins->a, -x);
  return OP_CONTINUE;
}

op_result op_eq_f32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  float a = vm_load_f32(&fr->rf, ins->b);
  float b = vm_load_f32(&fr->rf, ins->c);
  vm_store_bool(&fr->rf, ins->a, a == b);
  return OP_CONTINUE;
}

op_result op_lt_f32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  float a = vm_load_f32(&fr->rf, ins->b);
  float b = vm_load_f32(&fr->rf, ins->c);
  vm_store_bool(&fr->rf, ins->a, a < b);
  return OP_CONTINUE;
}

/* -- conversions -- */

op_result op_sext_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int32_t x = (int32_t)vm_load_u32(&fr->rf, ins->b);
  vm_store_i64(&fr->rf, ins->a, (int64_t)x);
  return OP_CONTINUE;
}

op_result op_sext_i16(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int16_t x = (int16_t)vm_load_u32(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(int32_t)x);
  return OP_CONTINUE;
}

op_result op_trunc_i8(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t x = vm_load_u32(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, x & 0xFFu);
  return OP_CONTINUE;
}

op_result op_trunc_i16(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t x = vm_load_u32(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, x & 0xFFFFu);
  return OP_CONTINUE;
}

op_result op_i32_from_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int64_t x = vm_load_i64(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(int32_t)x);
  return OP_CONTINUE;
}

op_result op_f64_from_i32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int32_t x = (int32_t)vm_load_u32(&fr->rf, ins->b);
  vm_store_f64(&fr->rf, ins->a, (double)x);
  return OP_CONTINUE;
}

op_result op_i32_from_f64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  double x = vm_load_f64(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(int32_t)x);
  return OP_CONTINUE;
}

op_result op_f64_from_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int64_t x = vm_load_i64(&fr->rf, ins->b);
  vm_store_f64(&fr->rf, ins->a, (double)x);
  return OP_CONTINUE;
}

op_result op_i64_from_f64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  double x = vm_load_f64(&fr->rf, ins->b);
  vm_store_i64(&fr->rf, ins->a, (int64_t)x);
  return OP_CONTINUE;
}

op_result op_f32_from_i32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int32_t x = (int32_t)vm_load_u32(&fr->rf, ins->b);
  vm_store_f32(&fr->rf, ins->a, (float)x);
  return OP_CONTINUE;
}

op_result op_i32_from_f32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  float x = vm_load_f32(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(int32_t)x);
  return OP_CONTINUE;
}

op_result op_f64_from_f32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  float x = vm_load_f32(&fr->rf, ins->b);
  vm_store_f64(&fr->rf, ins->a, (double)x);
  return OP_CONTINUE;
}

op_result op_f32_from_f64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  double x = vm_load_f64(&fr->rf, ins->b);
  vm_store_f32(&fr->rf, ins->a, (float)x);
  return OP_CONTINUE;
}

op_result op_f32_from_i64(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int64_t x = vm_load_i64(&fr->rf, ins->b);
  vm_store_f32(&fr->rf, ins->a, (float)x);
  return OP_CONTINUE;
}

op_result op_i64_from_f32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  float x = vm_load_f32(&fr->rf, ins->b);
  vm_store_i64(&fr->rf, ins->a, (int64_t)x);
  return OP_CONTINUE;
}

op_result op_f16_from_f32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  float x = vm_load_f32(&fr->rf, ins->b);
  vm_store_f16_bits(&fr->rf, ins->a, f32_to_f16_bits(x));
  return OP_CONTINUE;
}

op_result op_f32_from_f16(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint16_t bits = vm_load_f16_bits(&fr->rf, ins->b);
  vm_store_f32(&fr->rf, ins->a, f16_bits_to_f32(bits));
  return OP_CONTINUE;
}

op_result op_f16_from_i32(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  int32_t x = (int32_t)vm_load_u32(&fr->rf, ins->b);
  vm_store_f16_bits(&fr->rf, ins->a, f32_to_f16_bits((float)x));
  return OP_CONTINUE;
}

op_result op_i32_from_f16(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint16_t bits = vm_load_f16_bits(&fr->rf, ins->b);
  float x = f16_bits_to_f32(bits);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(int32_t)x);
  return OP_CONTINUE;
}
