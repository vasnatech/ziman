#include "typecheck.h"

#include <string.h>

#include "env.h"
#include "error.h"

static FunctionDecl *find_function(Program *program, const char *name) {
    for (int i = 0; i < program->functions.count; i++) {
        if (strcmp(program->functions.items[i]->name, name) == 0) return program->functions.items[i];
    }
    return NULL;
}

static Type check_expr(Program *program, Env *env, Expr *e);

static void require_not_void(Expr *e, const char *context) {
    if (e->resolved_type.kind == TY_VOID) {
        ziman_error("type", e->line, e->col, "cannot use a void value %s", context);
    }
}

static Type check_binary(Program *program, Env *env, Expr *e) {
    Type lt = check_expr(program, env, e->as.binary.left);
    Type rt = check_expr(program, env, e->as.binary.right);
    require_not_void(e->as.binary.left, "as an operand");
    require_not_void(e->as.binary.right, "as an operand");

    switch (e->as.binary.op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
            if (lt.kind != rt.kind || (lt.kind != TY_INT && lt.kind != TY_FLOAT)) {
                ziman_error("type", e->line, e->col,
                            "arithmetic operator requires two operands of the same numeric type, found %s and %s (use 'as' to cast explicitly)",
                            type_name(lt), type_name(rt));
            }
            return lt;
        case OP_LT:
        case OP_GT:
        case OP_LE:
        case OP_GE:
            if (lt.kind != rt.kind || (lt.kind != TY_INT && lt.kind != TY_FLOAT)) {
                ziman_error("type", e->line, e->col,
                            "comparison operator requires two operands of the same numeric type, found %s and %s",
                            type_name(lt), type_name(rt));
            }
            return type_scalar(TY_BOOL);
        case OP_EQ:
        case OP_NEQ:
            if (lt.kind == TY_ARRAY || rt.kind == TY_ARRAY) {
                ziman_error("type", e->line, e->col, "arrays cannot be compared with '=='/'!=' in v0");
            }
            if (!type_equals(lt, rt)) {
                ziman_error("type", e->line, e->col, "cannot compare %s and %s for equality", type_name(lt), type_name(rt));
            }
            return type_scalar(TY_BOOL);
        case OP_AND:
        case OP_OR:
            if (lt.kind != TY_BOOL || rt.kind != TY_BOOL) {
                ziman_error("type", e->line, e->col, "'&&'/'||' require bool operands, found %s and %s", type_name(lt), type_name(rt));
            }
            return type_scalar(TY_BOOL);
        default:
            ziman_error("type", e->line, e->col, "internal error: not a binary operator");
            return type_scalar(TY_VOID);
    }
}

static Type check_unary(Program *program, Env *env, Expr *e) {
    Type t = check_expr(program, env, e->as.unary.operand);
    require_not_void(e->as.unary.operand, "as an operand");
    if (e->as.unary.op == OP_NEG) {
        if (t.kind != TY_INT && t.kind != TY_FLOAT) {
            ziman_error("type", e->line, e->col, "unary '-' requires an int or float operand, found %s", type_name(t));
        }
        return t;
    }
    /* OP_NOT */
    if (t.kind != TY_BOOL) {
        ziman_error("type", e->line, e->col, "unary '!' requires a bool operand, found %s", type_name(t));
    }
    return type_scalar(TY_BOOL);
}

/* Scans a format-string literal's already-unescaped content for `{}`
 * placeholders, exiting with a type error on a stray/unmatched brace.
 * `fmt_expr` is only used to anchor error locations -- v0 doesn't track
 * per-character positions inside a string literal, so mistakes are
 * reported at the literal as a whole. */
static int count_placeholders(Expr *fmt_expr) {
    const char *s = fmt_expr->as.string_val;
    int count = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '{') {
            if (p[1] != '}') {
                ziman_error("type", fmt_expr->line, fmt_expr->col,
                            "malformed placeholder in format string (expected '{}', found stray '{')");
            }
            count++;
            p++; /* skip the matching '}' too */
        } else if (*p == '}') {
            ziman_error("type", fmt_expr->line, fmt_expr->col, "unmatched '}' in format string");
        }
    }
    return count;
}

