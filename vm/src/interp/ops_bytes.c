// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

/* -- Bytes opcode handlers (Chapter 4) -- */

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

op_result op_bytes_new(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t len = vm_load_u32(&fr->rf, ins->b);
  jello_bytes* b = jello_bytes_new(ctx->vm, len);
  void* p = (void*)b;
  memcpy(vm_reg_ptr(&fr->rf, ins->a), &p, sizeof(void*));
  return OP_CONTINUE;
}

op_result op_bytes_len(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  void* p = NULL;
  memcpy(&p, vm_reg_ptr(&fr->rf, ins->b), sizeof(void*));
  if(!p) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_bytes* b = (jello_bytes*)p;
  vm_store_u32(&fr->rf, ins->a, b->length);
  return OP_CONTINUE;
}

op_result op_bytes_get_u8(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  void* p = NULL;
  memcpy(&p, vm_reg_ptr(&fr->rf, ins->b), sizeof(void*));
  if(!p) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_bytes* b = (jello_bytes*)p;
  uint32_t idx = vm_load_u32(&fr->rf, ins->c);
  if(idx >= b->length) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  vm_store_u32(&fr->rf, ins->a, (uint32_t)b->data[idx]);
  return OP_CONTINUE;
}

op_result op_bytes_set_u8(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  void* p = NULL;
  memcpy(&p, vm_reg_ptr(&fr->rf, ins->b), sizeof(void*));
  if(!p) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_bytes* b = (jello_bytes*)p;
  uint32_t idx = vm_load_u32(&fr->rf, ins->c);
  if(idx >= b->length) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint32_t v = vm_load_u32(&fr->rf, ins->a);
  if(v > 255u) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  b->data[idx] = (uint8_t)v;
  return OP_CONTINUE;
}
