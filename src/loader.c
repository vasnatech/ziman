#define _POSIX_C_SOURCE 200809L
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "lexer.h"
#include "parser.h"

/* ---- small file/path helpers ---- */

static char *read_all(const char *path) {
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

/* ---- pass (a): self-qualify a module's own declarations ---- */

/* Snapshot of a module's own top-level names, taken *before* renaming them,
 * so the body-walk that follows can still recognize "this bare name is
 * mine" after the declarations themselves have already been renamed. */
typedef struct {
    const char *own_path;
    char **orig_fn_names;
    int n_fns;
    char **orig_struct_names;
    int n_structs;
    char **orig_enum_names;
    int n_enums;
} SelfQualifyCtx;

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
    if (t->kind != TY_STRUCT) return;
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
    /* Not one of this module's own structs or enums -- leave it; typecheck
     * reports "unknown struct or enum type" if it's genuinely undefined. */
}

static void self_qualify_expr(Expr *e, SelfQualifyCtx *ctx) {
    switch (e->kind) {
        case EXPR_INT: case EXPR_FLOAT: case EXPR_STRING: case EXPR_BOOL: case EXPR_IDENT:
            break;
        case EXPR_UNARY:
            self_qualify_expr(e->as.unary.operand, ctx);
            break;
        case EXPR_BINARY:
            self_qualify_expr(e->as.binary.left, ctx);
            self_qualify_expr(e->as.binary.right, ctx);
            break;
        case EXPR_CALL:
            if (strchr(e->as.call.callee, '.') == NULL) {
                for (int i = 0; i < ctx->n_fns; i++) {
                    if (strcmp(e->as.call.callee, ctx->orig_fn_names[i]) == 0) {
                        e->as.call.callee = qualify_name(ctx->own_path, e->as.call.callee);
                        break;
                    }
                }
            }
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
                for (int i = 0; i < ctx->n_structs; i++) {
                    if (strcmp(e->as.struct_lit.type_name, ctx->orig_struct_names[i]) == 0) {
                        e->as.struct_lit.type_name = qualify_name(ctx->own_path, e->as.struct_lit.type_name);
                        break;
                    }
                }
            }
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
            /* `enum_name` is always this-module-local for now (no
             * cross-module `use`-qualified enum access yet), so this
             * always either matches one of this module's own enums or is
             * genuinely undefined -- never something pass (b) needs to
             * resolve. `member_name` is never touched, same as a struct
             * field name never is. */
            for (int i = 0; i < ctx->n_enums; i++) {
                if (strcmp(e->as.enum_access.enum_name, ctx->orig_enum_names[i]) == 0) {
                    e->as.enum_access.enum_name = qualify_name(ctx->own_path, e->as.enum_access.enum_name);
                    break;
                }
            }
            for (int i = 0; i < e->as.enum_access.args.count; i++) self_qualify_expr(e->as.enum_access.args.items[i], ctx);
            break;
    }
}

static void self_qualify_stmt(Stmt *s, SelfQualifyCtx *ctx) {
    switch (s->kind) {
        case STMT_VAL: case STMT_VAR:
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
        case STMT_RETURN:
            if (s->as.ret.value != NULL) self_qualify_expr(s->as.ret.value, ctx);
            break;
        case STMT_EXPR:
            self_qualify_expr(s->as.expr_stmt.expr, ctx);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) self_qualify_stmt(s->as.block.stmts.items[i], ctx);
            break;
    }
}

