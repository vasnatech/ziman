#define _POSIX_C_SOURCE 200809L
#include "native_metrics.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"

/* The `metrics` core module's natives: counters, gauges and histograms that several threads share,
 * and a render of them in the Prometheus text format. See lib/core/metrics.zm for the Ziman side,
 * which is where labels become the canonical string this file stores them under.
 *
 * Why this lives in C at all: a Ziman lambda cannot mutate what it captures, so state shared by
 * the threads of a server has to sit behind a handle, the way StringBuilder and Iter do.
 *
 * Three decisions worth knowing before changing anything here.
 *
 * One: recording is infallible. `add`, `set` and `observe` return nothing and cannot fail. A
 * counter increment that could fail would mean a request failing because it was being measured,
 * which is a worse outcome than a missing number. Past the per-metric series cap the write is
 * dropped and counted, and the count is itself rendered as `ziman_metrics_dropped_series_total`,
 * so a cap that is being hit is visible in the output rather than silent.
 *
 * Two: the lock is per metric, not one for the registry. The problem this module exists to
 * diagnose (docs/reviews/tasks.md F7) is that throughput does not scale with concurrent clients,
 * and a single mutex taken on every request would add to exactly that. Metrics are declared at
 * start-up and never removed, so a declared metric's name, kind and buckets never change again:
 * a lookup reads the published count with an acquire load and walks immutable entries without
 * locking, and only the series list inside one metric is guarded. Two threads recording different
 * metrics never meet.
 *
 * Three: the series cap is per metric and fixed. A label carrying something unbounded -- a user
 * id, a raw path -- would otherwise grow the registry until the process died, and that is the
 * usual way a metrics integration takes a service down. METRICS_SERIES_MAX is the documented
 * ceiling; `dispatch`'s matched pattern, not the request's path, is the label to use. */

#define METRICS_MAX 64         /* distinct metric names */
#define METRICS_SERIES_MAX 128 /* distinct label sets per metric -- the documented cap;
                               * must stay under the 1024 a handle packs it in (see native_series) */
#define METRICS_BUCKETS_MAX 16

typedef enum { M_COUNTER, M_GAUGE, M_HISTOGRAM } MetricKind;

typedef struct {
    char *labels; /* the canonical `k="v",k2="v2"` from the Ziman side; "" when there are none */
    double value; /* a counter's total, a gauge's current value, a histogram's sum */
    unsigned long long count;
    unsigned long long bucket[METRICS_BUCKETS_MAX]; /* cumulative, histogram only */
} Series;

typedef struct {
    char *name;
    char *help;
    MetricKind kind;
    double bounds[METRICS_BUCKETS_MAX];
    int nbuckets;
    Series series[METRICS_SERIES_MAX];
    int nseries;
    unsigned long long dropped;
    pthread_mutex_t mu;
} Metric;

static Metric g_metrics[METRICS_MAX];
static int g_count; /* published with a release store; read with an acquire load */
static pthread_mutex_t g_declare_mu = PTHREAD_MUTEX_INITIALIZER;

/* Only ever called with g_declare_mu held, or from a lookup that may miss a concurrent declare --
 * which is harmless: the caller then declares it, and a second declare of the same name is a no-op. */
static Metric *find(const char *name) {
    int n = __atomic_load_n(&g_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        if (strcmp(g_metrics[i].name, name) == 0) return &g_metrics[i];
    }
    return NULL;
}

/* A double has no native atomic add, so one is built from a compare-exchange on its bits. This
 * stays in user space, where taking the metric's mutex per increment did not: a mutex on a counter
 * that every request touches made throughput *fall* as threads were added -- measured at 3.1M
 * increments/s on one thread against 2.1M on eight -- which is the very shape of problem this
 * module exists to diagnose (F7). Contending on one cache line still does not scale linearly, but
 * it no longer goes backwards. */
