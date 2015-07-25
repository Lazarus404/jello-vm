// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_JDLL_H
#define JELLO_JDLL_H

#include <jello.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jdlo_ctx jdlo_ctx;

/* -- argument access -- */
int32_t jdl_arg_i8(jdlo_ctx* c, int index);
int32_t jdl_arg_i16(jdlo_ctx* c, int index);
int32_t jdl_arg_i32(jdlo_ctx* c, int index);
int64_t jdl_arg_i64(jdlo_ctx* c, int index);
float jdl_arg_f16(jdlo_ctx* c, int index); /* widened to float; caller narrows on return if needed */
float jdl_arg_f32(jdlo_ctx* c, int index);
double jdl_arg_f64(jdlo_ctx* c, int index);
int jdl_arg_bool(jdlo_ctx* c, int index);
uint32_t jdl_arg_atom(jdlo_ctx* c, int index);
const uint8_t* jdl_arg_bytes_data(jdlo_ctx* c, int index);
uint32_t jdl_arg_bytes_len(jdlo_ctx* c, int index);
jello_value jdl_arg_value(jdlo_ctx* c, int index); /* Dynamic-kind argument, already boxed */

/* -- returns -- */
void jdl_return_i8(jdlo_ctx* c, int32_t v);
void jdl_return_i16(jdlo_ctx* c, int32_t v);
void jdl_return_i32(jdlo_ctx* c, int32_t v);
void jdl_return_i64(jdlo_ctx* c, int64_t v);
void jdl_return_f16(jdlo_ctx* c, float v);
void jdl_return_f32(jdlo_ctx* c, float v);
void jdl_return_f64(jdlo_ctx* c, double v);
void jdl_return_bool(jdlo_ctx* c, int v);
void jdl_return_atom(jdlo_ctx* c, uint32_t atom_id);
void jdl_return_bytes_copy(jdlo_ctx* c, const uint8_t* data, uint32_t len);
void jdl_return_value(jdlo_ctx* c, jello_value v); /* Dynamic-kind return */
void jdl_return_void(jdlo_ctx* c);

/* -- errors -- */
void jdl_fail(jdlo_ctx* c, const char* msg);

/* -- the export table -- */
typedef void (*jdll_export_fn)(jdlo_ctx* c);

typedef struct jello_jdll_export {
  const char* name; /* Jello-visible export name, e.g. "file_read" */
  jdll_export_fn fn;
  uint8_t ret_kind; /* jello_type_kind */
  uint8_t arity;
  uint8_t arg_kinds[8]; /* v1 caps arity at 8; no varargs */
} jello_jdll_export;

#define JELLO_JDLL_MANIFEST_MAGIC 0x4D4C4444u /* 'JDLM' as bytes, LE */
#define JELLO_JDLL_ABI_VERSION 1u

typedef struct jello_jdll_manifest {
  uint32_t magic;
  uint32_t abi_version;
  const char* library_name;
  const jello_jdll_export* exports;
  uint32_t nexports;
} jello_jdll_manifest;

#ifdef __cplusplus
}
#endif

#endif /* JELLO_JDLL_H */
