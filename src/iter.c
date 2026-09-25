#define _POSIX_C_SOURCE 200809L
#include "iter.h"

#ifdef ZIMAN_GC
#include <gc.h>
#endif
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
    GC_REGISTER_FINALIZER_NO_ORDER(it, iter_finalizer, NULL, NULL, NULL);
#endif
    Value *fields = malloc(sizeof(Value));
    fields[0] = value_int((int64_t)(intptr_t)it);
    return value_struct(type_struct("Iter"), struct_obj_new(ITER_FIELD_NAMES, fields, 1));
}

int64_t iter_handle_of(Value iter) { return iter.as.st->fields[0].as.i; }

int iter_advance(int64_t handle, Value *out, int line, int col) {
    IterObj *it = (IterObj *)(intptr_t)handle;
    if (!handleset_has(&g_iters, it)) return 0; /* closed early: nothing more to give */
    (void)line;
    (void)col;
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

void native_iter_register(void) {
    native_register("iter_next", native_iter_next);
    native_register("iter_close", native_iter_close);
}
