/* A temporary measurement aid for F7: counts allocations per source site so the hottest allocators
 * can be named from evidence rather than guessed at. Built only with -DZIMAN_ALLOC_PROFILE; the
 * normal build does not contain it. Dumps to stderr at exit, hottest first. */
#ifdef ZIMAN_ALLOC_PROFILE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SITES 4096

typedef struct {
    const char *file;
    int line;
    /* Part of the identity, not a label: the same line could in principle allocate both ways,
     * and collapsing them would hide exactly the thing being measured. */
    int atomic;
    /* 0 = Boehm (zgc.h's malloc family, xalloc, zalloc_atomic), 1 = Ziman's own per-thread
     * collector (src/gc_heap.c). Two separate heaps with separate mark phases, so a byte's cost
     * depends on which one holds it. */
    int heap;
    unsigned long long count;
    unsigned long long bytes;
} Site;

static Site g_sites[SITES];
static int g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void note_at(const char *file, int line, size_t n, int atomic, int heap) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; i++) {
        if (g_sites[i].line == line && g_sites[i].file == file && g_sites[i].atomic == atomic &&
            g_sites[i].heap == heap) {
            g_sites[i].count++;
            g_sites[i].bytes += n;
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    if (g_n < SITES) {
        g_sites[g_n].file = file;
        g_sites[g_n].line = line;
        g_sites[g_n].atomic = atomic;
        g_sites[g_n].heap = heap;
        g_sites[g_n].count = 1;
        g_sites[g_n].bytes = n;
        g_n++;
    }
    pthread_mutex_unlock(&g_mu);
}

static void note(const char *file, int line, size_t n, int atomic) {
    note_at(file, line, n, atomic, 0);
}

/* See zgc.h's comment on the declaration: this heap allocates from its own arena, so there is
 * nothing to forward -- only the accounting happens here. */
void ziman_alloc_note_heap(const char *file, int line, size_t n, int atomic) {
    note_at(file, line, n, atomic, 1);
}

/* GC_MALLOC and friends are named explicitly here: zgc.h has redefined `malloc` to come back through
 * this file, so calling it by that name would recurse. */
void *ziman_alloc_malloc(const char *file, int line, size_t n) {
    note(file, line, n, 0);
    return GC_MALLOC(n);
}

/* See zgc.h's comment on the declaration. */
void *ziman_alloc_atomic(const char *file, int line, size_t n) {
    note(file, line, n, 1);
    return GC_MALLOC_ATOMIC(n);
}

void *ziman_alloc_realloc(const char *file, int line, void *p, size_t n) {
    note(file, line, n, 0);
    return GC_REALLOC(p, n);
}

char *ziman_alloc_strdup(const char *file, int line, const char *s) {
    note(file, line, strlen(s) + 1, 0);
    return GC_STRDUP(s);
}

static int by_count(const void *a, const void *b) {
    const Site *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

/* Both orders are printed, because they answer different questions: how often a site runs drives
 * collection frequency, how many bytes it asks for drives the heap's size. A realloc site counts its
 * whole new size every time it grows, so a buffer that doubles its way to a megabyte is credited with
 * far more than it ever holds -- which is why the two lists disagree and why neither is "the" answer. */
static int by_bytes(const void *a, const void *b) {
    const Site *x = a, *y = b;
    return x->bytes < y->bytes ? 1 : x->bytes > y->bytes ? -1 : 0;
}

__attribute__((destructor)) static void dump(void) {
    pthread_mutex_lock(&g_mu);
    qsort(g_sites, (size_t)g_n, sizeof g_sites[0], by_count);
    unsigned long long total = 0, total_bytes = 0;
    for (int i = 0; i < g_n; i++) { total += g_sites[i].count; total_bytes += g_sites[i].bytes; }
    unsigned long long b[2][2] = {{0, 0}, {0, 0}}, c[2][2] = {{0, 0}, {0, 0}};
    for (int i = 0; i < g_n; i++) {
        b[g_sites[i].heap][g_sites[i].atomic] += g_sites[i].bytes;
        c[g_sites[i].heap][g_sites[i].atomic] += g_sites[i].count;
    }
    unsigned long long walked = b[0][0] + b[1][0];
    fprintf(stderr, "\n== allocations by site: %llu calls, %llu bytes, %d sites ==\n", total, total_bytes, g_n);
    /* The scanned total is the one that drives mark time: those are the bytes the collector walks
     * on every cycle they are live for. The atomic total is paid for once at allocation and once
     * at sweep, and never walked. A site moved from the first column to the second costs nothing
     * and saves marking, which is what makes this split worth printing first. */
    fprintf(stderr, "   boehm  traced : %12llu bytes %10llu calls\n", b[0][0], c[0][0]);
    fprintf(stderr, "   boehm  atomic : %12llu bytes %10llu calls\n", b[0][1], c[0][1]);
    fprintf(stderr, "   zheap  traced : %12llu bytes %10llu calls\n", b[1][0], c[1][0]);
    fprintf(stderr, "   zheap  atomic : %12llu bytes %10llu calls\n", b[1][1], c[1][1]);
    fprintf(stderr, "   walked by a mark phase: %llu bytes (%.1f%% of all bytes)\n",
            walked, 100.0 * (double)walked / (double)(total_bytes ? total_bytes : 1));
    for (int i = 0; i < g_n && i < 15; i++) {
        fprintf(stderr, "%7.3f%%  %10llu calls  %12llu bytes  %-5s %-6s %s:%d\n",
                100.0 * (double)g_sites[i].count / (double)(total ? total : 1),
                g_sites[i].count, g_sites[i].bytes, g_sites[i].heap ? "zheap" : "boehm", g_sites[i].atomic ? "atomic" : "traced",
                g_sites[i].file, g_sites[i].line);
    }
    qsort(g_sites, (size_t)g_n, sizeof g_sites[0], by_bytes);
    fprintf(stderr, "-- by bytes --\n");
    for (int i = 0; i < g_n && i < 10; i++) {
        fprintf(stderr, "%7.3f%%  %12llu bytes  %10llu calls  %-5s %-6s %s:%d\n",
                100.0 * (double)g_sites[i].bytes / (double)(total_bytes ? total_bytes : 1),
                g_sites[i].bytes, g_sites[i].count, g_sites[i].heap ? "zheap" : "boehm",
                g_sites[i].atomic ? "atomic" : "traced",
                g_sites[i].file, g_sites[i].line);
    }
    pthread_mutex_unlock(&g_mu);
}
#else
/* -Wpedantic refuses an empty translation unit, and this file is empty in every build but the
 * profiling one. A typedef nobody uses is the standard way to give it something to read. */
typedef int ziman_allocsites_unused_in_this_build;
#endif
