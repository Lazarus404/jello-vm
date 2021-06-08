// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * Opening native libraries; finds and opens `<name>.jdll` and checks its jello_jdll_manifest. */

#include <jello/internal/jdll_internal.h>

#include <dlfcn.h>
#include <jello/internal/platform.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

struct jello_jdll_handle {
  void* dl;
  const jello_jdll_manifest* manifest;
  char* path;
};

#define JELLO_JDLL_SONAME_FMT "%s.jdll"

static char* join_path(const char* dir, const char* file) {
  size_t ld = strlen(dir), lf = strlen(file);
  int need_slash = (ld > 0 && dir[ld - 1] != '/');
  char* out = (char*)malloc(ld + lf + (size_t)need_slash + 1u);
  if(!out) return NULL;
  memcpy(out, dir, ld);
  size_t pos = ld;
  if(need_slash) out[pos++] = '/';
  memcpy(out + pos, file, lf);
  out[pos + lf] = '\0';
  return out;
}

static void* try_dlopen_in(const char* dir, const char* soname) {
  char* path = join_path(dir, soname);
  if(!path) return NULL;
  void* dl = (access(path, F_OK) == 0) ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
  free(path);
  return dl;
}

static void* resolve_and_open(const char* library_name) {
  char soname[256];
  snprintf(soname, sizeof soname, JELLO_JDLL_SONAME_FMT, library_name);

  void* dl = try_dlopen_in(".", soname);
  if(dl) return dl;

  const char* search = getenv("JELLO_JDLL_PATH");
  if(!search || !search[0]) return NULL;

  char* copy = (char*)malloc(strlen(search) + 1u);
  if(!copy) return NULL;
  strcpy(copy, search);
  char* tok = copy;
  while(tok && *tok) {
    char* sep = strchr(tok, ':');
    if(sep) *sep = '\0';
    if(*tok) {
      dl = try_dlopen_in(tok, soname);
      if(dl) { free(copy); return dl; }
    }
    tok = sep ? sep + 1 : NULL;
  }
  free(copy);
  return NULL;
}

static char* g_search;
static _Thread_local char g_jdll_err[4608];

int jello_directory_of(const char* path, char* dir) {
  const char* slash = jello_last_slash(path);
  int len = slash ? (slash == path ? 1 : (int)(slash - path)) : 1;
  return snprintf(dir, PATH_MAX, "%.*s", len, slash ? path : ".") < PATH_MAX;
}

int jello_jdll_search_beside(const char* program, const char* dirs) {
  char dir[PATH_MAX], search[3 * PATH_MAX];
  int n = 0;
  if(program) {
    if(!jello_directory_of(program, dir)) return 0;
    n = snprintf(search, sizeof search, "%s/jdll%c%s", dir, JELLO_PATH_LIST_SEP, dir);
  }
  if(dirs && n >= 0 && n < (int)sizeof search) {
    if(n > 0) search[n++] = JELLO_PATH_LIST_SEP;
    n += snprintf(search + n, sizeof search - (size_t)n, "%s", dirs);
  }
  if(n < 0 || n >= (int)sizeof search) return 0;
  free(g_search);
  g_search = n > 0 ? strdup(search) : NULL;
  return 1;
}

const char* jello_jdll_last_error(void) { return g_jdll_err; }

int jello_executable_path(char* out) { return jello_self_path(out, PATH_MAX); }

typedef struct search {
  char soname[256];
  char path[PATH_MAX];
  void* dl;
  char tried[4096];
  char unopened[sizeof "cannot open : " + PATH_MAX + 256];
} search;

jello_jdll_handle* jello_jdll_load(const char* library_name) {
  if(!library_name || !library_name[0]) return NULL;

  void* dl = resolve_and_open(library_name);
  if(!dl) return NULL;

  char sym[256];
  snprintf(sym, sizeof sym, "jello_jdll_manifest_%s", library_name);
  const jello_jdll_manifest* manifest = (const jello_jdll_manifest*)dlsym(dl, sym);
  if(!manifest || manifest->magic != JELLO_JDLL_MANIFEST_MAGIC ||
     manifest->abi_version != JELLO_JDLL_ABI_VERSION ||
     !manifest->library_name || strcmp(manifest->library_name, library_name) != 0) {
    dlclose(dl);
    return NULL;
  }

  jello_jdll_handle* h = (jello_jdll_handle*)malloc(sizeof(jello_jdll_handle));
  if(!h) { dlclose(dl); return NULL; }
  h->dl = dl;
  h->manifest = manifest;
  return h;
}

const char* jello_jdll_path(const jello_jdll_handle* h) { return h->path; }

void jello_jdll_unload(jello_jdll_handle* h) {
  if(!h) return;
  if(h->dl) jello_dl_close(h->dl);
  free(h->path);
  free(h);
}

void* jello_jdll_symbol(const jello_jdll_handle* h, const char* name) {
  return h && h->dl ? jello_dl_symbol(h->dl, name) : NULL;
}

const jello_jdll_export* jello_jdll_find_export(const jello_jdll_handle* h, const char* name) {
  if(!h || !h->manifest) return NULL;
  for(uint32_t i = 0; i < h->manifest->nexports; i++) {
    if(strcmp(h->manifest->exports[i].name, name) == 0) return &h->manifest->exports[i];
  }
  return NULL;
}
