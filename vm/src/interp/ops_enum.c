// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stddef.h>
#if defined(_WIN32) || defined(__MINGW32__)
#  include <malloc.h>
#  ifndef alloca
#    define alloca _alloca
#  endif
#else
#  include <alloca.h>
#endif
#include <string.h>

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

jello_enum* jello_enum_new(jello_vm* vm, uint32_t type_id, uint32_t tag, uint32_t nfields, const jello_value* fields) {
  size_t payload = (size_t)nfields * sizeof(jello_value);
  size_t total = offsetof(jello_enum, fields) + payload;
  jello_enum* e = (jello_enum*)jello_gc_alloc(vm, total);
  e->h.kind = (uint8_t)JELLO_OBJ_ENUM;
  e->h.type_id = type_id;
  e->tag = tag;
  e->nfields = nfields;
  if(nfields && fields) {
    memcpy(e->fields, fields, payload);
  }
  return e;
}

op_result op_enum_new(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;
  call_frame* fr = current_frame(ctx);
  const jello_bc_function* f = fr->f;

  uint32_t type_id = f->reg_types[ins->a];
  uint32_t tag = ins->imm;
  uint32_t nfields = (uint32_t)ins->c;
  jello_enum* e;
  if(nfields == 0) {
    e = jello_enum_new(vm, type_id, tag, 0, NULL);
  } else {
    jello_value* fields = (jello_value*)alloca((size_t)nfields * sizeof(jello_value));
    for(uint32_t i = 0; i < nfields; i++) {
      uint32_t r = (uint32_t)ins->b + i;
      if(r >= f->nregs) {
        ctx->vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return OP_TRAP;
      }
      fields[i] = vm_box_from_typed(vm, m, f, &fr->rf, r);
    }
    e = jello_enum_new(vm, type_id, tag, nfields, fields);
  }
  vm_store_ptr(&fr->rf, ins->a, e);
  return OP_CONTINUE;
}

op_result op_enum_tag(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);

  jello_enum* e = (jello_enum*)vm_load_ptr(&fr->rf, ins->b);
  if(!e) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  if(((const jello_obj_header*)e)->kind != (uint8_t)JELLO_OBJ_ENUM) {
    ctx->vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }
  vm_store_u32(&fr->rf, ins->a, e->tag);
  return OP_CONTINUE;
}

op_result op_enum_get(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;
  call_frame* fr = current_frame(ctx);
  const jello_bc_function* f = fr->f;

  jello_enum* e = (jello_enum*)vm_load_ptr(&fr->rf, ins->b);
  if(!e) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  if(((const jello_obj_header*)e)->kind != (uint8_t)JELLO_OBJ_ENUM) {
    ctx->vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }
  uint32_t idx = vm_load_u32(&fr->rf, ins->c);
  if(idx >= e->nfields) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  if(!vm_store_from_boxed(vm, m, f, &fr->rf, ins->a, e->fields[idx])) return OP_TRAP;
  return OP_CONTINUE;
}
