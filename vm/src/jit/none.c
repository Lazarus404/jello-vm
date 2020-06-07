// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The JIT entry points of a build without the JIT (JIT=0); no module can be created, so every
 * run is interpreted. */

#include <jello/internal/jit_internal.h>

int jello_jit_available(void) { return 0; }
