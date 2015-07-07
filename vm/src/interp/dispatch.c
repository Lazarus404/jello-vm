// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdio.h>
#include <stdlib.h>

/* -- op_dispatch: the cold-path table -- */

static op_result op_unimplemented(exec_ctx* ctx, const jello_insn* ins) {
  (void)ctx;
  fprintf(stderr,
      "jello_vm: opcode %u has no Chapter 3 interpreter handler yet "
      "(deferred to the chapter that owns it -- see JELLO_CHAPTER_3.md Sec 2.7)\n",
      (unsigned)ins->op);
  abort();
}

op_result op_dispatch(exec_ctx* ctx, const jello_insn* ins) {
#if defined(JELLO_USE_COMPUTED_GOTO) && (defined(__GNUC__) || defined(__clang__))
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wgnu-label-as-value"
#else
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
  static const void* const op_table[JOP_COUNT] = {
    [JOP_DIV_I32] = &&L_DIV_I32, [JOP_MOD_I32] = &&L_MOD_I32,
    [JOP_SHL_I32] = &&L_SHL_I32, [JOP_SHR_I32] = &&L_SHR_I32,
    [JOP_BITAND_I32] = &&L_BITAND_I32, [JOP_BITOR_I32] = &&L_BITOR_I32,
    [JOP_BITXOR_I32] = &&L_BITXOR_I32, [JOP_NEG_I32] = &&L_NEG_I32,

    [JOP_ADD_I64] = &&L_ADD_I64, [JOP_SUB_I64] = &&L_SUB_I64,
    [JOP_MUL_I64] = &&L_MUL_I64, [JOP_DIV_I64] = &&L_DIV_I64,
    [JOP_MOD_I64] = &&L_MOD_I64, [JOP_NEG_I64] = &&L_NEG_I64,
    [JOP_SHL_I64] = &&L_SHL_I64, [JOP_SHR_I64] = &&L_SHR_I64,
    [JOP_BITAND_I64] = &&L_BITAND_I64, [JOP_BITOR_I64] = &&L_BITOR_I64,
    [JOP_BITXOR_I64] = &&L_BITXOR_I64,
    [JOP_EQ_I64] = &&L_EQ_I64, [JOP_LT_I64] = &&L_LT_I64,

    [JOP_ADD_F16] = &&L_ADD_F16, [JOP_SUB_F16] = &&L_SUB_F16, [JOP_MUL_F16] = &&L_MUL_F16,

    [JOP_ADD_F32] = &&L_ADD_F32, [JOP_SUB_F32] = &&L_SUB_F32,
    [JOP_MUL_F32] = &&L_MUL_F32, [JOP_DIV_F32] = &&L_DIV_F32,
    [JOP_NEG_F32] = &&L_NEG_F32, [JOP_NEG_F64] = &&L_NEG_F64,
    [JOP_EQ_F32] = &&L_EQ_F32, [JOP_LT_F32] = &&L_LT_F32,

    [JOP_SEXT_I64] = &&L_SEXT_I64, [JOP_SEXT_I16] = &&L_SEXT_I16,
    [JOP_TRUNC_I8] = &&L_TRUNC_I8, [JOP_TRUNC_I16] = &&L_TRUNC_I16,
    [JOP_I32_FROM_I64] = &&L_I32_FROM_I64, [JOP_F64_FROM_I32] = &&L_F64_FROM_I32,
    [JOP_I32_FROM_F64] = &&L_I32_FROM_F64, [JOP_F64_FROM_I64] = &&L_F64_FROM_I64,
    [JOP_I64_FROM_F64] = &&L_I64_FROM_F64, [JOP_F32_FROM_I32] = &&L_F32_FROM_I32,
    [JOP_I32_FROM_F32] = &&L_I32_FROM_F32, [JOP_F64_FROM_F32] = &&L_F64_FROM_F32,
    [JOP_F32_FROM_F64] = &&L_F32_FROM_F64, [JOP_F32_FROM_I64] = &&L_F32_FROM_I64,
    [JOP_I64_FROM_F32] = &&L_I64_FROM_F32, [JOP_F16_FROM_F32] = &&L_F16_FROM_F32,
    [JOP_F32_FROM_F16] = &&L_F32_FROM_F16, [JOP_F16_FROM_I32] = &&L_F16_FROM_I32,
    [JOP_I32_FROM_F16] = &&L_I32_FROM_F16,
  };

  if((uint32_t)ins->op >= (uint32_t)JOP_COUNT || op_table[ins->op] == NULL) {
    return op_unimplemented(ctx, ins);
  }
  goto *op_table[ins->op];

  L_DIV_I32: return op_div_i32(ctx, ins);
  L_MOD_I32: return op_mod_i32(ctx, ins);
  L_SHL_I32: return op_shl_i32(ctx, ins);
  L_SHR_I32: return op_shr_i32(ctx, ins);
  L_BITAND_I32: return op_bitand_i32(ctx, ins);
  L_BITOR_I32: return op_bitor_i32(ctx, ins);
  L_BITXOR_I32: return op_bitxor_i32(ctx, ins);
  L_NEG_I32: return op_neg_i32(ctx, ins);

  L_ADD_I64: return op_add_i64(ctx, ins);
  L_SUB_I64: return op_sub_i64(ctx, ins);
  L_MUL_I64: return op_mul_i64(ctx, ins);
  L_DIV_I64: return op_div_i64(ctx, ins);
  L_MOD_I64: return op_mod_i64(ctx, ins);
  L_NEG_I64: return op_neg_i64(ctx, ins);
  L_SHL_I64: return op_shl_i64(ctx, ins);
  L_SHR_I64: return op_shr_i64(ctx, ins);
  L_BITAND_I64: return op_bitand_i64(ctx, ins);
  L_BITOR_I64: return op_bitor_i64(ctx, ins);
  L_BITXOR_I64: return op_bitxor_i64(ctx, ins);
  L_EQ_I64: return op_eq_i64(ctx, ins);
  L_LT_I64: return op_lt_i64(ctx, ins);

  L_ADD_F16: return op_add_f16(ctx, ins);
  L_SUB_F16: return op_sub_f16(ctx, ins);
  L_MUL_F16: return op_mul_f16(ctx, ins);

  L_ADD_F32: return op_add_f32(ctx, ins);
  L_SUB_F32: return op_sub_f32(ctx, ins);
  L_MUL_F32: return op_mul_f32(ctx, ins);
  L_DIV_F32: return op_div_f32(ctx, ins);
  L_NEG_F32: return op_neg_f32(ctx, ins);
  L_NEG_F64: return op_neg_f64(ctx, ins);
  L_EQ_F32: return op_eq_f32(ctx, ins);
  L_LT_F32: return op_lt_f32(ctx, ins);

  L_SEXT_I64: return op_sext_i64(ctx, ins);
  L_SEXT_I16: return op_sext_i16(ctx, ins);
  L_TRUNC_I8: return op_trunc_i8(ctx, ins);
  L_TRUNC_I16: return op_trunc_i16(ctx, ins);
  L_I32_FROM_I64: return op_i32_from_i64(ctx, ins);
  L_F64_FROM_I32: return op_f64_from_i32(ctx, ins);
  L_I32_FROM_F64: return op_i32_from_f64(ctx, ins);
  L_F64_FROM_I64: return op_f64_from_i64(ctx, ins);
  L_I64_FROM_F64: return op_i64_from_f64(ctx, ins);
  L_F32_FROM_I32: return op_f32_from_i32(ctx, ins);
  L_I32_FROM_F32: return op_i32_from_f32(ctx, ins);
  L_F64_FROM_F32: return op_f64_from_f32(ctx, ins);
  L_F32_FROM_F64: return op_f32_from_f64(ctx, ins);
  L_F32_FROM_I64: return op_f32_from_i64(ctx, ins);
  L_I64_FROM_F32: return op_i64_from_f32(ctx, ins);
  L_F16_FROM_F32: return op_f16_from_f32(ctx, ins);
  L_F32_FROM_F16: return op_f32_from_f16(ctx, ins);
  L_F16_FROM_I32: return op_f16_from_i32(ctx, ins);
  L_I32_FROM_F16: return op_i32_from_f16(ctx, ins);
#pragma GCC diagnostic pop
#else
  switch((jello_op)ins->op) {
    case JOP_DIV_I32: return op_div_i32(ctx, ins);
    case JOP_MOD_I32: return op_mod_i32(ctx, ins);
    case JOP_SHL_I32: return op_shl_i32(ctx, ins);
    case JOP_SHR_I32: return op_shr_i32(ctx, ins);
    case JOP_BITAND_I32: return op_bitand_i32(ctx, ins);
    case JOP_BITOR_I32: return op_bitor_i32(ctx, ins);
    case JOP_BITXOR_I32: return op_bitxor_i32(ctx, ins);
    case JOP_NEG_I32: return op_neg_i32(ctx, ins);

    case JOP_ADD_I64: return op_add_i64(ctx, ins);
    case JOP_SUB_I64: return op_sub_i64(ctx, ins);
    case JOP_MUL_I64: return op_mul_i64(ctx, ins);
    case JOP_DIV_I64: return op_div_i64(ctx, ins);
    case JOP_MOD_I64: return op_mod_i64(ctx, ins);
    case JOP_NEG_I64: return op_neg_i64(ctx, ins);
    case JOP_SHL_I64: return op_shl_i64(ctx, ins);
    case JOP_SHR_I64: return op_shr_i64(ctx, ins);
    case JOP_BITAND_I64: return op_bitand_i64(ctx, ins);
    case JOP_BITOR_I64: return op_bitor_i64(ctx, ins);
    case JOP_BITXOR_I64: return op_bitxor_i64(ctx, ins);
    case JOP_EQ_I64: return op_eq_i64(ctx, ins);
    case JOP_LT_I64: return op_lt_i64(ctx, ins);

    case JOP_ADD_F16: return op_add_f16(ctx, ins);
    case JOP_SUB_F16: return op_sub_f16(ctx, ins);
    case JOP_MUL_F16: return op_mul_f16(ctx, ins);

    case JOP_ADD_F32: return op_add_f32(ctx, ins);
    case JOP_SUB_F32: return op_sub_f32(ctx, ins);
    case JOP_MUL_F32: return op_mul_f32(ctx, ins);
    case JOP_DIV_F32: return op_div_f32(ctx, ins);
    case JOP_NEG_F32: return op_neg_f32(ctx, ins);
    case JOP_NEG_F64: return op_neg_f64(ctx, ins);
    case JOP_EQ_F32: return op_eq_f32(ctx, ins);
    case JOP_LT_F32: return op_lt_f32(ctx, ins);

    case JOP_SEXT_I64: return op_sext_i64(ctx, ins);
    case JOP_SEXT_I16: return op_sext_i16(ctx, ins);
    case JOP_TRUNC_I8: return op_trunc_i8(ctx, ins);
    case JOP_TRUNC_I16: return op_trunc_i16(ctx, ins);
    case JOP_I32_FROM_I64: return op_i32_from_i64(ctx, ins);
    case JOP_F64_FROM_I32: return op_f64_from_i32(ctx, ins);
    case JOP_I32_FROM_F64: return op_i32_from_f64(ctx, ins);
    case JOP_F64_FROM_I64: return op_f64_from_i64(ctx, ins);
    case JOP_I64_FROM_F64: return op_i64_from_f64(ctx, ins);
    case JOP_F32_FROM_I32: return op_f32_from_i32(ctx, ins);
    case JOP_I32_FROM_F32: return op_i32_from_f32(ctx, ins);
    case JOP_F64_FROM_F32: return op_f64_from_f32(ctx, ins);
    case JOP_F32_FROM_F64: return op_f32_from_f64(ctx, ins);
    case JOP_F32_FROM_I64: return op_f32_from_i64(ctx, ins);
    case JOP_I64_FROM_F32: return op_i64_from_f32(ctx, ins);
    case JOP_F16_FROM_F32: return op_f16_from_f32(ctx, ins);
    case JOP_F32_FROM_F16: return op_f32_from_f16(ctx, ins);
    case JOP_F16_FROM_I32: return op_f16_from_i32(ctx, ins);
    case JOP_I32_FROM_F16: return op_i32_from_f16(ctx, ins);
    default: return op_unimplemented(ctx, ins);
  }
#endif
}
