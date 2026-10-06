#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_error_context = NULL;
static int g_json_errors = 0;
static const char *g_error_file = NULL;
static int g_error_count = 0;
#define MAX_ERRORS 25
#define MAX_RECOVERY_DEPTH 512
static jmp_buf *g_recovery[MAX_RECOVERY_DEPTH];
static int g_recovery_depth = 0;

/* A compile-time error (`ziman_error`, `g_sink == NULL` only -- the LSP's own sink streams each diagnostic as
 * found instead, unaffected by any of this) is collected here rather than printed as it is raised, so the whole
 * batch can be sorted by position before anything reaches the terminal: parsing and type-checking each keep
 * going after an error (see the recovery-point machinery above), one function or top-level declaration at a
 * time, in *declaration* order -- not necessarily *source* order once a `switch` fires more than one check on
 * the same statement (see check_stmt's STMT_SWITCH/STMT_VALUE_BLOCK), or once a cross-module program's functions
 * interleave. `ziman_flush_errors` (called from `ziman_finish_errors`) sorts and prints this buffer once,
 * exactly when the old code would have printed the final "N errors" line -- every individual message is
 * unchanged, only when it appears is. */
typedef struct {
    const char *phase, *file, *context, *message;
    int line, col;
} CollectedError;
static CollectedError g_collected[MAX_ERRORS];
static int g_collected_count = 0;
static void ziman_flush_errors(void);

static ZimanErrorSink g_sink = NULL;
void ziman_set_error_sink(ZimanErrorSink sink) { g_sink = sink; }
void ziman_reset_errors(void) {
    g_error_count = 0;
    g_collected_count = 0;
    g_recovery_depth = 0;
    g_error_file = NULL;
    g_error_context = NULL;
}

void ziman_set_error_file(const char *file) { g_error_file = file; }
int ziman_error_count(void) { return g_error_count; }

/* file/text pairs registered so far, for the excerpt-and-caret ziman_flush_errors prints; see ziman_register_source's own comment in error.h. */
enum { MAX_REGISTERED_SOURCES = 512 };
typedef struct { const char *file, *text; } RegisteredSource;
static RegisteredSource g_sources[MAX_REGISTERED_SOURCES];
static int g_source_count = 0;

void ziman_register_source(const char *file, const char *text) {
    if (g_source_count < MAX_REGISTERED_SOURCES) g_sources[g_source_count++] = (RegisteredSource){file, text};
}

static const char *lookup_source(const char *file) {
    if (file == NULL) return NULL;
    for (int i = 0; i < g_source_count; i++) {
        if (strcmp(g_sources[i].file, file) == 0) return g_sources[i].text;
    }
    return NULL;
}

/* Prints `line` (1-based) of `file`'s registered text, with a caret under `col` (1-based), indented under a
 * gutter the width of the line number -- the same shape rustc/clang use, minus an end column (most call sites
 * only ever had a start position to give; a caret spanning the whole culprit needs each one to also carry
 * where it ends, which is a much larger, separate change). Prints nothing when `file` was never registered
 * (a bundled core module) or `line` is past the end of it (should not happen, but a wrong excerpt is worse
 * than none). */
static void print_excerpt(const char *file, int line, int col) {
    const char *text = lookup_source(file);
    if (text == NULL || line < 1 || col < 1) return;
    const char *p = text;
    int cur = 1;
    while (cur < line && *p != '\0') {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) return;
    const char *line_start = p;
    const char *line_end = strchr(p, '\n');
    if (line_end == NULL) line_end = p + strlen(p);
    char gutter[16];
    int gutter_width = snprintf(gutter, sizeof gutter, "%d", line);
    fprintf(stderr, "%*s |\n", gutter_width, "");
    fprintf(stderr, "%s | %.*s\n", gutter, (int)(line_end - line_start), line_start);
    fprintf(stderr, "%*s | ", gutter_width, "");
    for (int i = 0; i < col - 1 && line_start + i < line_end; i++) {
        fputc(line_start[i] == '\t' ? '\t' : ' ', stderr);
    }
    fputs("^\n", stderr);
}

