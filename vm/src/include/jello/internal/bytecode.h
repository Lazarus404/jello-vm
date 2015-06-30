// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_BYTECODE_H
#define JELLO_BYTECODE_H

#include <jello.h>

/* Opcode tables and in-memory function layouts. */

/* -- container feature bitset -- */
typedef enum jello_bc_feature_flags {
  JELLO_BC_FEAT_NONE = 0,
  JELLO_BC_FEAT_CONST64 = 1u << 0,
  JELLO_BC_FEAT_CONSTBYTES = 1u << 1,
  JELLO_BC_FEAT_CAP_START = 1u << 2,
  JELLO_BC_FEAT_LINE_TABLE = 1u << 3,
  JELLO_BC_FEAT_LINE_COL = 1u << 4,
  JELLO_BC_FEAT_LINE_DELTA = 1u << 5,
  JELLO_BC_FEAT_COMPRESSED_INSN = 1u << 6,
  JELLO_BC_FEAT_LOOP_ANCHOR = 1u << 7,
  JELLO_BC_FEAT_TRAP_MAP = 1u << 8,
  JELLO_BC_FEAT_HANDLER_MAP = 1u << 9,
  JELLO_BC_FEAT_TYPE_PROFILE = 1u << 10,
  JELLO_BC_FEAT_STACK_MAP = 1u << 11,
  JELLO_BC_FEAT_PKG_DEPS = 1u << 12,
} jello_bc_feature_flags;

