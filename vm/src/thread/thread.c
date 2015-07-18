// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdlib.h>

/* -- the shared heap: lifecycle + registration -- */

void vm_free_layouts(jello_vm* vm);

static jello_heap* heap_new(void) {
  jello_heap* heap = (jello_heap*)calloc(1, sizeof(jello_heap));
  if(!heap) abort();
  heap->gc_next_collect = 0; /* jello_gc_init (gc.c) sets the real initial threshold */
  if(pthread_mutex_init(&heap->alloc_lock, NULL) != 0) abort();
  if(pthread_mutex_init(&heap->stw_lock, NULL) != 0) abort();
  if(pthread_cond_init(&heap->stw_resume_cv, NULL) != 0) abort();
  atomic_init(&heap->stop_requested, 0);
  heap->refcount = 1;
  return heap;
}

static void heap_register(jello_heap* heap, jello_vm* vm) {
  pthread_mutex_lock(&heap->alloc_lock);
  if(heap->threads_len == heap->threads_cap) {
    uint32_t ncap = heap->threads_cap ? heap->threads_cap * 2u : 4u;
    jello_vm** nt = (jello_vm**)realloc(heap->threads, sizeof(jello_vm*) * (size_t)ncap);
    if(!nt) abort();
    heap->threads = nt;
    heap->threads_cap = ncap;
  }
  heap->threads[heap->threads_len++] = vm;
  pthread_mutex_unlock(&heap->alloc_lock);
}

static int heap_unregister(jello_heap* heap, jello_vm* vm) {
  pthread_mutex_lock(&heap->alloc_lock);
  for(uint32_t i = 0; i < heap->threads_len; i++) {
    if(heap->threads[i] == vm) {
      heap->threads[i] = heap->threads[heap->threads_len - 1u];
      heap->threads_len--;
      break;
    }
  }
  heap->refcount--;
  int last = (heap->refcount == 0);
  pthread_mutex_unlock(&heap->alloc_lock);
  return last;
}

jello_vm* jello_vm_create(void) {
  jello_vm* vm = (jello_vm*)calloc(1, sizeof(jello_vm));
  if(!vm) return NULL;
  vm->heap = heap_new();
  heap_register(vm->heap, vm);
  jello_gc_init(vm);
  return vm;
}

jello_vm* jello_vm_create_joined(jello_vm* existing) {
  if(!existing || !existing->heap) return NULL;
  jello_vm* vm = (jello_vm*)calloc(1, sizeof(jello_vm));
  if(!vm) return NULL;
  jello_heap* heap = existing->heap;
  pthread_mutex_lock(&heap->alloc_lock);
  heap->refcount++;
  pthread_mutex_unlock(&heap->alloc_lock);
  vm->heap = heap;
  heap_register(heap, vm);
  jello_gc_init(vm); /* idempotent: gc_next_collect is already nonzero on a shared heap */
  return vm;
}

void jello_vm_destroy(jello_vm* vm) {
  if(!vm) return;
  jello_heap* heap = vm->heap;
  if(heap) {
    int last = heap_unregister(heap, vm);
    if(last) {
      jello_gc_shutdown(vm); /* frees heap-owned gc state via vm->heap; never frees heap itself */
      pthread_mutex_destroy(&heap->alloc_lock);
      pthread_mutex_destroy(&heap->stw_lock);
      pthread_cond_destroy(&heap->stw_resume_cv);
      free(heap->threads);
      free(heap);
    }
  }
  free(vm->frame_stack);
  free(vm->call_frames);
  vm_free_layouts(vm);
  free(vm);
}

/* -- quiescent tracking -- */
void vm_go_quiescent(jello_vm* vm) {
  atomic_fetch_add(&vm->quiescent_depth, 1);
}

void vm_wait_for_resume(jello_vm* vm) {
  jello_heap* heap = vm->heap;
  if(!atomic_load(&heap->stop_requested)) return;
  pthread_mutex_lock(&heap->stw_lock);
  while(atomic_load(&heap->stop_requested)) {
    pthread_cond_wait(&heap->stw_resume_cv, &heap->stw_lock);
  }
  pthread_mutex_unlock(&heap->stw_lock);
}

void vm_leave_quiescent(jello_vm* vm) {
  vm_wait_for_resume(vm);
  atomic_fetch_sub(&vm->quiescent_depth, 1);
}

void vm_safepoint_check(jello_vm* vm) {
  if(!atomic_load(&vm->heap->stop_requested)) return;
  vm_go_quiescent(vm);
  vm_wait_for_resume(vm);
  vm_leave_quiescent(vm);
}

void jello_vm_blocking(jello_vm* vm, int is_blocking) {
  if(!vm) abort();
  if(is_blocking) {
    vm_go_quiescent(vm);
  } else {
    if(atomic_load(&vm->quiescent_depth) == 0) abort(); /* unbalanced enter/exit */
    vm_leave_quiescent(vm);
  }
}

/* -- stop-the-world -- */
int vm_heap_stop_the_world(jello_vm* vm) {
  jello_heap* heap = vm->heap;
  int expected = 0;
  if(!atomic_compare_exchange_strong(&heap->stop_requested, &expected, 1)) {
    vm_go_quiescent(vm);
    vm_wait_for_resume(vm);
    vm_leave_quiescent(vm);
    return 0;
  }

  pthread_mutex_lock(&heap->alloc_lock);
  for(uint32_t i = 0; i < heap->threads_len; i++) {
    jello_vm* t = heap->threads[i];
    if(t == vm) continue;
    while(atomic_load(&t->quiescent_depth) == 0 && atomic_load(&t->in_exec) != 0) {
    }
  }
  return 1;
}

void vm_heap_resume_the_world(jello_vm* vm) {
  jello_heap* heap = vm->heap;
  pthread_mutex_unlock(&heap->alloc_lock);
  atomic_store(&heap->stop_requested, 0);
  pthread_mutex_lock(&heap->stw_lock);
  pthread_cond_broadcast(&heap->stw_resume_cv);
  pthread_mutex_unlock(&heap->stw_lock);
}
