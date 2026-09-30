#include "value_copy.h"

#include <stdlib.h>
#include <string.h>

#include "mapobj.h"

/* Creates `*slot` on first use, reuses it on every later call within the same `value_copy_send`
 * "batch" (see value_copy.h's own comment on several values sharing one arena) -- this is what
 * makes sending a scalar or a `Channel` free of any arena/`mmap`/`GC_add_roots` cost at all: nothing
 * below ever calls this unless it is actually about to `arena_alloc` something. */
static Arena *lazy_arena(Arena **slot) {
    if (*slot == NULL) *slot = arena_create_scanned();
    return *slot;
}

static Value arena_string_copy(Arena **slot, const char *ptr, size_t len) {
    char *buf = arena_alloc(lazy_arena(slot), len + 1);
    memcpy(buf, ptr, len);
    buf[len] = '\0';
    Value v;
    v.type = type_scalar(TY_STRING);
    v.as.str.ptr = buf;
    v.as.str.len = (uint32_t)len;
    v.as.str.term = 1;
    return v;
}

/* `n`'s digit array duplicated into `*slot`; `n` itself is a plain struct, passed by value, so only
 * the heap-allocated `digits` underneath is aliased until this runs. `count == 0` means no digits at
 * all (see bignum.h), so nothing to duplicate -- and, notably, no `lazy_arena` call either. */
static BigNum arena_bignum_copy(Arena **slot, BigNum n) {
    if (n.count > 0) {
        uint32_t *digits = arena_alloc(lazy_arena(slot), (size_t)n.count * sizeof(uint32_t));
        memcpy(digits, n.digits, (size_t)n.count * sizeof(uint32_t));
        n.digits = digits;
    }
    return n;
}

static Value arena_bigint(Arena **slot, BigNum n) {
    BigNum *boxed = arena_alloc(lazy_arena(slot), sizeof(BigNum));
    *boxed = arena_bignum_copy(slot, n);
    Value v;
    v.type = type_scalar(TY_BIGINT);
    v.as.bi = boxed;
    return v;
}

static Value arena_decimal(Arena **slot, Decimal d) {
    d.unscaled = arena_bignum_copy(slot, d.unscaled);
    Decimal *boxed = arena_alloc(lazy_arena(slot), sizeof(Decimal));
    *boxed = d;
    Value v;
    v.type = type_scalar(TY_DECIMAL);
    v.as.dec = boxed;
    return v;
}

static Value value_deep_copy_into(Arena **slot, Value v);

static Value arena_array_copy(Arena **slot, ArrayObj *a, Type array_type) {
    /* Copied element by element into a plain boxed array regardless of whether `a` was packed --
     * packed-vs-boxed is purely an internal storage optimisation, invisible to any program (see
     * docs/SYNTAX.md's Builtins section), so a value that crosses a thread coming back boxed is not
     * a correctness change, only a missed optimisation for the rare (thread-crossing) case. `store`
     * (the spare capacity `push` shares between snapshots of the same growing array) is deliberately
     * dropped: it is tied to one thread's own compare-and-swap sequence of pushes, and a copy on
     * another thread sharing it would mean two independently-managed heaps racing a CAS over memory
     * only one of them will end up owning. */
    Value *items = a->count > 0 ? arena_alloc(lazy_arena(slot), (size_t)a->count * sizeof(Value)) : NULL;
    for (int i = 0; i < a->count; i++) {
        items[i] = value_deep_copy_into(slot, array_get(a, i));
    }
    ArrayObj *copy = arena_alloc(lazy_arena(slot), sizeof(ArrayObj));
    copy->boxed_items = items;
    copy->bytes = NULL;
    copy->bools = NULL;
    copy->ints = NULL;
    copy->floats = NULL;
    copy->count = a->count;
    copy->store = NULL;
    Value v;
    v.type = array_type;
    v.as.arr = copy;
    return v;
}

static Value arena_struct_copy(Arena **slot, StructObj *st, Type struct_type) {
    Value *fields = st->count > 0 ? arena_alloc(lazy_arena(slot), (size_t)st->count * sizeof(Value)) : NULL;
    for (int i = 0; i < st->count; i++) {
        fields[i] = value_deep_copy_into(slot, st->fields[i]);
    }
    StructObj *copy = arena_alloc(lazy_arena(slot), sizeof(StructObj));
    /* field_names is borrowed from the StructDecl (part of the AST, never freed, never duplicated
     * per instance, same as every other copy of this struct) -- shared, not copied. */
    copy->field_names = st->field_names;
    copy->fields = fields;
    copy->count = st->count;
    Value v;
    v.type = struct_type;
    v.as.st = copy;
    return v;
}

