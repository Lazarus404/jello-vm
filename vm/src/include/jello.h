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

#endif /* JELLO_H */
