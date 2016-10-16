// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_JIT_ARM64_H
#define JELLO_JIT_ARM64_H

#include <stdint.h>

static inline uint32_t a64_movz_w(uint8_t rd, uint16_t imm, uint8_t shift) {
  return 0x52800000u | (((uint32_t)(shift / 16u) & 3u) << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}

static inline uint32_t a64_movk_w(uint8_t rd, uint16_t imm, uint8_t shift) {
  return 0x72800000u | (((uint32_t)(shift / 16u) & 3u) << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}

static inline uint32_t a64_movz_x(uint8_t rd, uint16_t imm, uint8_t shift) {
  return 0xD2800000u | (((uint32_t)(shift / 16u) & 3u) << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}

static inline uint32_t a64_movk_x(uint8_t rd, uint16_t imm, uint8_t shift) {
  return 0xF2800000u | (((uint32_t)(shift / 16u) & 3u) << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}

static inline uint32_t a64_sub_w_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x4B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_add_x_rr(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_add_x_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
  return 0x91000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_mul_w(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x1B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_ldr_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 4u;
  return 0xB9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_str_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 4u;
  return 0xB9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_ldr_x_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 8u;
  return 0xF9400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_str_x_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 8u;
  return 0xF9000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_sub_x_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
  return 0xD1000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_mul_x(uint8_t rd, uint8_t rn, uint8_t rm) {
  return 0x9B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}

static inline uint32_t a64_ldr_d_uimm(uint8_t dt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 8u;
  return 0xFD400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)dt;
}

static inline uint32_t a64_str_d_uimm(uint8_t dt, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 8u;
  return 0xFD000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)dt;
}

static inline uint32_t a64_ldr_s_uimm(uint8_t st, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 4u;
  return 0xBD400000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)st;
}

static inline uint32_t a64_str_s_uimm(uint8_t st, uint8_t rn, uint32_t byte_off) {
  uint32_t imm12 = byte_off / 4u;
  return 0xBD000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)st;
}

static inline uint32_t a64_cbz_w(uint8_t rt, int32_t off_insns) {
  uint32_t imm19 = ((uint32_t)off_insns & 0x7FFFFu) << 5;
  return 0x34000000u | imm19 | (uint32_t)rt;
}

static inline uint32_t a64_b(int32_t off_insns) {
  return 0x14000000u | ((uint32_t)off_insns & 0x03FFFFFFu);
}

static inline uint32_t a64_bl(int32_t off_insns) {
  return 0x94000000u | ((uint32_t)off_insns & 0x03FFFFFFu);
}

static inline uint32_t a64_blr_x(uint8_t xn) {
  return 0xD63F0000u | ((uint32_t)xn << 5);
}

static inline uint32_t a64_uxtw_x_w(uint8_t xd, uint8_t ws) {
  return 0xD3407C00u | ((uint32_t)ws << 5) | (uint32_t)xd;
}

static inline uint32_t a64_mov_w_rr(uint8_t rd, uint8_t rm) {
  return 0x2A0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}

static inline uint32_t a64_mov_x_rr(uint8_t rd, uint8_t rm) {
  return 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}

static inline uint32_t a64_ldrb_w_uimm(uint8_t rt, uint8_t rn, uint32_t byte_off) {
  return 0x39400000u | ((byte_off & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_ret(void) {
  return 0xD65F03C0u;
}

static inline uint32_t a64_stp_x_pre(uint8_t rt, uint8_t rt2, uint8_t rn, int32_t imm7) {
  return 0xA9800000u | (((uint32_t)imm7 & 0x7Fu) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static inline uint32_t a64_ldp_x_post(uint8_t rt, uint8_t rt2, uint8_t rn, int32_t imm7) {
  return 0xA8C00000u | (((uint32_t)imm7 & 0x7Fu) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

#endif
