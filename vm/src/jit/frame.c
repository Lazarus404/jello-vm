// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_impl.h>
#include <jello/internal/vm_internal.h>
#include <string.h>

/* -- The JIT frame is Chapter 3's frame, unmodified -- */
uint8_t* jit_frame_entry_base(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  return fr->rf.mem;
}


/* -- Small C helpers a JIT-compiled Bytes op calls directly -- */
void jit_frame_zero_and_null_init(uint8_t* mem, uint32_t total, const uint8_t* kind, uint32_t nregs) {
  memset(mem, 0, total);
  for(uint32_t r = 0; r < nregs; r++) {
    if((jello_type_kind)kind[r] == JELLO_T_DYNAMIC) {
      jello_value v = jello_make_null();
      memcpy(mem + (size_t)r * REG_SLOT_BYTES, &v, sizeof(v));
    }
  }
}
