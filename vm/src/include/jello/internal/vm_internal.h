// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_VM_INTERNAL_H
#define JELLO_VM_INTERNAL_H

#include <jello.h>
#include <jello/internal/bytecode.h>
#include <string.h>

/* Internal VM layouts, frames, dispatch, GC fields. */

/* -- the concrete module -- */
struct jello_bc_module {
  uint32_t version;
  uint32_t features;
  uint32_t ntypes;
  const jello_type_entry* types;
  uint32_t nsigs;
  const jello_fun_sig* sigs;
  uint32_t nstruct_sigs;
  const jello_struct_sig* struct_sigs;
  uint32_t natoms;
  const char* atoms_data;
  const char* const* atoms;
  uint32_t nconst_i64;
  const int64_t* const_i64;
  uint32_t nconst_f64;
  const double* const_f64;
  uint32_t nconst_bytes;
  const uint32_t* const_bytes_len;
  const uint32_t* const_bytes_off;
  const uint8_t* const_bytes_data;
  uint32_t nfuncs;
  const jello_bc_function* funcs;
  uint32_t entry;
};

/* -- semantic validation (check.c) -- */
jello_bc_result jello_bc_validate_insn(
    const jello_bc_module* m,
    const jello_type_id* reg_types,
    const jello_insn* ins,
    uint32_t nregs,
    uint32_t pc,
    uint32_t ninsns,
    uint32_t nfuncs);

/* -- interpreter core -- */
typedef struct reg_frame {
  uint8_t* mem;
  const uint32_t* off;
  uint32_t size;
} reg_frame;

typedef struct call_frame {
  const jello_bc_function* f;
  uint32_t func_index;
  uint32_t pc;
  reg_frame rf;
  uint8_t has_caller;
  uint8_t has_pointer;
  uint32_t caller_dst; /* register in the caller's frame that RET writes into */
} call_frame;

typedef struct frame_layout {
  const jello_bc_function* f; /* cache key */
  uint32_t* off;
  uint32_t total; /* 8-byte aligned, so the next bump-allocated frame stays aligned too */
  int has_pointer;
} frame_layout;

/* -- GC v1 -- */
typedef struct jello_gc_hdr {
  struct jello_gc_hdr* next; /* live-object list, or free-list, depending on state */
  uint32_t total_size;       /* header + payload, rounded up to this object's size class */
  uint8_t marked;
} jello_gc_hdr;

#define GC_NUM_SIZE_CLASSES 8
#define GC_MAX_SIZE_CLASS_BYTES 4096u

struct jello_vm {
  uint8_t* frame_stack;
  uint32_t frame_stack_top;
  uint32_t frame_stack_cap;

  call_frame* call_frames;
  uint32_t call_frames_len;
  uint32_t call_frames_cap;

  frame_layout* frame_layouts;
  uint32_t frame_layouts_len;
  uint32_t frame_layouts_cap;
  const jello_bc_module* frame_layouts_mod; /* cache invalidates when the module changes */

  jello_trap_code trap_code;
  const jello_bc_module* running_module;

  /* GC v1 */
  jello_gc_hdr* gc_objects; /* every live object, singly linked */
  jello_gc_hdr* gc_free_classes[GC_NUM_SIZE_CLASSES]; /* one free-list head per size class */
  uint32_t gc_bytes_live;
  uint32_t gc_next_collect;
  uint32_t gc_collections;
  uint32_t gc_freed_objects;
  void** gc_roots;
  uint32_t gc_roots_len;
  uint32_t gc_roots_cap;
  double gc_pause_ms; /* cumulative wall-clock time spent inside jello_gc_collect (Sec 2.10) */
  int gc_blocking_depth;
};

