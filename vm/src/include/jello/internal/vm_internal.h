// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_VM_INTERNAL_H
#define JELLO_VM_INTERNAL_H

#include <jello.h>
#include <jello/internal/bytecode.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* Internal VM layouts, frames, dispatch, GC fields. */

/* -- native imports -- */
typedef struct jello_import_sig {
  uint32_t library_atom;
  uint32_t export_atom;
  uint8_t ret_kind;
  uint8_t arity;
  uint8_t arg_kinds[8];
  const struct jello_jdll_export* resolved;
} jello_import_sig;

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
  uint32_t nenum_sigs;
  const jello_enum_sig* enum_sigs;
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
  struct jello_bytes* const* const_bytes_values;
  uint32_t nglobals;
  const jello_type_id* global_types;
  uint64_t* globals;
  const uint32_t* ptr_globals; /**< The globals whose kind may hold a pointer, by index. */
  uint32_t nptr_globals;
  uint32_t nfuncs;
  const jello_bc_function* funcs;
  _Atomic(struct jello_function*)* const_funs;
  _Atomic(struct jello_enum*)* const_enums;
  uint32_t nconst_enums;
  const uint32_t* const_enum_base; /**< By type; meaningful for an enum type only. */
  uint32_t entry;
  uint8_t proto_enabled;
  uint32_t nimports;
  const jello_import_sig* imports;
  uint32_t nsource_files;
  const char* const* source_files; /**< Line-table source paths (JELLO_BC_FEAT_LINE_TABLE). */
  void* jdll_handles;
  uint32_t njdll_handles;
};

#define JELLO_ATOM___PROTO__ 0u

enum {
  JELLO_OBJ_SLOT_EMPTY = 0,
  JELLO_OBJ_SLOT_OCCUPIED = 1,
  JELLO_OBJ_SLOT_TOMB = 2,
  JELLO_OBJ_SLOT_OWNED = 3,
  JELLO_OBJ_SLOT_SEALED = 5, /**< Holds a method that may not change (jello_object_seal). */
};

