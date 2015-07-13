// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdlib.h>
#include <string.h>

/* -- Object: an open-addressing, tombstone-supporting hash table -- */

static uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static size_t table_bytes(uint32_t cap) {
  size_t keys = (size_t)cap * sizeof(uint32_t);
  size_t align = sizeof(jello_value);
  size_t pad = (align - (keys % align)) % align;
  size_t vals = (size_t)cap * sizeof(jello_value);
  size_t states = (size_t)cap;
  return keys + pad + vals + states;
}

static void bind_table(jello_object* o, uint8_t* base, uint32_t cap) {
  size_t keys = (size_t)cap * sizeof(uint32_t);
  size_t align = sizeof(jello_value);
  size_t pad = (align - (keys % align)) % align;
  o->keys = (uint32_t*)base;
  o->vals = (jello_value*)(base + keys + pad);
  o->states = base + keys + pad + (size_t)cap * sizeof(jello_value);
  o->cap = cap;
}

jello_object* jello_object_new(jello_vm* vm, uint32_t type_id) {
  jello_object* o = (jello_object*)jello_gc_alloc(vm, sizeof(jello_object));
  o->h.kind = (uint8_t)JELLO_OBJ_OBJECT;
  o->h.type_id = type_id;
  o->proto = NULL;
  o->len = 0;
  o->tombstones = 0;
  uint8_t* block = (uint8_t*)malloc(table_bytes(JELLO_OBJECT_INIT_CAP));
  if(!block) abort();
  bind_table(o, block, JELLO_OBJECT_INIT_CAP);
  memset(o->states, 0, o->cap);
  return o;
}

static uint32_t find_slot(jello_object* o, uint32_t atom_id, int* found) {
  if(o->len == 0u) {
    *found = 0;
    return hash_u32(atom_id) & (o->cap - 1u);
  }
  uint32_t mask = o->cap - 1u;
  uint32_t i = hash_u32(atom_id) & mask;
  uint32_t first_tomb = UINT32_MAX;
  for(;;) {
    uint8_t st = o->states[i];
    if(st == JELLO_OBJ_SLOT_EMPTY) {
      *found = 0;
      return (first_tomb != UINT32_MAX) ? first_tomb : i;
    }
    if(st == JELLO_OBJ_SLOT_OCCUPIED && o->keys[i] == atom_id) {
      *found = 1;
      return i;
    }
    if(st == JELLO_OBJ_SLOT_TOMB && first_tomb == UINT32_MAX) first_tomb = i;
    i = (i + 1u) & mask;
  }
}

static uint32_t find_occupied_slot(const jello_object* o, uint32_t atom_id, int* found) {
  if(o->len == 0u) { *found = 0; return 0; }
  uint32_t mask = o->cap - 1u;
  uint32_t i = hash_u32(atom_id) & mask;
  for(;;) {
    uint8_t st = o->states[i];
    if(st == JELLO_OBJ_SLOT_EMPTY) { *found = 0; return 0; }
    if(st == JELLO_OBJ_SLOT_OCCUPIED && o->keys[i] == atom_id) { *found = 1; return i; }
    i = (i + 1u) & mask;
  }
}

static void rehash(jello_object* o, uint32_t new_cap) {
  uint32_t* old_keys = o->keys;
  jello_value* old_vals = o->vals;
  uint8_t* old_states = o->states;
  uint32_t old_cap = o->cap;

  uint8_t* block = (uint8_t*)malloc(table_bytes(new_cap));
  if(!block) abort();
  bind_table(o, block, new_cap);
  memset(o->states, 0, new_cap);
  o->len = 0;
  o->tombstones = 0;

  for(uint32_t i = 0; i < old_cap; i++) {
    if(old_states[i] != JELLO_OBJ_SLOT_OCCUPIED) continue;
    int found = 0;
    uint32_t slot = find_slot(o, old_keys[i], &found);
    o->keys[slot] = old_keys[i];
    o->states[slot] = JELLO_OBJ_SLOT_OCCUPIED;
    o->vals[slot] = old_vals[i];
    o->len++;
  }
  free(old_keys);
}

static void ensure_cap(jello_object* o) {
  if((o->len + 1u) * JELLO_OBJECT_LOAD_NUM >= o->cap * JELLO_OBJECT_LOAD_DEN) {
    rehash(o, o->cap * 2u);
    return;
  }
  if((o->len + o->tombstones + 1u) * JELLO_OBJECT_LOAD_NUM >= o->cap * JELLO_OBJECT_LOAD_DEN) {
    rehash(o, o->cap);
  }
}

int jello_object_has(const jello_object* o, uint32_t atom_id) {
  int found = 0;
  (void)find_occupied_slot(o, atom_id, &found);
  return found;
}

jello_value jello_object_get(const jello_object* o, uint32_t atom_id) {
  int found = 0;
  uint32_t i = find_occupied_slot(o, atom_id, &found);
  if(!found) return jello_make_null();
  return o->vals[i];
}

void jello_object_set(jello_object* o, uint32_t atom_id, jello_value v) {
  ensure_cap(o);
  int found = 0;
  uint32_t i = find_slot(o, atom_id, &found);
  if(!found) {
    o->keys[i] = atom_id;
    o->states[i] = JELLO_OBJ_SLOT_OCCUPIED;
    o->len++;
  }
  o->vals[i] = v;
}

int jello_object_remove(jello_object* o, uint32_t atom_id) {
  int found = 0;
  uint32_t i = find_slot(o, atom_id, &found);
  if(!found) return 0;
  o->states[i] = JELLO_OBJ_SLOT_TOMB;
  o->len--;
  o->tombstones++;
  return 1;
}

void jello_object_clear(jello_object* o) {
  memset(o->states, 0, o->cap);
  o->len = 0;
  o->tombstones = 0;
}

jello_object* jello_object_copy(jello_vm* vm, const jello_object* src) {
  jello_object* dst = jello_object_new(vm, src->h.type_id);
  for(uint32_t i = 0; i < src->cap; i++) {
    if(src->states[i] == JELLO_OBJ_SLOT_OCCUPIED) {
      jello_object_set(dst, src->keys[i], src->vals[i]);
    }
  }
  dst->proto = src->proto;
  return dst;
}
