// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The garbage collector; a non-moving mark-and-sweep collector over the spaces of a heap shared
 * by threads, stopping the world only for the heap's own space, with size-class free lists, plus
 * the constructors for Bytes, numeric boxes, arrays, functions, structs and native handles. */

#include <jello/internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/jdll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

static const uint32_t GC_SIZE_CLASSES[GC_NUM_SIZE_CLASSES] = {
  32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u,
};

static jello_gc_hdr* hdr_of(void* obj) {
  return (jello_gc_hdr*)((uint8_t*)obj - sizeof(jello_gc_hdr));
}
static void* obj_of(jello_gc_hdr* h) {
  return (void*)((uint8_t*)h + sizeof(jello_gc_hdr));
}

static int size_class_index(uint32_t total) {
  for(int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
    if(total <= GC_SIZE_CLASSES[i]) return i;
  }
  return -1;
}

static void destroy_object(void* p);

#define GC_INITIAL_THRESHOLD (1024u * 1024u) /* 1MB: a round, generous first-collection budget */

typedef struct shared_head {
  jdl_refs refs;
  jello_gc_hdr hdr;
} shared_head;

static _Atomic uint32_t shared_live;

uint32_t vm_shared_binaries_live(void) {
  return atomic_load_explicit(&shared_live, memory_order_relaxed);
}

/** The bytes a shared binary of `length` bytes takes, its block's headers included. */
static size_t shared_size(uint32_t length) {
  return sizeof(shared_head) + sizeof(jello_bytes) + (size_t)length;
}

static shared_head* shared_head_of(jello_bytes* bin) {
  return (shared_head*)((uint8_t*)hdr_of(bin) - offsetof(shared_head, hdr));
}