static void atomic_add_double(double *p, double delta) {
    unsigned long long seen, next;
    double seenv, nextv;
    memcpy(&seen, p, sizeof seen);
    do {
        memcpy(&seenv, &seen, sizeof seenv);
        nextv = seenv + delta;
        memcpy(&next, &nextv, sizeof next);
    } while (!__atomic_compare_exchange_n((unsigned long long *)p, &seen, next, 0,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

static void atomic_store_double(double *p, double v) {
    unsigned long long bits;
    memcpy(&bits, &v, sizeof bits);
    __atomic_store_n((unsigned long long *)p, bits, __ATOMIC_RELAXED);
}

static double atomic_load_double(const double *p) {
    unsigned long long bits = __atomic_load_n((const unsigned long long *)p, __ATOMIC_RELAXED);
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

/* Finds an existing series without taking anything: a published series' `labels` never changes and
 * the array never shrinks, so walking it up to the acquire-loaded count is safe while another
 * thread is appending. Returns NULL when it is not there yet, and the caller then takes the lock. */
static Series *series_find(Metric *m, const char *labels) {
    int n = __atomic_load_n(&m->nseries, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        if (strcmp(m->series[i].labels, labels) == 0) return &m->series[i];
    }
    return NULL;
}

/* Called with m->mu held: creates the series, or finds the one another thread just created. */
static Series *series_for(Metric *m, const char *labels) {
    for (int i = 0; i < m->nseries; i++) {
        if (strcmp(m->series[i].labels, labels) == 0) return &m->series[i];
    }
    if (m->nseries >= METRICS_SERIES_MAX) {
        m->dropped++;
        return NULL;
    }
    Series *s = &m->series[m->nseries];
    memset(s, 0, sizeof *s);
    s->labels = strdup(labels);
    if (!s->labels) {
        m->dropped++;
        return NULL;
    }
    /* Released last, so a reader that sees the higher count sees a series with its labels set. */
    __atomic_store_n(&m->nseries, m->nseries + 1, __ATOMIC_RELEASE);
    return s;
}

/* ---- declare: start-up only, so this is the one operation that locks the registry. */

static Value native_declare(Value *args, int argc, int line, int col) {
    (void)argc;
    const char *name = value_cstr(args[0]);
    const char *kind = value_cstr(args[1]);
    const char *help = value_cstr(args[2]);
    const char *bounds = value_cstr(args[3]); /* comma-separated, histograms only */

    MetricKind k;
    if (strcmp(kind, "counter") == 0) k = M_COUNTER;
    else if (strcmp(kind, "gauge") == 0) k = M_GAUGE;
    else if (strcmp(kind, "histogram") == 0) k = M_HISTOGRAM;
    else ziman_error("runtime", line, col, "metrics: unknown metric kind '%s'", kind);

    pthread_mutex_lock(&g_declare_mu);
    Metric *existing = find(name);
    if (existing) {
        /* Declaring the same metric twice is how a module can declare its own without caring
         * whether the application got there first; a declaration that disagrees is a mistake. */
        int same = existing->kind == k;
        pthread_mutex_unlock(&g_declare_mu);
        if (!same) {
            ziman_error("runtime", line, col, "metrics: '%s' is already declared as a different kind", name);
        }
        return value_bool(1);
    }
    if (g_count >= METRICS_MAX) {
        pthread_mutex_unlock(&g_declare_mu);
        return value_bool(0);
    }
    Metric *m = &g_metrics[g_count];
    memset(m, 0, sizeof *m);
    m->name = strdup(name);
    m->help = strdup(help);
    m->kind = k;
    pthread_mutex_init(&m->mu, NULL);
    if (k == M_HISTOGRAM && bounds[0]) {
        const char *p = bounds;
        while (*p && m->nbuckets < METRICS_BUCKETS_MAX) {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p) break;
            m->bounds[m->nbuckets++] = v;
            p = (*end == ',') ? end + 1 : end;
        }
    }
    if (!m->name || !m->help) {
        pthread_mutex_unlock(&g_declare_mu);
        return value_bool(0);
    }
    /* Published last, so a reader that sees the new count sees a fully built metric. */
    __atomic_store_n(&g_count, g_count + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_declare_mu);
    return value_bool(1);
}

/* ---- recording: never fails, never reports. */

static Value record(Value *args, MetricKind want, int op) {
    const char *name = value_cstr(args[0]);
    const char *labels = value_cstr(args[1]);
    double v = args[2].as.f;
    Metric *m = find(name);
    if (!m || m->kind != want) return value_unit();
    Series *s = series_find(m, labels);
    if (!s) {
        /* First write to this label set: the only part that needs the lock. */
        pthread_mutex_lock(&m->mu);
        s = series_for(m, labels);
        pthread_mutex_unlock(&m->mu);
        if (!s) return value_unit(); /* at the cap; dropped and counted */
    }
    if (op == 0) {
        atomic_add_double(&s->value, v);
    } else if (op == 1) {
        atomic_store_double(&s->value, v);
    } else {
        atomic_add_double(&s->value, v);
        __atomic_fetch_add(&s->count, 1ULL, __ATOMIC_RELAXED);
        for (int i = 0; i < m->nbuckets; i++) {
            if (v <= m->bounds[i]) __atomic_fetch_add(&s->bucket[i], 1ULL, __ATOMIC_RELAXED);
        }
    }
    return value_unit();
}

/* ---- handles: one series, resolved once, recorded to without touching a string again.
 *
 * Building a label key costs far more than the update it leads to: sorting the names, escaping the
 * values and joining them came to about 4.5us per increment in Ziman, against 0.25us for the
 * update itself -- so a server labelling by route would spend nearly all its metrics time
 * rebuilding the same key per request. A handle is resolved once, at start-up or on the first
 * request for a route, and is a plain index afterwards. This is what a Prometheus client library
 * means by a labelled child.
 *
 * The handle packs the metric and series indices into one int. Neither array ever shrinks and a
 * published entry never moves, so a handle stays valid for the life of the process -- except
 * across `reset`, which is why reset is documented as being for tests. */

static Value native_series(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    const char *name = value_cstr(args[0]);
    const char *labels = value_cstr(args[1]);
    int n = __atomic_load_n(&g_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        if (strcmp(g_metrics[i].name, name) != 0) continue;
        Metric *m = &g_metrics[i];
        Series *s = series_find(m, labels);
        if (!s) {
            pthread_mutex_lock(&m->mu);
            s = series_for(m, labels);
            pthread_mutex_unlock(&m->mu);
        }
        if (!s) return value_int(-1); /* at the cap */
        return value_int((long long)i * 1024 + (long long)(s - m->series));
    }
    return value_int(-1); /* never declared */
}

static Value record_at(Value *args, int op) {
    long long h = args[0].as.i;
    if (h < 0) return value_unit();
    int mi = (int)(h / 1024);
    int si = (int)(h % 1024);
    if (mi >= __atomic_load_n(&g_count, __ATOMIC_ACQUIRE)) return value_unit();
    Metric *m = &g_metrics[mi];
    if (si >= __atomic_load_n(&m->nseries, __ATOMIC_ACQUIRE)) return value_unit();
    Series *s = &m->series[si];
    double v = args[1].as.f;
    if (op == 0) {
        atomic_add_double(&s->value, v);
    } else if (op == 1) {
        atomic_store_double(&s->value, v);
    } else {
        atomic_add_double(&s->value, v);
        __atomic_fetch_add(&s->count, 1ULL, __ATOMIC_RELAXED);
        for (int i = 0; i < m->nbuckets; i++) {
            if (v <= m->bounds[i]) __atomic_fetch_add(&s->bucket[i], 1ULL, __ATOMIC_RELAXED);
        }
    }
    return value_unit();
}

static Value native_add_at(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return record_at(args, 0);
}

static Value native_set_at(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return record_at(args, 1);
}

static Value native_observe_at(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return record_at(args, 2);
}

static Value native_add(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return record(args, M_COUNTER, 0);
}

static Value native_set(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return record(args, M_GAUGE, 1);
}

/* A gauge moved by a delta rather than set outright: what an open-connections count needs, since
 * two threads opening at once must not read-modify-write over each other. `add` refuses a gauge
 * on purpose -- a counter and a gauge are different promises -- so this is its own entry. */
static Value native_gauge_add(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return record(args, M_GAUGE, 3);
}

static Value native_gauge_add_at(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return record_at(args, 3);
}

static Value native_observe(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return record(args, M_HISTOGRAM, 2);
}

/* ---- render: the Prometheus text format, built into a growing buffer. */

typedef struct {
    char *data;
    size_t len, cap;
} Buf;

static void buf_appendf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t need = b->len + (size_t)n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < need) cap *= 2;
        char *grown = realloc(b->data, cap);
        if (!grown) return;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, tmp, (size_t)n);
    b->len += (size_t)n;
    b->data[b->len] = '\0';
}

