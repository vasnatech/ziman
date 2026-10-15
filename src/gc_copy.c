#include "gc_copy.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gc_object.h"
#include "mapobj.h"
#include "xalloc.h"

/* Insert-only, single-threaded open addressing, same hash and probe scheme as src/handles.c's
 * HandleSet -- no removal and no lock needed here, since a GcDedup lives for exactly one copy pass
 * on the one thread running it, then is destroyed. Plain malloc'd (not arena_alloc'd): this table
 * is bookkeeping about the copy, not part of the copied graph, and is garbage the moment the pass
 * finishes. */
struct GcDedup {
    const void **keys;
    void **vals;
    size_t capacity;
    size_t used;
};

static size_t hash_ptr(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x;
}

GcDedup *gc_dedup_create(void) {
    GcDedup *d = ziman_xmalloc(sizeof(GcDedup));
    d->keys = NULL;
    d->vals = NULL;
    d->capacity = 0;
    d->used = 0;
    return d;
}

void gc_dedup_destroy(GcDedup *d) {
    free(d->keys);
    free(d->vals);
    free(d);
}

static void dedup_grow(GcDedup *d) {
    size_t new_capacity = d->capacity == 0 ? 64 : d->capacity * 2;
    const void **fresh_keys = ziman_xcalloc(new_capacity, sizeof(void *));
    void **fresh_vals = ziman_xcalloc(new_capacity, sizeof(void *));
    for (size_t i = 0; i < d->capacity; i++) {
        if (d->keys[i] == NULL) continue;
        size_t at = hash_ptr(d->keys[i]) & (new_capacity - 1);
        while (fresh_keys[at] != NULL) at = (at + 1) & (new_capacity - 1);
        fresh_keys[at] = d->keys[i];
        fresh_vals[at] = d->vals[i];
    }
    free(d->keys);
    free(d->vals);
    d->keys = fresh_keys;
    d->vals = fresh_vals;
    d->capacity = new_capacity;
}

/* NULL when `key` has not been copied yet during this pass. */
static void *dedup_get(GcDedup *d, const void *key) {
    if (d->capacity == 0) return NULL;
    size_t at = hash_ptr(key) & (d->capacity - 1);
    while (d->keys[at] != NULL) {
        if (d->keys[at] == key) return d->vals[at];
        at = (at + 1) & (d->capacity - 1);
    }
    return NULL;
}

static void dedup_put(GcDedup *d, const void *key, void *val) {
    if (d->capacity == 0 || (d->used + 1) * 2 > d->capacity) dedup_grow(d);
    size_t at = hash_ptr(key) & (d->capacity - 1);
    while (d->keys[at] != NULL) at = (at + 1) & (d->capacity - 1);
    d->keys[at] = key;
    d->vals[at] = val;
    d->used++;
}

static Value gc_copy_into(Arena *dest, GcDedup *dedup, Value v);

/* Always a fresh copy of exactly `len` bytes from wherever `ptr` currently points -- never
 * deduplicated; see gc_copy.h's header comment on why a string can be an interior pointer. */
static Value gc_copy_string(Arena *dest, const char *ptr, size_t len) {
    char *buf = gc_alloc(dest, GC_ATOMIC, len + 1);
    memcpy(buf, ptr, len);
    buf[len] = '\0';
    Value v;
    v.type = type_scalar_ref(TY_STRING);
    v.as.str.ptr = buf;
    v.as.str.len = (uint32_t)len;
    v.as.str.term = 1;
    return v;
}

/* `n`'s digit array is always a whole, own-start allocation (every BigNum operation builds a fresh
 * one -- see bignum.h), never a view, so it would be safe to dedup; not worth the bookkeeping since
 * a bignum's digits are never independently reachable from two different places the way an
 * ArrayObj/StructObj can be. `count == 0` means no digits at all. */
static BigNum gc_copy_bignum_digits(Arena *dest, BigNum n) {
    if (n.count > 0) {
        uint32_t *digits = gc_alloc(dest, GC_ATOMIC, (size_t)n.count * sizeof(uint32_t));
        memcpy(digits, n.digits, (size_t)n.count * sizeof(uint32_t));
        n.digits = digits;
    }
    return n;
}

