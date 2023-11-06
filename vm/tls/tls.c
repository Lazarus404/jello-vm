// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * tls.jdll: TLS for Jello code, on the vendored Mbed TLS (third_party/mbedtls-3.5.1). It exports
 * configurations to Jello code, and to the VM's net.c, through the table `jello_tls_ops`
 * (jello/internal/tls.h), the sessions it runs on a stream's socket once its handshake starts.
 *
 * A configuration is one side's: a server's certificate chain and private key, or the
 * certificates a client trusts, which it checks the server's chain and name against. It never
 * changes once made, so a handle copied to another process shares it. A session belongs to its
 * socket, so it moves with it; its socket is non-blocking, and a call that cannot go on returns
 * what it waits for, for net.c to park its process until then. Mbed TLS's errors become net_error
 * codes from NET_TLS_ERRORS, and its socket errors errno values. */
#include <jello/internal/entropy.h>
#include <jello/internal/platform.h>
#include <jello/internal/sockets.h>
#include <jello/internal/tls.h>

#include "crypto.h"

#include <mbedtls/build_info.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <wincrypt.h>
#else
static const char* const SYSTEM_CERTIFICATES[] = {
    "/etc/ssl/cert.pem",                  /* macOS, Alpine, the BSDs */
    "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu */
    "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora, RHEL */
    "/etc/ssl/ca-bundle.pem",             /* openSUSE */
};
#endif

typedef struct net_tls_config {
  jdl_refs refs;
  int server;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt certs;
  mbedtls_pk_context key; /**< A server's private key. */
} net_tls_config;

struct net_tls {
  mbedtls_ssl_context ssl;
  net_tls_config* config; /**< Held while the session lives. */
  int fd;
  int sys_err; /**< The errno of the last send or receive that failed. */
  int32_t err; /**< Why the last call failed, as a net_error code. */
};

unsigned char* tls_with_nul(const uint8_t* pem, uint32_t len, size_t* out_len) {
  unsigned char* s = (unsigned char*)malloc((size_t)len + 1);
  if(!s) return NULL;
  if(len) memcpy(s, pem, len);
  s[len] = '\0';
  *out_len = (size_t)len + 1;
  return s;
}

int tls_parse_certificates(mbedtls_x509_crt* certs, const unsigned char* pem, size_t len) {
  int ret = mbedtls_x509_crt_parse(certs, pem, len);
  return ret > 0 ? MBEDTLS_ERR_X509_CERT_UNKNOWN_FORMAT : ret;
}

int tls_system_certificates(mbedtls_x509_crt* certs) {
  int ret = MBEDTLS_ERR_X509_FILE_IO_ERROR;
  const char* file = getenv("SSL_CERT_FILE");
  const char* dirs = getenv("SSL_CERT_DIR");
  if(file && !*file) file = NULL;
  if(dirs && !*dirs) dirs = NULL;
  if(file || dirs) {
    if(file) ret = mbedtls_x509_crt_parse_file(certs, file);
    for(const char* dir = dirs; dir && *dir;) {
      const char* end = strchr(dir, JELLO_PATH_LIST_SEP);
      size_t len = end ? (size_t)(end - dir) : strlen(dir);
      char path[4096];
      if(len > 0 && len < sizeof path) {
        memcpy(path, dir, len);
        path[len] = '\0';
        int got = mbedtls_x509_crt_parse_path(certs, path);
        if(got < 0) ret = got;
      }
      dir = end ? end + 1 : dir + len;
    }
  } else {
#ifdef _WIN32
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    for(PCCERT_CONTEXT cert = NULL; store && (cert = CertEnumCertificatesInStore(store, cert));) {
      if(cert->dwCertEncodingType & X509_ASN_ENCODING)
        mbedtls_x509_crt_parse_der(certs, cert->pbCertEncoded, cert->cbCertEncoded);
    }
    if(store) CertCloseStore(store, 0);
#else
    size_t n = sizeof SYSTEM_CERTIFICATES / sizeof SYSTEM_CERTIFICATES[0];
    struct stat st;
    for(size_t i = 0; i < n && !certs->version; i++) {
      if(stat(SYSTEM_CERTIFICATES[i], &st) != 0) continue;
      ret = mbedtls_x509_crt_parse_file(certs, SYSTEM_CERTIFICATES[i]);
    }
#endif
  }
  return certs->version ? 0 : ret < 0 ? ret : MBEDTLS_ERR_X509_CERT_UNKNOWN_FORMAT;
}

#define B (uint8_t)JELLO_T_BOOL
#define Y (uint8_t)JELLO_T_BYTES
#define I (uint8_t)JELLO_T_I32
#define L (uint8_t)JELLO_T_I64
#define D (uint8_t)JELLO_T_DYNAMIC
