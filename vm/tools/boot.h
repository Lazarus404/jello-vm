// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Boot images: a copy of jello with a `.jlo` module appended, which runs that module as itself.
 * The file ends with a 24-byte footer of little-endian u64s, the module's size and offset, then
 * the magic `JLYBOOT1`. The executable's own headers are left as they are, so the image is still
 * a valid ELF or Mach-O program; the loader never maps the bytes after them. */
#ifndef JELLO_TOOLS_BOOT_H
#define JELLO_TOOLS_BOOT_H

#include <jello.h>
#include <stddef.h>
#include <stdint.h>

#define JELLO_BOOT_MAGIC "JLYBOOT1"
#define JELLO_BOOT_FOOTER_SIZE 24u

int jello_boot_probe(const char* path, uint8_t** data, size_t* size);

int jello_boot_create(const char* vm, const uint8_t* module, size_t size, const char* out,
                      char* error, size_t error_size);

int jello_boot_bundle(const jello_bc_module* m, const char* out, char* error, size_t error_size);

#endif
