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

static void vm_join(jdlo_ctx* c) {
  jello_process* p = arg_process(c, 0);
  if(!p) return;
  jello_trap_code code = proc_join(c->vm, p);
  if(code) proc_trap(c, code);
  else jdl_return_i32(c, (int32_t)proc_exit_of(p));
}

static void vm_join_within(jdlo_ctx* c) {
  jello_process* p = arg_process(c, 0);
  if(!p) return;
  jello_trap_code code = proc_await(c->vm, p, proc_deadline(jdl_arg_i32(c, 1)));
  if(code) proc_trap(c, code);
  else jdl_return_i32(c, (int32_t)proc_exit_of(p));
}

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

static proc_reply* arg_reply(jdlo_ctx* c, int index) {
  return (proc_reply*)proc_arg_handle(c, index, &proc_reply_class);
}

static void vm_reply_send(jdlo_ctx* c) {
  proc_reply* r = arg_reply(c, 0);
  if(!r || !proc_self_or_fail(c)) return;
  vm_parcel v;
  if(pack_arg(c, &v)) jdl_return_bool(c, proc_reply_send(c->vm, r, &v));
}

static void vm_reply_wait(jdlo_ctx* c) {
  proc_reply* r = arg_reply(c, 0);
  jello_process* p = r ? arg_process(c, 1) : NULL;
  if(!p || !proc_self_or_fail(c)) return;
  int outcome = 0;
  jello_trap_code code = proc_reply_wait(c->vm, r, p, proc_deadline(jdl_arg_i32(c, 2)), &outcome);
  if(code) proc_trap(c, code);
  else jdl_return_i32(c, outcome);
}

static void vm_reply_value(jdlo_ctx* c) {
  proc_reply* r = arg_reply(c, 0);
  if(!r) return;
  jdl_return_value(c, vm_parcel_read(c->vm, &r->value));
}