static inline int vm_obj_slot_live(uint8_t state) { return state & 1u; }
static inline uint32_t vm_atom_hash(uint32_t atom_id) {
  uint32_t x = atom_id;
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
#define JELLO_OBJECT_INIT_CAP 8u
#define JELLO_OBJECT_LOAD_NUM 10u
#define JELLO_OBJECT_LOAD_DEN 7u

/* -- semantic validation (check.c) -- */
jello_bc_result jello_bc_validate_insn(
    const jello_bc_module* m,
    const jello_type_id* reg_types,
    const jello_insn* ins,
    uint32_t nregs,
    uint32_t pc,
    uint32_t ninsns,
    uint32_t nfuncs);
jello_bc_result jello_bc_validate_try_regions(const jello_bc_module* m);

/* -- interpreter core -- */
#define REG_SLOT_BYTES 8u

typedef struct reg_frame {
  uint8_t* mem;
  const uint8_t* kind;
  const uint8_t* wide;
  uint32_t size;
} reg_frame;

static inline uint8_t* vm_reg_ptr(reg_frame* rf, uint32_t r) {
  return rf->mem + (size_t)r * REG_SLOT_BYTES;
}

static inline jello_type_kind vm_reg_kind_fast(const reg_frame* rf, uint32_t r) {
  return (jello_type_kind)rf->kind[r];
}
static inline int vm_reg_is_wide(const reg_frame* rf, uint32_t r) {
  return rf->wide[r] != 0;
}

static inline int vm_kind_is_gc_pointer(jello_type_kind k) {
  switch(k) {
    case JELLO_T_BYTES:
    case JELLO_T_LIST:
    case JELLO_T_ARRAY:
    case JELLO_T_STRUCT:
    case JELLO_T_OBJECT:
    case JELLO_T_FUNCTION:
    case JELLO_T_ABSTRACT:
    case JELLO_T_ENUM: return 1;
    default: return 0;
  }
}

static inline int vm_kind_needs_gc_scan(jello_type_kind k) {
  return k == JELLO_T_DYNAMIC || vm_kind_is_gc_pointer(k);
}

typedef struct jello_handle {
  jello_obj_header h;
  const struct jello_jdll_handle_class* cls;
  void* payload;
  uint32_t native_bytes;
} jello_handle;

jello_bytes* jello_bytes_join(jello_vm* vm, jello_bytes* const* parts, uint32_t n,
                              const jello_bytes* separator);

#define JELLO_TRY_STACK_MAX 16u
#define JELLO_SPILL_SLOTS_MAX 65536u

typedef struct try_scope {
  uint32_t catch_pc;
  uint32_t payload_reg;
} try_scope;

typedef struct call_frame {
  const jello_bc_function* f;
  uint32_t func_index;
  uint32_t pc;
  reg_frame rf;
  uint8_t has_caller;
  uint8_t has_pointer;
  uint32_t caller_dst; /* register in the caller's frame that RET writes into */
  try_scope try_stack[JELLO_TRY_STACK_MAX];
  uint32_t try_stack_len;
  jello_value* spill_slots;
  uint32_t spill_cap;
} call_frame;

typedef struct frame_segment {
  uint8_t* mem;
  uint32_t bytes;              /**< Reserved. */
  uint32_t limit;              /**< Committed. */
  uint32_t top;                /**< In use. */
  struct frame_segment* below; /**< The segment it was entered from; NULL for the first. */
  struct frame_segment* above; /**< The segment mapped above it, kept once left; or NULL. */
} frame_segment;

typedef struct frame_layout {
  const jello_bc_function* f; /* cache key */
  uint8_t* kind; /* fl->kind[r] == vm_reg_kind(m, f, r), precomputed once per function */
  uint8_t* wide;
  uint8_t* init;
  uint32_t total;
  int has_pointer;
} frame_layout;

/* -- GC -- */
typedef struct jello_gc_hdr {
  struct jello_gc_hdr* next; /* live-object list, or free-list, depending on state */
  uint32_t total_size;       /* header + payload, rounded up to this object's size class */
  uint8_t marked;
} jello_gc_hdr;

#define GC_IMMORTAL 2u
#define GC_SHARED 3u

#define VM_BYTES_SHARED_MIN 64u

#define GC_NUM_SIZE_CLASSES 8
#define GC_MAX_SIZE_CLASS_BYTES 4096u

typedef struct jello_space {
  jello_gc_hdr* objects; /**< Every live object, singly linked. */
  jello_gc_hdr* free_classes[GC_NUM_SIZE_CLASSES];
  uint64_t bytes_live;
  uint64_t next_collect; /**< Allocation collects once bytes_live reaches this. */
  uint32_t collections;
  uint32_t finalized_handles;
  double pause_ms; /**< Cumulative wall-clock time inside jello_gc_collect. */
} jello_space;

/* -- the shared heap -- */
typedef struct jello_heap {
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
  jello_space space;

  /* -- stop-the-world -- */
  pthread_mutex_t alloc_lock;
  atomic_int stop_requested;
  pthread_mutex_t stw_lock;
  pthread_cond_t stw_resume_cv;
  pthread_mutex_t proc_lock;    /**< Guards the schedulers of processes on the heap. */
  _Atomic uint32_t collections; /**< Of every space on it. */

  struct jello_vm** threads;
  uint32_t threads_len;
  uint32_t threads_cap;
  uint32_t refcount; /* freed once the last jello_vm registered against this heap is destroyed */

  int32_t program_argc; /**< From jello_vm_set_program_args. */
  const char* const* program_argv;
} jello_heap;

typedef struct jello_trap_trace {
  uint32_t len;
  uint32_t site;
  uint32_t func_index[JELLO_TRAP_CALLERS_MAX];
  uint32_t pc[JELLO_TRAP_CALLERS_MAX];
  uint8_t repeated[JELLO_TRAP_CALLERS_MAX]; /**< Whether the site was passed more than once. */
  uint8_t full; /**< A site was dropped for want of room, so no later one is recorded. */
} jello_trap_trace;

struct jello_vm {
  jello_heap* heap;
  jello_space* space; /**< Where it allocates, and what its collections sweep: its heap's, or a
                       * process's own. */

  uint8_t* frame_stack;
  uint32_t frame_stack_top;
  uint32_t frame_stack_cap;
  uint32_t frame_stack_limit;  /**< The bytes of frame_stack committed; a push past them grows
                                * them, through jit_try_grow_push. */
  uint32_t frame_stack_max;    /**< Bytes of frame_stack; a push past them enters a segment above,
                                * through jit_try_grow_push. */
  frame_segment* segment;      /**< The current segment; first_segment, or one mapped above it. */
  frame_segment first_segment; /**< The frame stack it was attached to. */
  uint32_t segment_bytes;      /**< The bytes of every segment mapped above the first. */
  uint32_t call_frames_limit;  /**< Most call frames at once; one more traps STACK_OVERFLOW. */

  call_frame* call_frames;
  uint32_t call_frames_len;
  uint32_t call_frames_cap;
  frame_layout* frame_layouts;
  uint32_t frame_layouts_len;
  uint32_t frame_layouts_cap;

  jello_trap_code trap_code;
  uint32_t trap_pc;
  uint32_t trap_func_index;
  jello_value thrown;    /**< What an uncaught throw threw; see jello_vm_last_thrown. */
  char trap_detail[160];
  const jello_bc_module* running_module;
  uint64_t* globals;
  uint8_t owns_globals;
  uint8_t keep_processes;  /**< Set by jello_vm_keep_processes. */
  void* gc_transient_root; /**< An object still being populated, held as a root. */
  struct vm_parcel* parcel;

  struct jello_jit_module* active_jit;
  struct jello_jit_module* kept_jit;
  uintptr_t jit_stack_floor;
  uintptr_t native_stack_limit;
  try_scope* try_scopes;
  uint32_t try_scopes_cap;
  uint32_t live_try_scopes; /**< Try scopes on every frame; zero means a throw is uncaught. */
  uint32_t unwind_len;
  uint64_t jit_enters;      /**< For --jit-report. */
  uint64_t jit_bridges;     /**< For --jit-report. */

  int32_t reductions;        /**< Charges left in the window, 0 when made; the last one ends it. */
  uint32_t reduction_window; /**< The charges the current window started with. */
  uint64_t fuel_limit;
  uint64_t fuel_remaining;
  uint32_t max_bytes_len;
  
  /* -- quiescent tracking -- */
  atomic_int quiescent_depth;
  atomic_int in_exec;

  frame_layout** jit_layout_by_func;
  uint32_t jit_layout_by_func_len;
  uint8_t layouts_shared;      /**< The layouts are another VM's, from vm_share_layouts. */
  struct jello_process* proc;  /**< The process it runs as, once its scheduler has started. */
  jello_trap_trace trap_trace; /**< How the last trap was reached; last, being cold. */
};

#define JIT_FRAME_STACK_MAX (64u * 1024u * 1024u)
#define JIT_CALL_FRAMES_MAX 200000u

static inline jello_type_kind vm_global_kind(const jello_bc_module* m, uint32_t g) {
  return m->types[m->global_types[g]].kind;
}

static inline void* vm_global_ptr(const jello_bc_module* m, const uint64_t* globals, uint32_t g) {
  if(vm_global_kind(m, g) == JELLO_T_DYNAMIC) {
    jello_value v;
    memcpy(&v, &globals[g], sizeof v);
    return jello_is_ptr(v) ? jello_as_ptr(v) : NULL;
  }
  void* p;
  memcpy(&p, &globals[g], sizeof p);
  return p;
}

static inline void vm_enter_module(jello_vm* vm, const jello_bc_module* m) {
  vm->running_module = m;
  vm->globals = m->globals;
}

#define JIT_NATIVE_RESERVE (64u * 1024u)
#define VM_DEFAULT_FRAME_STACK_BYTES (64u * 1024u * 1024u)
#define VM_DEFAULT_CALL_FRAMES 1000000u
#define VM_MAX_SEGMENT_BYTES (1024u * 1024u * 1024u)
#define VM_FIRST_CALL_FRAMES 4u

#if defined(_WIN32) && defined(__x86_64__)
#else
#define JELLO_JIT_ABI
#endif

void vm_warm_jit_layouts(jello_vm* vm, const jello_bc_module* m);
int jit_try_grow_call_frames(jello_vm* vm);
uint32_t jit_layout_total_or_zero(jello_vm* vm, uint32_t fi);
frame_layout* jit_layout_ptr_or_null(jello_vm* vm, uint32_t fi);
int jit_call_push_numeric(jello_vm* vm, uint32_t callee_fi, uint32_t caller_dst,
                          uint32_t arg_first, uint32_t nargs);
int jit_try_grow_frame_stack(jello_vm* vm, uint32_t need);

uint8_t* jit_current_frame_mem(jello_vm* vm);

/* -- register load/store: static inline for the hot dispatch path -- */
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

static inline int vm_kind_immediate_tag(jello_type_kind k) {
  switch(k) {
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32: return (int)JELLO_TAG_I32;
    case JELLO_T_BOOL: return (int)JELLO_TAG_BOOL;
    case JELLO_T_ATOM: return (int)JELLO_TAG_ATOM;
    default: return -1;
  }
}

static inline uint32_t vm_kind_immediate_bits(jello_type_kind k) {
  switch(k) {
    case JELLO_T_I8: return 8u;
    case JELLO_T_I16: return 16u;
    case JELLO_T_BOOL: return 1u;
    default: return 32u;
  }
}

float vm_f16_to_f32(uint16_t bits);
uint16_t vm_f32_to_f16(float f);

#define VM_I32_FROM_FLOAT_LO (-2147483649.0)
#define VM_I32_FROM_FLOAT_HI 2147483648.0
#define VM_I64_FROM_FLOAT_LO (-9223372036854777856.0)
#define VM_I64_FROM_FLOAT_HI 9223372036854775808.0

jello_type_kind vm_reg_kind(const jello_bc_module* m, const jello_bc_function* f, uint32_t r);
int vm_func_has_pointer(const jello_bc_module* m, const jello_bc_function* f);

/* -- frame stack + per-function layout cache (frame.c) -- */
int vm_push_frame(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                  uint32_t caller_dst, uint32_t arg_first, uint32_t arg_count, uint8_t has_caller);
int vm_push_frame_raw(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                      uint32_t caller_dst, uint8_t has_caller,
                      const uint64_t* argbuf, uint32_t arg_count,
                      const uint64_t* capbuf, uint32_t ncaps);
void vm_copy_captures_into_top(jello_vm* vm, const jello_function* fn);
const jello_function* vm_callee_function(jello_vm* vm, jello_value v);
const jello_function* vm_push_callr_frame(jello_vm* vm, const jello_bc_module* m, jello_value v,
                                          uint32_t caller_dst, uint32_t arg_first,
                                          uint32_t arg_count);
void vm_pop_frame(jello_vm* vm);
void vm_frame_unwind(jello_vm* vm);
void vm_frame_reset(jello_vm* vm);
void vm_free_layouts(jello_vm* vm);

/* -- dispatch (dispatch.c) + main loop (exec_loop.c) -- */
typedef enum op_result {
  OP_CONTINUE,
  OP_RETURN,
  OP_TRAP,
  OP_TRAP_LOCATED, /**< Trapped, and vm->trap_func_index and vm->trap_pc are already set. */
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
op_result op_div_u32(exec_ctx* ctx, const jello_insn* ins);
op_result op_mod_u32(exec_ctx* ctx, const jello_insn* ins);
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
op_result op_div_u64(exec_ctx* ctx, const jello_insn* ins);
op_result op_mod_u64(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_shl_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_shr_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitand_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitor_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_bitxor_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_eq_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_u32(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_u64(exec_ctx* ctx, const jello_insn* ins);

op_result op_add_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_sub_f16(exec_ctx* ctx, const jello_insn* ins);
op_result op_mul_f16(exec_ctx* ctx, const jello_insn* ins);

op_result op_add_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_sub_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_mul_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_div_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_neg_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_eq_f32(exec_ctx* ctx, const jello_insn* ins);
op_result op_lt_f32(exec_ctx* ctx, const jello_insn* ins);

op_result op_sext_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_zext_i64(exec_ctx* ctx, const jello_insn* ins);
op_result op_fma_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_ldexp_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_copysign_f64(exec_ctx* ctx, const jello_insn* ins);
op_result op_sext_i16(exec_ctx* ctx, const jello_insn* ins);
op_result op_trunc_i8(exec_ctx* ctx, const jello_insn* ins);
op_result op_trunc_i16(exec_ctx* ctx, const jello_insn* ins);
op_result op_i32_from_i64(exec_ctx* ctx, const jello_insn* ins);
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
op_result op_const_bytes(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_len(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_get_u8(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_set_u8(exec_ctx* ctx, const jello_insn* ins);

/* -- Bytes memory/layout opcode handlers (ops_bytes.c) -- */
op_result op_bytes_concat2(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_eq(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_bitand2(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_bitor2(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_bitxor2(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_u16_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_u16_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_u32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_u32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_i32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_i32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_f32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_f32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_i64_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_i64_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_f64_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_read_f64_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_u16_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_u16_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_u32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_u32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_i32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_i32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_f32_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_f32_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_i64_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_i64_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_f64_le(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_write_f64_be(exec_ctx* ctx, const jello_insn* ins);
op_result op_bits_write(exec_ctx* ctx, const jello_insn* ins);
op_result op_bits_read(exec_ctx* ctx, const jello_insn* ins);
op_result op_bytes_blit(exec_ctx* ctx, const jello_insn* ins);

/* -- Null and enum tests (ops_dyn.c, value/value.c) -- */
op_result op_is_null(exec_ctx* ctx, const jello_insn* ins);
op_result op_is_enum(exec_ctx* ctx, const jello_insn* ins);
int vm_dynamic_fits_ptr(const jello_bc_module* m, jello_value v, jello_type_kind k,
                        jello_type_id type);
uint32_t vm_dynamic_is_enum(jello_value v, jello_type_id type);
op_result op_null_check(exec_ctx* ctx, const jello_insn* ins);

/* -- the boxing boundary (value/value.c) -- */
jello_value vm_box_from_typed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                               reg_frame* rf, uint32_t r);
int vm_store_from_boxed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                         reg_frame* rf, uint32_t dst, jello_value v);
uint32_t vm_kindof_dynamic(jello_value v);
uint32_t vm_dynamic_static_kind(jello_value v);

int vm_object_find(const jello_object* o, uint32_t atom_id, int keep, jello_value* out);
void vm_object_set_f64(jello_vm* vm, jello_object* o, uint32_t atom_id, double d);

/* -- JIT-facing Object wrappers (ops_obj.c) -- */
jello_value jit_obj_chain_get(jello_vm* vm, const jello_bc_module* m, jello_object* o, uint32_t atom_id);
void jit_obj_set_direct(const jello_bc_module* m, jello_object* o, uint32_t atom_id, jello_value v);
uint8_t vm_object_state(const jello_object* o, uint32_t atom_id);

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
op_result op_const_fun(exec_ctx* ctx, const jello_insn* ins);
op_result op_closure(exec_ctx* ctx, const jello_insn* ins);
op_result op_callr(exec_ctx* ctx, const jello_insn* ins);
op_result op_tailcall(exec_ctx* ctx, const jello_insn* ins);
op_result op_tailcallr(exec_ctx* ctx, const jello_insn* ins);
op_result op_spill_push(exec_ctx* ctx, const jello_insn* ins);
op_result op_spill_pop(exec_ctx* ctx, const jello_insn* ins);
op_result op_switch_kind(exec_ctx* ctx, const jello_insn* ins);
op_result op_case_kind(exec_ctx* ctx, const jello_insn* ins);

op_result op_global_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_global_set(exec_ctx* ctx, const jello_insn* ins);

/* -- Struct opcode handlers (ops_struct.c) -- */
op_result op_struct_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_struct_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_struct_set(exec_ctx* ctx, const jello_insn* ins);

/* -- Array opcode handlers (ops_array.c) -- */
op_result op_array_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_len(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_set(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_resize(exec_ctx* ctx, const jello_insn* ins);
op_result op_array_concat2(exec_ctx* ctx, const jello_insn* ins);

/* -- List opcode handlers (ops_list.c) -- */
op_result op_list_nil(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_cons(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_head(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_tail(exec_ctx* ctx, const jello_insn* ins);
op_result op_list_is_nil(exec_ctx* ctx, const jello_insn* ins);

/* -- Exception-handling opcode handlers (ops_exc.c) -- */
op_result op_try(exec_ctx* ctx, const jello_insn* ins);
op_result op_endtry(exec_ctx* ctx, const jello_insn* ins);
op_result op_throw(exec_ctx* ctx, const jello_insn* ins);

#define JELLO_TRAP_INTERNAL_UNWIND ((jello_trap_code)0x7F)
void vm_try_pop(jello_vm* vm);

/* -- Object opcode handlers (ops_obj.c) -- */
op_result op_obj_new(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_has_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_get_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_set_atom(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_get(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_set(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_has(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_remove(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_clear(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_copy(exec_ctx* ctx, const jello_insn* ins);
op_result op_obj_keys(exec_ctx* ctx, const jello_insn* ins);

typedef struct vm_protos {
  uint32_t len;
} vm_protos;

typedef struct vm_parcel {
  jello_value value;
  vm_protos* protos;     /**< Its Objects to delegate to the receiver's globals, or NULL. */
  jello_gc_hdr* objects; /**< The blocks copied for it, linked as a space's are. */
  jello_gc_hdr* tail;    /**< The last of them. */
  uint64_t bytes;        /**< What they count live, as alloc counts it. */
  struct proc_socket_list* sockets;
} vm_parcel;

static inline vm_parcel vm_parcel_empty(void) { return (vm_parcel){.value = jello_make_null()}; }
static inline void vm_parcel_move(vm_parcel* to, vm_parcel* from) {
  *to = *from;
  *from = vm_parcel_empty();
}
void vm_parcel_join(vm_parcel* into, vm_parcel* from);
void proc_sockets_join(struct proc_socket_list** into, struct proc_socket_list** from);

#define JELLO_BYTES_BUFFER 1u
#define VM_REDUCTIONS 2000u

static inline int vm_bytes_is_buffer(const jello_bytes* b) {
  return b->h.type_id == JELLO_BYTES_BUFFER;
}
jello_bytes* vm_buffer_new(jello_vm* vm, uint32_t length);
jello_value vm_clone_numbox(jello_vm* vm, jello_value v);
void* vm_immortal_alloc(size_t size);
size_t vm_bytes_immortal_size(uint32_t length);
jello_function* vm_function_immortal(uint32_t func_index, jello_type_id type);
void vm_immortal_free(void* p);

/* -- threads + the shared heap (thread/thread.c) -- */
jello_vm* jello_vm_create_joined(jello_vm* existing);
void vm_go_quiescent(jello_vm* vm);
void vm_wait_for_resume(jello_vm* vm);
void vm_leave_quiescent(jello_vm* vm);
void vm_safepoint_check(jello_vm* vm);
int vm_fuel_charge(jello_vm* vm);
int vm_heap_stop_the_world(jello_vm* vm);
void vm_heap_lock(jello_vm* vm);
void vm_heap_resume_the_world(jello_vm* vm);

/* -- the JDLL ABI (jdll/loader.c, jdll/marshal.c, jdll/call_native.c) -- */
typedef struct jello_jdll_handle jello_jdll_handle;
jello_jdll_handle* jello_jdll_load(const char* library_name);
const char* jello_jdll_last_error(void);
const char* jello_jdll_path(const jello_jdll_handle* h);
void jello_jdll_unload(jello_jdll_handle* h);
const struct jello_jdll_export* jello_jdll_find_export(const jello_jdll_handle* h, const char* name);

op_result op_call_native(exec_ctx* ctx, const jello_insn* ins);

#endif /* JELLO_VM_INTERNAL_H */
