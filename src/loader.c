#define _POSIX_C_SOURCE 200809L
#include "loader.h"

#include "typecheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "corelib.h"
#include "derive.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "prelude.h"
#include "stack.h"
#include "xalloc.h"

/* ---- small file/path helpers ---- */

/* Unsaved editor buffers (the language server): a path listed here reads as this
 * text instead of what is on disk. */
static const char **g_override_paths = NULL;
static const char **g_override_texts = NULL;
static int g_override_count = 0;

void loader_set_overrides(const char **paths, const char **texts, int count) {
    g_override_paths = paths;
    g_override_texts = texts;
    g_override_count = count;
}

static char *read_all(const char *path) {
    for (int i = 0; i < g_override_count; i++) {
        if (strcmp(g_override_paths[i], path) == 0) return ziman_xstrdup(g_override_texts[i]);
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = ziman_xmalloc((size_t)size + 1);
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

static char *dirname_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) return ziman_xstrdup("");
    size_t len = (size_t)(slash - path);
    char *dir = ziman_xmalloc(len + 1);
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

/* The bare module name a path would have if something `use`d it: its filename, minus
 * directory and ".zm". Used only to recognize an entry file that is itself one of the bundled
 * corelib modules (see its one call site below) -- every other module's name instead comes
 * straight from the `use` path text that named it, never a path computation like this one. */
static char *module_name_of(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash != NULL ? slash + 1 : path;
    size_t len = strlen(base);
    if (len > 3 && strcmp(base + len - 3, ".zm") == 0) len -= 3;
    char *name = ziman_xmalloc(len + 1);
    memcpy(name, base, len);
    name[len] = '\0';
    return name;
}

static char *resolve_file_path(const char *project_root, const char *use_path) {
    size_t len = strlen(project_root) + strlen(use_path) + 6;
    char *out = ziman_xmalloc(len);
    if (project_root[0] == '\0') {
        snprintf(out, len, "%s.zm", use_path);
    } else {
        snprintf(out, len, "%s/%s.zm", project_root, use_path);
    }
    return out;
}

/* The directory of the nearest `ziman.toml` at or above `start`, or NULL when there is none within
 * eight levels -- a bound rather than a walk to `/`, so a stray marker in a home directory cannot
 * silently claim every file beneath it, the same reasoning as lsp.c's six-level `main.zm` hunt.
 *
 * ROADMAP.md asked for this marker so that a test under `test/` could `use` a module under `src/`,
 * and so that tooling could find a project's root without hunting for `main.zm`. The first half was
 * already solved by ZIMAN_MODULE_PATH below, so the marker does **not** introduce a second way to
 * resolve a module: its directory simply joins that same search path, which is why it is declared
 * here rather than next to the project root. One mechanism, two ways to point it at a directory --
 * an environment variable for a harness, a file in the tree for everyone else.
 *
 * **Its contents are not read.** `ziman.toml` is the name the roadmap reserved for a manifest that
 * does not exist yet; naming the marker now means that manifest later lands in the file already
 * marking the root instead of a second file appearing beside it. An empty file is the usual case. */
static char *find_project_marker(const char *start) {
    char *dir = ziman_xstrdup(start);
    for (int level = 0; level < 8; level++) {
        /* The "." substitution below is one byte longer than the empty `dir` it stands in for, so
         * size the buffer for what is actually written -- not for `dir`, which silently truncated
         * the probe to "./ziman.tom" and made the current-directory case fail. */
        const char *probe_dir = dir[0] == '\0' ? "." : dir;
        size_t len = strlen(probe_dir) + sizeof "/ziman.toml";
        char *probe = ziman_xmalloc(len);
        snprintf(probe, len, "%s/ziman.toml", probe_dir);
        FILE *f = fopen(probe, "rb");
        free(probe);
        if (f != NULL) {
            fclose(f);
            if (dir[0] == '\0') { /* the marker is in the current directory: return a usable path */
                free(dir);
                return ziman_xstrdup(".");
            }
            return dir;
        }
        char *slash = strrchr(dir, '/');
        if (slash == NULL) {
            /* A *relative* start -- `src/test/ziman`, which is how the README and every handwritten
             * invocation spell it -- runs out of slashes while one directory is still untried: the
             * current one. Without this the walk stopped at `src` and never probed `./ziman.toml`,
             * so the marker worked only when the entry file was named by an absolute path (which
             * always has a leading '/' to walk down to). F33 exists precisely so a project's tests
             * can import its modules, and that is the one invocation where it silently did nothing
             * (tasks.md F52). */
            if (dir[0] == '\0') break; /* "." has already been probed */
            dir[0] = '\0';
            continue;
        }
        *slash = '\0';
        if (dir[0] == '\0') break; /* reached the filesystem root */
    }
    free(dir);
    return NULL;
}

/* `use foo` looks for `foo.zm` beside the importing file and nowhere else, which is enough for a program
 * laid out in one tree and not enough for the convention every language this one borrows from uses for
 * tests: the tests in a directory of their own, importing the code they test. `src/test/ziman/calc_test.zm`
 * cannot say `use calc` when calc.zm is in `src/main/ziman/`, so the reference backend has no unit tests at
 * all -- `ziman test` works perfectly when the test file sits beside production code, which is not where
 * anyone wants to keep it.
 *
 * ZIMAN_MODULE_PATH is a colon-separated list of directories searched after the importing file's own, in
 * order, first match winning. Deliberately the same shape and spelling convention as ZIMAN_RESOURCE_PATH,
 * which already does this for resources and which the backend's own test harness already sets -- so this
 * is one more familiar variable rather than a new mechanism. The importing file's own directory is still
 * tried first, so nothing that resolves today resolves differently.
 *
 * Returns a readable path from the list, or NULL when none of them is readable (the caller then reports
 * using the project-relative name, which is the one the author wrote and expects to see). */
static char *resolve_via_module_path(const char *use_path, const char *marker_dir) {
    /* The project marker's own directory is tried first, before anything ZIMAN_MODULE_PATH names:
     * a file committed in the tree describes that tree better than an environment variable a
     * harness happened to export, and a harness that wants to win can still point the entry file
     * somewhere else. Both are tried only after the importing file's own directory, so nothing that
     * resolves today resolves differently. */
    if (marker_dir != NULL) {
        size_t len = strlen(marker_dir) + strlen(use_path) + 6;
        char *candidate = ziman_xmalloc(len);
        snprintf(candidate, len, "%s/%s.zm", marker_dir, use_path);
        FILE *f = fopen(candidate, "rb");
        if (f != NULL) {
            fclose(f);
            return candidate;
        }
        free(candidate);
    }
    const char *env = getenv("ZIMAN_MODULE_PATH");
    if (env == NULL || env[0] == '\0') return NULL;
    const char *p = env;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        size_t dir_len = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (dir_len > 0) {
            size_t len = dir_len + strlen(use_path) + 6;
            char *candidate = ziman_xmalloc(len);
            snprintf(candidate, len, "%.*s/%s.zm", (int)dir_len, p, use_path);
            FILE *f = fopen(candidate, "rb");
            if (f != NULL) {
                fclose(f);
                return candidate;
            }
            free(candidate);
        }
        if (colon == NULL) break;
        p = colon + 1;
    }
    return NULL;
}

/* "<use_path>::<bare_name>" -- the fully-qualified form every cross-module
 * reference (and every non-entry module's own declaration) is rewritten
 * to. Chosen so it can never collide with a bare (entry-module or
 * builtin) name, which never contains "::". */
static char *qualify_name(const char *use_path, const char *bare_name) {
    size_t len = strlen(use_path) + 2 + strlen(bare_name) + 1;
    char *q = ziman_xmalloc(len);
    snprintf(q, len, "%s::%s", use_path, bare_name);
    return q;
}

/* ---- the loader's module cache + in-progress stack (cycle detection) ---- */

typedef struct {
    char *use_path;
    Module *module;
} LoadedEntry;

typedef struct {
    const char *project_root;
    /* The nearest `ziman.toml`'s directory, or NULL -- see find_project_marker. Computed once per
     * load rather than per `use`, since it cannot change while a graph is loading. */
    char *marker_dir;
    LoadedEntry *loaded;
    int loaded_count;
    int loaded_cap;
    char **loading_stack;
    int loading_count;
    int loading_cap;
    /* Parsed once, up front, in load_module_graph -- see
     * resolve_type_refs's own comment for why every other module needs
     * to see this: a bare type reference to a prelude enum (`Option<T>`)
     * from *any* module, not just the entry one, needs reclassifying
     * from its provisional TY_STRUCT the same way an entry module's own
     * bare enum reference already does. */
    Module *prelude_mod;
} Loader;

static Module *find_loaded(Loader *ldr, const char *use_path) {
    for (int i = 0; i < ldr->loaded_count; i++) {
        if (strcmp(ldr->loaded[i].use_path, use_path) == 0) return ldr->loaded[i].module;
    }
    return NULL;
}

static void register_loaded(Loader *ldr, const char *use_path, Module *mod) {
    if (ldr->loaded_count == ldr->loaded_cap) {
        ldr->loaded_cap = ldr->loaded_cap == 0 ? 8 : ldr->loaded_cap * 2;
        ldr->loaded = ziman_xrealloc(ldr->loaded, (size_t)ldr->loaded_cap * sizeof(LoadedEntry));
    }
    ldr->loaded[ldr->loaded_count].use_path = ziman_xstrdup(use_path);
    ldr->loaded[ldr->loaded_count].module = mod;
    ldr->loaded_count++;
}

static int is_loading(Loader *ldr, const char *use_path) {
    for (int i = 0; i < ldr->loading_count; i++) {
        if (strcmp(ldr->loading_stack[i], use_path) == 0) return 1;
    }
    return 0;
}

static void push_loading(Loader *ldr, const char *use_path) {
    if (ldr->loading_count == ldr->loading_cap) {
        ldr->loading_cap = ldr->loading_cap == 0 ? 8 : ldr->loading_cap * 2;
        ldr->loading_stack = ziman_xrealloc(ldr->loading_stack, (size_t)ldr->loading_cap * sizeof(char *));
    }
    ldr->loading_stack[ldr->loading_count++] = (char *)use_path;
}

static void pop_loading(Loader *ldr) { ldr->loading_count--; }

/* ---- per-name imports (`use path.Name [as alias]`) ---- */

typedef enum { IMPORT_FN, IMPORT_STRUCT, IMPORT_ENUM, IMPORT_VAL } ImportKind;

/* One resolved `use path.Name [as alias]`: `local_name` is the bare name
 * usable (with no dot) in the importing module's own code, already
 * mapped to the fully-qualified name (`<path>::Name`) it actually
 * refers to -- computed once in resolve_named_imports, not re-resolved
 * at every reference the way a dotted `mod.Name` is. */
typedef struct {
    char *local_name;
    char *qualified_name;
    ImportKind kind;
} NamedImport;

/* ---- pass (a): self-qualify a module's own declarations ---- */

/* Snapshot of a module's own top-level names, taken *before* renaming them,
 * so the body-walk that follows can still recognize "this bare name is
 * mine" after the declarations themselves have already been renamed.
 * `imports`/`n_imports` are this module's resolved per-name imports (see
 * resolve_named_imports) -- checked as a fallback wherever a bare name
 * doesn't match one of the module's own declarations. Empty for the
 * entry module's own top-level names (own_fn_names etc.) when walking
 * it, since its own bare names are never rewritten -- but its imports
 * still are. */
typedef struct {
    const char *own_path;
    char **orig_fn_names;
    int n_fns;
    char **orig_struct_names;
    int n_structs;
    char **orig_enum_names;
    int n_enums;
    char **orig_val_names;
    int n_vals;
    NamedImport *imports;
    int n_imports;
    int in_lambda; /* >0 while walking a lambda body, whose locals the val-shadowing pass can't see */
} SelfQualifyCtx;

static const NamedImport *find_named_import(SelfQualifyCtx *ctx, const char *name, ImportKind kind);
static void self_qualify_stmt(Stmt *s, SelfQualifyCtx *ctx);

static const NamedImport *find_named_import(SelfQualifyCtx *ctx, const char *name, ImportKind kind) {
    for (int i = 0; i < ctx->n_imports; i++) {
        if (ctx->imports[i].kind == kind && strcmp(ctx->imports[i].local_name, name) == 0) return &ctx->imports[i];
    }
    return NULL;
}

/* parse_type() always builds a provisional TY_STRUCT for a bare TYPE_IDENT
 * reference, since the parser can't tell a struct name from an enum name
 * (both are just PascalCase identifiers) -- this is the point that can,
 * since it already has both name lists to check a same-module reference
 * against. A match against `orig_enum_names` reclassifies the Type to
 * TY_ENUM in the same step it gets qualified, so every later pass only
 * ever sees the correct kind. */
/* Reclassify a type the parser recorded as `TY_STRUCT` into the `TY_ENUM` it really is -- the
 * parser cannot tell the two apart from a bare name (see ast.h's Type comment), so several passes
 * below discover it and have to rewrite the type in place.
 *
 * Five places did this with their own copy of the same three assignments, in two different orders.
 * They are one call now, which is worth it on its own, and worth it twice over because the order
 * is a latent trap: two of the five wrote `t->enum_name = t->struct_name` *before* clearing
 * `struct_name`, which is correct only while those two fields have separate storage. An experiment
 * that unioned them (mutually exclusive by kind, so it looked free) would have silently erased the
 * name at exactly those two sites -- the experiment was reverted for unrelated reasons, see
 * docs/DESIGN.md, but the trap it exposed is real and now has only one place to go wrong. */