static void vm_schedulers(jdlo_ctx* c) {
  jello_process* p = proc_self_or_fail(c);
  if(p) jdl_return_i32(c, (int32_t)p->sched->nworkers);
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

static void vm_buf_new(jdlo_ctx* c) {
  int32_t n = jdl_arg_i32(c, 0);
  if(n < 0) {
    proc_trap(c, JELLO_TRAP_BOUNDS);
    return;
  }
  proc_buffer* b = (proc_buffer*)calloc(1, sizeof(proc_buffer) + (size_t)n);
  if(!b) {
    jdl_fail(c, "out of memory");
    return;
  }
  b->refs = 1;
  b->length = (uint32_t)n;
  jdl_return_handle(c, &proc_buffer_class, b, (uint32_t)sizeof(proc_buffer) + (uint32_t)n);
}

static void vm_buf_length(jdlo_ctx* c) {
  proc_buffer* b = (proc_buffer*)proc_arg_handle(c, 0, &proc_buffer_class);
  if(b) jdl_return_i32(c, (int32_t)b->length);
}

static void* element(jdlo_ctx* c, int index, uint32_t width) {
  proc_buffer* b = (proc_buffer*)proc_arg_handle(c, 0, &proc_buffer_class);
  if(!b) return NULL;
  int32_t i = jdl_arg_i32(c, index);
  if(i < 0 || (uint64_t)(uint32_t)i * width + width > b->length) {
    proc_trap(c, JELLO_TRAP_BOUNDS);
    return NULL;
  }
  return b->data + (size_t)i * width;
}

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

static void* sized_element(jdlo_ctx* c, uint32_t* width) {
  int32_t w = jdl_arg_i32(c, 1);
  if(w != 1 && w != 2 && w != 4) {
    proc_trap(c, JELLO_TRAP_TYPE_MISMATCH);
    return NULL;
  }
  *width = (uint32_t)w;
  return element(c, 2, *width);
}

static void vm_buf_get(jdlo_ctx* c) {
  uint32_t w;
  void* at = sized_element(c, &w);
  if(at) jdl_return_i32(c, (int32_t)(uint32_t)load_bits(at, w));
}

static void vm_buf_set(jdlo_ctx* c) {
  uint32_t w;
  void* at = sized_element(c, &w);
  if(!at) return;
  store_bits(at, w, (uint32_t)jdl_arg_i32(c, 3));
  jdl_return_bool(c, 1);
}

static void vm_buf_get_i64(jdlo_ctx* c) {
  void* at = element(c, 1, 8);
  if(at) jdl_return_i64(c, (int64_t)load_bits(at, 8));
}

static void vm_buf_set_i64(jdlo_ctx* c) {
  void* at = element(c, 1, 8);
  if(!at) return;
  store_bits(at, 8, (uint64_t)jdl_arg_i64(c, 2));
  jdl_return_bool(c, 1);
}

static void vm_buf_get_f32(jdlo_ctx* c) {
  void* at = element(c, 1, 4);
  if(!at) return;
  uint32_t bits = (uint32_t)load_bits(at, 4);
  float v;
  memcpy(&v, &bits, sizeof v);
  jdl_return_f32(c, v);
}

static void vm_buf_set_f32(jdlo_ctx* c) {
  void* at = element(c, 1, 4);
  if(!at) return;
  float v = jdl_arg_f32(c, 2);
  uint32_t bits;
  memcpy(&bits, &v, sizeof bits);
  store_bits(at, 4, bits);
  jdl_return_bool(c, 1);
}

static void vm_buf_get_f64(jdlo_ctx* c) {
  void* at = element(c, 1, 8);
  if(!at) return;
  uint64_t bits = load_bits(at, 8);
  double v;
  memcpy(&v, &bits, sizeof v);
  jdl_return_f64(c, v);
}

static void vm_buf_set_f64(jdlo_ctx* c) {
  void* at = element(c, 1, 8);
  if(!at) return;
  double v = jdl_arg_f64(c, 2);
  uint64_t bits;
  memcpy(&bits, &v, sizeof bits);
  store_bits(at, 8, bits);
  jdl_return_bool(c, 1);
}

static void vm_buf_add_i32(jdlo_ctx* c) {
  uint32_t* at = (uint32_t*)element(c, 1, 4);
  if(at) {
    jdl_return_i32(c,
                   (int32_t)__atomic_fetch_add(at, (uint32_t)jdl_arg_i32(c, 2), __ATOMIC_ACQ_REL));
  }
}

static void vm_buf_cas_i32(jdlo_ctx* c) {
  uint32_t* at = (uint32_t*)element(c, 1, 4);
  if(!at) return;
  uint32_t old = (uint32_t)jdl_arg_i32(c, 2);
  jdl_return_bool(c, __atomic_compare_exchange_n(at, &old, (uint32_t)jdl_arg_i32(c, 3), 0,
                                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

static void vm_buf_add_i64(jdlo_ctx* c) {
  uint64_t* at = (uint64_t*)element(c, 1, 8);
  if(at) {
    jdl_return_i64(c,
                   (int64_t)__atomic_fetch_add(at, (uint64_t)jdl_arg_i64(c, 2), __ATOMIC_ACQ_REL));
  }
}

static void vm_buf_cas_i64(jdlo_ctx* c) {
  uint64_t* at = (uint64_t*)element(c, 1, 8);
  if(!at) return;
  uint64_t old = (uint64_t)jdl_arg_i64(c, 2);
  jdl_return_bool(c, __atomic_compare_exchange_n(at, &old, (uint64_t)jdl_arg_i64(c, 3), 0,
                                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

static void vm_buf_slice(jdlo_ctx* c) {
  proc_buffer* b = (proc_buffer*)proc_arg_handle(c, 0, &proc_buffer_class);
  if(!b) return;
  int32_t start = jdl_arg_i32(c, 1), n = jdl_arg_i32(c, 2);
  if(start < 0 || n < 0 || (uint64_t)start + (uint64_t)n > b->length) {
    proc_trap(c, JELLO_TRAP_BOUNDS);
    return;
  }
  jello_bytes* out = jello_bytes_new(c->vm, (uint32_t)n);
  if(!out) {
    jdl_fail(c, "out of memory");
    return;
  }
  for(int32_t i = 0; i < n; i++) out->data[i] = (uint8_t)load_bits(&b->data[start + i], 1);
  jdl_return_value(c, jello_from_ptr(out));
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
    {"proc_spawn_detached", vm_spawn_detached, D, 1, {D}, 0},
    {"proc_spawn_on_main", vm_spawn_on_main, D, 1, {D}, 0},
    {"proc_on_main", vm_on_main, B, 0, {0}, 0},
    {"proc_self", vm_self, D, 0, {0}, 0},
    {"proc_send", vm_send, B, 2, {D, D}, 0},
    {"proc_receive", vm_receive, D, 0, {0}, 0},
    {"proc_pending", vm_pending, I, 0, {0}, 0},
    {"proc_join", vm_join, I, 1, {D}, 0},
    {"proc_join_within", vm_join_within, I, 2, {D, I}, 0},
    {"proc_wait_message", vm_wait_message, B, 1, {I}, 0},
    {"proc_sleep", vm_sleep, B, 1, {I}, 0},
    {"proc_poll_open", vm_poll_open, B, 1, {I}, 0},
    {"proc_poll_wait", vm_poll_wait, I, 3, {I, I, I}, 0},
    {"proc_poll_close", vm_poll_close, B, 1, {I}, 0},
    {"proc_clock", vm_clock, I, 0, {0}, 0},
    {"proc_send_after", vm_send_after, D, 2, {I, D}, 0},
    {"proc_timer_cancel", vm_timer_cancel, B, 1, {D}, 0},
    {"proc_exited", vm_exited, I, 1, {D}, 0},
    {"proc_exit_value", vm_exit_value, D, 1, {D}, 0},
    {"proc_stop", vm_stop, B, 1, {D}, 0},
    {"proc_on_exit", vm_on_exit, B, 1, {D}, 0},
    {"proc_yield", vm_yield, B, 0, {0}, 0},
    {"proc_reply_new", vm_reply_new, D, 0, {0}, 0},
    {"proc_reply_send", vm_reply_send, B, 2, {D, D}, 0},
    {"proc_reply_wait", vm_reply_wait, I, 3, {D, D, I}, 0},
    {"proc_reply_value", vm_reply_value, D, 1, {D}, 0},
    {"proc_schedulers", vm_schedulers, I, 0, {0}, 0},
    {"buffer_new", vm_buf_new, D, 1, {I}, 0},
    {"buffer_length", vm_buf_length, I, 1, {D}, 0},
    {"buffer_get", vm_buf_get, I, 3, {D, I, I}, 0},
    {"buffer_set", vm_buf_set, B, 4, {D, I, I, I}, 0},
    {"buffer_get_i64", vm_buf_get_i64, L, 2, {D, I}, 0},
    {"buffer_set_i64", vm_buf_set_i64, B, 3, {D, I, L}, 0},
    {"buffer_get_f32", vm_buf_get_f32, F, 2, {D, I}, 0},
    {"buffer_set_f32", vm_buf_set_f32, B, 3, {D, I, F}, 0},
    {"buffer_get_f64", vm_buf_get_f64, G, 2, {D, I}, 0},
    {"buffer_set_f64", vm_buf_set_f64, B, 3, {D, I, G}, 0},
    {"buffer_add_i32", vm_buf_add_i32, I, 3, {D, I, I}, 0},
    {"buffer_cas_i32", vm_buf_cas_i32, B, 4, {D, I, I, I}, 0},
    {"buffer_add_i64", vm_buf_add_i64, L, 3, {D, I, L}, 0},
    {"buffer_cas_i64", vm_buf_cas_i64, B, 4, {D, I, L, L}, 0},
    {"buffer_slice", vm_buf_slice, D, 3, {D, I, I}, 0},
    {"net_tcp_listen", vm_net_tcp_listen, D, 2, {Y, I}, 0},
    {"net_tcp_accept", vm_net_tcp_accept, D, 2, {D, I}, 0},
    {"net_tcp_connect", vm_net_tcp_connect, D, 3, {Y, I, I}, 0},
    {"net_udp_bind", vm_net_udp_bind, D, 2, {Y, I}, 0},
    {"net_read", vm_net_read, Y, 3, {D, I, I}, 0},
    {"net_write", vm_net_write, B, 2, {D, Y}, 0},
    {"net_receive_from", vm_net_receive_from, Y, 3, {D, I, I}, 0},
    {"net_send_to", vm_net_send_to, B, 4, {D, Y, Y, I}, 0},
    {"net_close", vm_net_close, B, 1, {D}, 0},
    {"net_local_port", vm_net_local_port, I, 1, {D}, 0},
    {"net_peer_host", vm_net_peer_host, Y, 1, {D}, 0},
    {"net_peer_port", vm_net_peer_port, I, 1, {D}, 0},
    {"net_error", vm_net_error, I, 0, {0}, 0},
    {"net_error_text", vm_net_error_text, Y, 1, {I}, 0},
    {"net_tls_start", vm_net_tls_start, B, 3, {D, D, Y}, 0},
    {"image_run", vm_image_run, Y, 3, {Y, Y, L}, 0},
};

const jello_jdll_manifest vm_builtin_manifest = {
    JELLO_JDLL_MANIFEST_MAGIC,
    JELLO_JDLL_ABI_VERSION,
    "vm",
    VM_EXPORTS,
    (uint32_t)(sizeof(VM_EXPORTS) / sizeof(VM_EXPORTS[0])),
    NULL,
};

int vm_builtin_export(const jello_jdll_export* e) {
  return e >= VM_EXPORTS && e < VM_EXPORTS + vm_builtin_manifest.nexports;
}