static void self_qualify_module(Module *mod, const char *own_path) {
    int n_fns = mod->functions.count;
    int n_structs = mod->structs.count;
    int n_enums = mod->enums.count;
    char **orig_fn_names = n_fns > 0 ? malloc((size_t)n_fns * sizeof(char *)) : NULL;
    char **orig_struct_names = n_structs > 0 ? malloc((size_t)n_structs * sizeof(char *)) : NULL;
    char **orig_enum_names = n_enums > 0 ? malloc((size_t)n_enums * sizeof(char *)) : NULL;
    for (int i = 0; i < n_fns; i++) orig_fn_names[i] = mod->functions.items[i]->name;
    for (int i = 0; i < n_structs; i++) orig_struct_names[i] = mod->structs.items[i]->name;
    for (int i = 0; i < n_enums; i++) orig_enum_names[i] = mod->enums.items[i]->name;

    for (int i = 0; i < n_fns; i++) {
        mod->functions.items[i]->name = qualify_name(own_path, orig_fn_names[i]);
    }
    for (int i = 0; i < n_structs; i++) {
        mod->structs.items[i]->name = qualify_name(own_path, orig_struct_names[i]);
    }
    for (int i = 0; i < n_enums; i++) {
        mod->enums.items[i]->name = qualify_name(own_path, orig_enum_names[i]);
    }

    SelfQualifyCtx ctx = {own_path, orig_fn_names, n_fns, orig_struct_names, n_structs, orig_enum_names, n_enums};
    for (int i = 0; i < n_fns; i++) {
        FunctionDecl *fn = mod->functions.items[i];
        for (int pi = 0; pi < fn->params.count; pi++) self_qualify_type(&fn->params.items[pi].type, &ctx);
        self_qualify_type(&fn->return_type, &ctx);
        if (!fn->is_native) self_qualify_stmt(fn->body, &ctx);
    }
    for (int i = 0; i < n_structs; i++) {
        StructDecl *sd = mod->structs.items[i];
        for (int fi = 0; fi < sd->fields.count; fi++) self_qualify_type(&sd->fields.items[fi].type, &ctx);
    }
    for (int i = 0; i < n_enums; i++) {
        EnumDecl *ed = mod->enums.items[i];
        for (int vi = 0; vi < ed->variants.count; vi++) {
            if (ed->variants.items[vi].has_payload) self_qualify_type(&ed->variants.items[vi].payload_type, &ctx);
        }
    }

    free(orig_fn_names);
    free(orig_struct_names);
    free(orig_enum_names);
}

/* ---- pass (b): resolve cross-module (dotted) references, enforcing pub ---- */

typedef struct {
    Loader *ldr;
    Module *mod; /* whose `use` list gives meaning to a qualifier */
} CrossRefCtx;

