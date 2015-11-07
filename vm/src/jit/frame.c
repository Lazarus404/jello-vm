// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_impl.h>
#include <jello/internal/vm_internal.h>

/* -- the JIT frame is Chapter 3's frame, unmodified -- */
uint8_t* jit_frame_entry_base(jello_vm* vm) {
  call_frame* fr = &vm->call_frames[vm->call_frames_len - 1u];
  return fr->rf.mem;
}
