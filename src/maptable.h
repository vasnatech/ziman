#ifndef ZIMAN_MAPTABLE_H
#define ZIMAN_MAPTABLE_H

#include <stddef.h>
#include <stdint.h>

#include "value.h"

/* A flat hash table for `MutableMap`/`MutableSet`, in the shape CPython's `dict` uses: a sparse
 * array of indices into a dense array of entries kept in insertion order.
 *
 * Why not the persistent trie the builders share with `Map`/`Set` today. The trie is right for
 * `Map`, which is a *value*: `m = set(m, k, v)` must leave the old map intact, and a 32-way hash
 * trie copies only the nodes on the path and shares the rest. A builder needs none of that -- it is
 * `#[thread_bound]`, mutated in place, and its whole purpose is to be the mutable staging area --
 * so it was paying for persistence it does not use. The measurement that sent this here
 * (docs/ROADMAP.md, "Make `Map`/`Set` operations faster"): the trie allocates one `Leaf` per entry,
 * and per-insert cost therefore rises with the *live set* -- 135 ns/insert building to 1,000
 * entries, 149 to 10,000, 291 to 100,000 -- because a tracing collector prices a short-lived
 * allocation by what is alive. A dense entries array allocates nothing per entry, amortised, which
 * is the one lever that measurement identified.
 *
 * Insertion order comes free here, from the dense array, where the trie needed a sequence number on
 * every entry and a sort at iteration time.
 *
 * Deliberately generic over nothing: it stores `Value` and calls `value_hash`/`value_key_equals`
 * (value.h), so a NaN key is found exactly as `Map` finds one. Those two live in interpreter.c,
 * which is why tests/c/maptable_test.c can stub them and test this file without linking the
 * interpreter. */

typedef struct {
    uint64_t hash;
    Value key; /* key.type == NULL marks a dead entry: removed, not yet compacted away */
    Value val;
} MapTableEntry;

typedef struct MapTable {
    int32_t *indices;  /* index_cap slots: MAPTABLE_EMPTY, MAPTABLE_DUMMY, or an entries index */
    size_t index_cap;  /* a power of two, or 0 before the first insert */
    MapTableEntry *entries;
    size_t entry_used; /* slots taken in `entries`, dead ones included */
    size_t entry_cap;
    size_t live;       /* entries whose key is not NULL */
} MapTable;

void maptable_init(MapTable *t);
/* Releases both arrays. The table is reusable afterward (it is back to its initial state). */
void maptable_release(MapTable *t);

/* 1 and *out set when `key` is present. */
int maptable_get(const MapTable *t, Value key, Value *out);
/* 1 when `key` was not already there, 0 when this overwrote an existing value. */
int maptable_set(MapTable *t, Value key, Value val);
/* 1 and *out set when `key` was present (and is now gone). */
int maptable_remove(MapTable *t, Value key, Value *out);
size_t maptable_len(const MapTable *t);
void maptable_clear(MapTable *t);

/* Every live entry, in insertion order. `visit` must not modify the table. */
void maptable_visit(const MapTable *t, void (*visit)(Value key, Value val, void *ctx), void *ctx);

#endif
