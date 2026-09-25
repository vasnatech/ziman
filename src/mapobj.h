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

#endif
