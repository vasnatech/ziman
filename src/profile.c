#define _POSIX_C_SOURCE 200809L
#include "profile.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "error.h"

/* Only for GC_set_on_collection_event -- a `make`/`make gc` build already force-includes src/zgc.h
 * into every translation unit; this explicit include is for gc.h's own declarations, the same reason
 * arena.c and gc_heap.c carry the identical comment. */
#ifdef ZIMAN_GC
#include <gc.h>

/* Boehm collects from inside whatever allocation crossed its threshold, so no call site in this
 * codebase can bracket a collection the way maybe_collect brackets Ziman's own. This is the hook
 * that can: GC_EVENT_START and GC_EVENT_END arrive around each cycle (and, under PARALLEL_MARK, may
 * arrive on a marker thread rather than the allocating one, which is fine -- the count is global
 * because ITIMER_PROF samples the process, so a sample on any thread during a collection is
 * correctly collection time). Every other event is ignored. */
static void on_collection_event(GC_EventType event) {
    if (event == GC_EVENT_START) ziman_profile_gc_enter();
    else if (event == GC_EVENT_END) ziman_profile_gc_leave();
}
#endif

/* Samples per second. 1000 keeps a one-second run statistically meaningful (a function worth
 * optimising shows up in tens of samples) while leaving the interval far longer than the handler
 * itself takes, so the sampler cannot meaningfully profile itself. */
#define PROFILE_HZ 1000

/* Raw samples, each the `name` pointer of the innermost Ziman frame at that instant. A flat buffer
 * written by the handler and aggregated afterwards, rather than counters the handler maintains
 * itself: aggregating means finding a name's slot, and the only ways to do that in a signal handler
 * are a linear scan of every name seen so far (unbounded work at an unbounded rate) or a hash table
 * it cannot safely grow. One store into a slot claimed with an atomic increment is bounded, lock
 * free and allocation free, which is what async-signal-safety actually demands.
 *
 * At 1000Hz this holds just over eight minutes of CPU time. A longer run keeps its first eight
 * minutes and counts the rest as dropped, which the report states: a truncated profile that says so
 * beats both a wrong one and a sampler that allocates at interrupt time. */
#define SAMPLE_CAPACITY (1 << 19)

/* Allocated by ziman_profile_start, not declared as a static array: at this capacity the array is
 * 4MB, and a static one puts 4MB of BSS into every ziman binary whether or not anyone profiles.
 * That was measured, not assumed -- as a static array it cost 9% of a call-heavy benchmark
 * (fib(30): +1.9% instructions but +10.4% cycles, IPC down, which is the signature of hot globals
 * being pushed into conflicting cache sets rather than of extra work). An unrelated change of the
 * same source size to the same file cost nothing on the same test, which is what ruled out plain
 * code-layout luck. NULL until profiling starts, and the handler is only ever installed after it is
 * non-NULL. */
static const char **g_samples = NULL;
/* `volatile sig_atomic_t` for the two counters the handler writes: they are read back by ordinary
 * code after the timer is stopped, and sig_atomic_t is the one type the C standard promises can be
 * written in a handler and read outside it without tearing. */
static volatile sig_atomic_t g_sample_count = 0;
static volatile sig_atomic_t g_dropped = 0;
static int g_started = 0;
/* The process's own CPU time when sampling started, so the report can compare how much CPU the run
 * actually used against how much the samples account for. They diverge whenever the kernel
 * coalesces SIGPROF -- several threads burning CPU expire the timer again before the pending signal
 * is delivered, and the extra expiries are merged into one. Measured on a four-thread program: 0.90s
 * of CPU against 421 samples, an undercount of better than two to one. The percentages are
 * unharmed, being shares of the samples actually taken, but a total presented as seconds of CPU
 * would be a figure the reader could check and find wrong, so the report states both and says which
 * to trust. */
static double g_cpu_at_start = 0.0;

static double process_cpu_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
/* How many collections are in progress. Read by the handler, so sig_atomic_t; a count rather than a
 * flag because a nested collection must not clear the outer one's bracket on the way out. */
static volatile sig_atomic_t g_in_gc = 0;

/* A sentinel standing for "this sample was taken inside a collection". Its *address* is the tally
 * key, which is why it only has to be a distinct object and never to hold anything -- the same
 * trick the NULL key uses for "not in Ziman code", with a second value needed because these are two
 * different answers. */
static const char g_gc_sentinel = 0;

void ziman_profile_gc_enter(void) { __atomic_fetch_add(&g_in_gc, 1, __ATOMIC_RELAXED); }
void ziman_profile_gc_leave(void) { __atomic_fetch_sub(&g_in_gc, 1, __ATOMIC_RELAXED); }

