// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdlib.h>

/* -- the dispatch loop -- */

jello_exec_status vm_exec_loop(exec_ctx* ctx) {
  jello_vm* vm = ctx->vm;
  const jello_bc_module* m = ctx->m;

  for(;;) {
    call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
    const jello_bc_function* f = fr->f;
    const jello_insn* ins = &f->insns[fr->pc++];
    reg_frame* rf = &fr->rf;

    switch((jello_op)ins->op) {
      case JOP_NOP:
        break;

      case JOP_RET: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint8_t has_caller = fr->has_caller;
        uint32_t caller_dst = fr->caller_dst;

        if(!has_caller) {
          jello_numeric_result* out = ctx->out;
          switch(k) {
            case JELLO_T_BOOL: out->kind = JELLO_NUM_BOOL; out->as.b = (int)vm_load_u32(rf, ins->a); break;
            case JELLO_T_I32: out->kind = JELLO_NUM_I32; out->as.i32 = (int32_t)vm_load_u32(rf, ins->a); break;
            case JELLO_T_I64: out->kind = JELLO_NUM_I64; out->as.i64 = vm_load_i64(rf, ins->a); break;
            case JELLO_T_F32: out->kind = JELLO_NUM_F32; out->as.f32 = vm_load_f32(rf, ins->a); break;
            case JELLO_T_F64: out->kind = JELLO_NUM_F64; out->as.f64 = vm_load_f64(rf, ins->a); break;
            case JELLO_T_BYTES: case JELLO_T_STRUCT: case JELLO_T_OBJECT:
              out->kind = JELLO_NUM_PTR; out->as.ptr = vm_load_ptr(rf, ins->a); break;
            case JELLO_T_DYNAMIC:
              out->kind = JELLO_NUM_DYN; out->as.dyn = vm_load_val(rf, ins->a); break;
            default: out->kind = JELLO_NUM_VOID; break;
          }
          vm_pop_frame(vm);
          return JELLO_EXEC_OK;
        }

        uint64_t tmp;
        int is_wide = vm_reg_is_wide(rf, ins->a);
        if(is_wide) tmp = *(const uint64_t*)vm_reg_ptr(rf, ins->a);
        else tmp = *(const uint32_t*)vm_reg_ptr(rf, ins->a);
        vm_pop_frame(vm);
        call_frame* caller = &vm->call_frames[vm->call_frames_len - 1u];
        uint8_t* dst = vm_reg_ptr(&caller->rf, caller_dst);
        if(is_wide) *(uint64_t*)dst = tmp;
        else *(uint32_t*)dst = (uint32_t)tmp;
        break;
      }

      case JOP_JMP:
        if((int32_t)ins->imm < 0) vm_safepoint_check(vm);
        fr->pc = (uint32_t)((int32_t)fr->pc + (int32_t)ins->imm);
        break;

      case JOP_JMP_IF:
        if(vm_load_u32(rf, ins->a) != 0u) {
          if((int32_t)ins->imm < 0) vm_safepoint_check(vm);
          fr->pc = (uint32_t)((int32_t)fr->pc + (int32_t)ins->imm);
        }
        break;

      case JOP_MOV: {
        if(vm_reg_is_wide(rf, ins->a)) vm_store_i64(rf, ins->a, vm_load_i64(rf, ins->b));
        else vm_store_u32(rf, ins->a, vm_load_u32(rf, ins->b));
        break;
      }

      case JOP_CALL: {
        vm_safepoint_check(vm);
        if(!vm_push_frame(vm, m, ins->imm, ins->a, ins->b, ins->c, 1)) {
          return JELLO_EXEC_TRAP;
        }
        break;
      }

      case JOP_CONST_I8_IMM: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        vm_store_u32_masked(rf, ins->a, ins->imm, k);
        break;
      }
      case JOP_CONST_I32:
        vm_store_u32(rf, ins->a, ins->imm);
        break;
      case JOP_CONST_I64:
        vm_store_i64(rf, ins->a, m->const_i64[ins->imm]);
        break;
      case JOP_CONST_F16:
        vm_store_u32(rf, ins->a, ins->imm & 0xFFFFu);
        break;
      case JOP_CONST_F32:
        vm_store_f32(rf, ins->a, vm_f32_from_bits(ins->imm));
        break;
      case JOP_CONST_F64:
        vm_store_f64(rf, ins->a, m->const_f64[ins->imm]);
        break;
      case JOP_CONST_BOOL:
        vm_store_u32(rf, ins->a, ins->imm ? 1u : 0u);
        break;

      case JOP_ADD_I32: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        uint32_t b = vm_load_u32(rf, ins->c);
        vm_store_u32_masked(rf, ins->a, a + b, k);
        break;
      }
      case JOP_SUB_I32: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        uint32_t b = vm_load_u32(rf, ins->c);
        vm_store_u32_masked(rf, ins->a, a - b, k);
        break;
      }
      case JOP_MUL_I32: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        uint32_t a = vm_load_u32(rf, ins->b);
        uint32_t b = vm_load_u32(rf, ins->c);
        vm_store_u32_masked(rf, ins->a, a * b, k);
        break;
      }
      case JOP_ADD_I32_IMM: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_u32_masked(rf, ins->a, (uint32_t)(a + (int32_t)ins->imm), k);
        break;
      }
      case JOP_SUB_I32_IMM: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_u32_masked(rf, ins->a, (uint32_t)(a - (int32_t)ins->imm), k);
        break;
      }
      case JOP_MUL_I32_IMM: {
        jello_type_kind k = vm_reg_kind_fast(rf, ins->a);
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_u32_masked(rf, ins->a, (uint32_t)(a * (int32_t)ins->imm), k);
        break;
      }

      case JOP_ADD_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_f64(rf, ins->a, a + b);
        break;
      }
      case JOP_SUB_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_f64(rf, ins->a, a - b);
        break;
      }
      case JOP_MUL_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_f64(rf, ins->a, a * b);
        break;
      }
      case JOP_DIV_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_f64(rf, ins->a, a / b);
        break;
      }
      case JOP_NEG_F64: {
        double x = vm_load_f64(rf, ins->b);
        vm_store_f64(rf, ins->a, -x);
        break;
      }
      case JOP_F64_FROM_I32: {
        int32_t x = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_f64(rf, ins->a, (double)x);
        break;
      }
      case JOP_NOT_BOOL: {
        uint32_t x = vm_load_u32(rf, ins->b);
        vm_store_u32(rf, ins->a, x ? 0u : 1u);
        break;
      }

      case JOP_EQ_I32: {
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        int32_t b = (int32_t)vm_load_u32(rf, ins->c);
        vm_store_u32(rf, ins->a, (a == b) ? 1u : 0u);
        break;
      }
      case JOP_LT_I32: {
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        int32_t b = (int32_t)vm_load_u32(rf, ins->c);
        vm_store_u32(rf, ins->a, (a < b) ? 1u : 0u);
        break;
      }
      case JOP_EQ_I32_IMM: {
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_u32(rf, ins->a, (a == (int32_t)ins->imm) ? 1u : 0u);
        break;
      }
      case JOP_LT_I32_IMM: {
        int32_t a = (int32_t)vm_load_u32(rf, ins->b);
        vm_store_u32(rf, ins->a, (a < (int32_t)ins->imm) ? 1u : 0u);
        break;
      }
      case JOP_EQ_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_u32(rf, ins->a, (a == b) ? 1u : 0u);
        break;
      }
      case JOP_LT_F64: {
        double a = vm_load_f64(rf, ins->b);
        double b = vm_load_f64(rf, ins->c);
        vm_store_u32(rf, ins->a, (a < b) ? 1u : 0u);
        break;
      }

      default: {
        op_result r = op_dispatch(ctx, ins);
        if(r == OP_TRAP) return JELLO_EXEC_TRAP;
        break;
      }
    }
  }
}

