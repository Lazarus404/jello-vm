// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jdll_internal.h>

#include <stdlib.h>
#include <string.h>

/* -- JOP_CALL_NATIVE -- */
op_result op_call_native(exec_ctx* ctx, const jello_insn* ins) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;
  const jello_import_sig* sig = &m->imports[ins->imm];
  const jello_jdll_export* exp = sig->resolved;
  if(!exp) {
    vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }

  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  jdlo_ctx jctx;
  jctx.vm = vm;
  jctx.m = m;
  jctx.caller_rf = &fr->rf;
  jctx.ins = ins;
  jctx.sig = sig;
  jctx.returned = 0;
  jctx.failed = 0;
  jctx.fail_msg = NULL;

  vm_go_quiescent(vm);
  exp->fn(&jctx);
  vm_leave_quiescent(vm);

  if(jctx.failed) {
    vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }
  if(!jctx.returned || jctx.ret_kind_given != (jello_type_kind)sig->ret_kind) {
    vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
    return OP_TRAP;
  }

  if(jctx.ret_kind_given == JELLO_T_VOID) return OP_CONTINUE;

  reg_frame* rf = &fr->rf;
  switch(jctx.ret_kind_given) {
    case JELLO_T_BOOL:
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32:
    case JELLO_T_ATOM:
      vm_store_u32_masked(rf, ins->a, (uint32_t)jctx.ret.i32, jctx.ret_kind_given);
      break;
    case JELLO_T_I64:
      vm_store_i64(rf, ins->a, jctx.ret.i64);
      break;
    case JELLO_T_F16:
      vm_store_u32(rf, ins->a, (uint32_t)vm_f32_to_f16(jctx.ret.f32));
      break;
    case JELLO_T_F32:
      vm_store_f32(rf, ins->a, jctx.ret.f32);
      break;
    case JELLO_T_F64:
      vm_store_f64(rf, ins->a, jctx.ret.f64);
      break;
    case JELLO_T_DYNAMIC:
      vm_store_val(rf, ins->a, jctx.ret.dyn);
      break;
    case JELLO_T_BYTES: {
      jello_bytes* b = jello_bytes_new(vm, jctx.ret.bytes.len);
      if(jctx.ret.bytes.len) memcpy(b->data, jctx.ret.bytes.data, jctx.ret.bytes.len);
      free((void*)jctx.ret.bytes.data); /* jdl_return_bytes_copy's own malloc'd copy */
      vm_store_ptr(rf, ins->a, b);
      break;
    }
    default:
      vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
      return OP_TRAP;
  }
  return OP_CONTINUE;
}
