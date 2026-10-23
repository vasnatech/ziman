/* Standalone correctness test for src/gc_mark.c. Lives outside src/ (which the Makefile wildcards
 * into the ziman binary) since this file has its own main() that would collide with main.c's at
 * link time. Run via `make test-gc-mark`.
 *
 * Unlike gc_copy_test.c's synthetic ArrayObj/StructObj/etc. (plain malloc'd, since gc_copy_value
 * only ever *reads* its input graph), every structural object here must be a real gc_alloc'd
 * object -- gc_mark_value calls gc_is_marked/gc_set_marked directly on the graph in place, which
 * needs a real header already sitting just before each one. `value_hash` is stubbed for the same
 * reason gc_copy_test.c stubs it: nothing here constructs a TY_MAP/TY_SET value, so its real
 * definition (src/interpreter.c, not linked here) is never actually called.
 *
 * Not fully valgrind-clean by construction, not by oversight, for the identical reason
 * gc_copy_test.c isn't: every type_array/type_struct/type_result/type_enum/type_channel/type_func
 * call below mallocs a Type node this codebase never frees, by design (ast.h's permanent-AST
 * policy) -- not a leak specific to this test or to gc_mark.c. Everything else here is either
 * gc_alloc'd (freed in bulk by arena_destroy) or a plain C local; nothing else to clean up. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arena.h"
#include "../../src/gc_mark.h"
#include "../../src/gc_object.h"
#include "../../src/value.h"

uint64_t value_hash(Value v) {
    (void)v;
    return 0;
}

static int failures = 0;

static void check(const char *label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

static Value gc_string(Arena *a, const char *s) {
    size_t len = strlen(s);
    char *buf = gc_alloc(a, GC_ATOMIC, len + 1);
    memcpy(buf, s, len + 1);
    /* The real constructor, so this helper holds no copy of the string Value's layout -- it gained
     * a flags word in 2026-10-23's character-indexing change. */
    return value_string_view(buf, len, 1);
}

static Value gc_array_of_ints(Arena *a, const int64_t *items, int count) {
    Value *boxed = count > 0 ? gc_alloc(a, GC_VALUES, (size_t)count * sizeof(Value)) : NULL;
    for (int i = 0; i < count; i++) boxed[i] = value_int(items[i]);
    ArrayObj *arr = gc_alloc(a, GC_ARRAY, sizeof(ArrayObj));
    arr->boxed_items = boxed;
    arr->bytes = NULL;
    arr->bools = NULL;
    arr->ints = NULL;
    arr->floats = NULL;
    arr->count = count;
    arr->store = NULL;
    Value v;
    v.type = type_ref(type_array(type_scalar(TY_INT)));
    v.as.arr = arr;
    return v;
}

static char *field_names[] = {"x", "y"};

static Value gc_point_struct(Arena *a, Value x, Value y) {
    Value *fields = gc_alloc(a, GC_VALUES, 2 * sizeof(Value));
    fields[0] = x;
    fields[1] = y;
    StructObj *st = gc_alloc(a, GC_STRUCT, sizeof(StructObj));
    st->field_names = field_names;
    st->fields = fields;
    st->count = 2;
    Value v;
    v.type = type_ref(type_struct("Point"));
    v.as.st = st;
    return v;
}