/** What a Bytes holding shared binary `bin` counts live for it: the binary's whole block. */
static uint32_t shared_native(const jello_bytes* bin) {
  size_t n = shared_size(bin->length);
  return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

/** Drops a reference to shared binary `bin`, freeing it with the last. One that sees itself the
 * only holder frees it without the atomic update, as nobody else can take a reference. */
static void shared_release(jello_bytes* bin) {
  shared_head* s = shared_head_of(bin);
  if(__atomic_load_n(&s->refs, __ATOMIC_ACQUIRE) != 1u && !jdl_release(s)) return;
  atomic_fetch_sub_explicit(&shared_live, 1u, memory_order_relaxed);
  free(s);
}

/** Live bytes before the first collection, and the least the threshold ever falls to. */
#define GC_MIN_THRESHOLD (4u * 1024u * 1024u)
/** The same for a process's space, smaller, as there may be many. */
#define GC_PROCESS_MIN_THRESHOLD (64u * 1024u)

void jello_gc_init(jello_vm* vm) {
  if(!vm || !vm->heap) return;
  if(vm->heap->gc_next_collect == 0) vm->heap->gc_next_collect = GC_INITIAL_THRESHOLD;
}

void jello_gc_shutdown(jello_vm* vm) {
  if(!vm || !vm->heap) return;
  jello_heap* heap = vm->heap;
  jello_gc_hdr* h = heap->gc_objects;
  while(h) {
    jello_gc_hdr* next = h->next;
    destroy_object(obj_of(h));
    free(h);
    h = next;
  }
  heap->gc_objects = NULL;
  heap->gc_bytes_live = 0;
  for(int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
    jello_gc_hdr* fh = heap->gc_free_classes[i];
    while(fh) {
      jello_gc_hdr* next = fh->next;
      free(fh);
      fh = next;
    }
    heap->gc_free_classes[i] = NULL;
  }
  free(heap->gc_roots);
  heap->gc_roots = NULL;
  heap->gc_roots_len = 0;
  heap->gc_roots_cap = 0;
}

void jello_gc_push_root(jello_vm* vm, void* obj) {
  if(!vm) abort();
  jello_heap* heap = vm->heap;
  pthread_mutex_lock(&heap->alloc_lock);
  if(heap->gc_roots_len == heap->gc_roots_cap) {
    uint32_t ncap = heap->gc_roots_cap ? heap->gc_roots_cap * 2u : 32u;
    void** nv = (void**)realloc(heap->gc_roots, sizeof(void*) * (size_t)ncap);
    if(!nv) abort();
    heap->gc_roots = nv;
    heap->gc_roots_cap = ncap;
  }
  heap->gc_roots[heap->gc_roots_len++] = obj;
  pthread_mutex_unlock(&heap->alloc_lock);
}

void jello_gc_pop_roots(jello_vm* vm, uint32_t n) {
  if(!vm) abort();
  jello_heap* heap = vm->heap;
  pthread_mutex_lock(&heap->alloc_lock);
  if(n > heap->gc_roots_len) {
    fprintf(stderr, "DIAG: gc_pop_roots underflow n=%u len=%u\n", n, heap->gc_roots_len);
    fflush(stderr);
    abort();
  }
  heap->gc_roots_len -= n;
  pthread_mutex_unlock(&heap->alloc_lock);
}

void jello_vm_gc_stats(const jello_vm* vm, uint32_t* out_live_bytes, uint32_t* out_collections,
                        double* out_pause_ms) {
  if(!vm || !vm->heap) return;
  if(out_live_bytes) *out_live_bytes = vm->heap->gc_bytes_live;
  if(out_collections) *out_collections = vm->heap->gc_collections;
  if(out_pause_ms) *out_pause_ms = vm->heap->gc_pause_ms;
}

static void out_of_memory(size_t size) {
  fprintf(stderr, "jello: out of memory: cannot allocate %zu bytes\n", size);
  abort();
}

static jello_gc_hdr* free_class_pop(jello_space* space, int ci) {
  jello_gc_hdr* h = ci >= 0 ? space->free_classes[ci] : NULL;
  if(h) space->free_classes[ci] = h->next;
  return h;
}

static void space_lock(jello_vm* vm, int shared) {
  if(shared) vm_heap_lock(vm);
}

static void space_unlock(jello_vm* vm, int shared) {
  if(shared) pthread_mutex_unlock(&vm->heap->alloc_lock);
}

void vm_parcel_join(vm_parcel* into, vm_parcel* from) {
  if(from->objects) {
    from->tail->next = into->objects;
    if(!into->objects) into->tail = from->tail;
    into->objects = from->objects;
    into->bytes += from->bytes;
  }
  if(from->sockets) proc_sockets_join(&into->sockets, &from->sockets);
  free(from->protos);
  *from = vm_parcel_empty();
}

void* jello_gc_alloc(jello_vm* vm, size_t size) {
  if(!vm) abort();
  vm_safepoint_check(vm);
  jello_heap* heap = vm->heap;
  uint32_t requested = (uint32_t)(sizeof(jello_gc_hdr) + size);

  pthread_mutex_lock(&heap->alloc_lock);
  int should_collect = heap->gc_next_collect && heap->gc_bytes_live >= heap->gc_next_collect;
  pthread_mutex_unlock(&heap->alloc_lock);
  if(should_collect) jello_gc_collect(vm);

  int ci = size_class_index(requested);
  uint32_t total = (ci >= 0) ? GC_SIZE_CLASSES[ci] : requested;
  jello_gc_hdr* h = NULL;

  pthread_mutex_lock(&heap->alloc_lock);
  if(ci >= 0 && heap->gc_free_classes[ci]) {
    h = heap->gc_free_classes[ci];
    heap->gc_free_classes[ci] = h->next;
  }
  pthread_mutex_unlock(&heap->alloc_lock);

  if(!h) {
    h = (jello_gc_hdr*)malloc(total);
    if(!h) abort();
  }
  h->total_size = total;
  h->marked = 0;

  pthread_mutex_lock(&heap->alloc_lock);
  h->next = heap->gc_objects;
  heap->gc_objects = h;
  heap->gc_bytes_live += total;
  pthread_mutex_unlock(&heap->alloc_lock);

  return obj_of(h);
}

typedef struct mark_stack {
  void** items;
  uint32_t len;
  uint32_t cap;
  uint32_t marked; /**< The objects it has marked, every one of which the sweep must find. */
} mark_stack;

static void ms_push(mark_stack* ms, void* p) {
  if(ms->len == ms->cap) {
    uint32_t ncap = ms->cap ? ms->cap * 2u : 64u;
    void** ni = (void**)realloc(ms->items, sizeof(void*) * (size_t)ncap);
    if(!ni) abort();
    ms->items = ni;
    ms->cap = ncap;
  }
  ms->items[ms->len++] = p;
}

static void* ms_pop(mark_stack* ms) {
  if(ms->len == 0) return NULL;
  return ms->items[--ms->len];
}

static void mark_ptr(mark_stack* ms, void* p) {
  if(!p) return;
  jello_gc_hdr* h = hdr_of(p);
  if(h->marked) return;
  h->marked = 1;
  ms->marked++;
  ms_push(ms, p);
}

static void trace_object(mark_stack* ms, const jello_bc_module* m, void* p) {
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_BYTES:
    case JELLO_OBJ_BOX_I64:
    case JELLO_OBJ_BOX_F64:
    case JELLO_OBJ_BOX_F32:
    case JELLO_OBJ_BOX_F16:
      return;
    case JELLO_OBJ_STRUCT: {
      const jello_struct* s = (const jello_struct*)p;
      const jello_struct_sig* sig = &m->struct_sigs[oh->type_id];
      for(uint16_t i = 0; i < sig->nfields; i++) {
        jello_type_kind fk = m->types[sig->field_type[i]].kind;
        if(fk == JELLO_T_DYNAMIC) {
          jello_value v;
          memcpy(&v, s->fields + sig->field_offset[i], sizeof(jello_value));
          if(jello_is_ptr(v)) mark_ptr(ms, jello_as_ptr(v));
        } else if(vm_kind_is_gc_pointer(fk)) {
          void* fp;
          memcpy(&fp, s->fields + sig->field_offset[i], sizeof(void*));
          mark_ptr(ms, fp);
        }
      }
      return;
    }
    case JELLO_OBJ_OBJECT: {
      const jello_object* o = (const jello_object*)p;
      if(o->proto) mark_ptr(ms, o->proto);
      for(uint32_t i = 0; i < o->cap; i++) {
        if(!vm_obj_slot_live(o->states[i])) continue;
        if(jello_is_ptr(o->vals[i])) mark_ptr(ms, jello_as_ptr(o->vals[i]));
      }
      return;
    }
    case JELLO_OBJ_ARRAY: {
      const jello_array* a = (const jello_array*)p;
      jello_type_id elem_tid = m->types[a->h.type_id].as.unary.elem;
      jello_type_kind ek = m->types[elem_tid].kind;
      if(ek == JELLO_T_DYNAMIC) {
        const jello_value* vals = (const jello_value*)a->data;
        for(uint32_t i = 0; i < a->length; i++) {
          if(jello_is_ptr(vals[i])) mark_ptr(ms, jello_as_ptr(vals[i]));
        }
      } else if(vm_kind_is_gc_pointer(ek)) {
        void* const* ptrs = (void* const*)a->data;
        for(uint32_t i = 0; i < a->length; i++) {
          mark_ptr(ms, ptrs[i]);
        }
      }
      return;
    }
    case JELLO_OBJ_LIST: {
      const jello_list* l = (const jello_list*)p;
      if(jello_is_ptr(l->head)) mark_ptr(ms, jello_as_ptr(l->head));
      if(l->tail) mark_ptr(ms, l->tail);
      return;
    }
    case JELLO_OBJ_ENUM: {
      const jello_enum* e = (const jello_enum*)p;
      for(uint32_t i = 0; i < e->nfields; i++) {
        if(jello_is_ptr(e->fields[i])) mark_ptr(ms, jello_as_ptr(e->fields[i]));
      }
      return;
    }
    case JELLO_OBJ_FUNCTION: {
      const jello_function* fn = (const jello_function*)p;
      const jello_bc_function* callee = &m->funcs[fn->func_index];
      for(uint32_t i = 0; i < fn->ncaps; i++) {
        jello_type_kind ck = vm_reg_kind(m, callee, callee->cap_start + i);
        if(ck == JELLO_T_DYNAMIC) {
          jello_value v;
          memcpy(&v, &fn->captures[i], sizeof(jello_value));
          if(jello_is_ptr(v)) mark_ptr(ms, jello_as_ptr(v));
        } else if(vm_kind_is_gc_pointer(ck)) {
          void* fp;
          memcpy(&fp, &fn->captures[i], sizeof(void*));
          mark_ptr(ms, fp);
        }
      }
      return;
    }
    default:
      return;
  }
}

