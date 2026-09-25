#include "mapobj.h"

#include <stdlib.h>
#include <string.h>

#define BITS 5
#define FANOUT 32
#define MAX_DEPTH 12 /* 12 levels use 60 of the 64 hash bits; below that, entries with equal hashes share a list */

typedef struct Leaf {
    uint64_t hash;
    Value key;
    Value val;
    int64_t seq;
} Leaf;

typedef struct Slot {
    Leaf *leaf;
    struct MapNode *child;
} Slot;

struct MapNode {
    uint32_t bitmap;
    int n;
    Slot slots[];
};

static MapNode *node_alloc(int n) {
    MapNode *node = calloc(1, sizeof(MapNode) + (size_t)n * sizeof(Slot)); /* zeroed: a slot holds a leaf or a child, never both */
    node->bitmap = 0;
    node->n = n;
    return node;
}

static MapNode *node_copy(const MapNode *src) {
    MapNode *node = node_alloc(src->n);
    node->bitmap = src->bitmap;
    memcpy(node->slots, src->slots, (size_t)src->n * sizeof(Slot));
    return node;
}

/* value_hash is the identity for ints; spread the bits so consecutive or aligned keys
 * do not all land in the same trie slot. */
static uint64_t map_hash(Value key) {
    uint64_t h = value_hash(key);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static int slot_pos(uint32_t bitmap, uint32_t bit) {
    return __builtin_popcount(bitmap & (bit - 1));
}

static uint32_t bit_at(uint64_t hash, int depth) {
    return 1u << ((hash >> (depth * BITS)) & (FANOUT - 1));
}

static int keys_equal(const Leaf *a, uint64_t hash, Value key) {
    return a->hash == hash && value_equals(a->key, key);
}

MapObj *mapobj_empty(void) {
    MapObj *m = malloc(sizeof(MapObj));
    m->root = node_alloc(0);
    m->count = 0;
    m->next_seq = 0;
    return m;
}

int mapobj_get(const MapObj *m, Value key, Value *out) {
    uint64_t hash = map_hash(key);
    const MapNode *node = m->root;
    for (int depth = 0;; depth++) {
        if (depth >= MAX_DEPTH) {
            for (int i = 0; i < node->n; i++) {
                if (keys_equal(node->slots[i].leaf, hash, key)) {
                    *out = node->slots[i].leaf->val;
                    return 1;
                }
            }
            return 0;
        }
        uint32_t bit = bit_at(hash, depth);
        if (!(node->bitmap & bit)) return 0;
        const Slot *s = &node->slots[slot_pos(node->bitmap, bit)];
        if (s->child != NULL) {
            node = s->child;
            continue;
        }
        if (keys_equal(s->leaf, hash, key)) {
            *out = s->leaf->val;
            return 1;
        }
        return 0;
    }
}

/* A node holding two entries whose hashes agree on every level above `depth`. */
static MapNode *make_pair(Leaf *a, Leaf *b, int depth) {
    if (depth >= MAX_DEPTH) {
        MapNode *node = node_alloc(2);
        node->slots[0].leaf = a;
        node->slots[1].leaf = b;
        return node;
    }
    uint32_t bit_a = bit_at(a->hash, depth), bit_b = bit_at(b->hash, depth);
    if (bit_a == bit_b) {
        MapNode *node = node_alloc(1);
        node->bitmap = bit_a;
        node->slots[0].child = make_pair(a, b, depth + 1);
        return node;
    }
    MapNode *node = node_alloc(2);
    node->bitmap = bit_a | bit_b;
    Leaf *first = bit_a < bit_b ? a : b, *second = bit_a < bit_b ? b : a;
    node->slots[0].leaf = first;
    node->slots[1].leaf = second;
    return node;
}

static MapNode *node_insert(const MapNode *node, int depth, Leaf *nl, int *added) {
    if (depth >= MAX_DEPTH) {
        for (int i = 0; i < node->n; i++) {
            if (keys_equal(node->slots[i].leaf, nl->hash, nl->key)) {
                nl->seq = node->slots[i].leaf->seq;
                MapNode *copy = node_copy(node);
                copy->slots[i].leaf = nl;
                *added = 0;
                return copy;
            }
        }
        MapNode *grown = node_alloc(node->n + 1);
        memcpy(grown->slots, node->slots, (size_t)node->n * sizeof(Slot));
        grown->slots[node->n].leaf = nl;
        *added = 1;
        return grown;
    }
    uint32_t bit = bit_at(nl->hash, depth);
    int pos = slot_pos(node->bitmap, bit);
    if (!(node->bitmap & bit)) {
        MapNode *grown = node_alloc(node->n + 1);
        grown->bitmap = node->bitmap | bit;
        memcpy(grown->slots, node->slots, (size_t)pos * sizeof(Slot));
        grown->slots[pos].leaf = nl;
        memcpy(grown->slots + pos + 1, node->slots + pos, (size_t)(node->n - pos) * sizeof(Slot));
        *added = 1;
        return grown;
    }
    const Slot *s = &node->slots[pos];
    MapNode *copy = node_copy(node);
    if (s->child != NULL) {
        copy->slots[pos].child = node_insert(s->child, depth + 1, nl, added);
        return copy;
    }
    if (keys_equal(s->leaf, nl->hash, nl->key)) {
        nl->seq = s->leaf->seq;
        copy->slots[pos].leaf = nl;
        *added = 0;
        return copy;
    }
    copy->slots[pos].leaf = NULL;
    copy->slots[pos].child = make_pair(s->leaf, nl, depth + 1);
    *added = 1;
    return copy;
}

MapObj *mapobj_set(const MapObj *m, Value key, Value val) {
    Leaf *nl = malloc(sizeof(Leaf));
    nl->hash = map_hash(key);
    nl->key = key;
    nl->val = val;
    nl->seq = m->next_seq;
    int added = 0;
    MapObj *out = malloc(sizeof(MapObj));
    out->root = node_insert(m->root, 0, nl, &added);
    out->count = m->count + added;
    out->next_seq = m->next_seq + added;
    return out;
}

static MapNode *node_without(const MapNode *node, int pos, uint32_t bit_to_clear) {
    MapNode *shrunk = node_alloc(node->n - 1);
    shrunk->bitmap = node->bitmap & ~bit_to_clear;
    memcpy(shrunk->slots, node->slots, (size_t)pos * sizeof(Slot));
    memcpy(shrunk->slots + pos, node->slots + pos + 1, (size_t)(node->n - pos - 1) * sizeof(Slot));
    return shrunk;
}

static const MapNode *node_remove(const MapNode *node, int depth, uint64_t hash, Value key, int *removed) {
    *removed = 0;
    if (depth >= MAX_DEPTH) {
        for (int i = 0; i < node->n; i++) {
            if (keys_equal(node->slots[i].leaf, hash, key)) {
                *removed = 1;
                return node_without(node, i, 0);
            }
        }
        return node;
    }
    uint32_t bit = bit_at(hash, depth);
    if (!(node->bitmap & bit)) return node;
    int pos = slot_pos(node->bitmap, bit);
    const Slot *s = &node->slots[pos];
    if (s->child == NULL) {
        if (!keys_equal(s->leaf, hash, key)) return node;
        *removed = 1;
        return node_without(node, pos, bit);
    }
    const MapNode *child = node_remove(s->child, depth + 1, hash, key, removed);
    if (!*removed) return node;
    if (child->n == 0) return node_without(node, pos, bit);
    MapNode *copy = node_copy(node);
    if (child->n == 1 && child->slots[0].leaf != NULL) {
        copy->slots[pos].leaf = child->slots[0].leaf;
        copy->slots[pos].child = NULL;
    } else {
        copy->slots[pos].child = (MapNode *)child;
    }
    return copy;
}

MapObj *mapobj_remove(const MapObj *m, Value key) {
    int removed = 0;
    const MapNode *root = node_remove(m->root, 0, map_hash(key), key, &removed);
    if (!removed) return (MapObj *)m;
    MapObj *out = malloc(sizeof(MapObj));
    out->root = (MapNode *)root;
    out->count = m->count - 1;
    out->next_seq = m->next_seq;
    return out;
}

static void collect(const MapNode *node, Leaf **out, int *n) {
    for (int i = 0; i < node->n; i++) {
        if (node->slots[i].child != NULL) collect(node->slots[i].child, out, n);
        else out[(*n)++] = node->slots[i].leaf;
    }
}

static int by_seq(const void *a, const void *b) {
    int64_t x = (*(Leaf *const *)a)->seq, y = (*(Leaf *const *)b)->seq;
    return x < y ? -1 : (x > y ? 1 : 0);
}

void mapobj_entries(const MapObj *m, Value **keys, Value **vals) {
    int count = m->count;
    Leaf **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(Leaf *));
    int n = 0;
    collect(m->root, leaves, &n);
    qsort(leaves, (size_t)n, sizeof(Leaf *), by_seq);
    if (keys != NULL) *keys = malloc((size_t)(count > 0 ? count : 1) * sizeof(Value));
    if (vals != NULL) *vals = malloc((size_t)(count > 0 ? count : 1) * sizeof(Value));
    for (int i = 0; i < n; i++) {
        if (keys != NULL) (*keys)[i] = leaves[i]->key;
        if (vals != NULL) (*vals)[i] = leaves[i]->val;
    }
}

