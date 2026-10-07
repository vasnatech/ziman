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
    unsigned long long count;
    unsigned long long bytes;
} Site;

static Site g_sites[SITES];
static int g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void *ziman_alloc_note(const char *file, int line, size_t n, void *p) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; i++) {
        if (g_sites[i].line == line && g_sites[i].file == file) {
            g_sites[i].count++;
            g_sites[i].bytes += n;
            pthread_mutex_unlock(&g_mu);
            return p;
        }
    }
    if (g_n < SITES) {
        g_sites[g_n].file = file;
        g_sites[g_n].line = line;
        g_sites[g_n].count = 1;
        g_sites[g_n].bytes = n;
        g_n++;
    }
    pthread_mutex_unlock(&g_mu);
    return p;
}

static int by_count(const void *a, const void *b) {
    const Site *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

__attribute__((destructor)) static void dump(void) {
    pthread_mutex_lock(&g_mu);
    qsort(g_sites, (size_t)g_n, sizeof g_sites[0], by_count);
    unsigned long long total = 0, total_bytes = 0;
    for (int i = 0; i < g_n; i++) { total += g_sites[i].count; total_bytes += g_sites[i].bytes; }
    fprintf(stderr, "\n== allocations by site: %llu calls, %llu bytes, %d sites ==\n", total, total_bytes, g_n);
    for (int i = 0; i < g_n && i < 25; i++) {
        fprintf(stderr, "%7.3f%%  %10llu calls  %12llu bytes  %s:%d\n",
                100.0 * (double)g_sites[i].count / (double)(total ? total : 1),
                g_sites[i].count, g_sites[i].bytes, g_sites[i].file, g_sites[i].line);
    }
    pthread_mutex_unlock(&g_mu);
}
#endif