static Type check_call(Program *program, Env *env, Expr *e) {
    const char *name = e->as.call.callee;

    if (strcmp(name, "print") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'print' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        check_expr(program, env, e->as.call.args.items[0]);
        require_not_void(e->as.call.args.items[0], "as an argument");
        return type_scalar(TY_VOID);
    }

    if (strcmp(name, "concat") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'concat' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        for (int i = 0; i < 2; i++) {
            Expr *arg = e->as.call.args.items[i];
            Type arg_type = check_expr(program, env, arg);
            if (arg_type.kind != TY_STRING) {
                ziman_error("type", arg->line, arg->col,
                            "argument %d of 'concat' expects string, found %s (use 'str' to convert first)",
                            i + 1, type_name(arg_type));
            }
        }
        return type_scalar(TY_STRING);
    }

    if (strcmp(name, "str") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'str' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(program, env, arg);
        if (arg_type.kind != TY_INT && arg_type.kind != TY_FLOAT && arg_type.kind != TY_BOOL) {
            ziman_error("type", arg->line, arg->col,
                        "'str' expects an int, float, or bool, found %s (it's already a string)",
                        type_name(arg_type));
        }
        return type_scalar(TY_STRING);
    }

    if (strcmp(name, "format") == 0) {
        if (e->as.call.args.count < 1) {
            ziman_error("type", e->line, e->col, "'format' requires at least a format string argument");
        }
        Expr *fmt_expr = e->as.call.args.items[0];
        if (fmt_expr->kind != EXPR_STRING) {
            ziman_error("type", fmt_expr->line, fmt_expr->col,
                        "'format' requires its first argument to be a string literal, not a computed value");
        }
        fmt_expr->resolved_type = type_scalar(TY_STRING);

        int placeholder_count = count_placeholders(fmt_expr);
        int provided = e->as.call.args.count - 1;
        if (placeholder_count != provided) {
            ziman_error("type", e->line, e->col,
                        "format string has %d placeholder(s) but %d argument(s) were provided",
                        placeholder_count, provided);
        }
        for (int i = 1; i < e->as.call.args.count; i++) {
            Expr *arg = e->as.call.args.items[i];
            check_expr(program, env, arg);
            require_not_void(arg, "as a format argument");
        }
        return type_scalar(TY_STRING);
    }

    if (strcmp(name, "len") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'len' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(program, env, arg);
        if (arg_type.kind != TY_ARRAY) {
            ziman_error("type", arg->line, arg->col, "'len' expects an array, found %s", type_name(arg_type));
        }
        return type_scalar(TY_INT);
    }

    if (strcmp(name, "push") == 0 || strcmp(name, "insert") == 0 || strcmp(name, "set") == 0) {
        int is_push = strcmp(name, "push") == 0;
        int expected_argc = is_push ? 2 : 3;
        if (e->as.call.args.count != expected_argc) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly %d argument(s), found %d", name, expected_argc, e->as.call.args.count);
        }

        Expr *arr_arg = e->as.call.args.items[0];
        Type arr_type = check_expr(program, env, arr_arg);
        if (arr_type.kind != TY_ARRAY) {
            ziman_error("type", arr_arg->line, arr_arg->col, "argument 1 of '%s' expects an array, found %s", name, type_name(arr_type));
        }

        int value_arg_index = is_push ? 1 : 2;
        if (!is_push) {
            Expr *idx_arg = e->as.call.args.items[1];
            Type idx_type = check_expr(program, env, idx_arg);
            if (idx_type.kind != TY_INT) {
                ziman_error("type", idx_arg->line, idx_arg->col, "argument 2 of '%s' expects int, found %s", name, type_name(idx_type));
            }
        }

        Expr *value_arg = e->as.call.args.items[value_arg_index];
        Type value_type = check_expr(program, env, value_arg);
        require_not_void(value_arg, "as an array element");
        if (!type_equals(value_type, *arr_type.elem)) {
            ziman_error("type", value_arg->line, value_arg->col,
                        "argument %d of '%s' expects %s (the array's element type), found %s",
                        value_arg_index + 1, name, type_name(*arr_type.elem), type_name(value_type));
        }
        return arr_type;
    }

    FunctionDecl *fn = find_function(program, name);
    if (fn == NULL) {
        ziman_error("type", e->line, e->col, "call to undefined function '%s'", name);
    }
    if (e->as.call.args.count != fn->params.count) {
        ziman_error("type", e->line, e->col, "function '%s' expects %d argument(s), found %d", name, fn->params.count, e->as.call.args.count);
    }
    for (int i = 0; i < e->as.call.args.count; i++) {
        Expr *arg = e->as.call.args.items[i];
        Type arg_type = check_expr(program, env, arg);
        require_not_void(arg, "as an argument");
        Type expected = fn->params.items[i].type;
        if (!type_equals(arg_type, expected)) {
            ziman_error("type", arg->line, arg->col,
                        "argument %d of '%s' expects %s, found %s",
                        i + 1, name, type_name(expected), type_name(arg_type));
        }
    }
    return fn->return_type;
}