static void on_sigprof(int sig) {
    (void)sig;
    /* A collection in progress outranks the frame stack: the innermost Ziman frame is whatever
     * allocated, and billing the collector's work to it is the one answer guaranteed to send the
     * reader to the wrong place. NULL when no Ziman function is running -- kept rather than skipped,
     * so the report can say how much of the run was outside interpreted code instead of quietly
     * rescaling. */
    const char *name;
    if (__atomic_load_n(&g_in_gc, __ATOMIC_RELAXED) > 0) name = &g_gc_sentinel;
    else name = ziman_frame_top_name();
    /* Claim a slot atomically: ITIMER_PROF is per process, so in a program using `spawn` the signal
     * can be delivered to any thread and two handlers can be in flight at once on different cores.
     * Relaxed ordering is enough -- each handler writes only the slot it claimed, and nothing reads
     * any of them until the timer is stopped. */
    int idx = __atomic_fetch_add(&g_sample_count, 1, __ATOMIC_RELAXED);
    if (idx >= SAMPLE_CAPACITY) {
        __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    g_samples[idx] = name;
}

void ziman_profile_start(void) {
    g_samples = calloc(SAMPLE_CAPACITY, sizeof(*g_samples));
    if (g_samples == NULL) {
        fprintf(stderr, "ziman: --profile: cannot allocate the sample buffer\n");
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigprof;
    /* SA_RESTART so a sample landing mid-syscall resumes it instead of failing it with EINTR: a
     * profiler that makes `read` fail would change the behaviour of the very program it is
     * measuring. The handler is blocked during its own execution (the default), so it cannot
     * re-enter itself. */
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) {
        fprintf(stderr, "ziman: --profile: cannot install the sampling handler\n");
        return;
    }
    /* ITIMER_PROF, so the clock is this process's own CPU time rather than wall time: a program
     * blocked on a socket is not accumulating samples that would otherwise be blamed on whichever
     * function happened to be waiting. */
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000000 / PROFILE_HZ;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, NULL) != 0) {
        fprintf(stderr, "ziman: --profile: cannot start the sampling timer\n");
        return;
    }
#ifdef ZIMAN_GC
    GC_set_on_collection_event(on_collection_event);
#endif
    g_cpu_at_start = process_cpu_seconds();
    g_started = 1;
}

typedef struct {
    const char *name;
    int count;
} Tally;

static int by_count_desc(const void *a, const void *b) {
    const Tally *x = a, *y = b;
    if (x->count != y->count) return y->count - x->count;
    /* Ties broken by name so a report is reproducible run to run, which is what lets a fixture
     * check one at all. A NULL name (time outside interpreted code) sorts last among its ties. */
    if (x->name == NULL) return 1;
    if (y->name == NULL) return -1;
    return strcmp(x->name, y->name);
}

void ziman_profile_report(void) {
    if (!g_started) return;
    /* Stop the timer before reading anything, so no handler can be appending while we aggregate. */
    struct itimerval off;
    memset(&off, 0, sizeof(off));
    setitimer(ITIMER_PROF, &off, NULL);
    signal(SIGPROF, SIG_IGN);

    int n = (int)g_sample_count;
    if (n > SAMPLE_CAPACITY) n = SAMPLE_CAPACITY;

    /* Tally by name pointer. Names come from the AST and are stable for the life of the process, so
     * identity is enough and no string hashing is needed; a program has a few hundred functions at
     * most, which a linear scan handles in well under the time the report itself takes to print. */
    Tally *tally = NULL;
    int tally_count = 0, tally_cap = 0;
    for (int i = 0; i < n; i++) {
        const char *name = g_samples[i];
        int found = -1;
        for (int t = 0; t < tally_count; t++) {
            if (tally[t].name == name) { found = t; break; }
        }
        if (found < 0) {
            if (tally_count == tally_cap) {
                tally_cap = tally_cap == 0 ? 64 : tally_cap * 2;
                tally = realloc(tally, (size_t)tally_cap * sizeof(Tally));
            }
            found = tally_count++;
            tally[found].name = name;
            tally[found].count = 0;
        }
        tally[found].count++;
    }
    qsort(tally, (size_t)tally_count, sizeof(Tally), by_count_desc);

    double cpu_used = process_cpu_seconds() - g_cpu_at_start;
    double sampled = (double)n / PROFILE_HZ;
    fprintf(stderr, "profile: %d samples at %dHz, over %.2fs of CPU time\n", n, PROFILE_HZ, cpu_used);
    /* Only when the shortfall is big enough to mislead: a few percent is ordinary rounding and
     * startup, while a factor like the two-to-one a threaded program shows is worth a warning. */
    if (cpu_used > 0.05 && sampled < cpu_used * 0.9) {
        fprintf(stderr,
                "  (samples account for %.2fs of it; the kernel merges timer signals when more than\n"
                "   one thread is busy -- the collector's own marker threads included, so this shows\n"
                "   up even in a single-threaded program -- so compare the percentages, not the totals)\n",
                sampled);
    }
    if (g_dropped > 0) {
        fprintf(stderr, "  (%d later samples dropped: the buffer holds %d)\n", (int)g_dropped, SAMPLE_CAPACITY);
    }
    if (n == 0) {
        fprintf(stderr, "  nothing sampled: the program finished in under one sampling interval\n");
        free(tally);
        return;
    }
    fprintf(stderr, "  self%%  samples  function\n");
    for (int t = 0; t < tally_count; t++) {
        double pct = 100.0 * tally[t].count / n;
        /* Everything below a tenth of a percent is noise at any realistic sample count, and a long
         * tail of 1-sample rows buries the answer the report exists to give. */
        if (pct < 0.1) {
            int rest = 0, rest_rows = 0;
            for (int u = t; u < tally_count; u++) { rest += tally[u].count; rest_rows++; }
            fprintf(stderr, "  %5.1f  %7d  (%d more functions, each under 0.1%%)\n",
                    100.0 * rest / n, rest, rest_rows);
            break;
        }
        const char *label;
        if (tally[t].name == &g_gc_sentinel) label = "(garbage collection)";
        else if (tally[t].name == NULL) label = "(not in Ziman code: startup, typecheck)";
        else label = tally[t].name;
        fprintf(stderr, "  %5.1f  %7d  %s\n", pct, tally[t].count, label);
    }
    free(tally);
}
