#include "handles.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "value.h"

#define TOMBSTONE ((void *)1)

static size_t hash_ptr(void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x;
}

/* What roots a handle, written down because the answer is not this table and the table looks like it
 * (docs/reviews/tasks/ F16).
 *
 * The slots are `zalloc_atomic`, which means the collector never scans them, and that is deliberate:
 * a set that kept every handle it had ever seen alive would be a leak with a membership test on top,
 * and `net_finalizer`/`iter_finalizer` exist precisely to close a handle whose Ziman value nobody
 * holds any more. So a pointer being *in* here keeps nothing alive.
 *
 * What keeps the object alive is the Ziman value that names it. `iter_make` boxes the address in an
 * ordinary `Value` field (`src/iter.c`: `value_int((int64_t)(intptr_t)it)`), the struct's field array
 * comes from the collector's own heap and is scanned, and Boehm is conservative -- an integer whose
 * value happens to be the address is indistinguishable from a pointer to it, which is exactly what
 * roots it. The handle survives as long as the `Iter` or `Socket` value does, and no longer.
 *
 * The edge in that argument, since it rests on where the integer lives rather than on what it is: a
 * packed `[int]` array is `zalloc_atomic` too (`src/value.c`'s pack_kind path), so an address stored
 * *there* would root nothing. `it.handle` is an ordinary int field, so a Ziman program can read the
 * address out and put it in such an array.
 *
 * This comment used to add that the gap was harmless "because nothing in the language takes a bare
 * int as a handle: every builtin wants the `Iter` or `Socket` value, which does the rooting."
 * **That is false, and was checked rather than reasoned about (2026-10-15).** These wrappers are
 * ordinary `pub struct`s with an ordinary `int` field, so a program can write
 * `net.Listener { handle: someInt }` and hand it to any of these functions -- the struct is a box,
 * not a capability. Verified both ways: rewrapping a *live* handle produces a working value
 * (`net.listener_port` on the copy answers the same port as the original), and a made-up handle is
 * rejected cleanly by this very table, with a runtime error rather than a dereference.
 *
 * So the safety property holds -- a fabricated handle cannot crash anything, which is what this
 * table is for -- and only the rooting claim was overstated. The residual hazard is narrow and was
 * *not* demonstrated: stash a real handle in a packed `[int]`, drop the wrapper, let the object be
 * collected and its finalizer close the descriptor, let a new object land at the same address, then
 * rewrap the stale integer -- which would silently operate on the wrong object, since the address is
 * live again and this table would say yes. An attempt to produce that (a listener stashed from a
 * dead scope, 800,000 string allocations of churn, 60 further listeners opened to reuse the address)
 * came back with the original still alive and answering its own port, so either the collection
 * pressure was not enough or something else still roots it. Worth knowing before trusting address
 * identity anywhere: `handles.h`'s own generation-counter idea would close it, and a validation
 * cache keyed on the address alone would widen it. */
static void **alloc_slots(size_t n) {
    void **slots = zalloc_atomic(n * sizeof(void *)); /* never scanned by the collector -- see above */
    memset(slots, 0, n * sizeof(void *));
    return slots;
}

/* Moves every entry into `fresh`, an empty table of `new_capacity` slots (a power of two). */
static void rehash_into(HandleShard *s, void **fresh, size_t new_capacity) {
    void **old = s->slots;
    size_t old_capacity = s->capacity;
    s->slots = fresh;
    s->capacity = new_capacity;
    s->used = 0;
    for (size_t i = 0; i < old_capacity; i++) {
        void *p = old[i];
        if (p == NULL || p == TOMBSTONE) continue;
        size_t at = hash_ptr(p) & (new_capacity - 1);
        while (s->slots[at] != NULL) at = (at + 1) & (new_capacity - 1);
        s->slots[at] = p;
        s->used++;
    }
}

/* The capacity the table must be rehashed to before one more entry goes in, or 0 when it has room. */
static size_t capacity_needed(const HandleShard *s) {
    if (s->capacity == 0) return 64;
    if ((s->used + 1) * 2 <= s->capacity) return 0;
    return s->live * 4 >= s->capacity ? s->capacity * 2 : s->capacity;
}

static void shard_add(HandleShard *s, void *p) {
    pthread_mutex_lock(&s->mu);
    for (;;) {
        size_t need = capacity_needed(s);
        if (need == 0) break;
        size_t seen = s->capacity;
        size_t seen_used = s->used;
        pthread_mutex_unlock(&s->mu);
        /* Allocated with the lock released: an allocation can start a collection, and the collector runs finalizers
         * (net_finalizer, ...) on the allocating thread, which take this same lock to remove their handle -- held
         * here, that is a thread waiting for itself, and every other thread that touches a socket waiting behind it. */
        void **fresh = alloc_slots(need);
        pthread_mutex_lock(&s->mu);
        if (s->capacity == seen && s->used == seen_used) rehash_into(s, fresh, need);
    }
    size_t at = hash_ptr(p) & (s->capacity - 1);
    while (s->slots[at] != NULL && s->slots[at] != TOMBSTONE) at = (at + 1) & (s->capacity - 1);
    if (s->slots[at] == NULL) s->used++;
    s->slots[at] = p;
    s->live++;
    pthread_mutex_unlock(&s->mu);
}

/* The probe itself. Called with the lock held. */
static int present_locked(const HandleShard *s, void *p) {
    if (s->capacity == 0 || p == NULL || p == TOMBSTONE) return 0;
    size_t at = hash_ptr(p) & (s->capacity - 1);
    while (s->slots[at] != NULL) {
        if (s->slots[at] == p) return 1;
        at = (at + 1) & (s->capacity - 1);
    }
    return 0;
}

static int shard_has(HandleShard *s, void *p) {
    pthread_mutex_lock(&s->mu);
    int found = present_locked(s, p);
    pthread_mutex_unlock(&s->mu);
    return found;
}

static int shard_has_then(HandleShard *s, void *p, void (*on_present)(void *)) {
    pthread_mutex_lock(&s->mu);
    int found = present_locked(s, p);
    if (found && on_present != NULL) on_present(p);
    pthread_mutex_unlock(&s->mu);
    return found;
}

static int shard_remove(HandleShard *s, void *p) {
    int found = 0;
    pthread_mutex_lock(&s->mu);
    if (s->capacity > 0 && p != NULL && p != TOMBSTONE) {
        size_t at = hash_ptr(p) & (s->capacity - 1);
        while (s->slots[at] != NULL) {
            if (s->slots[at] == p) { s->slots[at] = TOMBSTONE; s->live--; found = 1; break; }
            at = (at + 1) & (s->capacity - 1);
        }
    }
    pthread_mutex_unlock(&s->mu);
    return found;
}

/* Which shard a handle belongs to. Taken from the top bits of the hash, because the slot index uses
 * the bottom ones (`hash_ptr(p) & (capacity - 1)`) -- sharing bits between the two would make the
 * shards correlate with the table positions and fill unevenly. */
static HandleShard *shard_for(HandleSet *s, void *p) {
    return &s->shard[(hash_ptr(p) >> 57) & (HANDLESET_SHARDS - 1)];
}

void handleset_add(HandleSet *s, void *p) { shard_add(shard_for(s, p), p); }
int handleset_has(HandleSet *s, void *p) { return shard_has(shard_for(s, p), p); }
int handleset_has_then(HandleSet *s, void *p, void (*on_present)(void *)) {
    return shard_has_then(shard_for(s, p), p, on_present);
}
int handleset_remove(HandleSet *s, void *p) { return shard_remove(shard_for(s, p), p); }
