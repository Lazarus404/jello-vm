// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Math.*, Float.is_nan, Float.is_infinite, and parsing the text of a number for Float.parse and
 * Integer.parse. */

#include "std.h"

#include <math.h>
#include <string.h>
#include <time.h>

static double sign(double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : x; }