/* -- opcode table -- */
typedef enum jello_op {
  /* Control */
  JOP_NOP = 0,
  JOP_RET = 1,
  JOP_JMP = 2,
  JOP_JMP_IF = 3,
  JOP_MOV = 4,
  JOP_TRY = 5,
  JOP_ENDTRY = 6,
  JOP_THROW = 7,
  JOP_ASSERT = 8,
  JOP_SWITCH_KIND = 9,
  JOP_CASE_KIND = 10,

  /* Calls / closures */
  JOP_CALL = 11,
  JOP_CALLR = 12,
  JOP_TAILCALL = 13,
  JOP_TAILCALLR = 14,
  JOP_CONST_FUN = 15,
  JOP_CLOSURE = 16,
  JOP_BIND_THIS = 17,

  /* Typed constants */
  JOP_CONST_I8_IMM = 18,
  JOP_CONST_I32 = 19,
  JOP_CONST_I64 = 20,
  JOP_CONST_F16 = 21,
  JOP_CONST_F32 = 22,
  JOP_CONST_F64 = 23,
  JOP_CONST_BOOL = 24,
  JOP_CONST_NULL = 25,
  JOP_CONST_ATOM = 26,
  JOP_CONST_BYTES = 27,

  /* I32 arithmetic / bitwise */
  JOP_ADD_I32 = 28,
  JOP_SUB_I32 = 29,
  JOP_MUL_I32 = 30,
  JOP_DIV_I32 = 31,
  JOP_MOD_I32 = 32,
  JOP_ADD_I32_IMM = 33,
  JOP_SUB_I32_IMM = 34,
  JOP_MUL_I32_IMM = 35,
  JOP_NEG_I32 = 36,
  JOP_SHL_I32 = 37,
  JOP_SHR_I32 = 38,
  JOP_BITAND_I32 = 39,
  JOP_BITOR_I32 = 40,
  JOP_BITXOR_I32 = 41,

  /* I64 arithmetic / bitwise */
  JOP_ADD_I64 = 42,
  JOP_SUB_I64 = 43,
  JOP_MUL_I64 = 44,
  JOP_DIV_I64 = 45,
  JOP_MOD_I64 = 46,
  JOP_NEG_I64 = 47,
  JOP_SHL_I64 = 48,
  JOP_SHR_I64 = 49,
  JOP_BITAND_I64 = 50,
  JOP_BITOR_I64 = 51,
  JOP_BITXOR_I64 = 52,

  /* Float arithmetic + bool negation */
  JOP_ADD_F16 = 53,
  JOP_SUB_F16 = 54,
  JOP_MUL_F16 = 55,
  JOP_ADD_F32 = 56,
  JOP_SUB_F32 = 57,
  JOP_MUL_F32 = 58,
  JOP_DIV_F32 = 59,
  JOP_NEG_F32 = 60,
  JOP_ADD_F64 = 61,
  JOP_SUB_F64 = 62,
  JOP_MUL_F64 = 63,
  JOP_DIV_F64 = 64,
  JOP_NEG_F64 = 65,
  JOP_NOT_BOOL = 66,

  /* Comparisons */
  JOP_EQ_I32 = 67,
  JOP_LT_I32 = 68,
  JOP_EQ_I32_IMM = 69,
  JOP_LT_I32_IMM = 70,
  JOP_EQ_I64 = 71,
  JOP_LT_I64 = 72,
  JOP_EQ_F32 = 73,
  JOP_LT_F32 = 74,
  JOP_EQ_F64 = 75,
  JOP_LT_F64 = 76,

  /* Conversions / width changes */
  JOP_SEXT_I64 = 77,
  JOP_SEXT_I16 = 78,
  JOP_TRUNC_I8 = 79,
  JOP_TRUNC_I16 = 80,
  JOP_I32_FROM_I64 = 81,
  JOP_F64_FROM_I32 = 82,
  JOP_I32_FROM_F64 = 83,
  JOP_F64_FROM_I64 = 84,
  JOP_I64_FROM_F64 = 85,
  JOP_F32_FROM_I32 = 86,
  JOP_I32_FROM_F32 = 87,
  JOP_F64_FROM_F32 = 88,
  JOP_F32_FROM_F64 = 89,
  JOP_F32_FROM_I64 = 90,
  JOP_I64_FROM_F32 = 91,
  JOP_F16_FROM_F32 = 92,
  JOP_F32_FROM_F16 = 93,
  JOP_F16_FROM_I32 = 94,
  JOP_I32_FROM_F16 = 95,

  /* Boxing/unboxing boundary + spill */
  JOP_TO_DYN = 96,
  JOP_FROM_DYN_I8 = 97,
  JOP_FROM_DYN_I16 = 98,
  JOP_FROM_DYN_I32 = 99,
  JOP_FROM_DYN_I64 = 100,
  JOP_FROM_DYN_F16 = 101,
  JOP_FROM_DYN_F32 = 102,
  JOP_FROM_DYN_F64 = 103,
  JOP_FROM_DYN_BOOL = 104,
  JOP_FROM_DYN_ATOM = 105,
  JOP_FROM_DYN_PTR = 106,
  JOP_SPILL_PUSH = 107,
  JOP_SPILL_POP = 108,

  /* Identity / type introspection */
  JOP_PHYSEQ = 109,
  JOP_KINDOF = 110,

  /* Lists / arrays */
  JOP_LIST_NIL = 111,
  JOP_LIST_CONS = 112,
  JOP_LIST_HEAD = 113,
  JOP_LIST_TAIL = 114,
  JOP_LIST_IS_NIL = 115,
  JOP_ARRAY_NEW = 116,
  JOP_ARRAY_LEN = 117,
  JOP_ARRAY_GET = 118,
  JOP_ARRAY_SET = 119,
  JOP_ARRAY_CONCAT2 = 120,
  JOP_ARRAY_RESIZE = 121,

  /* Bytes */
  JOP_BYTES_NEW = 122,
  JOP_BYTES_LEN = 123,
  JOP_BYTES_GET_U8 = 124,
  JOP_BYTES_SET_U8 = 125,
  JOP_BYTES_CONCAT2 = 126,
  JOP_BYTES_CONCAT_MANY = 127,
  JOP_BYTES_EQ = 128,
  JOP_BYTES_BITAND2 = 129,
  JOP_BYTES_BITOR2 = 130,
  JOP_BYTES_BITXOR2 = 131,
  JOP_BYTES_READ_U16_LE = 132,
  JOP_BYTES_READ_U16_BE = 133,
  JOP_BYTES_READ_U32_LE = 134,
  JOP_BYTES_READ_U32_BE = 135,
  JOP_BYTES_READ_I32_LE = 136,
  JOP_BYTES_READ_I32_BE = 137,
  JOP_BYTES_READ_F32_LE = 138,
  JOP_BYTES_READ_F32_BE = 139,
  JOP_BYTES_WRITE_U16_LE = 140,
  JOP_BYTES_WRITE_U16_BE = 141,
  JOP_BYTES_WRITE_U32_LE = 142,
  JOP_BYTES_WRITE_U32_BE = 143,
  JOP_BYTES_WRITE_I32_LE = 144,
  JOP_BYTES_WRITE_I32_BE = 145,
  JOP_BYTES_WRITE_F32_LE = 146,
  JOP_BYTES_WRITE_F32_BE = 147,
  JOP_BITS_WRITE = 148,
  JOP_BITS_READ = 149,

  /* Struct (fixed-offset field access) */
  JOP_STRUCT_GET = 150,
  JOP_STRUCT_SET = 151,

  /* Object (atom-dispatched field access) */
  JOP_OBJ_NEW = 152,
  JOP_OBJ_HAS_ATOM = 153,
  JOP_OBJ_GET_ATOM = 154,
  JOP_OBJ_SET_ATOM = 155,
  JOP_OBJ_GET = 156,
  JOP_OBJ_SET = 157,
  JOP_OBJ_HAS = 158,
  JOP_OBJ_REMOVE = 159,
  JOP_OBJ_CLEAR = 160,
  JOP_OBJ_COPY = 161,
  JOP_OBJ_KEYS = 162,

  /* Enum */
  JOP_ENUM_NEW = 163,
  JOP_ENUM_TAG = 164,
  JOP_ENUM_GET = 165,
} jello_op;

#define JOP_COUNT (JOP_ENUM_GET + 1)

/* -- canonical instruction encoding -- */
typedef struct jello_insn {
  uint8_t op;
  uint8_t a;
  uint8_t b;
  uint8_t c;
  uint32_t imm;
} jello_insn;

typedef struct jello_bc_function {
  uint32_t nregs;
  uint32_t cap_start;
  const jello_type_id* reg_types;
  uint32_t ninsns;
  const jello_insn* insns;
} jello_bc_function;

static inline int jello_bc_op_is_valid(uint32_t op) {
  return op < (uint32_t)JOP_COUNT;
}

static inline int jello_bc_type_kind_is_valid(uint32_t kind) {
  return kind >= (uint32_t)JELLO_T_VOID && kind <= (uint32_t)JELLO_T_ENUM;
}

#endif /* JELLO_BYTECODE_H */
