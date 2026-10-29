#define _POSIX_C_SOURCE 200809L
#include "generators.h"

#include "corelib.h"
#include "derive.h"
#include "error.h"
#include "interpreter.h"
#include "loader.h"
#include "typecheck.h"
#include "value.h"
#include "xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The generators, as one program. Named here rather than discovered, because these five are the
 * compiler's own; a project's generator is found through the importing module's `use` list,
 * which is docs/design/user-generators.md's design and is not built. */
static const char *const GENERATOR_MODULES[] = {"derive_ord", "derive_row", "derive_properties"};

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
    char *whole = ziman_xmalloc(strlen(src) + 32);
    sprintf(whole, "%sfn main() {\n}\n", src);
    free(src);

    const char *path = "<generators>";
    const char *text = whole;
    loader_set_overrides(&path, &text, 1);
    Module *m = load_module_graph(path, "/nonexistent-generator-root");
    loader_set_overrides(NULL, NULL, 0);
    typecheck(m);
    ziman_init_module_globals(m);
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

/* The compiler's `TypeKind` as `meta.MetaType.kind` spells it. One table rather than a switch so
 * that a kind added to the language is a compile error here (the array is indexed by kind) rather
 * than a silent "struct". */
static const char *kind_name(TypeKind k) {
    switch (k) {
        case TY_VOID: return "void";
        case TY_UNIT: return "unit";
        case TY_INT: return "int";
        case TY_FLOAT: return "float";
        case TY_STRING: return "string";
        case TY_BIGINT: return "bigint";
        case TY_DECIMAL: return "decimal";
        case TY_BOOL: return "bool";
        case TY_BYTE: return "byte";
        case TY_ARRAY: return "array";
        case TY_STRUCT: return "struct";
        case TY_RESULT: return "result";
        case TY_ENUM: return "enum";
        case TY_CHANNEL: return "channel";
        case TY_TYPEVAR: return "typevar";
        case TY_FUNC: return "func";
        case TY_MAP: return "map";
        case TY_SET: return "set";
    }
    return "unknown";
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
    Value args[3];
    if (t.kind == TY_ARRAY && t.elem != NULL) {
        Value inner[1] = {reify_type(*t.elem, line, col)};
        args[0] = value_string(ziman_xstrdup("array"));
        args[1] = value_string(ziman_xstrdup(""));
        args[2] = array_of(inner, 1);
        return call_meta("generic", args, 3, line, col);
    }
    if (t.kind == TY_STRUCT || t.kind == TY_ENUM) {
        const char *name = source_name(t.kind == TY_STRUCT ? t.struct_name : t.enum_name);
        if (t.type_args.count > 0) {
            Value *inner = ziman_xmalloc(sizeof(Value) * (size_t)t.type_args.count);
            for (int i = 0; i < t.type_args.count; i++) {
                inner[i] = reify_type(t.type_args.items[i], line, col);
            }
            args[0] = value_string(ziman_xstrdup(kind_name(t.kind)));
            args[1] = value_string(ziman_xstrdup(name));
            args[2] = array_of(inner, t.type_args.count);
            return call_meta("generic", args, 3, line, col);
        }
        args[0] = value_string(ziman_xstrdup(kind_name(t.kind)));
        args[1] = value_string(ziman_xstrdup(name));
        return call_meta("named", args, 2, line, col);
    }
    if (t.kind == TY_TYPEVAR) {
        /* A type variable carries its name -- `T` -- which a generator needs both to spell the
         * type and to name the converter parameter a generic `derive(Json)` threads through. */
        args[0] = value_string(ziman_xstrdup("typevar"));
        args[1] = value_string(ziman_xstrdup(t.type_var_name != NULL ? t.type_var_name : ""));
        return call_meta("named", args, 2, line, col);
    }
    args[0] = value_string(ziman_xstrdup(kind_name(t.kind)));
    return call_meta("scalar", args, 1, line, col);
}

static Value reify_member(const char *name, Type t, int has_payload, int line, int col) {
    Value args[3];
    args[0] = value_string(ziman_xstrdup(name));
    args[1] = reify_type(t, line, col);
    args[2] = value_bool(has_payload);
    return call_meta("member", args, 3, line, col);
}

static Value reify_decl(StructDecl *sd, EnumDecl *ed, int line, int col) {
    const char *name = sd != NULL ? sd->name : ed->name;
    TypeParamList *params = sd != NULL ? &sd->type_params : &ed->type_params;
    int count = sd != NULL ? sd->fields.count : ed->variants.count;

    Value *members = ziman_xmalloc(sizeof(Value) * (size_t)(count > 0 ? count : 1));
    for (int i = 0; i < count; i++) {
        if (sd != NULL) {
            members[i] = reify_member(sd->fields.items[i].name, sd->fields.items[i].type, 0, line, col);
        } else {
            EnumVariant *v = &ed->variants.items[i];
            Type payload = v->has_payload ? v->payload_type : type_scalar(TY_UNIT);
            members[i] = reify_member(v->name, payload, v->has_payload, line, col);
        }
    }
    Value *names = ziman_xmalloc(sizeof(Value) * (size_t)(params->count > 0 ? params->count : 1));
    for (int i = 0; i < params->count; i++) {
        names[i] = value_string(ziman_xstrdup(params->items[i].name));
    }

    char *snake = ziman_pascal_to_snake(plain_name(name));
    Value args[5];
    args[0] = value_string(ziman_xstrdup(plain_name(name)));
    args[1] = value_string(snake);
    args[2] = value_bool(ed != NULL);
    args[3] = array_of(names, params->count);
    args[4] = array_of(members, count);
    return call_meta("decl", args, 5, line, col);
}

char *ziman_generate_in_ziman(const char *target, StructDecl *sd, EnumDecl *ed, int line, int col) {
    if (!ziman_has_ziman_generator(target)) return NULL;
    load_generators();
    if (g_generators == NULL) return NULL;

    Value decl = reify_decl(sd, ed, line, col);
    char *fn = generate_name_for(target);
    Value out;
    const char *problem = NULL;
    int ok = ziman_call_in_module(g_generators, fn, &decl, 1, &out, &problem);
    if (!ok) {
        ziman_error("module", line, col, "the generator for '%s' failed: %s", target,
                    problem != NULL ? problem : "unknown");
    }
    /* `generate` answers a `Result<string, string>`: an `Err` is the generator refusing, which is
     * a mistake in the declaration and belongs at the declaration. */
    int is_result = out.type != NULL && out.type->kind == TY_RESULT && out.as.res != NULL;
    if (is_result && !out.as.res->is_ok) {
        ziman_error("type", line, col, "%s", copy_string(out.as.res->value));
    }
    return copy_string(is_result ? out.as.res->value : out);
}