static void retype_struct_as_enum(Type *t, char *enum_name) {
    t->struct_name = NULL;
    t->enum_name = enum_name;
    t->kind = TY_ENUM;
}

static void self_qualify_type(Type *t, SelfQualifyCtx *ctx) {
    ZIMAN_STACK_CHECK();
    if (t->kind == TY_ARRAY) {
        self_qualify_type(t->elem, ctx);
        return;
    }
    if (t->kind == TY_RESULT) {
        self_qualify_type(t->elem, ctx);
        self_qualify_type(t->elem_err, ctx);
        return;
    }
    if (t->kind == TY_CHANNEL || t->kind == TY_SET) {
        self_qualify_type(t->elem, ctx);
        return;
    }
    if (t->kind == TY_MAP) {
        self_qualify_type(t->elem, ctx);
        self_qualify_type(t->elem_err, ctx);
        return;
    }
    if (t->kind == TY_FUNC) {
        for (int i = 0; i < t->type_args.count; i++) self_qualify_type(&t->type_args.items[i], ctx);
        self_qualify_type(t->elem, ctx);
        return;
    }
    if (t->kind != TY_STRUCT) return;
    /* A generic struct reference's own type arguments (`Box<other::Foo>`)
     * can themselves need self-qualifying, independent of whether `Box`
     * itself does -- visited unconditionally, before any of the
     * early-return branches below. */
    for (int i = 0; i < t->type_args.count; i++) {
        self_qualify_type(&t->type_args.items[i], ctx);
    }
    if (strchr(t->struct_name, '.') != NULL) return; /* already dotted -- pass (b)'s job */
    for (int i = 0; i < ctx->n_structs; i++) {
        if (strcmp(t->struct_name, ctx->orig_struct_names[i]) == 0) {
            t->struct_name = qualify_name(ctx->own_path, t->struct_name);
            return;
        }
    }
    for (int i = 0; i < ctx->n_enums; i++) {
        if (strcmp(t->struct_name, ctx->orig_enum_names[i]) == 0) {
            retype_struct_as_enum(t, qualify_name(ctx->own_path, t->struct_name));
            return;
        }
    }
    {
        const NamedImport *imp = find_named_import(ctx, t->struct_name, IMPORT_STRUCT);
        if (imp != NULL) {
            t->struct_name = imp->qualified_name;
            return;
        }
        imp = find_named_import(ctx, t->struct_name, IMPORT_ENUM);
        if (imp != NULL) {
            retype_struct_as_enum(t, imp->qualified_name);
            return;
        }
    }
    /* Not one of this module's own structs/enums, nor a named import --
     * leave it; typecheck reports "unknown struct or enum type" if it's
     * genuinely undefined. */
}

/* check_no_val_shadowing only sees statements, so a local declared inside a
 * lambda body (which lives in an expression) is checked here instead, while the
 * body is being walked: a local sharing a global val's name would be silently
 * rewritten to that global by the blind substitution below. */
static void forbid_global_val_name(SelfQualifyCtx *ctx, const char *name, int line, int col) {
    if (ctx->in_lambda == 0) return;
    int reserved = find_named_import(ctx, name, IMPORT_VAL) != NULL;
    for (int i = 0; i < ctx->n_vals && !reserved; i++) {
        if (strcmp(name, ctx->orig_val_names[i]) == 0) reserved = 1;
    }
    if (reserved) {
        ziman_error("module", line, col,
                    "'%s' is a global val in this module, so it can't also be used as a local name inside a lambda (rename one)", name);
    }
}

static void self_qualify_expr(Expr *e, SelfQualifyCtx *ctx) {
    for (int i = 0; i < e->fn_ref_type_args.count; i++) self_qualify_type(&e->fn_ref_type_args.items[i], ctx);
    switch (e->kind) {
        case EXPR_INT: case EXPR_FLOAT: case EXPR_BIGINT: case EXPR_DECIMAL: case EXPR_BYTE: case EXPR_STRING: case EXPR_BOOL: case EXPR_UNIT:
            break;
        /* A bare identifier is a local variable/parameter *unless* it
         * names one of this module's own global vals or a val-kind named
         * import -- checked in that order, own-declaration first. Both
         * cases get rewritten to the fully-qualified name every global
         * `val` is stored under in the one flat global Env every
         * function's own scope chains to (see typecheck.c's
         * check_global_vals / interpreter.c's init_global_vals) -- same
         * "always qualified, never scope-sensitive" treatment a bare
         * function/struct/enum reference already gets elsewhere in this
         * file. This rewrite is a blind text substitution with no scope
         * tracking, so it would be unsound if some *local*
         * val/var/parameter in this module happened to share a global
         * val's name -- check_no_val_shadowing forbids exactly that
         * (for both an own val and an imported one), so it never
         * actually happens. */
        case EXPR_IDENT: {
            int matched = 0;
            for (int i = 0; i < ctx->n_vals; i++) {
                if (strcmp(e->as.ident_name, ctx->orig_val_names[i]) == 0) {
                    e->as.ident_name = qualify_name(ctx->own_path, e->as.ident_name);
                    matched = 1;
                    break;
                }
            }
            if (!matched) {
                const NamedImport *imp = find_named_import(ctx, e->as.ident_name, IMPORT_VAL);
                if (imp != NULL) e->as.ident_name = imp->qualified_name;
                matched = imp != NULL;
            }
            if (!matched) {
                /* Maybe a function used as a value. Only a candidate: this pass
                 * cannot see local variables, and a local of the same name must
                 * win, so the type checker decides. */
                const char *bare = e->as.ident_name;
                const NamedImport *fn_imp = find_named_import(ctx, bare, IMPORT_FN);
                if (fn_imp != NULL) {
                    e->fn_ref_name = fn_imp->qualified_name;
                } else {
                    e->fn_ref_name = (char *)bare;
                    for (int i = 0; i < ctx->n_fns; i++) {
                        if (strcmp(bare, ctx->orig_fn_names[i]) == 0) {
                            e->fn_ref_name = qualify_name(ctx->own_path, bare);
                            break;
                        }
                    }
                }
            }
            break;
        }
        case EXPR_UNARY:
            self_qualify_expr(e->as.unary.operand, ctx);
            break;
        case EXPR_BINARY:
            self_qualify_expr(e->as.binary.left, ctx);
            self_qualify_expr(e->as.binary.right, ctx);
            break;
        case EXPR_CALL:
            if (strchr(e->as.call.callee, '.') == NULL) {
                int matched = 0;
                for (int i = 0; i < ctx->n_fns; i++) {
                    if (strcmp(e->as.call.callee, ctx->orig_fn_names[i]) == 0) {
                        e->as.call.callee = qualify_name(ctx->own_path, e->as.call.callee);
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    const NamedImport *imp = find_named_import(ctx, e->as.call.callee, IMPORT_FN);
                    if (imp != NULL) e->as.call.callee = imp->qualified_name;
                }
            }
            for (int i = 0; i < e->as.call.type_args.count; i++) self_qualify_type(&e->as.call.type_args.items[i], ctx);
            for (int i = 0; i < e->as.call.args.count; i++) self_qualify_expr(e->as.call.args.items[i], ctx);
            break;
        case EXPR_CAST:
            self_qualify_expr(e->as.cast.operand, ctx);
            self_qualify_type(&e->as.cast.target_type, ctx);
            break;
        case EXPR_ARRAY_LIT:
            for (int i = 0; i < e->as.array_lit.elements.count; i++) self_qualify_expr(e->as.array_lit.elements.items[i], ctx);
            break;
        case EXPR_INDEX:
            self_qualify_expr(e->as.index.array, ctx);
            self_qualify_expr(e->as.index.index, ctx);
            break;
        case EXPR_STRUCT_LIT:
            if (strchr(e->as.struct_lit.type_name, '.') == NULL) {
                int matched = 0;
                for (int i = 0; i < ctx->n_structs; i++) {
                    if (strcmp(e->as.struct_lit.type_name, ctx->orig_struct_names[i]) == 0) {
                        e->as.struct_lit.type_name = qualify_name(ctx->own_path, e->as.struct_lit.type_name);
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    const NamedImport *imp = find_named_import(ctx, e->as.struct_lit.type_name, IMPORT_STRUCT);
                    if (imp != NULL) e->as.struct_lit.type_name = imp->qualified_name;
                }
            }
            for (int i = 0; i < e->as.struct_lit.type_args.count; i++) self_qualify_type(&e->as.struct_lit.type_args.items[i], ctx);
            for (int i = 0; i < e->as.struct_lit.fields.count; i++) self_qualify_expr(e->as.struct_lit.fields.items[i].value, ctx);
            break;
        case EXPR_FIELD_ACCESS:
            self_qualify_expr(e->as.field_access.base, ctx);
            break;
        case EXPR_WITH:
            self_qualify_expr(e->as.with_expr.base, ctx);
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) self_qualify_expr(e->as.with_expr.overrides.items[i].value, ctx);
            break;
        case EXPR_ENUM_ACCESS:
            /* An already-dotted `enum_name` (`mod.Option.Some(...)`,
             * written by this module against one of its own `use`s) is
             * pass (b)'s job, same as a struct literal's already-dotted
             * `type_name`. A bare one always either matches one of this
             * module's own enums, matches a named import
             * (`use json.Json`), or is genuinely undefined --
             * never something pass (b) needs to resolve. `member_name`
             * is never touched, same as a struct field name never is. */
            if (strchr(e->as.enum_access.enum_name, '.') == NULL) {
                int matched = 0;
                for (int i = 0; i < ctx->n_enums; i++) {
                    if (strcmp(e->as.enum_access.enum_name, ctx->orig_enum_names[i]) == 0) {
                        e->as.enum_access.enum_name = qualify_name(ctx->own_path, e->as.enum_access.enum_name);
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    const NamedImport *imp = find_named_import(ctx, e->as.enum_access.enum_name, IMPORT_ENUM);
                    if (imp != NULL) e->as.enum_access.enum_name = imp->qualified_name;
                }
            }
            for (int i = 0; i < e->as.enum_access.type_args.count; i++) self_qualify_type(&e->as.enum_access.type_args.items[i], ctx);
            for (int i = 0; i < e->as.enum_access.args.count; i++) self_qualify_expr(e->as.enum_access.args.items[i], ctx);
            break;
        case EXPR_TRY_UNWRAP:
            self_qualify_expr(e->as.try_unwrap.inner, ctx);
            break;
        case EXPR_VALUE_BLOCK:
            /* declarations inside the arms are checked against global val names like a lambda's */
            ctx->in_lambda++;
            self_qualify_stmt(e->as.value_block.inner, ctx);
            ctx->in_lambda--;
            break;
        case EXPR_LAMBDA: {
            FunctionDecl *lam = e->lambda_decl;
            ctx->in_lambda++;
            for (int i = 0; i < lam->params.count; i++) {
                self_qualify_type(&lam->params.items[i].type, ctx);
                forbid_global_val_name(ctx, lam->params.items[i].name, lam->params.items[i].line, lam->params.items[i].col);
            }
            self_qualify_type(&lam->return_type, ctx);
            self_qualify_stmt(lam->body, ctx);
            ctx->in_lambda--;
            break;
        }
    }
}

static void self_qualify_stmt(Stmt *s, SelfQualifyCtx *ctx) {
    switch (s->kind) {
        case STMT_VAL: case STMT_VAR:
            forbid_global_val_name(ctx, s->as.decl.name, s->line, s->col);
            if (s->as.decl.has_declared_type) self_qualify_type(&s->as.decl.declared_type, ctx);
            self_qualify_expr(s->as.decl.init, ctx);
            break;
        case STMT_ASSIGN:
            self_qualify_expr(s->as.assign.value, ctx);
            break;
        case STMT_INDEX_ASSIGN:
            self_qualify_expr(s->as.index_assign.target, ctx);
            self_qualify_expr(s->as.index_assign.value, ctx);
            break;
        case STMT_IF:
            self_qualify_expr(s->as.if_stmt.cond, ctx);
            self_qualify_stmt(s->as.if_stmt.then_block, ctx);
            if (s->as.if_stmt.else_block != NULL) self_qualify_stmt(s->as.if_stmt.else_block, ctx);
            break;
        case STMT_WHILE:
            self_qualify_expr(s->as.while_stmt.cond, ctx);
            self_qualify_stmt(s->as.while_stmt.body, ctx);
            break;
        case STMT_FOR:
            forbid_global_val_name(ctx, s->as.for_stmt.binder1, s->line, s->col);
            if (s->as.for_stmt.binder2 != NULL) forbid_global_val_name(ctx, s->as.for_stmt.binder2, s->line, s->col);
            if (s->as.for_stmt.is_range) {
                self_qualify_expr(s->as.for_stmt.range_start, ctx);
                self_qualify_expr(s->as.for_stmt.range_end, ctx);
            } else {
                self_qualify_expr(s->as.for_stmt.iterable, ctx);
            }
            self_qualify_stmt(s->as.for_stmt.body, ctx);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break; /* no sub-expressions */
        case STMT_SPAWN:
            /* Same "dotted or not" logic EXPR_CALL's callee already gets --
             * see that case for why this checks this module's own
             * functions and its named imports the same way a call does. */
            if (strchr(s->as.spawn_stmt.callee, '.') == NULL) {
                int matched = 0;
                for (int i = 0; i < ctx->n_fns; i++) {
                    if (strcmp(s->as.spawn_stmt.callee, ctx->orig_fn_names[i]) == 0) {
                        s->as.spawn_stmt.callee = qualify_name(ctx->own_path, s->as.spawn_stmt.callee);
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    const NamedImport *imp = find_named_import(ctx, s->as.spawn_stmt.callee, IMPORT_FN);
                    if (imp != NULL) s->as.spawn_stmt.callee = imp->qualified_name;
                }
            }
            for (int i = 0; i < s->as.spawn_stmt.type_args.count; i++) self_qualify_type(&s->as.spawn_stmt.type_args.items[i], ctx);
            for (int i = 0; i < s->as.spawn_stmt.args.count; i++) self_qualify_expr(s->as.spawn_stmt.args.items[i], ctx);
            if (s->as.spawn_stmt.on_error != NULL) self_qualify_expr(s->as.spawn_stmt.on_error, ctx);
            break;
        case STMT_RETURN:
            if (s->as.ret.value != NULL) self_qualify_expr(s->as.ret.value, ctx);
            break;
        case STMT_EXPR:
            self_qualify_expr(s->as.expr_stmt.expr, ctx);
            break;
        case STMT_DEFER:
            self_qualify_expr(s->as.defer_stmt.call, ctx);
            break;
        case STMT_SWITCH:
            self_qualify_expr(s->as.switch_stmt.scrutinee, ctx);
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) {
                if (s->as.switch_stmt.arms[i].binder != NULL) {
                    forbid_global_val_name(ctx, s->as.switch_stmt.arms[i].binder, s->as.switch_stmt.arms[i].line, s->as.switch_stmt.arms[i].col);
                }
                self_qualify_stmt(s->as.switch_stmt.arms[i].body, ctx);
            }
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) self_qualify_stmt(s->as.block.stmts.items[i], ctx);
            break;
        case STMT_VALUE_BLOCK:
            if (s->as.value_block.target == STMT_VAL || s->as.value_block.target == STMT_VAR) {
                forbid_global_val_name(ctx, s->as.value_block.name, s->line, s->col);
            }
            if (s->as.value_block.has_declared_type) self_qualify_type(&s->as.value_block.declared_type, ctx);
            self_qualify_stmt(s->as.value_block.inner, ctx);
            break;
        case STMT_YIELD:
            self_qualify_expr(s->as.yield_stmt.value, ctx);
            break;
    }
}

/* The body-walk half of self-qualifying a module: rewrites every bare
 * reference `ctx` recognizes (this module's own declarations, own_path
 * non-empty; its named imports, always) to a fully-qualified name.
 * Split out from self_qualify_module so the entry module -- which never
 * renames its own top-level declarations, see load_module_graph -- can
 * still run just this half, with empty own-name arrays (so only its
 * named imports actually get rewritten). */
static void self_qualify_walk_module(Module *mod, SelfQualifyCtx *ctx) {
    for (int i = 0; i < mod->functions.count; i++) {
        FunctionDecl *fn = mod->functions.items[i];
        ziman_set_error_context(fn->derived_from);
        for (int pi = 0; pi < fn->params.count; pi++) self_qualify_type(&fn->params.items[pi].type, ctx);
        self_qualify_type(&fn->return_type, ctx);
        if (fn_has_body(fn)) self_qualify_stmt(fn->body, ctx);
        ziman_set_error_context(NULL);
    }
    for (int i = 0; i < mod->structs.count; i++) {
        StructDecl *sd = mod->structs.items[i];
        for (int fi = 0; fi < sd->fields.count; fi++) self_qualify_type(&sd->fields.items[fi].type, ctx);
    }
    for (int i = 0; i < mod->enums.count; i++) {
        EnumDecl *ed = mod->enums.items[i];
        for (int vi = 0; vi < ed->variants.count; vi++) {
            if (ed->variants.items[vi].has_payload) self_qualify_type(&ed->variants.items[vi].payload_type, ctx);
        }
    }
    for (int i = 0; i < mod->vals.count; i++) {
        GlobalValDecl *v = mod->vals.items[i];
        if (v->has_declared_type) self_qualify_type(&v->type, ctx);
        self_qualify_expr(v->init, ctx);
    }
}

/* Reports whether `name` is either one of this module's own global val
 * names (`val_names`/`n_val_names`, *before* self-qualify renames them)
 * or the local name of a val-kind named import (`use mod.NAME`) -- see
 * check_no_val_shadowing below for why both need guarding against reuse
 * as a local name. */
static int is_reserved_val_name(const char *name, char **val_names, int n_val_names, NamedImport *imports, int n_imports) {
    for (int i = 0; i < n_val_names; i++) {
        if (strcmp(val_names[i], name) == 0) return 1;
    }
    for (int i = 0; i < n_imports; i++) {
        if (imports[i].kind == IMPORT_VAL && strcmp(imports[i].local_name, name) == 0) return 1;
    }
    return 0;
}

static void check_no_val_shadowing_stmt(Stmt *s, char **val_names, int n_val_names, NamedImport *imports, int n_imports) {
    switch (s->kind) {
        case STMT_VAL: case STMT_VAR:
            if (is_reserved_val_name(s->as.decl.name, val_names, n_val_names, imports, n_imports)) {
                ziman_error("module", s->line, s->col,
                            "'%s' is a global val in this module, so it can't also be used as a local name (rename one)",
                            s->as.decl.name);
            }
            break;
        case STMT_FOR:
            if (is_reserved_val_name(s->as.for_stmt.binder1, val_names, n_val_names, imports, n_imports)) {
                ziman_error("module", s->line, s->col,
                            "'%s' is a global val in this module, so it can't also be used as a loop variable name (rename one)",
                            s->as.for_stmt.binder1);
            }
            if (s->as.for_stmt.binder2 != NULL && is_reserved_val_name(s->as.for_stmt.binder2, val_names, n_val_names, imports, n_imports)) {
                ziman_error("module", s->line, s->col,
                            "'%s' is a global val in this module, so it can't also be used as a loop variable name (rename one)",
                            s->as.for_stmt.binder2);
            }
            check_no_val_shadowing_stmt(s->as.for_stmt.body, val_names, n_val_names, imports, n_imports);
            break;
        case STMT_IF:
            check_no_val_shadowing_stmt(s->as.if_stmt.then_block, val_names, n_val_names, imports, n_imports);
            if (s->as.if_stmt.else_block != NULL) check_no_val_shadowing_stmt(s->as.if_stmt.else_block, val_names, n_val_names, imports, n_imports);
            break;
        case STMT_WHILE:
            check_no_val_shadowing_stmt(s->as.while_stmt.body, val_names, n_val_names, imports, n_imports);
            break;
        case STMT_SWITCH:
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) {
                SwitchArm *arm = &s->as.switch_stmt.arms[i];
                if (arm->binder != NULL && is_reserved_val_name(arm->binder, val_names, n_val_names, imports, n_imports)) {
                    ziman_error("module", arm->line, arm->col,
                                "'%s' is a global val in this module, so it can't also be used as a switch payload name (rename one)",
                                arm->binder);
                }
                check_no_val_shadowing_stmt(arm->body, val_names, n_val_names, imports, n_imports);
            }
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) check_no_val_shadowing_stmt(s->as.block.stmts.items[i], val_names, n_val_names, imports, n_imports);
            break;
        case STMT_VALUE_BLOCK:
            if ((s->as.value_block.target == STMT_VAL || s->as.value_block.target == STMT_VAR) &&
                is_reserved_val_name(s->as.value_block.name, val_names, n_val_names, imports, n_imports)) {
                ziman_error("module", s->line, s->col,
                            "'%s' is a global val in this module, so it can't also be used as a local name (rename one)",
                            s->as.value_block.name);
            }
            check_no_val_shadowing_stmt(s->as.value_block.inner, val_names, n_val_names, imports, n_imports);
            break;
        case STMT_ASSIGN: case STMT_INDEX_ASSIGN: case STMT_BREAK: case STMT_CONTINUE:
        case STMT_SPAWN: case STMT_RETURN: case STMT_EXPR: case STMT_DEFER: case STMT_YIELD:
            break; /* declares no new name */
    }
}

