// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/jdll.h>

#include <string.h>

static void tn_echo_i32(jdlo_ctx* c) {
  jdl_return_i32(c, jdl_arg_i32(c, 0));
}

static void tn_sum2_i32(jdlo_ctx* c) {
  jdl_return_i32(c, jdl_arg_i32(c, 0) + jdl_arg_i32(c, 1));
}

static void tn_echo_bytes(jdlo_ctx* c) {
  jdl_return_bytes_copy(c, jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0));
}

static void tn_echo_dyn(jdlo_ctx* c) {
  jdl_return_value(c, jdl_arg_value(c, 0));
}

static const jello_jdll_export TEST_NATIVE_EXPORTS[] = {
  {"echo_i32", tn_echo_i32, (uint8_t)JELLO_T_I32, 1, {(uint8_t)JELLO_T_I32}},
  {"sum2_i32", tn_sum2_i32, (uint8_t)JELLO_T_I32, 2, {(uint8_t)JELLO_T_I32, (uint8_t)JELLO_T_I32}},
  {"echo_bytes", tn_echo_bytes, (uint8_t)JELLO_T_BYTES, 1, {(uint8_t)JELLO_T_BYTES}},
  {"echo_dyn", tn_echo_dyn, (uint8_t)JELLO_T_DYNAMIC, 1, {(uint8_t)JELLO_T_DYNAMIC}},
};

const jello_jdll_manifest jello_jdll_manifest_test_native = {
  JELLO_JDLL_MANIFEST_MAGIC,
  JELLO_JDLL_ABI_VERSION,
  "test_native",
  TEST_NATIVE_EXPORTS,
  (uint32_t)(sizeof(TEST_NATIVE_EXPORTS) / sizeof(TEST_NATIVE_EXPORTS[0])),
};
