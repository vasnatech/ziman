#ifndef ZIMAN_MAPOBJ_H
#define ZIMAN_MAPOBJ_H

#include "value.h"

/* The storage behind Map<K, V> and Set<T> (a set is a map whose values are unused).
 *
 * A persistent hash trie: 32-way, 5 hash bits per level, bitmap-compressed nodes. Like
 * ArrayObj it is immutable once built -- set/remove copy only the nodes on the path to
 * the changed entry (O(log32 n), in practice a handful) and share the rest, so a Value
 * can share a MapObj by pointer copy and `m = set(m, k, v)` stays cheap. Each entry
 * carries the sequence number it was first inserted with; iteration sorts by it, which
 * is what makes a map's order the insertion order. */
typedef struct MapNode MapNode;
typedef struct MapObj {
    MapNode *root;
    int count;
    int64_t next_seq;
} MapObj;

MapObj *mapobj_empty(void);
/* 1 and *out set when `key` is present. */
int mapobj_get(const MapObj *m, Value key, Value *out);
MapObj *mapobj_set(const MapObj *m, Value key, Value val);
MapObj *mapobj_remove(const MapObj *m, Value key);
/* Every entry in insertion order: fresh arrays of `count` keys and values (either
 * pointer may be NULL to skip it). */
void mapobj_entries(const MapObj *m, Value **keys, Value **vals);

/* In-place mutation for a builder's private MapObj (see mapobj.c). `shared` is set once a
 * snapshot has been taken, since a snapshot shares the leaves. */
MapObj *mapobj_mut_new(void);
void mapobj_set_mut(MapObj *m, Value key, Value val, int shared);
void mapobj_remove_mut(MapObj *m, Value key);
MapObj *mapobj_snapshot(const MapObj *m);

/* Calls `visit(leaf, key, val, ctx)` once for every live entry in `m`'s own trie, in no particular
 * order (unlike mapobj_entries, nothing here needs insertion order) -- for gc_mark.c's own mark
 * phase, which needs two things mapobj_entries's flat Value arrays alone can't give it: the key/val
 * Values to mark (as before), and `leaf` itself, the gc_heap-allocated Leaf's own opaque address, to
 * set its own header's mark bit. `Leaf` is private to this file -- gc_mark.c never sees its own
 * layout, only this opaque pointer, which `gc_set_marked`/`is_gc_heap_object`-style header
 * operations need nothing more than. A Leaf may be shared across more than one MapObj (persistent
 * structural sharing, this header's own top comment) and so visited once per MapObj it's reachable
 * through within one collection cycle -- harmless, idempotent, the same accepted redundant-work
 * shape every other kind's own marking already has for shared substructure. */
void mapobj_mark_leaves(const MapObj *m, void (*visit)(const void *leaf, Value key, Value val, void *ctx), void *ctx);

/* The exact same shape as mapobj_empty/mapobj_set, but the MapObj wrapper and the Leaf it builds are
 * plain `malloc`'d -- which a `ZIMAN_GC` build's own zgc.h routes to `GC_MALLOC`, i.e. Boehm's own
 * shared heap, belonging to no thread in particular -- rather than coming from the calling thread's
 * own gc_heap. For value_copy.c's own crossing construction, the identical reason its own
 * `ordinary_*` counterparts exist instead of calling struct_obj_new/array_obj_new directly: a value
 * crossing a spawn/Channel boundary must not end up living in a heap that the sending thread might
 * destroy (ziman_thread_heap_release, at that thread's own exit) before the receiving side has had a
 * chance to copy it out. That is the whole requirement -- *not* thread-tied -- and Boehm's own heap
 * meets it directly, while being traced precisely through whatever references the copy (a
 * `ChannelMsg` in a channel's own `malloc`'d queue, a `SpawnArgs`) instead of needing the copy's
 * memory registered as a root range. See value_copy.h for the full reasoning and what it replaced.
 *
 * MapNode itself is untouched either way (still calloc'd/realloc'd, via the existing
 * node_alloc/node_insert this file already has) -- only the wrapper and the leaf, the two kinds that
 * actually migrated to a thread-tied allocator, need an alternate destination at all.
 *
 * A Map/Set built this way has no `gc_alloc` header, so gc_mark.c never touches a mark bit on it:
 * `gc_mark_map` and `mark_leaf` are both already `is_gc_heap_object`-gated and both already walk
 * entries regardless, exactly as they do for any other not-yet-migrated object. */
MapObj *mapobj_empty_crossing(void);
MapObj *mapobj_set_crossing(const MapObj *m, Value key, Value val);

#endif