static Value arena_result_copy(Arena **slot, ResultObj *res, Type result_type) {
    ResultObj *copy = arena_alloc(lazy_arena(slot), sizeof(ResultObj));
    copy->is_ok = res->is_ok;
    copy->value = value_deep_copy_into(slot, res->value);
    Value v;
    v.type = result_type;
    v.as.res = copy;
    return v;
}

static Value arena_enum_copy(Arena **slot, EnumObj *en, Type enum_type) {
    EnumObj *copy = arena_alloc(lazy_arena(slot), sizeof(EnumObj));
    /* enum_name/variant_name borrowed from the EnumDecl, same reasoning as field_names above. */
    copy->enum_name = en->enum_name;
    copy->variant_name = en->variant_name;
    copy->has_payload = en->has_payload;
    copy->payload = en->has_payload ? value_deep_copy_into(slot, en->payload) : en->payload;
    Value v;
    v.type = enum_type;
    v.as.en = copy;
    return v;
}

static Value arena_closure_copy(Arena **slot, ClosureObj *clo, Type func_type) {
    Value *captured = clo->ncaptured > 0 ? arena_alloc(lazy_arena(slot), (size_t)clo->ncaptured * sizeof(Value)) : NULL;
    for (int i = 0; i < clo->ncaptured; i++) {
        captured[i] = value_deep_copy_into(slot, clo->captured[i]);
    }
    ClosureObj *copy = arena_alloc(lazy_arena(slot), sizeof(ClosureObj));
    /* fn is a FunctionDecl*: part of the AST, never freed, shared same as everywhere else. */
    copy->fn = clo->fn;
    copy->ncaptured = clo->ncaptured;
    copy->captured = captured;
    Value v;
    v.type = func_type;
    v.as.clo = copy;
    return v;
}

static Value value_deep_copy_into(Arena **slot, Value v) {
    switch (v.type.kind) {
        case TY_STRING:
            return arena_string_copy(slot, v.as.str.ptr, v.as.str.len);
        case TY_BIGINT:
            return arena_bigint(slot, *v.as.bi);
        case TY_DECIMAL:
            return arena_decimal(slot, *v.as.dec);
        case TY_ARRAY:
            return arena_array_copy(slot, v.as.arr, v.type);
        case TY_STRUCT:
            return arena_struct_copy(slot, v.as.st, v.type);
        case TY_RESULT:
            return arena_result_copy(slot, v.as.res, v.type);
        case TY_ENUM:
            return arena_enum_copy(slot, v.as.en, v.type);
        case TY_FUNC:
            return arena_closure_copy(slot, v.as.clo, v.type);
        case TY_MAP:
        case TY_SET: {
            /* The trie itself is rebuilt through the ordinary allocator (mapobj_set/mapobj_empty),
             * not the arena -- MapObj is not yet arena-routed at all (a separate, later piece of
             * work, not a regression: this is exactly how it was allocated before this file
             * existed). A nested structural key or value still recurses through
             * value_deep_copy_into, so `*slot` can still end up non-NULL for a Map/Set whose own
             * entries hold one -- an all-scalar Map/Set costs no arena at all. */
            Value *keys, *vals;
            mapobj_entries(v.as.map, &keys, &vals);
            MapObj *copy = mapobj_empty();
            for (int i = 0; i < v.as.map->count; i++) {
                copy = mapobj_set(copy, value_deep_copy_into(slot, keys[i]), value_deep_copy_into(slot, vals[i]));
            }
            free(keys);
            free(vals);
            return value_map(v.type, copy);
        }
        case TY_CHANNEL:
            /* Deliberately shared by identity, not copied -- see value_copy.h's own comment. */
            return v;
        default:
            /* int, float, bool, byte, unit: embedded in the Value itself, nothing to copy. */
            return v;
    }
}

Value value_copy_send(Value v, Arena **arena_slot) {
    return value_deep_copy_into(arena_slot, v);
}

/* --- The receive side: copies out of whatever arena value_copy_send used, into the calling
 * thread's own ordinary allocators, mirroring the exact same per-kind shape above with `arena_alloc`
 * swapped for the real constructors (value_string_copy, struct_obj_new, array_obj_new, ...) every
 * *non*-crossing construction already uses -- so a received value becomes, from this thread's own
 * perspective, indistinguishable from one it built itself: correctly participating in its own
 * gc_heap's future collections for the kinds already migrated there, and in Boehm's ordinary
 * GC_MALLOC tracking for the kinds that aren't yet. */

