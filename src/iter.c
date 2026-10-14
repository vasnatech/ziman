#define _POSIX_C_SOURCE 200809L
#include "iter.h"

#ifdef ZIMAN_GC
#include <gc.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "gc_mark.h"
#include "handles.h"
#include "native.h"
#include "xalloc.h"

static HandleSet g_iters = HANDLE_SET_INIT;
static char *ITER_FIELD_NAMES[] = {"handle"};

#ifdef ZIMAN_GC
static void iter_finalizer(void *obj, void *client_data) {
    (void)client_data;
    IterObj *it = obj;
    if (handleset_remove(&g_iters, it)) it->release(it);
}

/* For an iterator whose `release` runs Ziman code (IterObj.no_finalizer): take it out of the live
 * set and release nothing. The set is a validation table, not a root -- see src/handles.c's own
 * comment -- so an abandoned iterator that never gets here leaves its pointer in it for the life of
 * the process, and the pointer is dangling the moment the collector takes the object. This is the
 * part of a finalizer that is safe to run for them: no interpreted code, just the removal, which is
 * the same handleset_remove the two existing finalizers already do from this context. What it
 * deliberately does not do is call `release` -- see iter.h for why (docs/reviews/tasks.md F16). */
static void iter_drop_finalizer(void *obj, void *client_data) {
    (void)client_data;
    handleset_remove(&g_iters, obj);
}
#endif

/* Every walk or close of an Iter checks the calling thread: a cursor moved from a thread to another (through a closure, a global) would be walked by
 * two at once. */
static void iter_owner_check(const IterObj *it) {
    if (!pthread_equal(it->owner, pthread_self())) {
        ziman_error("runtime", 0, 0,
                    "an Iter belongs to the thread that made it and was used from another thread (an iterator cannot be shared between threads; hand over the items instead)");
    }
}

Value iter_make(IterObj *it) {
    it->owner = pthread_self();
    handleset_add(&g_iters, it);
#ifdef ZIMAN_GC
    /* Two finalizers, not one and a gap; see IterObj.no_finalizer in iter.h for why an iterator that
     * would run Ziman code to close itself gets the removal-only one. */
    GC_REGISTER_FINALIZER_NO_ORDER(it, it->no_finalizer ? iter_drop_finalizer : iter_finalizer, NULL,
                                   NULL, NULL);
#endif
    Value *fields = ziman_xmalloc(sizeof(Value));
    fields[0] = value_int((int64_t)(intptr_t)it);
    return value_struct(type_struct("Iter"), struct_obj_new(ITER_FIELD_NAMES, fields, 1));
}

int64_t iter_handle_of(Value iter) { return iter.as.st->fields[0].as.i; }

int iter_advance(int64_t handle, Value *out, int line, int col) {
    IterObj *it = (IterObj *)(intptr_t)handle;
    if (!handleset_has(&g_iters, it)) return 0; /* closed early: nothing more to give */
    iter_owner_check(it);
    it->line = line;
    it->col = col;
    if (it->next(it, out)) return 1;
    if (handleset_remove(&g_iters, it)) it->release(it);
    return 0;
}

void iter_close_handle(int64_t handle) {
    IterObj *it = (IterObj *)(intptr_t)handle;
    if (handleset_has(&g_iters, it)) iter_owner_check(it);
    if (handleset_remove(&g_iters, it)) it->release(it);
}

/* iter_next<T>(it) -> Option<T>: for a `while` loop that wants the items one by one. */
static Value native_iter_next(Value *args, int argc, int line, int col) {
    (void)argc;
    Value item;
    Type option = type_enum("Option");
    if (iter_advance(iter_handle_of(args[0]), &item, line, col)) {
        return value_enum(option, enum_obj_new("Option", "Some", 1, item));
    }
    return value_enum(option, enum_obj_new("Option", "None", 0, value_int(0)));
}

static Value native_iter_close(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    iter_close_handle(iter_handle_of(args[0]));
    return value_int(0);
}

struct LineSource {
    FILE *file;
    char *buf;
    size_t cap;
    int64_t number;
    int failed;
};

LineSource *line_source_open(const char *path) {
    FILE *f = fopen(path, "r");
#ifdef ZIMAN_GC
    if (f == NULL && (errno == EMFILE || errno == ENFILE)) {
        GC_gcollect();
        GC_invoke_finalizers();
        f = fopen(path, "r");
    }
#endif
    if (f == NULL) return NULL;
    LineSource *ls = ziman_xcalloc(1, sizeof(LineSource));
    ls->file = f;
    return ls;
}

int line_source_next(LineSource *ls, const char **line, size_t *len, int64_t *number, char *err, size_t err_size) {
    if (ls->file == NULL || ls->failed) return 0;
    ssize_t n = getline(&ls->buf, &ls->cap, ls->file);
    if (n < 0) {
        if (ferror(ls->file)) {
            ls->failed = 1;
            snprintf(err, err_size, "read error after line %lld: %s", (long long)ls->number, strerror(errno));
            return -1;
        }
        return 0;
    }
    ls->number++;
    size_t l = (size_t)n;
    if (l > 0 && ls->buf[l - 1] == '\n') l--;
    if (l > 0 && ls->buf[l - 1] == '\r') l--;
    *line = ls->buf;
    *len = l;
    *number = ls->number;
    return 1;
}

