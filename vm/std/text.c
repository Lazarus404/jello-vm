// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Text conversions the compiler lowers to; Std.string and std.core.Bytes.u8. */

#include "std.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t* value_text(jdlo_ctx* c, jello_value v, char* text, size_t size,
                                 uint32_t* len) {
  const char* s = "[object]";
  if(jello_is_null(v)) s = "null";
  else if(jello_is_bool(v)) s = jello_as_bool(v) ? "true" : "false";
  else if(jello_is_i32(v)) {
    snprintf(text, size, "%d", (int)jello_as_i32(v));
    s = text;
  } else if(jello_is_atom(v)) {
    const char* name = jdl_atom_name(c, jello_as_atom(v));
    if(name) s = name;
  } else if(jello_is_box_i64(v)) {
    snprintf(text, size, "%lld", (long long)jello_as_box_i64(v));
    s = text;
  } else if(jello_is_box_f64(v)) {
    std_float_text(text, size, jello_as_box_f64(v), 64);
    s = text;
  } else if(jello_is_box_f32(v)) {
    std_float_text(text, size, jello_as_box_f32(v), 32);
    s = text;
  } else if(jello_is_box_f16(v)) {
    std_float_text(text, size, std_half_value(jello_as_box_f16(v)), 16);
    s = text;
  } else if(std_is_heap(v, JELLO_OBJ_BYTES)) {
    const jello_bytes* b = (const jello_bytes*)jello_as_ptr(v);
    *len = b->length;
    return b->data;
  }
  *len = (uint32_t)strlen(s);
  return (const uint8_t*)s;
}

void std_to_string(jdlo_ctx* c) {
  char text[40];
  uint32_t len;
  const uint8_t* s = value_text(c, jdl_arg_value(c, 0), text, sizeof text, &len);
  jdl_return_bytes_copy(c, s, len);
}

void std_u64_string(jdlo_ctx* c) {
  char text[24];
  snprintf(text, sizeof text, "%llu", (unsigned long long)jdl_arg_i64(c, 0));
  jdl_return_bytes_copy(c, (const uint8_t*)text, (uint32_t)strlen(text));
}

void std_bytes_u8(jdlo_ctx* c) {
  int32_t v = jdl_arg_i32(c, 0);
  if(v < 0 || v > 255) {
    jdl_fail(c, "value out of range");
    return;
  }
  uint8_t b = (uint8_t)v;
  jdl_return_bytes_copy(c, &b, 1);
}

static uint32_t quote_into(uint8_t* out, const uint8_t* s, uint32_t len) {
  static const char HEX[] = "0123456789abcdef";
  uint32_t n = 0;
  out[n++] = '"';
  for(uint32_t i = 0; i < len; i++) {
    uint8_t b = s[i];
    char escape = b == '\\'   ? '\\'
                  : b == '"'  ? '"'
                  : b == '\n' ? 'n'
                  : b == '\r' ? 'r'
                  : b == '\t' ? 't'
                  : b == 0    ? '0'
                              : 0;
    if(escape) {
      out[n++] = '\\';
      out[n++] = (uint8_t)escape;
    } else if(b < 0x20 || b == 0x7f) {
      out[n++] = '\\';
      out[n++] = 'x';
      out[n++] = (uint8_t)HEX[b >> 4];
      out[n++] = (uint8_t)HEX[b & 15];
    } else out[n++] = b;
  }
  out[n++] = '"';
  return n;
}

void std_quote(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  /* Each byte takes at most 4 (\xHH), and the result's length is 32 bits. */
  uint8_t* out = len > (UINT32_MAX - 2) / 4 ? NULL : malloc((size_t)len * 4 + 2);
  if(!out) {
    jdl_fail(c, "out of memory");
    return;
  }
  uint32_t n = quote_into(out, s, len);
  jdl_return_bytes_copy(c, out, n);
  free(out);
}

typedef struct text_buffer {
  uint8_t* data;
  size_t len, cap;
  int failed;
} text_buffer;

static uint8_t* reserve(text_buffer* t, size_t more) {
  if(t->failed) return NULL;
  if(more > UINT32_MAX - t->len) {
    t->failed = 1;
    return NULL;
  }
  if(t->len + more > t->cap) {
    size_t cap = t->cap ? t->cap : 64;
    while(cap < t->len + more) cap *= 2;
    uint8_t* data = realloc(t->data, cap);
    if(!data) {
      t->failed = 1;
      return NULL;
    }
    t->data = data;
    t->cap = cap;
  }
  return t->data + t->len;
}

static void append(text_buffer* t, const void* s, size_t len) {
  uint8_t* end = reserve(t, len);
  if(!end) return;
  memcpy(end, s, len);
  t->len += len;
}

#define OBJECT_DEPTH 32

typedef struct object_path {
  const jello_object* objects[OBJECT_DEPTH];
  int depth;
} object_path;

static void field_text(jdlo_ctx* c, text_buffer* t, jello_value v, object_path* path) {
  if(std_is_heap(v, JELLO_OBJ_OBJECT)) {
    const jello_object* o = (const jello_object*)jello_as_ptr(v);
    int seen = path->depth >= OBJECT_DEPTH;
    for(int i = 0; i < path->depth && !seen; i++) seen = path->objects[i] == o;
    if(seen) {
      append(t, "{...}", 5);
      return;
    }
    uint32_t* keys = malloc(((size_t)o->len + 1u) * sizeof *keys);
    if(!keys) {
      t->failed = 1;
      return;
    }
    /* Prototypes are enabled when atom 0 is __proto__. */
    const char* first = jdl_atom_name(c, 0);
    uint32_t n = jdl_object_keys(o, first && strcmp(first, "__proto__") == 0, keys);
    append(t, "{", 1);
    for(uint32_t i = 0; i < n; i++) {
      const char* name = jdl_atom_name(c, keys[i]);
      if(i) append(t, ", ", 2);
      append(t, name, strlen(name));
      append(t, ": ", 2);
      path->objects[path->depth++] = o;
      field_text(c, t, jdl_object_get(o, keys[i]), path);
      path->depth--;
    }
    append(t, "}", 1);
    free(keys);
    return;
  }
  char text[40];
  uint32_t len;
  const uint8_t* s = value_text(c, v, text, sizeof text, &len);
  if(jello_is_atom(v)) append(t, ":", 1);
  if(!std_is_heap(v, JELLO_OBJ_BYTES)) {
    append(t, s, len);
    return;
  }
  uint8_t* end = reserve(t, (size_t)len * 4 + 2);
  if(end) t->len += quote_into(end, s, len);
}

void std_object_string(jdlo_ctx* c) {
  jello_value v = jdl_arg_value(c, 0);
  text_buffer t = {NULL, 0, 0, 0};
  object_path path = {{NULL}, 0};
  field_text(c, &t, v, &path);
  if(t.failed) {
    free(t.data);
    jdl_fail(c, "out of memory");
    return;
  }
  jdl_return_bytes_copy(c, t.data, (uint32_t)t.len);
  free(t.data);
}
