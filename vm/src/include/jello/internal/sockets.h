// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Sockets as the VM and tls.jdll use them, the same on every system: a socket is an `int`
 * descriptor, and a call that fails returns -1 with errno set to the POSIX error. On Windows the
 * descriptor is the socket's handle, which, as every handle, fits in 32 bits, and Winsock's errors
 * are mapped to errno's. */

#ifndef JELLO_INTERNAL_SOCKETS_H
#define JELLO_INTERNAL_SOCKETS_H

#include <jello/internal/platform.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#ifdef _WIN32
#include <mstcpip.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32

#else

#define JELLO_SOCKET(fd) (fd)

#endif

#ifdef _WIN32
#endif

static inline void jello_sock_close(int fd) {
#ifdef _WIN32
  closesocket(JELLO_SOCKET(fd));
#else
  close(fd);
#endif
}

#endif /* JELLO_INTERNAL_SOCKETS_H */