void ziman_recovery_push(jmp_buf *jb) {
    if (g_recovery_depth < MAX_RECOVERY_DEPTH) g_recovery[g_recovery_depth] = jb;
    g_recovery_depth++;
}

void ziman_recovery_pop(void) {
    if (g_recovery_depth > 0) g_recovery_depth--;
}

/* Only points that fit in the table are real; a deeper push is ignored, so an
 * error there jumps to the deepest real one. */
static void jump_to_recovery(void) {
    int depth = g_recovery_depth > MAX_RECOVERY_DEPTH ? MAX_RECOVERY_DEPTH : g_recovery_depth;
    g_recovery_depth = depth - 1;
    longjmp(*g_recovery[depth - 1], 1);
}

void ziman_abort_quietly(void) {
    if (g_recovery_depth > 0) jump_to_recovery();
    exit(1);
}

static void jump_to_outermost(void) {
    if (g_recovery_depth > MAX_RECOVERY_DEPTH) g_recovery_depth = MAX_RECOVERY_DEPTH;
    g_recovery_depth = 0;
    longjmp(*g_recovery[0], 1);
}

void ziman_finish_errors(void) {
    if (g_error_count == 0) return;
    if (g_sink != NULL && g_recovery_depth > 0) jump_to_outermost();
    ziman_flush_errors();
    if (g_error_count > 1 && !g_json_errors) fprintf(stderr, "ziman: %d errors\n", g_error_count);
    exit(1);
}

void ziman_set_error_context(const char *context) {
    g_error_context = context;
}

const char *ziman_error_context(void) {
    return g_error_context;
}

void ziman_set_json_errors(int on) {
    g_json_errors = on;
}

/* Writes `s` as a JSON string literal (quotes included). Bytes >= 0x80 pass
 * through, so UTF-8 in a message stays UTF-8. */
static void print_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20) fprintf(out, "\\u%04x", *p);
                else fputc(*p, out);
        }
    }
    fputc('"', out);
}

static __thread ThreadBoundary *t_boundary = NULL;

/* `env`: the interpreter's own Env* for this call's top-level scope, kept as `void *` since error.c
 * has no reason to know about env.h -- set separately (ziman_set_frame_env), not by
 * ziman_push_frame itself, since call_function pushes its frame before its own Env exists (see
 * interpreter.c). NULL for a native call's frame (natives have no Env at all) or a frame nothing
 * has set it for yet. Read back by a real collection cycle's root walk (docs/MEMORY.md, "the second
 * root source") via ziman_frame_count/ziman_frame_env_at -- a conservative stack scan can never see
 * into an Env's own bindings on its own, since Env lives on the ordinary allocator's heap, not a
 * thread's GcHeap arena. */
typedef struct { const char *name, *file; int call_line, call_col; void *env; } ZimanFrame;

/* Reachable only through the `__thread` pointer, same as t_defer_envs/t_env_pool in
 * interpreter.c: the collector does not reliably scan thread-local storage, so the
 * buffer is allocated uncollectable rather than through the ordinary GC_MALLOC a
 * ZIMAN_GC build's malloc/calloc already redirect to. */
static __thread ZimanFrame *t_frames = NULL;
static __thread int t_frame_count = 0;
static __thread int t_frame_capacity = 0;

static void frames_grow(int capacity) {
#ifdef ZIMAN_GC
    ZimanFrame *fresh = GC_MALLOC_UNCOLLECTABLE(sizeof(ZimanFrame) * (size_t)capacity);
    if (t_frames != NULL) {
        memcpy(fresh, t_frames, sizeof(ZimanFrame) * (size_t)t_frame_count);
        GC_FREE(t_frames);
    }
    memset(fresh + t_frame_count, 0, sizeof(ZimanFrame) * (size_t)(capacity - t_frame_count));
#else
    ZimanFrame *fresh = realloc(t_frames, sizeof(ZimanFrame) * (size_t)capacity);
    /* Every slot not yet in use is zeroed, which is what lets ziman_push_frame stay a single
     * unordered store and still be safe to sample from a signal handler -- see there. realloc does
     * not zero what it adds, and GC_MALLOC_UNCOLLECTABLE's own zeroing does not cover the copy
     * above, so both paths do it explicitly. */
    memset(fresh + t_frame_count, 0, sizeof(ZimanFrame) * (size_t)(capacity - t_frame_count));
#endif
    t_frames = fresh;
    t_frame_capacity = capacity;
}

