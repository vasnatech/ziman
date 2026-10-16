#include "maptable.h"

#include <string.h>

#include "xalloc.h"

#define MAPTABLE_EMPTY (-1)
#define MAPTABLE_DUMMY (-2)
#define MAPTABLE_MIN_INDEX 8

/* Two attempts at the single large `entries` block, both measured and both reverted -- recorded so
 * they are not repeated. A table of 100,000 entries is one 5.6 MB array, and repeatedly allocating
 * a block that size makes the collector emit "Repeated allocation of very large block"; main.c's
 * warn proc caps that at three lines, so it is noise rather than a flood.
 *
 * Fixed 1024-entry chunks fixed the warning and removed the growth copying, and allocated 57 KB on
 * the first insert of every builder -- csv_extended went from 0.16s to 6.8s, because it builds a
 * small map per row. `GC_malloc_ignore_off_page` (the documented remedy, whose precondition does
 * hold here: `t->entries` is the only reference and points at the start) removed the warning
 * without that cost, but it cannot be `realloc`'d, and allocate-and-copy gave up the in-place
 * growth `GC_REALLOC` manages for large blocks: the many-small-builders benchmark went 262 ->
 * 322-356 ns/insert.
 *
 * The sizes to serve are two-entry maps and hundred-thousand-entry maps at once, which is what
 * makes this harder than it looks. */


/* The index array is kept at most two-thirds full, the same load factor `dict` uses: open
 * addressing degrades sharply past that, and the probe below is only pseudo-random. */
static int index_is_full(const MapTable *t) {
    return (t->entry_used + 1) * 3 > t->index_cap * 2;
}

/* `value_hash` already finalises an int key's identity hash (mapobj.c's own map_hash does the same
 * for the trie, and for the same reason: consecutive keys must not land in one run of slots). */
static size_t first_slot(uint64_t hash, size_t mask) {
    return (size_t)hash & mask;
}

/* `dict`'s probe: scatter using the high hash bits, five at a time, so a cluster of keys that
 * collide on the low bits does not walk the table linearly. */
static size_t next_slot(size_t slot, uint64_t *perturb, size_t mask) {
    *perturb >>= 5;
    return (slot * 5 + (size_t)*perturb + 1) & mask;
}

/* The entries index `key` is stored at, or -1. Tombstones are walked past, never stopped on. */
static int32_t find_entry(const MapTable *t, Value key, uint64_t hash) {
    if (t->index_cap == 0) return -1;
    size_t mask = t->index_cap - 1;
    size_t slot = first_slot(hash, mask);
    uint64_t perturb = hash;
    for (;;) {
        int32_t at = t->indices[slot];
        if (at == MAPTABLE_EMPTY) return -1;
        if (at != MAPTABLE_DUMMY) {
            const MapTableEntry *e = &t->entries[at];
            if (e->hash == hash && e->key.type != NULL && value_key_equals(e->key, key)) return at;
        }
        slot = next_slot(slot, &perturb, mask);
    }
}

/* Rebuilds the index, and compacts `entries` while it is at it: a removal leaves a dead entry
 * behind so that the live ones keep their insertion order, and this is where that space comes
 * back. Called when the index fills, which is also the only time the dead can have accumulated
 * enough to matter. */
