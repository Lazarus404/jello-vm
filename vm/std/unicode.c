// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* UTF-8 over Bytes. Indexes count codepoints; invalid UTF-8 fails the call,
 * except in unicode_validate, which reports it. */

#include "std.h"

#include <stddef.h>

static int valid_codepoint(uint32_t cp) {
  return cp <= 0x10FFFFu && (cp < 0xD800u || cp > 0xDFFFu);
}

static int decode(const uint8_t* s, uint32_t len, uint32_t at, uint32_t* cp, uint32_t* size) {
  static const uint8_t SIZES[16] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 2, 2, 3, 4};
  static const uint32_t MIN[5] = {0, 0, 0x80u, 0x800u, 0x10000u};
  if(at >= len) return 0;
  uint32_t n = SIZES[s[at] >> 4];
  if(n == 0 || n > len - at) return 0;
  uint32_t c = n == 1 ? s[at] : (uint32_t)(s[at] & (0x7Fu >> n));
  for(uint32_t i = 1; i < n; i++) {
    if((s[at + i] & 0xC0u) != 0x80u) return 0;
    c = (c << 6) | (uint32_t)(s[at + i] & 0x3Fu);
  }
  if(c < MIN[n] || !valid_codepoint(c)) return 0;
  *cp = c;
  *size = n;
  return 1;
}

int std_utf8_valid(const uint8_t* s, uint32_t len) {
  uint32_t at = 0, cp = 0, size = 0;
  while(at < len && decode(s, len, at, &cp, &size)) at += size;
  return at == len;
}

void std_unicode_length(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  uint32_t at = 0, cp = 0, size = 0;
  int32_t n = 0;
  while(at < len) {
    if(!decode(s, len, at, &cp, &size)) {
      jdl_fail(c, "invalid UTF-8");
      return;
    }
    at += size;
    n++;
  }
  jdl_return_i32(c, n);
}

void std_unicode_compare(jdlo_ctx* c) {
  const uint8_t* a = jdl_arg_bytes_data(c, 0);
  uint32_t alen = jdl_arg_bytes_len(c, 0);
  const uint8_t* b = jdl_arg_bytes_data(c, 1);
  uint32_t blen = jdl_arg_bytes_len(c, 1);
  uint32_t i = 0, j = 0;
  for(;;) {
    uint32_t ca = 0, cb = 0, sa = 0, sb = 0;
    int ha = decode(a, alen, i, &ca, &sa), hb = decode(b, blen, j, &cb, &sb);
    if(!ha || !hb || ca != cb) {
      jdl_return_i32(c, ha == hb ? (ha ? (ca < cb ? -1 : 1) : 0) : (ha ? 1 : -1));
      return;
    }
    i += sa;
    j += sb;
  }
}

void std_unicode_from_codepoint(jdlo_ctx* c) {
  int32_t value = jdl_arg_i32(c, 0);
  uint32_t cp = (uint32_t)value;
  if(value < 0 || !valid_codepoint(cp)) {
    jdl_fail(c, "invalid codepoint");
    return;
  }
  uint8_t out[4];
  uint32_t n = cp < 0x80u ? 1u : cp < 0x800u ? 2u : cp < 0x10000u ? 3u : 4u;
  static const uint8_t LEAD[5] = {0, 0, 0xC0u, 0xE0u, 0xF0u};
  for(uint32_t i = n - 1; i > 0; i--) {
    out[i] = (uint8_t)(0x80u | (cp & 0x3Fu));
    cp >>= 6;
  }
  out[0] = (uint8_t)(n == 1 ? cp : (LEAD[n] | cp));
  jdl_return_bytes_copy(c, out, n);
}