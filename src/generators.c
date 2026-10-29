#define _POSIX_C_SOURCE 200809L
#include "generators.h"

#include "corelib.h"
#include "derive.h"
#include "error.h"
#include "interpreter.h"
#include "loader.h"
#include "native.h"
#include "typecheck.h"
#include "value.h"
#include "xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The generators, as one program. Named here rather than discovered, because these five are the
 * compiler's own; a project's generator is found through the importing module's `use` list,
 * which is docs/design/user-generators.md's design and is not built. */
static const char *const GENERATOR_MODULES[] = {"derive_ord", "derive_row", "derive_properties",
                                                "derive_json", "derive_reflect", "derive_entity"};

/* The module generators -- the second kind (docs/design/meta.md §8). One, so far, and the list
 * exists anyway: `load_generators` has to import it, and a second module-level feature should
 * change this line rather than that function. */
static const char *const MODULE_GENERATOR_MODULES[] = {"derive_repository"};

static Module *g_generators = NULL;
static int g_tried = 0;

/* A synthetic entry that imports every generator, loaded through the override hook the LSP
 * already uses for unsaved buffers. **The project root is a path that cannot exist**, so every
 * `use` falls through to the bundled source rather than finding a file of the same name beside
 * some real directory -- the shadowing that `tests/lang/derive_properties.zm` demonstrated. */
static void load_generators(void) {
    if (g_tried) return;
    g_tried = 1;

    char *src = ziman_xstrdup("");
    for (size_t i = 0; i < sizeof GENERATOR_MODULES / sizeof GENERATOR_MODULES[0]; i++) {
        char *next = ziman_xmalloc(strlen(src) + strlen(GENERATOR_MODULES[i]) + 8);
        sprintf(next, "%suse %s\n", src, GENERATOR_MODULES[i]);
        free(src);
        src = next;
    }
    for (size_t i = 0; i < sizeof MODULE_GENERATOR_MODULES / sizeof MODULE_GENERATOR_MODULES[0]; i++) {
        char *next = ziman_xmalloc(strlen(src) + strlen(MODULE_GENERATOR_MODULES[i]) + 8);
        sprintf(next, "%suse %s\n", src, MODULE_GENERATOR_MODULES[i]);
        free(src);
        src = next;
    }
    char *whole = ziman_xmalloc(strlen(src) + 32);
    sprintf(whole, "%sfn main() {\n}\n", src);
    free(src);

    const char *path = "<generators>";
    const char *text = whole;
    /* The user's program may already have recorded errors; they are not this program's, and
     * parsing ends by exiting when the tally is non-empty. */
    int outer_errors = ziman_errors_begin_nested();
    loader_set_overrides(&path, &text, 1);
    Module *m = load_module_graph(path, "/nonexistent-generator-root");
    loader_set_overrides(NULL, NULL, 0);
    typecheck(m);
    ziman_init_module_globals(m);
    ziman_errors_end_nested(outer_errors);
    g_generators = m;
}

/* `derive_ord::generate` -- the qualified name a loaded module's declarations carry. */
static char *generate_name_for(const char *target) {
    char *snake = ziman_pascal_to_snake(target);
    char *out = ziman_xmalloc(strlen(snake) + 32);
    sprintf(out, "derive_%s::generate", snake);
    free(snake);
    return out;
}

int ziman_has_ziman_generator(const char *target) {
    char *snake = ziman_pascal_to_snake(target);
    char *mod = ziman_xmalloc(strlen(snake) + 8);
    sprintf(mod, "derive_%s", snake);
    int found = 0;
    for (size_t i = 0; i < sizeof GENERATOR_MODULES / sizeof GENERATOR_MODULES[0]; i++) {
        if (strcmp(GENERATOR_MODULES[i], mod) == 0) found = 1;
    }
    free(snake);
    free(mod);
    return found;
}


/* The name of the declaration being derived, with both qualifiers stripped: the loader's
 * `uuid::Uuid` and a source's `uuid.Uuid` both answer `Uuid`. A declaration is derived in the
 * module that declares it, so its own name is written bare in the code generated beside it. */
static const char *plain_name(const char *name) {
    if (name == NULL) return "";
    const char *sep = strstr(name, "::");
    const char *bare = sep != NULL ? sep + 2 : name;
    const char *dot = strrchr(bare, '.');
    return dot != NULL ? dot + 1 : bare;
}