static Value gc_copy_bigint(Arena *dest, GcDedup *dedup, BigNum *src) {
    BigNum *cached = dedup_get(dedup, src);
    Value v;
    v.type = type_scalar_ref(TY_BIGINT);
    if (cached != NULL) {
        v.as.bi = cached;
        return v;
    }
    BigNum *copy = gc_alloc(dest, GC_ATOMIC, sizeof(BigNum));
    *copy = gc_copy_bignum_digits(dest, *src);
    dedup_put(dedup, src, copy);
    v.as.bi = copy;
    return v;
}

static Value gc_copy_decimal(Arena *dest, GcDedup *dedup, Decimal *src) {
    Decimal *cached = dedup_get(dedup, src);
    Value v;
    v.type = type_scalar_ref(TY_DECIMAL);
    if (cached != NULL) {
        v.as.dec = cached;
        return v;
    }
    Decimal *copy = gc_alloc(dest, GC_ATOMIC, sizeof(Decimal));
    *copy = *src;
    copy->unscaled = gc_copy_bignum_digits(dest, src->unscaled);
    dedup_put(dedup, src, copy);
    v.as.dec = copy;
    return v;
}

/* A Value[] buffer is always copied fresh, `count` elements, never deduplicated by its own address
 * -- ArrayObj.boxed_items can be an interior pointer (array_obj_view slices it at an offset), so
 * the same rule is applied uniformly to StructObj.fields/ClosureObj.captured too, even though
 * neither of those two is ever actually sliced in practice; one rule for every Value[] buffer is
 * simpler to reason about than tracking which call sites happen to be safe. */
static Value *gc_copy_value_array(Arena *dest, GcDedup *dedup, const Value *src, int count) {
    if (count <= 0) return NULL;
    Value *copy = gc_alloc(dest, GC_VALUES, (size_t)count * sizeof(Value));
    for (int i = 0; i < count; i++) {
        copy[i] = gc_copy_into(dest, dedup, src[i]);
    }
    return copy;
}

static Value gc_copy_array(Arena *dest, GcDedup *dedup, ArrayObj *a, const Type *array_type) {
    ArrayObj *cached = dedup_get(dedup, a);
    Value v;
    v.type = array_type;
    if (cached != NULL) {
        v.as.arr = cached;
        return v;
    }
    /* Read every element (array_get abstracts over packed-vs-boxed storage) before allocating the
     * copy's own Value[] buffer, then always land it boxed -- same reasoning and same "packed is
     * purely an internal optimisation" tradeoff src/value_copy.c already made. `store` is dropped
     * for the same reason it is there: it is tied to one thread's own compare-and-swap sequence of
     * pushes into a specific heap, not something a copy into a different arena can safely share. */
    ArrayObj *copy = gc_alloc(dest, GC_ARRAY, sizeof(ArrayObj));
    dedup_put(dedup, a, copy);
    Value *items = a->count > 0 ? gc_alloc(dest, GC_VALUES, (size_t)a->count * sizeof(Value)) : NULL;
    for (int i = 0; i < a->count; i++) {
        items[i] = gc_copy_into(dest, dedup, array_get(a, i));
    }
    copy->boxed_items = items;
    copy->bytes = NULL;
    copy->bools = NULL;
    copy->ints = NULL;
    copy->floats = NULL;
    copy->count = a->count;
    copy->store = NULL;
    v.as.arr = copy;
    return v;
}

static Value gc_copy_struct(Arena *dest, GcDedup *dedup, StructObj *st, const Type *struct_type) {
    StructObj *cached = dedup_get(dedup, st);
    Value v;
    v.type = struct_type;
    if (cached != NULL) {
        v.as.st = cached;
        return v;
    }
    StructObj *copy = gc_alloc(dest, GC_STRUCT, sizeof(StructObj));
    dedup_put(dedup, st, copy);
    /* field_names is borrowed from the StructDecl (part of the AST, permanent, never copied) --
     * shared, not copied, same as value_copy.c. */
    copy->field_names = st->field_names;
    copy->fields = gc_copy_value_array(dest, dedup, st->fields, st->count);
    copy->count = st->count;
    v.as.st = copy;
    return v;
}

