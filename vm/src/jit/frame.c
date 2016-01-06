// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_impl.h>
#include <jello/internal/vm_internal.h>
#include <assert.h>
#include <string.h>

/* -- The JIT frame is Chapter 3's frame, unmodified -- */
uint8_t* jit_frame_entry_base(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  return fr->rf.mem;
}


/* -- Small C helpers a JIT-compiled Bytes op calls directly -- */
jello_bytes* jit_bytes_bitop(jello_vm* vm, jello_bytes* b, jello_bytes* c, int op_kind) {
  if(b->length != c->length) {
    vm->trap_code = JELLO_TRAP_BOUNDS;
    return NULL;
  }
  jello_bytes* dst = jello_bytes_new(vm, b->length);
  uint32_t n = b->length;
  for(uint32_t i = 0; i < n; i++) {
    switch(op_kind) {
      case 0: dst->data[i] = (uint8_t)(b->data[i] & c->data[i]); break;
      case 1: dst->data[i] = (uint8_t)(b->data[i] | c->data[i]); break;
      default: dst->data[i] = (uint8_t)(b->data[i] ^ c->data[i]); break;
    }
  }
  return dst;
}

void jit_bits_write(jello_bytes* b, uint32_t width, uint32_t bit_off, uint32_t value) {
  for(uint32_t i = 0; i < width; i++) {
    uint32_t bit_val = (value >> (width - 1u - i)) & 1u;
    uint64_t dst_bit = (uint64_t)bit_off + i;
    uint32_t byte_idx = (uint32_t)(dst_bit / 8u);
    uint32_t bit_in_byte = (uint32_t)(dst_bit % 8u);
    uint8_t mask = (uint8_t)(0x80u >> bit_in_byte);
    if(bit_val) b->data[byte_idx] = (uint8_t)(b->data[byte_idx] | mask);
    else b->data[byte_idx] = (uint8_t)(b->data[byte_idx] & (uint8_t)~mask);
  }
}

void jit_frame_zero_and_null_init(uint8_t* mem, uint32_t total, const uint8_t* kind, uint32_t nregs) {
  memset(mem, 0, total);
  for(uint32_t r = 0; r < nregs; r++) {
    if((jello_type_kind)kind[r] == JELLO_T_DYNAMIC) {
      jello_value v = jello_make_null();
      memcpy(mem + (size_t)r * REG_SLOT_BYTES, &v, sizeof(v));
    }
  }
}

jello_bytes* jit_bytes_concat2(jello_vm* vm, jello_bytes* b, jello_bytes* c) {
  uint64_t total = (uint64_t)b->length + (uint64_t)c->length;
  if(total > (uint64_t)UINT32_MAX) {
    vm->trap_code = JELLO_TRAP_BOUNDS;
    return NULL;
  }
  jello_bytes* dst = jello_bytes_new(vm, (uint32_t)total);
  memcpy(dst->data, b->data, b->length);
  memcpy(dst->data + b->length, c->data, c->length);
  return dst;
}

void jit_debug_assert_rsp_aligned16(uint64_t rsp) {
  (void)rsp;
  assert((rsp & 0xFu) == 0 && "body_off's own 16-byte call alignment was broken by a survive-slot stack adjustment");
}

uint32_t jit_bits_read(const jello_bytes* b, uint32_t width, uint32_t bit_off) {
  uint32_t value = 0;
  for(uint32_t i = 0; i < width; i++) {
    uint64_t src_bit = (uint64_t)bit_off + i;
    uint32_t byte_idx = (uint32_t)(src_bit / 8u);
    uint32_t bit_in_byte = (uint32_t)(src_bit % 8u);
    uint32_t bit_val = (uint32_t)(b->data[byte_idx] >> (7u - bit_in_byte)) & 1u;
    value = (value << 1) | bit_val;
  }
  return value;
}
