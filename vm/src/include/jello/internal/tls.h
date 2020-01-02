// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * What tls.jdll gives the VM: sessions on the vendored Mbed TLS, through the table net_tls_ops,
 * its data symbol `jello_tls_ops`. Jello code makes configurations with the library's own
 * exports, so a program using TLS imports it, and fails to load without it, and its bundle
 * carries it; no other program loads it. The VM finds the table among the libraries of the
 * module securing a stream, and keeps it with the session. */

#ifndef JELLO_INTERNAL_TLS_H
#define JELLO_INTERNAL_TLS_H

#include <jello/jdll.h>

#include <stddef.h>
#include <stdint.h>

typedef struct net_tls net_tls;

typedef struct net_tls_ops {
  uint32_t version; /**< NET_TLS_OPS_VERSION. */
  net_tls* (*open)(void* config, int fd, const uint8_t* host, uint32_t len, int32_t* err);
  void (*close)(net_tls* s, int notify);
} net_tls_ops;

#endif
