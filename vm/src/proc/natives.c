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
static void free_reply(void* payload) {
  if(!jdl_release(payload)) return;
  proc_reply* r = (proc_reply*)payload;
  vm_parcel_free(&r->value);
  free(r);
}

const jello_jdll_handle_class proc_reply_class = {"reply", free_reply, jdl_retain};

static void release_timer(void* payload) { proc_timer_release((proc_timer*)payload); }

static void retain_timer(void* payload) {
  atomic_fetch_add_explicit(&((proc_timer*)payload)->refs, 1, memory_order_relaxed);
}

static const jello_jdll_handle_class proc_timer_class = {"timer", release_timer, retain_timer};

static void return_process(jdlo_ctx* c, jello_process* p) {
  proc_retain(p);
  jdl_return_handle(c, &proc_handle_class, p, (uint32_t)sizeof(jello_process));
}

void proc_trap(jdlo_ctx* c, jello_trap_code code) {
  c->trap = code;
  jdl_fail(c, NULL);
}

void* proc_arg_handle(jdlo_ctx* c, int index, const jello_jdll_handle_class* cls) {
  if(jello_is_null(jdl_arg_value(c, index))) {
    proc_trap(c, JELLO_TRAP_NULL_DEREF);
    return NULL;
  }
  return jdl_arg_handle(c, index, cls);
}

static jello_process* arg_process(jdlo_ctx* c, int index) {
  return (jello_process*)proc_arg_handle(c, index, &proc_handle_class);
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
static void vm_spawn_detached(jdlo_ctx* c) { spawn(c, PROC_SPAWN_DETACHED); }
static void vm_spawn_on_main(jdlo_ctx* c) { spawn(c, PROC_SPAWN_ON_MAIN); }
static void vm_on_main(jdlo_ctx* c) { jdl_return_bool(c, proc_on_main(c->vm)); }

jello_process* proc_self_or_fail(jdlo_ctx* c) {
  jello_process* p = proc_self(c->vm);
  if(!p) jdl_fail(c, "out of memory");
  return p;
}

static void vm_self(jdlo_ctx* c) {
  jello_process* p = proc_self_or_fail(c);
  if(p) return_process(c, p);
}

static uint64_t due_in(int32_t ms) { return proc_deadline(ms < 0 ? 0 : ms); }

static int pack_arg(jdlo_ctx* c, vm_parcel* out) {
  jello_trap_code code = vm_parcel_pack(c->vm, jdl_arg_value(c, 1), 0, out);
  if(code) proc_trap(c, code);
  return !code;
}

static void vm_send(jdlo_ctx* c) {
  jello_process* p = arg_process(c, 0);
  if(!p) return;
  int sent = 0;
  if(!proc_finished(p)) {
    vm_parcel msg;
    if(!pack_arg(c, &msg)) return;
    sent = proc_send(c->vm->proc, p, &msg);
  }
  jdl_return_bool(c, sent);
}

static void vm_receive(jdlo_ctx* c) {
  jello_value msg;
  jello_trap_code code = proc_receive(c->vm, &msg);
  if(code) proc_trap(c, code);
  else jdl_return_value(c, msg);
}

static void vm_pending(jdlo_ctx* c) { jdl_return_i32(c, (int32_t)proc_pending(c->vm)); }

static void vm_wait_message(jdlo_ctx* c) {
  jello_trap_code code = proc_wait_message(c->vm, proc_deadline(jdl_arg_i32(c, 0)));
  if(code) proc_trap(c, code);
  else jdl_return_bool(c, proc_pending(c->vm) > 0);
}

static void vm_sleep(jdlo_ctx* c) {
  jello_trap_code code = proc_sleep(c->vm, due_in(jdl_arg_i32(c, 0)));
  if(code) proc_trap(c, code);
  else jdl_return_bool(c, 1);
}

static void vm_poll_open(jdlo_ctx* c) {
  int watched = 0;
  jello_trap_code code = proc_poll_open(c->vm, jdl_arg_i32(c, 0), &watched);
  if(code) proc_trap(c, code);
  else jdl_return_bool(c, watched);
}

static void vm_poll_wait(jdlo_ctx* c) {
  uint32_t ready = 0;
  jello_trap_code code = proc_poll_wait(c->vm, jdl_arg_i32(c, 0), (uint32_t)jdl_arg_i32(c, 1),
                                        proc_deadline(jdl_arg_i32(c, 2)), &ready);
  if(code) proc_trap(c, code);
  else jdl_return_i32(c, (int32_t)ready);
}

static void vm_poll_close(jdlo_ctx* c) {
  jello_trap_code code = proc_poll_close(c->vm, jdl_arg_i32(c, 0));
  if(code) proc_trap(c, code);
  else jdl_return_bool(c, 1);
}

static void vm_clock(jdlo_ctx* c) {
  jello_process* p = proc_self_or_fail(c);
  if(p) jdl_return_i32(c, (int32_t)(uint32_t)((proc_now() - p->sched->epoch) / 1000000u));
}

static void vm_send_after(jdlo_ctx* c) {
  vm_parcel msg;
  if(!proc_self_or_fail(c) || !pack_arg(c, &msg)) return;
  proc_timer* t = proc_send_after(c->vm, due_in(jdl_arg_i32(c, 0)), &msg);
  if(!t) {
    vm_parcel_free(&msg);
    jdl_fail(c, "out of memory");
    return;
  }
  jdl_return_handle(c, &proc_timer_class, t, (uint32_t)sizeof(proc_timer));
}

static void vm_timer_cancel(jdlo_ctx* c) {
  proc_timer* t = (proc_timer*)proc_arg_handle(c, 0, &proc_timer_class);
  if(t && proc_self_or_fail(c)) jdl_return_bool(c, proc_timer_cancel(c->vm, t));
}

static void vm_exited(jdlo_ctx* c) {
  jello_process* p = arg_process(c, 0);
  if(p) jdl_return_i32(c, (int32_t)proc_exit_of(p));
}

static void vm_exit_value(jdlo_ctx* c) {
  jello_process* p = arg_process(c, 0);
  if(!p) return;
  if(proc_finished(p)) jdl_return_value(c, vm_parcel_read(c->vm, &p->exit));
  else jdl_return_null(c);
}

static void vm_on_exit(jdlo_ctx* c) {
  if(!arg_body(c)) return;
  proc_on_exit(c->vm, jdl_arg_value(c, 0));
  jdl_return_bool(c, 1);
}

static void vm_stop(jdlo_ctx* c) {
  jello_process* p = arg_process(c, 0);
  if(!p) return;
  if(p == c->vm->proc) {
    proc_trap(c, JELLO_TRAP_STOPPED);
    return;
  }
  jdl_return_bool(c, proc_stop(p, PROC_STOPPED));
}

static void vm_yield(jdlo_ctx* c) {
  jello_trap_code code = proc_yield(c->vm);
  if(code) proc_trap(c, code);
  else jdl_return_bool(c, 1);
}

static void vm_reply_new(jdlo_ctx* c) {
  proc_reply* r = (proc_reply*)malloc(sizeof(proc_reply));
  if(!r) {
    jdl_fail(c, "out of memory");
    return;
  }
  r->refs = 1;
  r->value = vm_parcel_empty();
  r->sent = 0;
  r->waiter = NULL;
  jdl_return_handle(c, &proc_reply_class, r, (uint32_t)sizeof(proc_reply));
}

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
