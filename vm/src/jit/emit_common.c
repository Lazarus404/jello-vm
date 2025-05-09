// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_impl.h>
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#endif

static size_t jit_icache_span(size_t n, size_t cap) {
  long page = sysconf(_SC_PAGESIZE);
  if(page < 64) page = 64;
  size_t span = (n + (size_t)page - 1u) & ~((size_t)page - 1u);
  if(span > cap) span = cap;
  return span;
}

static void jit_sync_icache(void* mem, size_t n) {
  if(!mem || n == 0) return;
#if defined(__APPLE__) && defined(__aarch64__)
  sys_icache_invalidate(mem, n);
#else
  __builtin___clear_cache((char*)mem, (char*)mem + n);
#endif
#if defined(__aarch64__) && !defined(__APPLE__)
  __asm__ __volatile__("dsb ish\n\tisb" ::: "memory");
#endif
}

/* -- the minimal W^X proof mechanism -- */

jit_exec_buf jit_alloc_exec_buf(size_t cap) {
  jit_exec_buf buf;
  buf.mem = NULL;
  buf.cap = cap;
  buf.len = 0;
  buf.finalized = 0;
  buf.overflowed = 0;
  buf.limit = cap;
  buf.append_from = 0;
  buf.append_end = 0;

  void* mem = mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(mem == MAP_FAILED) return buf;
  buf.mem = (uint8_t*)mem;
  return buf;
}

int jit_exec_buf_write(jit_exec_buf* buf, const void* bytes, size_t n) {
  if(!buf->mem || buf->finalized || buf->len + n > buf->limit) {
    buf->overflowed = 1;
    return 0;
  }
  memcpy(buf->mem + buf->len, bytes, n);
  buf->len += n;
  return 1;
}

int jit_finalize_exec_buf(jit_exec_buf* buf) {
  if(!buf->mem) return 0;
  if(mprotect(buf->mem, buf->cap, PROT_READ | PROT_EXEC) != 0) return 0;
  jit_sync_icache(buf->mem, jit_icache_span(buf->len, buf->cap));
  buf->finalized = 1;
  return 1;
}

static size_t page_floor(size_t n) { return n & ~(jello_page_size() - 1u); }

int jit_exec_buf_begin_append(jit_exec_buf* buf, size_t bound) {
  if(!buf->mem || !buf->finalized || buf->len >= buf->cap) return 0;
  size_t from = page_floor(buf->len);
  size_t limit = buf->cap - buf->len > bound ? buf->len + bound : buf->cap;
  size_t end = page_floor(limit + jello_page_size() - 1u);
  if(end > buf->cap) end = buf->cap;
  if(!jello_pages_protect(buf->mem + from, end - from, JELLO_PAGES_RW)) return 0;
  buf->append_from = buf->len;
  buf->append_end = end;
  buf->limit = limit;
  buf->finalized = 0;
  return 1;
}

int jit_exec_buf_end_append(jit_exec_buf* buf) {
  size_t from = page_floor(buf->append_from);
  if(!jello_pages_protect(buf->mem + from, buf->append_end - from, JELLO_PAGES_RX)) return 0;
  if(buf->len > buf->append_from)
    jit_sync_icache(buf->mem + buf->append_from, buf->len - buf->append_from);
  buf->limit = buf->cap;
  buf->finalized = 1;
  return 1;
}

void jit_free_exec_buf(jit_exec_buf* buf) {
  if(!buf->mem) return;
  munmap(buf->mem, buf->cap);
  buf->mem = NULL;
}

void jit_trap_arithmetic(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_ARITHMETIC;
}

void jit_trap_stack_overflow(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
}

void jit_trap_null_deref(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_NULL_DEREF;
}

void jit_trap_bounds(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_BOUNDS;
}

void jit_trap_type_mismatch(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_TYPE_MISMATCH;
}

void jit_trap_fuel(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_FUEL;
}

void jit_trap_limit(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_LIMIT;
}

