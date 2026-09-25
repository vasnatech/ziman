#ifndef ZIMAN_ITER_H
#define ZIMAN_ITER_H

#include <stdint.h>

#include "value.h"

/* A lazy source of values that a `for` loop walks one at a time: a file's lines, later a database
 * cursor. Ziman code holds it as an `Iter<T>` struct whose one field is a handle (the address of an
 * IterObj, tracked in a HandleSet so a closed or made-up handle is an error, not a crash). A
 * source that is never finished or closed is closed when the garbage collector collects it. */
typedef struct IterObj {
    /* Stores the next item in *out and returns 1, or returns 0 when the source is exhausted. */
    int (*next)(struct IterObj *it, Value *out);
    /* Releases what the source holds (a file, a cursor); called once. */
    void (*release)(struct IterObj *it);
    void *state;
} IterObj;

/* Wraps `it` as an `Iter<T>` value. */
Value iter_make(IterObj *it);

/* Used by the interpreter's `for`: the next item of the iterator in `handle` (1) or none (0). An
 * exhausted iterator has already released its source. */
int iter_advance(int64_t handle, Value *out, int line, int col);

/* Releases the iterator early (`break`, `return`); harmless if it is already finished or closed. */
void iter_close_handle(int64_t handle);

/* The handle stored in an `Iter<T>` value. */
int64_t iter_handle_of(Value iter);

void native_iter_register(void);

#endif
