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

#ifdef ZIMAN_ALLOC_PROFILE
/* A measurement build (see src/allocsites.c): every allocation is counted against its source site.
 *
 * Each of these takes the size as an ordinary argument, evaluated once, rather than expanding it twice
 * into a call that also allocates. The first version did the latter, and it was wrong in a way worth
 * remembering: `realloc(node, sizeof(MapNode) + node->n * sizeof(Slot))` expanded to a call taking both
 * the size expression *and* GC_REALLOC's result, argument evaluation order is unspecified, so the size
 * could be computed from `node->n` after the realloc had already moved `node`. That is undefined
 * behaviour, and in practice it reported a 32GB allocation site that did not exist. */
#include <pthread.h>
void *ziman_alloc_malloc(const char *file, int line, size_t n);
void *ziman_alloc_realloc(const char *file, int line, void *p, size_t n);
char *ziman_alloc_strdup(const char *file, int line, const char *s);
#define malloc(n) ziman_alloc_malloc(__FILE__, __LINE__, (n))
#define calloc(a, b) ziman_gc_calloc_at(__FILE__, __LINE__, (size_t)(a), (size_t)(b))
#define realloc(p, n) ziman_alloc_realloc(__FILE__, __LINE__, (p), (n))
#define strdup(s) ziman_alloc_strdup(__FILE__, __LINE__, (s))
#else
#define malloc(n) GC_MALLOC(n)
#define calloc(a, b) ziman_gc_calloc_at(__FILE__, __LINE__, (size_t)(a), (size_t)(b))
#define realloc(p, n) GC_REALLOC((p), (n))
#define strdup(s) GC_STRDUP(s)
#endif
#define free(p) ((void)(p))

/* `calloc` goes through this rather than straight to the allocator, because `GC_MALLOC((a) * (b))`
 * -- what this header used to expand to -- silently dropped the overflow check libc's calloc makes:
 * the product wraps, a small block comes back, and the caller fills it as though it were the size it
 * asked for (docs/reviews/tasks.md F15). A function, so each argument is evaluated exactly once, for
 * the reason the comment above this one records. It zeroes, as both allocators underneath already do,
 * and reports and exits on failure like the rest of src/xalloc.h. */
void *ziman_gc_calloc_at(const char *file, int line, size_t count, size_t size);

#endif