/* A *field's* type name as the source spells it: the loader's internal `uuid::Uuid` answers
 * `uuid.Uuid`, and a name already written that way is passed through.
 *
 * **The module qualifier is kept, and that is the whole point.** This text is written into the
 * user's file, where `Option<time.Instant>` resolves and a bare `Option<Instant>` does not. The
 * first version of this used `plain_name` here too, which type-checked for every field whose
 * type name never reaches the output -- a flat `at: time.Instant` goes through `to_instant(..)`
 * and names no type -- and broke the moment one did, as `Option<time.Instant>`. A generator that
 * wants the bare name for a *lookup* strips it itself, which is what `meta.bare_name` is for and
 * what `standard_flat_type_name` has always done on the C side. */
static const char *source_name(const char *name) {
    if (name == NULL) return "";
    const char *sep = strstr(name, "::");
    return sep != NULL ? sep + 2 : name;
}

/* A `[T]` holding `items`. **The items are copied onto the heap**, because `array_obj_new`
 * borrows the pointer rather than copying it -- handing it a stack array leaves the ArrayObj
 * pointing at a dead frame, which is what the first version of this did. It survived every
 * hand-built fixture and failed the moment a generator read an element back: "field 'kind' not
 * found at runtime", from a `MetaType` that was no longer there.
 *
 * The element type is only ever read for diagnostics: this call path bypasses the type checker,
 * which has already passed over the generator's own signature. */
static Value array_of(const Value *items, int count) {
    Value *owned = ziman_xmalloc(sizeof(Value) * (size_t)(count > 0 ? count : 1));
    for (int i = 0; i < count; i++) owned[i] = items[i];
    return value_array(type_ref(type_array(type_scalar(TY_VOID))), array_obj_new(owned, count));
}

/* A Ziman string as a NUL-terminated C string the AST can own. A Ziman string is a pointer and
 * a length into a shared buffer and is not terminated, so this copies rather than borrowing --
 * and the copy has to happen before the generator's environment is reclaimed. */
static char *copy_string(Value v) {
    size_t n = (size_t)v.as.str.len;
    char *out = ziman_xmalloc(n + 1);
    memcpy(out, v.as.str.ptr, n);
    out[n] = '\0';
    return out;
}

static Value call_meta(const char *name, Value *args, int argc, int line, int col) {
    Value out;
    const char *problem = NULL;
    char full[64];
    snprintf(full, sizeof full, "meta::%s", name);
    if (!ziman_call_in_module(g_generators, full, args, argc, &out, &problem)) {
        ziman_error("module", line, col, "reifying a declaration for a generator failed in '%s': %s",
                    full, problem != NULL ? problem : "unknown");
    }
    return out;
}

