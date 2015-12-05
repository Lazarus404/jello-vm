// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_impl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#endif

static size_t jit_icache_span(size_t n, size_t cap) {
  long page = sysconf(_SC_PAGESIZE);
  if(page < 64) page = 64;
  size_t span = (n + (size_t)page - 1u) & ~((size_t)page - 1u);
  if(span > cap) span = cap;
  return span;
}

static void jit_sync_icache(void* mem, size_t n) {
  if(!mem || n == 0) return;
#if defined(__APPLE__) && defined(__aarch64__)
  sys_icache_invalidate(mem, n);
#else
  __builtin___clear_cache((char*)mem, (char*)mem + n);
#endif
#if defined(__aarch64__) && !defined(__APPLE__)
  __asm__ __volatile__("dsb ish\n\tisb" ::: "memory");
#endif
}

/* -- the minimal W^X proof mechanism -- */

jit_exec_buf jit_alloc_exec_buf(size_t cap) {
  jit_exec_buf buf;
  buf.mem = NULL;
  buf.cap = cap;
  buf.len = 0;
  buf.finalized = 0;

  void* mem = mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(mem == MAP_FAILED) return buf;
  buf.mem = (uint8_t*)mem;
  return buf;
}

int jit_exec_buf_write(jit_exec_buf* buf, const void* bytes, size_t n) {
  if(!buf->mem || buf->finalized || buf->len + n > buf->cap) return 0;
  memcpy(buf->mem + buf->len, bytes, n);
  buf->len += n;
  return 1;
}

int jit_finalize_exec_buf(jit_exec_buf* buf) {
  if(!buf->mem) return 0;
  if(mprotect(buf->mem, buf->cap, PROT_READ | PROT_EXEC) != 0) return 0;
  jit_sync_icache(buf->mem, jit_icache_span(buf->len, buf->cap));
  buf->finalized = 1;
  return 1;
}

void jit_free_exec_buf(jit_exec_buf* buf) {
  if(!buf->mem) return;
  munmap(buf->mem, buf->cap);
  buf->mem = NULL;
}

void jit_trap_arithmetic(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_ARITHMETIC;
}

void jit_trap_stack_overflow(jello_vm* vm) {
  vm->trap_code = JELLO_TRAP_STACK_OVERFLOW;
}
