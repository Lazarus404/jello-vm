// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

/* -- exception-handling opcodes -- */

op_result op_try(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  if(fr->try_stack_len >= JELLO_TRY_STACK_MAX) {
    vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
    return OP_TRAP;
  }
  try_scope* ts = &fr->try_stack[fr->try_stack_len++];
  ts->catch_pc = (uint32_t)((int32_t)fr->pc + (int32_t)ins->imm);
  ts->payload_reg = ins->a;
  return OP_CONTINUE;
}

op_result op_endtry(exec_ctx* ctx, const jello_insn* ins) {
  (void)ins;
  jello_vm* vm = ctx->vm;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  if(fr->try_stack_len > 0) fr->try_stack_len--;
  return OP_CONTINUE;
}

op_result op_throw(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  jello_value payload = vm_load_val(&fr->rf, ins->a);

  while(vm->call_frames_len > 0) {
    call_frame* top = &vm->call_frames[vm->call_frames_len - 1u];
    if(top->try_stack_len > 0) {
      try_scope ts = top->try_stack[--top->try_stack_len];
      vm_store_val(&top->rf, ts.payload_reg, payload);
      top->pc = ts.catch_pc;
      return OP_CONTINUE;
    }
    if(vm->call_frames_len == 1u) break; /* the top-level frame itself has no handler */
    vm_pop_frame(vm);
  }
  vm->trap_code = JELLO_TRAP_UNCAUGHT_THROW;
  return OP_TRAP;
}
