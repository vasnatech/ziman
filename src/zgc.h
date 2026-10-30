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
/* `zalloc_atomic` (src/value.h) reaches GC_MALLOC_ATOMIC without passing through any of the
 * three names above, so without this it was invisible: a profile of a string-heavy program
 * showed only the parser, because every string and packed array it built was atomic. Counted
 * apart rather than together, because the distinction is the whole point -- an atomic block is
 * not walked by the mark phase, a scanned one is walked on every cycle it is live for. */
void *ziman_alloc_atomic(const char *file, int line, size_t n);
/* Ziman's own per-thread collector (src/gc_heap.c), which allocates from its own arena and so
 * never touches any of the names above. Records only; the allocation itself is that heap's. */
void ziman_alloc_note_heap(const char *file, int line, size_t n, int atomic);
/* The live set, tallied by the sweep phase (src/gc_heap.c), which already visits every object
 * and already knows whether the mark phase reached it. Allocation volume does not predict mark
 * cost -- a short-lived object is never marked at all -- so this is the number that does: what
 * the next mark phase will have to walk. Per object, then once per cycle. */
void ziman_alloc_note_live(const char *kind, size_t bytes, int live);
void ziman_alloc_note_collection(size_t live_bytes, size_t live_objects, size_t dead_objects);
/* Mark and sweep separately, because the counts only let the split be inferred: sweep visits
 * every object and mark only the reachable ones, so on a program whose live set is small the
 * two are nothing alike and a single "collection" bucket hides which one to attack. */
void ziman_alloc_note_phases(unsigned long long mark_ns, unsigned long long sweep_ns);
/* Counted where they are allocated rather than where they die, because the question this
 * answers is how many allocations could be avoided outright: a variant carrying no payload
 * holds nothing that distinguishes one instance from another, so every one of them could be a
 * single shared object instead of a fresh heap allocation. */
void ziman_alloc_note_enum(const char *enum_name, const char *variant_name, int has_payload);
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
 * asked for (docs/reviews/tasks/ F15). A function, so each argument is evaluated exactly once, for
 * the reason the comment above this one records. It zeroes, as both allocators underneath already do,
 * and reports and exits on failure like the rest of src/xalloc.h. */
void *ziman_gc_calloc_at(const char *file, int line, size_t count, size_t size);

#endif
