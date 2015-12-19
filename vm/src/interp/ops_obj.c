// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

/* -- Object opcodes -- */

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

static int obj_get_chain(jello_vm* vm, const jello_bc_module* m, jello_object* o,
                          uint32_t atom_id, jello_value* out) {
  if(!m->proto_enabled || atom_id == JELLO_ATOM___PROTO__ || !o->proto) {
    *out = jello_object_get(o, atom_id);
    return 1;
  }
  jello_object* cur = o;
  jello_value v = jello_make_null();
  for(uint32_t steps = 0;; steps++) {
    if(jello_object_has(cur, atom_id)) { v = jello_object_get(cur, atom_id); break; }
    jello_object* p = cur->proto;
    if(!p) break;
    cur = p;
    if(steps > 1024u) {
      vm->trap_code = JELLO_TRAP_BOUNDS;
      return 0;
    }
  }
  *out = v;
  return 1;
}

static void obj_set_direct(const jello_bc_module* m, jello_object* o, uint32_t atom_id, jello_value v) {
  jello_object_set(o, atom_id, v);
  if(!m->proto_enabled || atom_id != JELLO_ATOM___PROTO__) return;
  if(jello_is_ptr(v) && jello_as_ptr(v) != NULL &&
     ((const jello_obj_header*)jello_as_ptr(v))->kind == (uint8_t)JELLO_OBJ_OBJECT) {
    o->proto = (jello_object*)jello_as_ptr(v);
  } else {
    o->proto = NULL;
  }
}

op_result op_obj_new(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t type_id = fr->f->reg_types[ins->a];
  jello_object* o = jello_object_new(ctx->vm, type_id);
  vm_store_ptr(&fr->rf, ins->a, o);
  return OP_CONTINUE;
}

op_result op_obj_has_atom(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(jello_object_has(o, ins->imm) != 0));
  return OP_CONTINUE;
}

op_result op_obj_get_atom(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_value v;
  if(!obj_get_chain(ctx->vm, ctx->m, o, ins->imm, &v)) return OP_TRAP;
  if(!vm_store_from_boxed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a, v)) return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_obj_set_atom(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_value v = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a);
  obj_set_direct(ctx->m, o, ins->imm, v);
  return OP_CONTINUE;
}

op_result op_obj_get(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t atom_id = vm_load_u32(&fr->rf, ins->c);
  if(atom_id >= ctx->m->natoms) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  jello_value v;
  if(!obj_get_chain(ctx->vm, ctx->m, o, atom_id, &v)) return OP_TRAP;
  if(!vm_store_from_boxed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a, v)) return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_obj_set(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t atom_id = vm_load_u32(&fr->rf, ins->c);
  if(atom_id >= ctx->m->natoms) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  jello_value v = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a);
  obj_set_direct(ctx->m, o, atom_id, v);
  return OP_CONTINUE;
}

op_result op_obj_has(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t atom_id = vm_load_u32(&fr->rf, ins->c);
  if(atom_id >= ctx->m->natoms) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(jello_object_has(o, atom_id) != 0));
  return OP_CONTINUE;
}

op_result op_obj_remove(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  uint32_t atom_id = vm_load_u32(&fr->rf, ins->c);
  if(atom_id >= ctx->m->natoms) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(jello_object_remove(o, atom_id) != 0));
  return OP_CONTINUE;
}

op_result op_obj_clear(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* o = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!o) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_object_clear(o);
  return OP_CONTINUE;
}

/* -- JIT-facing wrappers around this file's own static helpers -- */
jello_value jit_obj_chain_get(jello_vm* vm, const jello_bc_module* m, jello_object* o, uint32_t atom_id) {
  vm->trap_code = JELLO_TRAP_NONE;
  jello_value v;
  if(!obj_get_chain(vm, m, o, atom_id, &v)) return jello_make_null();
  return v;
}

void jit_obj_set_direct(const jello_bc_module* m, jello_object* o, uint32_t atom_id, jello_value v) {
  obj_set_direct(m, o, atom_id, v);
}

op_result op_obj_copy(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_object* src = (jello_object*)vm_load_ptr(&fr->rf, ins->b);
  if(!src) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_object* dst = jello_object_copy(ctx->vm, src);
  vm_store_ptr(&fr->rf, ins->a, dst);
  return OP_CONTINUE;
}
