// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Interpreter handlers for Bytes ops; every ranged or multi-byte access is bounds-checked in
 * 64-bit arithmetic, so an offset plus a width cannot wrap back into range. */

#include <jello/internal.h>

static call_frame* current_frame(exec_ctx* ctx) {
  return &ctx->vm->call_frames[ctx->vm->call_frames_len - 1u];
}

op_result op_const_bytes(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  const jello_bc_module* m = ctx->m;
  uint32_t idx = ins->imm;
  if(idx >= m->nconst_bytes) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  vm_store_ptr(&fr->rf, ins->a, m->const_bytes_values[idx]);
  return OP_CONTINUE;
}

op_result op_bytes_new(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  uint32_t len = vm_load_u32(&fr->rf, ins->b);
  if(len > (uint32_t)INT32_MAX) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  if(ctx->vm->max_bytes_len && len > ctx->vm->max_bytes_len) {
    ctx->vm->trap_code = JELLO_TRAP_LIMIT;
    return OP_TRAP;
  }
  jello_bytes* b = vm_buffer_new(ctx->vm, len);
  void* p = (void*)b;
  memcpy(vm_reg_ptr(&fr->rf, ins->a), &p, sizeof(void*));
  return OP_CONTINUE;
}

op_result op_bytes_len(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  void* p = NULL;
  memcpy(&p, vm_reg_ptr(&fr->rf, ins->b), sizeof(void*));
  if(!p) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_bytes* b = (jello_bytes*)p;
  vm_store_u32(&fr->rf, ins->a, b->length);
  return OP_CONTINUE;
}

op_result op_bytes_get_u8(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  void* p = NULL;
  memcpy(&p, vm_reg_ptr(&fr->rf, ins->b), sizeof(void*));
  if(!p) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_bytes* b = (jello_bytes*)p;
  uint32_t idx = vm_load_u32(&fr->rf, ins->c);
  if(idx >= b->length) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  vm_store_u32(&fr->rf, ins->a, (uint32_t)b->data[idx]);
  return OP_CONTINUE;
}

op_result op_bytes_set_u8(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  void* p = NULL;
  memcpy(&p, vm_reg_ptr(&fr->rf, ins->b), sizeof(void*));
  if(!p) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return OP_TRAP;
  }
  jello_bytes* b = (jello_bytes*)p;
  uint32_t idx = vm_load_u32(&fr->rf, ins->c);
  if(idx >= b->length) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint32_t v = vm_load_u32(&fr->rf, ins->a);
  if(v > 255u) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  b->data[idx] = (uint8_t)v;
  return OP_CONTINUE;
}

/* -- Memory-layout Bytes opcodes -- */

static jello_bytes* bytes_arg(exec_ctx* ctx, call_frame* fr, uint32_t r) {
  void* p = vm_load_ptr(&fr->rf, r);
  if(!p) {
    ctx->vm->trap_code = JELLO_TRAP_NULL_DEREF;
    return NULL;
  }
  return (jello_bytes*)p;
}

static int bytes_access_in_range(const jello_bytes* b, uint32_t offset, uint32_t width) {
  return (uint64_t)offset + (uint64_t)width <= (uint64_t)b->length;
}

op_result op_bytes_concat2(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = bytes_arg(ctx, fr, ins->b);
  if(!b) return OP_TRAP;
  jello_bytes* c = bytes_arg(ctx, fr, ins->c);
  if(!c) return OP_TRAP;
  uint64_t total = (uint64_t)b->length + (uint64_t)c->length;
  if(total > (uint64_t)UINT32_MAX) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  jello_bytes* dst = jello_bytes_new(ctx->vm, (uint32_t)total);
  memcpy(dst->data, b->data, b->length);
  memcpy(dst->data + b->length, c->data, c->length);
  vm_store_ptr(&fr->rf, ins->a, dst);
  return OP_CONTINUE;
}

op_result op_bytes_eq(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = (jello_bytes*)vm_load_ptr(&fr->rf, ins->b);
  jello_bytes* c = (jello_bytes*)vm_load_ptr(&fr->rf, ins->c);
  int eq;
  if(b == c) eq = 1;
  else if(!b || !c) eq = 0;
  else eq = b->length == c->length && memcmp(b->data, c->data, b->length) == 0;
  vm_store_u32(&fr->rf, ins->a, (uint32_t)(eq ? 1 : 0));
  return OP_CONTINUE;
}