static Value reify_type(Type t, int line, int col) {
    /* **A total switch with no `default:`**, so `-Wswitch` makes a new `TypeKind` a build
     * warning here rather than a type that silently arrives as something else. That is the
     * producer half of the guarantee; the consumer half is that `meta.Type` is an enum, so a
     * generator cannot ignore a variant either (docs/design/meta.md §5). */
    switch (t.kind) {
        case TY_INT: return call_meta("int_type", NULL, 0, line, col);
        case TY_FLOAT: return call_meta("float_type", NULL, 0, line, col);
        case TY_BOOL: return call_meta("bool_type", NULL, 0, line, col);
        case TY_STRING: return call_meta("string_type", NULL, 0, line, col);
        case TY_BIGINT: return call_meta("bigint_type", NULL, 0, line, col);
        case TY_DECIMAL: return call_meta("decimal_type", NULL, 0, line, col);
        case TY_BYTE: return call_meta("byte_type", NULL, 0, line, col);
        case TY_UNIT: return call_meta("unit_type", NULL, 0, line, col);
        case TY_VOID: return call_meta("void_type", NULL, 0, line, col);
        case TY_ARRAY: {
            Value a[1] = {reify_type(t.elem != NULL ? *t.elem : type_scalar(TY_UNIT), line, col)};
            return call_meta("array_type", a, 1, line, col);
        }
        case TY_SET: {
            Value a[1] = {reify_type(t.elem != NULL ? *t.elem : type_scalar(TY_UNIT), line, col)};
            return call_meta("set_type", a, 1, line, col);
        }
        case TY_CHANNEL: {
            Value a[1] = {reify_type(t.elem != NULL ? *t.elem : type_scalar(TY_UNIT), line, col)};
            return call_meta("channel_type", a, 1, line, col);
        }
        case TY_MAP: {
            Value a[2] = {reify_type(t.elem != NULL ? *t.elem : type_scalar(TY_UNIT), line, col),
                          reify_type(t.elem_err != NULL ? *t.elem_err : type_scalar(TY_UNIT), line, col)};
            return call_meta("map_type", a, 2, line, col);
        }
        case TY_RESULT: {
            Value a[2] = {reify_type(t.elem != NULL ? *t.elem : type_scalar(TY_UNIT), line, col),
                          reify_type(t.elem_err != NULL ? *t.elem_err : type_scalar(TY_UNIT), line, col)};
            return call_meta("result_type", a, 2, line, col);
        }
        case TY_FUNC: {
            Value *params = ziman_xmalloc(sizeof(Value) * (size_t)(t.type_args.count > 0 ? t.type_args.count : 1));
            for (int i = 0; i < t.type_args.count; i++) params[i] = reify_type(t.type_args.items[i], line, col);
            Value a[2] = {array_of(params, t.type_args.count),
                          reify_type(t.elem != NULL ? *t.elem : type_scalar(TY_VOID), line, col)};
            return call_meta("func_type", a, 2, line, col);
        }
        case TY_TYPEVAR: {
            Value a[1] = {value_string(ziman_xstrdup(t.type_var_name != NULL ? t.type_var_name : ""))};
            return call_meta("var_type", a, 1, line, col);
        }
        case TY_STRUCT:
        case TY_ENUM: {
            const char *name = source_name(t.kind == TY_STRUCT ? t.struct_name : t.enum_name);
            /* `Option<T>` is a struct in some positions and an enum in others -- the ambiguity
             * `is_option` used to apologise for. `meta` normalises it to one variant here, which
             * is the whole reason a generator can now ask the question by pattern. */
            if (strcmp(name, "Option") == 0 && t.type_args.count == 1) {
                Value a[1] = {reify_type(t.type_args.items[0], line, col)};
                return call_meta("optional_type", a, 1, line, col);
            }
            Value *inner = ziman_xmalloc(sizeof(Value) * (size_t)(t.type_args.count > 0 ? t.type_args.count : 1));
            for (int i = 0; i < t.type_args.count; i++) inner[i] = reify_type(t.type_args.items[i], line, col);
            Value a[2] = {value_string(ziman_xstrdup(name)), array_of(inner, t.type_args.count)};
            return call_meta("named_type", a, 2, line, col);
        }
    }
    ziman_error("module", line, col, "a type kind that meta does not model reached a generator");
    return call_meta("unit_type", NULL, 0, line, col);
}

/* A field's attributes, as bare names. `#[pk]` is the only one the parser records on a field
 * today -- `is_pk` on `Param` -- so this list is at most one long, and is a list anyway because
 * the second attribute should not change the shape of `meta.Field`. */
static Value reify_field(const Param *f, int line, int col) {
    Value attrs[1];
    int n = 0;
    if (f->is_pk) attrs[n++] = value_string(ziman_xstrdup("pk"));

    Value args[3];
    args[0] = value_string(ziman_xstrdup(f->name));
    args[1] = reify_type(f->type, line, col);
    args[2] = array_of(attrs, n);
    return call_meta("field", args, 3, line, col);
}

/* A variant is one of two constructors rather than a name beside a `has_payload` flag and a
 * `unit` standing in for absence -- the two spellings of "no payload" that `meta.Variant` now
 * has one of. */
static Value reify_variant(EnumVariant *v, int line, int col) {
    Value args[2];
    args[0] = value_string(ziman_xstrdup(v->name));
    if (!v->has_payload) return call_meta("unit_variant", args, 1, line, col);
    args[1] = reify_type(v->payload_type, line, col);
    return call_meta("payload_variant", args, 2, line, col);
}

