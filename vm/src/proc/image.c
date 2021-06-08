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
