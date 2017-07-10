// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_internal.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(__aarch64__)
static void test_register_role_constants(void) {
  assert(JIT_REG_OUT == 19);
  assert(JIT_REG_VM == 20);
  assert(JIT_REG_BASE == 21);
  assert(JIT_REG_SURVIVE == 28);
  printf("test_register_role_constants: PASS\n");
}

static void test_gpr_and_xmm_pool_tables(void) {
  assert(jit_gpr_pool_len == 6u);
  assert(jit_xmm_pool_len == 8u);
  static const uint8_t expected_gpr[] = {9, 10, 11, 12, 13, 14};
  assert(memcmp(jit_gpr_pool, expected_gpr, sizeof(expected_gpr)) == 0);
  printf("test_gpr_and_xmm_pool_tables: PASS\n");
}
#else
static void test_register_role_constants(void) {
  printf("test_register_role_constants: SKIPPED (host is not aarch64)\n");
}

static void test_gpr_and_xmm_pool_tables(void) {
  printf("test_gpr_and_xmm_pool_tables: SKIPPED (host is not aarch64)\n");
}

#endif

int main(void) {
  test_register_role_constants();
  test_gpr_and_xmm_pool_tables();
  printf("chapter13_jit_arm64_tests: all tests passed\n");
  return 0;
}