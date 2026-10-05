#include "value_copy.h"

#include <stdlib.h>
#include <string.h>

#include "mapobj.h"

/* Every allocation below is a plain `malloc` -- `GC_MALLOC` under zgc.h, Boehm's own shared heap,
 * belonging to no thread in particular. That is the whole requirement (see value_copy.h): a crossing
 * copy must not live in a heap its *sending* thread may destroy at exit, and Boehm's heap is not
 * tied to any thread. Nothing here needs rooting of its own: the copy is reachable, and traced
 * precisely, through whatever holds it -- a `ChannelMsg` in a channel's own `malloc`'d queue, or a
 * `SpawnArgs` the new thread is started with -- for exactly the reason `arena_enum_copy`'s
 * payload-less branch already relied on before any of the rest of this did. */

static Value crossing_string_copy(const char *ptr, size_t len) {
    char *buf = malloc(len + 1);
    memcpy(buf, ptr, len);
    buf[len] = '\0';
    Value v;
    v.type = type_scalar(TY_STRING);
    v.as.str.ptr = buf;
    v.as.str.len = (uint32_t)len;
    v.as.str.term = 1;
    return v;
}

/* `n`'s digit array duplicated; `n` itself is a plain struct, passed by value, so only the
 * heap-allocated `digits` underneath is aliased until this runs. `count == 0` means no digits at
 * all (see bignum.h), so nothing to duplicate and nothing to allocate. */
static BigNum crossing_bignum_copy(BigNum n) {
    if (n.count > 0) {
        uint32_t *digits = malloc((size_t)n.count * sizeof(uint32_t));
        memcpy(digits, n.digits, (size_t)n.count * sizeof(uint32_t));
        n.digits = digits;
    }
    return n;
}

static Value crossing_bigint(BigNum n) {
    BigNum *boxed = malloc(sizeof(BigNum));
    *boxed = crossing_bignum_copy(n);
    Value v;
    v.type = type_scalar(TY_BIGINT);
    v.as.bi = boxed;
    return v;
}

static Value crossing_decimal(Decimal d) {
    d.unscaled = crossing_bignum_copy(d.unscaled);
    Decimal *boxed = malloc(sizeof(Decimal));
    *boxed = d;
    Value v;
    v.type = type_scalar(TY_DECIMAL);
    v.as.dec = boxed;
    return v;
}

static Value value_deep_copy_crossing(Value v);

static Value crossing_array_copy(ArrayObj *a, Type array_type) {
    /* Copied element by element into a plain boxed array regardless of whether `a` was packed --
     * packed-vs-boxed is purely an internal storage optimisation, invisible to any program (see
     * docs/SYNTAX.md's Builtins section), so a value that crosses a thread coming back boxed is not
     * a correctness change, only a missed optimisation for the rare (thread-crossing) case. `store`
     * (the spare capacity `push` shares between snapshots of the same growing array) is deliberately
     * dropped: it is tied to one thread's own compare-and-swap sequence of pushes, and a copy on
     * another thread sharing it would mean two independently-managed heaps racing a CAS over memory
     * only one of them will end up owning. */
    Value *items = a->count > 0 ? malloc((size_t)a->count * sizeof(Value)) : NULL;
    for (int i = 0; i < a->count; i++) {
        items[i] = value_deep_copy_crossing(array_get(a, i));
    }
    ArrayObj *copy = malloc(sizeof(ArrayObj));
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

static Value crossing_struct_copy(StructObj *st, Type struct_type) {
    Value *fields = st->count > 0 ? malloc((size_t)st->count * sizeof(Value)) : NULL;
    for (int i = 0; i < st->count; i++) {
        fields[i] = value_deep_copy_crossing(st->fields[i]);
    }
    StructObj *copy = malloc(sizeof(StructObj));
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

static Value crossing_result_copy(ResultObj *res, Type result_type) {
    ResultObj *copy = malloc(sizeof(ResultObj));
    copy->is_ok = res->is_ok;
    copy->value = value_deep_copy_crossing(res->value);
    Value v;
    v.type = result_type;
    v.as.res = copy;
    return v;
}

static Value crossing_enum_copy(EnumObj *en, Type enum_type) {
    EnumObj *copy = malloc(sizeof(EnumObj));
    /* `payload` is unused when there is none to copy, but copying it across unconditionally keeps
     * this branch-free and matches what `value_copy_receive`'s own `ordinary_enum_copy` does. */
    copy->payload = en->has_payload ? value_deep_copy_crossing(en->payload) : en->payload;
    /* enum_name/variant_name borrowed from the EnumDecl, same reasoning as field_names above. */
    copy->enum_name = en->enum_name;
    copy->variant_name = en->variant_name;
    copy->has_payload = en->has_payload;
    Value v;
    v.type = enum_type;
    v.as.en = copy;
    return v;
}

static Value crossing_closure_copy(ClosureObj *clo, Type func_type) {
    Value *captured = clo->ncaptured > 0 ? malloc((size_t)clo->ncaptured * sizeof(Value)) : NULL;
    for (int i = 0; i < clo->ncaptured; i++) {
        captured[i] = value_deep_copy_crossing(clo->captured[i]);
    }
    ClosureObj *copy = malloc(sizeof(ClosureObj));
    /* fn is a FunctionDecl*: part of the AST, never freed, shared same as everywhere else. */
    copy->fn = clo->fn;
    copy->ncaptured = clo->ncaptured;
    copy->captured = captured;
    Value v;
    v.type = func_type;
    v.as.clo = copy;
    return v;
}

static Value value_deep_copy_crossing(Value v) {
    switch (v.type.kind) {
        case TY_STRING:
            return crossing_string_copy(v.as.str.ptr, v.as.str.len);
        case TY_BIGINT:
            return crossing_bigint(*v.as.bi);
        case TY_DECIMAL:
            return crossing_decimal(*v.as.dec);
        case TY_ARRAY:
            return crossing_array_copy(v.as.arr, v.type);
        case TY_STRUCT:
            return crossing_struct_copy(v.as.st, v.type);
        case TY_RESULT:
            return crossing_result_copy(v.as.res, v.type);
        case TY_ENUM:
            return crossing_enum_copy(v.as.en, v.type);
        case TY_FUNC:
            return crossing_closure_copy(v.as.clo, v.type);
        case TY_MAP:
        case TY_SET: {
            /* mapobj_empty_crossing/mapobj_set_crossing (mapobj.h), not mapobj_empty/mapobj_set:
             * the wrapper and each entry's own Leaf are gc_heap-migrated now, so building them
             * through the ordinary mapobj_empty/mapobj_set (always this thread's own gc_heap) would
             * leave them living in a heap the sending thread might destroy at its own exit
             * (ziman_thread_heap_release) before the receiving side ever gets to copy them out --
             * found exactly that way, a spawned thread sending a Map and exiting right after
             * segfaulting the receiver. MapNode itself is untouched either way (the crossing pair
             * still calls the same node_insert everything else does -- it was never part of this
             * problem, see mapobj.h's own comment). */
            Value *keys, *vals;
            mapobj_entries(v.as.map, &keys, &vals);
            MapObj *copy = mapobj_empty_crossing();
            for (int i = 0; i < v.as.map->count; i++) {
                copy = mapobj_set_crossing(copy, value_deep_copy_crossing(keys[i]), value_deep_copy_crossing(vals[i]));
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

Value value_copy_send(Value v) {
    return value_deep_copy_crossing(v);
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
