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

static void* shallow_copy(copier* c, void* p) {
  if(vm_is_immortal(p)) return p;
  jello_vm* vm = c->vm;
  const jello_bc_module* m = c->m;
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_BYTES: return vm_bytes_clone(vm, (const jello_bytes*)p);
    case JELLO_OBJ_BOX_I64:
    case JELLO_OBJ_BOX_F64:
    case JELLO_OBJ_BOX_F32:
    case JELLO_OBJ_BOX_F16: return jello_as_ptr(vm_clone_numbox(vm, jello_from_ptr(p)));
    case JELLO_OBJ_STRUCT: {
      jello_struct* n = jello_struct_new(vm, m, oh->type_id);
      memcpy(n->fields, ((const jello_struct*)p)->fields, vm_struct_fields_bytes(m, oh->type_id));
      return n;
    }
    case JELLO_OBJ_OBJECT: return vm_object_clone(vm, (const jello_object*)p);
    case JELLO_OBJ_ARRAY: {
      const jello_array* a = (const jello_array*)p;
      jello_array* n = jello_array_new(vm, m, oh->type_id, a->length);
      jello_type_kind ek = m->types[m->types[oh->type_id].as.unary.elem].kind;
      if(a->length) memcpy(n->data, a->data, (size_t)a->length * jello_slot_size(ek));
      return n;
    }
    case JELLO_OBJ_LIST: {
      const jello_list* l = (const jello_list*)p;
      return jello_list_cons(vm, oh->type_id, l->head, l->tail);
    }
    case JELLO_OBJ_ENUM: {
      const jello_enum* e = (const jello_enum*)p;
      return jello_enum_new(vm, oh->type_id, e->tag, e->nfields, e->fields);
    }
    case JELLO_OBJ_HANDLE: {
      const jello_handle* hd = (const jello_handle*)p;
      if(c->moves && hd->cls == &proc_socket_class) proc_socket_note(&c->sockets, hd->payload);
      if(!hd->cls->retain) {
        c->refused |= !c->drop;
        return c->drop ? NULL : p;
      }
      hd->cls->retain(hd->payload);
      return jello_handle_new(vm, hd->cls, hd->payload, hd->native_bytes);
    }
    case JELLO_OBJ_FUNCTION: {
      const jello_function* f = (const jello_function*)p;
      jello_function* n = jello_function_new(vm, f->func_index, f->ncaps);
      n->h.type_id = oh->type_id;
      memcpy(n->captures, f->captures, sizeof(uint64_t) * f->ncaps);
      return n;
    }
    default: abort(); /* Shared, it could outlive its space while the copy reaches it. */
  }
}

static void* copy_ptr(copier* c, void* p) {
  if(!p) return NULL;
  void* n = map_get(c, p);
  if(n) return n;
  n = shallow_copy(c, p);
  if(n == p || !n) return n; /* Shared, or dropped; shallow_copy decides again at each visit. */
  map_put(c, p, n);
  if(!leaf(n)) work_push(c, n);
  return n;
}