/* libc's own `free`, for the one block here that libc allocated. `getline` grows `ls->buf` with
 * realloc, so it belongs to libc -- while under the collector build zgc.h has redefined `free` to a
 * no-op, on the reasoning that nothing the interpreter allocates is ever released by hand. That
 * reasoning does not reach a buffer the collector never handed out: Boehm cannot reclaim it, so it
 * stayed for the life of the process. 20,000 lines of 60 KB cost 1.27 GB of RSS that way, against
 * 28 MB under ziman-nogc, whose `free` is real -- the collector build leaking where the build that
 * "never frees anything" does not (docs/reviews/tasks.md A17). Suspended rather than undefined, so
 * the rest of this file still allocates through the collector; the same idiom as
 * src/native_http.c:15-27, which suspends these macros to read malloc.h. */
#pragma push_macro("free")
#undef free
static void libc_free(void *p) { free(p); }
#pragma pop_macro("free")

void line_source_close(LineSource *ls) {
    if (ls->file != NULL) fclose(ls->file);
    ls->file = NULL;
    libc_free(ls->buf);
    ls->buf = NULL;
}

/* iter_from<T>(next, close): a source written in Ziman. `next` returns the next item as Some, or None
 * at the end; `close` is called once when the source is finished, left early or closed by hand. The
 * state lives in whatever the closures call (a native handle, a channel), since a closure cannot
 * change a variable it captured. Not closed by the garbage collector: it would run Ziman code from a
 * finalizer. */
typedef struct {
    Value next;
    Value close;
} FromState;

static int from_next(IterObj *it, Value *out) {
    FromState *st = it->state;
    Value r = native_call_closure(st->next, NULL, 0, it->line, it->col);
    if (strcmp(r.as.en->variant_name, "Some") != 0) return 0;
    *out = r.as.en->payload;
    return 1;
}

static void from_release(IterObj *it) {
    FromState *st = it->state;
    native_call_closure(st->close, NULL, 0, it->line, it->col);
}

static Value native_iter_from(Value *args, int argc, int line, int col) {
    (void)argc;
    FromState *st = ziman_xcalloc(1, sizeof(FromState));
    st->next = args[0];
    st->close = args[1];
    IterObj *it = ziman_xcalloc(1, sizeof(IterObj));
    it->next = from_next;
    it->release = from_release;
    it->state = st;
    it->line = line;
    it->col = col;
    it->no_finalizer = 1;
    return iter_make(it);
}

/* iter_of_array<T>(items): the elements of an array as an Iter. */
typedef struct {
    Value items;
    int index;
} ArrayIterState;

static int array_iter_next(IterObj *it, Value *out) {
    ArrayIterState *st = it->state;
    if (st->index >= st->items.as.arr->count) return 0;
    *out = array_get(st->items.as.arr, st->index++);
    return 1;
}

static void array_iter_release(IterObj *it) { (void)it; }

static Value native_iter_of_array(Value *args, int argc, int line, int col) {
    (void)argc;
    (void)line;
    (void)col;
    ArrayIterState *st = ziman_xcalloc(1, sizeof(ArrayIterState));
    st->items = args[0];
    IterObj *it = ziman_xcalloc(1, sizeof(IterObj));
    it->next = array_iter_next;
    it->release = array_iter_release;
    it->state = st;
    return iter_make(it);
}

/* Registered with gc_mark.c (see gc_mark_register_struct_content_marker's own comment, gc_mark.h --
 * the identical seam MutableArray/MutableMap/MutableSet already use) so a real collection landing
 * while an Iter is still open doesn't sweep a Value its own native state holds but nothing else
 * references: `FromState.next`/`.close` (the closures `iter_from<T>` was built with) and
 * `ArrayIterState.items` (the array `iter_of_array<T>` is walking) are each referenced *only* by
 * `it->state`, a plain `calloc`'d (GC_MALLOC'd) struct invisible to both root sources the mark phase
 * otherwise relies on -- not on the stack once a `for`/`while` loop driving this Iter has moved past
 * the statement that created it, and not in any Env, since nothing ever binds `it` or `st`
 * themselves to a Ziman variable. Found the same way as every other gap in this family this
 * session: a real crash (`csv_records.zm`, a lazy `iter_from`-based CSV reader whose own `close`
 * closure, read back out of a since-swept `FromState`, pointed at corrupted memory), newly exposed
 * once `enum_obj_new` migrated to `gc_heap` -- `from_next`'s own `Option.Some`/`None` result made
 * real collections fire noticeably more often than this code path had ever been exercised against
 * before. `it->next` (a plain function pointer, set once at construction and never reassigned)
 * doubles as the discriminant for which state shape `it->state` actually is -- no separate tag
 * needed, since the two are always set together. An Iter a Ziman program has already closed (not in
 * `g_iters` any more) needs nothing marked: `iter_advance`/`iter_close_handle` already treat that
 * state as fully done, same as this does. */
static void iter_mark_contents(const Arena *a, StructObj *st) {
    IterObj *it = (IterObj *)(intptr_t)st->fields[0].as.i;
    if (!handleset_has(&g_iters, it)) return;
    if (it->next == from_next) {
        FromState *fs = it->state;
        gc_mark_value(a, fs->next);
        gc_mark_value(a, fs->close);
    } else if (it->next == array_iter_next) {
        ArrayIterState *as = it->state;
        gc_mark_value(a, as->items);
    }
}

void native_iter_register(void) {
    native_register("iter_from", native_iter_from);
    native_register("iter_of_array", native_iter_of_array);
    native_register("iter_next", native_iter_next);
    native_register("iter_close", native_iter_close);
    gc_mark_register_struct_content_marker("Iter", iter_mark_contents);
}
