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

static void **alloc_slots(size_t n) {
    void **slots = zalloc_atomic(n * sizeof(void *)); /* never scanned by the collector */
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