void ziman_push_frame(const char *name, const char *file, int call_line, int call_col) {
    if (t_frame_count == t_frame_capacity) frames_grow(t_frame_capacity == 0 ? 64 : t_frame_capacity * 2);
    /* Deliberately left as one unordered store-and-increment, with no barrier. --profile's SIGPROF
     * handler reads the top frame on this very thread and so can land in the middle of this line,
     * which looks like it needs the fields published before the count. Ordering it that way was
     * tried and cost 9.5% on call-heavy code (12 paired rounds, fib(30), CI +7.6%..+11.3%): a
     * signal fence is a full compiler barrier, and putting one here stops the compiler keeping this
     * thread-local in a register across the call path that every Ziman call goes through. Paying
     * that on every program to serve the rare profiled one is the wrong trade.
     *
     * What makes the barrier unnecessary is frames_grow zeroing every slot it has not handed out
     * yet. The handler therefore reads one of exactly three things, never a torn pointer: the name
     * just stored, NULL (reported as time outside Ziman code), or the name a previous call left at
     * this depth, which is a valid string and at worst attributes one sample of a thousand per
     * second to a function that really did run there moments ago. Safety comes from the array's own
     * invariant rather than from ordering, which is why the hot path can stay as it was. */
    t_frames[t_frame_count++] = (ZimanFrame){name, file, call_line, call_col, NULL};
}

/* The innermost Ziman function running on the calling thread, or NULL when none is (startup, or a
 * thread that has not entered interpreted code). Safe to call from a signal handler: it reads two
 * thread-locals and returns a `const char *` the parser owns for the life of the process, taking no
 * lock and allocating nothing. The count is read once into a local so the bounds check and the
 * index cannot disagree if a push or pop runs in between, and the fence is on this side only --
 * free here, where it runs a thousand times a second, and not on the push path, where it was
 * measured at 9.5%. See ziman_push_frame for why a slot can never hold a torn pointer. */
const char *ziman_frame_top_name(void) {
    int n = t_frame_count;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    if (n <= 0) return NULL;
    return t_frames[n - 1].name;
}

void ziman_pop_frame(void) {
    if (t_frame_count > 0) t_frame_count--;
}

void ziman_set_frame_env(void *env) {
    if (t_frame_count > 0) t_frames[t_frame_count - 1].env = env;
}

int ziman_frame_count(void) {
    return t_frame_count;
}

void *ziman_frame_env_at(int i) {
    return t_frames[i].env;
}

void ziman_call_stack_release(void) {
#ifdef ZIMAN_GC
    if (t_frames != NULL) GC_FREE(t_frames);
#else
    free(t_frames);
#endif
    t_frames = NULL;
    t_frame_count = 0;
    t_frame_capacity = 0;
}

ZimanFramesRootAddrs ziman_frames_root_addrs(void) {
    ZimanFramesRootAddrs addrs;
    addrs.frames_ptr_addr = (void **)&t_frames;
    addrs.frame_count_addr = &t_frame_count;
    return addrs;
}

int ziman_frames_root_ranges(ZimanFramesRootAddrs addrs, ZimanMemRange *out_ranges, int max_ranges) {
    (void)max_ranges; /* always at most 1 -- see the header's own comment */
    ZimanFrame *frames = *(ZimanFrame **)addrs.frames_ptr_addr;
    int count = *addrs.frame_count_addr;
    if (frames == NULL || count == 0) return 0;
    out_ranges[0].start = frames;
    out_ranges[0].end = frames + count;
    return 1;
}

/* The current thread's call chain, innermost first, one frame per line joined by "\n"
 * -- with neither a leading nor a trailing newline, so a caller decides how it joins
 * onto whatever came before and after -- empty ("") when there are no frames (a fatal
 * error before any Ziman function ran, such as the stack-depth guard tripping before
 * main). Malloc'd; the caller frees it. */
