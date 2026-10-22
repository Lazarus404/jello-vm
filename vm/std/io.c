// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* Files, directories, the environment, the process arguments, exit and exec. A path
 * is Bytes without zero bytes; any other path cannot name a file. */

#define _DARWIN_C_SOURCE 1

#include "std.h"

#include <jello/internal/platform.h>
#include "jello_version.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

static int c_string(jdlo_ctx* c, int index, char* out, uint32_t cap) {
  const uint8_t* data = jdl_arg_bytes_data(c, index);
  uint32_t len = jdl_arg_bytes_len(c, index);
  if(len == 0 || len >= cap || memchr(data, 0, len)) return 0;
  memcpy(out, data, len);
  out[len] = '\0';
  return 1;
}

static FILE* std_stream(const char* path) {
  if(strcmp(path, "/dev/stdout") == 0) return stdout;
  if(strcmp(path, "/dev/stderr") == 0) return stderr;
  return NULL;
}

static int null_device(const char* path) { return strcmp(path, "/dev/null") == 0; }

static FILE* std_open(const char* path, const char* mode) {
#ifdef _WIN32
  if(null_device(path)) path = "NUL";
#endif
  return fopen(path, mode);
}

void std_file_exists(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  struct stat st;
  int ok = c_string(c, 0, path, sizeof path) &&
           (std_stream(path) || null_device(path) || stat(path, &st) == 0);
  jdl_return_bool(c, ok);
}

static int read_file(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  FILE* f = c_string(c, 0, path, sizeof path) ? std_open(path, "rb") : NULL;
  if(!f) return 0;
  uint8_t* data = NULL;
  long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
  int ok = size >= 0 && (unsigned long)size <= STD_MAX_FILE && fseek(f, 0, SEEK_SET) == 0;
  if(ok && size > 0) {
    data = (uint8_t*)malloc((size_t)size);
    ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
  }
  fclose(f);
  if(ok) jdl_return_bytes_copy(c, data, (uint32_t)size);
  free(data);
  return ok;
}

void std_file_read(jdlo_ctx* c) {
  if(!read_file(c)) jdl_fail(c, "cannot read file");
}

void std_file_contents(jdlo_ctx* c) {
  if(!read_file(c)) jdl_return_null(c);
}