static Value value_copy_ordinary_into(Value v);

static BigNum ordinary_bignum_copy(BigNum n) {
    if (n.count > 0) {
        uint32_t *digits = malloc((size_t)n.count * sizeof(uint32_t));
        memcpy(digits, n.digits, (size_t)n.count * sizeof(uint32_t));
        n.digits = digits;
    }
    return n;
}

static Value ordinary_array_copy(ArrayObj *a, Type array_type) {
    Value *items = a->count > 0 ? malloc((size_t)a->count * sizeof(Value)) : NULL;
    for (int i = 0; i < a->count; i++) {
        items[i] = value_copy_ordinary_into(array_get(a, i));
    }
    Value v;
    v.type = array_type;
    v.as.arr = array_obj_new(items, a->count);
    return v;
}

static Value ordinary_struct_copy(StructObj *st, Type struct_type) {
    Value *fields = st->count > 0 ? malloc((size_t)st->count * sizeof(Value)) : NULL;
    for (int i = 0; i < st->count; i++) {
        fields[i] = value_copy_ordinary_into(st->fields[i]);
    }
    Value v;
    v.type = struct_type;
    v.as.st = struct_obj_new(st->field_names, fields, st->count);
    return v;
}

static Value ordinary_result_copy(ResultObj *res, Type result_type) {
    Value v;
    v.type = result_type;
    v.as.res = result_obj_new(res->is_ok, value_copy_ordinary_into(res->value));
    return v;
}

static Value ordinary_enum_copy(EnumObj *en, Type enum_type) {
    Value payload = en->has_payload ? value_copy_ordinary_into(en->payload) : en->payload;
    Value v;
    v.type = enum_type;
    v.as.en = enum_obj_new(en->enum_name, en->variant_name, en->has_payload, payload);
    return v;
}

static Value ordinary_closure_copy(ClosureObj *clo, Type func_type) {
    Value *captured = clo->ncaptured > 0 ? malloc((size_t)clo->ncaptured * sizeof(Value)) : NULL;
    for (int i = 0; i < clo->ncaptured; i++) {
        captured[i] = value_copy_ordinary_into(clo->captured[i]);
    }
    /* Built by hand, not value_closure: that constructor recomputes .type from fn/ncaptured, which
     * need not agree with func_type's own (possibly already-substituted) type if this closure came
     * from a generic instantiation -- preserving func_type directly, as arena_closure_copy already
     * does, is the one thing that must not change between the two directions. */
    ClosureObj *copy = malloc(sizeof(ClosureObj));
    copy->fn = clo->fn;
    copy->ncaptured = clo->ncaptured;
    copy->captured = captured;
    Value v;
    v.type = func_type;
    v.as.clo = copy;
    return v;
}

static Value value_copy_ordinary_into(Value v) {
    switch (v.type.kind) {
        case TY_STRING:
            return value_string_copy(v.as.str.ptr, v.as.str.len);
        case TY_BIGINT:
            return value_bigint(ordinary_bignum_copy(*v.as.bi));
        case TY_DECIMAL: {
            Decimal d = *v.as.dec;
            d.unscaled = ordinary_bignum_copy(d.unscaled);
            return value_decimal(d);
        }
        case TY_ARRAY:
            return ordinary_array_copy(v.as.arr, v.type);
        case TY_STRUCT:
            return ordinary_struct_copy(v.as.st, v.type);
        case TY_RESULT:
            return ordinary_result_copy(v.as.res, v.type);
        case TY_ENUM:
            return ordinary_enum_copy(v.as.en, v.type);
        case TY_FUNC:
            return ordinary_closure_copy(v.as.clo, v.type);
        case TY_MAP:
        case TY_SET: {
            Value *keys, *vals;
            mapobj_entries(v.as.map, &keys, &vals);
            MapObj *copy = mapobj_empty();
            for (int i = 0; i < v.as.map->count; i++) {
                copy = mapobj_set(copy, value_copy_ordinary_into(keys[i]), value_copy_ordinary_into(vals[i]));
            }
            free(keys);
            free(vals);
            return value_map(v.type, copy);
        }
        case TY_CHANNEL:
            return v;
        default:
            return v;
    }
}

Value value_copy_receive(Value v) {
    return value_copy_ordinary_into(v);
}