static int jir_op_allowed(jello_jit_ir_op op, int allow_calls) {
  switch(op) {
    case JIR_NOP:
    case JIR_RET:
    case JIR_MOV:
    case JIR_JMP:
    case JIR_JMP_IF:
    case JIR_SAFEPOINT:
    case JIR_CONST_I8_IMM:
    case JIR_CONST_I32:
    case JIR_CONST_I64:
    case JIR_CONST_F16:
    case JIR_CONST_F32:
    case JIR_CONST_F64:
    case JIR_CONST_BOOL:
    case JIR_BIN_I32:
    case JIR_BIN_I64:
    case JIR_BIN_F16:
    case JIR_BIN_F32:
    case JIR_BIN_F64:
    case JIR_ADD_I32_IMM:
    case JIR_SUB_I32_IMM:
    case JIR_MUL_I32_IMM:
    case JIR_NEG_I32:
    case JIR_NEG_I64:
    case JIR_NEG_F32:
    case JIR_NEG_F64:
    case JIR_NOT_BOOL:
    case JIR_CMP_I32:
    case JIR_CMP_I64:
    case JIR_CMP_F32:
    case JIR_CMP_F64:
    case JIR_EQ_I32_IMM:
    case JIR_LT_I32_IMM:
    case JIR_CONV:
    case JIR_BYTES_NEW:
    case JIR_BYTES_LEN:
    case JIR_BYTES_GET_U8:
    case JIR_BYTES_SET_U8:
    case JIR_BYTES_EQ:
    case JIR_BYTES_CONCAT2:
    case JIR_BYTES_BITOP:
    case JIR_BYTES_READ:
    case JIR_BYTES_WRITE:
    case JIR_BYTES_BLIT:
    case JIR_BITS_WRITE:
    case JIR_BITS_READ:
    case JIR_BYTES_FREEZE:
    case JIR_STRUCT_NEW:
    case JIR_STRUCT_GET:
    case JIR_STRUCT_SET:
    case JIR_CONST_NULL:
    case JIR_CONST_ATOM:
    case JIR_TO_DYN:
    case JIR_FROM_DYN:
    case JIR_KINDOF:
    case JIR_PHYSEQ:
    case JIR_IS_NULL:
    case JIR_NULL_CHECK:
    case JIR_IS_ENUM:
    case JIR_OBJ_NEW:
    case JIR_OBJ_HAS_ATOM:
    case JIR_OBJ_GET_ATOM:
    case JIR_OBJ_SET_ATOM:
    case JIR_OBJ_GET:
    case JIR_OBJ_SET:
    case JIR_OBJ_HAS:
    case JIR_OBJ_REMOVE:
    case JIR_OBJ_CLEAR:
    case JIR_OBJ_COPY:
    case JIR_ARRAY_LEN:
    case JIR_ARRAY_GET:
    case JIR_ARRAY_SET:
    case JIR_GLOBAL_GET:
    case JIR_GLOBAL_SET:
    case JIR_ENUM_TAG:
    case JIR_ENUM_GET:
    case JIR_TRY:
    case JIR_ENDTRY:
    case JIR_THROW: return 1;
    case JIR_CALL_SELF:
    case JIR_CALL_DIRECT:
    case JIR_CALLR: return allow_calls;
    case JIR_SLOW:
    case JIR_CALL_NATIVE:
      return 1; // JIR_SLOW only when jello_jit_ir_slow_runs_handler allows its source op
    default: return 0;
  }
}

int jit_resolve_target_bc(uint32_t bc_pc, int32_t imm, uint32_t ninsns, uint32_t* out) {
  int64_t t = (int64_t)(int32_t)bc_pc + 1 + (int64_t)imm;
  if(t < 0 || t >= (int64_t)ninsns) return 0;
  *out = (uint32_t)t;
  return 1;
}

int jit_ir_shape_ok(const jello_jit_ir_function* ir, const jello_bc_module* m,
                    const jello_bc_function* f, uint32_t fi, jello_type_kind* out_ret_kind,
                    int allow_calls, uint32_t* bad_pc) {
  int saw_ret = 0;
  jello_type_kind ret_kind = JELLO_T_VOID;
  for(uint32_t i = 0; i < ir->ninsns; i++) {
    jello_jit_ir_op op = (jello_jit_ir_op)ir->insns[i].op;
    *bad_pc = ir->insns[i].bc_pc;
    if(!jir_op_allowed(op, allow_calls)) return 0;
    if(op == JIR_SLOW && !jello_jit_ir_slow_runs_handler((jello_op)f->insns[ir->insns[i].bc_pc].op))
      return 0;
    if(op == JIR_CALL_SELF || op == JIR_CALL_DIRECT) {
      uint32_t callee = (op == JIR_CALL_SELF) ? fi : ir->insns[i].imm;
      if(callee >= m->nfuncs) return 0;
    }
    if(op == JIR_JMP || op == JIR_JMP_IF || op == JIR_TRY) {
      uint32_t tgt = 0;
      if(!jit_resolve_target_bc(ir->insns[i].bc_pc, (int32_t)ir->insns[i].imm, f->ninsns, &tgt))
        return 0;
    }
    if(op == JIR_RET) {
      jello_type_kind k = vm_reg_kind(m, f, ir->insns[i].a);
      if(k == JELLO_T_F16) return 0;
      if(saw_ret && k != ret_kind) return 0;
      ret_kind = k;
      saw_ret = 1;
    }
  }
  *bad_pc = UINT32_MAX;
  *out_ret_kind = ret_kind;
  return 1;
}

