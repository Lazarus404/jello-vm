// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * What the tools share as hosts; running a module as a program (run.c), for jello and jelloc,
 * and showing its result and traps (host.c), also used by `jlo_tool run --print`. */
#ifndef JELLO_TOOLS_HOST_H
#define JELLO_TOOLS_HOST_H

#include <jello/internal.h>
#include <stdio.h>

extern const uint8_t jello_jelloc_image[];
extern const uint8_t jello_jelloc_image_end[];

typedef struct jello_host_options {
  const char* tool; /**< Names the host in its messages; "jello: ...". */
  int jit;          /**< Create a JIT module for the run. A build without the JIT interprets,
                         and refuses `require_jit` and `jit_report`. */
  int require_jit;  /**< With `jit`; compile every function first, and fail if one cannot be. */
  int jit_report;   /**< With `jit`; report on stderr what the JIT compiled. */
  int raw_bytes;    /**< Print a Bytes result as is, without a newline. */
} jello_host_options;

#endif