static Value reify_decl(StructDecl *sd, EnumDecl *ed, const char *arg, int line, int col) {
    const char *name = sd != NULL ? sd->name : ed->name;
    TypeParamList *params = sd != NULL ? &sd->type_params : &ed->type_params;
    int count = sd != NULL ? sd->fields.count : ed->variants.count;

    Value *members = ziman_xmalloc(sizeof(Value) * (size_t)(count > 0 ? count : 1));
    for (int i = 0; i < count; i++) {
        members[i] = sd != NULL ? reify_field(&sd->fields.items[i], line, col)
                                : reify_variant(&ed->variants.items[i], line, col);
    }
    Value *names = ziman_xmalloc(sizeof(Value) * (size_t)(params->count > 0 ? params->count : 1));
    for (int i = 0; i < params->count; i++) {
        names[i] = value_string(ziman_xstrdup(params->items[i].name));
    }

    Value args[5];
    args[0] = value_string(ziman_xstrdup(plain_name(name)));
    args[1] = value_string(ziman_pascal_to_snake(plain_name(name)));
    if (arg != NULL) {
        Value a[1] = {value_string(ziman_xstrdup(arg))};
        args[2] = call_meta("some_arg", a, 1, line, col);
    } else {
        args[2] = call_meta("no_arg", NULL, 0, line, col);
    }
    args[3] = array_of(names, params->count);
    args[4] = array_of(members, count);
    return call_meta(sd != NULL ? "struct_decl" : "enum_decl", args, 5, line, col);
}

/* Calls one generator entry point and answers the text it returned.
 *
 * **The error file is restored after the call as well as before it**: running a generator sets it
 * to the generator's own module, and a refusal belongs to the user's file. `what` is the subject
 * of the two messages this raises -- "the generator for 'Ord'", "the repository generator" --
 * because the one thing a reader needs from either is which generator was running. */
static char *run_generator(const char *fn, Value *args, int argc, const char *what,
                           const char *caller_file, int line, int col) {
    Value out;
    const char *problem = NULL;
    int ok = ziman_call_in_module(g_generators, fn, args, argc, &out, &problem);
    ziman_set_error_file(caller_file);
    if (!ok) {
        ziman_error("module", line, col, "%s failed: %s", what, problem != NULL ? problem : "unknown");
    }
    /* A generator answers a `Result<string, string>`: an `Err` is it refusing, which is a mistake
     * in the declaration (or the signature) and belongs at that position. */
    int is_result = out.type != NULL && out.type->kind == TY_RESULT && out.as.res != NULL;
    if (is_result && !out.as.res->is_ok) {
        ziman_error("type", line, col, "%s", copy_string(out.as.res->value));
    }
    return copy_string(is_result ? out.as.res->value : out);
}

char *ziman_generate_in_ziman(const char *target, StructDecl *sd, EnumDecl *ed, const char *arg, int line, int col) {
    if (!ziman_has_ziman_generator(target)) return NULL;
    /* **Captured before anything else runs.** Loading the generator program and then running it
     * both set the error file to a generator's own module, and the declaration being derived is
     * in the *user's* file -- so a refusal reported below would name the wrong file, or none.
     * Worse, the first version restored a value read *after* `load_generators()`, which had
     * already overwritten it: `errors_derive_several` then reported one error where it expects
     * three, because the later ones were raised against a file the run was no longer in. */
    const char *caller_file = ziman_error_file();
    load_generators();
    ziman_set_error_file(caller_file);
    if (g_generators == NULL) return NULL;

    /* **Collection is held off while the declaration is reified.**
     *
     * `reify_decl` builds a `meta.Decl` out of dozens of `call_meta` calls -- a `MetaType` per
     * field, a `Member` wrapping each, an array of those -- and every one of them can allocate.
     * The wrappers come from the per-thread `GcHeap`, whose mark phase roots from the
     * interpreter's own frames, and a `Value` that exists only in this C function's locals and
     * in a buffer it malloc'd is not one of those roots. So a collection partway through the
     * build frees a `Member` that is finished but not yet referenced from anywhere Ziman can
     * see, and the generator later reads a field off it: *"field 'name' not found at runtime"*.
     *
     * It needs enough allocation to reach a collection, so every fixture passed and every small
     * file passed; `health-record`'s 140 derive sites hit it, and reported it against whichever
     * declaration happened to be live when the collector ran. `struct_obj_new`'s own comment
     * names this guard as the rule for callers that build a buffer across more than one call.
     *
     * The guard ends before the generator runs: `decl` is that call's argument, so from there it
     * is a parameter in the callee's environment and a root like any other. */
    native_gc_unsafe_enter();
    Value decl = reify_decl(sd, ed, arg, line, col);
    native_gc_unsafe_leave();
    char *fn = generate_name_for(target);
    char what[96];
    snprintf(what, sizeof what, "the generator for '%s'", target);
    return run_generator(fn, &decl, 1, what, caller_file, line, col);
}

