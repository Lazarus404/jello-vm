// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_H
#define JELLO_H

#include <stddef.h>
#include <stdint.h>

/* Host API -- opaque embedder surface. */

typedef uint32_t jello_type_id;

/* -- value kind tag -- */
typedef enum jello_type_kind {
  JELLO_T_VOID = 1,
  JELLO_T_BOOL = 2,
  JELLO_T_ATOM = 3,
  JELLO_T_I8 = 4,
  JELLO_T_I16 = 5,
  JELLO_T_I32 = 6,
  JELLO_T_I64 = 7,
  JELLO_T_F16 = 8,
  JELLO_T_F32 = 9,
  JELLO_T_F64 = 10,
  JELLO_T_BYTES = 11,
  JELLO_T_LIST = 12,
  JELLO_T_ARRAY = 13,
  JELLO_T_STRUCT = 14,
  JELLO_T_OBJECT = 15,
  JELLO_T_FUNCTION = 16,
  JELLO_T_ABSTRACT = 17,
  JELLO_T_DYNAMIC = 18,
  JELLO_T_ENUM = 19,
} jello_type_kind;

#define JELLO_TYPE_KIND_COUNT 19

size_t jello_slot_size(jello_type_kind k);

/* -- type-table entry and payload structs -- */
typedef struct jello_fun_sig {
  jello_type_id ret;
  uint16_t nargs;
  const jello_type_id* args;
} jello_fun_sig;

typedef struct jello_struct_sig {
  uint16_t nfields;
  uint8_t named;
  uint8_t is_mutable;
  const uint32_t* field_type;
  const uint32_t* field_name_atom; /* NULL when named == 0 */
} jello_struct_sig;

typedef struct jello_type_entry {
  jello_type_kind kind;
  union {
    struct { jello_type_id elem; } unary;          /* LIST, ARRAY */
    struct { uint32_t sig_id; } fun;               /* FUNCTION */
    struct { uint32_t struct_sig_id; } strct;      /* STRUCT */
    struct { uint32_t abstract_id; } abs;          /* ABSTRACT */
  } as;
} jello_type_entry;

/* -- loader API -- */
struct jello_bc_module;
typedef struct jello_bc_module jello_bc_module;

typedef enum jello_bc_error {
  JELLO_BC_OK = 0,
  JELLO_BC_EOF,
  JELLO_BC_BAD_MAGIC,
  JELLO_BC_UNSUPPORTED_VERSION,
  JELLO_BC_BAD_FORMAT,
  JELLO_BC_OUT_OF_MEMORY,
} jello_bc_error;

typedef struct jello_bc_result {
  jello_bc_error err;
  const char* msg;
  size_t offset;
} jello_bc_result;

jello_bc_result jello_bc_read(const uint8_t* data, size_t size, jello_bc_module** out);
void jello_bc_free(jello_bc_module* m);
uint32_t jello_bc_get_entry(const jello_bc_module* m);

/* -- interpreter (Chapter 4) -- */
struct jello_vm;
typedef struct jello_vm jello_vm;

jello_vm* jello_vm_create(void);
void jello_vm_destroy(jello_vm* vm);

typedef enum jello_exec_status {
  JELLO_EXEC_OK = 0,
  JELLO_EXEC_TRAP = 1,
} jello_exec_status;

typedef enum jello_trap_code {
  JELLO_TRAP_NONE = 0,
  JELLO_TRAP_ARITHMETIC = 1,
  JELLO_TRAP_STACK_OVERFLOW = 2,
} jello_trap_code;

jello_trap_code jello_vm_last_trap_code(const jello_vm* vm);

/* -- numeric-only value boundary (Chapter 4, deliberately not jello_value) --  */
typedef enum jello_numeric_kind {
  JELLO_NUM_VOID = 0,
  JELLO_NUM_BOOL,
  JELLO_NUM_I32,
  JELLO_NUM_I64,
  JELLO_NUM_F32,
  JELLO_NUM_F64,
} jello_numeric_kind;

typedef struct jello_numeric_result {
  jello_numeric_kind kind;
  union {
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    int b; /* 0/1, for JELLO_NUM_BOOL */
  } as;
} jello_numeric_result;

jello_exec_status jello_vm_exec_numeric(jello_vm* vm, const jello_bc_module* m, jello_numeric_result* out);

#endif /* JELLO_H */
