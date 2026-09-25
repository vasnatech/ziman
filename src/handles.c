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

static void rehash(HandleSet *s, size_t new_capacity) {
    void **old = s->slots;
    size_t old_capacity = s->capacity;
    s->slots = alloc_slots(new_capacity);
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

void handleset_add(HandleSet *s, void *p) {
    pthread_mutex_lock(&s->mu);
    if (s->capacity == 0) rehash(s, 64);
    else if ((s->used + 1) * 2 > s->capacity) rehash(s, s->live * 4 >= s->capacity ? s->capacity * 2 : s->capacity);
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
