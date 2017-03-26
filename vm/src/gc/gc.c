// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* -- object header + size classes -- */
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

/* -- mark -- */

typedef struct mark_stack {
  void** items;
  uint32_t len;
  uint32_t cap;
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
        } else if(fk == JELLO_T_BYTES || fk == JELLO_T_STRUCT || fk == JELLO_T_OBJECT) {
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
        if(o->states[i] != JELLO_OBJ_SLOT_OCCUPIED) continue;
        if(jello_is_ptr(o->vals[i])) mark_ptr(ms, jello_as_ptr(o->vals[i]));
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
    default:
      return;
  }
}

static void scan_typed_frames(jello_vm* vm, mark_stack* ms) {
  const jello_bc_module* m = vm->running_module;
  if(!m) return;
  for(uint32_t fi = 0; fi < vm->call_frames_len; fi++) {
    call_frame* fr = &vm->call_frames[fi];
    if(!fr->has_pointer) continue;
    const jello_bc_function* f = fr->f;
    reg_frame* rf = &fr->rf;
    for(uint32_t r = 0; r < f->nregs; r++) {
      jello_type_kind k = vm_reg_kind_fast(rf, r);
      if(k == JELLO_T_BYTES || k == JELLO_T_STRUCT || k == JELLO_T_OBJECT) {
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
      heap->gc_freed_objects++;
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
  heap->gc_pause_ms += (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

  vm_heap_resume_the_world(vm);
}

/* -- Bytes -- */

jello_bytes* jello_bytes_new(jello_vm* vm, uint32_t length) {
  jello_bytes* b = (jello_bytes*)jello_gc_alloc(vm, sizeof(jello_bytes) + (size_t)length);
  b->h.kind = (uint8_t)JELLO_OBJ_BYTES;
  b->h.type_id = 0; /* Bytes is not a parameterised kind (Chapter 4 Sec 2.2); type_id unused */
  b->length = length;
  return b;
}

/* -- numeric boxing -- */

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

/* -- Struct -- */
jello_struct* jello_struct_new(jello_vm* vm, const jello_bc_module* m, uint32_t struct_sig_id) {
  const jello_struct_sig* sig = &m->struct_sigs[struct_sig_id];
  uint32_t total = 0;
  if(sig->nfields > 0) {
    uint32_t last = sig->nfields - 1u;
    jello_type_kind lk = m->types[sig->field_type[last]].kind;
    total = sig->field_offset[last] + (uint32_t)jello_slot_size(lk);
    total = (total + 7u) & ~7u;
  }
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
