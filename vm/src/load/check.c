// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

/* -- check -- */

static jello_bc_result ok(void) {
  jello_bc_result r = { JELLO_BC_OK, "ok", 0 };
  return r;
}

static jello_bc_result err(jello_bc_error e, const char* msg, size_t off) {
  jello_bc_result r = { e, msg, off };
  return r;
}

size_t jello_slot_size(jello_type_kind k) {
  switch(k) {
    case JELLO_T_VOID:
    case JELLO_T_BOOL:
    case JELLO_T_ATOM:
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32:
    case JELLO_T_F16:
    case JELLO_T_F32:
      return 4;
    case JELLO_T_I64:
    case JELLO_T_F64:
      return 8;
    case JELLO_T_DYNAMIC:
      return sizeof(uintptr_t);
    case JELLO_T_BYTES:
    case JELLO_T_LIST:
    case JELLO_T_ARRAY:
    case JELLO_T_STRUCT:
    case JELLO_T_OBJECT:
    case JELLO_T_FUNCTION:
    case JELLO_T_ABSTRACT:
    case JELLO_T_ENUM:
      return sizeof(void*);
  }
  return 0;
}

static jello_type_kind reg_kind(const jello_bc_module* m, const jello_type_id* reg_types, uint32_t r) {
  return m->types[reg_types[r]].kind;
}

static int is_narrow_int_kind(jello_type_kind k) {
  return k == JELLO_T_I8 || k == JELLO_T_I16 || k == JELLO_T_I32;
}

/* -- arithmetic/comparison/conversion validation -- */
static jello_bc_result validate_bin(const jello_bc_module* m, const jello_type_id* reg_types,
                                     const jello_insn* ins, uint32_t nregs,
                                     jello_type_kind operand_kind, int dest_may_narrow) {
  if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) {
    return err(JELLO_BC_BAD_FORMAT, "arith reg out of range", 0);
  }
  jello_type_kind ka = reg_kind(m, reg_types, ins->a);
  if(dest_may_narrow ? !is_narrow_int_kind(ka) : ka != operand_kind) {
    return err(JELLO_BC_BAD_FORMAT, "arith dest wrong kind", 0);
  }
  if(reg_kind(m, reg_types, ins->b) != operand_kind || reg_kind(m, reg_types, ins->c) != operand_kind) {
    return err(JELLO_BC_BAD_FORMAT, "arith operand wrong kind", 0);
  }
  return ok();
}

static jello_bc_result validate_un(const jello_bc_module* m, const jello_type_id* reg_types,
                                    const jello_insn* ins, uint32_t nregs,
                                    jello_type_kind operand_kind, int dest_may_narrow) {
  if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "arith reg out of range", 0);
  jello_type_kind ka = reg_kind(m, reg_types, ins->a);
  if(dest_may_narrow ? !is_narrow_int_kind(ka) : ka != operand_kind) {
    return err(JELLO_BC_BAD_FORMAT, "arith dest wrong kind", 0);
  }
  if(reg_kind(m, reg_types, ins->b) != operand_kind) return err(JELLO_BC_BAD_FORMAT, "arith operand wrong kind", 0);
  return ok();
}

static jello_bc_result validate_bin_imm(const jello_bc_module* m, const jello_type_id* reg_types,
                                         const jello_insn* ins, uint32_t nregs,
                                         jello_type_kind operand_kind) {
  if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "arith reg out of range", 0);
  if(!is_narrow_int_kind(reg_kind(m, reg_types, ins->a))) return err(JELLO_BC_BAD_FORMAT, "arith dest wrong kind", 0);
  if(reg_kind(m, reg_types, ins->b) != operand_kind) return err(JELLO_BC_BAD_FORMAT, "arith operand wrong kind", 0);
  return ok();
}

static jello_bc_result validate_cmp(const jello_bc_module* m, const jello_type_id* reg_types,
                                     const jello_insn* ins, uint32_t nregs, jello_type_kind operand_kind) {
  if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "cmp reg out of range", 0);
  if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL) return err(JELLO_BC_BAD_FORMAT, "cmp dest must be Bool", 0);
  if(reg_kind(m, reg_types, ins->b) != operand_kind || reg_kind(m, reg_types, ins->c) != operand_kind) {
    return err(JELLO_BC_BAD_FORMAT, "cmp operand wrong kind", 0);
  }
  return ok();
}

static jello_bc_result validate_cmp_imm(const jello_bc_module* m, const jello_type_id* reg_types,
                                         const jello_insn* ins, uint32_t nregs, jello_type_kind operand_kind) {
  if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "cmp reg out of range", 0);
  if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL) return err(JELLO_BC_BAD_FORMAT, "cmp dest must be Bool", 0);
  if(reg_kind(m, reg_types, ins->b) != operand_kind) return err(JELLO_BC_BAD_FORMAT, "cmp operand wrong kind", 0);
  return ok();
}