int jit_scan_targets(const jello_jit_ir_function* ir, uint32_t ninsns, uint8_t* is_target,
                     uint8_t* is_loop_anchor, uint32_t* out_nloop_anchors, uint32_t** out_catch_pcs,
                     uint32_t* out_ncatch) {
  uint32_t nloop_anchors = 0;
  for(uint32_t i = 0; i < ir->ninsns; i++) {
    jello_jit_ir_op op = (jello_jit_ir_op)ir->insns[i].op;
    if(op != JIR_JMP && op != JIR_JMP_IF && op != JIR_TRY) continue;
    uint32_t tgt = 0; // in range; jit_ir_shape_ok checked it
    (void)jit_resolve_target_bc(ir->insns[i].bc_pc, (int32_t)ir->insns[i].imm, ninsns, &tgt);
    is_target[tgt] |= (uint8_t)(op == JIR_TRY ? 3u : 1u);
    if(op == JIR_TRY) continue;
    if(tgt <= ir->insns[i].bc_pc && !is_loop_anchor[tgt]) {
      is_loop_anchor[tgt] = 1;
      nloop_anchors++;
    }
  }
  *out_nloop_anchors = nloop_anchors;

  uint32_t ncatch = 0;
  for(uint32_t pc = 0; pc < ninsns; pc++) ncatch += (is_target[pc] >> 1) & 1u;
  uint32_t* catch_pcs = (uint32_t*)malloc(sizeof(uint32_t) * (ncatch ? ncatch : 1u));
  if(!catch_pcs) return 0;
  for(uint32_t pc = 0, w = 0; pc < ninsns; pc++) {
    if(is_target[pc] & 2u) catch_pcs[w++] = pc;
  }
  *out_catch_pcs = catch_pcs;
  *out_ncatch = ncatch;
  return 1;
}

int jit_list_push(void* items_ptr, size_t* count, size_t* cap, const void* item, size_t elem) {
  uint8_t* items;
  memcpy(&items, items_ptr, sizeof(items));
  if(*count == *cap) {
    size_t grown = *cap ? *cap * 2u : 16u;
    uint8_t* p = (uint8_t*)realloc(items, grown * elem);
    if(!p) return 0;
    items = p;
    *cap = grown;
    memcpy(items_ptr, &items, sizeof(items));
  }
  memcpy(items + *count * elem, item, elem);
  (*count)++;
  return 1;
}

int jit_decline(jello_jit_func_code* fc, jello_jit_decline why, const jello_bc_function* f,
                uint32_t pc) {
  fc->compiled = 0;
  fc->decline_reason = (uint8_t)why;
  fc->decline_pc = pc;
  fc->decline_op = (uint16_t)(pc < f->ninsns ? f->insns[pc].op : 0u);
  return 0;
}

void jit_push_trap_jump(emit_ctx* ctx, size_t at, uint8_t kind) {
  jit_trap_jump t = {at, kind};
  if(!jit_list_push(&ctx->trap_jumps, &ctx->trap_jump_count, &ctx->trap_jump_cap, &t, sizeof(t)))
    ctx->oom = 1;
}

void jit_push_branch(emit_ctx* ctx, size_t at, uint32_t bc_tgt) {
  jit_branch_patch b = {at, bc_tgt};
  if(!jit_list_push(&ctx->branches, &ctx->branch_count, &ctx->branch_cap, &b, sizeof(b)))
    ctx->oom = 1;
}

void jit_push_ret_jump(emit_ctx* ctx, size_t at) {
  if(!jit_list_push(&ctx->ret_jumps, &ctx->ret_jump_count, &ctx->ret_jump_cap, &at, sizeof(at)))
    ctx->oom = 1;
}

