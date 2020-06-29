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
