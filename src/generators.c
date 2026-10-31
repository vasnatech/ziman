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

/* The generators, as one program. Named here rather than discovered, because these are the
 * compiler's own; a project's generator is found through the importing module's `use` list,
 * which is docs/design/user-generators.md's design and is not built. */
static const char *const GENERATOR_MODULES[] = {"derive_ord", "derive_row", "derive_properties",
                                                "derive_json", "derive_reflect", "derive_entity",
                                                "derive_validate"};

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
    /* **Kept alive past the next program's start** (F247). The generator program is cached for the
     * process, so its globals outlive the `g_global_env` that built them -- and the collector roots
     * only the current one. */
    ziman_keep_globals(m->globals);
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

/* ---- a project's own generator ------------------------------------------------------------
 *
 * **Discovery is the importing module's `use` list, and nothing else** (§3 of
 * docs/design/user-generators.md): `#[derive(Csv)]` finds a loaded module carrying
 * `#![generator(Csv)]` among this module's imports, and falls back to the bundled set. No
 * manifest, no search path, no registry -- the file that wants a derive imports its generator the
 * way it imports anything else, and a reader can see where `Csv` comes from without leaving the
 * file. `UseDecl.resolved` is what the loader leaves behind so this needs no `Loader`.
 *
 * **§6 called initialisation "the real cost" and it is no longer one.** A generator's
 * `val sheet = template.must_parse(..)` has to be initialised before `generate` runs, and
 * generation happens *during* loading, so the rule §6 asks for is "a generator module's globals
 * are initialised on first use, into an environment that outlives the load". That is exactly what
 * F247 built for the bundled program: `ziman_init_module_globals` records the env on
 * `Module.globals`, `ziman_keep_globals` keeps it rooted past the program that follows, and
 * `ziman_call_in_module` swaps to it. So this is three lines rather than the intricate thing §6
 * expected.
 *
 * The ordering §6 relies on is still what makes it safe: generation for a module runs after its
 * dependencies are loaded to completion, so a generator named in the `use` list is fully parsed
 * and spliced before anything asks it to generate. */
/* One loaded program per user generator, keyed by its use path. */
typedef struct {
    const char *path;
    Module *program;
} UserGenerator;

static UserGenerator g_user_gens[32];
static int g_user_gen_count = 0;

static Module *user_generator_program(const char *path) {
    for (int i = 0; i < g_user_gen_count; i++) {
        if (strcmp(g_user_gens[i].path, path) == 0) return g_user_gens[i].program;
    }
    return NULL;
}

/* The project root a `use <path>` inside the generator resolves against: its file with
 * `/<path>.zm` taken off the end. `/proj/derive/csv.zm` imported as `derive/csv` gives `/proj`. */
static char *root_of(const char *source_file, const char *path) {
    size_t flen = strlen(source_file);
    size_t plen = strlen(path);
    if (flen < plen + 4) return NULL;
    const char *tail = source_file + flen - (plen + 3);
    if (strncmp(tail, path, plen) != 0 || strcmp(tail + plen, ".zm") != 0) return NULL;
    size_t rlen = (size_t)(tail - source_file);
    while (rlen > 1 && source_file[rlen - 1] == '/') rlen--;
    char *root = ziman_xmalloc(rlen + 1);
    memcpy(root, source_file, rlen);
    root[rlen] = '\0';
    return root;
}

/* The module among `module`'s imports that claims `target`, or NULL. A module may claim one
 * target (`#![generator(X)]` twice in a file is refused at the attribute), so the first match is
 * the only match. */
static Module *find_user_generator(Module *module, const char *target, const char **out_path) {
    if (module == NULL || target == NULL) return NULL;
    for (int i = 0; i < module->uses.count; i++) {
        Module *used = module->uses.items[i].resolved;
        if (used == NULL || used->generator_target == NULL) continue;
        /* A bundled generator reached through `use` is not a *user* generator: it belongs to the
         * cached generator program, which is already initialised and already the fallback. */
        if (used->is_corelib) continue;
        if (strcmp(used->generator_target, target) == 0) {
            if (out_path != NULL) *out_path = module->uses.items[i].path;
            return used;
        }
    }
    return NULL;
}