static void destroy_object(void* p) {
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_OBJECT: {
      jello_object* o = (jello_object*)p;
      free(o->keys); /* keys is the malloc base for the whole keys+vals+states block */
      return;
    }
    case JELLO_OBJ_ARRAY: {
      jello_array* a = (jello_array*)p;
      free(a->data);
      return;
    }
    default:
      return;
  }
}

static void scan_typed_frames(jello_vm* vm, mark_stack* ms) {
  const jello_bc_module* m = vm->running_module;
  if(!m) return;
  for(uint32_t fi = 0; fi < vm->call_frames_len; fi++) {
    call_frame* fr = &vm->call_frames[fi];
    for(uint32_t s = 0; s < fr->spill_cap; s++) {
      jello_value v = fr->spill_slots[s];
      if(jello_is_ptr(v)) mark_ptr(ms, jello_as_ptr(v));
    }
    if(!fr->has_pointer) continue;
    const jello_bc_function* f = fr->f;
    reg_frame* rf = &fr->rf;
    for(uint32_t r = 0; r < f->nregs; r++) {
      jello_type_kind k = vm_reg_kind_fast(rf, r);
      if(vm_kind_is_gc_pointer(k)) {
        void* p = NULL;
        memcpy(&p, vm_reg_ptr(rf, r), sizeof(void*));
        mark_ptr(ms, p);
      } else if(k == JELLO_T_DYNAMIC) {
        jello_value v;
        memcpy(&v, vm_reg_ptr(rf, r), sizeof(jello_value));
        if(jello_is_ptr(v)) mark_ptr(ms, jello_as_ptr(v));
      }
    }
  }
}

