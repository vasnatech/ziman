/* Standalone correctness test for src/gc_interp_roots.c. Lives outside src/ (which the Makefile
 * wildcards into the ziman binary) since this file has its own main() that would collide with
 * main.c's at link time. Run via `make test-gc-interp-roots`.
 *
 * gc_mark_interpreter_roots takes interpreter.c's own state (t_defer_envs/t_env_pool) as explicit
 * parameters rather than reading it directly -- see gc_interp_roots.h's own header comment for why
 * (interpreter.c is too large to link into a standalone test the way every other module here has
 * been) -- so this test builds synthetic stand-ins for that state, exercising the identical code
 * path a real interpreter would. `value_hash` is stubbed for the same reason gc_mark_test.c/
 * gc_env_roots_test.c stub it: nothing here constructs a TY_MAP/TY_SET value. Not fully
 * valgrind-clean by construction, not by oversight, for the identical reason those tests aren't --
 * type_scalar calls below build permanent AST Type nodes this codebase never frees, by design. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/arena.h"
#include "../../src/env.h"
#include "../../src/error.h"
#include "../../src/gc_interp_roots.h"
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
    v.type = type_scalar(TY_STRING);
    v.as.str.ptr = buf;
    v.as.str.len = (uint32_t)len;
    v.as.str.term = 1;
    return v;
}

int main(void) {
    Arena *a = arena_create();

    /* The global scope. */
    Env *global = env_new(NULL);
    Value global_str = gc_string(a, "reached through global_env");
    env_define(global, "g", type_scalar(TY_STRING), 0, global_str);

    /* Two active call frames, each with its own scope and its own binding -- proving every frame
     * gets walked, not just one. */
    Env *frame1_env = env_new(global);
    Value frame1_str = gc_string(a, "reached through the outer active frame");
    env_define(frame1_env, "a", type_scalar(TY_STRING), 0, frame1_str);
    ziman_push_frame("outer", "test.zm", 1, 1);
    ziman_set_frame_env(frame1_env);

    Env *frame2_env = env_new(frame1_env);
    Value frame2_str = gc_string(a, "reached through the inner active frame");
    env_define(frame2_env, "b", type_scalar(TY_STRING), 0, frame2_str);
    ziman_push_frame("inner", "test.zm", 2, 1);
    ziman_set_frame_env(frame2_env);

    /* A native call's frame: pushed with no env at all (ziman_set_frame_env never called for it) --
     * must not crash, and must not be mistaken for a real Env. */
    ziman_push_frame("some_native", NULL, 3, 1);

    /* Synthetic t_defer_envs/t_env_pool stand-ins. */
    Env *defer_env = env_new(NULL);
    Value defer_str = gc_string(a, "reached through a tracked defer scope");
    env_define(defer_env, "d", type_scalar(TY_STRING), 0, defer_str);
    Env *defer_envs[] = {defer_env};

    Env *pooled = env_new(NULL); /* a real pooled Env has no bindings left -- env_reset already cleared them */
    Env *env_pool[] = {pooled};

    gc_mark_interpreter_roots(a, global, defer_envs, 1, env_pool, 1);

    check("the global scope's own binding is marked", gc_is_marked(global_str.as.str.ptr));
    check("the outer active frame's own binding is marked", gc_is_marked(frame1_str.as.str.ptr));
    check("the inner active frame's own binding is marked too", gc_is_marked(frame2_str.as.str.ptr));
    check("a tracked defer scope's own binding is marked", gc_is_marked(defer_str.as.str.ptr));
    check("a native call's frame (no env) does not crash the walk", 1);
    check("an empty pooled env does not crash the walk", 1);

    ziman_pop_frame();
    ziman_pop_frame();
    ziman_pop_frame();
    ziman_call_stack_release();

    env_free(global);
    env_free(frame1_env);
    env_free(frame2_env);
    env_free(defer_env);
    env_free(pooled);
    arena_destroy(a);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
