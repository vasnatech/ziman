/* Standalone correctness test for src/gc_env_roots.c. Lives outside src/ (which the Makefile
 * wildcards into the ziman binary) since this file has its own main() that would collide with
 * main.c's at link time. Run via `make test-gc-env-roots`.
 *
 * Needs the same real-Value-construction dependency chain as gc_mark_test.c (env.c is new on top of
 * that). `value_hash` is stubbed for the identical reason those tests stub it: nothing here
 * constructs a TY_MAP/TY_SET value. Not fully valgrind-clean by construction, not by oversight, for
 * the identical reason those tests aren't: type_scalar/type_struct calls below build permanent AST
 * `Type` nodes this codebase never frees, by design. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/arena.h"
#include "../../src/env.h"
#include "../../src/gc_env_roots.h"
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
    Value v;
    v.type = type_scalar_ref(TY_STRING);
    v.as.str.ptr = buf;
    v.as.str.len = (uint32_t)len;
    v.as.str.term = 1;
    return v;
}

int main(void) {
    Arena *a = arena_create();

    /* A two-scope chain: a global-like outer scope and an inner one, mirroring a function call's
     * own scope nested under g_global_env. Marking from the inner scope must reach both. */
    Env *global = env_new(NULL);
    Value global_str = gc_string(a, "reached through the parent scope");
    env_define(global, "g", type_scalar(TY_STRING), 0, global_str);

    Env *inner = env_new(global);
    Value inner_str = gc_string(a, "reached through the inner scope directly");
    env_define(inner, "x", type_scalar(TY_STRING), 0, inner_str);
    env_define(inner, "n", type_scalar(TY_INT), 0, value_int(42));

    gc_mark_env_chain(a, inner);
    check("a binding in the inner scope is marked", gc_is_marked(inner_str.as.str.ptr));
    check("a binding in the parent scope is marked too", gc_is_marked(global_str.as.str.ptr));

    /* A scalar binding (int): nothing to mark, must not crash. */
    check("marking a chain with a scalar binding does not crash", 1);

    env_free(inner);
    env_free(global);

    /* A single scope with no parent: must terminate, not walk into garbage past NULL. */
    Env *lone = env_new(NULL);
    Value lone_str = gc_string(a, "a scope with no parent at all");
    env_define(lone, "s", type_scalar(TY_STRING), 0, lone_str);
    gc_mark_env_chain(a, lone);
    check("a scope with no parent is marked and the walk terminates", gc_is_marked(lone_str.as.str.ptr));
    env_free(lone);

    /* An empty scope: no bindings at all, must not crash. */
    Env *empty = env_new(NULL);
    gc_mark_env_chain(a, empty);
    check("an empty scope does not crash", 1);
    env_free(empty);

    /* A structural value (not just a leaf string) bound in a scope: confirms this composes with
     * gc_mark_value's own recursive tracing, not just its leaf cases. */
    Env *with_struct = env_new(NULL);
    char *field_names[] = {"n"};
    Value *fields = gc_alloc(a, GC_VALUES, sizeof(Value));
    fields[0] = gc_string(a, "nested inside a struct bound in a scope");
    StructObj *st = gc_alloc(a, GC_STRUCT, sizeof(StructObj));
    st->field_names = field_names;
    st->fields = fields;
    st->count = 1;
    Value st_val;
    st_val.type = type_ref(type_struct("Named"));
    st_val.as.st = st;
    env_define(with_struct, "obj", *st_val.type, 0, st_val);
    gc_mark_env_chain(a, with_struct);
    check("a struct bound in a scope is marked", gc_is_marked(st));
    check("a struct bound in a scope has its own fields marked", gc_is_marked(fields[0].as.str.ptr));
    env_free(with_struct);

    arena_destroy(a);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