static void copy_slot(copier* c, void* slot, int dynamic) {
  if(dynamic) {
    jello_value v;
    memcpy(&v, slot, sizeof v);
    if(!jello_is_ptr(v)) return;
    void* n = copy_ptr(c, jello_as_ptr(v));
    v = n ? jello_from_ptr(n) : jello_make_null(); /* A handle dropped is null. */
    memcpy(slot, &v, sizeof v);
  } else {
    void* p;
    memcpy(&p, slot, sizeof p);
    p = copy_ptr(c, p);
    memcpy(slot, &p, sizeof p);
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

static uint32_t global_holding(copier* c, jello_object* proto) {
  for(uint32_t i = 0; i < c->found_len; i++) {
    if(c->found[i].object == proto) return c->found[i].global;
  }
  const jello_bc_module* m = c->m;
  uint32_t g = UINT32_MAX;
  for(uint32_t j = 0; m && j < m->nptr_globals && g == UINT32_MAX; j++) {
    if(vm_global_ptr(m, c->vm->globals, m->ptr_globals[j]) == proto) g = m->ptr_globals[j];
  }
  c->found = (struct vm_proto_fix*)grown(c->found, c->found_len, &c->found_cap, 0,
                                         sizeof(struct vm_proto_fix), 4u);
  c->found[c->found_len++] = (struct vm_proto_fix){proto, g};
  return g;
}

static jello_object* copy_proto(copier* c, jello_object* o) {
  uint32_t g = c->out ? global_holding(c, o->proto) : UINT32_MAX;
  if(g == UINT32_MAX) return (jello_object*)copy_ptr(c, o->proto);
  uint32_t len = c->fixes ? c->fixes->len : 0u;
  c->fixes = (vm_protos*)grown(c->fixes, len, &c->fixes_cap, sizeof(vm_protos),
                               sizeof(struct vm_proto_fix), 8u);
  c->fixes->at[len] = (struct vm_proto_fix){o, g};
  c->fixes->len = len + 1u;
  return NULL;
}

static void copy_refs(copier* c, void* p) {
  const jello_bc_module* m = c->m;
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_STRUCT: {
      jello_struct* s = (jello_struct*)p;
      const jello_struct_sig* sig = &m->struct_sigs[oh->type_id];
      for(uint16_t i = 0; i < sig->nfields; i++) {
        jello_type_kind fk = m->types[sig->field_type[i]].kind;
        if(!vm_kind_needs_gc_scan(fk)) continue;
        copy_slot(c, s->fields + sig->field_offset[i], fk == JELLO_T_DYNAMIC);
      }
      return;
    }
    case JELLO_OBJ_OBJECT: {
      jello_object* o = (jello_object*)p;
      jello_value* link = NULL;
      for(uint32_t i = 0; i < o->cap; i++) {
        if(!vm_obj_slot_live(o->states[i])) continue;
        if(o->proto && o->keys[i] == JELLO_ATOM___PROTO__) link = &o->vals[i];
        else copy_slot(c, &o->vals[i], 1);
      }
      if(o->proto) set_proto(o, link, copy_proto(c, o));
      return;
    }
    case JELLO_OBJ_ARRAY: {
      jello_array* a = (jello_array*)p;
      jello_type_kind ek = m->types[m->types[oh->type_id].as.unary.elem].kind;
      if(!vm_kind_needs_gc_scan(ek)) return;
      size_t size = jello_slot_size(ek);
      for(uint32_t i = 0; i < a->length; i++) {
        copy_slot(c, (uint8_t*)a->data + i * size, ek == JELLO_T_DYNAMIC);
      }
      return;
    }
    case JELLO_OBJ_LIST: {
      jello_list* l = (jello_list*)p;
      copy_slot(c, &l->head, 1);
      copy_slot(c, &l->tail, 0);
      return;
    }
    case JELLO_OBJ_ENUM: {
      jello_enum* e = (jello_enum*)p;
      for(uint32_t i = 0; i < e->nfields; i++) copy_slot(c, &e->fields[i], 1);
      return;
    }
    case JELLO_OBJ_FUNCTION: {
      jello_function* f = (jello_function*)p;
      const jello_bc_function* callee = &m->funcs[f->func_index];
      for(uint32_t i = 0; i < f->ncaps; i++) {
        jello_type_kind ck = vm_reg_kind(m, callee, callee->cap_start + i);
        if(vm_kind_needs_gc_scan(ck)) copy_slot(c, &f->captures[i], ck == JELLO_T_DYNAMIC);
      }
      return;
    }
    default: return;
  }
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

static jello_value copy_root(copier* c, jello_value v) {
  if(!jello_is_ptr(v) || !jello_as_ptr(v)) return v;
  void* root = copy_ptr(c, jello_as_ptr(v));
  if(!root) return jello_make_null(); /* A handle dropped. */
  while(c->work_len) copy_refs(c, c->work[--c->work_len]);
  return jello_from_ptr(root);
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

static jello_trap_code copy_end(copier* c, vm_parcel* out) {
  c->vm->parcel = NULL;
  if(c->sockets) proc_sockets_pack(c->refused ? NULL : c->vm, &c->sockets);
  out->sockets = c->sockets;
  out->protos = c->fixes;
  c->fixes = NULL;
  copier_free(c);
  if(!c->refused) return JELLO_TRAP_NONE;
  vm_parcel_free(out);
  return JELLO_TRAP_UNSENDABLE;
}

static void fix_proto(const jello_vm* vm, jello_object* o, uint32_t g) {
  void* p = vm_global_ptr(vm->running_module, vm->globals, g);
  jello_object* proto = p && ((const jello_obj_header*)p)->kind == JELLO_OBJ_OBJECT ? p : NULL;
  set_proto(o, proto_slot(o), proto);
}

jello_trap_code vm_parcel_pack(jello_vm* vm, jello_value v, int drop, vm_parcel* out) {
  if(!jello_is_ptr(v) || !jello_as_ptr(v)) { /* Spares a scalar setting up a copier. */
    *out = vm_parcel_empty();
    out->value = v;
    return JELLO_TRAP_NONE;
  }
  copier c;
  copy_begin(&c, vm, 1, drop, out);
  c.moves = !drop;
  out->value = copy_root(&c, v);
  return copy_end(&c, out);
}

static void fix_protos(jello_vm* vm, const vm_protos* protos, const copier* c) {
  for(uint32_t i = 0; protos && i < protos->len; i++) {
    jello_object* o = protos->at[i].object;
    if(c) o = (jello_object*)map_get(c, o);
    if(o) fix_proto(vm, o, protos->at[i].global);
  }
}

jello_value vm_parcel_take(jello_vm* vm, vm_parcel* pc) {
  vm_parcel_splice(vm, vm->space, pc);
  fix_protos(vm, pc->protos, NULL);
  jello_value v = pc->value;
  if(pc->sockets) proc_sockets_take(vm->proc, &pc->sockets);
  vm_parcel_free(pc); /* Its objects are the space's now; this frees the prototype list. */
  return v;
}

jello_value vm_parcel_read(jello_vm* vm, vm_parcel* pc) {
  copier c;
  vm_parcel copy;
  copy_begin(&c, vm, 0, 0, &copy);
  copy.value = copy_root(&c, pc->value);
  fix_protos(vm, pc->protos, &c);
  copy_end(&c, &copy); /* Never refuses: a parcel holds only what may leave. */
  vm_parcel_splice(vm, vm->space, &copy);
  proc_sockets_take(vm->proc, &pc->sockets);
  return copy.value;
}

jello_trap_code vm_copy_start(jello_vm* vm, jello_space* into, void** root, uint64_t* globals,
                              struct proc_socket_list** sockets) {
  copier c;
  vm_parcel copy;
  copy_begin(&c, vm, 0, 0, &copy);
  c.moves = 1;
  copy_slot(&c, root, 0);
  while(c.work_len) copy_refs(&c, c.work[--c.work_len]);
  if(c.sockets) proc_sockets_pack(c.refused ? NULL : vm, &c.sockets);
  *sockets = c.sockets;
  c.sockets = NULL;
  c.moves = 0;
  c.drop = 1;
  for(uint32_t j = 0; !c.refused && c.m && j < c.m->nptr_globals; j++) {
    uint32_t i = c.m->ptr_globals[j];
    copy_slot(&c, &globals[i], vm_global_kind(c.m, i) == JELLO_T_DYNAMIC);
  }
  while(c.work_len) copy_refs(&c, c.work[--c.work_len]);
  jello_trap_code trap = copy_end(&c, &copy);
  if(!trap) vm_parcel_splice(vm, into, &copy);
  return trap;
}