/* Every global val reference -- same-module or a val-kind named import
 * (`use mod.NAME`) -- is rewritten to its fully-qualified form at every
 * bare reference by self_qualify_expr's EXPR_IDENT case, the same
 * "always qualified, never scope-sensitive" treatment a bare function/
 * struct/enum reference already gets. That rewrite is a blind text
 * substitution with no scope tracking, so it would silently capture an
 * unrelated *local* val/var/parameter/loop-binder of the same name too
 * -- forbidding a module's own global vals and its val-kind imports from
 * being reused as any local name *anywhere* in the module (not just the
 * one scope that would actually collide) is a coarser rule than real
 * scope tracking would need, but it's simple, and matches the same non-
 * scope-aware "collides with an existing declaration" checks already
 * used elsewhere in this file. Must run on `mod`'s still-unrenamed
 * declarations, before self_qualify_module renames its own val names. */
static void check_no_val_shadowing(Module *mod, char **val_names, int n_val_names, NamedImport *imports, int n_imports) {
    if (n_val_names == 0) {
        int has_val_import = 0;
        for (int i = 0; i < n_imports; i++) {
            if (imports[i].kind == IMPORT_VAL) { has_val_import = 1; break; }
        }
        if (!has_val_import) return;
    }
    for (int i = 0; i < mod->functions.count; i++) {
        FunctionDecl *fn = mod->functions.items[i];
        for (int pi = 0; pi < fn->params.count; pi++) {
            if (is_reserved_val_name(fn->params.items[pi].name, val_names, n_val_names, imports, n_imports)) {
                ziman_error("module", fn->params.items[pi].line, fn->params.items[pi].col,
                            "'%s' is a global val in this module, so it can't also be used as a parameter name (rename one)",
                            fn->params.items[pi].name);
            }
        }
        if (fn_has_body(fn)) check_no_val_shadowing_stmt(fn->body, val_names, n_val_names, imports, n_imports);
    }
}

static void self_qualify_module(Module *mod, const char *own_path, NamedImport *imports, int n_imports) {
    int n_fns = mod->functions.count;
    int n_structs = mod->structs.count;
    int n_enums = mod->enums.count;
    int n_vals = mod->vals.count;
    char **orig_fn_names = n_fns > 0 ? ziman_xmalloc((size_t)n_fns * sizeof(char *)) : NULL;
    char **orig_struct_names = n_structs > 0 ? ziman_xmalloc((size_t)n_structs * sizeof(char *)) : NULL;
    char **orig_enum_names = n_enums > 0 ? ziman_xmalloc((size_t)n_enums * sizeof(char *)) : NULL;
    char **orig_val_names = n_vals > 0 ? ziman_xmalloc((size_t)n_vals * sizeof(char *)) : NULL;
    for (int i = 0; i < n_fns; i++) orig_fn_names[i] = mod->functions.items[i]->name;
    for (int i = 0; i < n_structs; i++) orig_struct_names[i] = mod->structs.items[i]->name;
    for (int i = 0; i < n_enums; i++) orig_enum_names[i] = mod->enums.items[i]->name;
    for (int i = 0; i < n_vals; i++) orig_val_names[i] = mod->vals.items[i]->name;

    check_no_val_shadowing(mod, orig_val_names, n_vals, imports, n_imports);

    for (int i = 0; i < n_fns; i++) {
        mod->functions.items[i]->name = qualify_name(own_path, orig_fn_names[i]);
    }
    for (int i = 0; i < n_structs; i++) {
        mod->structs.items[i]->name = qualify_name(own_path, orig_struct_names[i]);
    }
    for (int i = 0; i < n_enums; i++) {
        mod->enums.items[i]->name = qualify_name(own_path, orig_enum_names[i]);
    }
    for (int i = 0; i < n_vals; i++) {
        mod->vals.items[i]->name = qualify_name(own_path, orig_val_names[i]);
    }

    SelfQualifyCtx ctx = {own_path, orig_fn_names, n_fns, orig_struct_names, n_structs, orig_enum_names, n_enums, orig_val_names, n_vals, imports, n_imports, 0};
    self_qualify_walk_module(mod, &ctx);

    free(orig_fn_names);
    free(orig_struct_names);
    free(orig_enum_names);
    free(orig_val_names);
}

/* ---- pass (b): resolve cross-module (dotted) references, enforcing pub ---- */

typedef struct {
    Loader *ldr;
    Module *mod; /* whose `use` list gives meaning to a qualifier */
} CrossRefCtx;

static UseDecl *find_use(Module *mod, const char *qualifier) {
    for (int i = 0; i < mod->uses.count; i++) {
        /* A per-name `use path.Name` has qualifier == NULL -- it was
         * never meant to be usable as a `qualifier.thing` prefix, so it
         * can never match here. */
        if (mod->uses.items[i].qualifier == NULL) continue;
        if (strcmp(mod->uses.items[i].qualifier, qualifier) == 0) return &mod->uses.items[i];
    }
    return NULL;
}

/* Splits "qualifier.rest" on the first '.' into two freshly-allocated
 * strings; caller frees both. */
static void split_qualified(const char *dotted, char **qualifier_out, char **rest_out) {
    const char *dot = strchr(dotted, '.');
    size_t qlen = (size_t)(dot - dotted);
    *qualifier_out = ziman_xmalloc(qlen + 1);
    memcpy(*qualifier_out, dotted, qlen);
    (*qualifier_out)[qlen] = '\0';
    *rest_out = ziman_xstrdup(dot + 1);
}

