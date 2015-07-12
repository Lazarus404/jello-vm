// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

/* -- the boxing boundary -- */

static uint16_t vm_load_f16_bits(reg_frame* rf, uint32_t r) {
  return (uint16_t)vm_load_u32(rf, r);
}
static void vm_store_f16_bits(reg_frame* rf, uint32_t r, uint16_t bits) {
  vm_store_u32(rf, r, (uint32_t)bits);
}

static uint32_t vm_expected_obj_kind_for_ptr_kind(jello_type_kind k) {
  switch(k) {
    case JELLO_T_BYTES: return (uint32_t)JELLO_OBJ_BYTES;
    case JELLO_T_STRUCT: return (uint32_t)JELLO_OBJ_STRUCT;
    case JELLO_T_OBJECT: return (uint32_t)JELLO_OBJ_OBJECT;
    default: return 0u;
  }
}

jello_value vm_box_from_typed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                               reg_frame* rf, uint32_t r) {
  jello_type_kind k = vm_reg_kind(m, f, r);
  switch(k) {
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32:
      return jello_make_i32((int32_t)vm_load_u32(rf, r));
    case JELLO_T_BOOL:
      return jello_make_bool((int)(vm_load_u32(rf, r) != 0u));
    case JELLO_T_ATOM:
      return jello_make_atom(vm_load_u32(rf, r));
    case JELLO_T_I64:
      return jello_from_ptr(jello_box_i64_new(vm, vm_load_i64(rf, r)));
    case JELLO_T_F16:
      return jello_from_ptr(jello_box_f16_new(vm, vm_load_f16_bits(rf, r)));
    case JELLO_T_F32:
      return jello_from_ptr(jello_box_f32_new(vm, vm_load_f32(rf, r)));
    case JELLO_T_F64:
      return jello_from_ptr(jello_box_f64_new(vm, vm_load_f64(rf, r)));
    case JELLO_T_DYNAMIC:
      return vm_load_val(rf, r);
    case JELLO_T_VOID:
      return jello_make_null();
    default: {
      void* p = vm_load_ptr(rf, r);
      return p ? jello_from_ptr(p) : jello_make_null();
    }
  }
}

int vm_store_from_boxed(jello_vm* vm, const jello_bc_module* m, const jello_bc_function* f,
                         reg_frame* rf, uint32_t dst, jello_value v) {
  jello_type_kind k = vm_reg_kind(m, f, dst);
  switch(k) {
    case JELLO_T_I8:
    case JELLO_T_I16:
    case JELLO_T_I32:
      if(!jello_is_i32(v)) { vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; return 0; }
      vm_store_u32_masked(rf, dst, (uint32_t)jello_as_i32(v), k);
      return 1;
    case JELLO_T_I64:
      if(jello_is_i32(v)) { vm_store_i64(rf, dst, (int64_t)jello_as_i32(v)); return 1; }
      if(!jello_is_box_i64(v)) { vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; return 0; }
      vm_store_i64(rf, dst, jello_as_box_i64(v));
      return 1;
    case JELLO_T_F16:
      if(jello_is_i32(v)) { vm_store_f16_bits(rf, dst, (uint16_t)(int16_t)jello_as_i32(v)); return 1; }
      if(!jello_is_box_f16(v)) { vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; return 0; }
      vm_store_f16_bits(rf, dst, jello_as_box_f16(v));
      return 1;
    case JELLO_T_F32:
      if(jello_is_i32(v)) { vm_store_f32(rf, dst, (float)jello_as_i32(v)); return 1; }
      if(!jello_is_box_f32(v)) { vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; return 0; }
      vm_store_f32(rf, dst, jello_as_box_f32(v));
      return 1;
    case JELLO_T_F64:
      if(jello_is_i32(v)) { vm_store_f64(rf, dst, (double)jello_as_i32(v)); return 1; }
      if(!jello_is_box_f64(v)) { vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; return 0; }
      vm_store_f64(rf, dst, jello_as_box_f64(v));
      return 1;
    case JELLO_T_BOOL:
      if(!jello_is_bool(v)) { vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; return 0; }
      vm_store_u32(rf, dst, (uint32_t)jello_as_bool(v));
      return 1;
    case JELLO_T_ATOM:
      if(!jello_is_atom(v)) { vm->trap_code = JELLO_TRAP_TYPE_MISMATCH; return 0; }
      vm_store_u32(rf, dst, jello_as_atom(v));
      return 1;
    case JELLO_T_DYNAMIC:
      vm_store_val(rf, dst, v);
      return 1;
    default: {
      if(jello_is_null(v)) { vm_store_ptr(rf, dst, NULL); return 1; }
      uint32_t want = vm_expected_obj_kind_for_ptr_kind(k);
      if(want == 0u || !jello_is_ptr(v) || jello_as_ptr(v) == NULL ||
         ((const jello_obj_header*)jello_as_ptr(v))->kind != (uint8_t)want) {
        vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
        return 0;
      }
      vm_store_ptr(rf, dst, jello_as_ptr(v));
      return 1;
    }
  }
}

uint32_t vm_kindof_dynamic(jello_value v) {
  if(jello_is_null(v)) return 0u;   /* JELLO_KINDOF_NULL */
  if(jello_is_bool(v)) return 1u;   /* JELLO_KINDOF_BOOL */
  if(jello_is_i32(v)) return 2u;    /* JELLO_KINDOF_I32 */
  if(jello_is_atom(v)) return 3u;   /* JELLO_KINDOF_ATOM */
  if(!jello_is_ptr(v) || jello_as_ptr(v) == NULL) return 0x7fffffffu; /* JELLO_KINDOF_UNKNOWN */
  switch(((const jello_obj_header*)jello_as_ptr(v))->kind) {
    case JELLO_OBJ_BYTES: return 4u;
    case JELLO_OBJ_STRUCT: return 5u;
    case JELLO_OBJ_OBJECT: return 6u;
    case JELLO_OBJ_BOX_I64: return 7u;
    case JELLO_OBJ_BOX_F64: return 8u;
    case JELLO_OBJ_BOX_F32: return 9u;
    case JELLO_OBJ_BOX_F16: return 10u;
    default: return 0x7fffffffu;
  }
}
