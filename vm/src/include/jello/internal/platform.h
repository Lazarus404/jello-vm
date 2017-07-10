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

#ifdef _WIN32
#else
#define JELLO_PAGES_COMMIT_COSTS 0
#endif

#ifdef _WIN32
#else
#define JELLO_PATH_LIST_SEP ':'
#endif

#ifdef _WIN32
#else
#endif

#ifdef _WIN32
#endif
