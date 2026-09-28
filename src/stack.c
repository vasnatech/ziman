#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "stack.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "error.h"

__thread char *ziman_stack_limit = NULL;
static __thread size_t t_stack_size = 0;
static __thread int t_is_main = 0;

static long g_main_mb = 0, g_thread_mb = 0, g_threads = 0; /* from the options */

void ziman_stack_mark_main(void) { t_is_main = 1; }
int ziman_stack_is_main(void) { return t_is_main; }

size_t ziman_stack_remaining(void) {
    char probe;
    if (ziman_stack_limit == NULL) ziman_stack_slow();
    return &probe > ziman_stack_limit ? (size_t)(&probe - ziman_stack_limit) : 0;
}

/* The margin kept below the limit: room to raise the error and unwind, and for a native that was mid-recursion when the check tripped. */
static size_t margin_for(size_t stack_size) {
    size_t margin = stack_size / 50;
    return margin > 262144 ? margin : 262144;
}

static void measure_this_thread(void) {
    pthread_attr_t attr;
    void *low = NULL;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return; /* cannot measure: no limit rather than a wrong one */
    pthread_attr_getstack(&attr, &low, &size);
    pthread_attr_destroy(&attr);
    t_stack_size = size;
    size_t margin = margin_for(size);
    ziman_stack_limit = size > 2 * margin ? (char *)low + margin : (char *)low + size / 2;
}

void ziman_stack_slow(void) {
    if (ziman_stack_limit == NULL) {
        measure_this_thread();
        if (ziman_stack_limit == NULL) return;
        char probe;
        if (&probe >= ziman_stack_limit) return;
    }
    /* Past the limit. Lower it for the error itself (it needs some stack) -- the program is ending, or the error is being recovered from by unwinding. */
    char *saved = ziman_stack_limit;
    ziman_stack_limit = NULL;
    size_t mb = (t_stack_size + 1048575) / 1048576;
    ziman_error("runtime", 0, 0,
                "stack depth exceeded: this %s thread's %zu MB stack is used up (deep or endless recursion?); raise it with %s, or plan for fewer threads with --threads=N / ZIMAN_THREADS",
                t_is_main ? "main" : "spawned", mb,
                t_is_main ? "--stack-mb=N / ZIMAN_STACK_MB" : "--thread-stack-mb=N / ZIMAN_THREAD_STACK_MB");
    ziman_stack_limit = saved; /* only reached when the error was recovered from */
}

/* ---------------------------------------------------------------- sizing */

static long env_long(const char *name) {
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') return 0;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    return (end != NULL && *end == '\0' && n > 0) ? n : 0;
}

static void refuse_size(const char *what, long mb) {
    fprintf(stderr, "ziman: %s %ld MB is out of range (1 to 768 MB; a larger stack makes the garbage collector fail)\n", what, mb);
    exit(2);
}

void ziman_stack_configure(long main_mb, long thread_mb, long threads) {
    g_main_mb = main_mb > 0 ? main_mb : env_long("ZIMAN_STACK_MB");
    g_thread_mb = thread_mb > 0 ? thread_mb : env_long("ZIMAN_THREAD_STACK_MB");
    g_threads = threads > 0 ? threads : env_long("ZIMAN_THREADS");
    if (g_main_mb > 768) refuse_size("the main stack of", g_main_mb);
    if (g_thread_mb > 768) refuse_size("the spawned-thread stack of", g_thread_mb);
    if (g_threads > 1000000) g_threads = 1000000;
}

/* The number after `key` in a /proc/meminfo-style text, in bytes (the file counts kB); -1 when absent. */
static int64_t meminfo_bytes(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) return -1;
    char line[256];
    int64_t found = -1;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, key, n) == 0) {
            found = (int64_t)strtoll(line + n, NULL, 10) * 1024;
            break;
        }
    }
    fclose(f);
    return found;
}

/* A single number from a small file (a cgroup counter), or -1 for a missing file, `max` or an absurdly large "no limit" value. */
static int64_t file_number(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    char buf[64];
    int64_t v = -1;
    if (fgets(buf, sizeof buf, f) != NULL && buf[0] >= '0' && buf[0] <= '9') v = (int64_t)strtoll(buf, NULL, 10);
    fclose(f);
    return v > ((int64_t)1 << 60) ? -1 : v;
}

