// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Deep-copies an object graph for message passing or process creation, 
 * ensuring complete memory isolation between sender and receiver.
 *
 * - Preserves object sharing and cycles while ensuring neither process sees
 *   the other's writes.
 * - Copies boxes, buffers, and small bytes. Slices are copied  only for active 
 *   content. Large bytes reside in shared, reference-counted binaries. 
 *   Immortal objects (CONST_FUN outputs, byte literals) belong to no heap.
 * - Native handles require a class `retain` method to cross processes
 *   (otherwise rejected or nulled). Sockets transfer ownership to the receiver
 *   or read as closed.
 * - Process-specific prototypes and globals are copied or mapped  based on
 *   whether the target is a new process or an existing one.
 * - Uses an iterative worklist (non-recursive) and temporary parcels requiring
 *   no garbage collection roots until fully adopted by the receiver.
 */

#include <jello/internal.h>
#include <jello/internal/proc_internal.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define COPY_INLINE 16u

typedef struct copier {
  jello_vm* vm;
  const jello_bc_module* m;
  int out;
  int drop;
  int moves;
  struct proc_socket_list* sockets;
  int refused;
  vm_protos* fixes;
  uint32_t fixes_cap;
  struct vm_proto_fix* found;
  uint32_t found_len;
  uint32_t found_cap;
  void** keys;
  void** vals;
  uint32_t cap;
  uint32_t len;
  uint32_t shift;
  void** work;
  uint32_t work_len;
  uint32_t work_cap;
  void* keys_inline[2u * COPY_INLINE];
  void* vals_inline[2u * COPY_INLINE];
  void* work_inline[COPY_INLINE];
} copier;

static uint32_t slot_of(const copier* c, const void* p) {
  return (uint32_t)(((uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull) >> c->shift);
}

static void map_insert(copier* c, void* from, void* to) {
  uint32_t i = slot_of(c, from);
  while(c->keys[i]) i = (i + 1u) & (c->cap - 1u);
  c->keys[i] = from;
  c->vals[i] = to;
  c->len++;
}

static void map_grow(copier* c) {
  void** keys = c->keys;
  void** vals = c->vals;
  uint32_t cap = c->cap;
  c->len = 0;
  c->cap = cap * 2u;
  c->shift--;
  c->keys = (void**)calloc(c->cap, sizeof(void*));
  c->vals = (void**)malloc(sizeof(void*) * c->cap);
  if(!c->keys || !c->vals) abort();
  for(uint32_t i = 0; i < cap; i++) {
    if(keys[i]) map_insert(c, keys[i], vals[i]);
  }
  if(keys != c->keys_inline) {
    free(keys);
    free(vals);
  }
}

static void map_put(copier* c, void* from, void* to) {
  if(2u * (c->len + 1u) > c->cap) map_grow(c);
  map_insert(c, from, to);
}

static void* map_get(const copier* c, const void* from) {
  for(uint32_t i = slot_of(c, from); c->keys[i]; i = (i + 1u) & (c->cap - 1u)) {
    if(c->keys[i] == from) return c->vals[i];
  }
  return NULL;
}

static void* grown(void* p, uint32_t len, uint32_t* cap, size_t header, size_t elem,
                   uint32_t first) {
  if(p && len < *cap) return p;
  *cap = *cap ? *cap * 2u : first;
  p = realloc(p, header + elem * *cap);
  if(!p) abort();
  return p;
}

static void work_push(copier* c, void* p) {
  if(c->work_len == c->work_cap) {
    void** old = c->work;
    int was_inline = old == c->work_inline;
    c->work =
        (void**)grown(was_inline ? NULL : old, c->work_len, &c->work_cap, 0, sizeof(void*), 0);
    if(was_inline) memcpy(c->work, old, sizeof c->work_inline);
  }
  c->work[c->work_len++] = p;
}

static int leaf(const void* p) {
  switch((jello_obj_kind)((const jello_obj_header*)p)->kind) {
    case JELLO_OBJ_BYTES:
    case JELLO_OBJ_BOX_I64:
    case JELLO_OBJ_BOX_F64:
    case JELLO_OBJ_BOX_F32:
    case JELLO_OBJ_BOX_F16:
    case JELLO_OBJ_HANDLE: return 1;
    default: return 0;
  }
}

static jello_value* proto_slot(jello_object* o) {
  for(uint32_t i = 0; i < o->cap; i++) {
    if(vm_obj_slot_live(o->states[i]) && o->keys[i] == JELLO_ATOM___PROTO__) return &o->vals[i];
  }
  return NULL;
}

static void set_proto(jello_object* o, jello_value* slot, jello_object* proto) {
  if(slot) *slot = proto ? jello_from_ptr(proto) : jello_make_null();
  o->proto = proto;
}

static void copier_free(copier* c) {
  if(c->keys != c->keys_inline) {
    free(c->keys);
    free(c->vals);
  }
  if(c->work != c->work_inline) free(c->work);
  free(c->fixes);
  free(c->found);
}

static void copy_begin(copier* c, jello_vm* vm, int pack, int drop, vm_parcel* out) {
  memset(c, 0, offsetof(copier, keys_inline));
  c->vm = vm;
  c->m = vm->running_module;
  c->out = pack;
  c->drop = drop;
  memset(c->keys_inline, 0, sizeof c->keys_inline);
  c->keys = c->keys_inline;
  c->vals = c->vals_inline;
  c->cap = 2u * COPY_INLINE;
  c->shift = 64u - (uint32_t)__builtin_ctz(2u * COPY_INLINE);
  c->work = c->work_inline;
  c->work_cap = COPY_INLINE;
  *out = vm_parcel_empty();
  vm->parcel = out;
}

static void fix_proto(const jello_vm* vm, jello_object* o, uint32_t g) {
  void* p = vm_global_ptr(vm->running_module, vm->globals, g);
  jello_object* proto = p && ((const jello_obj_header*)p)->kind == JELLO_OBJ_OBJECT ? p : NULL;
  set_proto(o, proto_slot(o), proto);
}
