// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdlib.h>
#include <string.h>

/* -- frame stack + per-function layout cache -- */

#define FRAME_STACK_INITIAL (64u * 1024u)
#define FRAME_STACK_MAX (64u * 1024u * 1024u)
#define CALL_FRAMES_MAX 200000u

static uint32_t align_up(uint32_t x, uint32_t a) {
  return (a == 0u) ? x : (x + (a - 1u)) & ~(a - 1u);
}

jello_type_kind vm_reg_kind(const jello_bc_module* m, const jello_bc_function* f, uint32_t r) {
  return m->types[f->reg_types[r]].kind;
}

static frame_layout* find_layout(jello_vm* vm, const jello_bc_function* f) {
  for(uint32_t i = 0; i < vm->frame_layouts_len; i++) {
    if(vm->frame_layouts[i].f == f) return &vm->frame_layouts[i];
  }
  return NULL;
}

void vm_free_layouts(jello_vm* vm) {
  for(uint32_t i = 0; i < vm->frame_layouts_len; i++) free(vm->frame_layouts[i].off);
  free(vm->frame_layouts);
  vm->frame_layouts = NULL;
  vm->frame_layouts_len = 0;
  vm->frame_layouts_cap = 0;
  vm->frame_layouts_mod = NULL;
}

static int is_pointer_kind(jello_type_kind k) {
  return k == JELLO_T_BYTES || k == JELLO_T_STRUCT || k == JELLO_T_OBJECT || k == JELLO_T_DYNAMIC;
}

static void null_init_dynamic_regs(const jello_bc_module* m, const jello_bc_function* f,
                                    reg_frame* rf) {
  for(uint32_t r = 0; r < f->nregs; r++) {
    if(vm_reg_kind(m, f, r) == JELLO_T_DYNAMIC) vm_store_val(rf, r, jello_make_null());
  }
}

static frame_layout* build_layout(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f) {
  uint32_t* off = f->nregs ? (uint32_t*)malloc((size_t)f->nregs * sizeof(uint32_t)) : NULL;
  uint32_t cursor = 0;
  int has_pointer = 0;
  for(uint32_t r = 0; r < f->nregs; r++) {
    jello_type_kind k = vm_reg_kind(m, f, r);
    uint32_t sz = (uint32_t)jello_slot_size(k);
    cursor = align_up(cursor, sz);
    off[r] = cursor;
    cursor += sz;
    if(is_pointer_kind(k)) has_pointer = 1;
  }
  cursor = align_up(cursor, 8u);

  if(vm->frame_layouts_len == vm->frame_layouts_cap) {
    vm->frame_layouts_cap = vm->frame_layouts_cap ? vm->frame_layouts_cap * 2u : 8u;
    vm->frame_layouts = (frame_layout*)realloc(vm->frame_layouts, (size_t)vm->frame_layouts_cap * sizeof(frame_layout));
  }
  frame_layout* fl = &vm->frame_layouts[vm->frame_layouts_len++];
  fl->f = f;
  fl->off = off;
  fl->total = cursor;
  fl->has_pointer = has_pointer;
  return fl;
}

static frame_layout* get_layout(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f) {
  if(vm->frame_layouts_mod != m) {
    vm_free_layouts(vm);
    vm->frame_layouts_mod = m;
  }
  frame_layout* fl = find_layout(vm, f);
  return fl ? fl : build_layout(vm, m, f);
}

static int frame_stack_grow(jello_vm* vm, uint32_t need) {
  if((uint64_t)vm->frame_stack_top + need <= vm->frame_stack_cap) return 1;
  uint32_t new_cap = vm->frame_stack_cap ? vm->frame_stack_cap * 4u : FRAME_STACK_INITIAL;
  while((uint64_t)new_cap < (uint64_t)vm->frame_stack_top + need) new_cap *= 4u;
  uint8_t* old = vm->frame_stack;
  uint8_t* p = (uint8_t*)realloc(vm->frame_stack, new_cap);
  if(!p) return 0;
  vm->frame_stack = p;
  vm->frame_stack_cap = new_cap;
  if(p != old && old != NULL) {
    for(uint32_t i = 0; i < vm->call_frames_len; i++) {
      call_frame* fr = &vm->call_frames[i];
      fr->rf.mem = p + (fr->rf.mem - old);
    }
  }
  return 1;
}

int vm_push_frame(jello_vm* vm, const jello_bc_module* m, uint32_t func_index,
                   uint32_t caller_dst, uint32_t arg_first, uint32_t arg_count, uint8_t has_caller) {
  const jello_bc_function* f = &m->funcs[func_index];
  frame_layout* fl = get_layout(vm, m, f);

  if((uint64_t)vm->frame_stack_top + fl->total > FRAME_STACK_MAX || vm->call_frames_len >= CALL_FRAMES_MAX) {
    vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
    return 0;
  }
  if(!frame_stack_grow(vm, fl->total)) {
    vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
    return 0;
  }
  if(vm->call_frames_len == vm->call_frames_cap) {
    uint32_t new_cap = vm->call_frames_cap ? vm->call_frames_cap * 2u : 64u;
    call_frame* p = (call_frame*)realloc(vm->call_frames, (size_t)new_cap * sizeof(call_frame));
    if(!p) {
      vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
      return 0;
    }
    vm->call_frames = p;
    vm->call_frames_cap = new_cap;
  }

  uint8_t* mem = vm->frame_stack + vm->frame_stack_top;
  vm->frame_stack_top += fl->total;

  call_frame* caller = has_caller ? &vm->call_frames[vm->call_frames_len - 1u] : NULL;
  call_frame* fr = &vm->call_frames[vm->call_frames_len];
  fr->f = f;
  fr->func_index = func_index;
  fr->pc = 0;
  fr->rf.mem = mem;
  fr->rf.off = fl->off;
  fr->rf.size = fl->total;
  fr->has_caller = has_caller;
  fr->has_pointer = (uint8_t)fl->has_pointer;
  fr->caller_dst = caller_dst;

  if(fl->has_pointer) {
    memset(mem, 0, fl->total);
    null_init_dynamic_regs(m, f, &fr->rf);
  }

  for(uint32_t i = 0; i < arg_count; i++) {
    if(jello_slot_size(vm_reg_kind(m, f, i)) == 8u) {
      vm_store_i64(&fr->rf, i, vm_load_i64(&caller->rf, arg_first + i));
    } else {
      vm_store_u32(&fr->rf, i, vm_load_u32(&caller->rf, arg_first + i));
    }
  }

  vm->call_frames_len++;
  return 1;
}

void vm_pop_frame(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  vm->frame_stack_top -= fr->rf.size;
  vm->call_frames_len--;
}

void vm_frame_reset(jello_vm* vm) {
  vm->frame_stack_top = 0;
  vm->call_frames_len = 0;
  vm->trap_code = JELLO_TRAP_NONE;
}