/* **A user generator is loaded as its own program**, on first use, and that is not an
 * optimisation -- it is the only thing that works. `typecheck` runs in `main.c` after
 * `load_module_graph` returns and generation runs *during* loading, so a generator module sitting
 * in the user's own graph has resolved names and no resolved slots; the interpreter reads
 * `resolved_depth`/`resolved_slot` and segfaults. Typechecking that one module in place does not
 * fix it either: `typecheck` needs its dependencies' declarations merged in, so `meta::Decl` comes
 * back "unknown struct or enum type".
 *
 * So it gets what the bundled generators get: a synthetic entry importing it, loaded against the
 * project root, typechecked in full, then initialised -- `ziman_init_module_globals` records the
 * env on `Module.globals`, `ziman_keep_globals` keeps it rooted past the program that follows, and
 * `ziman_call_in_module` swaps to it (all three built by F247). §6 of the design called
 * initialisation "the real cost" and expected something intricate; the cost turned out to be this
 * ordering fact, which §6 does not mention.
 *
 * Reentrant on purpose: this runs while the user's own graph is mid-load, which `load_generators`
 * has always done from the same place. */
static Module *ready_user_generator(const char *path, const char *source_file, int line, int col) {
    Module *found = user_generator_program(path);
    if (found != NULL) return found;
    if (g_user_gen_count == (int)(sizeof g_user_gens / sizeof g_user_gens[0])) return NULL;
    if (source_file == NULL) return NULL;
    char *root = root_of(source_file, path);
    if (root == NULL) return NULL;

    char *whole = ziman_xmalloc(strlen(path) + 32);
    sprintf(whole, "use %s\nfn main() {\n}\n", path);
    const char *entry = "<user-generator>";
    const char *text = whole;
    const char *caller_file = ziman_error_file();
    int outer_errors = ziman_errors_begin_nested();
    loader_set_overrides(&entry, &text, 1);
    Module *program = load_module_graph(entry, root);
    loader_set_overrides(NULL, NULL, 0);
    typecheck(program);
    ziman_init_module_globals(program);
    ziman_keep_globals(program->globals);
    ziman_errors_end_nested(outer_errors);
    ziman_set_error_file(caller_file);
    (void)line;
    (void)col;

    g_user_gens[g_user_gen_count].path = ziman_xstrdup(path);
    g_user_gens[g_user_gen_count].program = program;
    g_user_gen_count++;
    return program;
}

