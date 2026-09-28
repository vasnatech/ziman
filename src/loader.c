#define _POSIX_C_SOURCE 200809L
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "corelib.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "prelude.h"

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
        if (strcmp(g_override_paths[i], path) == 0) return strdup(g_override_texts[i]);
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

static char *dirname_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) return strdup("");
    size_t len = (size_t)(slash - path);
    char *dir = malloc(len + 1);
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

static char *resolve_file_path(const char *project_root, const char *use_path) {
    size_t len = strlen(project_root) + strlen(use_path) + 6;
    char *out = malloc(len);
    if (project_root[0] == '\0') {
        snprintf(out, len, "%s.zm", use_path);
    } else {
        snprintf(out, len, "%s/%s.zm", project_root, use_path);
    }
    return out;
}

/* "<use_path>::<bare_name>" -- the fully-qualified form every cross-module
 * reference (and every non-entry module's own declaration) is rewritten
 * to. Chosen so it can never collide with a bare (entry-module or
 * builtin) name, which never contains "::". */
static char *qualify_name(const char *use_path, const char *bare_name) {
    size_t len = strlen(use_path) + 2 + strlen(bare_name) + 1;
    char *q = malloc(len);
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
        ldr->loaded = realloc(ldr->loaded, (size_t)ldr->loaded_cap * sizeof(LoadedEntry));
    }
    ldr->loaded[ldr->loaded_count].use_path = strdup(use_path);
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
        ldr->loading_stack = realloc(ldr->loading_stack, (size_t)ldr->loading_cap * sizeof(char *));
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
static void self_qualify_type(Type *t, SelfQualifyCtx *ctx) {
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
            char *qualified = qualify_name(ctx->own_path, t->struct_name);
            t->struct_name = NULL;
            t->enum_name = qualified;
            t->kind = TY_ENUM;
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
            t->struct_name = NULL;
            t->enum_name = imp->qualified_name;
            t->kind = TY_ENUM;
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
             * (`use collections.Option`), or is genuinely undefined --
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
        if (!fn->is_native) self_qualify_stmt(fn->body, ctx);
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
        case STMT_ASSIGN: case STMT_BREAK: case STMT_CONTINUE:
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
        if (!fn->is_native) check_no_val_shadowing_stmt(fn->body, val_names, n_val_names, imports, n_imports);
    }
}

static void self_qualify_module(Module *mod, const char *own_path, NamedImport *imports, int n_imports) {
    int n_fns = mod->functions.count;
    int n_structs = mod->structs.count;
    int n_enums = mod->enums.count;
    int n_vals = mod->vals.count;
    char **orig_fn_names = n_fns > 0 ? malloc((size_t)n_fns * sizeof(char *)) : NULL;
    char **orig_struct_names = n_structs > 0 ? malloc((size_t)n_structs * sizeof(char *)) : NULL;
    char **orig_enum_names = n_enums > 0 ? malloc((size_t)n_enums * sizeof(char *)) : NULL;
    char **orig_val_names = n_vals > 0 ? malloc((size_t)n_vals * sizeof(char *)) : NULL;
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
    *qualifier_out = malloc(qlen + 1);
    memcpy(*qualifier_out, dotted, qlen);
    (*qualifier_out)[qlen] = '\0';
    *rest_out = strdup(dot + 1);
}

/* `p.to_string()` parses as a call into a module named `p`. The most common
 * reason there is no such module is that `p` is a value and the author
 * reached for a method, which Ziman doesn't have -- so say that first, with
 * the function-call form that does work. */
static void report_unknown_module_call(const char *qualifier, const char *member, int line, int col) {
    char hint[160];
    if (strcmp(member, "to_string") == 0 || strcmp(member, "toString") == 0 || strcmp(member, "to_str") == 0) {
        snprintf(hint, sizeof hint, "str(%s)", qualifier);
    } else if (strcmp(member, "len") == 0 || strcmp(member, "length") == 0 || strcmp(member, "size") == 0) {
        snprintf(hint, sizeof hint, "len(%s)", qualifier);
    } else if (strcmp(member, "push") == 0 || strcmp(member, "append") == 0) {
        snprintf(hint, sizeof hint, "%s = push(%s, item)", qualifier, qualifier);
    } else {
        snprintf(hint, sizeof hint, "%s(%s, ...)", member, qualifier);
    }
    ziman_error("module", line, col,
                "'%s.%s(...)': '%s' is not an imported module, and Ziman has no methods -- if '%s' is a value, call a function on it instead, e.g. %s "
                "(if '%s' should be a module, add 'use %s')",
                qualifier, member, qualifier, qualifier, hint, qualifier, qualifier);
}