void jit_push_stack_overflow_jump(emit_ctx* ctx, size_t at) {
  if(!jit_list_push(&ctx->stack_overflow_jumps, &ctx->stack_overflow_jump_count,
                    &ctx->stack_overflow_jump_cap, &at, sizeof(at)))
    ctx->oom = 1;
}

uint32_t jit_branch_target(const emit_ctx* ctx, const jello_jit_ir_insn* ins) {
  uint32_t tgt = 0;
  (void)jit_resolve_target_bc(ins->bc_pc, (int32_t)ins->imm, ctx->f->ninsns, &tgt);
  return tgt;
}

typedef struct emit_scan {
  uint8_t* is_target;
  uint8_t* discarded;
  size_t* native_off;
  uint8_t* is_loop_anchor;
  uint32_t* catch_pcs;
  uint32_t ncatch, nloop_anchors;
} emit_scan;

static void emit_scan_free(emit_scan* s) {
  free(s->is_target);
  free(s->discarded);
  free(s->native_off);
  free(s->is_loop_anchor);
  free(s->catch_pcs);
}

static int emit_scan_build(emit_scan* s, const jello_jit_ir_function* ir, uint32_t ninsns) {
  memset(s, 0, sizeof *s);
  s->is_target = (uint8_t*)calloc(ninsns, 1);
  s->discarded = (uint8_t*)calloc(ninsns, 1);
  s->native_off = (size_t*)malloc(sizeof(size_t) * (size_t)ninsns);
  s->is_loop_anchor = (uint8_t*)calloc(ninsns, 1);
  if(!s->is_target || !s->discarded || !s->native_off || !s->is_loop_anchor) return 0;
  for(uint32_t i = 0; i < ninsns; i++) s->native_off[i] = (size_t)-1;
  return jit_scan_targets(ir, ninsns, s->is_target, s->is_loop_anchor, &s->nloop_anchors,
                          &s->catch_pcs, &s->ncatch);
}

static int emit_exits(emit_ctx* ctx, const size_t* native_off) {
  jit_exec_buf* buf = ctx->buf;
  size_t trap_off_by_kind[JIT_TRAP_KIND_COUNT];
  size_t ok_off = buf->len;
  jit_emit_ok_exit(ctx);
  trap_off_by_kind[JIT_TRAP_ARITH] = buf->len;
  jit_emit_trap_exit(ctx, (uintptr_t)jit_trap_arithmetic);
  trap_off_by_kind[JIT_TRAP_NULL] = buf->len;
  jit_emit_trap_exit(ctx, (uintptr_t)jit_trap_null_deref);
  trap_off_by_kind[JIT_TRAP_BOUNDS] = buf->len;
  jit_emit_trap_exit(ctx, (uintptr_t)jit_trap_bounds);
  trap_off_by_kind[JIT_TRAP_TYPE] = buf->len;
  jit_emit_trap_exit(ctx, (uintptr_t)jit_trap_type_mismatch);
  trap_off_by_kind[JIT_TRAP_PASSTHROUGH] = buf->len;
  if(ctx->ncatch) jit_emit_catch_dispatch(ctx);
  else jit_emit_trap_exit(ctx, 0);
  trap_off_by_kind[JIT_TRAP_CALLEE] = trap_off_by_kind[JIT_TRAP_PASSTHROUGH];
  trap_off_by_kind[JIT_TRAP_HELPER] = trap_off_by_kind[JIT_TRAP_PASSTHROUGH];
  trap_off_by_kind[JIT_TRAP_LIMIT] = buf->len;
  jit_emit_trap_exit(ctx, (uintptr_t)jit_trap_limit);
  size_t stack_overflow_off = buf->len;
  jit_emit_trap_exit(ctx, (uintptr_t)jit_trap_stack_overflow);

  for(size_t i = 0; i < ctx->trap_jump_count; i++)
    jit_patch_jump(buf, ctx->trap_jumps[i].at, trap_off_by_kind[ctx->trap_jumps[i].kind]);
  for(size_t i = 0; i < ctx->stack_overflow_jump_count; i++)
    jit_patch_jump(buf, ctx->stack_overflow_jumps[i], stack_overflow_off);
  for(size_t i = 0; i < ctx->ret_jump_count; i++) jit_patch_jump(buf, ctx->ret_jumps[i], ok_off);
  for(size_t i = 0; i < ctx->branch_count; i++) {
    uint32_t tgt = ctx->branches[i].bc_tgt;
    if(tgt >= ctx->f->ninsns || native_off[tgt] == (size_t)-1) return 0;
    jit_patch_jump(buf, ctx->branches[i].at, native_off[tgt]);
  }
  return 1;
}