/* A whole number is written without a fraction, and anything else in the shortest form that reads
 * back as the same double -- what Prometheus's own client libraries emit, because %.17g is always
 * exact but turns a sum of 9.4121 into 9.4121000000000006, which is noise a human has to read past. */
static void append_value(Buf *b, double v) {
    if (isinf(v)) { buf_appendf(b, "%s", v > 0 ? "+Inf" : "-Inf"); return; }
    if (isnan(v)) { buf_appendf(b, "NaN"); return; }
    if (v == (double)(long long)v && v < 1e15 && v > -1e15) {
        buf_appendf(b, "%lld", (long long)v);
        return;
    }
    char tmp[64];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*g", prec, v);
        if (strtod(tmp, NULL) == v) break;
    }
    buf_appendf(b, "%s", tmp);
}

static void append_series_name(Buf *b, const char *name, const char *suffix, const char *labels, const char *extra) {
    buf_appendf(b, "%s%s", name, suffix);
    if (labels[0] || extra) {
        buf_appendf(b, "{");
        if (labels[0]) buf_appendf(b, "%s", labels);
        if (extra) buf_appendf(b, "%s%s", labels[0] ? "," : "", extra);
        buf_appendf(b, "}");
    }
    buf_appendf(b, " ");
}

static Value native_render(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    Buf b = {0};
    unsigned long long dropped_total = 0;
    int n = __atomic_load_n(&g_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        Metric *m = &g_metrics[i];
        pthread_mutex_lock(&m->mu);
        if (m->help[0]) buf_appendf(&b, "# HELP %s %s\n", m->name, m->help);
        buf_appendf(&b, "# TYPE %s %s\n", m->name,
                    m->kind == M_COUNTER ? "counter" : m->kind == M_GAUGE ? "gauge" : "histogram");
        int nseries = __atomic_load_n(&m->nseries, __ATOMIC_ACQUIRE);
        for (int j = 0; j < nseries; j++) {
            Series *s = &m->series[j];
            if (m->kind == M_HISTOGRAM) {
                unsigned long long cumulative = 0;
                for (int k = 0; k < m->nbuckets; k++) {
                    cumulative = __atomic_load_n(&s->bucket[k], __ATOMIC_RELAXED);
                    char le[64];
                    snprintf(le, sizeof le, "le=\"%g\"", m->bounds[k]);
                    append_series_name(&b, m->name, "_bucket", s->labels, le);
                    buf_appendf(&b, "%llu\n", cumulative);
                }
                append_series_name(&b, m->name, "_bucket", s->labels, "le=\"+Inf\"");
                buf_appendf(&b, "%llu\n", __atomic_load_n(&s->count, __ATOMIC_RELAXED));
                append_series_name(&b, m->name, "_sum", s->labels, NULL);
                append_value(&b, atomic_load_double(&s->value));
                buf_appendf(&b, "\n");
                append_series_name(&b, m->name, "_count", s->labels, NULL);
                buf_appendf(&b, "%llu\n", __atomic_load_n(&s->count, __ATOMIC_RELAXED));
            } else {
                append_series_name(&b, m->name, "", s->labels, NULL);
                append_value(&b, atomic_load_double(&s->value));
                buf_appendf(&b, "\n");
            }
        }
        dropped_total += m->dropped;
        pthread_mutex_unlock(&m->mu);
    }
    /* Always rendered, even at zero: a reader should be able to tell "no label set was ever
     * dropped" from "this build has no such counter". */
    buf_appendf(&b, "# HELP ziman_metrics_dropped_series_total Writes dropped because a metric was at its series cap.\n");
    buf_appendf(&b, "# TYPE ziman_metrics_dropped_series_total counter\n");
    buf_appendf(&b, "ziman_metrics_dropped_series_total %llu\n", dropped_total);
    if (!b.data) return value_string(strdup(""));
    return value_string(b.data); /* the buffer is handed over, not copied */
}

