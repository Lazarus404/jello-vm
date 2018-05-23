// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * x64 register-allocation backend; the register pools and the encoders that load and store a
 * register's home slot. */

#include <jello/internal/jit_impl.h>
#include <string.h>

const uint8_t jit_gpr_pool[] = {6, 7, 8, 9, 10, 11, 14};
const uint32_t jit_gpr_pool_len = 7;
const uint32_t jit_xmm_pool_len = 6;

void jit_spill_load_gpr64(jit_exec_buf* buf, unsigned dst_gpr, uint8_t vreg) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[8];
  bytes[0] = (uint8_t)(0x48u | ((dst_gpr >= 8) ? 0x04u : 0) | 0x01u);
  bytes[1] = 0x8B;
  bytes[2] = (uint8_t)(0x80u | ((dst_gpr & 7u) << 3) | 5u);
  memcpy(&bytes[3], &disp, 4);
  jit_exec_buf_write(buf, bytes, 7);
}

void jit_spill_store_gpr32(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x40u | ((src_gpr >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src_gpr & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

void jit_spill_store_gpr64(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[8];
  bytes[0] = (uint8_t)(0x48u | ((src_gpr >= 8) ? 0x04u : 0) | 0x01u);
  bytes[1] = 0x89;
  bytes[2] = (uint8_t)(0x80u | ((src_gpr & 7u) << 3) | 5u);
  memcpy(&bytes[3], &disp, 4);
  jit_exec_buf_write(buf, bytes, 7);
}

void jit_spill_load_xmm32(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[10];
  size_t n = 0;
  bytes[n++] = 0xF3;
  bytes[n++] = (uint8_t)(0x40u | ((dst_xmm >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x80u | ((dst_xmm & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

void jit_spill_load_xmm64(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[10];
  size_t n = 0;
  bytes[n++] = 0xF2;
  bytes[n++] = (uint8_t)(0x40u | ((dst_xmm >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x80u | ((dst_xmm & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

void jit_spill_store_xmm32(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[10];
  size_t n = 0;
  bytes[n++] = 0xF3;
  bytes[n++] = (uint8_t)(0x40u | ((src_xmm >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x80u | ((src_xmm & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

void jit_spill_store_xmm64(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[10];
  size_t n = 0;
  bytes[n++] = 0xF2;
  bytes[n++] = (uint8_t)(0x40u | ((src_xmm >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x80u | ((src_xmm & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}