/* -- register load/store: static inline for the hot dispatch path -- */
static inline uint8_t* vm_reg_ptr(reg_frame* rf, uint32_t r) {
  return rf->mem + rf->off[r];
}
static inline uint32_t vm_load_u32(reg_frame* rf, uint32_t r) {
  return *(const uint32_t*)vm_reg_ptr(rf, r);
}
static inline void vm_store_u32(reg_frame* rf, uint32_t r, uint32_t v) {
  *(uint32_t*)vm_reg_ptr(rf, r) = v;
}
static inline int64_t vm_load_i64(reg_frame* rf, uint32_t r) {
  return *(const int64_t*)vm_reg_ptr(rf, r);
}
static inline void vm_store_i64(reg_frame* rf, uint32_t r, int64_t v) {
  *(int64_t*)vm_reg_ptr(rf, r) = v;
}
static inline float vm_load_f32(reg_frame* rf, uint32_t r) {
  return *(const float*)vm_reg_ptr(rf, r);
}
static inline void vm_store_f32(reg_frame* rf, uint32_t r, float v) {
  *(float*)vm_reg_ptr(rf, r) = v;
}
static inline double vm_load_f64(reg_frame* rf, uint32_t r) {
  return *(const double*)vm_reg_ptr(rf, r);
}
static inline void vm_store_f64(reg_frame* rf, uint32_t r, double v) {
  *(double*)vm_reg_ptr(rf, r) = v;
}
static inline void* vm_load_ptr(reg_frame* rf, uint32_t r) {
  void* p;
  memcpy(&p, vm_reg_ptr(rf, r), sizeof(void*));
  return p;
}
static inline void vm_store_ptr(reg_frame* rf, uint32_t r, void* p) {
  memcpy(vm_reg_ptr(rf, r), &p, sizeof(void*));
}
static inline jello_value vm_load_val(reg_frame* rf, uint32_t r) {
  jello_value v;
  memcpy(&v, vm_reg_ptr(rf, r), sizeof(jello_value));
  return v;
}
static inline void vm_store_val(reg_frame* rf, uint32_t r, jello_value v) {
  memcpy(vm_reg_ptr(rf, r), &v, sizeof(jello_value));
}
static inline float vm_f32_from_bits(uint32_t bits) {
  float f;
  memcpy(&f, &bits, sizeof f);
  return f;
}
static inline void vm_store_u32_masked(reg_frame* rf, uint32_t r, uint32_t v, jello_type_kind k) {
  uint32_t masked = v;
  if(k == JELLO_T_I8) masked = v & 0xFFu;
  else if(k == JELLO_T_I16) masked = v & 0xFFFFu;
  vm_store_u32(rf, r, masked);
}

jello_type_kind vm_reg_kind(const jello_bc_module* m, const jello_bc_function* f, uint32_t r);

/* -- frame stack + per-function layout cache (frame.c) -- */
int vm_push_frame(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                   uint32_t caller_dst, uint32_t arg_first, uint32_t arg_count, uint8_t has_caller);
void vm_pop_frame(jello_vm* vm);
void vm_frame_reset(jello_vm* vm);

/* -- dispatch (dispatch.c) + main loop (exec_loop.c) -- */
typedef enum op_result {
  OP_CONTINUE,
  OP_RETURN,
  OP_TRAP,
} op_result;

typedef struct exec_ctx {
  jello_vm* vm;
  const jello_bc_module* m;
  jello_numeric_result* out;
} exec_ctx;

op_result op_dispatch(exec_ctx* ctx, const jello_insn* ins);
jello_exec_status vm_exec_loop(exec_ctx* ctx);

/* -- cold-path opcode handlers (ops_arith.c) -- */
op_result op_div_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_mod_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_shl_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_shr_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitand_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitor_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitxor_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_i32(exec_ctx* ctx, const jello_insn* ins);

op_result op_add_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_sub_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_mul_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_div_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_mod_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_shl_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_shr_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitand_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitor_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitxor_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_eq_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_i64(exec_ctx* ctx, const jello_insn* ins);

op_result op_add_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_sub_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_mul_f16(exec_ctx* ctx, const jello_insn* ins);

op_result op_add_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_sub_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_mul_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_div_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_eq_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_f32(exec_ctx* ctx, const jello_insn* ins);

op_result op_sext_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_sext_i16(exec_ctx* ctx, const jello_insn* ins);
op_result op_trunc_i8(exec_ctx* ctx, const jello_insn* ins);
op_result op_trunc_i16(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_f64_from_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_f64_from_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_i64_from_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_f32_from_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_f64_from_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_f32_from_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_f32_from_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_i64_from_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_f16_from_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_f32_from_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_f16_from_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_f16(exec_ctx* ctx, const jello_insn* ins);

/* -- Bytes opcode handlers (ops_bytes.c) -- */
op_result op_bytes_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_len(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_get_u8(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_set_u8(exec_ctx* ctx, const jello_insn* ins);

/* -- the boxing boundary (value/value.c) -- */
jello_value vm_box_from_typed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                               reg_frame* rf, uint32_t r);
int vm_store_from_boxed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                         reg_frame* rf, uint32_t dst, jello_value v);
uint32_t vm_kindof_dynamic(jello_value v);

/* -- Dynamic boundary opcode handlers (ops_dyn.c) -- */
op_result op_to_dyn(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_i8(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_i16(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_i32(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_bool(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_from_dyn_ptr(exec_ctx* ctx, const jello_insn* ins);
op_result op_kindof(exec_ctx* ctx, const jello_insn* ins);
op_result op_physeq(exec_ctx* ctx, const jello_insn* ins);
op_result op_const_null(exec_ctx* ctx, const jello_insn* ins);
op_result op_const_atom(exec_ctx* ctx, const jello_insn* ins);

/* -- Struct opcode handlers (ops_struct.c) -- */
op_result op_struct_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_struct_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_struct_set(exec_ctx* ctx, const jello_insn* ins);

#endif /* JELLO_VM_INTERNAL_H */