int main(void) {
    Arena *a = arena_create();

    /* A scalar: nothing to mark, nothing to crash on. */
    gc_mark_value(a, value_int(42));
    check("marking a scalar does not crash", 1);

    /* A plain string: its own buffer gets marked. */
    Value s = gc_string(a, "hello world");
    gc_mark_value(a, s);
    check("a marked string's own buffer is marked", gc_is_marked(s.as.str.ptr));

    /* The interior-pointer case: a view sharing its parent's exact start address with a shorter
     * length. Marking the view must mark the *enclosing* buffer (resolved via gc_lookup inside
     * gc_mark.c), not silently fail to mark anything because the view's own pointer isn't itself a
     * gc_alloc-returned address. */
    Value parent = gc_string(a, "hello");
    Value prefix = value_string_view(parent.as.str.ptr, 3, 0); /* same start as parent, len 3 */
    gc_mark_value(a, prefix);
    check("marking a string view marks the enclosing buffer it's a view of", gc_is_marked(parent.as.str.ptr));

    /* An array: the array object itself, its Value[] buffer, and every element get marked. */
    int64_t nums[] = {1, 2, 3};
    Value arr = gc_array_of_ints(a, nums, 3);
    gc_mark_value(a, arr);
    check("marking an array marks the ArrayObj itself", gc_is_marked(arr.as.arr));
    check("marking an array marks its own boxed_items buffer", gc_is_marked(arr.as.arr->boxed_items));

    /* An empty array (boxed_items == NULL): must not crash trying to mark a NULL buffer. */
    Value empty_arr = gc_array_of_ints(a, NULL, 0);
    gc_mark_value(a, empty_arr);
    check("marking an empty array does not crash", gc_is_marked(empty_arr.as.arr));

    /* A struct containing a string field: marking the struct must reach into its own fields. */
    Value point = gc_point_struct(a, value_int(3), gc_string(a, "label"));
    gc_mark_value(a, point);
    check("marking a struct marks the StructObj itself", gc_is_marked(point.as.st));
    check("marking a struct marks its own fields buffer", gc_is_marked(point.as.st->fields));
    check("marking a struct reaches a string field's own buffer", gc_is_marked(point.as.st->fields[1].as.str.ptr));

    /* Sharing: the same StructObj reachable from two different array elements must be marked
     * exactly once, with no crash or infinite loop -- the whole reason marking is idempotent per
     * object, not just an optimisation. */
    Value shared_items[] = {point, point};
    Value *shared_boxed = gc_alloc(a, GC_VALUES, 2 * sizeof(Value));
    shared_boxed[0] = shared_items[0];
    shared_boxed[1] = shared_items[1];
    ArrayObj *shared_arr = gc_alloc(a, GC_ARRAY, sizeof(ArrayObj));
    shared_arr->boxed_items = shared_boxed;
    shared_arr->bytes = NULL;
    shared_arr->bools = NULL;
    shared_arr->ints = NULL;
    shared_arr->floats = NULL;
    shared_arr->count = 2;
    shared_arr->store = NULL;
    Value shared_val;
    shared_val.type = type_ref(type_array(*point.type));
    shared_val.as.arr = shared_arr;
    gc_mark_value(a, shared_val);
    check("marking a value graph with shared substructure terminates and marks it", gc_is_marked(point.as.st));

    /* A bigint: the boxed wrapper and its digit array both get marked. */
    BigNum n = {.sign = 1, .digits = gc_alloc(a, GC_ATOMIC, 2 * sizeof(uint32_t)), .count = 2};
    n.digits[0] = 7;
    n.digits[1] = 11;
    BigNum *boxed_bi = gc_alloc(a, GC_ATOMIC, sizeof(BigNum));
    *boxed_bi = n;
    Value bi;
    bi.type = type_scalar_ref(TY_BIGINT);
    bi.as.bi = boxed_bi;
    gc_mark_value(a, bi);
    check("marking a bigint marks its own boxed wrapper", gc_is_marked(bi.as.bi));
    check("marking a bigint marks its own digit array", gc_is_marked(bi.as.bi->digits));

    /* A Result and an Enum: payloads get traced through. */
    ResultObj *res = gc_alloc(a, GC_RESULT, sizeof(ResultObj));
    res->is_ok = 1;
    res->value = gc_string(a, "ok-payload");
    Value res_val;
    res_val.type = type_ref(type_result(type_scalar(TY_STRING), type_scalar(TY_STRING)));
    res_val.as.res = res;
    gc_mark_value(a, res_val);
    check("marking a Result marks the ResultObj itself", gc_is_marked(res_val.as.res));
    check("marking a Result reaches its own payload's buffer", gc_is_marked(res->value.as.str.ptr));

    EnumObj *en = gc_alloc(a, GC_ENUM, sizeof(EnumObj));
    en->enum_name = "Shape";
    en->variant_name = "Circle";
    en->has_payload = 1;
    en->payload = value_int(7);
    Value en_val;
    en_val.type = type_ref(type_enum("Shape"));
    en_val.as.en = en;
    gc_mark_value(a, en_val);
    check("marking an enum marks the EnumObj itself", gc_is_marked(en_val.as.en));

    /* A closure: captured values get traced through, fn (borrowed AST) is left alone. */
    Value *captured = gc_alloc(a, GC_VALUES, sizeof(Value));
    captured[0] = gc_string(a, "captured-string");
    ClosureObj *clo = gc_alloc(a, GC_CLOSURE, sizeof(ClosureObj));
    clo->fn = NULL;
    clo->ncaptured = 1;
    clo->captured = captured;
    Value clo_val;
    clo_val.type = type_ref(type_func((TypeList){0}, type_scalar(TY_VOID)));
    clo_val.as.clo = clo;
    gc_mark_value(a, clo_val);
    check("marking a closure marks the ClosureObj itself", gc_is_marked(clo_val.as.clo));
    check("marking a closure reaches a captured value's own buffer", gc_is_marked(clo->captured[0].as.str.ptr));

    /* Channel: never traced into -- marking a Value that holds one must not crash even though a
     * ChannelObj here is not a gc_alloc'd object at all (a real ChannelObj never would be, per
     * MEMORY.md's own design -- it belongs to no single thread's heap). */
    ChannelObj chan;
    memset(&chan, 0, sizeof(chan));
    Value chan_val = value_channel(type_ref(type_channel(type_scalar(TY_INT))), &chan);
    gc_mark_value(a, chan_val);
    check("marking a channel value does not crash and does not try to mark it", 1);

    /* gc_mark_object: the bridge a bare, conservatively-found root needs (no Value, no Type.kind to
     * dispatch on -- only the object's own GcKind header). */
    char *bare_atomic = gc_alloc(a, GC_ATOMIC, 16);
    gc_mark_object(a, bare_atomic);
    check("gc_mark_object marks a bare GC_ATOMIC root directly", gc_is_marked(bare_atomic));

    /* A bare Value[] buffer found with no enclosing wrapper yet -- exactly the shape mid-array-
     * construction has (see gc_mark.h's own comment): still has to mark itself and trace each of
     * its own elements, found purely from gc_size_of, with no ArrayObj/StructObj to consult. */
    Value *bare_values = gc_alloc(a, GC_VALUES, 2 * sizeof(Value));
    bare_values[0] = value_int(5);
    bare_values[1] = gc_string(a, "reached only via a bare Value[] root");
    gc_mark_object(a, bare_values);
    check("gc_mark_object marks a bare GC_VALUES root itself", gc_is_marked(bare_values));
    check("gc_mark_object traces into a bare GC_VALUES root's own elements", gc_is_marked(bare_values[1].as.str.ptr));

    /* A bare structural object (an ArrayObj found directly, not via a Value carrying its Type). */
    int64_t bare_nums[] = {9, 9, 9};
    Value bare_arr_val = gc_array_of_ints(a, bare_nums, 3);
    gc_mark_object(a, bare_arr_val.as.arr);
    check("gc_mark_object marks a bare structural (GC_ARRAY) root and its own children", gc_is_marked(bare_arr_val.as.arr->boxed_items));

    /* A free chunk: a stray root resolving to one must be a safe no-op, not mistaken for something
     * to trace into (see the GC_FREE case in gc_object.h/gc_mark.h's own comments). */
    void *freed = gc_alloc(a, GC_ATOMIC, 16);
    gc_set_kind(freed, GC_FREE);
    gc_mark_object(a, freed);
    check("gc_mark_object leaves a GC_FREE root untouched, not marked", !gc_is_marked(freed));

    gc_mark_object(a, NULL);
    check("gc_mark_object on NULL does not crash", 1);

    arena_destroy(a);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