/* `p.to_string()` parses as a call into a module named `p`. The most common
 * reason there is no such module is that `p` is a value and the author
 * reached for a method, which Ziman doesn't have -- so say that first, with
 * the function-call form that does work.
 *
 * Since C15 this is no longer raised for a field holding a function value: the type checker resolves
 * `p.member(...)` to that field and calls it, so the advice C9 added here about binding the field to a val
 * first has been removed again -- it described a workaround that is no longer needed. What is left is the
 * case this pass can still be sure about: the qualifier is neither an imported module nor a local with a
 * callable field, so either a free function was meant or a `use` is missing. */
/* The parsed prelude, kept for method_call_hint alone: it is the honest answer to "is there such a
 * function", the same source `--api builtins` prints from and for the same reason main.c records
 * there -- typecheck.c's BUILTINS_NO_NAMED_ARGS is the subset taking no named arguments, not the
 * builtins, and an incomplete answer to "what exists" reads as "it does not". NULL while the
 * prelude itself is being parsed (parser.c reports chained calls during its own parse), and the
 * hint then falls back to naming the call *form* exactly as it always did. */
static Module *g_prelude_for_hints = NULL;

static int prelude_has_function(const char *name) {
    if (g_prelude_for_hints == NULL) return -1;  /* unknown, not absent */
    for (int i = 0; i < g_prelude_for_hints->functions.count; i++) {
        FunctionDecl *f = g_prelude_for_hints->functions.items[i];
        if (f->visibility != VIS_PUBLIC && !fn_is_global_vocabulary(f)) continue;
        if (strcmp(f->name, name) == 0) return 1;
    }
    return 0;
}

/* The free-function form for a member a model reached for with method syntax, written against
 * `qualifier` as the receiver. Factored out of ziman_report_unknown_module_call so the chained-call
 * diagnostic (ziman_report_method_chain_call, added for F50) names the same measured functions --
 * those entries exist because naming the wrong one costs the reader a round, and that is just as
 * true when the receiver is an expression as when it is a variable. */
static int method_call_hint(const char *member, const char *qualifier, char *hint, size_t cap) {
    if (strcmp(member, "to_string") == 0 || strcmp(member, "toString") == 0 || strcmp(member, "to_str") == 0) {
        snprintf(hint, cap, "str(%s)", qualifier);
    } else if (strcmp(member, "len") == 0 || strcmp(member, "length") == 0 || strcmp(member, "size") == 0) {
        snprintf(hint, cap, "len(%s)", qualifier);
    } else if (strcmp(member, "push") == 0 || strcmp(member, "append") == 0) {
        snprintf(hint, cap, "%s = push(%s, item)", qualifier, qualifier);
        /* The names below were measured, not guessed: each is one a cold model reached for in the
         * 2026-10-09 benchmark runs, where the bare `member(qualifier, ...)` fallback below sent it
         * to a function that does not exist (`lines`) or cannot take the receiver (`join`, which is
         * a path join over strings, not a separator join over a list). A hint that names the wrong
         * function is worse than one that names none, because the reader spends a round finding out
         * (docs/reviews/llm-benchmark-2026-10-09.md, tasks.md F28). */
    } else if (strcmp(member, "lines") == 0) {
        snprintf(hint, cap, "lines(%s)", qualifier);
    } else if (strcmp(member, "join") == 0) {
        snprintf(hint, cap, "array_join(%s, sep) (the builtin 'join' is a path join over strings)", qualifier);
    } else if (strcmp(member, "sort") == 0 || strcmp(member, "sorted") == 0) {
        snprintf(hint, cap, "array_sort<T>(%s)", qualifier);
    } else if (strcmp(member, "keys") == 0) {
        snprintf(hint, cap, "map_keys(%s), which answers a Set (set_items turns it into an array)", qualifier);
    } else if (strcmp(member, "values") == 0) {
        snprintf(hint, cap, "map_values(%s)", qualifier);
    } else if (strcmp(member, "reverse") == 0 || strcmp(member, "reversed") == 0) {
        snprintf(hint, cap, "array_reverse(%s) for an array, or reverse_string(%s) for text", qualifier, qualifier);
    } else if (strcmp(member, "sum") == 0) {
        snprintf(hint, cap, "array_sum(%s) for ints, or array_fold for anything else", qualifier);
    } else {
        /* The free-function *form*, not a claim that such a function exists -- this pass has no
         * scope to look the name up in, and the measured cases above are where guessing wrong
         * actually cost something. An earlier version hedged with "if there is such a function",
         * which read as noise in the common case where the guess is right (`s.trim()`). */
        snprintf(hint, cap, "%s(%s, ...)", member, qualifier);
        /* F92: the one case where the form is not enough. Until now this named `member(q, ...)`
         * unconditionally, as "the form, not a claim that such a function exists" -- and the second
         * instance of round 2 shows what that costs when the claim is read as one: `a.iter()` was
         * answered with `iter(a, ...)`, there is no `iter`, and that run spent 194 compiler
         * invocations before concluding `Iter<T>` was "unreachable from source" when
         * `iter` exists. So the form is still named when the function is real, and the
         * absence is named when it is not. The earlier hedge this file removed ("if there is such a
         * function") was noise because it fired on the common case where the guess is right; this
         * fires only when the guess is known to be wrong. */
        return prelude_has_function(member) == 0 ? 0 : 1;
    }
    return 1;
}

/* `expr.member(...)` where the receiver is not a plain variable -- a chained method call
 * (`s.to_upper().len()`), or one on an index or a nested field (`hs[0].go(1)`, `o.inner.go(1)`).
 * Only `name.member(...)` reaches parse_primary's qualified-call path; everything else became a
 * field access that no `(` could follow, so the whole family used to fail as a bare
 * `expected ')', found '('` from several tokens away. Two of ten cold attempts in the 2026-10-10
 * benchmark wave wrote a chain and neither ever saw the "Ziman has no methods" sentence, which is
 * backwards: chaining is the *more* common shape in the training distribution, so the likelier
 * mistake was getting the worse message (tasks.md F50, F49).
 *
 * Both readings are named because the parser cannot tell them apart and both are true. If a method
 * was meant, there are none, and the fix is to nest the calls or bind each step. If `member` is a
 * field holding a function value, that is real (C15) but callable only on a plain variable today --
 * `o.inner.go(1)` and `hs[0].go(1)` are both still syntax errors -- so binding the receiver is the
 * fix for that reading too, which makes one sentence serve both. */
void ziman_report_method_chain_call(const char *member, int line, int col) {
    char hint[160];
    if (!method_call_hint(member, "v", hint, sizeof hint)) {
        ziman_error("syntax", line, col,
                    "Ziman has no methods, so '.%s(...)' cannot be called on the result of an expression -- a "
                    "function takes its receiver as an argument instead, but there is no function named '%s': "
                    "'ziman --api builtins' lists the ones that need no 'use', and 'ziman --api <module>' a "
                    "module's own",
                    member, member);
    }
    ziman_error("syntax", line, col,
                "Ziman has no methods, so '.%s(...)' cannot be called on the result of an expression -- a "
                "function takes its receiver as an argument instead: bind the inner expression with "
                "'val v = ...' and write '%s', or nest the calls directly. (If '%s' is a struct field "
                "holding a function value, that is callable too, but only on a plain variable, so the same "
                "binding is the fix.)",
                member, hint, member);
}

void ziman_report_unknown_module_call(const char *qualifier, const char *member, int line, int col) {
    char hint[160];
    if (!method_call_hint(member, qualifier, hint, sizeof hint)) {
        ziman_error("module", line, col,
                    "'%s.%s(...)': '%s' is not an imported module, and Ziman has no methods -- a function takes "
                    "its receiver as an argument instead, but there is no function named '%s': 'ziman --api "
                    "builtins' lists the ones that need no 'use' (and if '%s' should be a module, add 'use %s')",
                    qualifier, member, qualifier, member, qualifier, qualifier);
    }
    ziman_error("module", line, col,
                "'%s.%s(...)': '%s' is not an imported module, and Ziman has no methods -- if '%s' is a value, call a function on it instead, e.g. %s "
                "(if '%s' should be a module, add 'use %s')",
                qualifier, member, qualifier, qualifier, hint, qualifier, qualifier);
}

/* Today's whole definition of a "package" for VIS_INTERNAL visibility (see Visibility's own
 * doc comment in ast.h): the bundled standard library is one package, a project is another --
 * `Module.is_corelib`, set once in load_module/load_module_graph from the same `from_disk` check
 * that already decides whether to stamp a declaration's `source_file`. Deliberately coarse (a
 * whole project is one package today, not sub-divided by directory -- see the DESIGN.md entry
 * for why a directory-based boundary would have actually broken the motivating case,
 * `net.zm`/`lib/core` and `client.zm`/`lib/http` sharing internals across different
 * directories). Meant to be the one place a future manifest-based package/crate system redefines
 * -- comparing package identities instead of this one flag -- without `internal` itself, or any
 * of its call sites below, needing to change. */
static int same_package(Module *a, Module *b) { return a->is_corelib == b->is_corelib; }

static int visibility_allows(Visibility v, Module *from, Module *target) {
    if (v == VIS_PUBLIC) return 1;
    if (v == VIS_PRIVATE) return 0;
    return same_package(from, target);
}

/* The tail of the "is private"/"is internal" error message, past "<kind> '<name>' in module
 * '<path>' is " -- only ever called once visibility_allows has already said no, so `v` is never
 * VIS_PUBLIC here. */
static const char *visibility_violation_reason(Visibility v) {
    return v == VIS_PRIVATE ? "private (add 'pub' to export it)"
                             : "internal to its own package (not visible from a different one; add 'pub' to export it more broadly)";
}

static char *resolve_function_ref(CrossRefCtx *ctx, const char *dotted, int line, int col) {
    char *qualifier, *rest;
    split_qualified(dotted, &qualifier, &rest);
    UseDecl *u = find_use(ctx->mod, qualifier);
    if (u == NULL) {
        ziman_report_unknown_module_call(qualifier, rest, line, col);
    }
    Module *target = find_loaded(ctx->ldr, u->path);
    char *qualified = qualify_name(u->path, rest);
    FunctionDecl *found = NULL;
    for (int i = 0; i < target->functions.count; i++) {
        if (strcmp(target->functions.items[i]->name, qualified) == 0) { found = target->functions.items[i]; break; }
    }
    if (found == NULL) {
        /* A dead end before this: the message confirmed the negative and stopped, though the
         * module's whole function list is right here. In the 2026-10-09 benchmark pilot one run
         * brute-forced eight names (`write_file`, `writeFile`, `write_string`, `save`, `put`, ...)
         * before reaching `fs.write_text`, with no other way to recover the name -- library sources
         * being off limits is the point of that measurement, and is also the position any reader
         * without the docs open is in (docs/reviews/llm-benchmark-2026-10-09.md, tasks.md F28).
         * Edit distance alone does not solve this one, which is worth recording: `write` to
         * `write_text` is five inserts, well past the "strictly less than 3" cutoff the rest of the
         * compiler uses, so the closest *name* is not the answer. What a reader has actually got
         * right is the stem, and what they are missing is this library's suffix convention
         * (`_text`, `_bytes`, `_dir`). So prefix matches come first and several are offered -- the
         * choice between `read_text` and `read_bytes` is one a reader can make, while guessing that
         * a suffix exists at all is not. Edit distance is kept as the fallback for an ordinary
         * misspelling. Visible functions only, so a suggestion cannot fail for a second reason. */
        /* Before guessing at a misspelling: the name may exist in that module and simply not be a
         * function. `mod.NAME` reaches functions only, so `number.float_min_positive` -- a `pub
         * val` -- was answered with "module 'number' has no function 'float_min_positive'", which
         * sends the reader looking for a function that was never the point. The name is right; the
         * form is wrong, and `use number.float_min_positive` then the bare name is what works. Two
         * reviews asked for this separately (docs/reviews/tasks.md C20).
         *
         * Only vals are checked here. A struct or an enum in this position never reaches the loader
         * at all -- the parser stops first with "expected '{' or '.' after 'net.Socket'", which
         * already says what to do -- so a branch for either would be dead code. Checked both forms,
         * `net.Socket` and `net.Socket(1)`.
         *
         * Qualified exactly as `collect_named_imports` does it, so the two lookups agree. */
        char *qualified_other = qualify_name(u->path, rest);
        for (int i = 0; i < target->vals.count; i++) {
            if (strcmp(target->vals.items[i]->name, qualified_other) != 0) continue;
            if (!visibility_allows(target->vals.items[i]->visibility, ctx->mod, target)) break;
            ziman_error("module", line, col,
                        "'%s' in module '%s' is a global val, not a function -- a val is not reachable as "
                        "'%s.%s'; write `use %s.%s` and then `%s`",
                        rest, u->path, u->path, rest, u->path, rest, rest);
        }

        const char *prefixed[3];
        int n_prefixed = 0;
        const char *best = NULL;
        int best_distance = 3;
        size_t rest_len = strlen(rest);
        for (int i = 0; i < target->functions.count; i++) {
            FunctionDecl *cand = target->functions.items[i];
            if (!visibility_allows(cand->visibility, ctx->mod, target)) continue;
            const char *bare = strstr(cand->name, "::");
            bare = bare != NULL ? bare + 2 : cand->name;
            if (rest_len > 1 && strncmp(bare, rest, rest_len) == 0 && n_prefixed < 3) {
                prefixed[n_prefixed++] = bare;
                continue;
            }
            int d = ziman_name_distance(rest, bare);
            if (d < best_distance) {
                best_distance = d;
                best = bare;
            }
        }
        if (n_prefixed == 1) {
            ziman_error("module", line, col, "module '%s' has no function '%s' -- did you mean '%s'?", u->path, rest, prefixed[0]);
        }
        if (n_prefixed == 2) {
            ziman_error("module", line, col, "module '%s' has no function '%s' -- did you mean '%s' or '%s'?", u->path, rest,
                        prefixed[0], prefixed[1]);
        }
        if (n_prefixed >= 3) {
            ziman_error("module", line, col, "module '%s' has no function '%s' -- did you mean '%s', '%s' or '%s'?", u->path,
                        rest, prefixed[0], prefixed[1], prefixed[2]);
        }
        if (best != NULL) {
            ziman_error("module", line, col, "module '%s' has no function '%s' -- did you mean '%s'?", u->path, rest, best);
        }
        ziman_error("module", line, col, "module '%s' has no function '%s'", u->path, rest);
    }
    if (!visibility_allows(found->visibility, ctx->mod, target)) {
        ziman_error("module", line, col, "function '%s' in module '%s' is %s", rest, u->path, visibility_violation_reason(found->visibility));
    }
    free(qualifier);
    free(rest);
    return qualified;
}

