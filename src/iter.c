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
#include "handles.h"
#include "native.h"

static HandleSet g_iters = HANDLE_SET_INIT;
static char *ITER_FIELD_NAMES[] = {"handle"};

#ifdef ZIMAN_GC
static void iter_finalizer(void *obj, void *client_data) {
    (void)client_data;
    IterObj *it = obj;
    if (handleset_remove(&g_iters, it)) it->release(it);
}
#endif

Value iter_make(IterObj *it) {
    handleset_add(&g_iters, it);
#ifdef ZIMAN_GC
    if (!it->no_finalizer) GC_REGISTER_FINALIZER_NO_ORDER(it, iter_finalizer, NULL, NULL, NULL);
#endif
    Value *fields = malloc(sizeof(Value));
    fields[0] = value_int((int64_t)(intptr_t)it);
    return value_struct(type_struct("Iter"), struct_obj_new(ITER_FIELD_NAMES, fields, 1));
}

int64_t iter_handle_of(Value iter) { return iter.as.st->fields[0].as.i; }

int iter_advance(int64_t handle, Value *out, int line, int col) {
    IterObj *it = (IterObj *)(intptr_t)handle;
    if (!handleset_has(&g_iters, it)) return 0; /* closed early: nothing more to give */
    it->line = line;
    it->col = col;
    if (it->next(it, out)) return 1;
    if (handleset_remove(&g_iters, it)) it->release(it);
    return 0;
}

void iter_close_handle(int64_t handle) {
    IterObj *it = (IterObj *)(intptr_t)handle;
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
    LineSource *ls = calloc(1, sizeof(LineSource));
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

void line_source_close(LineSource *ls) {
    if (ls->file != NULL) fclose(ls->file);
    ls->file = NULL;
    free(ls->buf);
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
    FromState *st = calloc(1, sizeof(FromState));
    st->next = args[0];
    st->close = args[1];
    IterObj *it = calloc(1, sizeof(IterObj));
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
    ArrayIterState *st = calloc(1, sizeof(ArrayIterState));
    st->items = args[0];
    IterObj *it = calloc(1, sizeof(IterObj));
    it->next = array_iter_next;
    it->release = array_iter_release;
    it->state = st;
    return iter_make(it);
}

void native_iter_register(void) {
    native_register("iter_from", native_iter_from);
    native_register("iter_of_array", native_iter_of_array);
    native_register("iter_next", native_iter_next);
    native_register("iter_close", native_iter_close);
}
