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

static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;

static jello_process* owner_of(const proc_socket* k) {
  return __atomic_load_n(&k->owner, __ATOMIC_ACQUIRE);
}

static void set_owner(proc_socket* k, jello_process* p) {
  __atomic_store_n(&k->owner, p, __ATOMIC_RELEASE);
}

static void adopt(proc_socket* k, jello_process* p) {
  proc_retain(p);
  k->owned_prev = NULL;
  k->owned_next = p->sockets;
  if(p->sockets) p->sockets->owned_prev = k;
  p->sockets = k;
  set_owner(k, p);
}

static void disown(proc_socket* k) {
  if(k->owned_prev) k->owned_prev->owned_next = k->owned_next;
  else k->owner->sockets = k->owned_next;
  if(k->owned_next) k->owned_next->owned_prev = k->owned_prev;
  k->owned_prev = k->owned_next = NULL;
  set_owner(k, NULL);
}

static const net_tls_ops* tls_library(const jello_bc_module* m) {
  const jello_jdll_handle* const* handles = (const jello_jdll_handle* const*)m->jdll_handles;
  for(uint32_t i = 0; i < m->njdll_handles; i++) {
    const net_tls_ops* ops = (const net_tls_ops*)jello_jdll_symbol(handles[i], "jello_tls_ops");
    if(ops && ops->version == NET_TLS_OPS_VERSION) return ops;
  }
  return NULL;
}

static void free_socket(void* payload) {
  if(!jdl_release(payload)) return;
  proc_socket* k = (proc_socket*)payload;
  pthread_mutex_lock(&net_lock);
  jello_process* owner = k->owner;
  if(owner) disown(k);
  pthread_mutex_unlock(&net_lock);
  if(k->tls) k->tls_ops->close(k->tls, 0);
  if(k->fd >= 0) jello_sock_close(k->fd);
  if(owner) proc_release(owner);
  free(k);
}

const jello_jdll_handle_class proc_socket_class = {"socket", free_socket, jdl_retain};

int proc_socket_note(struct proc_socket_list** list, void* payload) {
  struct proc_socket_list* l = *list;
  if(!l || l->len == l->cap) {
    uint32_t cap = l ? l->cap * 2 : 4;
    l = (struct proc_socket_list*)realloc(l, sizeof *l + sizeof(proc_socket*) * cap);
    if(!l) return 0;
    if(!*list) {
      l->next = NULL;
      l->len = 0;
    }
    l->cap = cap;
    *list = l;
  }
  l->at[l->len++] = (proc_socket*)payload;
  return 1;
}

void proc_sockets_pack(const jello_vm* vm, struct proc_socket_list** list) {
  struct proc_socket_list* l = *list;
  jello_process* from = vm ? vm->proc : NULL;
  uint32_t kept = 0;
  pthread_mutex_lock(&net_lock);
  for(uint32_t i = 0; from && i < l->len; i++) {
    proc_socket* k = l->at[i];
    if(k->owner != from || k->fd < 0) continue;
    disown(k);
    jdl_retain(k);
    l->at[kept++] = k;
  }
  pthread_mutex_unlock(&net_lock);
  for(uint32_t i = 0; i < kept; i++) proc_release(from);
  l->len = kept;
  if(!kept) {
    free(l);
    *list = NULL;
  }
}

void proc_sockets_take(jello_process* p, struct proc_socket_list** list) {
  struct proc_socket_list* chain = __atomic_exchange_n(list, NULL, __ATOMIC_ACQ_REL);
  if(!chain) return;
  if(p) {
    pthread_mutex_lock(&net_lock);
    for(struct proc_socket_list* l = chain; l; l = l->next) {
      for(uint32_t i = 0; i < l->len; i++) {
        if(l->at[i]->fd >= 0) adopt(l->at[i], p);
      }
    }
    pthread_mutex_unlock(&net_lock);
  }
  while(chain) {
    struct proc_socket_list* l = chain;
    chain = l->next;
    for(uint32_t i = 0; i < l->len; i++) {
      proc_socket* k = l->at[i];
      if(!p && k->fd >= 0) {
        jello_sock_close(k->fd);
        k->fd = -1;
      }
      free_socket(k);
    }
    free(l);
  }
}

void proc_sockets_join(struct proc_socket_list** into, struct proc_socket_list** from) {
  struct proc_socket_list* last = *from;
  while(last->next) last = last->next;
  last->next = *into;
  *into = *from;
  *from = NULL;
}

static void close_locked(proc_socket* k, jello_process* p) {
  disown(k);
  int fd = k->fd;
  net_tls* tls = k->tls;
  const net_tls_ops* ops = k->tls_ops;
  k->fd = -1;
  k->tls = NULL;
  pthread_mutex_unlock(&net_lock);
  if(tls) ops->close(tls, 1);
  proc_poll_close(p->vm, fd);
  jello_sock_close(fd);
  proc_release(p);
}

void proc_sockets_end(jello_process* p) {
  for(;;) {
    pthread_mutex_lock(&net_lock);
    if(!p->sockets) break;
    close_locked(p->sockets, p);
  }
  pthread_mutex_unlock(&net_lock);
}

static void failed(jdlo_ctx* c, int32_t err) { c->vm->net_error = err; }

typedef struct net_addrs {
  struct addrinfo* list;
  struct addrinfo one;
  struct sockaddr_storage addr;
} net_addrs;

static int32_t port_of(const struct sockaddr_storage* a) {
  if(a->ss_family == AF_INET) return ntohs(((const struct sockaddr_in*)a)->sin_port);
  if(a->ss_family == AF_INET6) return ntohs(((const struct sockaddr_in6*)a)->sin6_port);
  return -1;
}

void vm_net_error(jdlo_ctx* c) { jdl_return_i32(c, c->vm->net_error); }

void vm_net_error_text(jdlo_ctx* c) {
  int32_t code = jdl_arg_i32(c, 0);
  char tls[256];
  const net_tls_ops* ops = code >= NET_TLS_ERRORS ? tls_library(c->m) : NULL;
  if(ops) ops->error_text(code, tls, sizeof tls);
  else if(code >= NET_TLS_ERRORS) snprintf(tls, sizeof tls, "TLS error %d", NET_TLS_ERRORS - code);
  const char* text = code == 0                ? "no error"
                     : code == NET_TIMEOUT    ? "timed out"
                     : code == NET_CLOSED     ? "socket closed, or another process's"
                     : code >= NET_TLS_ERRORS ? tls
                     : code >= NET_GAI        ? gai_strerror(code - NET_GAI)
                                              : jello_error_text(code);
  jdl_return_bytes_copy(c, (const uint8_t*)text, (uint32_t)strlen(text));
}