static Type check_cast(Program *program, Env *env, Expr *e) {
    Type from = check_expr(program, env, e->as.cast.operand);
    require_not_void(e->as.cast.operand, "as a cast operand");
    Type to = e->as.cast.target_type;
    int numeric_from = from.kind == TY_INT || from.kind == TY_FLOAT;
    int numeric_to = to.kind == TY_INT || to.kind == TY_FLOAT;
    if (!(numeric_from && numeric_to)) {
        ziman_error("type", e->line, e->col, "cannot cast %s to %s (only int <-> float casts are supported)", type_name(from), type_name(to));
    }
    return to;
}

static Type check_array_literal(Program *program, Env *env, Expr *e) {
    ExprList *elements = &e->as.array_lit.elements;
    if (elements->count == 0) {
        ziman_error("type", e->line, e->col,
                    "cannot infer the element type of an empty array literal; add a type annotation (e.g. val x: [int] = [])");
    }
    Type elem_type = check_expr(program, env, elements->items[0]);
    require_not_void(elements->items[0], "as an array element");
    for (int i = 1; i < elements->count; i++) {
        Expr *el = elements->items[i];
        Type t = check_expr(program, env, el);
        if (!type_equals(t, elem_type)) {
            ziman_error("type", el->line, el->col,
                        "array elements must all have the same type: element 0 is %s, element %d is %s",
                        type_name(elem_type), i, type_name(t));
        }
    }
    return type_array(elem_type);
}

static Type check_index(Program *program, Env *env, Expr *e) {
    Type arr_type = check_expr(program, env, e->as.index.array);
    if (arr_type.kind != TY_ARRAY) {
        ziman_error("type", e->as.index.array->line, e->as.index.array->col, "cannot index into %s (not an array)", type_name(arr_type));
    }
    Type idx_type = check_expr(program, env, e->as.index.index);
    if (idx_type.kind != TY_INT) {
        ziman_error("type", e->as.index.index->line, e->as.index.index->col, "array index must be int, found %s", type_name(idx_type));
    }
    return *arr_type.elem;
}

static Type check_expr(Program *program, Env *env, Expr *e) {
    Type t;
    switch (e->kind) {
        case EXPR_INT: t = type_scalar(TY_INT); break;
        case EXPR_FLOAT: t = type_scalar(TY_FLOAT); break;
        case EXPR_STRING: t = type_scalar(TY_STRING); break;
        case EXPR_BOOL: t = type_scalar(TY_BOOL); break;
        case EXPR_IDENT: {
            Binding *b = env_lookup(env, e->as.ident_name);
            if (b == NULL) {
                ziman_error("type", e->line, e->col, "use of undefined variable '%s'", e->as.ident_name);
            }
            t = b->type;
            break;
        }
        case EXPR_UNARY: t = check_unary(program, env, e); break;
        case EXPR_BINARY: t = check_binary(program, env, e); break;
        case EXPR_CALL: t = check_call(program, env, e); break;
        case EXPR_CAST: t = check_cast(program, env, e); break;
        case EXPR_ARRAY_LIT: t = check_array_literal(program, env, e); break;
        case EXPR_INDEX: t = check_index(program, env, e); break;
        default:
            ziman_error("type", e->line, e->col, "internal error: unhandled expression kind");
            t = type_scalar(TY_VOID);
    }
    e->resolved_type = t;
    return t;
}

