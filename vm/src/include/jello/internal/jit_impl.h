// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * JIT internals shared by every file under src/jit; the JIR, executable buffers, the register
 * allocator, call-survive planning, and the C helpers JIT code calls. */

#ifndef JELLO_JIT_IMPL_H
#define JELLO_JIT_IMPL_H

#include <jello/internal/bytecode.h>
#include <jello/internal/vm_internal.h>

typedef enum jello_jit_ir_op {
  JIR_NOP = 0,
  JIR_RET,
  JIR_JMP,
  JIR_JMP_IF,
  JIR_MOV,
  JIR_SAFEPOINT,
  JIR_SLOW, /* a/b/c/imm copied verbatim from the source instruction */

  /* -- calls -- */
  JIR_CALL_SELF,   /* self-recursive CALL: a=dst, b=first_arg, c=nargs */
  JIR_CALL_DIRECT, /* same-module CALL to a different function: a=dst, b=first_arg, c=nargs, imm=callee function index */
  JIR_CALL_NATIVE, /* a=dst, b=first_arg, c=nargs, imm=import index */
  JIR_CALLR,       /**< Call through a Function value; a=dst, b=callee, c=nargs, imm=first_arg. */

  /* -- typed constants -- */
  JIR_CONST_I8_IMM, /* a=dst, imm=literal value (masked to the dest's own narrow width at emit time, exactly as vm_store_u32_masked already does) */
  JIR_CONST_I32,    /* a=dst, imm=literal value */
  JIR_CONST_I64,    /* a=dst, imm=const_i64 pool index */
  JIR_CONST_F16,    /* a=dst, imm=raw 16-bit payload */
  JIR_CONST_F32,    /* a=dst, imm=raw bit pattern */
  JIR_CONST_F64,    /* a=dst, imm=const_f64 pool index */
  JIR_CONST_BOOL,   /* a=dst, imm=0/1 */
  JIR_CONST_NULL,   /* a=dst */
  JIR_CONST_ATOM,   /* a=dst, imm=atom id */
  JIR_TRY,    /**< a=payload register (Dynamic), imm=handler offset from bc_pc + 1, as for JMP. */
  JIR_ENDTRY, /**< Closes the innermost try region. */
  JIR_THROW,  /**< a=payload register (Dynamic); never falls through. */

  /* -- arithmetic -- */
  JIR_BIN_I32, /* a=dst, b=lhs, c=rhs, imm=jello_jir_bin */
  JIR_BIN_I64,
  JIR_BIN_F16,
  JIR_BIN_F32,
  JIR_BIN_F64,
  JIR_ADD_I32_IMM, /* a=dst, b=src, imm=literal -- kept 1:1, Sec 3.3: already carries its own literal, no second imm slot to also name an operator */
  JIR_SUB_I32_IMM,
  JIR_MUL_I32_IMM,
  JIR_NEG_I32, /* a=dst, b=src */
  JIR_NEG_I64,
  JIR_NEG_F32,
  JIR_NEG_F64,
  JIR_NOT_BOOL,

  /* -- comparisons -- */
  JIR_CMP_I32, /* a=dst Bool, b=lhs, c=rhs, imm=jello_jir_cmp */
  JIR_CMP_I64,
  JIR_CMP_F32,
  JIR_CMP_F64,
  JIR_EQ_I32_IMM, /* a=dst Bool, b=src, imm=literal -- kept 1:1, same reason as the _IMM arithmetic ops */
  JIR_LT_I32_IMM,

  /* -- conversions -- */
  JIR_CONV, /* a=dst, b=src, imm=jello_jir_conv */

  /* -- the Dynamic boundary -- */
  JIR_TO_DYN,   /* a=dst Dynamic, b=src */
  JIR_FROM_DYN, /* a=dst, b=src Dynamic, imm=target jello_type_kind (resolved at lowering time even for FROM_DYN_PTR, whose bytecode-level dest kind is POLY) */
  JIR_KINDOF,   /* a=dst I32, b=src Dynamic */
  JIR_PHYSEQ,   /* a=dst Bool, b=reg1, c=reg2 */
  JIR_IS_NULL,    /**< a=dst Bool, b=src Dynamic or reference. */
  JIR_NULL_CHECK, /**< a=src Dynamic or reference; traps NULL_DEREF when null. */
  JIR_IS_ENUM,    /**< a=dst Bool, b=src Dynamic, imm=enum type. */

  /* -- Bytes -- */
  JIR_BYTES_NEW,     /* a=dst, b=len */
  JIR_BYTES_LEN,     /* a=dst, b=bytes */
  JIR_BYTES_GET_U8,  /* a=dst, b=bytes, c=idx */
  JIR_BYTES_SET_U8,  /* a=val, b=bytes, c=idx */
  JIR_BYTES_EQ,      /* a=dst Bool, b=lhs, c=rhs */
  JIR_BYTES_CONCAT2, /* a=dst, b=lhs, c=rhs */
  JIR_BYTES_BITOP,   /* a=dst, b=lhs, c=rhs, imm=jello_jir_bin (AND/OR/XOR only) */
  JIR_BYTES_READ,    /* a=dst, b=bytes, c=offset, imm=jello_jir_bytes_rw */
  JIR_BYTES_WRITE,   /* a=val, b=bytes, c=offset, imm=jello_jir_bytes_rw */
  JIR_BYTES_BLIT,    /* a=dst, b=src, c=offset */
  JIR_BITS_WRITE,    /* a=bytes, b=val, c=width, imm=bit offset */
  JIR_BITS_READ,     /* a=dst, b=bytes, c=width, imm=bit offset */
  JIR_BYTES_FREEZE,  /**< a=bytes. */

  /* -- Struct -- */
  JIR_STRUCT_NEW, /* a=dst */
  JIR_STRUCT_GET, /* a=dst, b=struct, imm=field index */
  JIR_STRUCT_SET, /* a=val, b=struct, imm=field index */

  /* -- Object -- */
  JIR_OBJ_NEW,      /* a=dst */
  JIR_OBJ_HAS_ATOM, /* a=dst Bool, b=object, imm=atom id */
  JIR_OBJ_GET_ATOM, /* a=dst, b=object, imm=atom id */
  JIR_OBJ_SET_ATOM, /* a=val, b=object, imm=atom id */
  JIR_OBJ_GET,      /* a=dst, b=object, c=key atom reg */
  JIR_OBJ_SET,      /* a=val, b=object, c=key atom reg */
  JIR_OBJ_HAS,      /* a=dst Bool, b=object, c=key atom reg */
  JIR_OBJ_REMOVE,   /* a=dst Bool, b=object, c=key atom reg */
  JIR_OBJ_CLEAR,    /* b=object */
  JIR_OBJ_COPY,     /* a=dst, b=src */

  JIR_ARRAY_LEN, /**< a=dst I32, b=array. */
  JIR_ARRAY_GET, /**< a=dst, b=array, c=idx. */
  JIR_ARRAY_SET, /**< a=val, b=array, c=idx. */

  JIR_ENUM_TAG, /**< a=dst I32, b=enum. */
  JIR_ENUM_GET, /**< a=dst, b=enum, c=field index. */

  JIR_GLOBAL_GET, /**< a=dst, imm=global index. */
  JIR_GLOBAL_SET, /**< a=val, imm=global index. */
} jello_jit_ir_op;