static char *build_trace(void) {
    size_t room = 1;
    char *trace = malloc(room);
    trace[0] = '\0';
    for (int i = t_frame_count - 1; i >= 0; i--) {
        ZimanFrame *f = &t_frames[i];
        char line[512];
        const char *sep = i == t_frame_count - 1 ? "" : "\n";
        if (f->file != NULL) {
            snprintf(line, sizeof line, "%s    at %s (%s:%d:%d)", sep, f->name, f->file, f->call_line, f->call_col);
        } else {
            snprintf(line, sizeof line, "%s    at %s (%d:%d)", sep, f->name, f->call_line, f->call_col);
        }
        size_t add = strlen(line);
        char *grown = malloc(room + add);
        memcpy(grown, trace, room - 1);
        memcpy(grown + room - 1, line, add + 1);
        free(trace);
        trace = grown;
        room += add;
    }
    return trace;
}

char *ziman_trace_string(void) { return build_trace(); }

void ziman_thread_boundary_enter(ThreadBoundary *b) {
    b->message = NULL;
    t_boundary = b;
    t_frame_count = 0;
}

void ziman_thread_boundary_leave(void) { t_boundary = NULL; }

void ziman_error(const char *phase, int line, int col, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *message = malloc((size_t)(n > 0 ? n : 0) + 1);
    vsnprintf(message, (size_t)(n > 0 ? n : 0) + 1, fmt, args);
    va_end(args);

    if (t_boundary != NULL && g_sink == NULL && strcmp(phase, "runtime") == 0) {
        ThreadBoundary *b = t_boundary;
        t_boundary = NULL;
        int has_file = g_error_file != NULL;
        size_t room = strlen(message) + (has_file ? strlen(g_error_file) : 0) + 96;
        char *text = malloc(room);
        snprintf(text, room, "runtime error at %d:%d%s%s: %s", line, col, has_file ? " in " : "", has_file ? g_error_file : "", message);
        free(message);
        b->message = text;
        longjmp(b->jb, 1);
    }
    if (g_sink != NULL) {
        g_sink(phase, g_error_file, line, col, g_error_context, message);
        g_error_count++;
        if (g_recovery_depth > 0) {
            if (g_error_count >= MAX_ERRORS) jump_to_outermost();
            jump_to_recovery();
        }
        exit(1);
    }
    /* A runtime error is a one-shot event during interpretation, never followed by a recovery-and-continue
     * cycle or a later ziman_finish_errors() call (there is only ever one, and nothing sorts an array of one) --
     * printed immediately, exactly as every phase used to be. Only "syntax" and "type" errors, which really do
     * accumulate across an unwind-and-keep-checking loop, are collected for ziman_flush_errors to sort. */
    if (strcmp(phase, "runtime") == 0) {
        int show_file = g_error_file != NULL;
        if (g_json_errors) {
            fprintf(stderr, "{\"severity\":\"error\",\"phase\":");
            print_json_string(stderr, phase);
            fputs(",\"file\":", stderr);
            if (g_error_file != NULL) print_json_string(stderr, g_error_file);
            else fputs("null", stderr);
            fprintf(stderr, ",\"line\":%d,\"col\":%d,\"context\":", line, col);
            if (g_error_context != NULL) print_json_string(stderr, g_error_context);
            else fputs("null", stderr);
            fputs(",\"message\":", stderr);
            print_json_string(stderr, message);
            fputs("}\n", stderr);
        } else {
            fprintf(stderr, "ziman: %s error at %d:%d", phase, line, col);
            if (show_file) fprintf(stderr, " in %s", g_error_file);
            if (g_error_context != NULL) fprintf(stderr, " (in %s)", g_error_context);
            fprintf(stderr, ": %s\n", message);
            print_excerpt(g_error_file, line, col);
            char *trace = build_trace();
            if (trace[0] != '\0') fprintf(stderr, "%s\n", trace);
            free(trace);
        }
        exit(1);
    }
    if (g_collected_count < MAX_ERRORS) {
        g_collected[g_collected_count++] = (CollectedError){phase, g_error_file, g_error_context, message, line, col};
    } else {
        free(message);
    }
    g_error_count++;
    /* The common case -- there is a recovery point to unwind to and the cap isn't hit -- longjmps away right
     * here, exactly as before, to keep checking the rest of the program; nothing below this ever runs for it.
     * The buffered errors from that whole run are only sorted and printed once, when the top-level loop that
     * kept jumping back in finally finishes normally and calls ziman_finish_errors(). What follows is reached
     * only when there is nowhere left to unwind to, or the cap was hit mid-loop -- both genuine "stop now"
     * cases, same as the original code's own inline exit here. */
    if (g_recovery_depth > 0 && g_error_count < MAX_ERRORS) jump_to_recovery();
    ziman_flush_errors();
    if (g_error_count >= MAX_ERRORS && g_recovery_depth > 0) {
        if (!g_json_errors) fprintf(stderr, "ziman: too many errors (%d); stopping\n", g_error_count);
    } else if (g_error_count > 1 && !g_json_errors) {
        fprintf(stderr, "ziman: %d errors\n", g_error_count);
    }
    exit(1);
}

