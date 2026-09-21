#include "interpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "error.h"
#include "value.h"

typedef enum { CTRL_NONE, CTRL_RETURN } CtrlKind;

typedef struct {
    CtrlKind kind;
    Value value;
} ExecResult;

static FunctionDecl *find_function(Program *program, const char *name) {
    for (int i = 0; i < program->functions.count; i++) {
        if (strcmp(program->functions.items[i]->name, name) == 0) return program->functions.items[i];
    }
    return NULL;
}

static StructDecl *find_struct(Program *program, const char *name) {
    for (int i = 0; i < program->structs.count; i++) {
        if (strcmp(program->structs.items[i]->name, name) == 0) return program->structs.items[i];
    }
    return NULL;
}

static Value eval_expr(Program *program, Env *env, Expr *e);
static ExecResult exec_stmt(Program *program, Env *env, Stmt *s);

static Value call_function(Program *program, FunctionDecl *fn, Value *args, int argc) {
    Env *env = env_new(NULL);
    for (int i = 0; i < argc; i++) {
        env_define(env, fn->params.items[i].name, fn->params.items[i].type, 0, args[i]);
    }
    ExecResult result = {CTRL_NONE, value_int(0)};
    for (int i = 0; i < fn->body->as.block.stmts.count && result.kind == CTRL_NONE; i++) {
        result = exec_stmt(program, env, fn->body->as.block.stmts.items[i]);
    }
    env_free(env);
    return result.kind == CTRL_RETURN ? result.value : value_int(0);
}

static Value call_print(Value arg) {
    char *s = value_display(arg);
    printf("%s\n", s);
    free(s);
    return value_int(0);
}

static Value call_concat(Value a, Value b) {
    size_t la = strlen(a.as.s), lb = strlen(b.as.s);
    char *buf = malloc(la + lb + 1);
    memcpy(buf, a.as.s, la);
    memcpy(buf + la, b.as.s, lb);
    buf[la + lb] = '\0';
    return value_string(buf);
}

static Value call_str(Value arg) {
    return value_string(value_display(arg));
}

static Value call_len(Value arr) {
    return value_int(arr.as.arr->count);
}

/* push/insert/set never mutate their `arr` argument -- they always build a
 * brand new ArrayObj and return a Value wrapping it, per docs/DESIGN.md's
 * value-semantics decision for arrays. Sharing `old->items[k]` Values by
 * plain struct copy into the new buffer is safe precisely because ArrayObj
 * is immutable once built (see value.h) -- there's nothing to alias into. */
static Value call_push(Type array_type, Value arr, Value x) {
    ArrayObj *old = arr.as.arr;
    int new_count = old->count + 1;
    Value *items = malloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->items, (size_t)old->count * sizeof(Value));
    items[old->count] = x;
    return value_array(array_type, array_obj_new(items, new_count));
}

static Value call_insert(Type array_type, Value arr, Value idx, Value x, int line, int col) {
    ArrayObj *old = arr.as.arr;
    int64_t i = idx.as.i;
    if (i < 0 || i > old->count) {
        ziman_error("runtime", line, col, "insert index %lld out of bounds (array length %d)", (long long)i, old->count);
    }
    int new_count = old->count + 1;
    Value *items = malloc((size_t)new_count * sizeof(Value));
    memcpy(items, old->items, (size_t)i * sizeof(Value));
    items[i] = x;
    memcpy(items + i + 1, old->items + i, (size_t)(old->count - (int)i) * sizeof(Value));
    return value_array(array_type, array_obj_new(items, new_count));
}

static Value call_set(Type array_type, Value arr, Value idx, Value x, int line, int col) {
    ArrayObj *old = arr.as.arr;
    int64_t i = idx.as.i;
    if (i < 0 || i >= old->count) {
        ziman_error("runtime", line, col, "set index %lld out of bounds (array length %d)", (long long)i, old->count);
    }
    Value *items = malloc((size_t)old->count * sizeof(Value));
    memcpy(items, old->items, (size_t)old->count * sizeof(Value));
    items[i] = x;
    return value_array(array_type, array_obj_new(items, old->count));
}

