#include "gc_guard_roots.h"

#include <pthread.h>
#include <stdlib.h>

#include "gc_mark.h"
#include "xalloc.h"

/* One record per currently-live Guard, on a global (non-`__thread`) list for the identical reason
 * native_thread.c's own ThreadInfo/g_threads is one: a guard's own finalizer can run on any thread,
 * not only the one that created it, so both register and unregister need to reach this list from
 * any thread -- see this module's own header comment for the full story. */
typedef struct GuardRoot {
    struct GuardRoot *next;
    void *owner_heap;
    Value *message_slot;
} GuardRoot;

static GuardRoot *g_roots = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void ziman_guard_register(void *owner_heap, Value *message_slot) {
    GuardRoot *r = ziman_xmalloc(sizeof(GuardRoot));
    r->owner_heap = owner_heap;
    r->message_slot = message_slot;
    pthread_mutex_lock(&g_lock);
    r->next = g_roots;
    g_roots = r;
    pthread_mutex_unlock(&g_lock);
}

void ziman_guard_unregister(Value *message_slot) {
    pthread_mutex_lock(&g_lock);
    for (GuardRoot **p = &g_roots; *p != NULL; p = &(*p)->next) {
        if ((*p)->message_slot == message_slot) {
            *p = (*p)->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void gc_mark_live_guards(void *heap, const Arena *a) {
    /* A real cross-thread race, not a formality: a different thread can register or finalize
     * (unregister) a guard at any moment, concurrently with this thread's own Phase 3 collection --
     * unlike gc_boehm_roots.c's own push_other_roots, which gets away without a lock because Boehm
     * itself stops every other thread first. Phase 3 stops nothing; the lock here is load-bearing. */
    pthread_mutex_lock(&g_lock);
    for (GuardRoot *r = g_roots; r != NULL; r = r->next) {
        if (r->owner_heap == heap) gc_mark_value(a, *r->message_slot);
    }
    pthread_mutex_unlock(&g_lock);
}
