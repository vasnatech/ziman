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
 * (docs/reviews/tasks.md F16).
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
 * *there* would root nothing. That is reachable today only halfway -- `it.handle` is an ordinary int
 * field and a Ziman program can read it (it prints a raw address) and put it in an array -- and it
 * is harmless only because nothing in the language takes a bare int as a handle: every builtin wants
 * the `Iter` or `Socket` value, which does the rooting. A native that accepted a handle integer
 * directly would break this, and that is the thing to notice before adding one. */
static void **alloc_slots(size_t n) {
    void **slots = zalloc_atomic(n * sizeof(void *)); /* never scanned by the collector -- see above */
    memset(slots, 0, n * sizeof(void *));
    return slots;
}

/* Moves every entry into `fresh`, an empty table of `new_capacity` slots (a power of two). */
static void rehash_into(HandleSet *s, void **fresh, size_t new_capacity) {
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
static size_t capacity_needed(const HandleSet *s) {
    if (s->capacity == 0) return 64;
    if ((s->used + 1) * 2 <= s->capacity) return 0;
    return s->live * 4 >= s->capacity ? s->capacity * 2 : s->capacity;
}

void handleset_add(HandleSet *s, void *p) {
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

int handleset_has(HandleSet *s, void *p) {
    int found = 0;
    pthread_mutex_lock(&s->mu);
    if (s->capacity > 0 && p != NULL && p != TOMBSTONE) {
        size_t at = hash_ptr(p) & (s->capacity - 1);
        while (s->slots[at] != NULL) {
            if (s->slots[at] == p) { found = 1; break; }
            at = (at + 1) & (s->capacity - 1);
        }
    }
    pthread_mutex_unlock(&s->mu);
    return found;
}

int handleset_remove(HandleSet *s, void *p) {
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