static jello_bc_result validate_conv(const jello_bc_module* m, const jello_type_id* reg_types,
                                      const jello_insn* ins, uint32_t nregs,
                                      jello_type_kind dst_kind, jello_type_kind src_kind) {
  if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "conv reg out of range", 0);
  if(reg_kind(m, reg_types, ins->a) != dst_kind) return err(JELLO_BC_BAD_FORMAT, "conv dest wrong kind", 0);
  if(reg_kind(m, reg_types, ins->b) != src_kind) return err(JELLO_BC_BAD_FORMAT, "conv src wrong kind", 0);
  return ok();
}

static jello_bc_result validate_const(const jello_bc_module* m, const jello_type_id* reg_types,
                                       const jello_insn* ins, uint32_t nregs, jello_type_kind kind) {
  if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "const reg out of range", 0);
  if(reg_kind(m, reg_types, ins->a) != kind) return err(JELLO_BC_BAD_FORMAT, "const dest wrong kind", 0);
  return ok();
}

static jello_bc_result validate_mov(const jello_bc_module* m, const jello_type_id* reg_types,
                                     const jello_insn* ins) {
  jello_type_id tid_a = reg_types[ins->a];
  jello_type_id tid_b = reg_types[ins->b];
  if(tid_a == tid_b) return ok();

  jello_type_kind ka = m->types[tid_a].kind;
  jello_type_kind kb = m->types[tid_b].kind;

  if(ka == kb) {
    if(ka == JELLO_T_STRUCT &&
       m->types[tid_a].as.strct.struct_sig_id != m->types[tid_b].as.strct.struct_sig_id) {
      return err(JELLO_BC_BAD_FORMAT, "mov struct layout mismatch", 0);
    }
    return ok();
  }

  if(ka == JELLO_T_STRUCT || kb == JELLO_T_STRUCT) {
    return err(JELLO_BC_BAD_FORMAT, "mov struct type mismatch", 0);
  }

  if(ka == JELLO_T_DYNAMIC || kb == JELLO_T_DYNAMIC) {
    size_t sa = jello_slot_size(ka);
    size_t sb = jello_slot_size(kb);
    if(sa == sb && sa > 0) return ok();
  }

  return err(JELLO_BC_BAD_FORMAT, "mov type mismatch", 0);
}

