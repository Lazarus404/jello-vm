// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Sockets for Jello code: TCP listeners and streams, and UDP sockets. Each is non-blocking and
 * watched by the poller from its making until its closing, so a call that would block parks only
 * its process, until the socket is ready or the call's time runs out.
 *
 * A socket belongs to one process, and only that process's calls may use it. A message or a new
 * process's body moves it: the copy takes it from its owner, whose handle then reads as closed,
 * and its receiver owns it once it takes the message, or the new process from its start. A
 * message never taken closes it, and one that cannot be sent gives it back. Copies of globals
 * and exit values hold one that is not theirs, which reads as closed. A socket closes when its
 * owner closes it or finishes, or else once its last handle is collected.
 *
 * A call that fails returns null, an empty Bytes or false, and records why for the calling VM,
 * where net_error reads it: an errno value, a getaddrinfo error past NET_GAI, a TLS error past
 * NET_TLS_ERRORS, or one of NET_TIMEOUT and NET_CLOSED. Each call clears it first, so an empty
 * read with none recorded is the end of the stream.
 *
 * A stream secured with net_tls_start carries its data through its TLS session from then on:
 * net_read and net_write go through it, and closing it tells the other end first. Sessions are
 * tls.jdll's (jello/internal/tls.h), which the calling module has loaded to make a
 * configuration. */

#include <jello/internal/jdll_internal.h>
#include <jello/internal/proc_internal.h>
#include <jello/internal/sockets.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  NET_TIMEOUT = -1, /**< The call's time ran out. */
  NET_CLOSED = -2,  /**< The socket is closed, or not the caller's. */
  NET_GAI = 100000, /**< Added to a getaddrinfo error. */
};

#define NET_READ_MAX (1u << 20)

typedef struct proc_socket {
  jdl_refs refs;
  int fd; /**< -1 once closed; its owner's to change. */
  jello_process* owner;
  struct proc_socket* owned_prev; /**< In its owner's `sockets`. */
  struct proc_socket* owned_next;
  struct sockaddr_storage peer; /**< A stream's other end, or the last datagram's sender. */
  socklen_t peer_len;
  net_tls* tls; /**< A secured stream's TLS session, else NULL; its owner's to change. */
  const net_tls_ops* tls_ops; /**< The table of the TLS library that made `tls`. */
} proc_socket;

struct proc_socket_list {
  struct proc_socket_list* next;
  uint32_t len;
  uint32_t cap;
  proc_socket* at[];
};

void proc_sockets_join(struct proc_socket_list** into, struct proc_socket_list** from) {
  struct proc_socket_list* last = *from;
  while(last->next) last = last->next;
  last->next = *into;
  *into = *from;
  *from = NULL;
}

typedef struct net_addrs {
  struct addrinfo* list;
  struct addrinfo one;
  struct sockaddr_storage addr;
} net_addrs;
