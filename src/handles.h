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
/* Sharded, because this used to be one table under one mutex and every socket read, write and
 * accept in the process went through it: measured at 21.6 ns per operation on one thread and
 * **248.0 ns on sixteen** -- the only thing on that path that got *worse* as threads were added,
 * which is the shape F7 and A19 are about, and it was already in the tree rather than proposed
 * (docs/reviews/tasks.md A34). Each handle belongs to one shard, chosen from bits of its hash that
 * the slot index does not use, so the shards fill evenly and a lock is only contended by the
 * handles that share it.
 *
 * Sixty-four, measured rather than guessed. Nanoseconds per `handleset_has`, best of three, 256
 * handles on a 16-core machine:
 *
 *     threads   1 mutex   4 shards   16 shards   64 shards   256 shards
 *           1      13.8       19.7        14.2        13.9         12.7
 *           4      85.7       43.0        32.8        16.4         15.2
 *          16     156.3       92.8        61.8        19.6         13.5
 *
 * One thread is unchanged whatever the count, so this costs nothing to a program that never
 * contends. Sixteen shards only halves the problem; 64 nearly removes it -- 13.9 ns at one thread
 * against 19.6 at sixteen, where one mutex went from 13.8 to 156.3. 256 is marginally better again
 * and takes four times the memory for it: a shard is a `pthread_mutex_t` and four words, so 64 of
 * them is about 2.5 KB per set against 10 KB, paid by every program whether or not it opens a
 * socket. Note what is *not* claimed: this reduces contention, it does not remove the lock. A fully
 * lock-free design -- a generation counter inside the handle value, so validation is an atomic load
 * -- is the real answer and a much larger change.
 *
 * Nothing outside this file sees the shards: add, has, has_then and remove take the whole set and
 * pick one. */
#define HANDLESET_SHARDS 64

typedef struct {
    pthread_mutex_t mu;
    void **slots;
    size_t capacity; /* a power of two, or 0 before the first add */
    size_t used;     /* live entries plus tombstones */
    size_t live;
} HandleShard;

typedef struct {
    HandleShard shard[HANDLESET_SHARDS];
} HandleSet;

#define HANDLE_SHARD_INIT {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0}
#define HANDLE_SHARD_INIT_8 \
    HANDLE_SHARD_INIT, HANDLE_SHARD_INIT, HANDLE_SHARD_INIT, HANDLE_SHARD_INIT, \
    HANDLE_SHARD_INIT, HANDLE_SHARD_INIT, HANDLE_SHARD_INIT, HANDLE_SHARD_INIT
#define HANDLE_SHARD_INIT_16 HANDLE_SHARD_INIT_8, HANDLE_SHARD_INIT_8
#define HANDLE_SET_INIT {{HANDLE_SHARD_INIT_16, HANDLE_SHARD_INIT_16, HANDLE_SHARD_INIT_16, HANDLE_SHARD_INIT_16}}

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
