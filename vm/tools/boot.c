// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Boot images (boot.h): making one, and finding the module the running executable carries. */
#define _DARWIN_C_SOURCE 1

#include "boot.h"

#include <jello/internal.h>
#include <jello/internal/platform.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for(unsigned i = 0; i < 8u; i++) v |= (uint64_t)p[i] << (8u * i);
  return v;
}

static int footer(FILE* f, uint64_t* offset, uint64_t* size) {
  off_t end = fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : -1;
  uint8_t bytes[JELLO_BOOT_FOOTER_SIZE];
  if(end < (off_t)JELLO_BOOT_FOOTER_SIZE ||
     fseeko(f, end - (off_t)JELLO_BOOT_FOOTER_SIZE, SEEK_SET) != 0 ||
     fread(bytes, 1, sizeof bytes, f) != sizeof bytes ||
     memcmp(bytes + 16, JELLO_BOOT_MAGIC, 8) != 0)
    return 0;
  *size = get_u64(bytes);
  *offset = get_u64(bytes + 8);
  uint64_t limit = (uint64_t)end - JELLO_BOOT_FOOTER_SIZE;
  return *size > 0 && *offset <= limit && *size <= limit - *offset;
}

int jello_boot_probe(const char* path, uint8_t** data, size_t* size) {
  *data = NULL;
  *size = 0;
  FILE* f = fopen(path, "rb");
  if(!f) return 0;
  uint64_t offset = 0, length = 0;
  int found = footer(f, &offset, &length) && length <= SIZE_MAX &&
              fseeko(f, (off_t)offset, SEEK_SET) == 0 &&
              (*data = (uint8_t*)malloc((size_t)length)) != NULL &&
              fread(*data, 1, (size_t)length, f) == (size_t)length;
  fclose(f);
  if(!found) {
    free(*data);
    *data = NULL;
    return 0;
  }
  *size = (size_t)length;
  return 1;
}
