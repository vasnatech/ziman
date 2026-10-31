#ifndef ZIMAN_XALLOC_H
#define ZIMAN_XALLOC_H

#include <stddef.h>

/* Allocation that does not return NULL, because almost nothing in this interpreter can do anything
 * useful with a NULL and almost nothing checked for one: 13 checks across 283 malloc/calloc sites
 * and 3 across 80 reallocs, before 2026-10-08 (docs/reviews/tasks/ A20). That is defensible for a
 * CLI that the OS is about to reclaim anyway, and not for the always-on backend this is meant to
 * become -- and the worst case was not the missing check but `src/lsp.c`'s `sb_grow`, which did
 * `p = realloc(p, n)` and so lost the old pointer *and* then wrote through the NULL, in the one
 * process a user keeps open all day.
 *
 * So these four report and exit instead, naming the file and line of the call. One exit beats 363
 * half-checks, and a caller that genuinely has something better to do keeps calling the plain
 * allocator and checking -- `src/regex.c` is the one that does, because its buffers are the only
 * ones big enough to fail on a healthy machine, and it can turn that into an ordinary Ziman
 * runtime error instead of taking the process down.
 *
 * Why functions rather than macros wrapping the allocator at the call site: the size must be
 * evaluated exactly once. zgc.h's own comment records what the other shape costs -- a macro that
 * expanded the size expression twice around a realloc read `node->n` after the realloc had already
 * moved `node`, which is undefined behaviour and reported a 32 GB allocation site that did not
 * exist. Evaluating once also means a zero-size request can be normalised to one byte, so a
 * legitimate `realloc(p, 0)` returning NULL is not mistaken for failure.
 *
 * The allocator underneath is whatever the build uses: the Boehm collector in the default build,
 * plain libc under `nogc`, and the per-site counters under `-DZIMAN_ALLOC_PROFILE`, which still
 * attribute each allocation to its real call site because these take the caller's file and line
 * rather than letting zgc.h stamp this file on everything. */
/* The same failure for an allocator these wrappers do not own: the collector's own
 * GC_MALLOC_ATOMIC and GC_MALLOC_UNCOLLECTABLE, which take a size this file cannot normalise and
 * return NULL the same way. The argument is evaluated once, so wrapping a call is safe. */
void *ziman_xcheck_at(void *p, const char *file, int line);

void *ziman_xmalloc_at(const char *file, int line, size_t n);
void *ziman_xcalloc_at(const char *file, int line, size_t count, size_t size);
void *ziman_xrealloc_at(const char *file, int line, void *p, size_t n);
char *ziman_xstrdup_at(const char *file, int line, const char *s);

#define ziman_xcheck(p) ziman_xcheck_at((p), __FILE__, __LINE__)
#define ziman_xmalloc(n) ziman_xmalloc_at(__FILE__, __LINE__, (n))
#define ziman_xcalloc(count, size) ziman_xcalloc_at(__FILE__, __LINE__, (count), (size))
#define ziman_xrealloc(p, n) ziman_xrealloc_at(__FILE__, __LINE__, (p), (n))
#define ziman_xstrdup(s) ziman_xstrdup_at(__FILE__, __LINE__, (s))

/* FNV-1a over a NUL-terminated string. Here rather than in a module of its own because it has
 * exactly one caller family -- the generated-code memo keys on a module's source text -- and it is
 * a content hash for a cache, never a security claim: a collision would serve one module's
 * generated code for another, which is why the memo compares the declaration's name and the
 * generator's identity as well, not the hash alone. */
unsigned long long ziman_hash_text(const char *s);

#endif
