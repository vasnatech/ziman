#include "gc_mark.h"

#include <stdlib.h>

#include "gc_lookup.h"
#include "gc_object.h"
#include "mapobj.h"

static void gc_mark_into(const Arena *a, Value v);

/* Resolves `ptr` (which may be an interior pointer -- a string view, an `array_obj_view` slice)
 * through `gc_lookup` to the enclosing object's own exact start, and marks *that* -- `gc_set_marked`
 * cannot be called on an interior address directly (see gc_object.h's own contract). A `NULL`
 * result would mean `ptr` doesn't resolve to anything `a` actually holds at all; harmless to just
 * skip, since there is nothing to mark either way. */
static void mark_interior_capable(const Arena *a, const void *ptr) {
    if (ptr == NULL) return;
    const void *obj = gc_lookup(a, ptr);
    if (obj != NULL) gc_set_marked((void *)obj, 1);
}

static void gc_mark_bignum_digits(BigNum n) {
    /* Always a whole, own-start allocation -- every BigNum operation builds a fresh one (see
     * bignum.h), never a view -- so a direct mark is correct and, unlike the interior-pointer-
     * capable cases above, needs no gc_lookup resolution at all (no `Arena *` parameter needed). */
    if (n.count > 0) gc_set_marked(n.digits, 1);
}

static void gc_mark_array(const Arena *a, ArrayObj *arr) {
    if (gc_is_marked(arr)) return;
    gc_set_marked(arr, 1);
    /* At most one of these is non-NULL (see ArrayObj's own doc comment); each is interior-pointer-
     * capable (array_obj_view slices whichever one is in use at an offset), so each goes through
     * mark_interior_capable, never a direct gc_set_marked. */
    mark_interior_capable(a, arr->boxed_items);
    mark_interior_capable(a, arr->bytes);
    mark_interior_capable(a, arr->bools);
    mark_interior_capable(a, arr->ints);
    mark_interior_capable(a, arr->floats);
    /* Packed elements (bytes/bools/ints/floats) are scalars -- nothing further to trace into them,
     * so only a boxed array's own elements need walking, each by its own Value.type.kind. */
    if (arr->boxed_items != NULL) {
        for (int i = 0; i < arr->count; i++) gc_mark_into(a, array_get(arr, i));
    }
}

static void gc_mark_struct(const Arena *a, StructObj *st) {
    if (gc_is_marked(st)) return;
    gc_set_marked(st, 1);
    /* field_names is borrowed from the StructDecl (permanent AST, never gc_alloc'd) -- nothing to
     * mark there. fields is a Value[] buffer, always its own whole allocation for a StructObj
     * (never sliced the way ArrayObj.boxed_items can be -- see gc_copy.h's own reasoning for why
     * the same uniform rule is still applied there anyway; marking tolerates either choice equally
     * well, since mark_interior_capable resolves an exact-start pointer to itself with no extra
     * cost beyond the lookup call itself). */
    mark_interior_capable(a, st->fields);
    for (int i = 0; i < st->count; i++) gc_mark_into(a, st->fields[i]);
}

static void gc_mark_result(const Arena *a, ResultObj *res) {
    if (gc_is_marked(res)) return;
    gc_set_marked(res, 1);
    gc_mark_into(a, res->value);
}

static void gc_mark_enum(const Arena *a, EnumObj *en) {
    if (gc_is_marked(en)) return;
    gc_set_marked(en, 1);
    /* enum_name/variant_name borrowed from the EnumDecl (permanent AST) -- nothing to mark. */
    if (en->has_payload) gc_mark_into(a, en->payload);
}

static void gc_mark_closure(const Arena *a, ClosureObj *clo) {
    if (gc_is_marked(clo)) return;
    gc_set_marked(clo, 1);
    /* fn is a FunctionDecl* -- permanent AST, nothing to mark. */
    mark_interior_capable(a, clo->captured);
    for (int i = 0; i < clo->ncaptured; i++) gc_mark_into(a, clo->captured[i]);
}

static void gc_mark_into(const Arena *a, Value v) {
    switch (v.type.kind) {
        case TY_STRING:
            mark_interior_capable(a, v.as.str.ptr);
            return;
        case TY_BIGINT:
            if (gc_is_marked(v.as.bi)) return;
            gc_set_marked(v.as.bi, 1);
            gc_mark_bignum_digits(*v.as.bi);
            return;
        case TY_DECIMAL:
            if (gc_is_marked(v.as.dec)) return;
            gc_set_marked(v.as.dec, 1);
            gc_mark_bignum_digits(v.as.dec->unscaled);
            return;
        case TY_ARRAY:
            gc_mark_array(a, v.as.arr);
            return;
        case TY_STRUCT:
            gc_mark_struct(a, v.as.st);
            return;
        case TY_RESULT:
            gc_mark_result(a, v.as.res);
            return;
        case TY_ENUM:
            gc_mark_enum(a, v.as.en);
            return;
        case TY_FUNC:
            gc_mark_closure(a, v.as.clo);
            return;
        case TY_MAP:
        case TY_SET:
            /* Not yet gc_alloc-routed -- same known gap gc_copy.c/value_copy.c already have (see
             * gc_mark.h). Walked anyway, in case a future MapObj ever does live in a gc_alloc'd
             * arena: entries are still real Values worth tracing, even though the trie nodes
             * themselves aren't gc_alloc'd objects this module can mark. */
            {
                Value *keys, *vals;
                mapobj_entries(v.as.map, &keys, &vals);
                for (int i = 0; i < v.as.map->count; i++) {
                    gc_mark_into(a, keys[i]);
                    gc_mark_into(a, vals[i]);
                }
                free(keys);
                free(vals);
            }
            return;
        case TY_CHANNEL:
            /* Never traced into -- see this file's own header comment. */
            return;
        default:
            /* int, float, bool, byte, unit: embedded in the Value itself, nothing to mark. */
            return;
    }
}

void gc_mark_value(const Arena *a, Value v) {
    gc_mark_into(a, v);
}

void gc_mark_object(const Arena *a, const void *obj) {
    if (obj == NULL) return;
    switch (gc_kind_of(obj)) {
        case GC_ATOMIC:
            /* A leaf: mark it and stop. Idempotent regardless (setting an already-set bit is a
             * no-op), so no gc_is_marked check is needed before it, unlike the structural kinds
             * below, which need one to avoid retracing an already-visited object's own children. */
            gc_set_marked((void *)obj, 1);
            return;
        case GC_VALUES: {
            if (gc_is_marked(obj)) return;
            gc_set_marked((void *)obj, 1);
            size_t count = gc_size_of(obj) / sizeof(Value);
            const Value *items = (const Value *)obj;
            for (size_t i = 0; i < count; i++) gc_mark_into(a, items[i]);
            return;
        }
        case GC_ARRAY:
            gc_mark_array(a, (ArrayObj *)obj);
            return;
        case GC_STRUCT:
            gc_mark_struct(a, (StructObj *)obj);
            return;
        case GC_RESULT:
            gc_mark_result(a, (ResultObj *)obj);
            return;
        case GC_ENUM:
            gc_mark_enum(a, (EnumObj *)obj);
            return;
        case GC_CLOSURE:
            gc_mark_closure(a, (ClosureObj *)obj);
            return;
        case GC_FREE:
            /* A stray stack word resolved to an already-reclaimed chunk -- not live, nothing to
             * mark (see this function's own header comment). */
            return;
    }
}
