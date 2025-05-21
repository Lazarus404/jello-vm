// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Float conversions; the bits of a float literal, which the compiler cannot compute with floats,
 * and Std.string of a float. */

#include "std.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double literal_value(jdlo_ctx* c, int single) {
  uint32_t len = jdl_arg_bytes_len(c, 0);
  char buffer[64];
  char* text = len < sizeof buffer ? buffer : malloc((size_t)len + 1);
  if(text == NULL) {
    jdl_fail(c, "out of memory");
    return NAN;
  }
  memcpy(text, jdl_arg_bytes_data(c, 0), len);
  text[len] = '\0';
  double value = single ? (double)strtof(text, NULL) : strtod(text, NULL);
  if(text != buffer) free(text);
  return value;
}

static uint16_t half_from_double(double d) {
  uint16_t sign = signbit(d) ? 0x8000u : 0u;
  double a = fabs(d);
  if(isnan(d)) return (uint16_t)(sign | 0x7e00u);
  if(isinf(d)) return (uint16_t)(sign | 0x7c00u);
  if(a < 0x1p-14) return (uint16_t)(sign | (uint16_t)nearbyint(ldexp(a, 24)));
  int e;
  frexp(a, &e);
  double q = nearbyint(ldexp(a, 11 - e));
  if(q == 2048.0) {
    q = 1024.0;
    e++;
  }
  int field = e - 1 + 15;
  if(field >= 31) return (uint16_t)(sign | 0x7c00u);
  return (uint16_t)(sign | (uint16_t)(field << 10) | (uint16_t)(q - 1024.0));
}

double std_half_value(uint16_t bits) {
  double magnitude = ldexp((double)(bits & 0x3ffu), -24);
  int field = (bits >> 10) & 0x1f;
  if(field == 31) magnitude = (bits & 0x3ffu) ? NAN : INFINITY;
  else if(field > 0) magnitude = ldexp((double)((bits & 0x3ffu) | 0x400u), field - 25);
  return (bits & 0x8000u) ? -magnitude : magnitude;
}

static int64_t f64_bits(jdlo_ctx* c) {
  double d = literal_value(c, 0);
  int64_t bits;
  memcpy(&bits, &d, sizeof bits);
  return bits;
}
void std_f64_low_bits(jdlo_ctx* c) {
  jdl_return_i32(c, (int32_t)(uint32_t)((uint64_t)f64_bits(c) & 0xffffffffu));
}
void std_f64_high_bits(jdlo_ctx* c) {
  jdl_return_i32(c, (int32_t)(uint32_t)((uint64_t)f64_bits(c) >> 32));
}

void std_f32_bits(jdlo_ctx* c) {
  float f = (float)literal_value(c, 1);
  int32_t bits;
  memcpy(&bits, &f, sizeof bits);
  jdl_return_i32(c, bits);
}

void std_f16_bits(jdlo_ctx* c) { jdl_return_i32(c, half_from_double(literal_value(c, 0))); }

void std_float_parse(jdlo_ctx* c) { jdl_return_f64(c, literal_value(c, 0)); }

void std_float_text(char* text, size_t size, double value, int width) {
  if(isnan(value)) {
    snprintf(text, size, "NaN");
    return;
  }
  if(isinf(value)) {
    snprintf(text, size, value < 0 ? "-Infinity" : "Infinity");
    return;
  }
  uint16_t half = half_from_double(value);
  int least = 1, most = width == 64 ? 17 : width == 32 ? 9 : 5;
  while(least < most) {
    int digits = (least + most) / 2;
    snprintf(text, size, "%.*e", digits - 1, value);
    int back = width == 64   ? strtod(text, NULL) == value
               : width == 32 ? strtof(text, NULL) == (float)value
                             : half_from_double(strtod(text, NULL)) == half;
    if(back) most = digits;
    else least = digits + 1;
  }
  snprintf(text, size, "%.*e", least - 1, value);
  char* e = strchr(text, 'e');
  int exponent = atoi(e + 1);
  if(exponent < -7 || exponent >= 21) {
    snprintf(e, size - (size_t)(e - text), "e%c%d", exponent < 0 ? '-' : '+', abs(exponent));
    return;
  }
  char significand[24];
  int count = 0;
  for(char* p = text; p < e; p++)
    if(*p >= '0' && *p <= '9') significand[count++] = *p;
  char* out = text;
  if(signbit(value)) *out++ = '-';
  if(exponent < 0) {
    *out++ = '0';
    *out++ = '.';
    for(int i = -1; i > exponent; i--) *out++ = '0';
    for(int i = 0; i < count; i++) *out++ = significand[i];
  } else {
    for(int i = 0; i <= exponent; i++) *out++ = i < count ? significand[i] : '0';
    *out++ = '.';
    if(count <= exponent + 1) *out++ = '0';
    for(int i = exponent + 1; i < count; i++) *out++ = significand[i];
  }
  *out = '\0';
}