static int record_loop_anchors(jello_jit_func_code* fc, const emit_scan* s, uint32_t ninsns,
                               size_t blob_start) {
  if(s->nloop_anchors == 0) return 1;
  fc->loop_anchor_pcs = (uint32_t*)malloc(sizeof(uint32_t) * s->nloop_anchors);
  fc->loop_anchor_offs = (size_t*)malloc(sizeof(size_t) * s->nloop_anchors);
  if(!fc->loop_anchor_pcs || !fc->loop_anchor_offs) return 0;
  uint32_t w = 0;
  for(uint32_t pc = 0; pc < ninsns; pc++) {
    if(!s->is_loop_anchor[pc]) continue;
    fc->loop_anchor_pcs[w] = pc;
    fc->loop_anchor_offs[w] = s->native_off[pc] - blob_start;
    w++;
  }
  fc->nloop_anchors = w;
  return 1;
}

static void free_ctx_lists(emit_ctx* ctx) {
  free(ctx->trap_jumps);
  free(ctx->branches);
  free(ctx->ret_jumps);
  free(ctx->stack_overflow_jumps);
}

int jello_jit_emit_function_into(const jello_bc_module* m, uint32_t fi, jit_exec_buf* buf,
                                 jello_jit_func_code* out_fc, const void* const* entries) {
  if(fi >= m->nfuncs) return 0;
  const jello_bc_function* f = &m->funcs[fi];

  jello_jit_ir_function* ir = jello_jit_ir_build(m, fi);
  if(!ir) return jit_decline(out_fc, JIT_DECLINE_IR_BUILD, f, UINT32_MAX);
  if(!jello_jit_ir_verify(ir, f)) {
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_VERIFY, f, UINT32_MAX);
  }

  jello_type_kind ret_kind = JELLO_T_VOID;
  uint32_t bad_pc = UINT32_MAX;
  if(!jit_ir_shape_ok(ir, m, f, fi, &ret_kind, entries != NULL, &bad_pc)) {
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_SHAPE, f, bad_pc);
  }

  emit_scan scan;
  if(!emit_scan_build(&scan, ir, f->ninsns)) {
    emit_scan_free(&scan);
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_OOM, f, UINT32_MAX);
  }

  // Branch targets bound the call sites' survive windows.
  jit_survive_plan splan;
  if(!jit_survive_plan_build(ir, m, f, scan.is_target, &splan)) {
    emit_scan_free(&scan);
    jello_jit_ir_free(ir);
    return jit_decline(out_fc, JIT_DECLINE_SURVIVE, f, UINT32_MAX);
  }

  size_t blob_start = buf->len;
  // A declined function leaves no code behind.
  int buf_overflowed_before = buf->overflowed;
  buf->overflowed = 0;
  emit_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.buf = buf;
  ctx.m = m;
  ctx.f = f;
  ctx.fi = fi;
  ctx.entries = entries;
  ctx.catch_pcs = scan.catch_pcs;
  ctx.ncatch = scan.ncatch;
  ctx.ret_kind = ret_kind;
  ctx.survive_any_armed = splan.any_armed;

  jit_reg_state ra;
  jit_reg_emit_ctx re;
  re.buf = buf;
  jit_reg_state_init(&ra, ir->nregs, re);
  ctx.ra = &ra;

  size_t body_off = 0;
  jit_emit_prologue(buf, &body_off, splan.any_armed);

  uint32_t fail_pc = UINT32_MAX;
  for(uint32_t i = 0; i < ir->ninsns && fail_pc == UINT32_MAX; i++) {
    const jello_jit_ir_insn* ins = &ir->insns[i];
    uint32_t bc = ins->bc_pc;
    ctx.cur_bc_pc = bc;
    // A branch target is reached with unknown bindings, so it starts with none.
    if(bc < f->ninsns && scan.is_target[bc] && !scan.discarded[bc]) {
      jit_reg_state_discard_currents(&ra);
      scan.discarded[bc] = 1;
    }
    if(bc < f->ninsns && scan.native_off[bc] == (size_t)-1) scan.native_off[bc] = buf->len;

    jello_jit_ir_op op = (jello_jit_ir_op)ins->op;
    if((op == JIR_CALL_SELF || op == JIR_CALL_DIRECT) && splan.armed[i])
      jit_reg_survive_hold(&ra, splan.candidate[i]);
    int ok = jit_emit_insn(&ctx, ins);
    if(splan.is_consumer[i] && jit_reg_survive_is_held(&ra)) jit_reg_survive_release(&ra);
    jit_reg_unpin_all(&ra);
    if(!ok || ctx.oom || buf->overflowed) fail_pc = bc;
  }

  out_fc->loop_anchor_pcs = NULL;
  out_fc->loop_anchor_offs = NULL;
  out_fc->nloop_anchors = 0;
  int ok = fail_pc == UINT32_MAX && emit_exits(&ctx, scan.native_off) &&
           record_loop_anchors(out_fc, &scan, f->ninsns, blob_start);

  jit_reg_state_free(&ra);
  jit_survive_plan_free(&splan);
  jello_jit_ir_free(ir);
  free_ctx_lists(&ctx);
  emit_scan_free(&scan);

  if(buf->overflowed || ctx.oom) ok = 0;
  buf->overflowed = buf_overflowed_before;
  if(!ok) {
    free(out_fc->loop_anchor_pcs);
    free(out_fc->loop_anchor_offs);
    out_fc->loop_anchor_pcs = NULL;
    out_fc->loop_anchor_offs = NULL;
    out_fc->nloop_anchors = 0;
    buf->len = blob_start;
    return jit_decline(out_fc, ctx.oom ? JIT_DECLINE_OOM : JIT_DECLINE_EMIT, f, fail_pc);
  }

  out_fc->compiled = 1;
  out_fc->decline_reason = JIT_DECLINE_NONE;
  out_fc->decline_pc = UINT32_MAX;
  out_fc->decline_op = 0;
  out_fc->blob_off = blob_start;
  out_fc->entry_off = 0;
  out_fc->body_off = body_off - blob_start;
  out_fc->size = buf->len - blob_start;
  return 1;
}

