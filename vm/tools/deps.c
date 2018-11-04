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