/* ---- in-place mutation, for MapBuilder / SetBuilder ------------------------------------
 * A builder owns a MapObj nobody else can see, so it may change nodes in place: growing a
 * node is a realloc (which frees the old block), not a copy. Leaves are the one thing a
 * snapshot shares with the builder; while `shared` is set an overwrite replaces the leaf
 * instead of updating it. */
static void node_insert_mut(MapNode **np, int depth, uint64_t hash, Value key, Value val, int64_t seq, int shared, int *added) {
    MapNode *node = *np;
    if (depth >= MAX_DEPTH) {
        for (int i = 0; i < node->n; i++) {
            Leaf *old = node->slots[i].leaf;
            if (keys_equal(old, hash, key)) {
                if (shared) {
                    Leaf *nl = malloc(sizeof(Leaf));
                    *nl = *old;
                    nl->val = val;
                    node->slots[i].leaf = nl;
                } else {
                    old->val = val;
                }
                *added = 0;
                return;
            }
        }
        node = realloc(node, sizeof(MapNode) + (size_t)(node->n + 1) * sizeof(Slot));
        Leaf *nl = malloc(sizeof(Leaf));
        *nl = (Leaf){hash, key, val, seq};
        node->slots[node->n].leaf = nl;
        node->slots[node->n].child = NULL;
        node->n++;
        *np = node;
        *added = 1;
        return;
    }
    uint32_t bit = bit_at(hash, depth);
    int pos = slot_pos(node->bitmap, bit);
    if (!(node->bitmap & bit)) {
        node = realloc(node, sizeof(MapNode) + (size_t)(node->n + 1) * sizeof(Slot));
        memmove(node->slots + pos + 1, node->slots + pos, (size_t)(node->n - pos) * sizeof(Slot));
        Leaf *nl = malloc(sizeof(Leaf));
        *nl = (Leaf){hash, key, val, seq};
        node->slots[pos].leaf = nl;
        node->slots[pos].child = NULL;
        node->bitmap |= bit;
        node->n++;
        *np = node;
        *added = 1;
        return;
    }
    Slot *s = &node->slots[pos];
    if (s->child != NULL) {
        node_insert_mut(&s->child, depth + 1, hash, key, val, seq, shared, added);
        return;
    }
    Leaf *old = s->leaf;
    if (keys_equal(old, hash, key)) {
        if (shared) {
            Leaf *nl = malloc(sizeof(Leaf));
            *nl = *old;
            nl->val = val;
            s->leaf = nl;
        } else {
            old->val = val;
        }
        *added = 0;
        return;
    }
    Leaf *nl = malloc(sizeof(Leaf));
    *nl = (Leaf){hash, key, val, seq};
    s->leaf = NULL;
    s->child = make_pair(old, nl, depth + 1);
    *added = 1;
}