/* ---- the second generator kind: a module generator ----------------------------------------
 *
 * `#![repository(Entity)]`, whose input is a `meta.Target` rather than a `meta.Decl`: see
 * generators.h and docs/design/meta.md §8. Everything above this line is shared with the
 * declaration kind -- the reified types, the one loaded generator program, the guard against a
 * collection landing inside a reification -- which is the argument for a second *kind* rather
 * than a second mechanism. */

/* A signature without its body, as `meta.Function`. The types are the source's own spelling,
 * which is what a generated body has to repeat: self-qualification runs after generation. */
static Value reify_function(FunctionDecl *fn, int line, int col) {
    Value *params = ziman_xmalloc(sizeof(Value) * (size_t)(fn->params.count > 0 ? fn->params.count : 1));
    for (int i = 0; i < fn->params.count; i++) {
        Value a[2] = {value_string(ziman_xstrdup(fn->params.items[i].name)),
                      reify_type(fn->params.items[i].type, line, col)};
        params[i] = call_meta("param", a, 2, line, col);
    }
    Value args[3] = {value_string(ziman_xstrdup(fn->name != NULL ? fn->name : "")),
                     array_of(params, fn->params.count),
                     reify_type(fn->return_type, line, col)};
    return call_meta("function", args, 3, line, col);
}

/* **The entity is reified by `reify_decl`, the same function the five derives go through.** A
 * `Target` is the module's context around a declaration, not a second kind of declaration, so
 * the part a generator reads most is the part that already had a fixture (`meta_boundary`). */
static Value reify_target(const ZimanTarget *t, int line, int col) {
    Value *defined = ziman_xmalloc(sizeof(Value) * (size_t)(t->defined_count > 0 ? t->defined_count : 1));
    for (int i = 0; i < t->defined_count; i++) {
        defined[i] = value_string(ziman_xstrdup(t->defined[i]));
    }
    Value label;
    if (t->label != NULL) {
        Value a[1] = {value_string(ziman_xstrdup(t->label))};
        label = call_meta("some_arg", a, 1, line, col);
    } else {
        label = call_meta("no_arg", NULL, 0, line, col);
    }
    Value args[4] = {reify_decl(t->entity, NULL, NULL, line, col), label, value_bool(t->crud),
                     array_of(defined, t->defined_count)};
    return call_meta("target", args, 4, line, col);
}

/* Reification holds off collection for its whole run, for the reason
 * `ziman_generate_in_ziman` records at length: a `Value` that exists only in this function's
 * locals is not a root, and a `Target` is more allocation than a `Decl`, not less. */
static Value reify_target_guarded(const ZimanTarget *t, int line, int col) {
    native_gc_unsafe_enter();
    Value v = reify_target(t, line, col);
    native_gc_unsafe_leave();
    return v;
}

char *ziman_generate_module_in_ziman(const ZimanTarget *t, int line, int col) {
    const char *caller_file = ziman_error_file();
    load_generators();
    ziman_set_error_file(caller_file);
    if (g_generators == NULL) return NULL;
    Value target = reify_target_guarded(t, line, col);
    return run_generator("derive_repository::generate_module", &target, 1,
                         "the repository generator", caller_file, line, col);
}

char *ziman_generate_query_in_ziman(const ZimanTarget *t, FunctionDecl *fn, int line, int col) {
    const char *caller_file = ziman_error_file();
    load_generators();
    ziman_set_error_file(caller_file);
    if (g_generators == NULL) return NULL;
    /* **One guard over both values**: the `Function` is the second argument of the same call, and
     * a collection landing between the two would sweep whichever was finished first -- the bug
     * `--gc-stress` exists for, in the one shape this file had not had yet. */
    native_gc_unsafe_enter();
    Value args[2] = {reify_target(t, line, col), reify_function(fn, line, col)};
    native_gc_unsafe_leave();
    char what[128];
    snprintf(what, sizeof what, "the repository generator, for '%s'", fn->name != NULL ? fn->name : "");
    return run_generator("derive_repository::generate_query", args, 2, what, caller_file, line, col);
}