static int compare_collected(const void *ap, const void *bp) {
    const CollectedError *a = ap, *b = bp;
    /* No file (a builtin/prelude position with nothing to name) sorts first; otherwise alphabetically, then by
     * position within that file -- so every error in one file prints together, in the order a reader would hit
     * them scanning down it. */
    int file_cmp = (a->file == NULL) - (b->file == NULL);
    if (file_cmp != 0) return b->file == NULL ? 1 : -1; /* NULL first: if a has no file it sorts before b */
    if (a->file != NULL && b->file != NULL) {
        int c = strcmp(a->file, b->file);
        if (c != 0) return c;
    }
    if (a->line != b->line) return a->line - b->line;
    return a->col - b->col;
}

/* Prints every collected "syntax"/"type" error, sorted by (file, line, col). Called from the two places that
 * used to print an error the moment it was raised: here, when ziman_error itself is about to exit (nowhere left
 * to unwind to, or the cap was hit); and from ziman_finish_errors, once a checking loop that kept jumping back
 * in finishes normally. Idempotent (a no-op once already emptied), and a no-op in --lsp mode, which never
 * populates the buffer (see ziman_error's g_sink branch, returned from before reaching it). A "runtime" error
 * printed itself immediately and never reaches here. */
static void ziman_flush_errors(void) {
    if (g_collected_count == 0) return;
    qsort(g_collected, (size_t)g_collected_count, sizeof g_collected[0], compare_collected);
    for (int i = 0; i < g_collected_count; i++) {
        CollectedError *e = &g_collected[i];
        if (g_json_errors) {
            fprintf(stderr, "{\"severity\":\"error\",\"phase\":");
            print_json_string(stderr, e->phase);
            fputs(",\"file\":", stderr);
            if (e->file != NULL) print_json_string(stderr, e->file);
            else fputs("null", stderr);
            fprintf(stderr, ",\"line\":%d,\"col\":%d,\"context\":", e->line, e->col);
            if (e->context != NULL) print_json_string(stderr, e->context);
            else fputs("null", stderr);
            fputs(",\"message\":", stderr);
            print_json_string(stderr, e->message);
            fputs("}\n", stderr);
        } else {
            fprintf(stderr, "ziman: %s error at %d:%d", e->phase, e->line, e->col);
            if (e->file != NULL) fprintf(stderr, " in %s", e->file);
            if (e->context != NULL) fprintf(stderr, " (in %s)", e->context);
            fprintf(stderr, ": %s\n", e->message);
            print_excerpt(e->file, e->line, e->col);
        }
    }
    g_collected_count = 0;
}

void ziman_warning(const char *file, int line, int col, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *message = malloc((size_t)(n > 0 ? n : 0) + 1);
    vsnprintf(message, (size_t)(n > 0 ? n : 0) + 1, fmt, args);
    va_end(args);

    if (g_json_errors) {
        fputs("{\"severity\":\"warning\",\"phase\":\"lint\",\"file\":", stderr);
        print_json_string(stderr, file);
        fprintf(stderr, ",\"line\":%d,\"col\":%d,\"context\":null,\"message\":", line, col);
        print_json_string(stderr, message);
        fputs("}\n", stderr);
    } else {
        fprintf(stderr, "%s:%d:%d: warning: %s\n", file, line, col, message);
    }
    free(message);
}