/* Returns true if executing `s` is statically guaranteed to hit a
 * `return` on every path -- used to require that non-void functions
 * can't fall off the end. Conservative: a `while` loop is never assumed
 * to return, since v0 does no loop-bound analysis. */
static int stmt_always_returns(Stmt *s) {
    switch (s->kind) {
        case STMT_RETURN:
            return 1;
        case STMT_BLOCK: {
            for (int i = 0; i < s->as.block.stmts.count; i++) {
                if (stmt_always_returns(s->as.block.stmts.items[i])) return 1;
            }
            return 0;
        }
        case STMT_IF:
            return s->as.if_stmt.else_block != NULL &&
                   stmt_always_returns(s->as.if_stmt.then_block) &&
                   stmt_always_returns(s->as.if_stmt.else_block);
        default:
            return 0;
    }
}

static void check_stmt(Program *program, Env *env, Stmt *s, Type fn_return_type);

static void check_block(Program *program, Env *parent, Stmt *block, Type fn_return_type) {
    Env *scope = env_new(parent);
    for (int i = 0; i < block->as.block.stmts.count; i++) {
        check_stmt(program, scope, block->as.block.stmts.items[i], fn_return_type);
    }
    env_free(scope);
}

static void check_stmt(Program *program, Env *env, Stmt *s, Type fn_return_type) {
    switch (s->kind) {
        case STMT_VAL:
        case STMT_VAR: {
            Expr *init = s->as.decl.init;
            Type declared;
            /* An empty array literal (`[]`) has no elements to infer a type
             * from -- it's only legal when a declared type gives its
             * element type explicitly, checked here rather than inside the
             * generic check_expr, which has no such context to work from. */
            if (init->kind == EXPR_ARRAY_LIT && init->as.array_lit.elements.count == 0) {
                if (!s->as.decl.has_declared_type) {
                    ziman_error("type", init->line, init->col,
                                "cannot infer the element type of an empty array literal; add a type annotation (e.g. val x: [int] = [])");
                }
                if (s->as.decl.declared_type.kind != TY_ARRAY) {
                    ziman_error("type", init->line, init->col,
                                "cannot initialize '%s' of declared type %s with an empty array literal",
                                s->as.decl.name, type_name(s->as.decl.declared_type));
                }
                init->resolved_type = s->as.decl.declared_type;
                declared = s->as.decl.declared_type;
            } else {
                Type init_type = check_expr(program, env, init);
                require_not_void(init, "to initialize a variable");
                declared = init_type;
                if (s->as.decl.has_declared_type) {
                    declared = s->as.decl.declared_type;
                    if (!type_equals(declared, init_type)) {
                        ziman_error("type", init->line, init->col,
                                    "cannot initialize '%s' of declared type %s with a value of type %s",
                                    s->as.decl.name, type_name(declared), type_name(init_type));
                    }
                }
            }
            int is_mutable = s->kind == STMT_VAR;
            if (!env_define(env, s->as.decl.name, declared, is_mutable, value_int(0))) {
                ziman_error("type", s->line, s->col, "'%s' is already declared in this scope", s->as.decl.name);
            }
            break;
        }
        case STMT_ASSIGN: {
            Binding *b = env_lookup(env, s->as.assign.name);
            if (b == NULL) {
                ziman_error("type", s->line, s->col, "assignment to undefined variable '%s'", s->as.assign.name);
            }
            if (!b->is_mutable) {
                ziman_error("type", s->line, s->col, "cannot assign to '%s' because it was declared with 'val' (use 'var' to make it mutable)", s->as.assign.name);
            }
            Type value_type = check_expr(program, env, s->as.assign.value);
            require_not_void(s->as.assign.value, "in an assignment");
            if (!type_equals(value_type, b->type)) {
                ziman_error("type", s->as.assign.value->line, s->as.assign.value->col,
                            "cannot assign a value of type %s to '%s' (declared as %s)",
                            type_name(value_type), s->as.assign.name, type_name(b->type));
            }
            break;
        }
        case STMT_IF: {
            Type cond_type = check_expr(program, env, s->as.if_stmt.cond);
            if (cond_type.kind != TY_BOOL) {
                ziman_error("type", s->as.if_stmt.cond->line, s->as.if_stmt.cond->col, "'if' condition must be bool, found %s", type_name(cond_type));
            }
            check_block(program, env, s->as.if_stmt.then_block, fn_return_type);
            if (s->as.if_stmt.else_block != NULL) {
                if (s->as.if_stmt.else_block->kind == STMT_IF) {
                    check_stmt(program, env, s->as.if_stmt.else_block, fn_return_type);
                } else {
                    check_block(program, env, s->as.if_stmt.else_block, fn_return_type);
                }
            }
            break;
        }
        case STMT_WHILE: {
            Type cond_type = check_expr(program, env, s->as.while_stmt.cond);
            if (cond_type.kind != TY_BOOL) {
                ziman_error("type", s->as.while_stmt.cond->line, s->as.while_stmt.cond->col, "'while' condition must be bool, found %s", type_name(cond_type));
            }
            check_block(program, env, s->as.while_stmt.body, fn_return_type);
            break;
        }
        case STMT_RETURN: {
            if (s->as.ret.value == NULL) {
                if (fn_return_type.kind != TY_VOID) {
                    ziman_error("type", s->line, s->col, "bare 'return' in a function declared to return %s", type_name(fn_return_type));
                }
            } else {
                Type value_type = check_expr(program, env, s->as.ret.value);
                if (fn_return_type.kind == TY_VOID) {
                    ziman_error("type", s->as.ret.value->line, s->as.ret.value->col, "cannot return a value from a function declared with no return type");
                }
                if (!type_equals(value_type, fn_return_type)) {
                    ziman_error("type", s->as.ret.value->line, s->as.ret.value->col,
                                "returned %s but function is declared to return %s", type_name(value_type), type_name(fn_return_type));
                }
            }
            break;
        }
        case STMT_EXPR: {
            check_expr(program, env, s->as.expr_stmt.expr);
            break;
        }
        case STMT_BLOCK:
            check_block(program, env, s, fn_return_type);
            break;
    }
}