typedef enum jello_jir_bin {
  JIR_BIN_ADD = 0,
  JIR_BIN_SUB,
  JIR_BIN_MUL,
  JIR_BIN_DIV,
  JIR_BIN_MOD,
  JIR_BIN_SHL,
  JIR_BIN_SHR,
  JIR_BIN_AND,
  JIR_BIN_OR,
  JIR_BIN_XOR,
} jello_jir_bin;

typedef enum jello_jir_cmp {
  JIR_CMP_EQ = 0,
  JIR_CMP_LT,
} jello_jir_cmp;

typedef enum jello_jir_conv {
  JIR_CONV_SEXT_I64 = 0,
  JIR_CONV_SEXT_I16,
  JIR_CONV_TRUNC_I8,
  JIR_CONV_TRUNC_I16,
  JIR_CONV_I32_FROM_I64,
  JIR_CONV_F64_FROM_I32,
  JIR_CONV_I32_FROM_F64,
  JIR_CONV_F64_FROM_I64,
  JIR_CONV_I64_FROM_F64,
  JIR_CONV_F32_FROM_I32,
  JIR_CONV_I32_FROM_F32,
  JIR_CONV_F64_FROM_F32,
  JIR_CONV_F32_FROM_F64,
  JIR_CONV_F32_FROM_I64,
  JIR_CONV_I64_FROM_F32,
  JIR_CONV_F16_FROM_F32,
  JIR_CONV_F32_FROM_F16,
  JIR_CONV_F16_FROM_I32,
  JIR_CONV_I32_FROM_F16,
} jello_jir_conv;

