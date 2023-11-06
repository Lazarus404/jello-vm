// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * What tls.c and crypto.c, the two halves of tls.jdll, share: the certificate readers, and
 * crypto.c's exports, which tls.c lists in the library's one manifest. */
#ifndef JELLO_TLS_CRYPTO_H
#define JELLO_TLS_CRYPTO_H

#include <jello/jdll.h>

#include <mbedtls/x509_crt.h>

#include <stddef.h>
#include <stdint.h>

unsigned char* tls_with_nul(const uint8_t* pem, uint32_t len, size_t* out_len);
int tls_parse_certificates(mbedtls_x509_crt* certs, const unsigned char* pem, size_t len);
int tls_system_certificates(mbedtls_x509_crt* certs);

#endif /* JELLO_TLS_CRYPTO_H */
