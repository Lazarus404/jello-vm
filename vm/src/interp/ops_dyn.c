// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

/* -- the Dynamic boundary opcodes -- */

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

op_result op_to_dyn(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->b);
  vm_store_val(&fr->rf, ins->a, v);
  return OP_CONTINUE;
}

static op_result from_dyn_generic(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = vm_load_val(&fr->rf, ins->b);
  if(!vm_store_from_boxed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->a, v)) return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_from_dyn_i8(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_i16(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_i32(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_i64(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_f16(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_f32(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_f64(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_bool(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_atom(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }
op_result op_from_dyn_ptr(exec_ctx* ctx, const jello_insn* ins) { return from_dyn_generic(ctx, ins); }

op_result op_kindof(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value v = vm_load_val(&fr->rf, ins->b);
  vm_store_u32(&fr->rf, ins->a, vm_kindof_dynamic(v));
  return OP_CONTINUE;
}

op_result op_physeq(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_value vb = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->b);
  jello_value vc = vm_box_from_typed(ctx->vm, ctx->m, fr->f, &fr->rf, ins->c);
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(vb == vc));
  return OP_CONTINUE;
}

op_result op_const_null(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  vm_store_val(&fr->rf, ins->a, jello_make_null());
  return OP_CONTINUE;
}

op_result op_const_atom(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  vm_store_u32(&fr->rf, ins->a, ins->imm);
  return OP_CONTINUE;
}
