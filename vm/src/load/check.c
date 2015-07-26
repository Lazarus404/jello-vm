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
    case JOP_CONST_NULL:
      return validate_const(m, reg_types, ins, nregs, JELLO_T_DYNAMIC);
    case JOP_CONST_ATOM:
      if(ins->imm >= m->natoms) return err(JELLO_BC_BAD_FORMAT, "const_atom atom id out of range", 0);
      return validate_const(m, reg_types, ins, nregs, JELLO_T_ATOM);

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

    /* -- the Dynamic boundary -- */
    case JOP_TO_DYN:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "to_dyn reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_DYNAMIC) return err(JELLO_BC_BAD_FORMAT, "to_dyn dst must be Dynamic", 0);
      return ok();
    case JOP_FROM_DYN_I8:
    case JOP_FROM_DYN_I16:
    case JOP_FROM_DYN_I32:
    case JOP_FROM_DYN_I64:
    case JOP_FROM_DYN_F16:
    case JOP_FROM_DYN_F32:
    case JOP_FROM_DYN_F64:
    case JOP_FROM_DYN_BOOL:
    case JOP_FROM_DYN_ATOM: {
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "from_dyn reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_DYNAMIC) return err(JELLO_BC_BAD_FORMAT, "from_dyn src must be Dynamic", 0);
      jello_type_kind want;
      switch((jello_op)op) {
        case JOP_FROM_DYN_I8: want = JELLO_T_I8; break;
        case JOP_FROM_DYN_I16: want = JELLO_T_I16; break;
        case JOP_FROM_DYN_I32: want = JELLO_T_I32; break;
        case JOP_FROM_DYN_I64: want = JELLO_T_I64; break;
        case JOP_FROM_DYN_F16: want = JELLO_T_F16; break;
        case JOP_FROM_DYN_F32: want = JELLO_T_F32; break;
        case JOP_FROM_DYN_F64: want = JELLO_T_F64; break;
        case JOP_FROM_DYN_BOOL: want = JELLO_T_BOOL; break;
        default: want = JELLO_T_ATOM; break;
      }
      if(reg_kind(m, reg_types, ins->a) != want) return err(JELLO_BC_BAD_FORMAT, "from_dyn dst wrong kind", 0);
      return ok();
    }
    case JOP_FROM_DYN_PTR: {
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "from_dyn_ptr reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_DYNAMIC) return err(JELLO_BC_BAD_FORMAT, "from_dyn_ptr src must be Dynamic", 0);
      jello_type_kind dk = reg_kind(m, reg_types, ins->a);
      if(dk != JELLO_T_BYTES && dk != JELLO_T_STRUCT && dk != JELLO_T_OBJECT) {
        return err(JELLO_BC_BAD_FORMAT, "from_dyn_ptr dst must be a pointer kind", 0);
      }
      return ok();
    }
    case JOP_KINDOF:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "kindof reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I32) return err(JELLO_BC_BAD_FORMAT, "kindof dst must be I32", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_DYNAMIC) return err(JELLO_BC_BAD_FORMAT, "kindof src must be Dynamic", 0);
      return ok();
    case JOP_PHYSEQ:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "physeq reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL) return err(JELLO_BC_BAD_FORMAT, "physeq dst must be Bool", 0);
      return ok();

    /* -- Struct -- */
    case JOP_STRUCT_NEW:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "struct_new reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_STRUCT) return err(JELLO_BC_BAD_FORMAT, "struct_new dst must be Struct", 0);
      return ok();

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

    /* -- Object -- */
    case JOP_OBJ_NEW:
      if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_new reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_new dst must be Object", 0);
      return ok();
    case JOP_OBJ_HAS_ATOM:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_has_atom reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL) return err(JELLO_BC_BAD_FORMAT, "obj_has_atom dst must be Bool", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_has_atom src must be Object", 0);
      if(ins->imm >= m->natoms) return err(JELLO_BC_BAD_FORMAT, "obj_has_atom atom id out of range", 0);
      return ok();
    case JOP_OBJ_GET_ATOM:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_get_atom reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_get_atom src must be Object", 0);
      if(ins->imm >= m->natoms) return err(JELLO_BC_BAD_FORMAT, "obj_get_atom atom id out of range", 0);
      return ok();
    case JOP_OBJ_SET_ATOM:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_set_atom reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_set_atom dst must be Object", 0);
      if(ins->imm >= m->natoms) return err(JELLO_BC_BAD_FORMAT, "obj_set_atom atom id out of range", 0);
      return ok();
    case JOP_OBJ_GET:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_get reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_get src must be Object", 0);
      if(reg_kind(m, reg_types, ins->c) != JELLO_T_ATOM) return err(JELLO_BC_BAD_FORMAT, "obj_get key must be Atom", 0);
      return ok();
    case JOP_OBJ_SET:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_set reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_set dst must be Object", 0);
      if(reg_kind(m, reg_types, ins->c) != JELLO_T_ATOM) return err(JELLO_BC_BAD_FORMAT, "obj_set key must be Atom", 0);
      return ok();
    case JOP_OBJ_HAS:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_has reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL) return err(JELLO_BC_BAD_FORMAT, "obj_has dst must be Bool", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_has src must be Object", 0);
      if(reg_kind(m, reg_types, ins->c) != JELLO_T_ATOM) return err(JELLO_BC_BAD_FORMAT, "obj_has key must be Atom", 0);
      return ok();
    case JOP_OBJ_REMOVE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_remove reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL) return err(JELLO_BC_BAD_FORMAT, "obj_remove dst must be Bool", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_remove src must be Object", 0);
      if(reg_kind(m, reg_types, ins->c) != JELLO_T_ATOM) return err(JELLO_BC_BAD_FORMAT, "obj_remove key must be Atom", 0);
      return ok();
    case JOP_OBJ_CLEAR:
      if(ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_clear reg out of range", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_clear src must be Object", 0);
      return ok();
    case JOP_OBJ_COPY:
      if(ins->a >= nregs || ins->b >= nregs) return err(JELLO_BC_BAD_FORMAT, "obj_copy reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_copy dst must be Object", 0);
      if(reg_kind(m, reg_types, ins->b) != JELLO_T_OBJECT) return err(JELLO_BC_BAD_FORMAT, "obj_copy src must be Object", 0);
      return ok();
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

    case JOP_BYTES_CONCAT2:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes_concat2 reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BYTES || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_BYTES) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_concat2 types required", 0);
      }
      return ok();
    case JOP_BYTES_EQ:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes_eq reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BOOL || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_BYTES) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_eq types required", 0);
      }
      return ok();
    case JOP_BYTES_BITAND2:
    case JOP_BYTES_BITOR2:
    case JOP_BYTES_BITXOR2:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes bitwise2 reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_BYTES || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_BYTES) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_bitwise2 types required", 0);
      }
      return ok();
    case JOP_BYTES_READ_U16_LE:
    case JOP_BYTES_READ_U16_BE:
    case JOP_BYTES_READ_U32_LE:
    case JOP_BYTES_READ_U32_BE:
    case JOP_BYTES_READ_I32_LE:
    case JOP_BYTES_READ_I32_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes read reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I32 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_read types required", 0);
      }
      return ok();
    case JOP_BYTES_READ_F32_LE:
    case JOP_BYTES_READ_F32_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes read reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_F32 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_read_f32 types required", 0);
      }
      return ok();
    case JOP_BYTES_READ_I64_LE:
    case JOP_BYTES_READ_I64_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes read reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I64 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_read_i64 types required", 0);
      }
      return ok();
    case JOP_BYTES_READ_F64_LE:
    case JOP_BYTES_READ_F64_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes read reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_F64 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_read_f64 types required", 0);
      }
      return ok();
    case JOP_BYTES_WRITE_U16_LE:
    case JOP_BYTES_WRITE_U16_BE:
    case JOP_BYTES_WRITE_U32_LE:
    case JOP_BYTES_WRITE_U32_BE:
    case JOP_BYTES_WRITE_I32_LE:
    case JOP_BYTES_WRITE_I32_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes write reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I32 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_write types required", 0);
      }
      return ok();
    case JOP_BYTES_WRITE_F32_LE:
    case JOP_BYTES_WRITE_F32_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes write reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_F32 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_write_f32 types required", 0);
      }
      return ok();
    case JOP_BYTES_WRITE_I64_LE:
    case JOP_BYTES_WRITE_I64_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes write reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_I64 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_write_i64 types required", 0);
      }
      return ok();
    case JOP_BYTES_WRITE_F64_LE:
    case JOP_BYTES_WRITE_F64_BE:
      if(ins->a >= nregs || ins->b >= nregs || ins->c >= nregs) return err(JELLO_BC_BAD_FORMAT, "bytes write reg out of range", 0);
      if(reg_kind(m, reg_types, ins->a) != JELLO_T_F64 || reg_kind(m, reg_types, ins->b) != JELLO_T_BYTES ||
         reg_kind(m, reg_types, ins->c) != JELLO_T_I32) {
        return err(JELLO_BC_BAD_FORMAT, "bytes_write_f64 types required", 0);
      }
      return ok();

    /* -- native calls -- */
    case JOP_CALL_NATIVE: {
      if(ins->imm >= m->nimports) return err(JELLO_BC_BAD_FORMAT, "import index out of range", 0);
      const jello_import_sig* sig = &m->imports[ins->imm];
      if(ins->c != sig->arity) return err(JELLO_BC_BAD_FORMAT, "call_native arg count mismatch", 0);
      if((uint64_t)ins->b + (uint64_t)ins->c > (uint64_t)nregs) {
        return err(JELLO_BC_BAD_FORMAT, "call_native arg regs out of range", 0);
      }
      for(uint32_t i = 0; i < ins->c; i++) {
        if((uint8_t)reg_kind(m, reg_types, ins->b + i) != sig->arg_kinds[i]) {
          return err(JELLO_BC_BAD_FORMAT, "call_native arg type mismatch", 0);
        }
      }
      if((jello_type_kind)sig->ret_kind != JELLO_T_VOID) {
        if(ins->a >= nregs) return err(JELLO_BC_BAD_FORMAT, "call_native dest reg out of range", 0);
        if(reg_kind(m, reg_types, ins->a) != (jello_type_kind)sig->ret_kind) {
          return err(JELLO_BC_BAD_FORMAT, "call_native dest wrong kind", 0);
        }
      }
      return ok();
    }

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