static Value gc_copy_result(Arena *dest, GcDedup *dedup, ResultObj *res, const Type *result_type) {
    ResultObj *cached = dedup_get(dedup, res);
    Value v;
    v.type = result_type;
    if (cached != NULL) {
        v.as.res = cached;
        return v;
    }
    ResultObj *copy = gc_alloc(dest, GC_RESULT, sizeof(ResultObj));
    dedup_put(dedup, res, copy);
    copy->is_ok = res->is_ok;
    copy->value = gc_copy_into(dest, dedup, res->value);
    v.as.res = copy;
    return v;
}

static Value gc_copy_enum(Arena *dest, GcDedup *dedup, EnumObj *en, const Type *enum_type) {
    EnumObj *cached = dedup_get(dedup, en);
    Value v;
    v.type = enum_type;
    if (cached != NULL) {
        v.as.en = cached;
        return v;
    }
    EnumObj *copy = gc_alloc(dest, GC_ENUM, sizeof(EnumObj));
    dedup_put(dedup, en, copy);
    /* enum_name/variant_name borrowed from the EnumDecl, same reasoning as field_names above. */
    copy->enum_name = en->enum_name;
    copy->variant_name = en->variant_name;
    copy->has_payload = en->has_payload;
    copy->payload = en->has_payload ? gc_copy_into(dest, dedup, en->payload) : en->payload;
    v.as.en = copy;
    return v;
}

static Value gc_copy_closure(Arena *dest, GcDedup *dedup, ClosureObj *clo, const Type *func_type) {
    ClosureObj *cached = dedup_get(dedup, clo);
    Value v;
    v.type = func_type;
    if (cached != NULL) {
        v.as.clo = cached;
        return v;
    }
    ClosureObj *copy = gc_alloc(dest, GC_CLOSURE, sizeof(ClosureObj));
    dedup_put(dedup, clo, copy);
    /* fn is a FunctionDecl*: part of the AST, permanent, shared same as everywhere else. */
    copy->fn = clo->fn;
    copy->ncaptured = clo->ncaptured;
    copy->captured = gc_copy_value_array(dest, dedup, clo->captured, clo->ncaptured);
    v.as.clo = copy;
    return v;
}

static Value gc_copy_into(Arena *dest, GcDedup *dedup, Value v) {
    switch (v.type->kind) {
        case TY_STRING:
            return gc_copy_string(dest, v.as.str.ptr, v.as.str.len);
        case TY_BIGINT:
            return gc_copy_bigint(dest, dedup, v.as.bi);
        case TY_DECIMAL:
            return gc_copy_decimal(dest, dedup, v.as.dec);
        case TY_ARRAY:
            return gc_copy_array(dest, dedup, v.as.arr, v.type);
        case TY_STRUCT:
            return gc_copy_struct(dest, dedup, v.as.st, v.type);
        case TY_RESULT:
            return gc_copy_result(dest, dedup, v.as.res, v.type);
        case TY_ENUM:
            return gc_copy_enum(dest, dedup, v.as.en, v.type);
        case TY_FUNC:
            return gc_copy_closure(dest, dedup, v.as.clo, v.type);
        case TY_MAP:
        case TY_SET: {
            /* Not yet gc_alloc-routed -- same known gap as value_copy.c, not a new one (see
             * gc_copy.h). Rebuilt through the ordinary set-one-at-a-time API, in insertion order,
             * same approach and same reasoning as value_copy.c's own TY_MAP/TY_SET case. */
            Value *keys, *vals;
            mapobj_entries(v.as.map, &keys, &vals);
            MapObj *copy = mapobj_empty();
            for (int i = 0; i < v.as.map->count; i++) {
                copy = mapobj_set(copy, gc_copy_into(dest, dedup, keys[i]), gc_copy_into(dest, dedup, vals[i]));
            }
            free(keys);
            free(vals);
            return value_map(v.type, copy);
        }
        case TY_CHANNEL:
            /* Shared by identity, never copied -- same rule and same reason as value_copy.c. */
            return v;
        default:
            /* int, float, bool, byte, unit: embedded in the Value itself, nothing to copy. */
            return v;
    }
}

Value gc_copy_value(Arena *dest, GcDedup *dedup, Value v) {
    return gc_copy_into(dest, dedup, v);
}
