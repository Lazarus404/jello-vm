// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * tls.jdll's cryptography for std.crypto, on Mbed TLS: digests, HMAC, PBKDF2 and HKDF; sealing
 * and opening data with AES-GCM and ChaCha20-Poly1305; and reading and checking X.509
 * certificates. Algorithms are named as Jello code names them, such as "sha256"; Jello code
 * checks names, lengths and counts before calling, through the *_length exports. */
#include "crypto.h"

#include <mbedtls/asn1.h>
#include <mbedtls/cipher.h>
#include <mbedtls/constant_time.h>
#include <mbedtls/error.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_crt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG_LEN 16

typedef struct check_time {
  mbedtls_x509_time second;
  int late;
} check_time;