static char *resolve_type_ref(CrossRefCtx *ctx, const char *dotted, int line, int col) {
    char *qualifier, *rest;
    split_qualified(dotted, &qualifier, &rest);
    UseDecl *u = find_use(ctx->mod, qualifier);
    if (u == NULL) {
        ziman_error("module", line, col, "no module imported under the name '%s' (add 'use %s')", qualifier, qualifier);
    }
    Module *target = find_loaded(ctx->ldr, u->path);
    char *qualified = qualify_name(u->path, rest);
    StructDecl *found = NULL;
    for (int i = 0; i < target->structs.count; i++) {
        if (strcmp(target->structs.items[i]->name, qualified) == 0) { found = target->structs.items[i]; break; }
    }
    if (found == NULL) {
        ziman_error("module", line, col, "module '%s' has no struct '%s'", u->path, rest);
    }
    if (!visibility_allows(found->visibility, ctx->mod, target)) {
        ziman_error("module", line, col, "struct '%s' in module '%s' is %s", rest, u->path, visibility_violation_reason(found->visibility));
    }
    free(qualifier);
    free(rest);
    return qualified;
}

/* Same shape as resolve_type_ref, but looking up an enum instead of a
 * struct -- the "does the loader's qualified-reference machinery need a
 * second dot for `qualifier.EnumName.Variant`" concern this used to raise
 * (see ROADMAP.md's "Generics" Stage 4) was actually just this: the
 * *parser* needed a second dot's worth of grammar (parse_primary now
 * has it), but once `enum_name` itself is qualified to
 * `<module_path>::EnumName` here, `check_enum_access` in typecheck.c
 * needs nothing new -- `find_enum` already looks a name up in the one
 * flat, fully-qualified Module every module gets merged into. */
static char *resolve_enum_ref(CrossRefCtx *ctx, const char *dotted, int line, int col) {
    char *qualifier, *rest;
    split_qualified(dotted, &qualifier, &rest);
    UseDecl *u = find_use(ctx->mod, qualifier);
    if (u == NULL) {
        ziman_error("module", line, col, "no module imported under the name '%s' (add 'use %s')", qualifier, qualifier);
    }
    Module *target = find_loaded(ctx->ldr, u->path);
    char *qualified = qualify_name(u->path, rest);
    EnumDecl *found = NULL;
    for (int i = 0; i < target->enums.count; i++) {
        if (strcmp(target->enums.items[i]->name, qualified) == 0) { found = target->enums.items[i]; break; }
    }
    if (found == NULL) {
        ziman_error("module", line, col, "module '%s' has no enum '%s'", u->path, rest);
    }
    if (!visibility_allows(found->visibility, ctx->mod, target)) {
        ziman_error("module", line, col, "enum '%s' in module '%s' is %s", rest, u->path, visibility_violation_reason(found->visibility));
    }
    free(qualifier);
    free(rest);
    return qualified;
}

/* Like resolve_type_ref, but for a *type-position* reference
 * (`val x: mod.Shape`), which -- unlike a struct literal's `type_name`,
 * always struct-shaped by construction (`Name { ... }` can only ever
 * build a struct) -- might actually name an enum: the parser always
 * provisionally builds TY_STRUCT for a qualified `mod.Name` the same way
 * it does for a bare one (see ast.h's Type comment), so this is the
 * dotted case's equivalent of the bare case's own struct-vs-enum
 * reclassification a few lines below. Mutates `t` directly (to TY_ENUM,
 * if that's what it turned out to be) instead of returning a name. */
static void resolve_type_or_enum_ref(Type *t, CrossRefCtx *ctx, int line, int col) {
    char *qualifier, *rest;
    split_qualified(t->struct_name, &qualifier, &rest);
    UseDecl *u = find_use(ctx->mod, qualifier);
    if (u == NULL) {
        ziman_error("module", line, col, "no module imported under the name '%s' (add 'use %s')", qualifier, qualifier);
    }
    Module *target = find_loaded(ctx->ldr, u->path);
    char *qualified = qualify_name(u->path, rest);
    for (int i = 0; i < target->structs.count; i++) {
        if (strcmp(target->structs.items[i]->name, qualified) == 0) {
            if (!visibility_allows(target->structs.items[i]->visibility, ctx->mod, target)) {
                ziman_error("module", line, col, "struct '%s' in module '%s' is %s", rest, u->path,
                            visibility_violation_reason(target->structs.items[i]->visibility));
            }
            t->struct_name = qualified;
            free(qualifier);
            free(rest);
            return;
        }
    }
    for (int i = 0; i < target->enums.count; i++) {
        if (strcmp(target->enums.items[i]->name, qualified) == 0) {
            if (!visibility_allows(target->enums.items[i]->visibility, ctx->mod, target)) {
                ziman_error("module", line, col, "enum '%s' in module '%s' is %s", rest, u->path,
                            visibility_violation_reason(target->enums.items[i]->visibility));
            }
            retype_struct_as_enum(t, qualified);
            free(qualifier);
            free(rest);
            return;
        }
    }
    ziman_error("module", line, col, "module '%s' has no struct or enum '%s'", u->path, rest);
}

static void resolve_type_refs(Type *t, CrossRefCtx *ctx, int line, int col) {
    ZIMAN_STACK_CHECK();
    if (t->kind == TY_ARRAY) {
        resolve_type_refs(t->elem, ctx, line, col);
        return;
    }
    if (t->kind == TY_RESULT) {
        resolve_type_refs(t->elem, ctx, line, col);
        resolve_type_refs(t->elem_err, ctx, line, col);
        return;
    }
    if (t->kind == TY_CHANNEL || t->kind == TY_SET) {
        resolve_type_refs(t->elem, ctx, line, col);
        return;
    }
    if (t->kind == TY_MAP) {
        resolve_type_refs(t->elem, ctx, line, col);
        resolve_type_refs(t->elem_err, ctx, line, col);
        return;
    }
    if (t->kind == TY_FUNC) {
        for (int i = 0; i < t->type_args.count; i++) resolve_type_refs(&t->type_args.items[i], ctx, line, col);
        resolve_type_refs(t->elem, ctx, line, col);
        return;
    }
    /* An enum reclassified by pass (a) (a same-module or named-import `Listing<mod.Item>`) still has type arguments
     * that may be dotted: they are resolved here, or the alias `mod` is looked up later in a module that does not have it. */
    if (t->kind == TY_ENUM) {
        for (int i = 0; i < t->type_args.count; i++) resolve_type_refs(&t->type_args.items[i], ctx, line, col);
        return;
    }
    if (t->kind != TY_STRUCT) return;
    /* Same reasoning as self_qualify_type: a generic reference's own type
     * arguments need resolving independent of whether the base name does. */
    for (int i = 0; i < t->type_args.count; i++) {
        resolve_type_refs(&t->type_args.items[i], ctx, line, col);
    }
    if (strchr(t->struct_name, '.') != NULL) {
        resolve_type_or_enum_ref(t, ctx, line, col);
        return;
    }
    /* Not dotted -- either a dependency's own struct/enum (already
     * qualified and, if it was actually an enum, already reclassified by
     * pass (a)'s self_qualify_type), or a same-module reference in the
     * *entry* module, which never runs pass (a) at all (see
     * load_module_graph: it keeps its own bare names, so there's nothing
     * for that pass to qualify). Either way, a bare TY_STRUCT that's still
     * unreclassified here might actually mean an enum -- the parser can't
     * tell the two apart at parse time (see ast.h's Type comment), so this
     * is the last point that reliably can for the entry-module case. */
    for (int i = 0; i < ctx->mod->enums.count; i++) {
        if (strcmp(t->struct_name, ctx->mod->enums.items[i]->name) == 0) {
            retype_struct_as_enum(t, t->struct_name);
            return;
        }
    }
    /* Or a third case the two above don't cover: a bare reference to a
     * prelude enum (`Option<T>`) from some *other* module's own
     * declaration -- `ctx->mod` above is that module's own (empty, in
     * this case) enum list, never the prelude's, since the prelude isn't
     * a dependency reached through any `use`. Checked last, after a
     * module's own enums, so a module could in principle still shadow
     * the name with its own same-named enum (not that any should).
     * `ldr->prelude_mod` is only NULL while the prelude module is
     * resolving *its own* cross-references (load_module_graph calls
     * resolve_cross_refs on it before assigning this field, since it's
     * what that very call is in the middle of producing) -- correctly a
     * no-op then, since a bare struct reference inside the prelude that
     * wasn't already found among the prelude's own enums above (`ctx->mod`
     * there being the prelude module itself, in that case) genuinely is a
     * struct (`MutableString`), not a second, redundant place to look. */
    if (ctx->ldr->prelude_mod != NULL) {
        for (int i = 0; i < ctx->ldr->prelude_mod->enums.count; i++) {
            if (strcmp(t->struct_name, ctx->ldr->prelude_mod->enums.items[i]->name) == 0) {
                retype_struct_as_enum(t, t->struct_name);
                return;
            }
        }
    }
}

static void resolve_stmt_refs(Stmt *s, CrossRefCtx *ctx);