static void check_function(Program *program, FunctionDecl *fn) {
    Env *env = env_new(NULL);
    for (int i = 0; i < fn->params.count; i++) {
        Param *param = &fn->params.items[i];
        if (!env_define(env, param->name, param->type, 0 /* params are immutable */, value_int(0))) {
            ziman_error("type", param->line, param->col, "duplicate parameter name '%s'", param->name);
        }
    }
    for (int i = 0; i < fn->body->as.block.stmts.count; i++) {
        check_stmt(program, env, fn->body->as.block.stmts.items[i], fn->return_type);
    }
    if (fn->return_type.kind != TY_VOID && !stmt_always_returns(fn->body)) {
        ziman_error("type", fn->line, fn->col, "function '%s' does not return a value on every path", fn->name);
    }
    env_free(env);
}

void typecheck(Program *program) {
    static const char *builtins[] = {"print", "concat", "str", "format", "len", "push", "insert", "set"};

    for (int i = 0; i < program->functions.count; i++) {
        FunctionDecl *fn = program->functions.items[i];
        for (size_t b = 0; b < sizeof(builtins) / sizeof(builtins[0]); b++) {
            if (strcmp(fn->name, builtins[b]) == 0) {
                ziman_error("type", fn->line, fn->col, "'%s' is a builtin function and cannot be redefined", fn->name);
            }
        }
        for (int j = i + 1; j < program->functions.count; j++) {
            if (strcmp(fn->name, program->functions.items[j]->name) == 0) {
                ziman_error("type", program->functions.items[j]->line, program->functions.items[j]->col,
                            "function '%s' is already defined", fn->name);
            }
        }
    }

    FunctionDecl *main_fn = find_function(program, "main");
    if (main_fn == NULL) {
        ziman_error("type", 0, 0, "no 'main' function found");
    }
    if (main_fn->params.count != 0 || main_fn->return_type.kind != TY_VOID) {
        ziman_error("type", main_fn->line, main_fn->col, "'main' must take no parameters and return nothing");
    }

    for (int i = 0; i < program->functions.count; i++) {
        check_function(program, program->functions.items[i]);
    }
}
