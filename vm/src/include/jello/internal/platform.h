// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * What the VM and its libraries ask of the system in more than one place, behind one name. */

#ifndef JELLO_INTERNAL_PLATFORM_H
#define JELLO_INTERNAL_PLATFORM_H

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#endif
#include <winsock2.h> /* Before windows.h, which would otherwise bring in the older winsock.h. */
#include <windows.h>
#include <direct.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <sched.h>
#endif
#endif

#ifndef MSG_NOSIGNAL
                        */
#endif

static inline long jello_cpu_count(void) {
#ifdef _WIN32
  DWORD_PTR process_mask = 0, system_mask = 0;
  if(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask) {
    long allowed = 0;
    for(; process_mask; process_mask &= process_mask - 1) allowed++;
    return allowed;
  }
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwNumberOfProcessors < 1 ? 1 : (long)info.dwNumberOfProcessors;
#else
#ifdef __linux__
  cpu_set_t set;
  int allowed = sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : 0;
  if(allowed > 0) return allowed;
#endif
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n < 1 ? 1 : n;
#endif
}

static inline size_t jello_page_size(void) {
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
#else
  return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

static inline uint64_t jello_page_round_up(uint64_t bytes) {
  uint64_t page = (uint64_t)jello_page_size();
  return (bytes + page - 1u) & ~(page - 1u);
}

typedef enum {
  JELLO_PAGES_NONE, /**< Nothing: any access faults, as a guard page's does. */
  JELLO_PAGES_RW,   /**< Read and write. */
  JELLO_PAGES_RX    /**< Read and run, as compiled code is. */
} jello_pages_access;

static inline void* jello_pages_map(size_t bytes) {
#ifdef _WIN32
  return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void* mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return mem == MAP_FAILED ? NULL : mem;
#endif
}

static inline void* jello_pages_reserve(size_t bytes) {
#ifdef _WIN32
  /* Readable and writable, not inaccessible, so that the system can grow a stack into it. */
  return VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
  return jello_pages_map(bytes);
#endif
}

#ifdef _WIN32
#else
#define JELLO_PAGES_COMMIT_COSTS 0
#endif

static inline int jello_pages_commit(void* mem, size_t bytes) {
#ifdef _WIN32
  return VirtualAlloc(mem, bytes, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
  (void)mem;
  (void)bytes;
  return 1;
#endif
}

static inline int jello_pages_protect(void* mem, size_t bytes, jello_pages_access access) {
#ifdef _WIN32
  static const DWORD protection[] = {PAGE_NOACCESS, PAGE_READWRITE, PAGE_EXECUTE_READ};
  DWORD old;
  return VirtualProtect(mem, bytes, protection[access], &old) != 0;
#else
  static const int protection[] = {PROT_NONE, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC};
  return mprotect(mem, bytes, protection[access]) == 0;
#endif
}

static inline void jello_pages_unmap(void* mem, size_t bytes) {
#ifdef _WIN32
  (void)bytes;
  VirtualFree(mem, 0, MEM_RELEASE);
#else
  munmap(mem, bytes);
#endif
}

static inline void* jello_aligned_alloc(size_t align, size_t bytes) {
#ifdef _WIN32
  return _aligned_malloc(bytes, align);
#else
  return aligned_alloc(align, (bytes + align - 1u) / align * align);
#endif
}

static inline void jello_aligned_free(void* p) {
#ifdef _WIN32
  _aligned_free(p);
#else
  free(p);
#endif
}

#ifdef _WIN32
#else
#define JELLO_PATH_LIST_SEP ':'
#endif

#ifdef _WIN32
#else
#endif

static inline int jello_is_slash(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

static inline const char* jello_last_slash(const char* path) {
  const char* last = NULL;
  for(; *path; path++)
    if(jello_is_slash(*path)) last = path;
  return last;
}

static inline void jello_slashes_forward(char* path) {
#ifdef _WIN32
  for(; *path; path++)
    if(*path == '\\') *path = '/';
#else
  (void)path;
#endif
}

static inline size_t jello_path_root_len(const char* path) {
#ifdef _WIN32
  if(((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z') && path[1] == ':')
    return jello_is_slash(path[2]) ? 3u : 2u;
  if(jello_is_slash(path[0]) && jello_is_slash(path[1])) {
    size_t at = 2;
    for(int part = 0; part < 2 && path[at]; part++) {
      while(path[at] && !jello_is_slash(path[at])) at++;
      if(path[at]) at++;
    }
    return at;
  }
#endif
  return jello_is_slash(path[0]) ? 1u : 0u;
}

static inline int jello_mkdir(const char* path) {
#ifdef _WIN32
  return mkdir(path);
#else
  return mkdir(path, 0777);
#endif
}

static inline int jello_real_path(const char* path, char* out, size_t out_len) {
#ifdef _WIN32
  DWORD n = GetFullPathNameA(path, (DWORD)out_len, out, NULL);
  if(n == 0 || n >= out_len || GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) return 0;
  jello_slashes_forward(out);
  return 1;
#else
  char resolved[PATH_MAX];
  if(!realpath(path, resolved) || strlen(resolved) >= out_len) return 0;
  memcpy(out, resolved, strlen(resolved) + 1u);
  return 1;
#endif
}

static inline int jello_self_path(char* out, size_t out_len) {
  char path[PATH_MAX];
#if defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
  if(n == 0 || n >= sizeof path) return 0;
#elif defined(__APPLE__)
  uint32_t size = sizeof path;
  if(_NSGetExecutablePath(path, &size) != 0) return 0;
#else
  ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1u);
  if(n <= 0) return 0;
  path[n] = '\0';
#endif
  return jello_real_path(path, out, out_len);
}

#ifdef _WIN32
#endif

static inline void* jello_dl_open(const char* path, char* why, size_t why_len) {
#ifdef _WIN32
  char native[MAX_PATH];
  size_t len = strlen(path);
  if(len >= sizeof native) {
    snprintf(why, why_len, "path too long");
    return NULL;
  }
  for(size_t i = 0; i <= len; i++) native[i] = path[i] == '/' ? '\\' : path[i];
  HMODULE dl = LoadLibraryExA(native, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  if(!dl) {
    DWORD err = GetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                             0, why, (DWORD)why_len, NULL);
    while(n > 0 && (why[n - 1] == '\n' || why[n - 1] == '\r' || why[n - 1] == '.')) why[--n] = '\0';
    if(n == 0) snprintf(why, why_len, "error %lu", (unsigned long)err);
  }
  return (void*)dl;
#else
  void* dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if(!dl) snprintf(why, why_len, "%s", dlerror());
  return dl;
#endif
}

static inline void* jello_dl_symbol(void* dl, const char* name) {
#ifdef _WIN32
  FARPROC f = GetProcAddress((HMODULE)dl, name);
  void* p;
  memcpy(&p, &f, sizeof p); /* A function's address as data, which ISO C has no cast for. */
  return p;
#else
  return dlsym(dl, name);
#endif
}

static inline void jello_dl_close(void* dl) {
#ifdef _WIN32
  FreeLibrary((HMODULE)dl);
#else
  dlclose(dl);
#endif
}

#endif /* JELLO_INTERNAL_PLATFORM_H */
