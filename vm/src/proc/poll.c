// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The I/O poller: kqueue on macOS and the BSDs, epoll on Linux, wepoll on Windows, watching each
 * descriptor for reading and writing at once, edge-triggered, from when it is opened until it is
 * closed; its thread, and its table of records, one a descriptor. */

#include <jello/internal/proc_internal.h>
#include <jello/internal/sockets.h>

#include <errno.h>
#include <stdlib.h>

#if defined(_WIN32)
#include "../../third_party/wepoll-1.5.8/wepoll.h"
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#else
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#endif

#if defined(_WIN32)

#elif defined(__linux__)

#else

static int change(proc_poller* pl, int fd, uint16_t flags, void* token) {
  static const int16_t filters[2] = {EVFILT_READ, EVFILT_WRITE};
  struct kevent evs[2];
  for(int i = 0; i < 2; i++) EV_SET(&evs[i], (uintptr_t)fd, filters[i], flags, 0, 0, token);
  return kevent(pl->fd, evs, 2, NULL, 0, NULL);
}

int proc_poller_open(proc_poller* pl) {
  pl->fd = kqueue();
  if(pl->fd < 0) return -1;
  struct kevent ev;
  EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  if(fcntl(pl->fd, F_SETFD, FD_CLOEXEC) != 0 || kevent(pl->fd, &ev, 1, NULL, 0, NULL) != 0) {
    int err = errno;
    close(pl->fd);
    errno = err;
    return -1;
  }
  return 0;
}

int proc_poller_watch(proc_poller* pl, int fd, void* token) {
  struct stat st;
  if(fstat(fd, &st) != 0) return -1;
  if(S_ISREG(st.st_mode)) return 1;
  if(change(pl, fd, EV_ADD | EV_CLEAR, token) == 0) return 0;
  int err = errno;
  change(pl, fd, EV_DELETE, NULL);
  errno = err;
  return -1;
}

void proc_poller_unwatch(proc_poller* pl, int fd) { change(pl, fd, EV_DELETE, NULL); }

uint32_t proc_poller_wait(proc_poller* pl, proc_poll_event* out) {
  struct kevent evs[PROC_POLL_BATCH];
  int n = kevent(pl->fd, NULL, 0, evs, (int)PROC_POLL_BATCH, NULL);
  if(n < 0) return 0;
  for(int i = 0; i < n; i++) {
    uint32_t ready = evs[i].filter == EVFILT_READ ? PROC_FD_READ : PROC_FD_WRITE;
    out[i] = (proc_poll_event){evs[i].filter == EVFILT_USER ? NULL : evs[i].udata, ready};
  }
  return (uint32_t)n;
}

void proc_poller_wake(proc_poller* pl) {
  struct kevent ev;
  EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  kevent(pl->fd, &ev, 1, NULL, 0, NULL);
}

void proc_poller_close(proc_poller* pl) { close(pl->fd); }

#endif

proc_poll_desc* proc_polls_find(proc_sched* s, int fd) {
  int32_t at = proc_poll_index(fd);
  if(at < 0) return NULL;
  proc_poll_desc* chunk =
      atomic_load_explicit(&s->poll_chunks[(uint32_t)at / PROC_POLL_CHUNK], memory_order_acquire);
  return chunk ? &chunk[(uint32_t)at % PROC_POLL_CHUNK] : NULL;
}
