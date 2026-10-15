/* Standalone correctness test for src/gc_copy.c. Lives outside src/ (which the Makefile wildcards
 * into the ziman binary) since this file has its own main() that would collide with main.c's at
 * link time. Run via `make test-gc-copy`.
 *
 * Links the real src/value.c and its own dependencies (ast.c/error.c/stack.c/bignum.c/decimal.c/
 * mapobj.c) rather than the interpreter's own object files, since this needs real ArrayObj/
 * StructObj/etc. construction, not a hand-rolled stand-in. `value_hash` is stubbed below: its real
 * definition lives in src/interpreter.c (not linked here), and nothing in this file constructs a
 * TY_MAP/TY_SET value -- gc_copy.c does not gc_alloc-route those yet (see gc_copy.h), so there is
 * nothing of this module's own to verify there.
 *
 * Not fully valgrind-clean by construction, not by oversight: every type_array/type_struct/
 * type_result/type_enum/type_channel/type_func call below mallocs a Type node that is never freed,
 * matching this codebase's own permanent-AST, never-free policy (see ast.h) -- the same thing
 * true of every Type the real interpreter ever builds, not a leak specific to this test or to
 * gc_copy.c. Everything this file itself owns (the dedup map, the destination arena, the plain
 * ArrayObj/StructObj/etc. built by hand below) is freed at the end. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arena.h"
#include "../../src/gc_copy.h"
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

static int str_eq(Value v, const char *expect) {
    return v.type->kind == TY_STRING && v.as.str.len == strlen(expect) && memcmp(v.as.str.ptr, expect, v.as.str.len) == 0;
}

int main(void) {
    Arena *dest = arena_create();
    GcDedup *dedup = gc_dedup_create();

    /* Scalars: embedded in the Value itself, nothing to allocate -- copy is a no-op. */
    Value vi = value_int(42);
    Value vi2 = gc_copy_value(dest, dedup, vi);
    check("an int copies as itself (no allocation)", vi2.type->kind == TY_INT && vi2.as.i == 42);

    /* A plain string: independent buffer, same bytes, tracked with a GC_ATOMIC header. */
    Value s1 = value_string_copy("hello world", strlen("hello world"));
    Value s1c = gc_copy_value(dest, dedup, s1);
    check("a copied string has the same content", str_eq(s1c, "hello world"));
    check("a copied string is a different buffer", s1c.as.str.ptr != s1.as.str.ptr);
    check("a copied string's buffer is GC_ATOMIC", gc_kind_of(s1c.as.str.ptr) == GC_ATOMIC);
    check("a copied string's header reports len+1", gc_size_of(s1c.as.str.ptr) == strlen("hello world") + 1);

    /* The interior-pointer case gc_copy.h's own header comment is about: a view sharing its
     * parent's exact start address but a shorter length. Both must copy to their own correct,
     * independent content -- neither may see the other's dedup entry (gc_copy_string never
     * consults the dedup map at all, precisely to avoid this ambiguity; this proves that holds in
     * practice, not just in the comment). */
    Value parent = value_string_copy("hello", 5);
    Value prefix = value_string_view(parent.as.str.ptr, 3, 0); /* same ptr as parent, len 3: "hel" */
    check("the view really does share its parent's start address", prefix.as.str.ptr == parent.as.str.ptr);
    Value parent_c = gc_copy_value(dest, dedup, parent);
    Value prefix_c = gc_copy_value(dest, dedup, prefix);
    check("the full string copies correctly despite the ambiguous shared prefix", str_eq(parent_c, "hello"));
    check("the short view copies its own 3 bytes, not the parent's 5", str_eq(prefix_c, "hel"));
    check("the two copies are independent buffers", parent_c.as.str.ptr != prefix_c.as.str.ptr);

    /* A BigNum, boxed: same digits, different heap object, dedup'd if reached twice. */
    BigNum n = {.sign = 1, .digits = (uint32_t[]){7, 11}, .count = 2};
    Value bi = value_bigint(n);
    Value bi_c1 = gc_copy_value(dest, dedup, bi);
    Value bi_c2 = gc_copy_value(dest, dedup, bi);
    check("a bigint's digits copy correctly", bi_c1.as.bi->count == 2 && bi_c1.as.bi->digits[0] == 7 && bi_c1.as.bi->digits[1] == 11);
    check("a bigint's boxed wrapper is a different object", bi_c1.as.bi != bi.as.bi);
    check("the same bigint reached twice dedups to the same copy", bi_c1.as.bi == bi_c2.as.bi);
    check("a copied bigint's header is GC_ATOMIC", gc_kind_of(bi_c1.as.bi) == GC_ATOMIC);

    /* Sharing preservation: the same ArrayObj reachable from two different array elements must
     * still be the same pointer after the copy, not two independent duplicates -- this is the
     * whole reason gc_copy.c carries a dedup map that value_copy.c does not need (a one-off
     * thread-crossing copy has no reason to care about duplicating a shared substructure; a value
     * that survives many collection cycles over a thread's lifetime very much does). */
    Value inner_items[] = {value_int(1), value_int(2)};
    ArrayObj *inner = array_obj_new(inner_items, 2);
    Type inner_type = type_array(type_scalar(TY_INT));
    Value inner_val = value_array(type_ref(inner_type), inner);
    Value outer_items[] = {inner_val, inner_val}; /* same ArrayObj* twice */
    ArrayObj *outer = array_obj_new(outer_items, 2);
    Type outer_type = type_array(inner_type);
    Value outer_val = value_array(type_ref(outer_type), outer);
    Value outer_c = gc_copy_value(dest, dedup, outer_val);
    check("a copied array has the right element count", outer_c.as.arr->count == 2);
    Value e0 = array_get(outer_c.as.arr, 0);
    Value e1 = array_get(outer_c.as.arr, 1);
    check("both elements copy to the same shared inner array", e0.as.arr == e1.as.arr);
    check("the shared inner array is a new object, not the original", e0.as.arr != inner);
    check("the shared inner array's content is correct", array_get(e0.as.arr, 0).as.i == 1 && array_get(e0.as.arr, 1).as.i == 2);
    check("a copied array's header is GC_ARRAY", gc_kind_of(outer_c.as.arr) == GC_ARRAY);

    /* The array-view interior-pointer case: array_obj_view shares boxed_items at an offset, but
     * since gc_copy_array always reads through array_get (which already resolves a view's own
     * offset) rather than touching boxed_items directly, a view just needs to copy correctly --
     * no special dedup handling is needed here, unlike strings, because the *wrapper* ArrayObj a
     * view gets is always its own fresh allocation (see array_obj_view in src/value.c), never the
     * parent's, so there is no shared-address ambiguity to resolve for arrays at all. */
    Value five_items[] = {value_int(10), value_int(20), value_int(30), value_int(40), value_int(50)};
    ArrayObj *five = array_obj_new(five_items, 5);
    ArrayObj *view = array_obj_view(five, 1, 4); /* [20, 30, 40] */
    Value view_val = value_array(type_ref(inner_type), view);
    Value view_c = gc_copy_value(dest, dedup, view_val);
    check("a copied array view has the sliced element count", view_c.as.arr->count == 3);
    check("a copied array view has the sliced content, not the parent's", array_get(view_c.as.arr, 0).as.i == 20 &&
                                                                               array_get(view_c.as.arr, 1).as.i == 30 &&
                                                                               array_get(view_c.as.arr, 2).as.i == 40);

    /* StructObj sharing, same idea as ArrayObj: field_names borrowed (not copied), fields deep-
     * copied, and the struct pointer itself dedup'd across two reachable paths. */
    char *field_names[] = {"x", "y"};
    Value point_fields[] = {value_int(3), value_int(4)};
    StructObj *point = malloc(sizeof(StructObj));
    point->field_names = field_names;
    point->fields = malloc(2 * sizeof(Value));
    point->fields[0] = point_fields[0];
    point->fields[1] = point_fields[1];
    point->count = 2;
    Type point_type = type_struct("Point");
    Value point_val = value_struct(type_ref(point_type), point);
    Value pair_items[] = {point_val, point_val};
    ArrayObj *pair = array_obj_new(pair_items, 2);
    Value pair_val = value_array(type_ref(type_array(point_type)), pair);
    Value pair_c = gc_copy_value(dest, dedup, pair_val);
    Value p0 = array_get(pair_c.as.arr, 0);
    Value p1 = array_get(pair_c.as.arr, 1);
    check("a shared struct dedups to the same copy", p0.as.st == p1.as.st);
    check("the struct copy is a new object", p0.as.st != point);
    check("field_names is borrowed, not copied", p0.as.st->field_names == field_names);
    check("the struct's fields copied correctly", p0.as.st->fields[0].as.i == 3 && p0.as.st->fields[1].as.i == 4);
    check("a copied struct's header is GC_STRUCT", gc_kind_of(p0.as.st) == GC_STRUCT);
    check("the struct's fields buffer header is GC_VALUES", gc_kind_of(p0.as.st->fields) == GC_VALUES);

    /* Result and Enum: content copies correctly, tagged the same as before. */
    ResultObj *ok = malloc(sizeof(ResultObj));
    ok->is_ok = 1;
    ok->value = value_string_copy("ok-payload", strlen("ok-payload"));
    Value res_val = value_result(type_ref(type_result(type_scalar(TY_STRING), type_scalar(TY_STRING))), ok);
    Value res_c = gc_copy_value(dest, dedup, res_val);
    check("a copied Result keeps is_ok", res_c.as.res->is_ok == 1);
    check("a copied Result's payload copied correctly", str_eq(res_c.as.res->value, "ok-payload"));
    check("a copied Result's header is GC_RESULT", gc_kind_of(res_c.as.res) == GC_RESULT);

    EnumObj *en = malloc(sizeof(EnumObj));
    en->enum_name = "Shape";
    en->variant_name = "Circle";
    en->has_payload = 1;
    en->payload = value_int(7);
    Value en_val = value_enum(type_ref(type_enum("Shape")), en);
    Value en_c = gc_copy_value(dest, dedup, en_val);
    check("a copied enum keeps its variant name (borrowed)", en_c.as.en->variant_name == en->variant_name);
    check("a copied enum's payload copied correctly", en_c.as.en->has_payload && en_c.as.en->payload.as.i == 7);
    check("a copied enum's header is GC_ENUM", gc_kind_of(en_c.as.en) == GC_ENUM);

    /* Closure: captured values deep-copy, fn stays borrowed (it is AST, permanent). */
    Value captured[] = {value_int(99)};
    ClosureObj *clo = malloc(sizeof(ClosureObj));
    clo->fn = NULL; /* borrowed AST pointer -- identity is all this test checks */
    clo->ncaptured = 1;
    clo->captured = malloc(sizeof(Value));
    clo->captured[0] = captured[0];
    Value clo_val;
    clo_val.type = type_ref(type_func((TypeList){0}, type_scalar(TY_VOID)));
    clo_val.as.clo = clo;
    Value clo_c = gc_copy_value(dest, dedup, clo_val);
    check("a copied closure's fn stays borrowed", clo_c.as.clo->fn == clo->fn);
    check("a copied closure's captured values copied correctly", clo_c.as.clo->captured[0].as.i == 99);
    check("a copied closure's header is GC_CLOSURE", gc_kind_of(clo_c.as.clo) == GC_CLOSURE);

    /* Channel: shared by identity, never copied -- same rule as value_copy.c. */
    ChannelObj chan;
    memset(&chan, 0, sizeof(chan));
    Value chan_val = value_channel(type_ref(type_channel(type_scalar(TY_INT))), &chan);
    Value chan_c = gc_copy_value(dest, dedup, chan_val);
    check("a channel is returned unchanged, not copied", chan_c.as.ch == &chan);

    free(point->fields);
    free(point);
    free(ok);
    free(en);
    free(clo->captured);
    free(clo);
    gc_dedup_destroy(dedup);
    arena_destroy(dest);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