static void resolve_expr_refs(Expr *e, CrossRefCtx *ctx) {
    for (int i = 0; i < e->fn_ref_type_args.count; i++) resolve_type_refs(&e->fn_ref_type_args.items[i], ctx, e->line, e->col);
    switch (e->kind) {
        case EXPR_INT: case EXPR_FLOAT: case EXPR_BIGINT: case EXPR_DECIMAL: case EXPR_BYTE: case EXPR_STRING: case EXPR_BOOL: case EXPR_UNIT: case EXPR_IDENT:
            break;
        case EXPR_UNARY:
            resolve_expr_refs(e->as.unary.operand, ctx);
            break;
        case EXPR_BINARY:
            resolve_expr_refs(e->as.binary.left, ctx);
            resolve_expr_refs(e->as.binary.right, ctx);
            break;
        case EXPR_CALL:
            if (strchr(e->as.call.callee, '.') != NULL) {
                /* If the qualifier is not a module this file imported, it may still be a local holding a
                 * struct with a function-typed field (`handler.run(21)`). Only the type checker knows,
                 * so the call is left exactly as written and marked; check_call either resolves it to a
                 * field or calls ziman_report_unknown_module_call itself, so the diagnostic is as good
                 * as it was before this became possible. */
                char *q, *rest;
                split_qualified(e->as.call.callee, &q, &rest);
                if (strcmp(q, "prelude") == 0 && find_use(ctx->mod, q) == NULL) {
                    /* `prelude.format(...)` -- the one qualifier that names no module. It reaches
                     * the implicit namespace past a declaration of the author's own that has taken
                     * the bare name, which is otherwise unreachable: `self_qualify_module` has
                     * already rewritten this module's own `format` to `<path>::format`, and it
                     * skipped this node precisely because the callee was dotted, so dropping the
                     * qualifier here leaves a bare name that cannot match it. The flag is what
                     * stops the *entry* module's shadow from matching, there being no path prefix
                     * to tell the two apart there.
                     *
                     * `find_use` is still consulted first, so a module a program really did import
                     * under the name `prelude` keeps working and keeps winning. */
                    e->as.call.callee = rest;
                    e->as.call.prelude_qualified = 1;
                    rest = NULL; /* now owned by the node */
                } else if (find_use(ctx->mod, q) == NULL) {
                    e->as.call.unresolved_qualifier = 1;
                } else {
                    e->as.call.callee = resolve_function_ref(ctx, e->as.call.callee, e->line, e->col);
                }
                free(q);
                free(rest);
            }
            for (int i = 0; i < e->as.call.type_args.count; i++) resolve_type_refs(&e->as.call.type_args.items[i], ctx, e->line, e->col);
            for (int i = 0; i < e->as.call.args.count; i++) resolve_expr_refs(e->as.call.args.items[i], ctx);
            break;
        case EXPR_CAST:
            resolve_expr_refs(e->as.cast.operand, ctx);
            resolve_type_refs(&e->as.cast.target_type, ctx, e->line, e->col);
            break;
        case EXPR_ARRAY_LIT:
            for (int i = 0; i < e->as.array_lit.elements.count; i++) resolve_expr_refs(e->as.array_lit.elements.items[i], ctx);
            break;
        case EXPR_INDEX:
            resolve_expr_refs(e->as.index.array, ctx);
            resolve_expr_refs(e->as.index.index, ctx);
            break;
        case EXPR_STRUCT_LIT:
            if (strchr(e->as.struct_lit.type_name, '.') != NULL) {
                e->as.struct_lit.type_name = resolve_type_ref(ctx, e->as.struct_lit.type_name, e->line, e->col);
            }
            for (int i = 0; i < e->as.struct_lit.type_args.count; i++) resolve_type_refs(&e->as.struct_lit.type_args.items[i], ctx, e->line, e->col);
            for (int i = 0; i < e->as.struct_lit.fields.count; i++) resolve_expr_refs(e->as.struct_lit.fields.items[i].value, ctx);
            break;
        case EXPR_FIELD_ACCESS:
            resolve_expr_refs(e->as.field_access.base, ctx);
            /* `module.name` with no call parens: maybe a public function of an
             * imported module used as a value. Only a candidate -- a local
             * variable named like the qualifier wins in the type checker, and
             * a name that isn't a function is left as ordinary field access. */
            if (e->as.field_access.base->kind == EXPR_IDENT) {
                UseDecl *u = find_use(ctx->mod, e->as.field_access.base->as.ident_name);
                if (u != NULL) {
                    Module *target = find_loaded(ctx->ldr, u->path);
                    char *qualified = qualify_name(u->path, e->as.field_access.field_name);
                    int found = 0, is_not_visible = 0;
                    Visibility found_vis = VIS_PRIVATE;
                    for (int i = 0; i < target->functions.count; i++) {
                        if (strcmp(target->functions.items[i]->name, qualified) == 0) {
                            found = 1;
                            found_vis = target->functions.items[i]->visibility;
                            if (visibility_allows(found_vis, ctx->mod, target)) e->fn_ref_name = qualified;
                            else is_not_visible = 1;
                            break;
                        }
                    }
                    if (!found || is_not_visible) {
                        /* Not a usable function. If the qualifier really is a
                         * local variable this is ordinary field access and the
                         * hint is never shown; if it is not, this is the
                         * message the author needs instead of "undefined
                         * variable 'util'". A leading '!' marks it as a hint. */
                        const char *field = e->as.field_access.field_name;
                        /* What the name actually is, when it is something. `mod.NAME` reaches
                         * functions only, so a `pub val` was reported as "module has no function",
                         * which sends the reader hunting for a function that was never there --
                         * `number.float_min_positive` being the live example. The name is right and
                         * the form is wrong (C20). Vals only: a struct or enum here is a parser
                         * error before this runs, with a message that already says what to do. */
                        int is_val = 0;
                        for (int i = 0; i < target->vals.count && !found && !is_val; i++) {
                            if (strcmp(target->vals.items[i]->name, qualified) == 0 &&
                                visibility_allows(target->vals.items[i]->visibility, ctx->mod, target)) {
                                is_val = 1;
                            }
                        }
                        size_t n = strlen(u->path) * 2 + strlen(field) * 3 + 160;
                        char *hint = ziman_xmalloc(n);
                        if (is_not_visible) {
                            snprintf(hint, n, "!function '%s' in module '%s' is %s", field, u->path,
                                     visibility_violation_reason(found_vis));
                        } else if (is_val) {
                            snprintf(hint, n,
                                     "!'%s' in module '%s' is a global val, not a function -- write `use %s.%s` and then `%s`",
                                     field, u->path, u->path, field, field);
                        } else {
                            snprintf(hint, n, "!module '%s' has no function '%s'", u->path, field);
                        }
                        e->fn_ref_name = hint;
                    }
                }
            }
            break;
        case EXPR_WITH:
            resolve_expr_refs(e->as.with_expr.base, ctx);
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) resolve_expr_refs(e->as.with_expr.overrides.items[i].value, ctx);
            break;
        case EXPR_ENUM_ACCESS:
            if (strchr(e->as.enum_access.enum_name, '.') != NULL) {
                e->as.enum_access.enum_name = resolve_enum_ref(ctx, e->as.enum_access.enum_name, e->line, e->col);
            }
            for (int i = 0; i < e->as.enum_access.type_args.count; i++) resolve_type_refs(&e->as.enum_access.type_args.items[i], ctx, e->line, e->col);
            for (int i = 0; i < e->as.enum_access.args.count; i++) resolve_expr_refs(e->as.enum_access.args.items[i], ctx);
            break;
        case EXPR_TRY_UNWRAP:
            resolve_expr_refs(e->as.try_unwrap.inner, ctx);
            break;
        case EXPR_VALUE_BLOCK:
            resolve_stmt_refs(e->as.value_block.inner, ctx);
            break;
        case EXPR_LAMBDA: {
            FunctionDecl *lam = e->lambda_decl;
            for (int i = 0; i < lam->params.count; i++) resolve_type_refs(&lam->params.items[i].type, ctx, lam->params.items[i].line, lam->params.items[i].col);
            resolve_type_refs(&lam->return_type, ctx, lam->line, lam->col);
            resolve_stmt_refs(lam->body, ctx);
            break;
        }
    }
}

static void resolve_stmt_refs(Stmt *s, CrossRefCtx *ctx) {
    switch (s->kind) {
        case STMT_VAL: case STMT_VAR:
            if (s->as.decl.has_declared_type) resolve_type_refs(&s->as.decl.declared_type, ctx, s->line, s->col);
            resolve_expr_refs(s->as.decl.init, ctx);
            break;
        case STMT_ASSIGN:
            resolve_expr_refs(s->as.assign.value, ctx);
            break;
        case STMT_INDEX_ASSIGN:
            resolve_expr_refs(s->as.index_assign.target, ctx);
            resolve_expr_refs(s->as.index_assign.value, ctx);
            break;
        case STMT_IF:
            resolve_expr_refs(s->as.if_stmt.cond, ctx);
            resolve_stmt_refs(s->as.if_stmt.then_block, ctx);
            if (s->as.if_stmt.else_block != NULL) resolve_stmt_refs(s->as.if_stmt.else_block, ctx);
            break;
        case STMT_WHILE:
            resolve_expr_refs(s->as.while_stmt.cond, ctx);
            resolve_stmt_refs(s->as.while_stmt.body, ctx);
            break;
        case STMT_FOR:
            if (s->as.for_stmt.is_range) {
                resolve_expr_refs(s->as.for_stmt.range_start, ctx);
                resolve_expr_refs(s->as.for_stmt.range_end, ctx);
            } else {
                resolve_expr_refs(s->as.for_stmt.iterable, ctx);
            }
            resolve_stmt_refs(s->as.for_stmt.body, ctx);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break; /* no sub-expressions */
        case STMT_SPAWN:
            if (strchr(s->as.spawn_stmt.callee, '.') != NULL) {
                s->as.spawn_stmt.callee = resolve_function_ref(ctx, s->as.spawn_stmt.callee, s->line, s->col);
            }
            for (int i = 0; i < s->as.spawn_stmt.type_args.count; i++) resolve_type_refs(&s->as.spawn_stmt.type_args.items[i], ctx, s->line, s->col);
            for (int i = 0; i < s->as.spawn_stmt.args.count; i++) resolve_expr_refs(s->as.spawn_stmt.args.items[i], ctx);
            if (s->as.spawn_stmt.on_error != NULL) resolve_expr_refs(s->as.spawn_stmt.on_error, ctx);
            break;
        case STMT_RETURN:
            if (s->as.ret.value != NULL) resolve_expr_refs(s->as.ret.value, ctx);
            break;
        case STMT_EXPR:
            resolve_expr_refs(s->as.expr_stmt.expr, ctx);
            break;
        case STMT_DEFER:
            resolve_expr_refs(s->as.defer_stmt.call, ctx);
            break;
        case STMT_SWITCH:
            resolve_expr_refs(s->as.switch_stmt.scrutinee, ctx);
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) resolve_stmt_refs(s->as.switch_stmt.arms[i].body, ctx);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) resolve_stmt_refs(s->as.block.stmts.items[i], ctx);
            break;
        case STMT_VALUE_BLOCK:
            if (s->as.value_block.has_declared_type) resolve_type_refs(&s->as.value_block.declared_type, ctx, s->line, s->col);
            resolve_stmt_refs(s->as.value_block.inner, ctx);
            break;
        case STMT_YIELD:
            resolve_expr_refs(s->as.yield_stmt.value, ctx);
            break;
    }
}

static void resolve_cross_refs(Loader *ldr, Module *mod) {
    CrossRefCtx ctx = {ldr, mod};
    for (int i = 0; i < mod->functions.count; i++) {
        FunctionDecl *fn = mod->functions.items[i];
        ziman_set_error_context(fn->derived_from);
        for (int pi = 0; pi < fn->params.count; pi++) {
            resolve_type_refs(&fn->params.items[pi].type, &ctx, fn->params.items[pi].line, fn->params.items[pi].col);
        }
        resolve_type_refs(&fn->return_type, &ctx, fn->line, fn->col);
        if (fn_has_body(fn)) resolve_stmt_refs(fn->body, &ctx);
        ziman_set_error_context(NULL);
    }
    for (int i = 0; i < mod->structs.count; i++) {
        StructDecl *sd = mod->structs.items[i];
        for (int fi = 0; fi < sd->fields.count; fi++) {
            resolve_type_refs(&sd->fields.items[fi].type, &ctx, sd->fields.items[fi].line, sd->fields.items[fi].col);
        }
    }
    for (int i = 0; i < mod->enums.count; i++) {
        EnumDecl *ed = mod->enums.items[i];
        for (int vi = 0; vi < ed->variants.count; vi++) {
            if (ed->variants.items[vi].has_payload) {
                resolve_type_refs(&ed->variants.items[vi].payload_type, &ctx,
                                   ed->variants.items[vi].line, ed->variants.items[vi].col);
            }
        }
    }
    for (int i = 0; i < mod->vals.count; i++) {
        GlobalValDecl *v = mod->vals.items[i];
        if (v->has_declared_type) resolve_type_refs(&v->type, &ctx, v->line, v->col);
        resolve_expr_refs(v->init, &ctx);
    }
}

/* ---- recursive module loading ---- */

static Module *parse_file_contents(const char *source) {
    TokenList tokens = lex(source);
    return parse(tokens);
}

/* Resolves and validates every per-name `use path.Name [as alias]` in
 * `mod`, once, up front -- not re-resolved at each bare reference the
 * way a dotted `mod.Name` is. Requires every use target already loaded
 * (true by construction: the caller loads every entry in `mod->uses`,
 * whole-module or per-name, before calling this). Must run before
 * `mod`'s own declarations are renamed by self_qualify_module, since
 * the collision check below needs to see their original bare names.
 *
 * Known gap, not guarded here: an `as` alias colliding with a prelude
 * function's bare name (`use foo.bar as parse_int`) would silently
 * retarget every bare `parse_int(...)` call in this module instead of
 * failing loudly -- narrow enough (nobody writes this by accident) that
 * it's left as a documented limitation rather than plumbing the
 * prelude's/builtins' name lists in here too. */
