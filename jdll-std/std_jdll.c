// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/* -- the std JDLL -- */

#include <jello/jdll.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
#include <stdio.h>
#endif

/* -- argv -- */
static int g_argv_loaded = 0;
static int g_argc = 0;
static char** g_argv = NULL;

static void load_argv_once(void) {
  if(g_argv_loaded) return;
  g_argv_loaded = 1;
#if defined(__APPLE__)
  g_argc = *_NSGetArgc();
  g_argv = *_NSGetArgv();
#else
  FILE* f = fopen("/proc/self/cmdline", "rb");
  if(!f) return;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  size_t count = 0;
  for(size_t i = 0; i < n; i++) if(buf[i] == '\0') count++;
  if(n > 0 && buf[n - 1] != '\0') count++;
  char** argv = (char**)calloc(count + 1u, sizeof(char*));
  if(!argv) return;
  size_t idx = 0, start = 0;
  for(size_t i = 0; i < n; i++) {
    if(buf[i] == '\0') {
      argv[idx++] = strdup(buf + start);
      start = i + 1u;
    }
  }
  g_argc = (int)idx;
  g_argv = argv;
#endif
}

static void std_argv_count(jdlo_ctx* c) {
  load_argv_once();
  jdl_return_i32(c, g_argc);
}

static void std_argv_get(jdlo_ctx* c) {
  load_argv_once();
  int32_t idx = jdl_arg_i32(c, 0);
  if(idx < 0 || idx >= g_argc) {
    jdl_fail(c, "std.argv_get: index out of range");
    return;
  }
  jdl_return_bytes_copy(c, (const uint8_t*)g_argv[idx], (uint32_t)strlen(g_argv[idx]));
}

/* -- environment -- */

static char* nul_terminated_copy(const uint8_t* data, uint32_t len) {
  char* s = (char*)malloc((size_t)len + 1u);
  if(!s) return NULL;
  if(len) memcpy(s, data, len);
  s[len] = '\0';
  return s;
}

static void std_get_env(jdlo_ctx* c) {
  char* name = nul_terminated_copy(jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0));
  if(!name) { jdl_fail(c, "std.get_env: out of memory"); return; }
  const char* val = getenv(name);
  free(name);
  jdl_return_bytes_copy(c, (const uint8_t*)(val ? val : ""), (uint32_t)(val ? strlen(val) : 0u));
}

/* -- file I/O -- */

static void std_file_read(jdlo_ctx* c) {
  char* path = nul_terminated_copy(jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0));
  if(!path) { jdl_fail(c, "std.file_read: out of memory"); return; }

  FILE* f = fopen(path, "rb");
  free(path);
  if(!f) { jdl_fail(c, "std.file_read: cannot open file"); return; }
  if(fseek(f, 0, SEEK_END) != 0) { fclose(f); jdl_fail(c, "std.file_read: seek failed"); return; }
  long size = ftell(f);
  if(size < 0) { fclose(f); jdl_fail(c, "std.file_read: tell failed"); return; }
  if(fseek(f, 0, SEEK_SET) != 0) { fclose(f); jdl_fail(c, "std.file_read: seek failed"); return; }

  uint8_t* buf = (uint8_t*)malloc((size_t)size > 0 ? (size_t)size : 1u);
  if(!buf) { fclose(f); jdl_fail(c, "std.file_read: out of memory"); return; }
  size_t n = fread(buf, 1, (size_t)size, f);
  fclose(f);
  jdl_return_bytes_copy(c, buf, (uint32_t)n);
  free(buf);
}

static void std_file_write(jdlo_ctx* c) {
  char* path = nul_terminated_copy(jdl_arg_bytes_data(c, 0), jdl_arg_bytes_len(c, 0));
  if(!path) { jdl_fail(c, "std.file_write: out of memory"); return; }

  const uint8_t* data = jdl_arg_bytes_data(c, 1);
  uint32_t len = jdl_arg_bytes_len(c, 1);

  FILE* f = fopen(path, "wb");
  free(path);
  if(!f) { jdl_return_bool(c, 0); return; }
  size_t written = len ? fwrite(data, 1, len, f) : 0u;
  fclose(f);
  jdl_return_bool(c, written == (size_t)len);
}

static const jello_jdll_export STD_EXPORTS[] = {
  {"file_read", std_file_read, (uint8_t)JELLO_T_BYTES, 1, {(uint8_t)JELLO_T_BYTES}},
  {"file_write", std_file_write, (uint8_t)JELLO_T_BOOL, 2, {(uint8_t)JELLO_T_BYTES, (uint8_t)JELLO_T_BYTES}},
  {"get_env", std_get_env, (uint8_t)JELLO_T_BYTES, 1, {(uint8_t)JELLO_T_BYTES}},
  {"argv_count", std_argv_count, (uint8_t)JELLO_T_I32, 0, {0}},
  {"argv_get", std_argv_get, (uint8_t)JELLO_T_BYTES, 1, {(uint8_t)JELLO_T_I32}},
};

const jello_jdll_manifest jello_jdll_manifest_std = {
  JELLO_JDLL_MANIFEST_MAGIC,
  JELLO_JDLL_ABI_VERSION,
  "std",
  STD_EXPORTS,
  (uint32_t)(sizeof(STD_EXPORTS) / sizeof(STD_EXPORTS[0])),
};