/* ---- reset: for a test that wants an empty registry; not for production use. */

static Value native_reset(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    pthread_mutex_lock(&g_declare_mu);
    int n = __atomic_load_n(&g_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        Metric *m = &g_metrics[i];
        pthread_mutex_lock(&m->mu);
        for (int j = 0; j < m->nseries; j++) free(m->series[j].labels);
        m->nseries = 0;
        m->dropped = 0;
        pthread_mutex_unlock(&m->mu);
        pthread_mutex_destroy(&m->mu);
        free(m->name);
        free(m->help);
    }
    __atomic_store_n(&g_count, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_declare_mu);
    return value_unit();
}

void native_metrics_register(void) {
    native_register("metrics::declare_raw", native_declare);
    native_register("metrics::add_raw", native_add);
    native_register("metrics::set_raw", native_set);
    native_register("metrics::gauge_add_raw", native_gauge_add);
    native_register("metrics::gauge_add_at_raw", native_gauge_add_at);
    native_register("metrics::observe_raw", native_observe);
    native_register("metrics::series_raw", native_series);
    native_register("metrics::add_at_raw", native_add_at);
    native_register("metrics::set_at_raw", native_set_at);
    native_register("metrics::observe_at_raw", native_observe_at);
    native_register("metrics::render_raw", native_render);
    native_register("metrics::reset_raw", native_reset);
}