typedef enum jello_jir_bytes_rw {
  JIR_BRW_U16_LE = 0,
  JIR_BRW_U16_BE,
  JIR_BRW_U32_LE,
  JIR_BRW_U32_BE,
  JIR_BRW_I32_LE,
  JIR_BRW_I32_BE,
  JIR_BRW_F32_LE,
  JIR_BRW_F32_BE,
  JIR_BRW_I64_LE,
  JIR_BRW_I64_BE,
  JIR_BRW_F64_LE,
  JIR_BRW_F64_BE,
} jello_jir_bytes_rw;

typedef enum jello_jit_ir_flags {
  JELLO_JIR_FLAG_NONE = 0,
  JELLO_JIR_FLAG_NUMERIC_FAST = 1u << 0,
} jello_jit_ir_flags;

typedef struct jello_jit_ir_insn {
  uint16_t op; /* jello_jit_ir_op */
  uint8_t a, b, c;
  uint32_t imm;
  uint32_t bc_pc; /* the source bytecode instruction this IR instruction came from */
} jello_jit_ir_insn;

typedef struct jello_jit_ir_function {
  uint32_t nregs;  /* copied from the source jello_bc_function */
  uint32_t ninsns;
  jello_jit_ir_insn* insns;
  uint32_t nslow;  /* count of JIR_SLOW instructions -- Sec 3.5 */
  uint32_t flags;  /* jello_jit_ir_flags */
} jello_jit_ir_function;

#define JELLO_JIR_REG_A (1u << 0)
#define JELLO_JIR_REG_B (1u << 1)
#define JELLO_JIR_REG_C (1u << 2)

unsigned jello_jit_ir_op_reg_mask(jello_jit_ir_op op);
jello_jit_ir_function* jello_jit_ir_build(const jello_bc_module* m, uint32_t fi);
int jello_jit_ir_verify(const jello_jit_ir_function* f, const jello_bc_function* src);
void jello_jit_ir_free(jello_jit_ir_function* f);
/* -- the private JIT-to-JIT calling convention -- */
#if defined(__aarch64__)
typedef enum jit_arm64_reg {
  JIT_REG_OUT = 19,     /* x19 */
  JIT_REG_VM = 20,      /* x20 */
  JIT_REG_BASE = 21,    /* x21 */
  JIT_REG_SURVIVE = 28, /* x28 -- Chapter 10/12 */
} jit_arm64_reg;
#else
typedef enum jit_x64_reg {
  JIT_REG_OUT = 3,
  JIT_REG_VM = 12,   /* R12 */
  JIT_REG_BASE = 13, /* R13 */
  JIT_REG_SURVIVE = 15, /* R15 */
} jit_x64_reg;
#endif

typedef struct jit_exec_buf {
  uint8_t* mem;  /* NULL if allocation failed */
  size_t cap;
  size_t len;    /* bytes written so far -- grows as the emitter writes, capped at `cap` */
  int finalized; /* 1 once jit_finalize_exec_buf has mprotect'd this region RX */
  int overflowed; /**< Set when a write did not fit; the emitter declines the function. */
  size_t limit;   /**< Writes stop here; cap, or the end of an open append window. */
  size_t append_from, append_end; /**< The open append window's start and its pages' end. */
} jit_exec_buf;

jit_exec_buf jit_alloc_exec_buf(size_t cap);
int jit_exec_buf_write(jit_exec_buf* buf, const void* bytes, size_t n);
int jit_finalize_exec_buf(jit_exec_buf* buf);
int jit_exec_buf_begin_append(jit_exec_buf* buf, size_t bound);
int jit_exec_buf_end_append(jit_exec_buf* buf);
void jit_free_exec_buf(jit_exec_buf* buf);

