// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdlib.h>
#include <stdio.h>

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


/* -- Function values (Chapter 16 self-host) -----------------------------------
 * CONST_FUN and CLOSURE both produce a jello_function heap object (jello.h's
 * own "Function" comment) -- CONST_FUN is exactly CLOSURE with ncaps == 0,
 * never a bare func_index stashed directly in the register the way this
 * opcode's own pre-Chapter-16 placeholder body did. That uniformity is what
 * lets CALLR/TAILCALLR treat every Function-kind value identically, whether
 * or not it actually closes over anything.
 */
op_result op_const_fun(exec_ctx* ctx, const jello_insn* ins) {
  jello_function* fn = jello_function_new(ctx->vm, ins->imm, 0);
  call_frame* fr = current_frame(ctx);
  vm_store_ptr(&fr->rf, ins->a, fn);
  return OP_CONTINUE;
}

op_result op_closure(exec_ctx* ctx, const jello_insn* ins) {
  uint32_t ncaps = (uint32_t)ins->c;
  jello_function* fn = jello_function_new(ctx->vm, ins->imm, ncaps);
  call_frame* fr = current_frame(ctx);
  for(uint32_t i = 0; i < ncaps; i++) {
    uint32_t src = (uint32_t)ins->b + i;
    fn->captures[i] = vm_reg_is_wide(&fr->rf, src)
        ? (uint64_t)vm_load_i64(&fr->rf, src)
        : (uint64_t)vm_load_u32(&fr->rf, src);
  }
  vm_store_ptr(&fr->rf, ins->a, fn);
  return OP_CONTINUE;
}

/* -- indirect/tail calls through a Function value (Chapter 16 self-host) -----
 * CALLR/TAILCALLR's own `b` register is the *first slot of a contiguous
 * window*: the Function value itself, immediately followed by its actual
 * call arguments -- jello-refined's own emit.jello em_phys CALL-family
 * override (b = ra_map(args[0]), c = Array.len(args), where args[0] is the
 * callee and args[1..] are its real arguments) is the live producer this
 * contract matches (JELLO_COMPILER_OPTIMISATION.md's own Chapter 16
 * findings on the encoding this VM must consume).
 */
op_result op_callr(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  void* p = vm_load_ptr(&fr->rf, ins->b);
  if(!p) { ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF; return OP_TRAP; }
  jello_function* fn = (jello_function*)p;
  /* Matches the host jellovm's own (pre-existing, authoritative --
   * jellovm/src/vm/ops/ops_call.c) CALLR contract: the callee register
   * (b) and the argument-window base are two genuinely separate fields,
   * not one combined "callee+args" window starting at b. c is the real
   * argument count outright; imm names where that count of consecutive
   * registers actually starts. Chapter 16 self-host bug: this opcode's
   * own first implementation folded the callee into the same window as
   * its arguments (b, b+1, b+2, ...), which jello-refined's own compiler
   * emission (emit.jello) was written to match -- correct only relative
   * to itself, and silently incompatible with the host jellovm every
   * Stage 1/Stage 2 bootstrap run actually depends on. */
  uint32_t arg_count = (uint32_t)ins->c;
  uint32_t arg_first = (uint32_t)ins->imm;
  if(!vm_push_frame(ctx->vm, ctx->m, fn->func_index, ins->a, arg_first, arg_count, 1)) return OP_TRAP;
  vm_copy_captures_into_top(ctx->vm, fn);
  return OP_CONTINUE;
}

/* TAILCALL/TAILCALLR replace the current frame outright rather than
 * growing the frame stack -- required for a self-tail-recursive Jello
 * function (pervasive throughout lower's own recursive AST walk modules)
 * not to blow JIT_FRAME_STACK_MAX the way an ordinary CALL-per-recursion
 * would. Arguments (and, for TAILCALLR, captures) are copied out to a flat
 * buffer *before* the current frame is popped, since both live in
 * registers that frame owns and would otherwise vanish under it -- see
 * vm_push_frame_raw's own header comment (vm_internal.h). emit.jello
 * appends a synthetic RET immediately after every TAILCALL/TAILCALLR
 * (Chapter 16 audit finding); that RET is unreachable dead code once this
 * opcode has genuinely replaced the frame, not a real fallback path this
 * handler needs to cooperate with. */
op_result op_tailcall(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  uint32_t arg_count = (uint32_t)ins->c;
  uint64_t argbuf[256];
  for(uint32_t i = 0; i < arg_count; i++) {
    uint32_t src = (uint32_t)ins->b + i;
    argbuf[i] = vm_reg_is_wide(&fr->rf, src) ? (uint64_t)vm_load_i64(&fr->rf, src) : (uint64_t)vm_load_u32(&fr->rf, src);
  }
  uint8_t has_caller = fr->has_caller;
  uint32_t caller_dst = fr->caller_dst;
  vm_pop_frame(vm);
  if(!vm_push_frame_raw(vm, ctx->m, ins->imm, caller_dst, has_caller, argbuf, arg_count, NULL, 0)) return OP_TRAP;
  return OP_CONTINUE;
}

op_result op_tailcallr(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  void* p = vm_load_ptr(&fr->rf, ins->b);
  if(!p) { vm->trap_code = JELLO_TRAP_NULL_DEREF; return OP_TRAP; }
  jello_function* fn = (jello_function*)p;
  /* Same contract fix as op_callr's own -- see its comment. */
  uint32_t arg_count = (uint32_t)ins->c;
  uint32_t arg_first = (uint32_t)ins->imm;
  uint64_t argbuf[256];
  for(uint32_t i = 0; i < arg_count; i++) {
    uint32_t src = arg_first + i;
    argbuf[i] = vm_reg_is_wide(&fr->rf, src) ? (uint64_t)vm_load_i64(&fr->rf, src) : (uint64_t)vm_load_u32(&fr->rf, src);
  }
  uint32_t ncaps = fn->ncaps;
  uint64_t capbuf[256];
  for(uint32_t i = 0; i < ncaps; i++) capbuf[i] = fn->captures[i];
  uint32_t func_index = fn->func_index;
  uint8_t has_caller = fr->has_caller;
  uint32_t caller_dst = fr->caller_dst;
  vm_pop_frame(vm);
  if(!vm_push_frame_raw(vm, ctx->m, func_index, caller_dst, has_caller, argbuf, arg_count, capbuf, ncaps)) return OP_TRAP;
  return OP_CONTINUE;
}