static char *resolve_function_ref(CrossRefCtx *ctx, const char *dotted, int line, int col) {
    char *qualifier, *rest;
    split_qualified(dotted, &qualifier, &rest);
    UseDecl *u = find_use(ctx->mod, qualifier);
    if (u == NULL) {
        report_unknown_module_call(qualifier, rest, line, col);
    }
    Module *target = find_loaded(ctx->ldr, u->path);
    char *qualified = qualify_name(u->path, rest);
    FunctionDecl *found = NULL;
    for (int i = 0; i < target->functions.count; i++) {
        if (strcmp(target->functions.items[i]->name, qualified) == 0) { found = target->functions.items[i]; break; }
    }
    if (found == NULL) {
        ziman_error("module", line, col, "module '%s' has no function '%s'", u->path, rest);
    }
    if (!found->is_public) {
        ziman_error("module", line, col, "function '%s' in module '%s' is private (add 'pub' to export it)", rest, u->path);
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
    if (!found->is_public) {
        ziman_error("module", line, col, "struct '%s' in module '%s' is private (add 'pub' to export it)", rest, u->path);
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
    if (!found->is_public) {
        ziman_error("module", line, col, "enum '%s' in module '%s' is private (add 'pub' to export it)", rest, u->path);
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
            if (!target->structs.items[i]->is_public) {
                ziman_error("module", line, col, "struct '%s' in module '%s' is private (add 'pub' to export it)", rest, u->path);
            }
            t->struct_name = qualified;
            free(qualifier);
            free(rest);
            return;
        }
    }
    for (int i = 0; i < target->enums.count; i++) {
        if (strcmp(target->enums.items[i]->name, qualified) == 0) {
            if (!target->enums.items[i]->is_public) {
                ziman_error("module", line, col, "enum '%s' in module '%s' is private (add 'pub' to export it)", rest, u->path);
            }
            t->kind = TY_ENUM;
            t->struct_name = NULL;
            t->enum_name = qualified;
            free(qualifier);
            free(rest);
            return;
        }
    }
    ziman_error("module", line, col, "module '%s' has no struct or enum '%s'", u->path, rest);
}

static void resolve_type_refs(Type *t, CrossRefCtx *ctx, int line, int col) {
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
            t->enum_name = t->struct_name;
            t->struct_name = NULL;
            t->kind = TY_ENUM;
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
     * struct (`StringBuilder`), not a second, redundant place to look. */
    if (ctx->ldr->prelude_mod != NULL) {
        for (int i = 0; i < ctx->ldr->prelude_mod->enums.count; i++) {
            if (strcmp(t->struct_name, ctx->ldr->prelude_mod->enums.items[i]->name) == 0) {
                t->enum_name = t->struct_name;
                t->struct_name = NULL;
                t->kind = TY_ENUM;
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
                e->as.call.callee = resolve_function_ref(ctx, e->as.call.callee, e->line, e->col);
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
                    int found = 0, is_private = 0;
                    for (int i = 0; i < target->functions.count; i++) {
                        if (strcmp(target->functions.items[i]->name, qualified) == 0) {
                            found = 1;
                            if (target->functions.items[i]->is_public) e->fn_ref_name = qualified;
                            else is_private = 1;
                            break;
                        }
                    }
                    if (!found || is_private) {
                        /* Not a usable function. If the qualifier really is a
                         * local variable this is ordinary field access and the
                         * hint is never shown; if it is not, this is the
                         * message the author needs instead of "undefined
                         * variable 'util'". A leading '!' marks it as a hint. */
                        size_t n = strlen(u->path) + strlen(e->as.field_access.field_name) + 96;
                        char *hint = malloc(n);
                        if (is_private) snprintf(hint, n, "!function '%s' in module '%s' is private (add 'pub' to export it)", e->as.field_access.field_name, u->path);
                        else snprintf(hint, n, "!module '%s' has no function '%s'", u->path, e->as.field_access.field_name);
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
        if (!fn->is_native) resolve_stmt_refs(fn->body, &ctx);
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
static NamedImport *resolve_named_imports(Loader *ldr, Module *mod, int *out_count) {
    NamedImport *imports = NULL;
    int count = 0, cap = 0;

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
        int found = 0, is_public = 0;
        ImportKind kind = IMPORT_FN;
        for (int j = 0; j < target->functions.count && !found; j++) {
            if (strcmp(target->functions.items[j]->name, qualified) == 0) {
                found = 1;
                kind = IMPORT_FN;
                is_public = target->functions.items[j]->is_public;
            }
        }
        for (int j = 0; j < target->structs.count && !found; j++) {
            if (strcmp(target->structs.items[j]->name, qualified) == 0) {
                found = 1;
                kind = IMPORT_STRUCT;
                is_public = target->structs.items[j]->is_public;
            }
        }
        for (int j = 0; j < target->enums.count && !found; j++) {
            if (strcmp(target->enums.items[j]->name, qualified) == 0) {
                found = 1;
                kind = IMPORT_ENUM;
                is_public = target->enums.items[j]->is_public;
            }
        }
        for (int j = 0; j < target->vals.count && !found; j++) {
            if (strcmp(target->vals.items[j]->name, qualified) == 0) {
                found = 1;
                kind = IMPORT_VAL;
                is_public = target->vals.items[j]->is_public;
            }
        }
        if (!found) {
            ziman_error("module", u->line, u->col, "module '%s' has no function, struct, enum, or global val '%s'", u->path, u->imported_name);
        }
        if (!is_public) {
            ziman_error("module", u->line, u->col, "'%s' in module '%s' is private (add 'pub' to export it)", u->imported_name, u->path);
        }

        if (count == cap) {
            cap = cap == 0 ? 4 : cap * 2;
            imports = realloc(imports, (size_t)cap * sizeof(NamedImport));
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
static void canonicalize_use_path(Loader *ldr, UseDecl *u) {
    char *file = resolve_file_path(ldr->project_root, u->path);
    char *found = read_all(file);
    free(file);
    if (found != NULL) {
        free(found);
        return;
    }
    const char *slash = strrchr(u->path, '/');
    const char *last = slash != NULL ? slash + 1 : u->path;
    size_t n = strlen(u->path) + strlen(last) + 2;
    char *inner = malloc(n);
    snprintf(inner, n, "%s/%s", u->path, last);
    file = resolve_file_path(ldr->project_root, inner);
    found = read_all(file);
    free(file);
    if (found != NULL) {
        free(found);
        u->path = inner;
    } else {
        free(inner);
    }
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
    int from_disk = source != NULL;
    if (source == NULL) {
        /* No project-relative file by this name -- fall back to a
         * bundled core module (see corelib.h) before giving up. A
         * project file always wins when both exist. */
        const char *core_source = corelib_source_for(use_path);
        if (core_source == NULL) {
            ziman_error("module", ref_line, ref_col,
                        "cannot find module '%s' (looked for %s, and it's not a bundled core module)",
                        use_path, file_path);
        }
        source = strdup(core_source);
    }
    ziman_set_error_file(from_disk ? file_path : NULL);
    if (from_disk) ziman_register_source(file_path, source);
    Module *mod = parse_file_contents(source);
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

    int n_imports = 0;
    NamedImport *imports = resolve_named_imports(ldr, mod, &n_imports);
    self_qualify_module(mod, use_path, imports, n_imports);
    resolve_cross_refs(ldr, mod);

    pop_loading(ldr);
    register_loaded(ldr, use_path, mod);
    return mod;
}

Module *load_module_graph(const char *entry_path) {
    Loader ldr = {0};
    ldr.project_root = dirname_of(entry_path);

    /* Parsed and resolved before anything else, and stashed on `ldr`
     * itself (not just merged in later, see the merge site below) --
     * every other module's own resolve_cross_refs call (inside
     * load_module, and the entry module's own a few lines down) needs
     * to see the prelude's declarations too, since a bare type reference
     * to a prelude enum (`Option<T>`) can appear in *any* module, not
     * just the entry one. See resolve_type_refs's own comment for the
     * reclassification fallback this enables. */
    Module *prelude_mod = parse_file_contents(prelude_source());
    resolve_cross_refs(&ldr, prelude_mod);
    ldr.prelude_mod = prelude_mod;

    char *entry_source = read_all(entry_path);
    if (entry_source == NULL) {
        ziman_error("module", 0, 0, "cannot open '%s'", entry_path);
    }
    ziman_set_error_file(entry_path);
    ziman_register_source(entry_path, entry_source);
    Module *entry_mod = parse_file_contents(entry_source);
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
    NamedImport *entry_imports = resolve_named_imports(&ldr, entry_mod, &n_entry_imports);
    char **entry_val_names = entry_mod->vals.count > 0 ? malloc((size_t)entry_mod->vals.count * sizeof(char *)) : NULL;
    for (int i = 0; i < entry_mod->vals.count; i++) entry_val_names[i] = entry_mod->vals.items[i]->name;
    check_no_val_shadowing(entry_mod, entry_val_names, entry_mod->vals.count, entry_imports, n_entry_imports);
    free(entry_val_names);
    SelfQualifyCtx entry_ctx = {"", NULL, 0, NULL, 0, NULL, 0, NULL, 0, entry_imports, n_entry_imports, 0};
    self_qualify_walk_module(entry_mod, &entry_ctx);
    resolve_cross_refs(&ldr, entry_mod);

    Module *merged = calloc(1, sizeof(Module));
    /* The prelude (see prelude.h) -- parsed and resolved up front, above
     * -- is merged into every program the same way the entry module's
     * own declarations are: bare names, no self-qualify pass, no `use`
     * required. Structs/enums/vals, not just functions, since
     * lib/prelude.zm carries ordinary Ziman declarations
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