static op_result bytes_bitwise2(exec_ctx* ctx, const jello_insn* ins, int op_kind) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = bytes_arg(ctx, fr, ins->b);
  if(!b) return OP_TRAP;
  jello_bytes* c = bytes_arg(ctx, fr, ins->c);
  if(!c) return OP_TRAP;
  if(b->length != c->length) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  jello_bytes* dst = jello_bytes_new(ctx->vm, b->length);
  uint32_t n = b->length;
  const uint8_t* bd = b->data;
  const uint8_t* cd = c->data;
  uint8_t* dd = dst->data;
  uint32_t i = 0;
  for(; i + 8u <= n; i += 8u) {
    uint64_t wb, wc, wr;
    memcpy(&wb, bd + i, 8);
    memcpy(&wc, cd + i, 8);
    switch(op_kind) {
      case 0: wr = wb & wc; break;
      case 1: wr = wb | wc; break;
      default: wr = wb ^ wc; break;
    }
    memcpy(dd + i, &wr, 8);
  }
  for(; i < n; i++) {
    switch(op_kind) {
      case 0: dd[i] = (uint8_t)(bd[i] & cd[i]); break;
      case 1: dd[i] = (uint8_t)(bd[i] | cd[i]); break;
      default: dd[i] = (uint8_t)(bd[i] ^ cd[i]); break;
    }
  }
  vm_store_ptr(&fr->rf, ins->a, dst);
  return OP_CONTINUE;
}

op_result op_bytes_bitand2(exec_ctx* ctx, const jello_insn* ins) { return bytes_bitwise2(ctx, ins, 0); }
op_result op_bytes_bitor2(exec_ctx* ctx, const jello_insn* ins) { return bytes_bitwise2(ctx, ins, 1); }
op_result op_bytes_bitxor2(exec_ctx* ctx, const jello_insn* ins) { return bytes_bitwise2(ctx, ins, 2); }

static uint64_t rd_le(const uint8_t* p, uint32_t width) {
  uint64_t v = 0;
  for(uint32_t i = width; i-- > 0;) v = (v << 8) | p[i];
  return v;
}
static uint64_t rd_be(const uint8_t* p, uint32_t width) {
  uint64_t v = 0;
  for(uint32_t i = 0; i < width; i++) v = (v << 8) | p[i];
  return v;
}
static void wr_le(uint8_t* p, uint64_t v, uint32_t width) {
  for(uint32_t i = 0; i < width; i++) {
    p[i] = (uint8_t)(v & 0xFFu);
    v >>= 8;
  }
}
static void wr_be(uint8_t* p, uint64_t v, uint32_t width) {
  for(uint32_t i = width; i-- > 0;) {
    p[i] = (uint8_t)(v & 0xFFu);
    v >>= 8;
  }
}

static op_result bytes_read_int(exec_ctx* ctx, const jello_insn* ins, uint32_t width, int be) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = bytes_arg(ctx, fr, ins->b);
  if(!b) return OP_TRAP;
  uint32_t off = vm_load_u32(&fr->rf, ins->c);
  if(!bytes_access_in_range(b, off, width)) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint64_t raw = be ? rd_be(b->data + off, width) : rd_le(b->data + off, width);
  if(width == 8u) vm_store_i64(&fr->rf, ins->a, (int64_t)raw);
  else vm_store_u32(&fr->rf, ins->a, (uint32_t)raw);
  return OP_CONTINUE;
}

static op_result bytes_read_float(exec_ctx* ctx, const jello_insn* ins, uint32_t width, int be) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = bytes_arg(ctx, fr, ins->b);
  if(!b) return OP_TRAP;
  uint32_t off = vm_load_u32(&fr->rf, ins->c);
  if(!bytes_access_in_range(b, off, width)) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint64_t raw = be ? rd_be(b->data + off, width) : rd_le(b->data + off, width);
  if(width == 8u) {
    double d;
    uint64_t bits = raw;
    memcpy(&d, &bits, sizeof d);
    vm_store_f64(&fr->rf, ins->a, d);
  } else {
    float f;
    uint32_t bits = (uint32_t)raw;
    memcpy(&f, &bits, sizeof f);
    vm_store_f32(&fr->rf, ins->a, f);
  }
  return OP_CONTINUE;
}

static op_result bytes_write_int(exec_ctx* ctx, const jello_insn* ins, uint32_t width, int be) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = bytes_arg(ctx, fr, ins->b);
  if(!b) return OP_TRAP;
  uint32_t off = vm_load_u32(&fr->rf, ins->c);
  if(!bytes_access_in_range(b, off, width)) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint64_t raw = (width == 8u) ? (uint64_t)vm_load_i64(&fr->rf, ins->a) : (uint64_t)vm_load_u32(&fr->rf, ins->a);
  if(be) wr_be(b->data + off, raw, width);
  else wr_le(b->data + off, raw, width);
  return OP_CONTINUE;
}

static op_result bytes_write_float(exec_ctx* ctx, const jello_insn* ins, uint32_t width, int be) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = bytes_arg(ctx, fr, ins->b);
  if(!b) return OP_TRAP;
  uint32_t off = vm_load_u32(&fr->rf, ins->c);
  if(!bytes_access_in_range(b, off, width)) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint64_t raw;
  if(width == 8u) {
    double d = vm_load_f64(&fr->rf, ins->a);
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    raw = bits;
  } else {
    float f = vm_load_f32(&fr->rf, ins->a);
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    raw = bits;
  }
  if(be) wr_be(b->data + off, raw, width);
  else wr_le(b->data + off, raw, width);
  return OP_CONTINUE;
}