static NamedImport *resolve_named_imports(Loader *ldr, Module *mod, const char *file, int *out_count) {
    NamedImport *imports = NULL;
    int count = 0, cap = 0;

    /* Two whole-module `use`s that bind the same qualifier used to be accepted, and the first silently won:
     * `use model/patient` then `use entity/patient` left every `patient.x` going to model's, with the second
     * line doing nothing at all. A module is referenced by its path's last segment (or its `as` alias), so
     * two files of the same name in different directories collide -- which is common enough that the
     * reference backend writes 351 aliases, and those aliases are load-bearing rather than noise.
     *
     * Reported rather than resolved by inventing a name from two path segments, which was the other option
     * on the table: a name the author never wrote is exactly what this module system sets out to avoid
     * ("never a bare name that silently resolves to an import"), and `as` already says it explicitly. The
     * per-name form below has always rejected its own version of this; this is the same rule for the other
     * form, where the consequence of not having it was worse -- a wrong function quietly called. */
    /* --lint: an `as` that binds the name the import would bind anyway. `use controller/admin as admin`
     * and `use json.Json as Json` are lines that do nothing -- a whole-module import's qualifier
     * is already the path's last segment, and a per-name import already binds the imported name. There
     * were 16 of the first kind in the reference backend. This lives here rather than with the other
     * lint rules in typecheck.c because a module's `use` list is the loader's: it is already gone from
     * the merged Module the type checker is handed, which cost a first attempt at this rule. Reported at
     * the alias, since that is the word to delete. */
    /* `#![deprecated]` on a whole module, reported at each `use` of it -- the one line where the
     * dependency is written, and so the one line that changing it means editing. Not behind
     * `--lint`, unlike the alias rule below: a deprecation warns on every compile (tasks.md F68).
     *
     * Only from a file that is not itself a bundled module, which lands the same rule the
     * function, struct, enum and val cases use: a user file importing a deprecated module warns,
     * and one bundled module importing another stays silent, since typecheck.c's lint_same_source
     * already treats two bundled modules as the same source. The entry module is nobody's import,
     * so `#![deprecated]` on the program's own file warns nowhere -- correct, and stated here so
     * it is not later mistaken for a fault. */
    if (!mod->is_corelib) {
        for (int i = 0; i < mod->uses.count && file != NULL; i++) {
            UseDecl *u = &mod->uses.items[i];
            Module *target = find_loaded(ldr, u->path);
            if (target == NULL || !target->deprecated) continue;
            if (target->deprecated_message != NULL) {
                ziman_warning(file, u->line, u->col, "module '%s' is deprecated: %s",
                              u->path, target->deprecated_message);
            } else {
                ziman_warning(file, u->line, u->col, "module '%s' is deprecated", u->path);
            }
            typecheck_lint_note();
        }
    }
    if (typecheck_lint_enabled() && !mod->is_corelib) {
        for (int i = 0; i < mod->uses.count && file != NULL; i++) {
            UseDecl *u = &mod->uses.items[i];
            if (!u->has_alias) continue;
            const char *bound = NULL;
            if (u->qualifier != NULL) {
                const char *slash = strrchr(u->path, '/');
                const char *last = slash != NULL ? slash + 1 : u->path;
                if (strcmp(u->qualifier, last) == 0) bound = u->qualifier;
            } else if (u->local_name != NULL && u->imported_name != NULL
                       && strcmp(u->local_name, u->imported_name) == 0) {
                bound = u->local_name;
            }
            if (bound != NULL) {
                ziman_warning(file, u->alias_line, u->alias_col,
                              "'as %s' binds the name this import would bind anyway; drop it", bound);
                typecheck_lint_note();
            }
        }
    }

    for (int i = 0; i < mod->uses.count; i++) {
        UseDecl *a = &mod->uses.items[i];
        if (a->imported_name != NULL || a->qualifier == NULL) continue;
        for (int j = 0; j < i; j++) {
            UseDecl *b = &mod->uses.items[j];
            if (b->imported_name != NULL || b->qualifier == NULL) continue;
            if (strcmp(a->qualifier, b->qualifier) != 0) continue;
            if (strcmp(a->path, b->path) == 0) {
                ziman_error("module", a->line, a->col, "module '%s' is imported twice", a->path);
            }
            /* Suggest the name the author would most likely pick: the parent directory joined to the
             * last segment, so `entity/patient` suggests `entity_patient`. Falls back to a suffix when
             * the path has no parent segment to borrow. */
            const char *last_slash = strrchr(a->path, '/');
            char suggestion[128];
            if (last_slash != NULL) {
                const char *parent_start = a->path;
                for (const char *q = a->path; q < last_slash; q++) {
                    if (*q == '/') parent_start = q + 1;
                }
                snprintf(suggestion, sizeof suggestion, "%.*s_%s",
                         (int)(last_slash - parent_start), parent_start, a->qualifier);
            } else {
                snprintf(suggestion, sizeof suggestion, "%s_alt", a->qualifier);
            }
            ziman_error("module", a->line, a->col,
                        "'%s' and '%s' would both be referenced as '%s' -- give one an alias, "
                        "e.g. 'use %s as %s'",
                        b->path, a->path, a->qualifier, a->path, suggestion);
        }
    }

    for (int i = 0; i < mod->uses.count; i++) {
        UseDecl *u = &mod->uses.items[i];
        if (u->imported_name == NULL) continue; /* whole-module use */

        for (int j = 0; j < count; j++) {
            if (strcmp(imports[j].local_name, u->local_name) == 0) {
                ziman_error("module", u->line, u->col, "'%s' is imported more than once under the same name", u->local_name);
            }
        }
        for (int j = 0; j < mod->functions.count; j++) {
            if (strcmp(mod->functions.items[j]->name, u->local_name) == 0) {
                ziman_error("module", u->line, u->col, "'%s' is imported here but also declared in this module", u->local_name);
            }
        }
        for (int j = 0; j < mod->structs.count; j++) {
            if (strcmp(mod->structs.items[j]->name, u->local_name) == 0) {
                ziman_error("module", u->line, u->col, "'%s' is imported here but also declared in this module", u->local_name);
            }
        }
        for (int j = 0; j < mod->enums.count; j++) {
            if (strcmp(mod->enums.items[j]->name, u->local_name) == 0) {
                ziman_error("module", u->line, u->col, "'%s' is imported here but also declared in this module", u->local_name);
            }
        }
        for (int j = 0; j < mod->vals.count; j++) {
            if (strcmp(mod->vals.items[j]->name, u->local_name) == 0) {
                ziman_error("module", u->line, u->col, "'%s' is imported here but also declared in this module", u->local_name);
            }
        }

        Module *target = find_loaded(ldr, u->path);
        char *qualified = qualify_name(u->path, u->imported_name);
        int found = 0;
        Visibility vis = VIS_PRIVATE;
        ImportKind kind = IMPORT_FN;
        for (int j = 0; j < target->functions.count && !found; j++) {
            if (strcmp(target->functions.items[j]->name, qualified) == 0) {
                found = 1;
                kind = IMPORT_FN;
                vis = target->functions.items[j]->visibility;
            }
        }
        for (int j = 0; j < target->structs.count && !found; j++) {
            if (strcmp(target->structs.items[j]->name, qualified) == 0) {
                found = 1;
                kind = IMPORT_STRUCT;
                vis = target->structs.items[j]->visibility;
            }
        }
        for (int j = 0; j < target->enums.count && !found; j++) {
            if (strcmp(target->enums.items[j]->name, qualified) == 0) {
                found = 1;
                kind = IMPORT_ENUM;
                vis = target->enums.items[j]->visibility;
            }
        }
        for (int j = 0; j < target->vals.count && !found; j++) {
            if (strcmp(target->vals.items[j]->name, qualified) == 0) {
                found = 1;
                kind = IMPORT_VAL;
                vis = target->vals.items[j]->visibility;
            }
        }
        if (!found) {
            ziman_error("module", u->line, u->col, "module '%s' has no function, struct, enum, or global val '%s'", u->path, u->imported_name);
        }
        if (!visibility_allows(vis, mod, target)) {
            ziman_error("module", u->line, u->col, "'%s' in module '%s' is %s", u->imported_name, u->path, visibility_violation_reason(vis));
        }

        if (count == cap) {
            cap = cap == 0 ? 4 : cap * 2;
            imports = ziman_xrealloc(imports, (size_t)cap * sizeof(NamedImport));
        }
        imports[count].local_name = u->local_name;
        imports[count].qualified_name = qualified;
        imports[count].kind = kind;
        count++;
    }

    *out_count = count;
    return imports;
}

/* A folder may hold its own module under its own name: `use a/b` where a/b.zm does not exist but a/b/b.zm does
 * names the module a/b/b. A file of the shorter name wins when both exist. The path is rewritten in place, before
 * anything else reads it, so a module has one name however it was written. */
/* Whether `use_path` names a readable module file, by either route a `use` may take: beside the
 * importing file (the project root) or through the marker directory / ZIMAN_MODULE_PATH.
 *
 * Both routes, not just the first, and that is the fix for a real asymmetry. Until 2026-10-14 the
 * directory-module rewrite below consulted only the project root, so `use common` found
 * common/common.zm when the entry file sat beside it and did not when the same directory was named
 * by ZIMAN_MODULE_PATH -- which is precisely the arrangement that variable exists for. The manual
 * says both mechanisms are "two ways to point one search path at a directory", so a module shape
 * that resolves by one and not the other is a bug in the loader, not a rule. It made every
 * directory module in a project unreachable from a test directory of its own; found while writing
 * the backend's first #[test] units (docs/reviews/tasks.md F134). */
static int module_file_exists(Loader *ldr, const char *use_path) {
    char *file = resolve_file_path(ldr->project_root, use_path);
    FILE *f = fopen(file, "rb");
    free(file);
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    char *from_path = resolve_via_module_path(use_path, ldr->marker_dir);
    if (from_path != NULL) {
        free(from_path);
        return 1;
    }
    return 0;
}

static void canonicalize_use_path(Loader *ldr, UseDecl *u) {
    if (module_file_exists(ldr, u->path)) return;
    /* `use foo` may instead name the directory module foo/foo.zm. */
    const char *slash = strrchr(u->path, '/');
    const char *last = slash != NULL ? slash + 1 : u->path;
    size_t n = strlen(u->path) + strlen(last) + 2;
    char *inner = ziman_xmalloc(n);
    snprintf(inner, n, "%s/%s", u->path, last);
    if (module_file_exists(ldr, inner)) {
        u->path = inner;
    } else {
        free(inner);
    }
}

/* `#![repository(X)]`: finds X's struct and hands it to the generator.
 *
 * **The lookup is here and the emitting is in derive.c**, because X is in another file: at the
 * point generation runs it is the loader that knows which files are loaded, and `find_loaded` is
 * the only way from a `use` path to the module it named. The generated signatures need the struct
 * itself -- `delete(conn, id: Uuid)` takes its parameter's name and type from the `#[pk]` field --
 * which is the requirement that moved generation out of the parser in the first place.
 *
 * A struct in the module's own file is allowed and needs no imports, which `NULL` says. */
static void resolve_repository(Loader *ldr, Module *mod) {
    if (mod->repository_entity == NULL) {
        /* No entity, so nothing to generate -- except that a `#[repository]` function in such a
         * module is a body nobody will ever write, which `generate_derived_queries` refuses by
         * name. A module with neither is the overwhelmingly common case and does nothing here. */
        generate_derived_queries(mod, NULL);
        return;
    }
    for (int s = 0; s < mod->structs.count; s++) {
        if (strcmp(mod->structs.items[s]->name, mod->repository_entity) == 0) {
            generate_repository_functions(mod, mod->structs.items[s], NULL);
            generate_derived_queries(mod, mod->structs.items[s]);
            return;
        }
    }
    /* A loaded module's own declarations are already self-qualified (`entity/vital_sign::VitalSign`),
     * so the comparison is against the bare name after the separator -- the attribute names the
     * type as the source does. */
    for (int i = 0; i < mod->uses.count; i++) {
        Module *target = find_loaded(ldr, mod->uses.items[i].path);
        if (target == NULL) continue;
        for (int s = 0; s < target->structs.count; s++) {
            const char *name = target->structs.items[s]->name;
            const char *sep = strstr(name, "::");
            const char *bare = sep != NULL ? sep + 2 : name;
            if (strcmp(bare, mod->repository_entity) == 0) {
                generate_repository_functions(mod, target->structs.items[s], mod->uses.items[i].path);
                generate_derived_queries(mod, target->structs.items[s]);
                return;
            }
        }
    }
    ziman_error("module", mod->repository_line, mod->repository_col,
                "#![repository(%s)] names a type this module cannot see -- a repository is generated from its "
                "entity's own fields, so the file has to import it (use <module>.%s)",
                mod->repository_entity, mod->repository_entity);
}

static Module *load_module(Loader *ldr, const char *use_path, int ref_line, int ref_col) {
    Module *existing = find_loaded(ldr, use_path);
    if (existing != NULL) return existing;

    if (is_loading(ldr, use_path)) {
        ziman_error("module", ref_line, ref_col, "circular 'use' involving module '%s'", use_path);
    }
    push_loading(ldr, use_path);

    char *file_path = resolve_file_path(ldr->project_root, use_path);
    char *source = read_all(file_path);
    if (source == NULL) {
        /* Beside the importing file wins; ZIMAN_MODULE_PATH is only consulted when that misses. */
        char *from_path = resolve_via_module_path(use_path, ldr->marker_dir);
        if (from_path != NULL) {
            source = read_all(from_path);
            if (source != NULL) {
                free(file_path);
                file_path = from_path;
            } else {
                free(from_path);
            }
        }
    }
    int from_disk = source != NULL;
    if (source == NULL) {
        /* No project-relative file by this name -- fall back to a
         * bundled core module (see corelib.h) before giving up. A
         * project file always wins when both exist. */
        const char *core_source = corelib_source_for(use_path);
        if (core_source == NULL) {
            const char *mod_path = getenv("ZIMAN_MODULE_PATH");
            ziman_error("module", ref_line, ref_col,
                        "cannot find module '%s' (looked for %s%s%s, and it's not a bundled core module)",
                        use_path, file_path,
                        mod_path != NULL && mod_path[0] != '\0' ? ", then in ZIMAN_MODULE_PATH=" : "",
                        mod_path != NULL && mod_path[0] != '\0' ? mod_path : "");
        }
        source = ziman_xstrdup(core_source);
    }
    ziman_set_error_file(from_disk ? file_path : NULL);
    if (from_disk) ziman_register_source(file_path, source);
    Module *mod = parse_file_contents(source);
    /* A module found on disk is ordinarily a project's own, never part of the bundled standard
     * library -- except when its own name is also a bundled module's (see module_name_of's own
     * comment on the entry-file case this mirrors): developing the stdlib itself means its
     * modules sit on disk right next to each other, each resolving its sibling corelib imports
     * straight off disk the same way, and leaving one `is_corelib` while the one importing it
     * is corelib too (or vice versa) breaks `internal` visibility between them for no reason a
     * real cross-package boundary would ever justify. A project that deliberately shadows a
     * corelib module's exact name -- rare, and already usable as-is since this only changes same
     * vs. different *package*, never which file's declarations a `use` of that name actually
     * sees -- gets the same corelib-like internal reach its stand-in is shadowing, not a loss. */
    mod->is_corelib = !from_disk || corelib_source_for(use_path) != NULL;
    if (from_disk) {
        for (int i = 0; i < mod->functions.count; i++) mod->functions.items[i]->source_file = file_path;
        for (int i = 0; i < mod->structs.count; i++) mod->structs.items[i]->source_file = file_path;
        for (int i = 0; i < mod->enums.count; i++) mod->enums.items[i]->source_file = file_path;
        for (int i = 0; i < mod->vals.count; i++) mod->vals.items[i]->source_file = file_path;
    }

    for (int i = 0; i < mod->uses.count; i++) {
        UseDecl *u = &mod->uses.items[i];
        ziman_set_error_file(from_disk ? file_path : NULL);
        canonicalize_use_path(ldr, u);
        load_module(ldr, u->path, u->line, u->col);
    }
    ziman_set_error_file(from_disk ? file_path : NULL);

    /* **Generation runs here, and the position is the whole point.** Every dependency above is
     * loaded to completion, including its own generated declarations, so a generator may read an
     * imported type; and named imports below are not resolved yet, so a generator may still add
     * the imports its output needs (`ensure_module_import`). Parsing is too early for the first
     * and resolution is too late for the second, which leaves exactly this line.
     *
     * **And what it adds has to be loaded**, which is the part that segfaulted on the first
     * attempt: `#[derive(Entity("t"))]` ensures a `use eql`, appended after the loop above had
     * already run, so nothing loaded `eql` and resolving the generated call dereferenced a module
     * that was never there. The loop below picks up exactly the uses generation appended. */
    int uses_before_derive = mod->uses.count;
    generate_derived_functions(mod);
    resolve_repository(ldr, mod);
    ziman_finish_errors();
    for (int i = uses_before_derive; i < mod->uses.count; i++) {
        UseDecl *u = &mod->uses.items[i];
        ziman_set_error_file(from_disk ? file_path : NULL);
        canonicalize_use_path(ldr, u);
        load_module(ldr, u->path, u->line, u->col);
    }
    ziman_set_error_file(from_disk ? file_path : NULL);
    /* **And the generated declarations need the file they came from**, which the loops above
     * assigned before any of them existed. A `source_file` is what later phases report an error
     * *in*: without it, `derive(Ord)` on an unordered field said "type error at 2:1 (in derive(Ord)
     * for 'Bad')" and named no file. Two fixtures caught that and nothing else did, because the
     * field is only visible when a derive's output fails to check. */
    if (from_disk) {
        for (int i = 0; i < mod->functions.count; i++) mod->functions.items[i]->source_file = file_path;
        for (int i = 0; i < mod->structs.count; i++) mod->structs.items[i]->source_file = file_path;
        for (int i = 0; i < mod->enums.count; i++) mod->enums.items[i]->source_file = file_path;
        for (int i = 0; i < mod->vals.count; i++) mod->vals.items[i]->source_file = file_path;
    }

    int n_imports = 0;
    NamedImport *imports = resolve_named_imports(ldr, mod, file_path, &n_imports);
    self_qualify_module(mod, use_path, imports, n_imports);
    resolve_cross_refs(ldr, mod);

    pop_loading(ldr);
    register_loaded(ldr, use_path, mod);
    return mod;
}


