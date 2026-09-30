#include "value_copy.h"

#include <string.h>

#include "arena.h"
#include "mapobj.h"

/* Where a crossing-point copy actually lives: not the ordinary allocator (`malloc`/`GC_MALLOC`,
 * whatever value.c's own constructors use), and -- once per-thread arenas exist for everything
 * else (docs/MEMORY.md Phase 2's remaining work) -- not the calling thread's own per-thread arena
 * either. An arena freed "at thread exit" would free memory a still-running thread on the other
 * end of a `spawn` or `Channel` still needs; see MEMORY.md's own correction on this before any
 * allocation site gets rewired to use per-thread arenas.
 *
 * The fix: every thread gets its own *outbound* arena, created lazily on first use and never
 * destroyed -- deliberately leaked, the honest interim answer MEMORY.md already gives, until a
 * real reclamation story for exactly these values (most plausibly a small refcount scoped to just
 * what crosses a thread boundary, not the general value graph) gets designed. This needs no lock
 * despite outliving its own thread: only the thread that calls `value_deep_copy` ever writes into
 * its own outbound arena; every other thread only ever *reads* an already-fully-built value out of
 * it afterward, through the copy `spawn`/`send` hands it -- the same "immutable, so safe to share
 * by pointer with no locking" reasoning this interpreter has relied on for every `Value` since
 * before this file existed (see value.h's own `ChannelObj` comment). */
static __thread Arena *t_outbound = NULL;

static Arena *outbound_arena(void) {
    /* Scanned, not plain: this arena can end up holding a native resource handle (a database
     * connection's DbConn* address, disguised as a plain int64_t Value field -- see
     * src/native_sqlite.c's own comment on the trick, and arena.h's own comment on
     * arena_create_scanned for why an *unscanned* copy of one is a real, silent bug, not just a
     * missed optimisation: Boehm only keeps the underlying native object alive because its address
     * sits somewhere conservative scanning looks, and an unscanned arena block is invisible to
     * that scanning by construction. */
    if (t_outbound == NULL) t_outbound = arena_create_scanned();
    return t_outbound;
}

static Value arena_string_copy(Arena *ar, const char *ptr, size_t len) {
    char *buf = arena_alloc(ar, len + 1);
    memcpy(buf, ptr, len);
    buf[len] = '\0';
    Value v;
    v.type = type_scalar(TY_STRING);
    v.as.str.ptr = buf;
    v.as.str.len = (uint32_t)len;
    v.as.str.term = 1;
    return v;
}

/* `n`'s digit array duplicated into `ar`; `n` itself is a plain struct, passed by value, so only
 * the heap-allocated `digits` underneath is aliased until this runs. `count == 0` means no digits
 * at all (see bignum.h), so nothing to duplicate. */
static BigNum arena_bignum_copy(Arena *ar, BigNum n) {
    if (n.count > 0) {
        uint32_t *digits = arena_alloc(ar, (size_t)n.count * sizeof(uint32_t));
        memcpy(digits, n.digits, (size_t)n.count * sizeof(uint32_t));
        n.digits = digits;
    }
    return n;
}

static Value arena_bigint(Arena *ar, BigNum n) {
    BigNum *boxed = arena_alloc(ar, sizeof(BigNum));
    *boxed = arena_bignum_copy(ar, n);
    Value v;
    v.type = type_scalar(TY_BIGINT);
    v.as.bi = boxed;
    return v;
}

static Value arena_decimal(Arena *ar, Decimal d) {
    d.unscaled = arena_bignum_copy(ar, d.unscaled);
    Decimal *boxed = arena_alloc(ar, sizeof(Decimal));
    *boxed = d;
    Value v;
    v.type = type_scalar(TY_DECIMAL);
    v.as.dec = boxed;
    return v;
}

static Value value_deep_copy_into(Arena *ar, Value v);