MapObj *mapobj_mut_new(void) {
    return mapobj_empty();
}

void mapobj_set_mut(MapObj *m, Value key, Value val, int shared) {
    int added = 0;
    node_insert_mut(&m->root, 0, map_hash(key), key, val, m->next_seq, shared, &added);
    m->count += added;
    m->next_seq += added;
}

static int node_remove_mut(MapNode *node, int depth, uint64_t hash, Value key) {
    if (depth >= MAX_DEPTH) {
        for (int i = 0; i < node->n; i++) {
            if (keys_equal(node->slots[i].leaf, hash, key)) {
                memmove(node->slots + i, node->slots + i + 1, (size_t)(node->n - i - 1) * sizeof(Slot));
                node->n--;
                return 1;
            }
        }
        return 0;
    }
    uint32_t bit = bit_at(hash, depth);
    if (!(node->bitmap & bit)) return 0;
    int pos = slot_pos(node->bitmap, bit);
    Slot *s = &node->slots[pos];
    if (s->child == NULL) {
        if (!keys_equal(s->leaf, hash, key)) return 0;
    } else {
        if (!node_remove_mut(s->child, depth + 1, hash, key)) return 0;
        if (s->child->n > 0) return 1;
    }
    memmove(node->slots + pos, node->slots + pos + 1, (size_t)(node->n - pos - 1) * sizeof(Slot));
    node->n--;
    node->bitmap &= ~bit;
    return 1;
}

void mapobj_remove_mut(MapObj *m, Value key) {
    if (node_remove_mut(m->root, 0, map_hash(key), key)) m->count--;
}

static MapNode *copy_tree(const MapNode *node) {
    MapNode *copy = node_copy(node);
    for (int i = 0; i < node->n; i++) {
        if (node->slots[i].child != NULL) copy->slots[i].child = copy_tree(node->slots[i].child);
    }
    return copy;
}

/* An independent immutable MapObj with the builder's entries. Leaves are shared, so the
 * caller must treat the builder's leaves as shared from now on. */
MapObj *mapobj_snapshot(const MapObj *m) {
    MapObj *out = malloc(sizeof(MapObj));
    out->root = copy_tree(m->root);
    out->count = m->count;
    out->next_seq = m->next_seq;
    return out;
}

