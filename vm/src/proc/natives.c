// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The built-in `vm` library: processes for Jello code. A process is held as a handle to its
 * record. Unlike a `.jdll` export, these run with their VM running rather than quiescent, since
 * they allocate, park and change scheduler state; parking makes the VM quiescent itself. */

#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>

#include <stdlib.h>
#include <string.h>

static void release_payload(void* payload) { proc_release((jello_process*)payload); }

static void retain_payload(void* payload) { proc_retain((jello_process*)payload); }

const jello_jdll_handle_class proc_handle_class = {"process", release_payload, retain_payload};

static void release_timer(void* payload) { proc_timer_release((proc_timer*)payload); }

static void retain_timer(void* payload) {
  atomic_fetch_add_explicit(&((proc_timer*)payload)->refs, 1, memory_order_relaxed);
}

static const jello_jdll_handle_class proc_timer_class = {"timer", release_timer, retain_timer};

void proc_trap(jdlo_ctx* c, jello_trap_code code) {
  c->trap = code;
  jdl_fail(c, NULL);
}

static int arg_body(jdlo_ctx* c) {
  jello_value v = jdl_arg_value(c, 0);
  const jello_function* f = jello_is_ptr(v) ? (const jello_function*)jello_as_ptr(v) : NULL;
  if(f && f->h.kind == JELLO_OBJ_FUNCTION && !c->vm->running_module->funcs[f->func_index].nargs) {
    return 1;
  }
  proc_trap(c, JELLO_TRAP_TYPE_MISMATCH);
  return 0;
}

static void spawn(jdlo_ctx* c, unsigned flags) {
  jello_vm* vm = c->vm;
  if(!arg_body(c)) return;
  const jello_function* body = (const jello_function*)jello_as_ptr(jdl_arg_value(c, 0));
  jello_trap_code code;
  jello_process* p = proc_spawn(vm, body, NULL, flags, &code);
  if(!p) {
    if(code) proc_trap(c, code);
    else jdl_fail(c, "out of memory");
    return;
  }
  jdl_return_handle(c, &proc_handle_class, p, (uint32_t)sizeof(jello_process));
}

static void vm_spawn(jdlo_ctx* c) { spawn(c, 0); }

static uint64_t due_in(int32_t ms) { return proc_deadline(ms < 0 ? 0 : ms); }

typedef struct proc_buffer {
  jdl_refs refs;
  uint32_t length;
  _Alignas(8) uint8_t data[];
} proc_buffer;

static void free_buffer(void* payload) {
  if(jdl_release(payload)) free(payload);
}

static const jello_jdll_handle_class proc_buffer_class = {"buffer", free_buffer, jdl_retain};

static uint64_t load_bits(const void* at, uint32_t width) {
  switch(width) {
    case 1: return __atomic_load_n((const uint8_t*)at, __ATOMIC_RELAXED);
    case 2: return __atomic_load_n((const uint16_t*)at, __ATOMIC_RELAXED);
    case 4: return __atomic_load_n((const uint32_t*)at, __ATOMIC_RELAXED);
    default: return __atomic_load_n((const uint64_t*)at, __ATOMIC_RELAXED);
  }
}

static void store_bits(void* at, uint32_t width, uint64_t v) {
  switch(width) {
    case 1: __atomic_store_n((uint8_t*)at, (uint8_t)v, __ATOMIC_RELAXED); break;
    case 2: __atomic_store_n((uint16_t*)at, (uint16_t)v, __ATOMIC_RELAXED); break;
    case 4: __atomic_store_n((uint32_t*)at, (uint32_t)v, __ATOMIC_RELAXED); break;
    default: __atomic_store_n((uint64_t*)at, v, __ATOMIC_RELAXED); break;
  }
}

#define D (uint8_t)JELLO_T_DYNAMIC
#define I (uint8_t)JELLO_T_I32
#define B (uint8_t)JELLO_T_BOOL
#define L (uint8_t)JELLO_T_I64
#define F (uint8_t)JELLO_T_F32
#define G (uint8_t)JELLO_T_F64
#define Y (uint8_t)JELLO_T_BYTES

static const jello_jdll_export VM_EXPORTS[] = {
    {"proc_spawn", vm_spawn, D, 1, {D}, 0},
};

const jello_jdll_manifest vm_builtin_manifest = {
    JELLO_JDLL_MANIFEST_MAGIC,
    JELLO_JDLL_ABI_VERSION,
    VM_EXPORTS,
    (uint32_t)(sizeof(VM_EXPORTS) / sizeof(VM_EXPORTS[0])),
    NULL,
};

int vm_builtin_export(const jello_jdll_export* e) {
  return e >= VM_EXPORTS && e < VM_EXPORTS + vm_builtin_manifest.nexports;
}
