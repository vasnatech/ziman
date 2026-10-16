#include "handleslot.h"

#include <pthread.h>
#include <string.h>

#ifdef ZIMAN_GC
#include <gc.h>
#endif

#include "value.h" /* zalloc_atomic */
#include "xalloc.h"

/* A slot's pointer is held weakly (see handleslot.h), so it lives in unscanned memory and is
 * cleared by the collector. `gen` is bumped on every release, so a handle naming an earlier tenant
 * of the same slot no longer matches. */
typedef struct {
    void *ptr;         /* NULL exactly when the slot is free or its object has been collected */
    uint32_t gen;
    int32_t next_free; /* free list, or -1; only meaningful while `ptr` is NULL */
} Slot;

struct HandleSlots {
    /* Blocks are allocated once and never moved or freed, which is what lets a reader index one
     * without a lock: a published block pointer stays valid for the life of the process. */
    Slot *block[HANDLESLOT_BLOCKS];
    size_t used;  /* slots ever handed out, so the next fresh index is known */
    int32_t free_head; /* index of the first reusable slot, or -1 */
    size_t live;
    pthread_mutex_t mu; /* add and release only; a lookup takes nothing */
};

#define HANDLE_INDEX_BITS 32
#define HANDLE_INDEX_MASK 0xffffffffULL
#define HANDLE_GEN_MASK 0x7fffffffULL /* 31 bits: the sign bit stays clear for Ziman's `int` */

/* The index is stored one higher than it is, so that a handle is never 0: slot 0 at generation 0
 * would otherwise pack to exactly the value this file reserves for "no handle", and the very first
 * handle a table ever issues is slot 0 at generation 0. Biasing the index rather than starting the
 * generation at 1 keeps that true for the slot's whole life, since a generation increments and
 * would eventually come back round to 0. */
static int64_t pack(uint32_t index, uint32_t gen) {
    return (int64_t)(((uint64_t)(gen & HANDLE_GEN_MASK) << HANDLE_INDEX_BITS) | (uint64_t)(index + 1));
}

static uint32_t handle_index(int64_t h) {
    return (uint32_t)(((uint64_t)h & HANDLE_INDEX_MASK) - 1);
}

static uint32_t handle_gen(int64_t h) {
    return (uint32_t)(((uint64_t)h >> HANDLE_INDEX_BITS) & HANDLE_GEN_MASK);
}

static Slot *slot_at(const HandleSlots *t, uint32_t index) {
    size_t b = index / HANDLESLOT_BLOCK;
    if (b >= HANDLESLOT_BLOCKS) return NULL;
    Slot *block = __atomic_load_n(&t->block[b], __ATOMIC_ACQUIRE);
    if (block == NULL) return NULL;
    return &block[index % HANDLESLOT_BLOCK];
}

HandleSlots *handleslot_table(void) {
    HandleSlots *t = ziman_xcalloc(1, sizeof(HandleSlots));
    pthread_mutex_init(&t->mu, NULL);
    t->free_head = -1;
    return t;
}

/* The free list gets its own field rather than being threaded through `ptr`. `Slot` has the room
 * for free (8 + 4 + 4 fits the same 16 bytes), and it keeps `ptr == NULL` meaning exactly "not
 * live" -- which is what `handleslot_get` leans on. Threading it through `ptr` left a freed slot
 * holding a disguised small integer, so that check stopped being reliable and the generation was
 * the only thing standing between a stale handle and a bogus pointer. */
static int32_t take_free(HandleSlots *t) {
    if (t->free_head < 0) return -1;
    int32_t index = t->free_head;
    Slot *s = slot_at(t, (uint32_t)index);
    t->free_head = s->next_free;
    return index;
}

int64_t handleslot_add(HandleSlots *t, void *p) {
    pthread_mutex_lock(&t->mu);
    int32_t index = take_free(t);
    if (index < 0) {
        if (t->used >= (size_t)HANDLESLOT_BLOCKS * HANDLESLOT_BLOCK) {
            pthread_mutex_unlock(&t->mu);
            return 0;
        }
        size_t b = t->used / HANDLESLOT_BLOCK;
        if (t->block[b] == NULL) {
            /* Unscanned on purpose: a scanned slot would root its object and turn this table into
             * the leak-with-a-membership-test handles.c refuses. See handleslot.h. */
            Slot *fresh = zalloc_atomic(HANDLESLOT_BLOCK * sizeof(Slot));
            memset(fresh, 0, HANDLESLOT_BLOCK * sizeof(Slot));
            __atomic_store_n(&t->block[b], fresh, __ATOMIC_RELEASE);
        }
        index = (int32_t)t->used;
        t->used++;
    }
    Slot *s = slot_at(t, (uint32_t)index);
    s->ptr = p;
    t->live++;
#ifdef ZIMAN_GC
    /* The weak reference itself: the collector clears `s->ptr` when `p` becomes unreachable, and
     * does so before running its finalizer -- so a handle stops resolving before the descriptor
     * behind it is closed, rather than after. */
    GC_general_register_disappearing_link(&s->ptr, p);
#endif
    int64_t handle = pack((uint32_t)index, s->gen);
    pthread_mutex_unlock(&t->mu);
    return handle;
}

void *handleslot_get(const HandleSlots *t, int64_t handle) {
    if (handle == 0) return NULL;
    Slot *s = slot_at(t, handle_index(handle));
    if (s == NULL) return NULL;
    /* The generation is read after the pointer, so a slot released and re-taken between the two
     * reads shows its new generation and the handle is rejected -- the stale-handle case this
     * design exists to catch. */
    void *p = __atomic_load_n(&s->ptr, __ATOMIC_ACQUIRE);
    if (p == NULL) return NULL; /* free, or the object has been collected */
    if (__atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) != handle_gen(handle)) return NULL;
    return p;
}

int handleslot_release(HandleSlots *t, int64_t handle) {
    if (handle == 0) return 0;
    pthread_mutex_lock(&t->mu);
    Slot *s = slot_at(t, handle_index(handle));
    if (s == NULL || s->ptr == NULL || s->gen != handle_gen(handle)) {
        pthread_mutex_unlock(&t->mu);
        return 0;
    }
#ifdef ZIMAN_GC
    GC_unregister_disappearing_link(&s->ptr);
#endif
    /* The pointer goes first and the generation second, so a concurrent `handleslot_get` can see
     * the slot as not-live but never as live-with-the-wrong-tenant. */
    __atomic_store_n(&s->ptr, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&s->gen, s->gen + 1, __ATOMIC_RELEASE);
    s->next_free = t->free_head;
    t->free_head = (int32_t)handle_index(handle);
    t->live--;
    pthread_mutex_unlock(&t->mu);
    return 1;
}

size_t handleslot_live(const HandleSlots *t) {
    return t->live;
}