void std_file_write(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  const uint8_t* body = jdl_arg_bytes_data(c, 1);
  uint32_t len = jdl_arg_bytes_len(c, 1);
  if(!c_string(c, 0, path, sizeof path)) {
    jdl_return_bool(c, 0);
    return;
  }
  FILE* stream = std_stream(path);
  if(stream) {
    jdl_return_bool(c, (len == 0 || fwrite(body, 1, len, stream) == len) && fflush(stream) == 0);
    return;
  }
  FILE* f = std_open(path, "wb");
  if(!f) {
    jdl_return_bool(c, 0);
    return;
  }
  int ok = len == 0 || fwrite(body, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  jdl_return_bool(c, ok);
}

void std_file_remove(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  jdl_return_bool(c, c_string(c, 0, path, sizeof path) && (unlink(path) == 0 || errno == ENOENT));
}

static int by_name(const void* a, const void* b) {
  return strcmp(*(char* const*)a, *(char* const*)b);
}

static int list_dir(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  DIR* dir = c_string(c, 0, path, sizeof path) ? opendir(path) : NULL;
  if(!dir) return 0;
  char** names = NULL;
  size_t count = 0, cap = 0, total = 0;
  int ok = 1;
  struct dirent* entry;
  while(ok && (entry = readdir(dir)) != NULL) {
    if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    struct stat st;
#ifdef _WIN32
    char entry_path[STD_MAX_PATH];
    int is_dir = snprintf(entry_path, sizeof entry_path, "%s/%s", path, entry->d_name) <
                     (int)sizeof entry_path &&
                 stat(entry_path, &st) == 0 && S_ISDIR(st.st_mode);
#else
    int is_dir = fstatat(dirfd(dir), entry->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode);
#endif
    size_t len = strlen(entry->d_name);
    char** grown =
        count < cap ? names : (char**)realloc(names, (cap = cap ? 2 * cap : 16) * sizeof(char*));
    char* name = grown ? (char*)malloc(len + 2) : NULL;
    ok = name != NULL;
    if(grown) names = grown;
    if(ok) {
      memcpy(name, entry->d_name, len);
      if(is_dir) name[len++] = '/';
      name[len] = '\0';
      names[count++] = name;
      total += len + 1;
    }
  }
  closedir(dir);
  if(count > 0) qsort(names, count, sizeof(char*), by_name);
  uint8_t* out = ok && total <= UINT32_MAX ? (uint8_t*)malloc(total + 1) : NULL;
  size_t at = 0;
  for(size_t i = 0; i < count; i++) {
    size_t len = strlen(names[i]) + 1;
    if(out) memcpy(out + at, names[i], len);
    at += len;
    free(names[i]);
  }
  free(names);
  if(out) jdl_return_bytes_copy(c, out, (uint32_t)total);
  else jdl_fail(c, "out of memory");
  free(out);
  return 1;
}

void std_dir_list(jdlo_ctx* c) {
  if(!list_dir(c)) jdl_fail(c, "cannot open directory");
}

void std_dir_entries(jdlo_ctx* c) {
  if(!list_dir(c)) jdl_return_null(c);
}

void std_dir_make(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  if(!c_string(c, 0, path, sizeof path)) {
    jdl_return_bool(c, 0);
    return;
  }
  size_t root = jello_path_root_len(path);
  for(char* p = path + (root ? root : 1u); *p; p++) {
    if(!jello_is_slash(*p)) continue;
    char slash = *p;
    *p = '\0';
    if(jello_mkdir(path) != 0 && errno != EEXIST) {
      jdl_return_bool(c, 0);
      return;
    }
    *p = slash;
  }
  struct stat st;
  jdl_return_bool(c, (jello_mkdir(path) == 0 || errno == EEXIST) && stat(path, &st) == 0 &&
                         S_ISDIR(st.st_mode));
}

void std_file_real_path(jdlo_ctx* c) {
  char path[STD_MAX_PATH], real[PATH_MAX];
  int found = c_string(c, 0, path, sizeof path) && jello_real_path(path, real, sizeof real);
  jdl_return_bytes_copy(c, (const uint8_t*)real, found ? (uint32_t)strlen(real) : 0u);
}

void std_process_executable(jdlo_ctx* c) {
  char path[PATH_MAX];
  int found = jello_self_path(path, sizeof path);
  jdl_return_bytes_copy(c, (const uint8_t*)path, found ? (uint32_t)strlen(path) : 0u);
}

void std_platform_os(jdlo_ctx* c) {
#if defined(__APPLE__)
  static const char os[] = "macos";
#elif defined(_WIN32)
  static const char os[] = "windows";
#elif defined(__linux__)
  static const char os[] = "linux";
#else
#error "std.jdll does not know this system"
#endif
  jdl_return_bytes_copy(c, (const uint8_t*)os, sizeof os - 1);
}

void std_platform_arch(jdlo_ctx* c) {
#if defined(__aarch64__) || defined(_M_ARM64)
  static const char arch[] = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  static const char arch[] = "x64";
#else
#error "std.jdll does not know this processor"
#endif
  jdl_return_bytes_copy(c, (const uint8_t*)arch, sizeof arch - 1);
}

void std_platform_version(jdlo_ctx* c) {
  static const char version[] = JELLO_VERSION;
  jdl_return_bytes_copy(c, (const uint8_t*)version, sizeof version - 1);
}

int std_temp_dir(char* out, size_t cap) {
  const char* dir = getenv("TMPDIR");
#ifdef _WIN32
  if(!dir || !*dir) dir = getenv("TEMP");
  if(!dir || !*dir) dir = getenv("TMP");
#endif
  if(!dir || !*dir) dir = "/tmp";
  size_t len = strlen(dir);
  if(len >= cap) return 0;
  memcpy(out, dir, len);
  out[len] = '\0';
  jello_slashes_forward(out);
  size_t root = jello_path_root_len(out);
  while(len > (root ? root : 1u) && out[len - 1] == '/') len--;
  out[len] = '\0';
  return 1;
}

void std_platform_temp_dir(jdlo_ctx* c) {
  char dir[STD_MAX_PATH];
  if(!std_temp_dir(dir, sizeof dir)) jdl_fail(c, "the path is too long");
  else jdl_return_bytes_copy(c, (const uint8_t*)dir, (uint32_t)strlen(dir));
}

void std_get_env(jdlo_ctx* c) {
  char name[STD_MAX_PATH];
  const char* value = c_string(c, 0, name, sizeof name) ? getenv(name) : NULL;
  jdl_return_bytes_copy(c, (const uint8_t*)value, value ? (uint32_t)strlen(value) : 0u);
}

void std_argv_count(jdlo_ctx* c) {
#if defined(__APPLE__)
  jdl_return_i32(c, *_NSGetArgc());
#elif defined(_WIN32)
  jdl_return_i32(c, __argc);
#else
  int32_t count = 0;
  FILE* f = fopen("/proc/self/cmdline", "rb");
  if(f) {
    int ch;
    while((ch = fgetc(f)) != EOF) count += ch == 0;
    fclose(f);
  }
  jdl_return_i32(c, count);
#endif
}

char** std_entries(jdlo_ctx* c, int index, char** store, size_t* count) {
  const uint8_t* data = jdl_arg_bytes_data(c, index);
  uint32_t len = jdl_arg_bytes_len(c, index);
  *store = NULL;
  *count = 0;
  if(len > 0 && data[len - 1] != 0) return NULL;
  for(uint32_t i = 0; i < len; i++) *count += data[i] == 0;
  char** list = (char**)calloc(*count + 1, sizeof(char*));
  *store = (char*)malloc(len + 1u);
  if(!list || !*store) {
    free(list);
    free(*store);
    *store = NULL;
    return NULL;
  }
  if(len > 0) memcpy(*store, data, len);
  for(size_t i = 0, at = 0; i < *count; i++) {
    list[i] = *store + at;
    at += strlen(list[i]) + 1;
  }
  return list;
}

void std_cpu_count(jdlo_ctx* c) {
  long n = jello_cpu_count();
  jdl_return_i32(c, n > 1024 ? 1024 : (int32_t)n);
}

void std_args_count(jdlo_ctx* c) { jdl_return_i32(c, jdl_program_arg_count(c)); }

void std_args_get(jdlo_ctx* c) {
  const char* arg = jdl_program_arg(c, jdl_arg_i32(c, 0));
  if(!arg) {
    jdl_fail(c, "index out of range");
    return;
  }
  jdl_return_bytes_copy(c, (const uint8_t*)arg, (uint32_t)strlen(arg));
}

#ifdef _WIN32
static BOOL WINAPI ignore_break(DWORD event) {
  (void)event;
  return TRUE;
}

static void exec_windows(char** argv, char* reason, size_t cap) {
  static const DWORD streams[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  HANDLE copies[3] = {NULL, NULL, NULL};
  for(int i = 0; i < 3; i++) {
    HANDLE h = GetStdHandle(streams[i]);
    if(!h || h == INVALID_HANDLE_VALUE ||
       !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copies[i], 0, TRUE,
                        DUPLICATE_SAME_ACCESS)) {
      SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
      copies[i] = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
    }
  }
  SetConsoleCtrlHandler(ignore_break, TRUE);
  HANDLE child = std_spawn(argv, NULL, copies[0], copies[1], copies[2], reason, cap);
  for(int i = 0; i < 3; i++)
    if(copies[i] != INVALID_HANDLE_VALUE) CloseHandle(copies[i]);
  if(!child) {
    SetConsoleCtrlHandler(ignore_break, FALSE);
    return;
  }
  DWORD code = 1;
  WaitForSingleObject(child, INFINITE);
  GetExitCodeProcess(child, &code);
  ExitProcess(code);
}
#endif

void std_process_exec(jdlo_ctx* c) {
  char* store = NULL;
  size_t argc = 0;
  char** argv = std_entries(c, 0, &store, &argc);
  char reason[STD_MAX_PATH + 128];
  if(!argv || argc == 0) {
    snprintf(reason, sizeof reason,
             "process_exec: argv must be NUL-terminated entries, at least one");
  } else {
    fflush(NULL);
#ifdef _WIN32
    exec_windows(argv, reason, sizeof reason);
#else
    execvp(argv[0], argv);
    snprintf(reason, sizeof reason, "cannot run %s: %s", argv[0], strerror(errno));
#endif
  }
  free(argv);
  free(store);
  jdl_return_bytes_copy(c, (const uint8_t*)reason, (uint32_t)strlen(reason));
}

void std_process_exit(jdlo_ctx* c) {
  fflush(NULL);
  exit(jdl_arg_i32(c, 0) & 255);
}