/* The memory the process may still use: what the machine has available, and what its container (cgroup v2 or v1) allows less what it already uses. */
static int64_t usable_memory(void) {
    int64_t usable = meminfo_bytes("MemAvailable:");
    int64_t limit = file_number("/sys/fs/cgroup/memory.max");
    int64_t used = file_number("/sys/fs/cgroup/memory.current");
    if (limit < 0) {
        limit = file_number("/sys/fs/cgroup/memory/memory.limit_in_bytes");
        used = file_number("/sys/fs/cgroup/memory/memory.usage_in_bytes");
    }
    if (limit > 0) {
        int64_t left = used > 0 && used < limit ? limit - used : limit;
        if (usable < 0 || left < usable) usable = left;
    }
    return usable > 0 ? usable : (int64_t)4 << 30; /* unreadable (not Linux): assume 4 GB */
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

static size_t round_up(size_t v, size_t unit) { return (v + unit - 1) / unit * unit; }

#define MB ((int64_t)1 << 20)

/* The largest stack (main or spawned) that is accepted or computed. The bundled garbage collector aborts ("Too many retries in GC_allocobj") once a thread's stack is
 * near 1 GB -- measured: 768 MB works, 900 MB does not -- so a bigger request is refused rather than crashing later. */
#define MAX_STACK_MB 768

/* An eighth of the usable memory is the budget for thread stacks; the main thread takes a quarter of it (8 to 512 MB) and the rest is shared between the
 * threads the program plans for (1 to 64 MB each). Stacks are reserved address space and only the pages a thread touches are memory, so these are ceilings. */
static void compute(size_t *main_bytes, size_t *spawn_bytes) {
    int64_t budget = usable_memory() / 8;
    int64_t threads = g_threads > 0 ? g_threads : 64;
    int64_t main_size = clamp64(budget / 4, 8 * MB, 512 * MB);
    int64_t spare = budget > main_size ? budget - main_size : 0;
    int64_t spawn_size = clamp64(spare / threads, 1 * MB, 64 * MB);
    *main_bytes = (size_t)main_size;
    *spawn_bytes = (size_t)spawn_size;
}

size_t ziman_main_stack_bytes(void) {
    if (g_main_mb > 0) return round_up((size_t)g_main_mb * 1048576, 65536);
    size_t m, s;
    compute(&m, &s);
    return round_up(m, 65536);
}

static pthread_mutex_t g_refresh_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t g_spawn_cached = 0;
static time_t g_spawn_cached_at = 0;

size_t ziman_spawn_stack_bytes(void) {
    if (g_thread_mb > 0) return round_up((size_t)g_thread_mb * 1048576, 65536);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&g_refresh_lock);
    if (g_spawn_cached == 0 || now.tv_sec != g_spawn_cached_at) { /* at most once a second */
        size_t m, s;
        compute(&m, &s);
        g_spawn_cached = round_up(s, 65536);
        g_spawn_cached_at = now.tv_sec;
    }
    size_t result = g_spawn_cached;
    pthread_mutex_unlock(&g_refresh_lock);
    return result;
}

/* ---------------------------------------------------------------- threads */

void *ziman_run_on_big_stack(void *(*fn)(void *), void *arg) {
    size_t size = ziman_main_stack_bytes();
    for (;;) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_t thread;
        int made = pthread_attr_setstacksize(&attr, size) == 0 && pthread_create(&thread, &attr, fn, arg) == 0;
        pthread_attr_destroy(&attr);
        if (made) {
            void *result = NULL;
            pthread_join(thread, &result);
            return result;
        }
        if (size <= 16 * (size_t)MB) break; /* the system would not give even that much */
        size /= 2;
    }
    return fn(arg); /* run on the calling thread, whatever stack it has (measured like any other) */
}

int ziman_spawn_thread(void *(*fn)(void *), void *arg) {
    size_t size = ziman_spawn_stack_bytes();
    int status = EAGAIN;
    while (size >= 256 * 1024) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        status = pthread_attr_setstacksize(&attr, size);
        if (status == 0) status = pthread_create(&thread, &attr, fn, arg);
        pthread_attr_destroy(&attr);
        if (status == 0) return 0;
        size /= 2; /* memory is tight: try a smaller stack */
    }
    return status;
}