static Value arena_array_copy(Arena *ar, ArrayObj *a, Type array_type) {
    /* Copied element by element into a plain boxed array regardless of whether `a` was packed --
     * packed-vs-boxed is purely an internal storage optimisation, invisible to any program (see
     * docs/SYNTAX.md's Builtins section), so a value that crosses a thread coming back boxed is
     * not a correctness change, only a missed optimisation for the rare (thread-crossing) case.
     * `store` (the spare capacity `push` shares between snapshots of the same growing array) is
     * deliberately dropped: it is tied to one thread's own compare-and-swap sequence of pushes,
     * and a copy on another thread sharing it would mean two independently-managed heaps racing a
     * CAS over memory only one of them will end up owning. */
    Value *items = a->count > 0 ? arena_alloc(ar, (size_t)a->count * sizeof(Value)) : NULL;
    for (int i = 0; i < a->count; i++) {
        items[i] = value_deep_copy_into(ar, array_get(a, i));
    }
    ArrayObj *copy = arena_alloc(ar, sizeof(ArrayObj));
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

static Value arena_struct_copy(Arena *ar, StructObj *st, Type struct_type) {
    Value *fields = st->count > 0 ? arena_alloc(ar, (size_t)st->count * sizeof(Value)) : NULL;
    for (int i = 0; i < st->count; i++) {
        fields[i] = value_deep_copy_into(ar, st->fields[i]);
    }
    StructObj *copy = arena_alloc(ar, sizeof(StructObj));
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

static Value arena_result_copy(Arena *ar, ResultObj *res, Type result_type) {
    ResultObj *copy = arena_alloc(ar, sizeof(ResultObj));
    copy->is_ok = res->is_ok;
    copy->value = value_deep_copy_into(ar, res->value);
    Value v;
    v.type = result_type;
    v.as.res = copy;
    return v;
}

static Value arena_enum_copy(Arena *ar, EnumObj *en, Type enum_type) {
    EnumObj *copy = arena_alloc(ar, sizeof(EnumObj));
    /* enum_name/variant_name borrowed from the EnumDecl, same reasoning as field_names above. */
    copy->enum_name = en->enum_name;
    copy->variant_name = en->variant_name;
    copy->has_payload = en->has_payload;
    copy->payload = en->has_payload ? value_deep_copy_into(ar, en->payload) : en->payload;
    Value v;
    v.type = enum_type;
    v.as.en = copy;
    return v;
}

static Value arena_closure_copy(Arena *ar, ClosureObj *clo, Type func_type) {
    Value *captured = clo->ncaptured > 0 ? arena_alloc(ar, (size_t)clo->ncaptured * sizeof(Value)) : NULL;
    for (int i = 0; i < clo->ncaptured; i++) {
        captured[i] = value_deep_copy_into(ar, clo->captured[i]);
    }
    ClosureObj *copy = arena_alloc(ar, sizeof(ClosureObj));
    /* fn is a FunctionDecl*: part of the AST, never freed, shared same as everywhere else. */
    copy->fn = clo->fn;
    copy->ncaptured = clo->ncaptured;
    copy->captured = captured;
    Value v;
    v.type = func_type;
    v.as.clo = copy;
    return v;
}

static Value value_deep_copy_into(Arena *ar, Value v) {
    switch (v.type.kind) {
        case TY_STRING:
            return arena_string_copy(ar, v.as.str.ptr, v.as.str.len);
        case TY_BIGINT:
            return arena_bigint(ar, *v.as.bi);
        case TY_DECIMAL:
            return arena_decimal(ar, *v.as.dec);
        case TY_ARRAY:
            return arena_array_copy(ar, v.as.arr, v.type);
        case TY_STRUCT:
            return arena_struct_copy(ar, v.as.st, v.type);
        case TY_RESULT:
            return arena_result_copy(ar, v.as.res, v.type);
        case TY_ENUM:
            return arena_enum_copy(ar, v.as.en, v.type);
        case TY_FUNC:
            return arena_closure_copy(ar, v.as.clo, v.type);
        case TY_MAP:
        case TY_SET: {
            /* Not yet arena-routed: rebuilding a MapObj's persistent trie needs mapobj.c's own
             * allocation, which this file has no arena-aware variant of yet (a separate, later
             * piece of work -- not a correctness regression, since this is exactly how every kind
             * of value was allocated before this file existed). Rebuilt through the ordinary
             * set-one-at-a-time API rather than duplicating trie nodes by hand: mapobj_entries
             * already gives every entry in insertion order, and re-inserting in that order
             * reproduces it in the copy (see MapObj's own doc comment on how order is derived from
             * insertion sequence numbers). */
            Value *keys, *vals;
            mapobj_entries(v.as.map, &keys, &vals);
            MapObj *copy = mapobj_empty();
            for (int i = 0; i < v.as.map->count; i++) {
                copy = mapobj_set(copy, value_deep_copy_into(ar, keys[i]), value_deep_copy_into(ar, vals[i]));
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

Value value_deep_copy(Value v) {
    return value_deep_copy_into(outbound_arena(), v);
}