Module *load_module_graph(const char *entry_path, const char *project_root_override) {
    Loader ldr = {0};
    ldr.project_root = project_root_override != NULL ? ziman_xstrdup(project_root_override) : dirname_of(entry_path);
    ldr.marker_dir = find_project_marker(ldr.project_root);

    /* Parsed and resolved before anything else, and stashed on `ldr`
     * itself (not just merged in later, see the merge site below) --
     * every other module's own resolve_cross_refs call (inside
     * load_module, and the entry module's own a few lines down) needs
     * to see the prelude's declarations too, since a bare type reference
     * to a prelude enum (`Option<T>`) can appear in *any* module, not
     * just the entry one. See resolve_type_refs's own comment for the
     * reclassification fallback this enables. */
    /* The prelude is several files (lib/prelude/<part>.zm), merged into one module before anything
     * resolves against it. It has to be one module and not a chain of them: a name may be declared
     * in more than one part -- `sort` is two declarations in array.zm, and an overload set that
     * spans worlds lives whole in collections.zm -- and the resolver has to see the entire
     * vocabulary at once to choose between them or to report an overlap. Each part is lexed on its
     * own so its line numbers are its own, and registered under its real path so that a diagnostic
     * inside the prelude prints the right excerpt; the merge below is the same push-every-list
     * shape load_module_graph uses for the program itself. */
    Module *prelude_mod = ziman_xcalloc(1, sizeof(Module));
    prelude_mod->is_corelib = 1;
    for (int p = 0; p < prelude_part_count(); p++) {
        const char *part_name = prelude_part_name(p);
        ziman_register_source(part_name, prelude_part_source(p));
        ziman_set_error_file(part_name);
        Module *part = parse_file_contents(prelude_part_source(p));
        /* A `use` here would be silently dropped by the merge, and the prelude is the one place
         * that cannot have one anyway (it is merged bare into every program, so there is nothing
         * for it to import from that is not already beside it). */
        if (part->uses.count > 0) {
            ziman_error("module", part->uses.items[0].line, part->uses.items[0].col,
                        "a prelude part cannot `use` another module -- every file in lib/prelude/ "
                        "is merged bare into every program, so they already see each other");
        }
        for (int i = 0; i < part->functions.count; i++) {
            /* Not `source_file`: that one means "the user wrote this" to the lint gate and to
             * coverage. This one is for the diagnostic alone. */
            part->functions.items[i]->origin_file = part_name;
            function_list_push(&prelude_mod->functions, part->functions.items[i]);
        }
        for (int i = 0; i < part->structs.count; i++) struct_list_push(&prelude_mod->structs, part->structs.items[i]);
        for (int i = 0; i < part->enums.count; i++) enum_list_push(&prelude_mod->enums, part->enums.items[i]);
        for (int i = 0; i < part->vals.count; i++) global_val_list_push(&prelude_mod->vals, part->vals.items[i]);
    }
    ziman_set_error_file(NULL);
    resolve_cross_refs(&ldr, prelude_mod);
    ldr.prelude_mod = prelude_mod;
    g_prelude_for_hints = prelude_mod;

    char *entry_source = read_all(entry_path);
    if (entry_source == NULL) {
        ziman_error("module", 0, 0, "cannot open '%s'", entry_path);
    }
    ziman_set_error_file(entry_path);
    ziman_register_source(entry_path, entry_source);
    Module *entry_mod = parse_file_contents(entry_source);
    /* An entry file is ordinarily a project's own program, never part of the bundled standard
     * library (same_package's whole point is distinguishing the two) -- but the language server
     * analyzes whatever file is open as its own "entry" (see lsp.c's run_analysis), which for
     * someone editing the stdlib's own source is one of the bundled modules itself. Left
     * unmarked, such a file would be the only corelib module ever loaded as a non-corelib
     * package, breaking every `internal` reference between it and its sibling corelib modules
     * (e.g. lib/http/client.zm's own call into net.zm's `internal fn idle_take`) -- not just a
     * diagnostics false positive, but the load_module_graph error that causes (caught via
     * ziman_recovery_push in lsp.c) leaves g_module on its previous, unrelated value, so every
     * go-to-definition/hover/symbol request against the open file silently finds nothing. Safe
     * for ordinary (non-LSP) execution too: a corelib module's own name is a name nobody else's
     * real program entry would coincidentally share and then expect `ziman` to run as a program
     * missing `main` in the first place. */
    char *entry_module_name = module_name_of(entry_path);
    if (corelib_source_for(entry_module_name) != NULL) entry_mod->is_corelib = 1;
    free(entry_module_name);
    for (int i = 0; i < entry_mod->functions.count; i++) entry_mod->functions.items[i]->source_file = entry_path;
    for (int i = 0; i < entry_mod->structs.count; i++) entry_mod->structs.items[i]->source_file = entry_path;
    for (int i = 0; i < entry_mod->enums.count; i++) entry_mod->enums.items[i]->source_file = entry_path;
    for (int i = 0; i < entry_mod->vals.count; i++) entry_mod->vals.items[i]->source_file = entry_path;
    for (int i = 0; i < entry_mod->uses.count; i++) {
        UseDecl *u = &entry_mod->uses.items[i];
        ziman_set_error_file(entry_path);
        canonicalize_use_path(&ldr, u);
        load_module(&ldr, u->path, u->line, u->col);
    }
    ziman_set_error_file(entry_path);
    /* The entry module's own generation, for `load_module`'s reason: its dependencies are loaded
     * and its imports are not yet resolved -- and, as there, whatever generation imports has to be
     * loaded after it. */
    int entry_uses_before_derive = entry_mod->uses.count;
    generate_derived_functions(entry_mod);
    resolve_repository(&ldr, entry_mod);
    ziman_finish_errors();
    for (int i = entry_uses_before_derive; i < entry_mod->uses.count; i++) {
        UseDecl *u = &entry_mod->uses.items[i];
        ziman_set_error_file(entry_path);
        canonicalize_use_path(&ldr, u);
        load_module(&ldr, u->path, u->line, u->col);
    }
    ziman_set_error_file(entry_path);
    /* The generated declarations' own file, for the reason `load_module` gives above. */
    for (int i = 0; i < entry_mod->functions.count; i++) entry_mod->functions.items[i]->source_file = entry_path;
    for (int i = 0; i < entry_mod->structs.count; i++) entry_mod->structs.items[i]->source_file = entry_path;
    for (int i = 0; i < entry_mod->enums.count; i++) entry_mod->enums.items[i]->source_file = entry_path;
    for (int i = 0; i < entry_mod->vals.count; i++) entry_mod->vals.items[i]->source_file = entry_path;
    /* The entry module is never a `use` target, so it keeps its own bare
     * names (no self-qualify *rename* pass) -- a program with no `use`
     * statements behaves exactly as it always has. Its named imports
     * still need rewriting to their qualified form, same as any other
     * module's (empty own-name arrays below: only the import fallback in
     * self_qualify_type/expr/stmt can ever fire, never the own-name
     * branch, since the entry module has no "own qualified name" to
     * rewrite to in the first place). It still needs its own dotted
     * cross-module references resolved too. */
    int n_entry_imports = 0;
    NamedImport *entry_imports = resolve_named_imports(&ldr, entry_mod, entry_path, &n_entry_imports);
    char **entry_val_names = entry_mod->vals.count > 0 ? ziman_xmalloc((size_t)entry_mod->vals.count * sizeof(char *)) : NULL;
    for (int i = 0; i < entry_mod->vals.count; i++) entry_val_names[i] = entry_mod->vals.items[i]->name;
    check_no_val_shadowing(entry_mod, entry_val_names, entry_mod->vals.count, entry_imports, n_entry_imports);
    free(entry_val_names);
    SelfQualifyCtx entry_ctx = {"", NULL, 0, NULL, 0, NULL, 0, NULL, 0, entry_imports, n_entry_imports, 0};
    self_qualify_walk_module(entry_mod, &entry_ctx);
    resolve_cross_refs(&ldr, entry_mod);

    Module *merged = ziman_xcalloc(1, sizeof(Module));
    /* The prelude (see prelude.h) -- parsed and resolved up front, above
     * -- is merged into every program the same way the entry module's
     * own declarations are: bare names, no self-qualify pass, no `use`
     * required. Structs/enums/vals, not just functions, since
     * lib/prelude/<part>.zm carry ordinary Ziman declarations
     * (`Option<T>`) alongside native fn signatures now, not native-only
     * the way it used to be. Merged first, ahead of the entry module and
     * every dependency, so a global val there (none yet, but the
     * mechanism is general) is available to every other global val's
     * initializer -- see the `vals` merge-order comment below for why
     * order matters there specifically. */
    for (int i = 0; i < prelude_mod->functions.count; i++) function_list_push(&merged->functions, prelude_mod->functions.items[i]);
    for (int i = 0; i < prelude_mod->structs.count; i++) struct_list_push(&merged->structs, prelude_mod->structs.items[i]);
    for (int i = 0; i < prelude_mod->enums.count; i++) enum_list_push(&merged->enums, prelude_mod->enums.items[i]);
    for (int i = 0; i < entry_mod->functions.count; i++) function_list_push(&merged->functions, entry_mod->functions.items[i]);
    for (int i = 0; i < entry_mod->structs.count; i++) struct_list_push(&merged->structs, entry_mod->structs.items[i]);
    for (int i = 0; i < entry_mod->enums.count; i++) enum_list_push(&merged->enums, entry_mod->enums.items[i]);
    /* `vals` merge in dependency order, unlike functions/structs/enums
     * above (which don't need to, since they're looked up by exact name
     * regardless of list order): a global val's initializer is evaluated
     * eagerly, once, before `main` runs (see interpreter.c's
     * init_global_vals), in the SAME order this list puts them in -- so a
     * dependency's own globals must come before the entry module's,
     * matching `ldr.loaded`'s own guarantee (a module is only ever
     * registered as loaded after everything it `use`s already is -- see
     * load_module) that it's already a valid dependency order. The
     * prelude's own vals (none yet) come first of all, for the same
     * reason. */
    for (int i = 0; i < prelude_mod->vals.count; i++) global_val_list_push(&merged->vals, prelude_mod->vals.items[i]);
    for (int m = 0; m < ldr.loaded_count; m++) {
        Module *dep = ldr.loaded[m].module;
        for (int i = 0; i < dep->vals.count; i++) global_val_list_push(&merged->vals, dep->vals.items[i]);
    }
    for (int i = 0; i < entry_mod->vals.count; i++) global_val_list_push(&merged->vals, entry_mod->vals.items[i]);
    for (int m = 0; m < ldr.loaded_count; m++) {
        Module *dep = ldr.loaded[m].module;
        for (int i = 0; i < dep->functions.count; i++) function_list_push(&merged->functions, dep->functions.items[i]);
        for (int i = 0; i < dep->structs.count; i++) struct_list_push(&merged->structs, dep->structs.items[i]);
        for (int i = 0; i < dep->enums.count; i++) enum_list_push(&merged->enums, dep->enums.items[i]);
    }
    return merged;
}