void jello_gc_collect(jello_vm* vm) {
  if(!vm) abort();
  jello_heap* heap = vm->heap;
  if(!vm_heap_stop_the_world(vm)) return; /* another thread's collection already ran */

  heap->gc_collections++;

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  mark_stack ms = {0};

  for(uint32_t i = 0; i < heap->gc_roots_len; i++) mark_ptr(&ms, heap->gc_roots[i]);
  for(uint32_t i = 0; i < heap->threads_len; i++) scan_typed_frames(heap->threads[i], &ms);

  for(;;) {
    void* p = ms_pop(&ms);
    if(!p) break;
    trace_object(&ms, vm->running_module, p);
  }
  free(ms.items);

  jello_gc_hdr** prev = &heap->gc_objects;
  jello_gc_hdr* h = heap->gc_objects;
  uint32_t live_bytes = 0;
  while(h) {
    jello_gc_hdr* next = h->next;
    if(!h->marked) {
      void* obj = obj_of(h);
      space->freed_objects++;
      *prev = next;
      destroy_object(obj);
      int ci = size_class_index(h->total_size);
      if(ci >= 0 && h->total_size == GC_SIZE_CLASSES[ci]) {
        h->next = heap->gc_free_classes[ci];
        heap->gc_free_classes[ci] = h;
      } else {
        free(h);
      }
    } else {
      h->marked = 0;
      live_bytes += h->total_size;
      prev = &h->next;
    }
    h = next;
  }

  heap->gc_bytes_live = live_bytes;
  heap->gc_next_collect = (live_bytes < 256u * 1024u) ? (512u * 1024u) : (live_bytes * 2u);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  space->pause_ms +=
      (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

  vm_heap_resume_the_world(vm);
}

jello_bytes* jello_bytes_new(jello_vm* vm, uint32_t length) {
  jello_bytes* b = (jello_bytes*)jello_gc_alloc(vm, sizeof(jello_bytes) + (size_t)length);
  b->h.kind = (uint8_t)JELLO_OBJ_BYTES;
  b->h.type_id = 0; /* Bytes is not a parameterised kind */
  b->length = length;
  return b;
}

jello_bytes* jello_bytes_new_zeroed(jello_vm* vm, uint32_t length) {
  jello_bytes* b = jello_bytes_new(vm, length);
  if(length > 0) memset(b->data, 0, length);
  return b;
}

jello_bytes* vm_buffer_new(jello_vm* vm, uint32_t length) {
  jello_bytes* b = jello_bytes_new_zeroed(vm, length);
  b->h.type_id = JELLO_BYTES_BUFFER;
  return b;
}

jello_bytes* jello_bytes_join(jello_vm* vm, jello_bytes* const* parts, uint32_t n,
                              const jello_bytes* separator) {
  uint32_t gap = separator ? separator->length : 0;
  uint64_t total = n ? (uint64_t)gap * (n - 1) : 0;
  /* Stopping once it is too long keeps the sum from wrapping. */
  for(uint32_t i = 0; i < n && total <= (uint64_t)UINT32_MAX; i++) total += parts[i]->length;
  if(total > (uint64_t)UINT32_MAX) {
    vm->trap_code = JELLO_TRAP_BOUNDS;
    return NULL;
  }
  jello_bytes* dst = jello_bytes_new(vm, (uint32_t)total);
  uint32_t at = 0;
  for(uint32_t i = 0; i < n; i++) {
    if(i && gap) {
      memcpy(dst->data + at, separator->data, gap);
      at += gap;
    }
    if(parts[i]->length) memcpy(dst->data + at, parts[i]->data, parts[i]->length);
    at += parts[i]->length;
  }
  return dst;
}

static jello_bytes* bytes_dup(jello_vm* vm, const jello_bytes* b, uint32_t start, uint32_t length,
                              uint32_t type_id) {
  jello_bytes* n = jello_bytes_new(vm, length);
  n->h.type_id = type_id;
  if(length) memcpy(n->data, b->data + start, length);
  return n;
}

jello_box_i64* jello_box_i64_new(jello_vm* vm, int64_t v) {
  jello_box_i64* b = (jello_box_i64*)jello_gc_alloc(vm, sizeof(jello_box_i64));
  b->h.kind = (uint8_t)JELLO_OBJ_BOX_I64;
  b->h.type_id = 0;
  b->value = v;
  return b;
}
jello_box_f64* jello_box_f64_new(jello_vm* vm, double v) {
  jello_box_f64* b = (jello_box_f64*)jello_gc_alloc(vm, sizeof(jello_box_f64));
  b->h.kind = (uint8_t)JELLO_OBJ_BOX_F64;
  b->h.type_id = 0;
  b->value = v;
  return b;
}
jello_box_f32* jello_box_f32_new(jello_vm* vm, float v) {
  jello_box_f32* b = (jello_box_f32*)jello_gc_alloc(vm, sizeof(jello_box_f32));
  b->h.kind = (uint8_t)JELLO_OBJ_BOX_F32;
  b->h.type_id = 0;
  b->value = v;
  return b;
}
jello_box_f16* jello_box_f16_new(jello_vm* vm, uint16_t bits) {
  jello_box_f16* b = (jello_box_f16*)jello_gc_alloc(vm, sizeof(jello_box_f16));
  b->h.kind = (uint8_t)JELLO_OBJ_BOX_F16;
  b->h.type_id = 0;
  b->value = bits;
  return b;
}

jello_value vm_clone_numbox(jello_vm* vm, jello_value v) {
  if(!vm || !jello_is_ptr(v) || jello_as_ptr(v) == NULL) return v;
  switch(((const jello_obj_header*)jello_as_ptr(v))->kind) {
    case JELLO_OBJ_BOX_F64:
      return jello_from_ptr(jello_box_f64_new(vm, ((const jello_box_f64*)jello_as_ptr(v))->value));
    case JELLO_OBJ_BOX_F32:
      return jello_from_ptr(jello_box_f32_new(vm, ((const jello_box_f32*)jello_as_ptr(v))->value));
    case JELLO_OBJ_BOX_F16:
      return jello_from_ptr(jello_box_f16_new(vm, ((const jello_box_f16*)jello_as_ptr(v))->value));
    case JELLO_OBJ_BOX_I64:
      return jello_from_ptr(jello_box_i64_new(vm, ((const jello_box_i64*)jello_as_ptr(v))->value));
    default: return v;
  }
}

static uint32_t array_elem_size(const jello_bc_module* m, uint32_t array_type_id) {
  jello_type_id elem_tid = m->types[array_type_id].as.unary.elem;
  return (uint32_t)jello_slot_size(m->types[elem_tid].kind);
}

jello_array* jello_array_new(jello_vm* vm, const jello_bc_module* m, uint32_t array_type_id, uint32_t length) {
  jello_array* a = (jello_array*)jello_gc_alloc(vm, sizeof(jello_array));
  a->h.kind = (uint8_t)JELLO_OBJ_ARRAY;
  a->h.type_id = array_type_id;
  a->length = 0;
  a->cap = 0;
  a->data = NULL;
  jello_array_resize(vm, m, a, length);
  return a;
}

void jello_array_resize(jello_vm* vm, const jello_bc_module* m, jello_array* a, uint32_t new_length) {
  (void)vm;
  uint32_t elem_size = array_elem_size(m, a->h.type_id);
  if(new_length > a->cap) {
    uint32_t new_cap = a->cap ? a->cap : 8u;
    while(new_cap < new_length) {
      if(new_cap > UINT32_MAX / 2u) {
        new_cap = new_length;
        break;
      }
      new_cap *= 2u;
    }
    if(elem_size != 0u && (size_t)new_cap > SIZE_MAX / (size_t)elem_size) abort();
    void* grown = realloc(a->data, (size_t)new_cap * (size_t)elem_size);
    if(!grown) abort();
    a->data = grown;
    a->cap = new_cap;
  }
  if(new_length > a->length) {
    memset((uint8_t*)a->data + (size_t)a->length * elem_size, 0,
           (size_t)(new_length - a->length) * elem_size);
    if(m->types[m->types[a->h.type_id].as.unary.elem].kind == JELLO_T_DYNAMIC) {
      jello_value* vals = (jello_value*)a->data;
      for(uint32_t i = a->length; i < new_length; i++) vals[i] = jello_make_null();
    }
  }
  a->length = new_length;
}

static jello_function* function_init(jello_function* fn, uint32_t func_index, uint32_t ncaps,
                                     jello_type_id type) {
  fn->h.kind = (uint8_t)JELLO_OBJ_FUNCTION;
  fn->h.type_id = type;
  fn->func_index = func_index;
  fn->ncaps = ncaps;
  for(uint32_t i = 0; i < ncaps; i++) fn->captures[i] = 0;
  return fn;
}

jello_function* jello_function_new(jello_vm* vm, uint32_t func_index, uint32_t ncaps) {
  jello_function* fn = (jello_function*)jello_gc_alloc(vm, sizeof(jello_function) + (size_t)ncaps * sizeof(uint64_t));
  fn->h.kind = (uint8_t)JELLO_OBJ_FUNCTION;
  fn->h.type_id = 0;
  fn->func_index = func_index;
  fn->ncaps = ncaps;
  for(uint32_t i = 0; i < ncaps; i++) fn->captures[i] = 0;
  return fn;
}

void* vm_immortal_alloc(size_t size) {
  if(size > SIZE_MAX - sizeof(jello_gc_hdr)) abort();
  jello_gc_hdr* h = (jello_gc_hdr*)calloc(1, sizeof(jello_gc_hdr) + size);
  if(!h) abort();
  h->marked = GC_IMMORTAL;
  return obj_of(h);
}

jello_function* vm_function_immortal(uint32_t func_index, jello_type_id type) {
  return function_init((jello_function*)vm_immortal_alloc(sizeof(jello_function)), func_index, 0,
                       type);
}

size_t vm_bytes_immortal_size(uint32_t length) {
  return (sizeof(jello_gc_hdr) + sizeof(jello_bytes) + (size_t)length + 7u) & ~(size_t)7u;
}

void vm_immortal_free(void* p) {
  if(p) free(hdr_of(p));
}

uint32_t vm_struct_fields_bytes(const jello_bc_module* m, uint32_t struct_sig_id) {
  const jello_struct_sig* sig = &m->struct_sigs[struct_sig_id];
  if(sig->nfields == 0) return 0;
  uint32_t last = sig->nfields - 1u;
  jello_type_kind lk = m->types[sig->field_type[last]].kind;
  return (sig->field_offset[last] + (uint32_t)jello_slot_size(lk) + 7u) & ~7u;
}

jello_struct* jello_struct_new(jello_vm* vm, const jello_bc_module* m, uint32_t struct_sig_id) {
  const jello_struct_sig* sig = &m->struct_sigs[struct_sig_id];
  uint32_t total = vm_struct_fields_bytes(m, struct_sig_id);
  jello_struct* s = (jello_struct*)jello_gc_alloc(vm, sizeof(jello_struct) + (size_t)total);
  s->h.kind = (uint8_t)JELLO_OBJ_STRUCT;
  s->h.type_id = struct_sig_id;
  if(total > 0) memset(s->fields, 0, total);
  for(uint16_t i = 0; i < sig->nfields; i++) {
    if(m->types[sig->field_type[i]].kind == JELLO_T_DYNAMIC) {
      jello_value nullv = jello_make_null();
      memcpy(s->fields + sig->field_offset[i], &nullv, sizeof(jello_value));
    }
  }
  return s;
}