static jello_exec_status vm_exec_numeric_impl(jello_vm* vm, const jello_bc_module* m,
    jello_numeric_result* out, jello_jit_module* persistent_jm) {
  vm_safepoint_check(vm);

  atomic_store(&vm->in_exec, 1);

  vm_frame_reset(vm);
  out->kind = JELLO_NUM_VOID;

  vm->running_module = m;

  if(!vm_push_frame(vm, m, jello_bc_get_entry(m), 0, 0, 0, 0)) {
    atomic_store(&vm->in_exec, 0);
    return JELLO_EXEC_TRAP;
  }

#if defined(JELLO_ENABLE_JIT) && defined(__x86_64__)
  {
    const char* jit_off = getenv("JELLO_JIT_DISABLE");
    if(!(jit_off && jit_off[0] != '\0')) {
      vm_warm_jit_layouts(vm, m);
      jello_jit_module local_jm;
      int owns_jm = persistent_jm == NULL;
      jello_jit_module* jm = persistent_jm;
      if(owns_jm) {
        memset(&local_jm, 0, sizeof(local_jm));
        jm = &local_jm;
        jello_jit_module_prepare(m, jm);
      }
      jello_exec_status jit_st = JELLO_EXEC_OK;
      if(jello_jit_try_enter(vm, jm, out, &jit_st)) {
        if(owns_jm) jello_jit_module_free(jm);
        atomic_store(&vm->in_exec, 0);
        return jit_st;
      }
      if(owns_jm) jello_jit_module_free(jm);
    }
  }
#endif

  exec_ctx ctx;
  ctx.vm = vm;
  ctx.m = m;
  ctx.out = out;
  jello_exec_status st = vm_exec_loop(&ctx);
  atomic_store(&vm->in_exec, 0);
  return st;
}

jello_exec_status jello_vm_exec_numeric(jello_vm* vm, const jello_bc_module* m, jello_numeric_result* out) {
  return vm_exec_numeric_impl(vm, m, out, NULL);
}

jello_exec_status jello_vm_exec_numeric_with_jit(jello_vm* vm, jello_jit_module* jm,
    const jello_bc_module* m, jello_numeric_result* out) {
  return vm_exec_numeric_impl(vm, m, out, jm);
}

jello_trap_code jello_vm_last_trap_code(const jello_vm* vm) {
  return vm->trap_code;
}