op_result op_bytes_read_u16_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_int(ctx, ins, 2, 0); }
op_result op_bytes_read_u16_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_int(ctx, ins, 2, 1); }
op_result op_bytes_read_u32_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_int(ctx, ins, 4, 0); }
op_result op_bytes_read_u32_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_int(ctx, ins, 4, 1); }
op_result op_bytes_read_i32_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_int(ctx, ins, 4, 0); }
op_result op_bytes_read_i32_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_int(ctx, ins, 4, 1); }
op_result op_bytes_read_i64_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_int(ctx, ins, 8, 0); }
op_result op_bytes_read_i64_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_int(ctx, ins, 8, 1); }
op_result op_bytes_read_f32_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_float(ctx, ins, 4, 0); }
op_result op_bytes_read_f32_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_float(ctx, ins, 4, 1); }
op_result op_bytes_read_f64_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_float(ctx, ins, 8, 0); }
op_result op_bytes_read_f64_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_read_float(ctx, ins, 8, 1); }

op_result op_bytes_write_u16_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_int(ctx, ins, 2, 0); }
op_result op_bytes_write_u16_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_int(ctx, ins, 2, 1); }
op_result op_bytes_write_u32_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_int(ctx, ins, 4, 0); }
op_result op_bytes_write_u32_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_int(ctx, ins, 4, 1); }
op_result op_bytes_write_i32_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_int(ctx, ins, 4, 0); }
op_result op_bytes_write_i32_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_int(ctx, ins, 4, 1); }
op_result op_bytes_write_i64_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_int(ctx, ins, 8, 0); }
op_result op_bytes_write_i64_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_int(ctx, ins, 8, 1); }
op_result op_bytes_write_f32_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_float(ctx, ins, 4, 0); }
op_result op_bytes_write_f32_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_float(ctx, ins, 4, 1); }
op_result op_bytes_write_f64_le(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_float(ctx, ins, 8, 0); }
op_result op_bytes_write_f64_be(exec_ctx* ctx, const jello_insn* ins) { return bytes_write_float(ctx, ins, 8, 1); }

op_result op_bits_write(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = bytes_arg(ctx, fr, ins->a);
  if(!b) return OP_TRAP;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t total_bits = (uint64_t)b->length * 8u;
  if((uint64_t)bit_off + (uint64_t)width > total_bits) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint32_t value = vm_load_u32(&fr->rf, ins->b);
  for(uint32_t i = 0; i < width; i++) {
    uint32_t bit_val = (value >> (width - 1u - i)) & 1u;
    uint64_t dst_bit = (uint64_t)bit_off + i;
    uint32_t byte_idx = (uint32_t)(dst_bit / 8u);
    uint32_t bit_in_byte = (uint32_t)(dst_bit % 8u);
    uint8_t mask = (uint8_t)(0x80u >> bit_in_byte);
    if(bit_val) b->data[byte_idx] = (uint8_t)(b->data[byte_idx] | mask);
    else b->data[byte_idx] = (uint8_t)(b->data[byte_idx] & (uint8_t)~mask);
  }
  return OP_CONTINUE;
}

op_result op_bits_read(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* b = bytes_arg(ctx, fr, ins->b);
  if(!b) return OP_TRAP;
  uint32_t width = ins->c;
  uint32_t bit_off = ins->imm;
  uint64_t total_bits = (uint64_t)b->length * 8u;
  if((uint64_t)bit_off + (uint64_t)width > total_bits) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  uint32_t value = 0;
  for(uint32_t i = 0; i < width; i++) {
    uint64_t src_bit = (uint64_t)bit_off + i;
    uint32_t byte_idx = (uint32_t)(src_bit / 8u);
    uint32_t bit_in_byte = (uint32_t)(src_bit % 8u);
    uint32_t bit_val = (uint32_t)(b->data[byte_idx] >> (7u - bit_in_byte)) & 1u;
    value = (value << 1) | bit_val;
  }
  vm_store_u32(&fr->rf, ins->a, value);
  return OP_CONTINUE;
}

op_result op_bytes_blit(exec_ctx* ctx, const jello_insn* ins) {
  call_frame* fr = current_frame(ctx);
  jello_bytes* dst = bytes_arg(ctx, fr, ins->a);
  if(!dst) return OP_TRAP;
  jello_bytes* src = bytes_arg(ctx, fr, ins->b);
  if(!src) return OP_TRAP;
  uint32_t off = vm_load_u32(&fr->rf, ins->c);
  if(!bytes_access_in_range(dst, off, src->length)) {
    ctx->vm->trap_code = JELLO_TRAP_BOUNDS;
    return OP_TRAP;
  }
  memmove(dst->data + off, src->data, src->length);
  return OP_CONTINUE;
}
