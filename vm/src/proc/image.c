// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * image_run, which runs a module image as a program on a VM of its own and returns the program's
 * Bytes result; the compiler runs macro expanders this way, and itself to compile them. The run
 * is made on a thread of its own, as a scheduler's state is its thread's, and shares nothing with
 * the caller's VM but the process's native libraries. */

#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IMAGE_RUN_RESULT, IMAGE_RUN_FAILED, IMAGE_RUN_OUT_OF_FUEL };

#define IMAGE_RUN_STACK (16u << 20)

typedef struct image_run {
  const uint8_t* image;
  size_t size;
  const char* const* argv;
  int32_t argc;
  uint64_t fuel;
  uint8_t* out;
  size_t out_len;
} image_run;

static void give(image_run* r, int tag, const void* data, size_t n) {
  r->out = (uint8_t*)malloc(n + 1u);
  r->out_len = r->out ? n + 1u : 0u;
  if(!r->out) return;
  r->out[0] = (uint8_t)tag;
  memcpy(r->out + 1, data, n);
}

static int split_args(image_run* r, const uint8_t* args, uint32_t len, char** store) {
  if(len && args[len - 1u]) return 0;
  int32_t n = 0;
  for(uint32_t i = 0; i < len; i++) n += !args[i];
  *store = (char*)malloc(len ? len : 1u);
  const char** argv = (const char**)malloc(sizeof(char*) * (size_t)(n ? n : 1));
  if(!*store || !argv) {
    free(argv);
    return 0;
  }
  if(len) memcpy(*store, args, len);
  for(uint32_t i = 0, k = 0; k < (uint32_t)n; k++) {
    argv[k] = *store + i;
    i += (uint32_t)strlen(argv[k]) + 1u;
  }
  r->argv = argv;
  r->argc = n;
  return 1;
}