int ziman_has_ziman_generator(Module *module, const char *target) {
    if (find_user_generator(module, target, NULL) != NULL) return 1;
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

/* A field's attributes, with their arguments -- the whole list the parser read, in source order.
 *
 * **The parser's list, which is now the only copy.** `Param` carried `is_pk` and `column_name`
 * beside it until 2026-10-30, for the C that wanted one question answered rather than a list
 * walked. Nothing in `src/` reads an attribute's meaning any more: a generator sees every
 * attribute it was given and decides, which is what lets a constraint be a library's business
 * rather than the compiler's. */
static Value reify_field(const Param *f, int line, int col) {
    Value *attrs = ziman_xmalloc(sizeof(Value) * (size_t)(f->attrs.count > 0 ? f->attrs.count : 1));
    for (int i = 0; i < f->attrs.count; i++) {
        const FieldAttr *one = &f->attrs.items[i];
        Value *args = ziman_xmalloc(sizeof(Value) * (size_t)(one->arg_count > 0 ? one->arg_count : 1));
        for (int a = 0; a < one->arg_count; a++) {
            Value pair[3] = {value_string(ziman_xstrdup(one->args[a].name)),
                             value_string(ziman_xstrdup(one->args[a].value)),
                             value_string(ziman_xstrdup(one->args[a].kind))};
            args[a] = call_meta("attr_arg", pair, 3, line, col);
        }
        if (one->arg_count == 0) {
            Value a[1] = {value_string(ziman_xstrdup(one->name))};
            attrs[i] = call_meta("attr", a, 1, line, col);
        } else {
            Value a[2] = {value_string(ziman_xstrdup(one->name)), array_of(args, one->arg_count)};
            attrs[i] = call_meta("attr_with", a, 2, line, col);
        }
    }

    Value args[3];
    args[0] = value_string(ziman_xstrdup(f->name));
    args[1] = reify_type(f->type, line, col);
    args[2] = array_of(attrs, f->attrs.count);
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

static Value reify_decl(StructDecl *sd, EnumDecl *ed, const FieldAttr *attr, int line, int col) {
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
    /* The target's own arguments, through the same `meta.attr_arg` a field attribute's go
     * through -- name, text as written, and which kind it was written as. */
    int targc = attr != NULL ? attr->arg_count : 0;
    Value *targs = ziman_xmalloc(sizeof(Value) * (size_t)(targc > 0 ? targc : 1));
    for (int i = 0; i < targc; i++) {
        Value pair[3] = {value_string(ziman_xstrdup(attr->args[i].name)),
                         value_string(ziman_xstrdup(attr->args[i].value)),
                         value_string(ziman_xstrdup(attr->args[i].kind))};
        targs[i] = call_meta("attr_arg", pair, 3, line, col);
    }
    args[2] = array_of(targs, targc);
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
static char *run_generator_in(Module *module, const char *fn, Value *args, int argc,
                              const char *what, const char *caller_file, int line, int col) {
    Value out;
    const char *problem = NULL;
    int ok = ziman_call_in_module(module, fn, args, argc, &out, &problem);
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

/* The same, in the cached generator program -- which is where every bundled generator lives. */
static char *run_generator(const char *fn, Value *args, int argc, const char *what,
                           const char *caller_file, int line, int col) {
    return run_generator_in(g_generators, fn, args, argc, what, caller_file, line, col);
}

/* ---- what attributes exist ---------------------------------------------------------------
 *
 * **The compiler holds no list of field attributes.** `src/parser.c` held one until 2026-10-30 --
 * eleven names and their argument grammars -- which meant a constraint was a line in the compiler
 * as well as in its generator, and a project could not have an attribute of its own at all. Each
 * generator declares its own set instead, as `pub fn attributes() -> [meta.AttrSpec]`, and this
 * collects them.
 *
 * A generator that owns no field attribute simply does not export it, and the call failing is how
 * that is read -- which does mean a *fault* inside an `attributes()` would look like "this
 * generator has none". The failure is loud rather than quiet: every attribute it owns becomes
 * "unknown field attribute" at the first field that carries one.
 *
 * Collected once. The bundled generators are a fixed set, so the answer cannot change within a
 * run; a project's own generator (docs/design/user-generators.md §5a) is found through the
 * importing module's `use` list and will want this keyed per module rather than globally. */
static ZimanAttrSpec *g_attr_specs = NULL;
static int g_attr_spec_count = 0;
static int g_attr_specs_tried = 0;

static void collect_attr_specs_from(const char *mod) {
    char *fn = ziman_xmalloc(strlen(mod) + 16);
    sprintf(fn, "%s::attributes", mod);
    Value out;
    const char *problem = NULL;
    int ok = ziman_call_in_module(g_generators, fn, NULL, 0, &out, &problem);
    free(fn);
    if (!ok || out.type == NULL || out.type->kind != TY_ARRAY || out.as.arr == NULL) return;
    /* An array of structs is never one of the packed buffers, so `boxed_items` is the storage. */
    if (out.as.arr->boxed_items == NULL) return;
    for (int i = 0; i < out.as.arr->count; i++) {
        Value one = out.as.arr->boxed_items[i];
        if (one.type == NULL || one.type->kind != TY_STRUCT || one.as.st == NULL) continue;
        char *name = NULL;
        char *args = NULL;
        for (int f = 0; f < one.as.st->count; f++) {
            const char *fname = one.as.st->field_names[f];
            Value v = one.as.st->fields[f];
            if (v.type == NULL || v.type->kind != TY_STRING) continue;
            /* Copied, not borrowed: a Ziman string points into a buffer the generator's
             * environment owns, and this table outlives the call. `copy_string`'s own comment. */
            if (strcmp(fname, "name") == 0) name = copy_string(v);
            if (strcmp(fname, "args") == 0) args = copy_string(v);
        }
        if (name == NULL || args == NULL) continue;
        g_attr_specs = ziman_xrealloc(g_attr_specs, (size_t)(g_attr_spec_count + 1) * sizeof *g_attr_specs);
        g_attr_specs[g_attr_spec_count].name = name;
        g_attr_specs[g_attr_spec_count].args = args;
        g_attr_spec_count++;
    }
}

const ZimanAttrSpec *ziman_generator_attr_specs(int *count) {
    if (!g_attr_specs_tried) {
        g_attr_specs_tried = 1;
        const char *caller_file = ziman_error_file();
        load_generators();
        ziman_set_error_file(caller_file);
        if (g_generators != NULL) {
            for (size_t i = 0; i < sizeof GENERATOR_MODULES / sizeof GENERATOR_MODULES[0]; i++) {
                collect_attr_specs_from(GENERATOR_MODULES[i]);
            }
            for (size_t i = 0; i < sizeof MODULE_GENERATOR_MODULES / sizeof MODULE_GENERATOR_MODULES[0]; i++) {
                collect_attr_specs_from(MODULE_GENERATOR_MODULES[i]);
            }
        }
        ziman_set_error_file(caller_file);
    }
    *count = g_attr_spec_count;
    return g_attr_specs;
}

/* The field attributes **this module** may carry: the bundled generators' union, plus the ones
 * declared by every generator in its own `use` list (§5a's per-module half).
 *
 * **Why per module and not one global list.** `#[csv_column(name:)]` means something in a file
 * that imports `derive/csv` and nothing in a file that does not -- which is the same rule
 * discovery follows for the target itself (§3), and the thing that keeps a project's attribute
 * from becoming vocabulary. A global union would make one project's attribute legal everywhere.
 *
 * Built fresh per call rather than cached: the answer depends on the module, the list is a dozen
 * entries, and `ziman_check_field_attributes` asks once per module and only when that module has
 * a field attribute at all.
 *
 * **A name claimed twice is refused at the importing module**, naming both generators. A
 * generator cannot namespace its attributes -- `#[csv.column]` is not a shape the grammar has --
 * so the collision is the importer's to resolve, by not importing both. */
const ZimanAttrSpec *ziman_module_attr_specs(Module *module, int *count) {
    int bundled_count = 0;
    const ZimanAttrSpec *bundled = ziman_generator_attr_specs(&bundled_count);
    if (module == NULL) {
        *count = bundled_count;
        return bundled;
    }

    static ZimanAttrSpec *merged = NULL;
    static int merged_cap = 0;
    int n = 0;
    if (merged_cap < bundled_count) {
        merged_cap = bundled_count + 32;
        merged = ziman_xrealloc(merged, (size_t)merged_cap * sizeof *merged);
    }
    for (int i = 0; i < bundled_count; i++) merged[n++] = bundled[i];

    for (int u = 0; u < module->uses.count; u++) {
        Module *used = module->uses.items[u].resolved;
        if (used == NULL || used->generator_target == NULL || used->is_corelib) continue;
        Module *program = ready_user_generator(module->uses.items[u].path, used->source_file, 0, 0);
        if (program == NULL) continue;
        int before = g_attr_spec_count;
        /* `collect_attr_specs_from` appends to the global table, which is where the strings live;
         * the entries it adds for a user generator are read out here and are harmless there. */
        Module *saved = g_generators;
        g_generators = program;
        char *fn = ziman_xmalloc(strlen(module->uses.items[u].path) + 16);
        sprintf(fn, "%s", module->uses.items[u].path);
        collect_attr_specs_from(fn);
        free(fn);
        g_generators = saved;
        for (int i = before; i < g_attr_spec_count; i++) {
            for (int j = 0; j < n; j++) {
                if (strcmp(merged[j].name, g_attr_specs[i].name) == 0) {
                    ziman_error("syntax", module->uses.items[u].line, module->uses.items[u].col,
                                "two generators this module imports both declare the field attribute "
                                "'%s' -- '%s' is one of them, and a generator cannot qualify its own "
                                "attributes, so one of the two imports has to go",
                                g_attr_specs[i].name, module->uses.items[u].path);
                }
            }
            if (n == merged_cap) {
                merged_cap *= 2;
                merged = ziman_xrealloc(merged, (size_t)merged_cap * sizeof *merged);
            }
            merged[n++] = g_attr_specs[i];
        }
    }
    *count = n;
    return merged;
}

/* ---------------------------------------------------------- the generated-code memo
 *
 * A declaration's derived text is a function of the declaration and the generator, and nothing
 * else. The declaration is fixed by the source text it was parsed from, so an unchanged module
 * means an unchanged answer -- the same property that makes `--emit-derived serve.zm`
 * byte-identical across unrelated compiler changes, which this repository checks on every such
 * change. This memo is that property spent.
 *
 * **Measured, which is why it exists** (docs/design/user-generators.md §8): `--lsp` reparses the
 * whole graph on every `didChange`, and on the North Star -- 227 files, 220 declarations carrying
 * a derive, 292 generator calls -- a reparse of the entry module cost **1,847 ms**, of which
 * callgrind put 51.7% in the derive pass and 41.3% in `ziman_call_in_module`, the interpreter
 * running generator code written in Ziman. After an edit in one file, 291 of those 292 calls
 * produce exactly what they produced before.
 *
 * **The key is the whole of what a generator can see.** The module's source hash covers the
 * declaration, its attributes and the `use` list the per-module attribute specs come from; the
 * declaration's own name picks it out within that module (so a hash collision alone cannot serve
 * one declaration's code for another's); the target names the generator; and a project's own
 * generator contributes its own module's source hash, so editing a generator invalidates
 * everything it produced. A bundled generator contributes 0: its source is in this binary.
 *
 * **What it assumes: a generator is pure.** One that read the clock, an environment variable or a
 * file would be cached wrongly -- but it would also already break `--emit-derived`'s byte
 * identity, so this depends on something the project already requires rather than on something
 * new. §8's option (3), restricting what a generator may reach, is what would make it enforced.
 *
 * In-process only. A stale entry is unreachable rather than wrong, because the key is content --
 * but it must not be *kept*, which the first version got wrong: every keystroke gives the edited
 * module a new hash, so its entries accumulated for the life of the server and the scan below got
 * slower the longer the session ran. An entry is therefore replaced when the same declaration of
 * the same file is generated again under a different hash, which caps the table at one entry per
 * (file, declaration, target) -- the size of the project rather than the length of the session. A
 * bundled module needs no eviction: its text cannot change while the process runs. */
typedef struct {
    unsigned long long module_hash;
    unsigned long long generator_hash;
    const char *source_file;  /* the module's file, or NULL for a bundled one: the eviction key */
    const char *target;      /* interned by the caller: a derive target name from the AST */
    const char *decl_name;   /* likewise */
    const char *attr_name;   /* the field attribute, for a generator keyed on one; NULL otherwise */
    char *text;              /* the generated source, owned here */
} GeneratedMemo;

static GeneratedMemo *g_memo = NULL;
static int g_memo_count = 0, g_memo_cap = 0;

static int memo_same(const GeneratedMemo *m, unsigned long long mh, unsigned long long gh,
                     const char *target, const char *decl_name, const char *attr_name) {
    if (m->module_hash != mh || m->generator_hash != gh) return 0;
    if (strcmp(m->target, target) != 0) return 0;
    if (strcmp(m->decl_name, decl_name) != 0) return 0;
    if ((m->attr_name == NULL) != (attr_name == NULL)) return 0;
    return m->attr_name == NULL || strcmp(m->attr_name, attr_name) == 0;
}

static char *memo_get(unsigned long long mh, unsigned long long gh, const char *target,
                      const char *decl_name, const char *attr_name) {
    if (mh == 0) return NULL;   /* a module with no source text of its own: nothing to key on */
    for (int i = 0; i < g_memo_count; i++) {
        if (memo_same(&g_memo[i], mh, gh, target, decl_name, attr_name)) {
            return ziman_xstrdup(g_memo[i].text);
        }
    }
    return NULL;
}

static int memo_same_slot(const GeneratedMemo *m, const char *source_file, const char *target,
                          const char *decl_name, const char *attr_name) {
    if ((m->source_file == NULL) != (source_file == NULL)) return 0;
    if (m->source_file != NULL && strcmp(m->source_file, source_file) != 0) return 0;
    if (strcmp(m->target, target) != 0 || strcmp(m->decl_name, decl_name) != 0) return 0;
    if ((m->attr_name == NULL) != (attr_name == NULL)) return 0;
    return m->attr_name == NULL || strcmp(m->attr_name, attr_name) == 0;
}

static void memo_put(unsigned long long mh, unsigned long long gh, const char *source_file,
                     const char *target, const char *decl_name, const char *attr_name,
                     const char *text) {
    if (mh == 0 || text == NULL) return;
    /* The same declaration of the same file, generated again after an edit: replace rather than
     * append, so the table is the size of the project and not of the session. */
    for (int i = 0; i < g_memo_count; i++) {
        if (!memo_same_slot(&g_memo[i], source_file, target, decl_name, attr_name)) continue;
        free(g_memo[i].text);
        g_memo[i].module_hash = mh;
        g_memo[i].generator_hash = gh;
        g_memo[i].text = ziman_xstrdup(text);
        return;
    }
    if (g_memo_count == g_memo_cap) {
        g_memo_cap = g_memo_cap == 0 ? 64 : g_memo_cap * 2;
        g_memo = ziman_xrealloc(g_memo, (size_t)g_memo_cap * sizeof(GeneratedMemo));
    }
    g_memo[g_memo_count].module_hash = mh;
    g_memo[g_memo_count].generator_hash = gh;
    g_memo[g_memo_count].source_file = source_file;
    g_memo[g_memo_count].target = target;
    g_memo[g_memo_count].decl_name = decl_name;
    g_memo[g_memo_count].attr_name = attr_name;
    g_memo[g_memo_count].text = ziman_xstrdup(text);
    g_memo_count++;
}

char *ziman_generate_in_ziman(Module *module, const char *target, StructDecl *sd, EnumDecl *ed, const FieldAttr *attr, int line, int col) {
    if (!ziman_has_ziman_generator(module, target)) return NULL;
    const char *user_path = NULL;
    Module *user = find_user_generator(module, target, &user_path);
    /* The memo, looked up before the generator program is loaded, which is itself part of the cost
     * on a bundled target's first call. `user` is resolved first because its own source hash is
     * part of the key. */
    const char *decl_name = sd != NULL ? sd->name : (ed != NULL ? ed->name : NULL);
    const char *attr_name = attr != NULL ? attr->name : NULL;
    unsigned long long module_hash = module->source_hash;
    unsigned long long generator_hash = user != NULL ? user->source_hash : 0;
    if (decl_name != NULL) {
        char *hit = memo_get(module_hash, generator_hash, target, decl_name, attr_name);
        if (hit != NULL) return hit;
    }
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
    Value decl = reify_decl(sd, ed, attr, line, col);
    native_gc_unsafe_leave();
    char what[96];
    snprintf(what, sizeof what, "the generator for '%s'", target);
    /* A project's own generator is its own program: initialised on first use and called in itself,
     * where a bundled one is a function of the cached generator program. */
    if (user != NULL) {
        Module *program = ready_user_generator(user_path, user->source_file, line, col);
        if (program == NULL) {
            ziman_error("module", line, col,
                        "the generator for '%s' is '%s', which could not be loaded as a program of "
                        "its own -- a generator is run before the program that derives it, so it is "
                        "loaded and type-checked separately",
                        target, user_path);
        }
        char *fn = ziman_xmalloc(strlen(user_path) + 16);
        sprintf(fn, "%s::generate", user_path);
        char *text = run_generator_in(program, fn, &decl, 1, what, caller_file, line, col);
        if (decl_name != NULL) {
            memo_put(module_hash, generator_hash, module->source_file, target, decl_name, attr_name, text);
        }
        return text;
    }
    char *text = run_generator(generate_name_for(target), &decl, 1, what, caller_file, line, col);
    if (decl_name != NULL) {
        memo_put(module_hash, generator_hash, module->source_file, target, decl_name, attr_name, text);
    }
    return text;
}

/* ---- the second generator kind: a module generator ----------------------------------------
 *
 * `#![derive(Repository(Entity))]`, whose input is a `meta.Target` rather than a `meta.Decl`: see
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
        label = call_meta("some_label", a, 1, line, col);
    } else {
        label = call_meta("no_label", NULL, 0, line, col);
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
