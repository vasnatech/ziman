#ifndef ZIMAN_ITER_H
#define ZIMAN_ITER_H

#include <pthread.h>
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
    /* the source position of the `for` or call asking for an item, for a source that runs Ziman code */
    int line, col;
    /* nonzero: the garbage collector must not run `release` (it runs Ziman code, which is not safe from
     * a finalizer), so the source is released only when finished or closed */
    /* Set for an iterator whose `release` calls back into Ziman -- `iter_from`'s, which invokes the
     * program's own `close` closure (src/iter.c's from_release). Such an iterator is deliberately
     * *not* given a collector finalizer: Boehm runs finalizers at an arbitrary allocation point on
     * whichever thread happens to allocate, and running interpreted code there would re-enter the
     * interpreter from inside a collection, against an Iter whose owner thread is very likely not
     * the one running the finalizer. The cost is that forgetting `close` on an `iter_from` leaks
     * whatever that closure would have released -- stated rather than fixed, because the fix proposed
     * for it was to register that finalizer anyway. What such an iterator does get is
     * iter_drop_finalizer, which takes it out of the live set and runs no interpreted code, so an
     * abandoned one no longer leaves a dangling pointer in the validation table either
     * (docs/reviews/tasks.md F16). */
    int no_finalizer;
    /* the thread that made it: an Iter is a mutable cursor and belongs to that thread (`#[thread_bound]` in lib/prelude/iter.zm), so walking or closing it from
     * another thread is a runtime error. Set by iter_make. The collector's finalizer releases a dropped one from its own thread and is exempt. */
    pthread_t owner;
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

/* A file read one line at a time, shared by the line-oriented sources (`fs.lines`, `ndjson.records`). */
typedef struct LineSource LineSource;

/* Opens `path` for reading, or returns NULL with errno set. Out of descriptors, it runs the collector
 * once (dropped iterators close their files in finalizers) and tries again. */
LineSource *line_source_open(const char *path);

/* Returns 1 with the next line (without its "\n" or "\r\n", not NUL-terminated, valid until the next
 * call) and its 1-based number, 0 at the end of the file, or -1 with a message in `err` after a read
 * error. */
int line_source_next(LineSource *ls, const char **line, size_t *len, int64_t *number, char *err, size_t err_size);

/* Closes the file; harmless if already closed. */
void line_source_close(LineSource *ls);

void native_iter_register(void);

#endif
