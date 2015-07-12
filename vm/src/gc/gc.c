// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define GC_INITIAL_THRESHOLD (1024u * 1024u) /* 1MB: a round, generous first-collection budget */

void jello_gc_init(jello_vm* vm) {
  if(!vm) return;
  if(vm->gc_next_collect == 0) vm->gc_next_collect = GC_INITIAL_THRESHOLD;
}

void jello_gc_shutdown(jello_vm* vm) {
  if(!vm) return;
  jello_gc_hdr* h = vm->gc_objects;
  while(h) {
    jello_gc_hdr* next = h->next;
    free(h);
    h = next;
  }
  vm->gc_objects = NULL;
  vm->gc_bytes_live = 0;
  for(int i = 0; i < GC_NUM_SIZE_CLASSES; i++) {
    jello_gc_hdr* fh = vm->gc_free_classes[i];
    while(fh) {
      jello_gc_hdr* next = fh->next;
      free(fh);
      fh = next;
    }
    vm->gc_free_classes[i] = NULL;
  }
  free(vm->gc_roots);
  vm->gc_roots = NULL;
  vm->gc_roots_len = 0;
  vm->gc_roots_cap = 0;
}

void jello_gc_push_root(jello_vm* vm, void* obj) {
  if(!vm) abort();
  if(vm->gc_roots_len == vm->gc_roots_cap) {
    uint32_t ncap = vm->gc_roots_cap ? vm->gc_roots_cap * 2u : 32u;
    void** nv = (void**)realloc(vm->gc_roots, sizeof(void*) * (size_t)ncap);
    if(!nv) abort();
    vm->gc_roots = nv;
    vm->gc_roots_cap = ncap;
  }
  vm->gc_roots[vm->gc_roots_len++] = obj;
}

void jello_gc_pop_roots(jello_vm* vm, uint32_t n) {
  if(!vm) abort();
  if(n > vm->gc_roots_len) abort();
  vm->gc_roots_len -= n;
}

void jello_vm_gc_stats(const jello_vm* vm, uint32_t* out_live_bytes, uint32_t* out_collections,
                        double* out_pause_ms) {
  if(!vm) return;
  if(out_live_bytes) *out_live_bytes = vm->gc_bytes_live;
  if(out_collections) *out_collections = vm->gc_collections;
  if(out_pause_ms) *out_pause_ms = vm->gc_pause_ms;
}

void* jello_gc_alloc(jello_vm* vm, size_t size) {
  if(!vm) abort();
  uint32_t requested = (uint32_t)(sizeof(jello_gc_hdr) + size);

  if(vm->gc_next_collect && vm->gc_bytes_live >= vm->gc_next_collect) {
    jello_gc_collect(vm);
  }

  int ci = size_class_index(requested);
  jello_gc_hdr* h;
  uint32_t total;
  if(ci >= 0) {
    total = GC_SIZE_CLASSES[ci];
    h = vm->gc_free_classes[ci];
    if(h) {
      vm->gc_free_classes[ci] = h->next;
    } else {
      h = (jello_gc_hdr*)malloc(total);
      if(!h) abort();
    }
  } else {
    total = requested;
    h = (jello_gc_hdr*)malloc(total);
    if(!h) abort();
  }

  h->total_size = total;
  h->marked = 0;
  h->next = vm->gc_objects;
  vm->gc_objects = h;
  vm->gc_bytes_live += total;
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

static void trace_object(mark_stack* ms, void* p) {
  (void)ms;
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_BYTES:
    case JELLO_OBJ_BOX_I64:
    case JELLO_OBJ_BOX_F64:
    case JELLO_OBJ_BOX_F32:
    case JELLO_OBJ_BOX_F16:
      return;
    default:
      return;
  }
}

static void destroy_object(void* p) {
  const jello_obj_header* oh = (const jello_obj_header*)p;
  switch((jello_obj_kind)oh->kind) {
    case JELLO_OBJ_BYTES:
      return;
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
      jello_type_kind k = m->types[f->reg_types[r]].kind;
      if(k == JELLO_T_BYTES) {
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
  vm->gc_collections++;

  clock_t t0 = clock();

  mark_stack ms = {0};

  for(uint32_t i = 0; i < vm->gc_roots_len; i++) mark_ptr(&ms, vm->gc_roots[i]);
  scan_typed_frames(vm, &ms);

  for(;;) {
    void* p = ms_pop(&ms);
    if(!p) break;
    trace_object(&ms, p);
  }
  free(ms.items);

  jello_gc_hdr** prev = &vm->gc_objects;
  jello_gc_hdr* h = vm->gc_objects;
  uint32_t live_bytes = 0;
  while(h) {
    jello_gc_hdr* next = h->next;
    if(!h->marked) {
      void* obj = obj_of(h);
      vm->gc_freed_objects++;
      *prev = next;
      destroy_object(obj);
      int ci = size_class_index(h->total_size);
      if(ci >= 0 && h->total_size == GC_SIZE_CLASSES[ci]) {
        h->next = vm->gc_free_classes[ci];
        vm->gc_free_classes[ci] = h;
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

  vm->gc_bytes_live = live_bytes;
  vm->gc_next_collect = (live_bytes < 256u * 1024u) ? (512u * 1024u) : (live_bytes * 2u);

  clock_t t1 = clock();
  vm->gc_pause_ms += (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC;
}

/* -- Bytes -- */

jello_bytes* jello_bytes_new(jello_vm* vm, uint32_t length) {
  jello_bytes* b = (jello_bytes*)jello_gc_alloc(vm, sizeof(jello_bytes) + (size_t)length);
  b->h.kind = (uint8_t)JELLO_OBJ_BYTES;
  b->h.type_id = 0; /* Bytes is not a parameterised kind; type_id unused */
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
