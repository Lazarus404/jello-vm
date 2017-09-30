// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <jello/internal/platform.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* -- frame stack + per-function layout cache -- */

static int frame_stack_reserve(jello_vm* vm) {
  if(vm->frame_stack) return 1;
  void* mem = mmap(NULL, JIT_FRAME_STACK_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(mem == MAP_FAILED) return 0;
  vm->frame_stack = (uint8_t*)mem;
  vm->frame_stack_cap = JIT_FRAME_STACK_MAX;
  return 1;
}

jello_type_kind vm_reg_kind(const jello_bc_module* m, const jello_bc_function* f, uint32_t r) {
  return m->types[f->reg_types[r]].kind;
}

int vm_func_has_pointer(const jello_bc_module* m, const jello_bc_function* f) {
  for(uint32_t r = 0; r < f->nregs; r++) {
    if(vm_kind_needs_gc_scan(vm_reg_kind(m, f, r))) return 1;
  }
  return 0;
}

static frame_layout* find_layout(jello_vm* vm, const jello_bc_function* f) {
  for(uint32_t i = 0; i < vm->frame_layouts_len; i++) {
    if(vm->frame_layouts[i].f == f) return &vm->frame_layouts[i];
  }
  return NULL;
}

void vm_free_layouts(jello_vm* vm) {
  for(uint32_t i = 0; i < vm->frame_layouts_len; i++) {
    free(vm->frame_layouts[i].kind);
    free(vm->frame_layouts[i].wide);
  }
  free(vm->frame_layouts);
  vm->frame_layouts = NULL;
  vm->frame_layouts_len = 0;
  vm->frame_layouts_cap = 0;
  free(vm->jit_layout_by_func);
  vm->jit_layout_by_func = NULL;
  vm->jit_layout_by_func_len = 0;
}

static int is_pointer_kind(jello_type_kind k) {
  return k == JELLO_T_BYTES || k == JELLO_T_STRUCT || k == JELLO_T_OBJECT || k == JELLO_T_DYNAMIC ||
         k == JELLO_T_ARRAY || k == JELLO_T_FUNCTION || k == JELLO_T_LIST;
}

static void null_init_dynamic_regs(uint32_t nregs, reg_frame* rf) {
  for(uint32_t r = 0; r < nregs; r++) {
    if(vm_reg_kind_fast(rf, r) == JELLO_T_DYNAMIC) vm_store_val(rf, r, jello_make_null());
  }
}

static frame_layout* build_layout(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f) {
  uint8_t* kind = f->nregs ? (uint8_t*)malloc((size_t)f->nregs * sizeof(uint8_t)) : NULL;
  uint8_t* wide = f->nregs ? (uint8_t*)malloc((size_t)f->nregs * sizeof(uint8_t)) : NULL;
  int has_pointer = 0;
  for(uint32_t r = 0; r < f->nregs; r++) {
    jello_type_kind k = vm_reg_kind(m, f, r);
    kind[r] = (uint8_t)k;
    wide[r] = (uint8_t)(jello_slot_size(k) == 8u);
    if(is_pointer_kind(k)) has_pointer = 1;
  }

  if(vm->frame_layouts_len == vm->frame_layouts_cap) {
    vm->frame_layouts_cap = vm->frame_layouts_cap ? vm->frame_layouts_cap * 2u : 8u;
    vm->frame_layouts = (frame_layout*)realloc(vm->frame_layouts, (size_t)vm->frame_layouts_cap * sizeof(frame_layout));
  }
  frame_layout* fl = &vm->frame_layouts[vm->frame_layouts_len++];
  fl->f = f;
  fl->kind = kind;
  fl->wide = wide;
  fl->total = f->nregs * REG_SLOT_BYTES;
  fl->has_pointer = has_pointer;
  return fl;
}

static frame_layout* get_layout(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f) {
  frame_layout* fl = find_layout(vm, f);
  return fl ? fl : build_layout(vm, m, f);
}

static int frame_stack_grow(jello_vm* vm, uint32_t need) {
  (void)need;
  return frame_stack_reserve(vm);
}

void vm_warm_jit_layouts(jello_vm* vm, const jello_bc_module* m) {
  free(vm->jit_layout_by_func);
  vm->jit_layout_by_func = NULL;
  vm->jit_layout_by_func_len = 0;
  if(!m || m->nfuncs == 0) return;
  vm->jit_layout_by_func = (frame_layout**)calloc(m->nfuncs, sizeof(frame_layout*));
  if(!vm->jit_layout_by_func) return;
  vm->jit_layout_by_func_len = m->nfuncs;
  for(uint32_t fi = 0; fi < m->nfuncs; fi++)
    (void)get_layout(vm, m, &m->funcs[fi]);
  for(uint32_t fi = 0; fi < m->nfuncs; fi++)
    vm->jit_layout_by_func[fi] = find_layout(vm, &m->funcs[fi]);
}

int jit_try_grow_call_frames(jello_vm* vm) {
  if(vm->call_frames_len < vm->call_frames_cap) return 1;
  uint32_t new_cap = vm->call_frames_cap ? vm->call_frames_cap * 2u : 64u;
  call_frame* p = (call_frame*)realloc(vm->call_frames, (size_t)new_cap * sizeof(call_frame));
  if(!p) return 0;
  vm->call_frames = p;
  vm->call_frames_cap = new_cap;
  return 1;
}

uint32_t jit_layout_total_or_zero(jello_vm* vm, uint32_t fi) {
  if(!vm || fi >= vm->jit_layout_by_func_len || !vm->jit_layout_by_func) return 0;
  frame_layout* fl = vm->jit_layout_by_func[fi];
  return fl ? fl->total : 0;
}

frame_layout* jit_layout_ptr_or_null(jello_vm* vm, uint32_t fi) {
  if(!vm || fi >= vm->jit_layout_by_func_len || !vm->jit_layout_by_func) return NULL;
  return vm->jit_layout_by_func[fi];
}

int jit_call_push_numeric(jello_vm* vm, uint32_t callee_fi, uint32_t caller_dst,
                          uint32_t arg_first, uint32_t nargs) {
  if(!vm || !vm->running_module) return 0;
  return vm_push_frame(vm, vm->running_module, callee_fi, caller_dst, arg_first, nargs, 1);
}

int jit_try_grow_frame_stack(jello_vm* vm, uint32_t need) {
  if((uint64_t)vm->frame_stack_top + need > JIT_FRAME_STACK_MAX) return 0;
  return frame_stack_grow(vm, need);
}

uint8_t* jit_current_frame_mem(jello_vm* vm) {
  if(!vm || vm->call_frames_len == 0) return vm ? vm->frame_stack : NULL;
  return vm->call_frames[vm->call_frames_len - 1u].rf.mem;
}

static call_frame* push_frame_shell(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                                     uint32_t caller_dst, uint8_t has_caller) {
  const jello_bc_function* f = &m->funcs[func_index];
  frame_layout* fl = get_layout(vm, m, f);

  if((uint64_t)vm->frame_stack_top + fl->total > JIT_FRAME_STACK_MAX || vm->call_frames_len >= JIT_CALL_FRAMES_MAX) {
    vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
    return NULL;
  }
  if(!frame_stack_grow(vm, fl->total)) {
    vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
    return NULL;
  }
  if(vm->call_frames_len == vm->call_frames_cap) {
    uint32_t new_cap = vm->call_frames_cap ? vm->call_frames_cap * 2u : 64u;
    call_frame* p = (call_frame*)realloc(vm->call_frames, (size_t)new_cap * sizeof(call_frame));
    if(!p) {
      vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
      return NULL;
    }
    vm->call_frames = p;
    vm->call_frames_cap = new_cap;
  }

  uint8_t* mem = vm->frame_stack + vm->frame_stack_top;
  vm->frame_stack_top += fl->total;

  call_frame* fr = &vm->call_frames[vm->call_frames_len];
  fr->f = f;
  fr->func_index = func_index;
  fr->pc = 0;
  fr->rf.mem = mem;
  fr->rf.kind = fl->kind;
  fr->rf.wide = fl->wide;
  fr->rf.size = fl->total;
  fr->has_caller = has_caller;
  fr->has_pointer = (uint8_t)fl->has_pointer;
  fr->caller_dst = caller_dst;
  fr->spill_slots = NULL;
  fr->spill_cap = 0;

  if(fl->has_pointer) {
    memset(mem, 0, fl->total);
    null_init_dynamic_regs(f->nregs, &fr->rf);
  }

  return fr;
}

int vm_push_frame(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                  uint32_t caller_dst, uint32_t arg_first, uint32_t arg_count, uint8_t has_caller) {
  call_frame* fr = push_frame_shell(vm, m, func_index, caller_dst, has_caller);
  if(!fr) return 0;
  call_frame* caller = has_caller ? &vm->call_frames[vm->call_frames_len - 1u] : NULL;

  for(uint32_t i = 0; i < arg_count; i++) {
    if(vm_reg_is_wide(&fr->rf, i)) {
      vm_store_i64(&fr->rf, i, vm_load_i64(&caller->rf, arg_first + i));
    } else {
      vm_store_u32(&fr->rf, i, vm_load_u32(&caller->rf, arg_first + i));
    }
  }

  vm->call_frames_len++;
  return 1;
}

int vm_push_frame_raw(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                       uint32_t caller_dst, uint8_t has_caller,
                       const uint64_t* argbuf, uint32_t arg_count,
                       const uint64_t* capbuf, uint32_t ncaps) {
  call_frame* fr = push_frame_shell(vm, m, func_index, caller_dst, has_caller);
  if(!fr) return 0;

  for(uint32_t i = 0; i < arg_count; i++) {
    if(vm_reg_is_wide(&fr->rf, i)) vm_store_i64(&fr->rf, i, (int64_t)argbuf[i]);
    else vm_store_u32(&fr->rf, i, (uint32_t)argbuf[i]);
  }
  uint32_t cap_start = fr->f->cap_start;
  for(uint32_t i = 0; i < ncaps; i++) {
    if(vm_reg_is_wide(&fr->rf, cap_start + i)) vm_store_i64(&fr->rf, cap_start + i, (int64_t)capbuf[i]);
    else vm_store_u32(&fr->rf, cap_start + i, (uint32_t)capbuf[i]);
  }

  vm->call_frames_len++;
  return 1;
}

void vm_copy_captures_into_top(jello_vm* vm, const jello_function* fn) {
  if(!fn || fn->ncaps == 0) return;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  uint32_t cap_start = fr->f->cap_start;
  for(uint32_t i = 0; i < fn->ncaps; i++) {
    if(vm_reg_is_wide(&fr->rf, cap_start + i)) vm_store_i64(&fr->rf, cap_start + i, (int64_t)fn->captures[i]);
    else vm_store_u32(&fr->rf, cap_start + i, (uint32_t)fn->captures[i]);
  }
}

const jello_function* vm_callee_function(jello_vm* vm, jello_value v) {
  const jello_function* fn = jello_is_ptr(v) ? (const jello_function*)jello_as_ptr(v) : NULL;
  if(!fn) {
    vm->trap_code =
        (jello_is_null(v) || jello_is_ptr(v)) ? JELLO_TRAP_NULL_DEREF : JELLO_TRAP_TYPE_MISMATCH;
    return NULL;
  }
  if(fn->h.kind != (uint8_t)JELLO_OBJ_FUNCTION) {
    vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return NULL;
  }
  return fn;
}

void vm_pop_frame(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  free(fr->spill_slots);
  fr->spill_slots = NULL;
  fr->spill_cap = 0;
  vm->frame_stack_top -= fr->rf.size;
  vm->call_frames_len--;
}

void vm_frame_reset(jello_vm* vm) {
  vm->frame_stack_top = 0;
  vm->call_frames_len = 0;
  vm->trap_code = JELLO_TRAP_NONE;
  vm_free_layouts(vm);
}
