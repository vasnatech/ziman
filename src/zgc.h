#ifndef ZIMAN_ZGC_H
#define ZIMAN_ZGC_H

/* Force-included into every translation unit of a `make gc` build (see the
 * Makefile): routes the interpreter's allocations through the Boehm-Demers-
 * Weiser collector. Ziman's values are immutable and shared by pointer, so a
 * conservative, thread-aware tracing collector needs no changes to the
 * interpreter's ownership model -- nothing is ever freed by hand, and a
 * `free` becomes a no-op. gc.h comes first so that with GC_THREADS it
 * redirects pthread_create before anything else declares it. */
#include <gc.h>
#include <stdlib.h>
#include <string.h>

#define malloc(n) GC_MALLOC(n)
#define calloc(a, b) GC_MALLOC((a) * (b))
#define realloc(p, n) GC_REALLOC((p), (n))
#define strdup(s) GC_STRDUP(s)
#define free(p) ((void)(p))

#endif