jello_bc_result jello_bc_validate_insn(
    const jello_bc_module* m,
    const jello_type_id* reg_types,
    const jello_insn* ins,
    uint32_t nregs,
    uint32_t pc,
    uint32_t ninsns,
    uint32_t nfuncs) {
  const uint8_t op = ins->op;

  switch((jello_op)op) {
    case JOP_NOP:
      return ok();

    case JOP_RET:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "ret reg out of range", 0);
      return ok();

    case JOP_MOV:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "mov reg out of range", 0);
      return validate_mov(m, reg_types, ins);

    case JOP_JMP: {
      int32_t d = (int32_t)ins->imm;
      int32_t tgt = (int32_t)(pc + 1u) + d;
      if(tgt < 0 || tgt >= (int32_t)ninsns) return err(JELLO_BC_BAD_FORMAT, "jmp target out of range", 0);
      return ok();
    }

    case JOP_JMP_IF: {
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "jmp_if cond reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL) return err(JELLO_BC_BAD_FORMAT, "jmp_if cond must be bool", 0);
      int32_t d = (int32_t)ins->imm;
      int32_t tgt = (int32_t)(pc + 1u) + d;
      if(tgt < 0 || tgt >= (int32_t)ninsns) return err(JELLO_BC_BAD_FORMAT, "jmp_if target out of range", 0);
      return ok();
    }

    case JOP_ASSERT:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "assert cond reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL) return err(JELLO_BC_BAD_FORMAT, "assert cond must be bool", 0);
      if(ins->c > 1) return err(JELLO_BC_BAD_FORMAT, "assert c must be 0/1", 0);
      if(ins->c == 1u) {
        if(ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "assert msg reg out of range", 0);
        if(reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES) return err(JELLO_BC_BAD_FORMAT, "assert msg must be Bytes", 0);
      } else if(ins->b != 0u) {
        return err(JELLO_BC_BAD_FORMAT, "assert b must be 0 when c is 0", 0);
      }
      return ok();

    case JOP_CALL: {
      if(ins->imm >= nfuncs) return err(JELLO_BC_BAD_FORMAT, "function index out of range", 0);
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "call dest reg out of range", 0);
      if((uint64_t)ins->b + (uint64_t)ins->c > (uint64_t)nregs) {
        return err(JELLO_BC_BAD_FORMAT, "call arg regs out of range", 0);
      }
      const jello_bc_function* callee = &m->funcs[ins->imm];
      if(ins->c > callee->nregs) return err(JELLO_BC_BAD_FORMAT, "call passes more args than callee has registers", 0);
      for(uint32_t i = 0; i < ins->c; i++) {
        if(reg_kind(m, reg_types, ins->b + i) != vm_reg_kind(m, callee, i)) {
          return err(JELLO_BC_BAD_FORMAT, "call arg type mismatch", 0);
        }
      }
      return ok();
    }

    case JOP_CALLR:
    case JOP_TAILCALL:
    case JOP_TAILCALLR:
    case JOP_CONST_FUN:
    case JOP_CLOSURE:
      if(ins->imm >= nfuncs) return err(JELLO_BC_BAD_FORMAT, "function index out of range", 0);
      return ok();

    case JOP_CONST_I8_IMM:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "const reg out of range", 0);
      if(!is_narrow_int_kind(reg_kind(m, reg_types, ins->a))) return err(JELLO_BC_BAD_FORMAT, "const dest wrong kind", 0);
      return ok();
    case JOP_CONST_I32:
      return validate_const(m, reg_types, ins, nregs, JELLO_T_I32);
    case JOP_CONST_I64:
      if(ins->imm >= m->nconst_i64) return err(JELLO_BC_BAD_FORMAT, "const_i64 index out of range", 0);
      return validate_const(m, reg_types, ins, nregs, JELLO_T_I64);
    case JOP_CONST_F16:
      return validate_const(m, reg_types, ins, nregs, JELLO_T_F16);
    case JOP_CONST_F32:
      return validate_const(m, reg_types, ins, nregs, JELLO_T_F32);
    case JOP_CONST_F64:
      if(ins->imm >= m->nconst_f64) return err(JELLO_BC_BAD_FORMAT, "const_f64 index out of range", 0);
      return validate_const(m, reg_types, ins, nregs, JELLO_T_F64);
    case JOP_CONST_BOOL:
      if(ins->imm > 1) return err(JELLO_BC_BAD_FORMAT, "const_bool imm must be 0/1", 0);
      return validate_const(m, reg_types, ins, nregs, JELLO_T_BOOL);

    case JOP_ADD_I32:
    case JOP_SUB_I32:
    case JOP_MUL_I32:
    case JOP_SHL_I32:
    case JOP_SHR_I32:
    case JOP_BITAND_I32:
    case JOP_BITOR_I32:
    case JOP_BITXOR_I32:
      return validate_bin(m, reg_types, ins, nregs, JELLO_T_I32, 1);
    case JOP_DIV_I32:
    case JOP_MOD_I32:
      return validate_bin(m, reg_types, ins, nregs, JELLO_T_I32, 1);
    case JOP_ADD_I32_IMM:
    case JOP_SUB_I32_IMM:
    case JOP_MUL_I32_IMM:
      return validate_bin_imm(m, reg_types, ins, nregs, JELLO_T_I32);
    case JOP_NEG_I32:
      return validate_un(m, reg_types, ins, nregs, JELLO_T_I32, 1);

    case JOP_ADD_I64:
    case JOP_SUB_I64:
    case JOP_MUL_I64:
    case JOP_DIV_I64:
    case JOP_MOD_I64:
    case JOP_SHL_I64:
    case JOP_SHR_I64:
    case JOP_BITAND_I64:
    case JOP_BITOR_I64:
    case JOP_BITXOR_I64:
      return validate_bin(m, reg_types, ins, nregs, JELLO_T_I64, 0);
    case JOP_NEG_I64:
      return validate_un(m, reg_types, ins, nregs, JELLO_T_I64, 0);

    case JOP_ADD_F16:
    case JOP_SUB_F16:
    case JOP_MUL_F16:
      return validate_bin(m, reg_types, ins, nregs, JELLO_T_F16, 0);
    case JOP_ADD_F32:
    case JOP_SUB_F32:
    case JOP_MUL_F32:
    case JOP_DIV_F32:
      return validate_bin(m, reg_types, ins, nregs, JELLO_T_F32, 0);
    case JOP_NEG_F32:
      return validate_un(m, reg_types, ins, nregs, JELLO_T_F32, 0);
    case JOP_ADD_F64:
    case JOP_SUB_F64:
    case JOP_MUL_F64:
    case JOP_DIV_F64:
      return validate_bin(m, reg_types, ins, nregs, JELLO_T_F64, 0);
    case JOP_NEG_F64:
      return validate_un(m, reg_types, ins, nregs, JELLO_T_F64, 0);
    case JOP_NOT_BOOL:
      return validate_un(m, reg_types, ins, nregs, JELLO_T_BOOL, 0);

    case JOP_EQ_I32:
    case JOP_LT_I32:
      return validate_cmp(m, reg_types, ins, nregs, JELLO_T_I32);
    case JOP_EQ_I32_IMM:
    case JOP_LT_I32_IMM:
      return validate_cmp_imm(m, reg_types, ins, nregs, JELLO_T_I32);
    case JOP_EQ_I64:
    case JOP_LT_I64:
      return validate_cmp(m, reg_types, ins, nregs, JELLO_T_I64);
    case JOP_EQ_F32:
    case JOP_LT_F32:
      return validate_cmp(m, reg_types, ins, nregs, JELLO_T_F32);
    case JOP_EQ_F64:
    case JOP_LT_F64:
      return validate_cmp(m, reg_types, ins, nregs, JELLO_T_F64);

    case JOP_SEXT_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I64, JELLO_T_I32);
    case JOP_SEXT_I16: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_I16);
    case JOP_TRUNC_I8: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I8, JELLO_T_I32);
    case JOP_TRUNC_I16: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I16, JELLO_T_I32);
    case JOP_I32_FROM_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_I64);
    case JOP_F64_FROM_I32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F64, JELLO_T_I32);
    case JOP_I32_FROM_F64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_F64);
    case JOP_F64_FROM_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F64, JELLO_T_I64);
    case JOP_I64_FROM_F64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I64, JELLO_T_F64);
    case JOP_F32_FROM_I32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F32, JELLO_T_I32);
    case JOP_I32_FROM_F32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_F32);
    case JOP_F64_FROM_F32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F64, JELLO_T_F32);
    case JOP_F32_FROM_F64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F32, JELLO_T_F64);
    case JOP_F32_FROM_I64: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F32, JELLO_T_I64);
    case JOP_I64_FROM_F32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I64, JELLO_T_F32);
    case JOP_F16_FROM_F32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F16, JELLO_T_F32);
    case JOP_F32_FROM_F16: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F32, JELLO_T_F16);
    case JOP_F16_FROM_I32: return validate_conv(m, reg_types, ins, nregs, JELLO_T_F16, JELLO_T_I32);
    case JOP_I32_FROM_F16: return validate_conv(m, reg_types, ins, nregs, JELLO_T_I32, JELLO_T_F16);

    case JOP_STRUCT_GET:
    case JOP_STRUCT_SET: {
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "struct reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_STRUCT) return err(JELLO_BC_BAD_FORMAT, "struct op target must be Struct", 0);
      uint32_t sig_id = m->types[reg_types[ins->b]].as.strct.struct_sig_id;
      if(sig_id >= m->nstruct_sigs) return err(JELLO_BC_BAD_FORMAT, "struct sig id out of range", 0);
      const jello_struct_sig* sig = &m->struct_sigs[sig_id];
      if(ins->imm >= sig->nfields) return err(JELLO_BC_BAD_FORMAT, "struct field index out of range", 0);
      if((jello_op)op == JOP_STRUCT_SET && !sig->is_mutable) {
        return err(JELLO_BC_BAD_FORMAT, "struct_set on immutable struct", 0);
      }
      if(reg_types[ins->a] != sig->field_type[ins->imm]) {
        return err(JELLO_BC_BAD_FORMAT, "struct field type mismatch", 0);
      }
      return ok();
    }

    case JOP_BYTES_NEW:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes_new reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BYTES || reg_kind(m, reg_types, ins->b) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_new types required", 0);
      }
      return ok();
    case JOP_BYTES_LEN:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes_len reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I32 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_len types required", 0);
      }
      return ok();
    case JOP_BYTES_GET_U8:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes_get_u8 reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I32 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_get_u8 types required", 0);
      }
      return ok();
    case JOP_BYTES_SET_U8:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes_set_u8 reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I32 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_set_u8 types required", 0);
      }
      return ok();

    case JOP_BITS_WRITE:
    case JOP_BITS_READ: {
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "bits op reg out of range", 0);
      if(ins->c < 1 || ins->c > 32) return err(JELLO_BC_BAD_FORMAT, "bits op width must be 1-32", 0);
      uint32_t bytes_reg = ((jello_op)op == JOP_BITS_WRITE) ? ins->a : ins->b;
      if(reg_kind(m, reg_types, bytes_reg) != JELLO_T_BYTES) return err(JELLO_BC_BAD_FORMAT, "bits op Bytes operand wrong kind", 0);
      return ok();
    }

    default:
      return ok();
  }
}
