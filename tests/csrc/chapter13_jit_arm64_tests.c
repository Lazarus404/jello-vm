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

static void test_bl_patch_dispatch(void) {
  jit_exec_buf buf = jit_alloc_exec_buf(64);
  assert(buf.mem != NULL);
  uint32_t placeholder = 0xD503201Fu;
  assert(jit_exec_buf_write(&buf, &placeholder, 4) == 1);
  jello_jit_patch_call(&buf, 0, 16);
  uint32_t word = 0;
  memcpy(&word, buf.mem, 4);
  assert((word & 0xFC000000u) == 0x94000000u);
  int32_t off = (int32_t)(word & 0x03FFFFFFu);
  if(off & 0x02000000) off |= (int32_t)~0x03FFFFFFu;
  assert((size_t)off * 4u == 16u);
  jit_free_exec_buf(&buf);
  printf("test_bl_patch_dispatch: PASS\n");
}
#else
static void test_register_role_constants(void) {
  printf("test_register_role_constants: SKIPPED (host is not aarch64)\n");
}

static void test_gpr_and_xmm_pool_tables(void) {
  printf("test_gpr_and_xmm_pool_tables: SKIPPED (host is not aarch64)\n");
}

static void test_bl_patch_dispatch(void) {
  printf("test_bl_patch_dispatch: SKIPPED (host is not aarch64)\n");
}
#endif

int main(void) {
  test_register_role_constants();
  test_gpr_and_xmm_pool_tables();
  test_bl_patch_dispatch();
  printf("chapter13_jit_arm64_tests: all tests passed\n");
  return 0;
}