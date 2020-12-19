// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * jlo_tool's init and get-deps commands; create a project and fetch its dependencies into
 * packages/, pinned by jello.lock. */

#include "deps.h"

#include <jello/internal/platform.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winioctl.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define LOCK_VERSION 2

typedef struct text {
  char* data;
  size_t len, cap;
} text;

static void* xrealloc(void* p, size_t n) {
  void* q = realloc(p, n ? n : 1);
  if(!q) {
    fputs("jlo_tool: out of memory\n", stderr);
    exit(1);
  }
  return q;
}

static void text_add(text* t, const char* s, size_t n) {
  if(t->len + n + 1 > t->cap) {
    t->cap = (t->len + n + 1) * 2;
    t->data = xrealloc(t->data, t->cap);
  }
  memcpy(t->data + t->len, s, n);
  t->len += n;
  t->data[t->len] = '\0';
}

static void text_puts(text* t, const char* s) { text_add(t, s, strlen(s)); }

static char* vformat(const char* format, va_list ap) {
  va_list again;
  va_copy(again, ap);
  int n = vsnprintf(NULL, 0, format, ap);
  if(n < 0) {
    fputs("jlo_tool: cannot format text\n", stderr);
    exit(1);
  }
  char* s = xrealloc(NULL, (size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, format, again);
  va_end(again);
  return s;
}

static void text_printf(text* t, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  char* s = vformat(format, ap);
  va_end(ap);
  text_puts(t, s);
  free(s);
}

static char* path_of(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  char* s = vformat(format, ap);
  va_end(ap);
  return s;
}

static int write_file(const char* path, const char* data, size_t len) {
  FILE* f = fopen(path, "wb");
  if(!f) {
    fprintf(stderr, "jlo_tool: cannot write %s: %s\n", path, strerror(errno));
    return 0;
  }
  int ok = fwrite(data, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  if(!ok) fprintf(stderr, "jlo_tool: cannot write %s\n", path);
  return ok;
}

static int is_file(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}


typedef enum json_kind {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
} json_kind;

typedef struct json {
  json_kind kind;
  char* string; /**< A string's value, a number's text, "true" or "false". */
  size_t count;
  char** keys; /**< An object's keys, parallel to `items`; NULL for an array. */
  struct json** items;
} json;

typedef struct json_parser {
  const char* s;
  size_t len, at;
  const char* error;
} json_parser;

static void json_quote(text* t, const char* s) {
  text_puts(t, "\"");
  for(; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if(c == '"' || c == '\\') {
      char e[2] = {'\\', (char)c};
      text_add(t, e, 2);
    } else if(c < 0x20) text_printf(t, "\\u%04x", c);
    else text_add(t, s, 1);
  }
  text_puts(t, "\"");
}

static int kind_of(const char* path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  if(attrs == INVALID_FILE_ATTRIBUTES) return 0;
  if(attrs & FILE_ATTRIBUTE_REPARSE_POINT) return 'l';
  return attrs & FILE_ATTRIBUTE_DIRECTORY ? 'd' : 'f';
#else
  struct stat st;
  if(lstat(path, &st) != 0) return 0;
  return S_ISLNK(st.st_mode) ? 'l' : S_ISDIR(st.st_mode) ? 'd' : 'f';
#endif
}

static int make_dir(const char* path) {
  if(jello_mkdir(path) == 0 || errno == EEXIST) return 1;
  fprintf(stderr, "jlo_tool: cannot make %s: %s\n", path, strerror(errno));
  return 0;
}

typedef enum dep_kind { DEP_ARCHIVE, DEP_PATH } dep_kind;

typedef struct dep {
  char* name;
  dep_kind kind;
  char* source;  //< The archive's URL, or the path from the project's root.
  char* version; //< The version the archive must hold, or NULL.
  char* by;      //< Who asked for it; "jello.json" or a package's name.
} dep;

typedef struct deps {
  dep* items;
  size_t count;
} deps;

static int alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int valid_word(const char* word, const char* others) {
  if(!alnum(*word)) return 0;
  for(const char* c = word; *c; c++)
    if(!alnum(*c) && !strchr(others, *c)) return 0;
  return 1;
}

static int valid_name(const char* name) { return valid_word(name, "._-"); }

typedef struct locked {
  char* name;
  char* version;
  char* url;
  char* sha256;
} locked;

typedef struct lock {
  locked* items;
  size_t count;
} lock;

static int write_new(const char* path, const char* body) {
  if(kind_of(path)) return 1;
  return write_file(path, body, strlen(body));
}

static int init(const char* dir, const char* name) {
  char* manifest = path_of("%s/jello.json", dir);
  char* src = path_of("%s/src", dir);
  char* main_path = path_of("%s/src/Main.jello", dir);
  char* ignore = path_of("%s/.gitignore", dir);
  char resolved[PATH_MAX];
  text t = {0};
  int ok = 0;
  if(!make_dir(dir)) goto done;
  if(is_file(manifest)) {
    fprintf(stderr, "jlo_tool: %s is already a project: it has jello.json\n", dir);
    goto done;
  }
  if(!name) {
    if(!jello_real_path(dir, resolved, sizeof resolved)) {
      fprintf(stderr, "jlo_tool: %s: %s\n", dir, strerror(errno ? errno : ENOENT));
      goto done;
    }
    const char* slash = jello_last_slash(resolved);
    name = slash ? slash + 1 : resolved;
  }
  if(!valid_name(name)) {
    fprintf(
        stderr,
        "jlo_tool: \"%s\" cannot name a package (letters, digits, '.', '_' and '-'); give --name\n",
        name);
    goto done;
  }
  text_puts(&t, "{\n  \"name\": ");
  json_quote(&t, name);
  text_puts(&t, ",\n  \"version\": \"0.1.0\",\n  \"dependencies\": {}\n}\n");
  ok = make_dir(src) &&
       write_new(main_path, "// The program's last value is its output.\n\"Hello, world!\"\n") &&
       write_new(ignore, "/build/\n/packages/\n") && write_file(manifest, t.data, t.len);
  if(ok)
    fprintf(stderr, "made %s in %s: jlo_tool get-deps fetches what jello.json depends on\n", name,
            dir);
done:
  free(t.data);
  free(manifest);
  free(src);
  free(main_path);
  free(ignore);
  return ok ? 0 : 1;
}

int jlo_init_main(int argc, char** argv) {
  const char* dir = NULL;
  const char* name = NULL;
  for(int i = 0; i < argc; i++) {
    if(strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
    else if(argv[i][0] != '-' && !dir) dir = argv[i];
    else {
      fputs("usage: jlo_tool init [DIR] [--name NAME]\n", stderr);
      return 2;
    }
  }
  return init(dir ? dir : ".", name);
}
