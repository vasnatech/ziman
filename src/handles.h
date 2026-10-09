#ifndef ZIMAN_HANDLES_H
#define ZIMAN_HANDLES_H

#include <pthread.h>
#include <stddef.h>

/* A set of live native objects (sockets, database connections), identified by address.
 *
 * Ziman code holds such an object as an integer handle -- the object's address. The set is what
 * lets a native function reject a handle that was closed, or made up, instead of dereferencing
 * it. Its table lives in memory the garbage collector does not scan, so being in the set does
 * not keep an object alive: an object nobody holds any more is collected, and its finalizer (see
 * native_net.c, native_sqlite.c) closes the descriptor or connection. */
typedef struct {
    pthread_mutex_t mu;
    void **slots;
    size_t capacity; /* a power of two, or 0 before the first add */
    size_t used;     /* live entries plus tombstones */
    size_t live;
} HandleSet;

#define HANDLE_SET_INIT {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0}

void handleset_add(HandleSet *s, void *p);
int handleset_has(HandleSet *s, void *p);
/* `handleset_has`, with `on_present(p)` run **while the lock is still held** if it was there.
 * That is how a caller takes a reference to an object it has just found without a window between
 * the two: a concurrent close has to win the whole lock, so it either runs before (and the lookup
 * fails) or after (and the reference is already counted). Checking and then referencing as two
 * operations is the bug this exists to prevent -- see native_net.c's handle_use. The callback runs
 * under the lock, so it must not allocate, block, or touch this set. */
int handleset_has_then(HandleSet *s, void *p, void (*on_present)(void *));
/* Returns 1 if `p` was in the set. */
int handleset_remove(HandleSet *s, void *p);

#endif
