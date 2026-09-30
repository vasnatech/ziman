#include "value_copy.h"

#include <string.h>

#include "mapobj.h"

/* `n`'s digit array duplicated; `n` itself is a plain struct, already copied by the caller
 * (`*v.as.bi` or `*v.as.dec.unscaled`) -- only the heap-allocated `digits` underneath is aliased
 * until this runs. `count == 0` means no digits at all (see bignum.h), so nothing to duplicate. */
static BigNum bignum_deep_copy(BigNum n) {
    if (n.count > 0) {
        uint32_t *digits = malloc((size_t)n.count * sizeof(uint32_t));
        memcpy(digits, n.digits, (size_t)n.count * sizeof(uint32_t));
        n.digits = digits;
    }
    return n;
}

Value value_deep_copy(Value v) {
    switch (v.type.kind) {
        case TY_STRING:
            return value_string_copy(v.as.str.ptr, v.as.str.len);
        case TY_BIGINT:
            return value_bigint(bignum_deep_copy(*v.as.bi));
        case TY_DECIMAL: {
            Decimal d = *v.as.dec;
            d.unscaled = bignum_deep_copy(d.unscaled);
            return value_decimal(d);
        }
        case TY_ARRAY: {
            ArrayObj *a = v.as.arr;
            /* Copied element by element into a plain boxed array regardless of whether `a` was
             * packed -- packed-vs-boxed is purely an internal storage optimisation, invisible to
             * any program (see docs/SYNTAX.md's Builtins section), so a value that crosses a
             * thread coming back boxed is not a correctness change, only a missed optimisation for
             * the rare (thread-crossing) case. `store` (the spare-capacity `push` shares between
             * snapshots of the same growing array) is deliberately not carried over: it is tied to
             * one thread's own sequence of pushes, and sharing it across threads would mean two
             * independently-collected heaps racing a compare-and-swap over memory only one of them
             * will end up owning. */
            Value *items = a->count > 0 ? malloc((size_t)a->count * sizeof(Value)) : NULL;
            for (int i = 0; i < a->count; i++) {
                items[i] = value_deep_copy(array_get(a, i));
            }
            return value_array(v.type, array_obj_new(items, a->count));
        }
        case TY_STRUCT: {
            StructObj *st = v.as.st;
            Value *fields = st->count > 0 ? malloc((size_t)st->count * sizeof(Value)) : NULL;
            for (int i = 0; i < st->count; i++) {
                fields[i] = value_deep_copy(st->fields[i]);
            }
            /* field_names is borrowed from the StructDecl (never freed, never duplicated per
             * instance, same as every other copy of this struct) -- shared, not copied. */
            return value_struct(v.type, struct_obj_new(st->field_names, fields, st->count));
        }
        case TY_RESULT: {
            ResultObj *res = v.as.res;
            return value_result(v.type, result_obj_new(res->is_ok, value_deep_copy(res->value)));
        }
        case TY_ENUM: {
            EnumObj *en = v.as.en;
            Value payload = en->has_payload ? value_deep_copy(en->payload) : en->payload;
            /* enum_name/variant_name borrowed from the EnumDecl, same reasoning as field_names. */
            return value_enum(v.type, enum_obj_new(en->enum_name, en->variant_name, en->has_payload, payload));
        }
        case TY_FUNC: {
            ClosureObj *clo = v.as.clo;
            Value *captured = clo->ncaptured > 0 ? malloc((size_t)clo->ncaptured * sizeof(Value)) : NULL;
            for (int i = 0; i < clo->ncaptured; i++) {
                captured[i] = value_deep_copy(clo->captured[i]);
            }
            /* fn is a FunctionDecl*: part of the AST, never freed, shared same as everywhere else. */
            return value_closure(clo->fn, captured, clo->ncaptured);
        }
        case TY_MAP:
        case TY_SET: {
            /* Rebuilt through the ordinary set-one-at-a-time API rather than duplicating MapObj's
             * own trie nodes by hand: mapobj_entries already gives every entry in insertion order,
             * and re-inserting in that same order reproduces the same order in the copy (see
             * MapObj's own doc comment on how order is derived from insertion sequence numbers). */
            Value *keys, *vals;
            mapobj_entries(v.as.map, &keys, &vals);
            MapObj *copy = mapobj_empty();
            for (int i = 0; i < v.as.map->count; i++) {
                copy = mapobj_set(copy, value_deep_copy(keys[i]), value_deep_copy(vals[i]));
            }
            free(keys);
            free(vals);
            return value_map(v.type, copy);
        }
        case TY_CHANNEL:
            /* Deliberately shared by identity, not copied -- see this function's own header
             * comment in value_copy.h. */
            return v;
        default:
            /* int, float, bool, byte, unit: embedded in the Value itself, nothing to copy. */
            return v;
    }
}
