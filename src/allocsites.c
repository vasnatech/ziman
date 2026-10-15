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

/* The live set, by kind, accumulated across every collection -- see zgc.h's comment on
 * ziman_alloc_note_live. Summed rather than sampled: the total of what each cycle found live is
 * the work the mark phase actually did over the run, which is the quantity mark time is
 * proportional to. The dead column beside it is the garbage each cycle reclaimed, so the two
 * together say whether a kind is expensive because it survives or merely because there is a lot
 * of it. */
#define KINDS 16
typedef struct {
    const char *kind;
    unsigned long long live_objects, live_bytes, dead_objects, dead_bytes;
} KindStat;
static KindStat g_kinds[KINDS];
static int g_kn;
static unsigned long long g_collections, g_peak_live_bytes, g_peak_live_objects;
static unsigned long long g_total_live_bytes, g_total_live_objects, g_total_dead_objects;
static unsigned long long g_mark_ns, g_sweep_ns;

/* Enum allocations by variant. Compared by string, not by pointer: the names are borrowed from
 * wherever the allocation came from, and the same variant reaches here under two different
 * pointers -- once from the AST of the module that declared it, once from a native's own string
 * literal (native_json.c builds `Json` values itself). Pointer comparison listed `Json.JString`
 * twice, at 120600 and 120000, which reads as two different things. */
#define VARIANTS 256
typedef struct {
    const char *enum_name;
    const char *variant_name;
    int has_payload;
    unsigned long long count;
} VariantStat;
static VariantStat g_variants[VARIANTS];
static int g_vn;
static unsigned long long g_enum_allocs, g_enum_payloadless;
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

void ziman_alloc_note_phases(unsigned long long mark_ns, unsigned long long sweep_ns) {
    pthread_mutex_lock(&g_mu);
    g_mark_ns += mark_ns;
    g_sweep_ns += sweep_ns;
    pthread_mutex_unlock(&g_mu);
}

void ziman_alloc_note_enum(const char *enum_name, const char *variant_name, int has_payload) {
    pthread_mutex_lock(&g_mu);
    g_enum_allocs++;
    if (!has_payload) g_enum_payloadless++;
    for (int i = 0; i < g_vn; i++) {
        if (strcmp(g_variants[i].enum_name, enum_name) == 0 &&
            strcmp(g_variants[i].variant_name, variant_name) == 0) {
            g_variants[i].count++;
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    if (g_vn < VARIANTS) {
        g_variants[g_vn].enum_name = enum_name;
        g_variants[g_vn].variant_name = variant_name;
        g_variants[g_vn].has_payload = has_payload;
        g_variants[g_vn].count = 1;
        g_vn++;
    }
    pthread_mutex_unlock(&g_mu);
}

void ziman_alloc_note_live(const char *kind, size_t bytes, int live) {
    pthread_mutex_lock(&g_mu);
    KindStat *k = NULL;
    for (int i = 0; i < g_kn; i++) {
        if (g_kinds[i].kind == kind) { k = &g_kinds[i]; break; }
    }
    if (k == NULL && g_kn < KINDS) {
        k = &g_kinds[g_kn++];
        k->kind = kind;
    }
    if (k != NULL) {
        if (live) {
            k->live_objects++;
            k->live_bytes += bytes;
        } else {
            k->dead_objects++;
            k->dead_bytes += bytes;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void ziman_alloc_note_collection(size_t live_bytes, size_t live_objects, size_t dead_objects) {
    pthread_mutex_lock(&g_mu);
    g_collections++;
    g_total_live_bytes += live_bytes;
    g_total_live_objects += live_objects;
    g_total_dead_objects += dead_objects;
    if (live_bytes > g_peak_live_bytes) g_peak_live_bytes = live_bytes;
    if (live_objects > g_peak_live_objects) g_peak_live_objects = live_objects;
    pthread_mutex_unlock(&g_mu);
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
static int by_live_bytes(const void *a, const void *b) {
    const KindStat *x = a, *y = b;
    return x->live_bytes < y->live_bytes ? 1 : x->live_bytes > y->live_bytes ? -1 : 0;
}

static int by_variant_count(const void *a, const void *b) {
    const VariantStat *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

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
    if (g_collections > 0) {
        qsort(g_kinds, (size_t)g_kn, sizeof g_kinds[0], by_live_bytes);
        fprintf(stderr, "\n== live set in the per-thread heap: %llu collections ==\n", g_collections);
        fprintf(stderr, "   summed over every cycle, this is what the mark phase walked:\n");
        fprintf(stderr, "   %llu bytes in %llu objects live; %llu objects reclaimed\n",
                g_total_live_bytes, g_total_live_objects, g_total_dead_objects);
        fprintf(stderr, "   peak live in one cycle: %llu bytes in %llu objects\n",
                g_peak_live_bytes, g_peak_live_objects);
        fprintf(stderr, "   %-9s %14s %12s   %14s %12s  %s\n",
                "kind", "live bytes", "live objs", "dead bytes", "dead objs", "survival");
        for (int i = 0; i < g_kn; i++) {
            unsigned long long seen = g_kinds[i].live_objects + g_kinds[i].dead_objects;
            fprintf(stderr, "   %-9s %14llu %12llu   %14llu %12llu  %5.1f%%\n",
                    g_kinds[i].kind, g_kinds[i].live_bytes, g_kinds[i].live_objects,
                    g_kinds[i].dead_bytes, g_kinds[i].dead_objects,
                    100.0 * (double)g_kinds[i].live_objects / (double)(seen ? seen : 1));
        }
        unsigned long long gc_ns = g_mark_ns + g_sweep_ns;
        fprintf(stderr, "   mark %.1f ms (%.1f%%), sweep %.1f ms (%.1f%%)\n",
                (double)g_mark_ns / 1e6, 100.0 * (double)g_mark_ns / (double)(gc_ns ? gc_ns : 1),
                (double)g_sweep_ns / 1e6, 100.0 * (double)g_sweep_ns / (double)(gc_ns ? gc_ns : 1));
        fprintf(stderr, "   (Boehm's own heap is not included: its mark phase is inside the\n"
                        "    collector, which this build does not instrument.)\n");
    }
    if (g_enum_allocs > 0) {
        qsort(g_variants, (size_t)g_vn, sizeof g_variants[0], by_variant_count);
        fprintf(stderr, "\n== enum allocations by variant: %llu total, %d variants ==\n",
                g_enum_allocs, g_vn);
        /* A payload-less variant holds nothing that distinguishes one instance from another, so
         * this count is exactly how many allocations a shared singleton per variant would
         * remove. A variant with a payload cannot be shared and is listed for scale only. */
        fprintf(stderr, "   %llu of them carry no payload (%.1f%%), and so could be one shared\n"
                        "   object per variant rather than an allocation each\n",
                g_enum_payloadless,
                100.0 * (double)g_enum_payloadless / (double)g_enum_allocs);
        for (int i = 0; i < g_vn && i < 12; i++) {
            fprintf(stderr, "%7.3f%%  %12llu  %-9s %s.%s\n",
                    100.0 * (double)g_variants[i].count / (double)g_enum_allocs,
                    g_variants[i].count, g_variants[i].has_payload ? "payload" : "EMPTY",
                    g_variants[i].enum_name, g_variants[i].variant_name);
        }
    }
    pthread_mutex_unlock(&g_mu);
}
#else
/* -Wpedantic refuses an empty translation unit, and this file is empty in every build but the
 * profiling one. A typedef nobody uses is the standard way to give it something to read. */
typedef int ziman_allocsites_unused_in_this_build;
#endif