static void rehash(MapTable *t, size_t want_entries) {
    size_t live = 0;
    for (size_t i = 0; i < t->entry_used; i++) {
        if (t->entries[i].key.type != NULL) {
            if (live != i) t->entries[live] = t->entries[i];
            live++;
        }
    }
    t->entry_used = live;
    t->live = live;

    size_t entry_cap = t->entry_cap == 0 ? MAPTABLE_MIN_INDEX : t->entry_cap;
    while (entry_cap < want_entries) entry_cap *= 2;
    if (entry_cap != t->entry_cap) {
        t->entries = ziman_xrealloc(t->entries, entry_cap * sizeof(MapTableEntry));
        t->entry_cap = entry_cap;
    }

    /* Three slots per two live entries at the smallest, so a table that has just filled gets real
     * headroom rather than rehashing again on the next insert. Sized by that rule alone: an
     * earlier version also forced `index_cap >= want_entries * 2`, which made every index array a
     * third larger than the load factor asks for, and every byte of that is garbage the moment the
     * builder is dropped.
     *
     * Allocated fresh rather than `realloc`'d, and *atomic*. Fresh because the memset below
     * overwrites every slot, so a realloc would copy the old contents only to discard them. Atomic
     * because an index array is `int32_t` and cannot hold a pointer by construction, so the
     * collector has no reason to scan it. */
    size_t index_cap = MAPTABLE_MIN_INDEX;
    while (index_cap * 2 < (live + 1) * 3) index_cap *= 2;
    t->indices = zalloc_atomic(index_cap * sizeof(int32_t));
    t->index_cap = index_cap;
    memset(t->indices, 0xff, index_cap * sizeof(int32_t)); /* 0xff.. is MAPTABLE_EMPTY */

    size_t mask = index_cap - 1;
    for (size_t i = 0; i < t->entry_used; i++) {
        uint64_t hash = t->entries[i].hash;
        size_t slot = first_slot(hash, mask);
        uint64_t perturb = hash;
        while (t->indices[slot] != MAPTABLE_EMPTY) slot = next_slot(slot, &perturb, mask);
        t->indices[slot] = (int32_t)i;
    }
}

void maptable_init(MapTable *t) {
    memset(t, 0, sizeof(*t));
}

void maptable_release(MapTable *t) {
    free(t->indices);
    free(t->entries);
    memset(t, 0, sizeof(*t));
}

int maptable_get(const MapTable *t, Value key, Value *out) {
    int32_t at = find_entry(t, key, value_hash(key));
    if (at < 0) return 0;
    if (out != NULL) *out = t->entries[at].val;
    return 1;
}

int maptable_set(MapTable *t, Value key, Value val) {
    uint64_t hash = value_hash(key);
    int32_t at = find_entry(t, key, hash);
    if (at >= 0) {
        t->entries[at].val = val;
        return 0;
    }
    if (t->index_cap == 0 || index_is_full(t) || t->entry_used == t->entry_cap) {
        rehash(t, t->live + 1);
    }
    size_t mask = t->index_cap - 1;
    size_t slot = first_slot(hash, mask);
    uint64_t perturb = hash;
    /* A tombstone is reusable here, unlike in find_entry: nothing beyond it can be this key, since
     * find_entry just walked the whole chain and did not find it. */
    while (t->indices[slot] != MAPTABLE_EMPTY && t->indices[slot] != MAPTABLE_DUMMY) {
        slot = next_slot(slot, &perturb, mask);
    }
    size_t i = t->entry_used++;
    t->entries[i].hash = hash;
    t->entries[i].key = key;
    t->entries[i].val = val;
    t->indices[slot] = (int32_t)i;
    t->live++;
    return 1;
}

int maptable_remove(MapTable *t, Value key, Value *out) {
    uint64_t hash = value_hash(key);
    if (t->index_cap == 0) return 0;
    size_t mask = t->index_cap - 1;
    size_t slot = first_slot(hash, mask);
    uint64_t perturb = hash;
    for (;;) {
        int32_t at = t->indices[slot];
        if (at == MAPTABLE_EMPTY) return 0;
        if (at != MAPTABLE_DUMMY) {
            MapTableEntry *e = &t->entries[at];
            if (e->hash == hash && e->key.type != NULL && value_key_equals(e->key, key)) {
                if (out != NULL) *out = e->val;
                /* The entry dies in place rather than being moved out: the live entries around it
                 * must keep their insertion order, and rehash() reclaims the space later. */
                e->key.type = NULL;
                t->indices[slot] = MAPTABLE_DUMMY;
                t->live--;
                return 1;
            }
        }
        slot = next_slot(slot, &perturb, mask);
    }
}

size_t maptable_len(const MapTable *t) {
    return t->live;
}

void maptable_clear(MapTable *t) {
    /* Keeps both allocations: `clear` on a builder is nearly always followed by refilling it. */
    if (t->index_cap > 0) memset(t->indices, 0xff, t->index_cap * sizeof(int32_t));
    t->entry_used = 0;
    t->live = 0;
}

void maptable_visit(const MapTable *t, void (*visit)(Value key, Value val, void *ctx), void *ctx) {
    for (size_t i = 0; i < t->entry_used; i++) {
        if (t->entries[i].key.type != NULL) visit(t->entries[i].key, t->entries[i].val, ctx);
    }
}