enum {
  JIT_PREG_NONE = -1,
  JIT_PREG_XMM0 = 64,
};

typedef struct jit_reg_emit_ctx {
  jit_exec_buf* buf;
} jit_reg_emit_ctx;

typedef struct jit_reg_state {
  uint32_t nregs;
  int8_t* current;
  int16_t gpr_vreg[11];
  int16_t xmm_vreg[8];
  uint8_t gpr_pinned[11];
  uint8_t xmm_pinned[8];
  unsigned next_gpr_victim;
  unsigned next_xmm_victim;
  jit_reg_emit_ctx emit;
  int survive_held;
  uint8_t survive_vreg;
} jit_reg_state;

extern _Atomic uint32_t jit_reg_reload_count;
extern _Atomic uint32_t jit_reg_discard_count; /**< Calls to jit_reg_state_discard_currents. */
extern _Atomic uint32_t jit_call_survive_armed_count;
extern _Atomic uint32_t jit_call_survive_decline_count;
extern const uint8_t jit_gpr_pool[];
extern const uint32_t jit_gpr_pool_len;
extern const uint32_t jit_xmm_pool_len;

void jit_spill_load_gpr64(jit_exec_buf* buf, unsigned dst_gpr, uint8_t vreg);
void jit_spill_store_gpr32(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr);
void jit_spill_store_gpr64(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr);
void jit_spill_load_xmm32(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg);
void jit_spill_load_xmm64(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg);
void jit_spill_store_xmm32(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm);
void jit_spill_store_xmm64(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm);
void jit_reg_state_init(jit_reg_state* st, uint32_t nregs, jit_reg_emit_ctx emit);
void jit_reg_state_free(jit_reg_state* st);
void jit_reg_state_clobber_call(jit_reg_state* st);
void jit_reg_state_discard_currents(jit_reg_state* st);
int jit_reg_alloc_gpr(jit_reg_state* st, uint8_t vreg, int and_load);
int jit_reg_alloc_xmm(jit_reg_state* st, uint8_t vreg, int and_load, int wide);
int jit_reg_get_gpr(jit_reg_state* st, uint8_t vreg, int and_load);
int jit_reg_get_xmm(jit_reg_state* st, uint8_t vreg, int and_load, int wide);
void jit_reg_bind(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_unbind_vreg(jit_reg_state* st, uint8_t vreg);
void jit_reg_unbind_preg(jit_reg_state* st, int preg);
void jit_reg_store_home_gpr32(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_store_home_gpr64(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_store_home_xmm32(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_store_home_xmm64(jit_reg_state* st, uint8_t vreg, int preg);
void jit_reg_unpin_all(jit_reg_state* st);
void jit_reg_survive_hold(jit_reg_state* st, uint8_t vreg);
void jit_reg_survive_release(jit_reg_state* st);
int jit_reg_survive_is_held(const jit_reg_state* st);
uint8_t jit_reg_survive_held_vreg(const jit_reg_state* st);

typedef struct jit_survive_plan {
  uint8_t* armed;      /* [ninsns] -- 1 iff this call index holds `candidate` across its own call */
  uint8_t* candidate;  /* [ninsns] -- the vreg held; meaningful only where armed[i] */
  uint8_t* is_consumer;/* [ninsns] -- 1 iff this index is the recorded consumer for some earlier armed[] call: release JIT_REG_SURVIVE after emitting it */
  uint32_t nregs;      /* redundant with ir->nregs -- kept for jit_survive_plan_free's own bounds-free symmetry with build */
  int any_armed;
} jit_survive_plan;

int jit_survive_plan_build(const jello_jit_ir_function* ir, const jello_bc_module* m,
    const jello_bc_function* f, const uint8_t* is_target, jit_survive_plan* out_plan);
void jit_survive_plan_free(jit_survive_plan* plan);

JELLO_JIT_ABI void jit_trap_arithmetic(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_stack_overflow(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_null_deref(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_bounds(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_type_mismatch(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_limit(jello_vm* vm);
JELLO_JIT_ABI void jit_trap_pop_frame(jello_vm* vm);

JELLO_JIT_ABI jello_bytes* jit_bytes_bitop(jello_vm* vm, jello_bytes* b, jello_bytes* c, int op_kind);
JELLO_JIT_ABI jello_bytes* jit_bytes_concat2(jello_vm* vm, jello_bytes* b, jello_bytes* c);
JELLO_JIT_ABI void jit_bits_write(jello_bytes* b, uint32_t width, uint32_t bit_off, uint32_t value);
JELLO_JIT_ABI uint32_t jit_bits_read(const jello_bytes* b, uint32_t width, uint32_t bit_off);
JELLO_JIT_ABI int jit_run_handler(jello_vm* vm, const jello_bc_module* m, uint32_t fi, uint32_t pc);

enum {
  JIT_EMIT_BUF_MIN_CAP = 65536u,
  JIT_EMIT_PER_INSN_BUDGET = 128u,
  JIT_EMIT_CALL_INSN_BUDGET = 512u,
};

uint8_t* jit_frame_entry_base(jello_vm* vm);
int jello_jit_ir_slow_runs_handler(jello_op op);

#define JIT_TRAP_SITE_OFF                                                                          \
  ((uint32_t)(offsetof(jello_vm, trap_trace) + offsetof(jello_trap_trace, site)))

#ifdef JELLO_JIT_SYSV_THUNKS
#define JIT_C(fn) jit_sysv_##fn
JELLO_JIT_ABI void* jit_sysv_memcpy(void* dst, const void* src, size_t n);
JELLO_JIT_ABI void* jit_sysv_memmove(void* dst, const void* src, size_t n);
JELLO_JIT_ABI int jit_sysv_memcmp(const void* a, const void* b, size_t n);
JELLO_JIT_ABI jello_box_f64* jit_sysv_jello_box_f64_new(jello_vm* vm, double v);
JELLO_JIT_ABI jello_object* jit_sysv_jello_object_new(jello_vm* vm, uint32_t type_id);
JELLO_JIT_ABI jello_object* jit_sysv_jello_object_copy(jello_vm* vm, const jello_object* src);
JELLO_JIT_ABI int jit_sysv_jello_object_has(const jello_object* o, uint32_t atom_id);
JELLO_JIT_ABI jello_struct* jit_sysv_jello_struct_new(jello_vm* vm, const jello_bc_module* m,
                                                      uint32_t struct_sig_id);
JELLO_JIT_ABI jello_value jit_sysv_vm_box_from_typed(jello_vm* vm, const jello_bc_module* m,
                                                     const jello_bc_function* f, reg_frame* rf,
                                                     uint32_t r);
JELLO_JIT_ABI int jit_sysv_vm_store_from_boxed(jello_vm* vm, const jello_bc_module* m,
                                               const jello_bc_function* f, reg_frame* rf,
                                               uint32_t dst, jello_value v);
JELLO_JIT_ABI jello_bytes* jit_sysv_vm_buffer_new(jello_vm* vm, uint32_t length);
JELLO_JIT_ABI uint32_t jit_sysv_vm_dynamic_is_enum(jello_value v, jello_type_id type);
JELLO_JIT_ABI uint32_t jit_sysv_vm_kindof_dynamic(jello_value v);
JELLO_JIT_ABI float jit_sysv_vm_f16_to_f32(uint16_t bits);
JELLO_JIT_ABI uint16_t jit_sysv_vm_f32_to_f16(float f);
JELLO_JIT_ABI void jit_sysv_vm_pop_frame(jello_vm* vm);
JELLO_JIT_ABI int jit_sysv_vm_try_push(jello_vm* vm, uint32_t catch_pc, uint32_t payload_reg);
JELLO_JIT_ABI void jit_sysv_vm_try_pop(jello_vm* vm);
JELLO_JIT_ABI void jit_sysv_vm_safepoint_check(jello_vm* vm);
JELLO_JIT_ABI int jit_sysv_vm_reductions_out(jello_vm* vm);
#else
#define JIT_C(fn) fn
#endif

#endif /* JELLO_JIT_IMPL_H */
