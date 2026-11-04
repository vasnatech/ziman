#include "gc_pending.h"

#include "error.h"
#include "gc_mark.h"

/* Innermost first: a site pushes before it starts filling and pops when the buffer has been handed
 * off to an object the ordinary root sources can reach. See the header for why the nodes live on
 * the caller's stack rather than in an array here. */
static __thread PendingValues *t_pending = NULL;

void gc_pending_push(PendingValues *node, const Value *items, int count) {
    node->next = t_pending;
    node->items = items;
    node->count = count;
    t_pending = node;
}

void gc_pending_pop(PendingValues *node) {
    /* The caller's own nesting is the invariant; popping anything but the head would leave the
     * chain naming a frame that has returned, so this trusts the pairing rather than searching. */
    if (t_pending != node) return;
    t_pending = node->next;
}

void gc_mark_pending(const Arena *a) {
    for (PendingValues *p = t_pending; p != NULL; p = p->next) {
        /* `count` read here, not at push time, so a site that raises it one element at a time has
         * exactly its initialised prefix marked. */
        int count = p->count;
        for (int i = 0; i < count; i++) gc_mark_value(a, p->items[i]);
    }
}

void *gc_pending_save(void) { return t_pending; }

void gc_pending_restore(void *saved) { t_pending = (PendingValues *)saved; }

void gc_pending_install_unwind_hook(void) { ziman_recovery_set_unwind(gc_pending_save, gc_pending_restore); }

int gc_pending_depth(void) {
    int n = 0;
    for (PendingValues *p = t_pending; p != NULL; p = p->next) n++;
    return n;
}