/* args[0] is the format string, args[1..argc-1] are the interpolation
 * values. The type checker already verified `{}` placeholder count
 * matches argc-1 and that the format string is well-formed, so this
 * doesn't re-validate -- it trusts that pass. */
static Value call_format(Value *args, int argc) {
    const char *fmt = args[0].as.s;
    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    int arg_i = 1;

    for (const char *p = fmt; *p; p++) {
        if (p[0] == '{' && p[1] == '}') {
            char *piece = value_display(args[arg_i++]);
            size_t piece_len = strlen(piece);
            while (len + piece_len + 1 > cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            memcpy(buf + len, piece, piece_len);
            len += piece_len;
            free(piece);
            p++; /* skip the matching '}' */
        } else {
            if (len + 2 > cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            buf[len++] = *p;
        }
    }
    buf[len] = '\0';
    (void)argc;
    return value_string(buf);
}

static Value eval_binary(Program *program, Env *env, Expr *e) {
    Value l = eval_expr(program, env, e->as.binary.left);

    /* Short-circuit && / || before evaluating the right side. */
    if (e->as.binary.op == OP_AND) {
        if (!l.as.b) return value_bool(0);
        return eval_expr(program, env, e->as.binary.right);
    }
    if (e->as.binary.op == OP_OR) {
        if (l.as.b) return value_bool(1);
        return eval_expr(program, env, e->as.binary.right);
    }

    Value r = eval_expr(program, env, e->as.binary.right);

    switch (e->as.binary.op) {
        case OP_ADD:
            return l.type.kind == TY_INT ? value_int(l.as.i + r.as.i) : value_float(l.as.f + r.as.f);
        case OP_SUB:
            return l.type.kind == TY_INT ? value_int(l.as.i - r.as.i) : value_float(l.as.f - r.as.f);
        case OP_MUL:
            return l.type.kind == TY_INT ? value_int(l.as.i * r.as.i) : value_float(l.as.f * r.as.f);
        case OP_DIV:
            if (l.type.kind == TY_INT) {
                if (r.as.i == 0) ziman_error("runtime", e->line, e->col, "division by zero");
                return value_int(l.as.i / r.as.i);
            }
            if (r.as.f == 0.0) ziman_error("runtime", e->line, e->col, "division by zero");
            return value_float(l.as.f / r.as.f);
        case OP_MOD:
            if (l.type.kind == TY_INT) {
                if (r.as.i == 0) ziman_error("runtime", e->line, e->col, "division by zero");
                return value_int(l.as.i % r.as.i);
            }
            ziman_error("runtime", e->line, e->col, "internal error: '%%' on float should have been rejected by the type checker");
            return value_int(0);
        case OP_EQ: return value_bool(value_equals(l, r));
        case OP_NEQ: return value_bool(!value_equals(l, r));
        case OP_LT: return value_bool(l.type.kind == TY_INT ? l.as.i < r.as.i : l.as.f < r.as.f);
        case OP_GT: return value_bool(l.type.kind == TY_INT ? l.as.i > r.as.i : l.as.f > r.as.f);
        case OP_LE: return value_bool(l.type.kind == TY_INT ? l.as.i <= r.as.i : l.as.f <= r.as.f);
        case OP_GE: return value_bool(l.type.kind == TY_INT ? l.as.i >= r.as.i : l.as.f >= r.as.f);
        default:
            ziman_error("runtime", e->line, e->col, "internal error: unhandled binary operator");
            return value_int(0);
    }
}

static Value eval_expr(Program *program, Env *env, Expr *e) {
    switch (e->kind) {
        case EXPR_INT: return value_int(e->as.int_val);
        case EXPR_FLOAT: return value_float(e->as.float_val);
        case EXPR_STRING: return value_string(e->as.string_val);
        case EXPR_BOOL: return value_bool(e->as.bool_val);
        case EXPR_IDENT: {
            Binding *b = env_lookup(env, e->as.ident_name);
            return b->value;
        }
        case EXPR_UNARY: {
            Value v = eval_expr(program, env, e->as.unary.operand);
            if (e->as.unary.op == OP_NEG) {
                return v.type.kind == TY_INT ? value_int(-v.as.i) : value_float(-v.as.f);
            }
            return value_bool(!v.as.b);
        }
        case EXPR_BINARY:
            return eval_binary(program, env, e);
        case EXPR_CALL: {
            int argc = e->as.call.args.count;
            Value *args = argc > 0 ? malloc((size_t)argc * sizeof(Value)) : NULL;
            for (int i = 0; i < argc; i++) {
                args[i] = eval_expr(program, env, e->as.call.args.items[i]);
            }
            Value result;
            if (strcmp(e->as.call.callee, "print") == 0) {
                result = call_print(args[0]);
            } else if (strcmp(e->as.call.callee, "concat") == 0) {
                result = call_concat(args[0], args[1]);
            } else if (strcmp(e->as.call.callee, "str") == 0) {
                result = call_str(args[0]);
            } else if (strcmp(e->as.call.callee, "format") == 0) {
                result = call_format(args, argc);
            } else if (strcmp(e->as.call.callee, "len") == 0) {
                result = call_len(args[0]);
            } else if (strcmp(e->as.call.callee, "push") == 0) {
                result = call_push(e->resolved_type, args[0], args[1]);
            } else if (strcmp(e->as.call.callee, "insert") == 0) {
                result = call_insert(e->resolved_type, args[0], args[1], args[2], e->line, e->col);
            } else if (strcmp(e->as.call.callee, "set") == 0) {
                result = call_set(e->resolved_type, args[0], args[1], args[2], e->line, e->col);
            } else {
                FunctionDecl *fn = find_function(program, e->as.call.callee);
                result = call_function(program, fn, args, argc);
            }
            free(args);
            return result;
        }
        case EXPR_CAST: {
            Value v = eval_expr(program, env, e->as.cast.operand);
            if (e->as.cast.target_type.kind == TY_FLOAT) {
                return value_float(v.type.kind == TY_INT ? (double)v.as.i : v.as.f);
            }
            /* target is TY_INT */
            return value_int(v.type.kind == TY_FLOAT ? (int64_t)v.as.f : v.as.i);
        }
        case EXPR_ARRAY_LIT: {
            int count = e->as.array_lit.elements.count;
            Value *items = count > 0 ? malloc((size_t)count * sizeof(Value)) : NULL;
            for (int i = 0; i < count; i++) {
                items[i] = eval_expr(program, env, e->as.array_lit.elements.items[i]);
            }
            return value_array(e->resolved_type, array_obj_new(items, count));
        }
        case EXPR_INDEX: {
            Value arr = eval_expr(program, env, e->as.index.array);
            Value idx = eval_expr(program, env, e->as.index.index);
            int64_t i = idx.as.i;
            if (i < 0 || i >= arr.as.arr->count) {
                ziman_error("runtime", e->line, e->col, "index %lld out of bounds (array length %d)", (long long)i, arr.as.arr->count);
            }
            return arr.as.arr->items[i];
        }
        case EXPR_STRUCT_LIT: {
            StructDecl *decl = find_struct(program, e->as.struct_lit.type_name);
            int count = decl->fields.count;
            char **names = malloc((size_t)count * sizeof(char *));
            Value *fields = malloc((size_t)count * sizeof(Value));
            for (int i = 0; i < count; i++) {
                names[i] = decl->fields.items[i].name; /* borrowed from the StructDecl, never freed */
                Expr *value_expr = NULL;
                for (int j = 0; j < e->as.struct_lit.fields.count; j++) {
                    if (strcmp(e->as.struct_lit.fields.items[j].name, names[i]) == 0) {
                        value_expr = e->as.struct_lit.fields.items[j].value;
                        break;
                    }
                }
                fields[i] = eval_expr(program, env, value_expr);
            }
            return value_struct(e->resolved_type, struct_obj_new(names, fields, count));
        }
        case EXPR_FIELD_ACCESS: {
            Value base = eval_expr(program, env, e->as.field_access.base);
            StructObj *st = base.as.st;
            for (int i = 0; i < st->count; i++) {
                if (strcmp(st->field_names[i], e->as.field_access.field_name) == 0) {
                    return st->fields[i];
                }
            }
            ziman_error("runtime", e->line, e->col, "internal error: field '%s' not found at runtime", e->as.field_access.field_name);
            return value_int(0);
        }
        case EXPR_WITH: {
            Value base = eval_expr(program, env, e->as.with_expr.base);
            StructObj *old = base.as.st;
            int count = old->count;
            Value *fields = malloc((size_t)count * sizeof(Value));
            memcpy(fields, old->fields, (size_t)count * sizeof(Value));
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) {
                FieldInit *fi = &e->as.with_expr.overrides.items[i];
                for (int j = 0; j < count; j++) {
                    if (strcmp(old->field_names[j], fi->name) == 0) {
                        fields[j] = eval_expr(program, env, fi->value);
                        break;
                    }
                }
            }
            /* Reuses old->field_names directly (same struct type, same
             * field order) -- never mutates `old`, so sharing it is safe. */
            return value_struct(e->resolved_type, struct_obj_new(old->field_names, fields, count));
        }
    }
    ziman_error("runtime", e->line, e->col, "internal error: unhandled expression kind");
    return value_int(0);
}

static ExecResult no_ctrl(void) {
    ExecResult r = {CTRL_NONE, value_int(0)};
    return r;
}

static ExecResult exec_block(Program *program, Env *parent, Stmt *block) {
    Env *scope = env_new(parent);
    ExecResult result = no_ctrl();
    for (int i = 0; i < block->as.block.stmts.count && result.kind == CTRL_NONE; i++) {
        result = exec_stmt(program, scope, block->as.block.stmts.items[i]);
    }
    env_free(scope);
    return result;
}

static ExecResult exec_stmt(Program *program, Env *env, Stmt *s) {
    switch (s->kind) {
        case STMT_VAL:
        case STMT_VAR: {
            Value v = eval_expr(program, env, s->as.decl.init);
            Type t = s->as.decl.has_declared_type ? s->as.decl.declared_type : v.type;
            env_define(env, s->as.decl.name, t, s->kind == STMT_VAR, v);
            return no_ctrl();
        }
        case STMT_ASSIGN: {
            Binding *b = env_lookup(env, s->as.assign.name);
            b->value = eval_expr(program, env, s->as.assign.value);
            return no_ctrl();
        }
        case STMT_IF: {
            Value cond = eval_expr(program, env, s->as.if_stmt.cond);
            if (cond.as.b) {
                return exec_block(program, env, s->as.if_stmt.then_block);
            }
            if (s->as.if_stmt.else_block != NULL) {
                if (s->as.if_stmt.else_block->kind == STMT_IF) {
                    return exec_stmt(program, env, s->as.if_stmt.else_block);
                }
                return exec_block(program, env, s->as.if_stmt.else_block);
            }
            return no_ctrl();
        }
        case STMT_WHILE: {
            for (;;) {
                Value cond = eval_expr(program, env, s->as.while_stmt.cond);
                if (!cond.as.b) break;
                ExecResult result = exec_block(program, env, s->as.while_stmt.body);
                if (result.kind == CTRL_RETURN) return result;
            }
            return no_ctrl();
        }
        case STMT_RETURN: {
            ExecResult r;
            r.kind = CTRL_RETURN;
            r.value = s->as.ret.value != NULL ? eval_expr(program, env, s->as.ret.value) : value_int(0);
            return r;
        }
        case STMT_EXPR: {
            eval_expr(program, env, s->as.expr_stmt.expr);
            return no_ctrl();
        }
        case STMT_BLOCK:
            return exec_block(program, env, s);
    }
    return no_ctrl();
}

void interpret(Program *program) {
    FunctionDecl *main_fn = find_function(program, "main");
    call_function(program, main_fn, NULL, 0);
}