static size_t emit_buf_cap(uint32_t ninsns) {
  size_t cap = 256u + (size_t)ninsns * JIT_EMIT_PER_INSN_BUDGET + 512u;
  if(cap < JIT_EMIT_BUF_MIN_CAP) cap = JIT_EMIT_BUF_MIN_CAP;
  return cap;
}

int jello_jit_compile_function(const jello_bc_module* m, uint32_t fi, jello_jit_code* out_code) {
  memset(&out_code->buf, 0, sizeof out_code->buf);
  out_code->entry_off = 0;
  out_code->body_off = 0;
  out_code->loop_anchor_pcs = NULL;
  out_code->loop_anchor_offs = NULL;
  out_code->nloop_anchors = 0;

  if(fi >= m->nfuncs) return 0;
  jello_jit_ir_function* ir = jello_jit_ir_build(m, fi);
  if(!ir) return 0;
  uint32_t ninsns = ir->ninsns;
  jello_jit_ir_free(ir);

  jit_exec_buf buf = jit_alloc_exec_buf(emit_buf_cap(ninsns));
  if(!buf.mem) return 0;

  jello_jit_func_code fc;
  memset(&fc, 0, sizeof(fc));
  if(!jello_jit_emit_function_into(m, fi, &buf, &fc, NULL)) {
    jit_free_exec_buf(&buf);
    return 0;
  }

  if(!jit_finalize_exec_buf(&buf)) {
    free(fc.loop_anchor_pcs);
    free(fc.loop_anchor_offs);
    jit_free_exec_buf(&buf);
    return 0;
  }

  out_code->buf = buf;
  out_code->entry_off = fc.entry_off;
  out_code->body_off = fc.body_off;
  out_code->loop_anchor_pcs = fc.loop_anchor_pcs;
  out_code->loop_anchor_offs = fc.loop_anchor_offs;
  out_code->nloop_anchors = fc.nloop_anchors;
  return 1;
}

void jello_jit_code_free(jello_jit_code* code) {
  free(code->loop_anchor_pcs);
  free(code->loop_anchor_offs);
  code->loop_anchor_pcs = NULL;
  code->loop_anchor_offs = NULL;
  code->nloop_anchors = 0;
  jit_free_exec_buf(&code->buf);
}
