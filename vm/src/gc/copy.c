// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Deep-copies an object graph for message passing or process creation, 
 * ensuring complete memory isolation between sender and receiver.
 *
 * - Preserves object sharing and cycles while ensuring neither process sees
 *   the other's writes.
 * - Copies boxes, buffers, and small bytes. Slices are copied  only for active 
 *   content. Large bytes reside in shared, reference-counted binaries. 
 *   Immortal objects (CONST_FUN outputs, byte literals) belong to no heap.
 * - Native handles require a class `retain` method to cross processes
 *   (otherwise rejected or nulled). Sockets transfer ownership to the receiver
 *   or read as closed.
 * - Process-specific prototypes and globals are copied or mapped  based on
 *   whether the target is a new process or an existing one.
 * - Uses an iterative worklist (non-recursive) and temporary parcels requiring
 *   no garbage collection roots until fully adopted by the receiver.
 */

#include <jello/internal.h>
#include <jello/internal/proc_internal.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define COPY_INLINE 16u

typedef struct copier {
  jello_vm* vm;
  const jello_bc_module* m;
  int out;
  int drop;
  int moves;
  struct proc_socket_list* sockets;
  int refused;
  vm_protos* fixes;
  uint32_t fixes_cap;
  struct vm_proto_fix* found;
  uint32_t found_len;
  uint32_t found_cap;
  void** keys;
  void** vals;
  uint32_t cap;
  uint32_t len;
  uint32_t shift;
  void** work;
  uint32_t work_len;
  uint32_t work_cap;
  void* keys_inline[2u * COPY_INLINE];
  void* vals_inline[2u * COPY_INLINE];
  void* work_inline[COPY_INLINE];
} copier;