static UseDecl *find_use(Module *mod, const char *qualifier) {
    for (int i = 0; i < mod->uses.count; i++) {
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

static char *resolve_function_ref(CrossRefCtx *ctx, const char *dotted, int line, int col) {
    char *qualifier, *rest;
    split_qualified(dotted, &qualifier, &rest);
    UseDecl *u = find_use(ctx->mod, qualifier);
    if (u == NULL) {
        ziman_error("module", line, col, "no module imported under the name '%s'", qualifier);
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
        ziman_error("module", line, col, "no module imported under the name '%s'", qualifier);
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
    if (t->kind != TY_STRUCT) return;
    if (strchr(t->struct_name, '.') != NULL) {
        t->struct_name = resolve_type_ref(ctx, t->struct_name, line, col);
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
}

static void resolve_expr_refs(Expr *e, CrossRefCtx *ctx) {
    switch (e->kind) {
        case EXPR_INT: case EXPR_FLOAT: case EXPR_STRING: case EXPR_BOOL: case EXPR_IDENT:
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
            for (int i = 0; i < e->as.struct_lit.fields.count; i++) resolve_expr_refs(e->as.struct_lit.fields.items[i].value, ctx);
            break;
        case EXPR_FIELD_ACCESS:
            resolve_expr_refs(e->as.field_access.base, ctx);
            break;
        case EXPR_WITH:
            resolve_expr_refs(e->as.with_expr.base, ctx);
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) resolve_expr_refs(e->as.with_expr.overrides.items[i].value, ctx);
            break;
        case EXPR_ENUM_ACCESS:
            /* `enum_name` itself is never cross-module (no `use`-qualified
             * enum access yet -- see self_qualify_expr's EXPR_ENUM_ACCESS
             * comment), but the arguments might still contain one. */
            for (int i = 0; i < e->as.enum_access.args.count; i++) resolve_expr_refs(e->as.enum_access.args.items[i], ctx);
            break;
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
        case STMT_RETURN:
            if (s->as.ret.value != NULL) resolve_expr_refs(s->as.ret.value, ctx);
            break;
        case STMT_EXPR:
            resolve_expr_refs(s->as.expr_stmt.expr, ctx);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) resolve_stmt_refs(s->as.block.stmts.items[i], ctx);
            break;
    }
}

static void resolve_cross_refs(Loader *ldr, Module *mod) {
    CrossRefCtx ctx = {ldr, mod};
    for (int i = 0; i < mod->functions.count; i++) {
        FunctionDecl *fn = mod->functions.items[i];
        for (int pi = 0; pi < fn->params.count; pi++) {
            resolve_type_refs(&fn->params.items[pi].type, &ctx, fn->params.items[pi].line, fn->params.items[pi].col);
        }
        resolve_type_refs(&fn->return_type, &ctx, fn->line, fn->col);
        if (!fn->is_native) resolve_stmt_refs(fn->body, &ctx);
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
}

/* ---- recursive module loading ---- */

static Module *parse_file_contents(const char *source) {
    TokenList tokens = lex(source);
    return parse(tokens);
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
        ziman_error("module", ref_line, ref_col, "cannot find module '%s' (looked for %s)", use_path, file_path);
    }
    Module *mod = parse_file_contents(source);

    for (int i = 0; i < mod->uses.count; i++) {
        UseDecl *u = &mod->uses.items[i];
        load_module(ldr, u->path, u->line, u->col);
    }

    self_qualify_module(mod, use_path);
    resolve_cross_refs(ldr, mod);

    pop_loading(ldr);
    register_loaded(ldr, use_path, mod);
    return mod;
}

Module *load_module_graph(const char *entry_path) {
    Loader ldr = {0};
    ldr.project_root = dirname_of(entry_path);

    char *entry_source = read_all(entry_path);
    if (entry_source == NULL) {
        fprintf(stderr, "ziman: cannot open '%s'\n", entry_path);
        exit(1);
    }
    Module *entry_mod = parse_file_contents(entry_source);
    for (int i = 0; i < entry_mod->uses.count; i++) {
        UseDecl *u = &entry_mod->uses.items[i];
        load_module(&ldr, u->path, u->line, u->col);
    }
    /* The entry module is never a `use` target, so it keeps its own bare
     * names (no self-qualify pass) -- a program with no `use` statements
     * behaves exactly as it always has. It still needs its own cross-
     * module references resolved. */
    resolve_cross_refs(&ldr, entry_mod);

    Module *merged = calloc(1, sizeof(Module));
    for (int i = 0; i < entry_mod->functions.count; i++) function_list_push(&merged->functions, entry_mod->functions.items[i]);
    for (int i = 0; i < entry_mod->structs.count; i++) struct_list_push(&merged->structs, entry_mod->structs.items[i]);
    for (int i = 0; i < entry_mod->enums.count; i++) enum_list_push(&merged->enums, entry_mod->enums.items[i]);
    for (int m = 0; m < ldr.loaded_count; m++) {
        Module *dep = ldr.loaded[m].module;
        for (int i = 0; i < dep->functions.count; i++) function_list_push(&merged->functions, dep->functions.items[i]);
        for (int i = 0; i < dep->structs.count; i++) struct_list_push(&merged->structs, dep->structs.items[i]);
        for (int i = 0; i < dep->enums.count; i++) enum_list_push(&merged->enums, dep->enums.items[i]);
    }
    return merged;
}
