// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* UTF-8 over Bytes. Indexes count codepoints; invalid UTF-8 fails the call,
 * except in unicode_validate, which reports it. */

#include "std.h"

#include <stddef.h>
#include <stdlib.h>

#include "unicode_norm.h"

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

static int64_t offset_of(const uint8_t* s, uint32_t len, int32_t index) {
  if(index < 0) return -1;
  uint32_t at = 0, cp = 0, size = 0;
  for(int32_t k = 0; k < index; k++) {
    if(!decode(s, len, at, &cp, &size)) return -1;
    at += size;
  }
  return at;
}

int std_utf8_valid(const uint8_t* s, uint32_t len) {
  uint32_t at = 0, cp = 0, size = 0;
  while(at < len && decode(s, len, at, &cp, &size)) at += size;
  return at == len;
}

void std_unicode_validate(jdlo_ctx* c) {
  jdl_return_bool(c, std_utf8_valid(jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0)));
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

void std_unicode_get(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  int64_t at = offset_of(s, len, jdl_arg_i32(c, 1));
  uint32_t cp = 0, size = 0;
  if(at < 0 || !decode(s, len, (uint32_t)at, &cp, &size)) {
    jdl_fail(c, "index out of range");
    return;
  }
  jdl_return_i32(c, (int32_t)cp);
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

void std_unicode_sub(jdlo_ctx* c) {
  const uint8_t* s = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  int32_t count = jdl_arg_i32(c, 2);
  int64_t start = offset_of(s, len, jdl_arg_i32(c, 1));
  if(start < 0 || count < 0) {
    jdl_fail(c, "range out of bounds");
    return;
  }
  uint32_t at = (uint32_t)start, cp = 0, size = 0;
  for(int32_t k = 0; k < count; k++) {
    if(!decode(s, len, at, &cp, &size)) {
      jdl_fail(c, "range out of bounds");
      return;
    }
    at += size;
  }
  jdl_return_bytes_copy(c, s + start, at - (uint32_t)start);
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

enum { NORM_NFC, NORM_NFD, NORM_NFKC, NORM_NFKD };
enum { SBASE = 0xAC00, LBASE = 0x1100, VBASE = 0x1161, TBASE = 0x11A7, LCOUNT = 19,
       VCOUNT = 21, TCOUNT = 28, NCOUNT = VCOUNT * TCOUNT, SCOUNT = LCOUNT * NCOUNT };

static const unicode_decomp* decomp_of(uint32_t cp) {
  uint32_t lo = 0, hi = (uint32_t)(sizeof UNICODE_DECOMP / sizeof *UNICODE_DECOMP);
  while(lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if(UNICODE_DECOMP[mid].cp < cp) lo = mid + 1; else hi = mid;
  }
  return lo < sizeof UNICODE_DECOMP / sizeof *UNICODE_DECOMP && UNICODE_DECOMP[lo].cp == cp ? UNICODE_DECOMP + lo : NULL;
}

static uint8_t ccc_of(uint32_t cp) {
  uint32_t lo = 0, hi = (uint32_t)(sizeof UNICODE_CCC / sizeof *UNICODE_CCC);
  while(lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if(UNICODE_CCC[mid].cp < cp) lo = mid + 1; else hi = mid;
  }
  return lo < sizeof UNICODE_CCC / sizeof *UNICODE_CCC && UNICODE_CCC[lo].cp == cp ? UNICODE_CCC[lo].value : 0;
}

static uint32_t compose_of(uint32_t first, uint32_t second) {
  uint32_t lo = 0, hi = (uint32_t)(sizeof UNICODE_COMPOSE / sizeof *UNICODE_COMPOSE);
  while(lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const unicode_compose* value = UNICODE_COMPOSE + mid;
    if(value->first < first || (value->first == first && value->second < second)) lo = mid + 1; else hi = mid;
  }
  return lo < sizeof UNICODE_COMPOSE / sizeof *UNICODE_COMPOSE && UNICODE_COMPOSE[lo].first == first && UNICODE_COMPOSE[lo].second == second ? UNICODE_COMPOSE[lo].composed : 0;
}

static int push(uint32_t** out, uint32_t* len, uint32_t* cap, uint32_t cp) {
  if(*len == *cap) {
    uint32_t next = *cap ? *cap * 2 : 32;
    uint32_t* grown = realloc(*out, (size_t)next * sizeof **out);
    if(!grown) return 0;
    *out = grown;
    *cap = next;
  }
  (*out)[(*len)++] = cp;
  return 1;
}

static int decompose(uint32_t** out, uint32_t* len, uint32_t* cap, uint32_t cp, int compat) {
  if(cp >= SBASE && cp < SBASE + SCOUNT) {
    uint32_t s = cp - SBASE, t = s % TCOUNT;
    return decompose(out, len, cap, LBASE + s / NCOUNT, compat) &&
           decompose(out, len, cap, VBASE + (s % NCOUNT) / TCOUNT, compat) &&
           (!t || decompose(out, len, cap, TBASE + t, compat));
  }
  const unicode_decomp* value = decomp_of(cp);
  if(!value || (value->compat && !compat)) return push(out, len, cap, cp);
  for(uint32_t i = 0; i < value->len; i++) if(!decompose(out, len, cap, UNICODE_DECOMP_DATA[value->at + i], compat)) return 0;
  return 1;
}

static uint32_t hangul_compose(uint32_t first, uint32_t second) {
  if(first >= LBASE && first < LBASE + LCOUNT && second >= VBASE && second < VBASE + VCOUNT)
    return SBASE + ((first - LBASE) * VCOUNT + second - VBASE) * TCOUNT;
  if(first >= SBASE && first < SBASE + SCOUNT && (first - SBASE) % TCOUNT == 0 && second > TBASE && second < TBASE + TCOUNT)
    return first + second - TBASE;
  return 0;
}

void std_unicode_normalize(jdlo_ctx* c) {
  const uint8_t* text = jdl_arg_bytes_data(c, 0);
  uint32_t len = jdl_arg_bytes_len(c, 0);
  int32_t form = jdl_arg_i32(c, 1);
  if(form < NORM_NFC || form > NORM_NFKD) { jdl_fail(c, "invalid normalization form"); return; }
  uint32_t at = 0, cp = 0, size = 0;
  int ascii = 1;
  while(at < len) {
    if(!decode(text, len, at, &cp, &size)) { jdl_fail(c, "invalid UTF-8"); return; }
    if(cp >= 0x80) ascii = 0;
    at += size;
  }
  if(ascii) { jdl_return_bytes_copy(c, text, len); return; }
  uint32_t *codepoints = NULL, count = 0, cap = 0;
  at = 0;
  while(at < len) {
    decode(text, len, at, &cp, &size);
    if(!decompose(&codepoints, &count, &cap, cp, form == NORM_NFKC || form == NORM_NFKD)) goto oom;
    at += size;
  }
  for(uint32_t i = 1; i < count; i++) {
    uint32_t value = codepoints[i], class = ccc_of(value), j = i;
    while(j && class && ccc_of(codepoints[j - 1]) > class) { codepoints[j] = codepoints[j - 1]; j--; }
    codepoints[j] = value;
  }
  if(form == NORM_NFC || form == NORM_NFKC) {
    uint32_t write = 0, starter = 0, last_class = 0;
    for(uint32_t i = 0; i < count; i++) {
      uint32_t class = ccc_of(codepoints[i]);
      uint32_t composed = write ? hangul_compose(codepoints[starter], codepoints[i]) : 0;
      if(!composed && write) composed = compose_of(codepoints[starter], codepoints[i]);
      if(composed && (last_class < class || class == 0)) codepoints[starter] = composed;
      else { if(class == 0) starter = write; codepoints[write++] = codepoints[i]; last_class = class; }
    }
    count = write;
  }
  uint8_t* out = malloc((size_t)count * 4);
  if(!out) goto oom;
  uint32_t outlen = 0;
  for(uint32_t i = 0; i < count; i++) {
    cp = codepoints[i];
    uint32_t n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    static const uint8_t LEAD[5] = {0, 0, 0xC0u, 0xE0u, 0xF0u};
    for(uint32_t j = n - 1; j; j--) { out[outlen + j] = (uint8_t)(0x80u | (cp & 0x3Fu)); cp >>= 6; }
    out[outlen] = (uint8_t)(n == 1 ? cp : LEAD[n] | cp); outlen += n;
  }
  jdl_return_bytes_copy(c, out, outlen); free(out); free(codepoints); return;
oom:
  free(codepoints); jdl_fail(c, "out of memory");
}
