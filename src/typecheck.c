#define _POSIX_C_SOURCE 200809L
#include "typecheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "derive.h"
#include "error.h"

static FunctionDecl *find_function(Module *module, const char *name) {
    for (int i = 0; i < module->functions.count; i++) {
        if (strcmp(module->functions.items[i]->name, name) == 0) return module->functions.items[i];
    }
    return NULL;
}

static StructDecl *find_struct(Module *module, const char *name) {
    for (int i = 0; i < module->structs.count; i++) {
        if (strcmp(module->structs.items[i]->name, name) == 0) return module->structs.items[i];
    }
    return NULL;
}

static EnumDecl *find_enum(Module *module, const char *name) {
    for (int i = 0; i < module->enums.count; i++) {
        if (strcmp(module->enums.items[i]->name, name) == 0) return module->enums.items[i];
    }
    return NULL;
}

/* The root of every function's own scope chain (see check_function) --
 * holds every module-level `val` (see check_global_vals), so a bare
 * reference to one falls out of an ordinary env_lookup exactly the way a
 * reference to an enclosing block's local already does, with no special-
 * casing anywhere else in this file. Built once, before any function
 * body is checked; never mutated afterward. */
static Env *g_global_env = NULL;

static Type check_expr(Module *module, Env *env, Expr *e);
static void check_type_well_formed(Module *module, Type t, int line, int col);

/* Rebuilds `t` with every TY_TYPEVAR named in `params` replaced by the
 * type argument at the same index in `args` -- the substitution-on-demand
 * a generic struct's own declared field types need whenever they're
 * looked at through a concrete instantiation (`Box<int>`'s `value` field
 * is `int`, not `T`). No caching/memoization: `Value` is already
 * runtime-type-erased (see ast.h's Type comment), so this is purely a
 * type-checking-time convenience, never a runtime cost. A type unrelated
 * to `params`/`args` (e.g. a plain `int` field, or a reference to some
 * other struct's own type parameter) passes through unchanged. */
static Type substitute_type(Type t, TypeParamList *params, TypeList *args) {
    switch (t.kind) {
        case TY_TYPEVAR:
            for (int i = 0; i < params->count; i++) {
                if (strcmp(params->items[i].name, t.type_var_name) == 0) return args->items[i];
            }
            return t;
        case TY_ARRAY:
            return type_array(substitute_type(*t.elem, params, args));
        case TY_RESULT:
            return type_result(substitute_type(*t.elem, params, args), substitute_type(*t.elem_err, params, args));
        case TY_CHANNEL:
            return type_channel(substitute_type(*t.elem, params, args));
        case TY_FUNC: {
            TypeList substituted = {0};
            for (int i = 0; i < t.type_args.count; i++) type_list_push(&substituted, substitute_type(t.type_args.items[i], params, args));
            return type_func(substituted, substitute_type(*t.elem, params, args));
        }
        case TY_STRUCT:
        case TY_ENUM: {
            if (t.type_args.count == 0) return t;
            Type result = t;
            result.type_args = (TypeList){0};
            for (int i = 0; i < t.type_args.count; i++) {
                type_list_push(&result.type_args, substitute_type(t.type_args.items[i], params, args));
            }
            return result;
        }
        default:
            return t;
    }
}

static int is_scalar_kind(TypeKind kind) {
    return kind == TY_INT || kind == TY_FLOAT || kind == TY_BOOL || kind == TY_STRING || kind == TY_BIGINT || kind == TY_DECIMAL || kind == TY_BYTE;
}

/* Whether '=='/'!=' is legal for a value of type `t`: scalars always,
 * `[T]` only when T is itself scalar (an array's own equality, not
 * generalized to "any comparable T" -- see docs/SYNTAX.md "Arrays"),
 * a struct only when every one of its fields is (recursively) comparable,
 * a `Result<T, E>` only when both `T` and `E` are (recursively)
 * comparable -- a runtime value only ever holds one of the two, but which
 * one isn't known until the value exists, so both branches have to type-check
 * as comparable for the type itself to be -- and a user-declared `enum`
 * only when every variant that carries a payload has a (recursively)
 * comparable payload type, same reasoning as Result. `depth` guards the still-open
 * "self-referential struct can't be constructed" gap (docs/ROADMAP.md) -- a
 * directly or mutually self-referential struct declaration would otherwise
 * send this into unbounded recursion just from its *shape*, even though no
 * value of that shape can ever actually exist yet. */
/* The type parameters of whichever generic declaration is currently being
 * validated or type-checked -- a struct's own fields, an enum's own
 * variant payloads, a function's own params/return type, or (separately)
 * a function's own body. Needed in two different situations: (1) a
 * bounded type parameter used *as a type argument to another bounded
 * generic type* inside the enclosing declaration's own signature (e.g.
 * `fn map_get<K: Eq, V>(m: Map<K, V>, ...)` -- checking `Map<K, V>`'s own
 * `K: Eq` bound against the *caller* `K`, which is itself just a bare,
 * unsubstituted type variable at this point, requires knowing it's
 * `map_get`'s own bound `K`, not some unrelated, unbounded one); and
 * (2) `==` on a bare `T` inside a generic function's own body (see
 * check_function's comment). Set/cleared symmetrically around each
 * declaration's own validation in typecheck() and around check_function's
 * body-checking -- a plain variable rather than a parameter threaded
 * through every mutually-recursive check_* function, sound because the
 * type checker validates exactly one declaration at a time, single-
 * threaded, with no nested declarations to re-enter this during. NULL
 * anywhere else, where a bare type variable is never comparable. */
static TypeParamList *current_decl_type_params = NULL;

static int type_supports_equality(Module *module, Type t, int depth) {
    if (depth > 64) return 0;
    switch (t.kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING: case TY_BIGINT: case TY_DECIMAL: case TY_BYTE:
            return 1;
        case TY_ARRAY:
            return is_scalar_kind(t.elem->kind);
        case TY_STRUCT: {
            StructDecl *decl = find_struct(module, t.struct_name);
            if (decl == NULL) return 0;
            for (int i = 0; i < decl->fields.count; i++) {
                Type field_type = substitute_type(decl->fields.items[i].type, &decl->type_params, &t.type_args);
                if (!type_supports_equality(module, field_type, depth + 1)) return 0;
            }
            return 1;
        }
        case TY_RESULT:
            return type_supports_equality(module, *t.elem, depth + 1) &&
                   type_supports_equality(module, *t.elem_err, depth + 1);
        case TY_ENUM: {
            EnumDecl *decl = find_enum(module, t.enum_name);
            if (decl == NULL) return 0;
            for (int i = 0; i < decl->variants.count; i++) {
                EnumVariant *v = &decl->variants.items[i];
                if (!v->has_payload) continue;
                Type payload_type = substitute_type(v->payload_type, &decl->type_params, &t.type_args);
                if (!type_supports_equality(module, payload_type, depth + 1)) return 0;
            }
            return 1;
        }
        case TY_CHANNEL:
            /* A channel is a stateful, identity-based handle, not a data
             * aggregate -- "are two channels equal" doesn't have the
             * obvious structural meaning it does for an array/struct/enum,
             * so it's just not comparable at all in v0. */
            return 0;
        case TY_TYPEVAR:
            /* A bare, unsubstituted type variable is comparable exactly
             * when it's the *current declaration's own* type parameter
             * and that parameter's bound bitmask includes `Eq` -- e.g.
             * `a == b` inside `fn are_equal<T: Eq>(a: T, b: T) -> bool
             * { ... }`. Anywhere else (a struct/enum's own declaration,
             * or a type variable belonging to some other, unrelated
             * declaration) this is unbounded and correctly not
             * comparable. */
            if (current_decl_type_params != NULL) {
                for (int i = 0; i < current_decl_type_params->count; i++) {
                    if (strcmp(current_decl_type_params->items[i].name, t.type_var_name) == 0) {
                        return (current_decl_type_params->items[i].bound & BOUND_EQ) != 0;
                    }
                }
            }
            return 0;
        case TY_VOID:
        default:
            return 0;
    }
}

/* Whether `t` supports the `hash` builtin, and so the `Hash` bound: exactly the
 * types that support `==` (see type_supports_equality) whose leaves are
 * hashable -- the scalars, arrays of scalars, structs and enums whose
 * fields/payloads are hashable, Option and Result (an enum and a builtin)
 * of hashable types, and a type parameter bounded by `Hash`. The hash is
 * structural, the same shape as `==`, so `a == b` implies `hash(a) ==
 * hash(b)` with no user function needed (unlike an order, which is not
 * structural and needs `<type>_compare`). Channels, and anything `==` rejects,
 * are not hashable. Needed for `HashMap<K, V>`'s bucket index -- see the
 * `collections` module. */
static int type_supports_hashing(Module *module, Type t, int depth) {
    if (depth > 64) return 0;
    switch (t.kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING: case TY_BIGINT: case TY_DECIMAL: case TY_BYTE:
            return 1;
        case TY_ARRAY:
            return is_scalar_kind(t.elem->kind);
        case TY_STRUCT: {
            StructDecl *decl = find_struct(module, t.struct_name);
            if (decl == NULL) return 0;
            for (int i = 0; i < decl->fields.count; i++) {
                Type field_type = substitute_type(decl->fields.items[i].type, &decl->type_params, &t.type_args);
                if (!type_supports_hashing(module, field_type, depth + 1)) return 0;
            }
            return 1;
        }
        case TY_RESULT:
            return type_supports_hashing(module, *t.elem, depth + 1) && type_supports_hashing(module, *t.elem_err, depth + 1);
        case TY_ENUM: {
            EnumDecl *decl = find_enum(module, t.enum_name);
            if (decl == NULL) return 0;
            for (int i = 0; i < decl->variants.count; i++) {
                EnumVariant *v = &decl->variants.items[i];
                if (!v->has_payload) continue;
                Type payload_type = substitute_type(v->payload_type, &decl->type_params, &t.type_args);
                if (!type_supports_hashing(module, payload_type, depth + 1)) return 0;
            }
            return 1;
        }
        case TY_TYPEVAR:
            if (current_decl_type_params != NULL) {
                for (int i = 0; i < current_decl_type_params->count; i++) {
                    if (strcmp(current_decl_type_params->items[i].name, t.type_var_name) == 0) {
                        return (current_decl_type_params->items[i].bound & BOUND_HASH) != 0;
                    }
                }
            }
            return 0;
        default:
            return 0;
    }
}

/* The `<snake_type>_compare` function that gives a struct or enum its order
 * (written by hand, or generated by #[derive(Ord)]), or NULL. A type declared
 * in module "path::Name" looks for "path::name_compare" -- the same flat,
 * qualified naming every other module-level function has. */
static FunctionDecl *find_compare_function(Module *module, Type t) {
    const char *type_name_str = t.kind == TY_STRUCT ? t.struct_name : t.enum_name;
    if (type_name_str == NULL) return NULL;
    char *fn_name = ziman_compare_function_name(type_name_str);
    FunctionDecl *fn = find_function(module, fn_name);
    if (fn == NULL) fn = find_function(module, ziman_compare_function_name_flat(type_name_str));
    if (fn == NULL || fn->params.count != 2 || fn->return_type.kind != TY_INT) return NULL;
    if (!type_equals(fn->params.items[0].type, t) || !type_equals(fn->params.items[1].type, t)) return NULL;
    return fn;
}

/* Whether `t` has a total order: the scalars that order naturally (int, float,
 * bigint, decimal, byte, string -- bytewise), a struct or enum with a
 * `<type>_compare(a, b) -> int` function, a type parameter bounded by `Ord`,
 * and -- recursively -- an array, an Option or a Result whose element/payload
 * types are ordered. Channels and bool don't. */
static int type_supports_ordering(Module *module, Type t) {
    switch (t.kind) {
        case TY_INT: case TY_FLOAT: case TY_STRING: case TY_BIGINT: case TY_DECIMAL: case TY_BYTE:
            return 1;
        case TY_ARRAY:
            return type_supports_ordering(module, *t.elem);
        case TY_RESULT:
            return type_supports_ordering(module, *t.elem) && type_supports_ordering(module, *t.elem_err);
        case TY_ENUM:
            if (t.enum_name != NULL && strcmp(t.enum_name, "Option") == 0 && t.type_args.count == 1) {
                return type_supports_ordering(module, t.type_args.items[0]);
            }
            return find_compare_function(module, t) != NULL;
        case TY_STRUCT:
            return find_compare_function(module, t) != NULL;
        case TY_TYPEVAR:
            if (current_decl_type_params != NULL) {
                for (int i = 0; i < current_decl_type_params->count; i++) {
                    if (strcmp(current_decl_type_params->items[i].name, t.type_var_name) == 0) {
                        return (current_decl_type_params->items[i].bound & BOUND_ORD) != 0;
                    }
                }
            }
            return 0;
        default:
            return 0;
    }
}

/* Enforces every bound a generic declaration's own type parameters carry
 * (`Eq`/`Hash`, independently) against the concrete type arguments an
 * instantiation actually supplies -- e.g. `Box<Channel<int>>` is
 * rejected here if `Box` were declared `struct Box<T: Eq>`, since
 * `Channel<int>` doesn't support `==`. This is what keeps "a bounded T
 * is always comparable/hashable" true in practice, so a generic
 * declaration's own body can rely on it (see ROADMAP.md's "Generics"
 * Stage 5). Assumes `params->count == args->count`, already checked by
 * the caller. */
static void check_type_args_satisfy_bounds(Module *module, TypeParamList *params, TypeList *args, int line, int col) {
    for (int i = 0; i < params->count; i++) {
        int bound = params->items[i].bound;
        if ((bound & BOUND_EQ) && !type_supports_equality(module, args->items[i], 0)) {
            ziman_error("type", line, col,
                        "type argument %s does not satisfy bound 'Eq' required by type parameter '%s' (it must support '==')",
                        type_name(args->items[i]), params->items[i].name);
        }
        if ((bound & BOUND_ORD) && !type_supports_ordering(module, args->items[i])) {
            ziman_error("type", line, col,
                        "type argument %s does not satisfy bound 'Ord' required by type parameter '%s' (it must be a number, string or byte, "
                        "or a struct/enum with a '<type>_compare(a, b) -> int' function -- '#[derive(Ord)]' writes one)",
                        type_name(args->items[i]), params->items[i].name);
        }
        if ((bound & BOUND_HASH) && !type_supports_hashing(module, args->items[i], 0)) {
            ziman_error("type", line, col,
                        "type argument %s does not satisfy bound 'Hash' required by type parameter '%s' (it must support '==' and have only hashable parts)",
                        type_name(args->items[i]), params->items[i].name);
        }
    }
}

static void require_not_void(Expr *e, const char *context) {
    if (e->resolved_type.kind == TY_VOID) {
        ziman_error("type", e->line, e->col, "cannot use a void value %s", context);
    }
}

static Type check_binary(Module *module, Env *env, Expr *e) {
    Type lt = check_expr(module, env, e->as.binary.left);
    Type rt = check_expr(module, env, e->as.binary.right);
    require_not_void(e->as.binary.left, "as an operand");
    require_not_void(e->as.binary.right, "as an operand");

    switch (e->as.binary.op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
            if (e->as.binary.op == OP_ADD && (lt.kind == TY_STRING || rt.kind == TY_STRING)) {
                ziman_error("type", e->line, e->col,
                            "'+' doesn't join strings (found %s and %s) -- use concat(a, b), or format(\"{}{}\", a, b) for several pieces; convert a number with str(n)",
                            type_name(lt), type_name(rt));
            }
            if (e->as.binary.op == OP_ADD && (lt.kind == TY_ARRAY || rt.kind == TY_ARRAY)) {
                ziman_error("type", e->line, e->col,
                            "'+' doesn't join arrays (found %s and %s) -- use push(arr, item) to add one element or concat_arrays<T>(a, b) to join two",
                            type_name(lt), type_name(rt));
            }
            if (lt.kind != rt.kind || (lt.kind != TY_INT && lt.kind != TY_FLOAT && lt.kind != TY_BIGINT && lt.kind != TY_DECIMAL)) {
                ziman_error("type", e->line, e->col,
                            "arithmetic operator requires two operands of the same numeric type, found %s and %s (use 'as' to cast explicitly)",
                            type_name(lt), type_name(rt));
            }
            return lt;
        case OP_LT:
        case OP_GT:
        case OP_LE:
        case OP_GE:
            if (lt.kind != rt.kind || (lt.kind != TY_INT && lt.kind != TY_FLOAT && lt.kind != TY_BIGINT && lt.kind != TY_DECIMAL &&
                                       lt.kind != TY_STRING && lt.kind != TY_BYTE)) {
                ziman_error("type", e->line, e->col,
                            "'<' '>' '<=' '>=' need two operands of the same number, string or byte type, found %s and %s "
                            "(for a struct or enum use compare(a, b), which needs a '<type>_compare' function or '#[derive(Ord)]')",
                            type_name(lt), type_name(rt));
            }
            return type_scalar(TY_BOOL);
        case OP_EQ:
        case OP_NEQ:
            if (!type_equals(lt, rt)) {
                ziman_error("type", e->line, e->col, "cannot compare %s and %s for equality", type_name(lt), type_name(rt));
            }
            if (!type_supports_equality(module, lt, 0)) {
                if (lt.kind == TY_RESULT) {
                    ziman_error("type", e->line, e->col,
                                "%s cannot be compared with '=='/'!=' because its Ok or Err type is not a comparable type",
                                type_name(lt));
                } else if (lt.kind == TY_ARRAY) {
                    ziman_error("type", e->line, e->col,
                                "array '=='/'!=' requires an array of a comparable (scalar) type, found %s", type_name(lt));
                } else if (lt.kind == TY_ENUM) {
                    ziman_error("type", e->line, e->col,
                                "enum '%s' cannot be compared with '=='/'!=' because one of its variants' payload is not a comparable type",
                                type_name(lt));
                } else if (lt.kind == TY_CHANNEL) {
                    ziman_error("type", e->line, e->col, "%s cannot be compared with '=='/'!=' in v0", type_name(lt));
                } else if (lt.kind == TY_FUNC) {
                    ziman_error("type", e->line, e->col,
                                "function values (%s) cannot be compared with '=='/'!=' -- compare a name or an id you keep alongside the function instead",
                                type_name(lt));
                } else if (lt.kind == TY_TYPEVAR) {
                    ziman_error("type", e->line, e->col,
                                "type parameter '%s' cannot be compared with '=='/'!=' because it has no 'Eq' bound (add '%s: Eq' to require one)",
                                type_name(lt), type_name(lt));
                } else {
                    ziman_error("type", e->line, e->col,
                                "struct '%s' cannot be compared with '=='/'!=' because one of its fields is not a comparable type",
                                type_name(lt));
                }
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

static Type check_unary(Module *module, Env *env, Expr *e) {
    Type t = check_expr(module, env, e->as.unary.operand);
    require_not_void(e->as.unary.operand, "as an operand");
    if (e->as.unary.op == OP_NEG) {
        if (t.kind != TY_INT && t.kind != TY_FLOAT && t.kind != TY_BIGINT && t.kind != TY_DECIMAL) {
            ziman_error("type", e->line, e->col, "unary '-' requires an int, float, bigint, or decimal operand, found %s", type_name(t));
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

/* True for `Option<T>` specifically (the prelude's own enum), never a
 * user-declared one of some other name -- the only enum type Ziman ever
 * lets a parameter be omitted for, or a bare unwrapped value be
 * auto-wrapped into. */
static int is_option_type(Type t) {
    return t.kind == TY_ENUM && strcmp(t.enum_name, "Option") == 0;
}

/* `Option<T>`'s own `T` -- only meaningful when is_option_type(t). */
static Type option_elem_type(Type t) {
    return t.type_args.items[0];
}

/* Rewrites `value` (already type-checked, of type option_elem_type
 * (option_type)) into a synthetic `Option<T>.Some(value)` node, the
 * auto-wrap sugar an Option<T> parameter gives a bare T argument or
 * default value (see docs/DESIGN.md's "Option<T> parameter omission and
 * defaults" entry). Reuses ordinary enum construction (EXPR_ENUM_ACCESS)
 * wholesale -- eval_enum_access in interpreter.c needs no changes at all
 * to evaluate this exactly like a `Some(...)` a person wrote by hand. */
static Expr *wrap_bare_in_some(Expr *value, Type option_type) {
    Expr *e = expr_new(EXPR_ENUM_ACCESS, value->line, value->col);
    e->as.enum_access.enum_name = "Option";
    e->as.enum_access.member_name = "Some";
    expr_list_push(&e->as.enum_access.args, value);
    e->as.enum_access.has_parens = 1;
    e->as.enum_access.type_args = option_type.type_args;
    e->resolved_type = option_type;
    return e;
}

/* The implicit default an omitted Option<T> parameter gets when its
 * declaration gave no `= expr` of its own -- a synthetic
 * `Option<T>.None`, again reusing ordinary enum construction as-is. */
static Expr *make_none_expr(Type option_type, int line, int col) {
    Expr *e = expr_new(EXPR_ENUM_ACCESS, line, col);
    e->as.enum_access.enum_name = "Option";
    e->as.enum_access.member_name = "None";
    e->as.enum_access.type_args = option_type.type_args;
    e->resolved_type = option_type;
    return e;
}

/* Rewrites `value` (already type-checked, of type `*result_type.elem`
 * when `is_ok`, `*result_type.elem_err` otherwise) into a synthetic
 * `ok(value)`/`err(value)` call -- the auto-wrap sugar a Result<T, E>
 * parameter gives a bare T-or-E argument (see docs/DESIGN.md's
 * "Result<T, E> bare-value auto-wrap" entry). Unlike Option<T>'s own
 * wrap (an ordinary enum, reusing EXPR_ENUM_ACCESS), Result<T, E> is a
 * hardcoded primitive with no enum declaration behind it at all --
 * `ok`/`err` are themselves hardcoded core builtins (see check_call's
 * and interpreter.c's own `strcmp(callee, "ok")` branches), so this
 * reuses THAT machinery instead: a synthetic EXPR_CALL to "ok"/"err"
 * needs zero changes anywhere it's evaluated, exactly like Option<T>'s
 * own wrap needed zero changes to eval_enum_access. */
static Expr *wrap_bare_in_result(Expr *value, Type result_type, int is_ok) {
    Expr *e = expr_new(EXPR_CALL, value->line, value->col);
    e->as.call.callee = is_ok ? "ok" : "err";
    expr_list_push(&e->as.call.args, value);
    e->resolved_type = result_type;
    return e;
}

/* The implicit value an omitted Option<T> parameter gets when its
 * declaration *did* give a `= expr` default -- a synthetic reference to
 * the global-env slot init_param_defaults (interpreter.c) will have
 * already filled in, once, before `main` runs, under this exact key
 * (see ast.h's Param.default_key). Reuses ordinary bare-name lookup
 * (EXPR_IDENT) wholesale: nothing in check_call's own env can ever
 * define a same-named local (the key contains '$', unforgeable by any
 * real Ziman identifier), so it always falls through to g_global_env,
 * exactly the way a bare reference to a real global val already does. */
static Expr *make_default_ref_expr(Param *param, int line, int col) {
    Expr *e = expr_new(EXPR_IDENT, line, col);
    e->as.ident_name = param->default_key;
    e->resolved_type = param->type;
    return e;
}

/* Checks `*arg_slot` against `param_type`. Two auto-wrap exceptions to
 * check_call's usual always-exact-match rule, both narrow and both
 * driven entirely by `param_type`'s own shape:
 *   - `Option<T>`: a bare `T` value is legal too, rewritten in place
 *     into `Some(T)` (see wrap_bare_in_some) -- needed by both an
 *     explicitly supplied argument and a parameter's own default
 *     expression (two of this function's callers).
 *   - `Result<T, E>`: a bare `T` value becomes `Ok(T)`; a bare `E`
 *     value becomes `Err(E)`; when `T == E`, always `Ok` (see
 *     wrap_bare_in_result and docs/DESIGN.md's "Result<T, E> bare-value
 *     auto-wrap" entry) -- `Result<T, E>` parameters are never
 *     omittable, so this only ever applies to a supplied argument, not
 *     a default (Result<T, E> can't have one).
 * Every other parameter type keeps check_call's always-exact-match
 * behavior, unchanged. Returns 1 and leaves `*out_arg_type` untouched
 * on success; returns 0 and sets `*out_arg_type` to the value's actual
 * type so the caller (which knows whether it's reporting a bad argument
 * or a bad default) can build its own error message. */
static int check_value_against_param_type(Module *module, Env *env, Expr **arg_slot, Type param_type, Type *out_arg_type) {
    Expr *arg = *arg_slot;
    Type arg_type;
    /* An empty array literal (`[]`) has no elements to infer a type
     * from, but the parameter it's passed to says exactly what it must
     * be -- the same "expected type from context" trick a `val`
     * annotation, struct field, or `return` already gets (see
     * check_expr_expecting). Without this, a call like
     * `execute(conn, sql, [])` needed a separately annotated `val` just
     * to spell "no parameters". */
    if (arg->kind == EXPR_ARRAY_LIT && arg->as.array_lit.elements.count == 0 && param_type.kind == TY_ARRAY) {
        arg->resolved_type = param_type;
        return 1;
    }
    /* A fresh (never-checked) Expr's resolved_type is always TY_VOID
     * (see expr_new) until check_expr fills it in -- the one exception
     * is a synthetic node canonicalize_named_args built for an omitted
     * Option<T> parameter (make_none_expr/make_default_ref_expr), which
     * sets resolved_type up front, to exactly this parameter's own
     * type, at construction time. Re-running check_expr on one would,
     * at best, redo work for nothing; for a default-value reference in
     * particular it would actively fail, since the global-env slot it
     * names isn't defined until interpretation starts (init_param_defaults),
     * long after type-checking is done. */
    if (arg->resolved_type.kind != TY_VOID) {
        arg_type = arg->resolved_type;
    } else {
        arg_type = check_expr(module, env, arg);
        require_not_void(arg, "as an argument");
    }
    if (type_equals(arg_type, param_type)) {
        return 1;
    }
    if (is_option_type(param_type) && type_equals(arg_type, option_elem_type(param_type))) {
        /* Option<Option<T>>: a bare Option<T> would become Some(that),
         * so a None quietly turns into Some(None) -- exactly the
         * distinction people get wrong. Make the author say which. */
        if (is_option_type(option_elem_type(param_type))) {
            ziman_error("type", arg->line, arg->col,
                        "a bare %s is ambiguous where %s is expected (it would silently become Some(...), so a None turns into Some(None)); write %s.Some(...) explicitly",
                        type_name(arg_type), type_name(param_type), type_name(param_type));
        }
        *arg_slot = wrap_bare_in_some(arg, param_type);
        return 1;
    }
    if (param_type.kind == TY_RESULT) {
        Type ok_type = *param_type.elem;
        Type err_type = *param_type.elem_err;
        if (type_equals(arg_type, ok_type)) {
            /* T == E: the sugar can't tell an intended error from a
             * success, and silently picking Ok turned `return "bad
             * input"` into a success. Require ok(...)/err(...). */
            if (type_equals(ok_type, err_type)) {
                ziman_error("type", arg->line, arg->col,
                            "a bare %s is ambiguous where %s is expected (the success and error types are the same): write ok(...) or err(...) to say which",
                            type_name(arg_type), type_name(param_type));
            }
            *arg_slot = wrap_bare_in_result(arg, param_type, 1);
            return 1;
        }
        if (!type_equals(ok_type, err_type) && type_equals(arg_type, err_type)) {
            *arg_slot = wrap_bare_in_result(arg, param_type, 0);
            return 1;
        }
    }
    *out_arg_type = arg_type;
    return 0;
}

/* Hardcoded core builtins (see README.md's "Builtins ship two ways")
 * each have exactly one fixed signature already special-cased below by
 * name, not an ordinary declared FunctionDecl -- so there's no
 * parameter list here for a named call to reorder against. Named
 * arguments are for real (user or prelude) `fn`/`native fn`
 * declarations only. */
static const char *const BUILTINS_NO_NAMED_ARGS[] = {
    "print", "str", "format", "len", "contains", "hash", "compare", "abs", "min", "max",
    "push", "insert", "set", "remove", "is_ok", "unwrap", "unwrap_err",
    "ok", "err", "channel_new", "send", "receive", NULL,
};

/* A named call (`f(y: 2, x: 1)`) is reordered here, once, into the same
 * positional order the rest of check_call (and, downstream, the
 * interpreter) already expects -- so nothing past this point ever needs
 * to know a call was written with names at all. Requires the given
 * names to be exactly `fn`'s declared parameter names, each appearing
 * at most once (a duplicate is always rejected, even for an Option<T>
 * parameter -- there's no reading of `f(x: 1, x: 2)` that isn't a
 * mistake). An Option<T> parameter may be left out entirely (never a
 * non-Option one, which has no "absent" state to fall back to) -- a
 * missing one is filled in with a synthetic reference to its own
 * default (make_default_ref_expr) when its declaration gave one, or a
 * synthetic `None` (make_none_expr) when it didn't, so this function's
 * one remaining job -- filling `e->as.call.args` to exactly
 * `fn->params.count`, in order -- always finishes the same way whether
 * or not any parameter was actually omitted.
 *
 * `f()` -- literally empty parentheses -- is also routed through here,
 * even though the parser calls that a positional call (`arg_names ==
 * NULL`): with zero arguments given, there's no `name:` for the parser
 * to have seen either way, so a completely empty call is genuinely
 * ambiguous between "positional, zero given" and "named, zero given,
 * everything omitted" from its syntax alone -- and unlike a *partial*
 * positional call (`f(1)` when `f` needs two, which must stay exactly
 * as strict as ever), there's nothing here that omission could get
 * wrong. Treating it as the latter is what makes `sb_new()` -- a
 * function whose every parameter is Option<T> -- callable at all; it's
 * a no-op for a function with zero parameters (the fill loop below
 * never runs) and still correctly rejects any non-Option parameter
 * (the same "missing required argument" error a partially-omitted named
 * call already gives). */
static void canonicalize_named_args(Expr *e, FunctionDecl *fn) {
    char **names = e->as.call.arg_names;
    if (names == NULL && (e->as.call.args.count > 0 || fn->params.count == 0)) {
        return;
    }
    int argc = e->as.call.args.count;
    Expr **reordered = malloc((size_t)fn->params.count * sizeof(Expr *));
    for (int i = 0; i < fn->params.count; i++) {
        reordered[i] = NULL;
    }
    for (int i = 0; i < argc; i++) {
        int param_index = -1;
        for (int j = 0; j < fn->params.count; j++) {
            if (strcmp(names[i], fn->params.items[j].name) == 0) {
                param_index = j;
                break;
            }
        }
        if (param_index == -1) {
            ziman_error("type", e->line, e->col,
                        "function '%s' has no parameter named '%s'",
                        e->as.call.callee, names[i]);
        }
        if (reordered[param_index] != NULL) {
            ziman_error("type", e->line, e->col,
                        "argument '%s' given more than once in this call to '%s'",
                        names[i], e->as.call.callee);
        }
        reordered[param_index] = e->as.call.args.items[i];
    }
    for (int i = 0; i < fn->params.count; i++) {
        if (reordered[i] != NULL) {
            continue;
        }
        Param *param = &fn->params.items[i];
        if (!is_option_type(param->type)) {
            ziman_error("type", e->line, e->col,
                        "function '%s' is missing required argument '%s'",
                        e->as.call.callee, param->name);
        }
        reordered[i] = param->default_expr != NULL
                           ? make_default_ref_expr(param, e->line, e->col)
                           : make_none_expr(param->type, e->line, e->col);
    }
    e->as.call.args.items = reordered;
    e->as.call.args.count = fn->params.count;
    e->as.call.args.capacity = fn->params.count;
    e->as.call.arg_names = NULL;
}

static Type check_call(Module *module, Env *env, Expr *e) {
    const char *name = e->as.call.callee;

    /* A call through a local of function type (a parameter or a val): a local
     * always wins over a top-level function of the same name. Named arguments
     * and type arguments don't apply -- the value carries only a parameter list. */
    if (e->as.call.local_name != NULL && e->as.call.type_args.count == 0) {
        Binding *fb = env_lookup(env, e->as.call.local_name);
        if (fb != NULL && fb->type.kind == TY_FUNC) {
            Type ft = fb->type;
            if (e->as.call.arg_names != NULL) {
                ziman_error("type", e->line, e->col, "named arguments aren't supported when calling through a function value ('%s')", e->as.call.local_name);
            }
            if (e->as.call.args.count != ft.type_args.count) {
                ziman_error("type", e->line, e->col, "'%s' has type %s, which takes %d argument(s), found %d",
                            e->as.call.local_name, type_name(ft), ft.type_args.count, e->as.call.args.count);
            }
            for (int i = 0; i < e->as.call.args.count; i++) {
                Type arg_type;
                if (!check_value_against_param_type(module, env, &e->as.call.args.items[i], ft.type_args.items[i], &arg_type)) {
                    ziman_error("type", e->as.call.args.items[i]->line, e->as.call.args.items[i]->col,
                                "argument %d of '%s' (a %s) expects %s, found %s", i + 1, e->as.call.local_name, type_name(ft),
                                type_name(ft.type_args.items[i]), type_name(arg_type));
                }
            }
            e->as.call.through_value = 1;
            return *ft.elem;
        }
    }

    if (e->as.call.arg_names != NULL) {
        for (int i = 0; BUILTINS_NO_NAMED_ARGS[i] != NULL; i++) {
            if (strcmp(name, BUILTINS_NO_NAMED_ARGS[i]) == 0) {
                ziman_error("type", e->line, e->col, "named arguments aren't supported for '%s'", name);
            }
        }
    }

    if (strcmp(name, "print") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'print' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        check_expr(module, env, e->as.call.args.items[0]);
        require_not_void(e->as.call.args.items[0], "as an argument");
        return type_scalar(TY_VOID);
    }

    if (strcmp(name, "str") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'str' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (arg_type.kind != TY_INT && arg_type.kind != TY_FLOAT && arg_type.kind != TY_BOOL && arg_type.kind != TY_BIGINT && arg_type.kind != TY_DECIMAL && arg_type.kind != TY_BYTE) {
            ziman_error("type", arg->line, arg->col,
                        "'str' expects an int, float, bool, bigint, decimal, or byte, found %s (it's already a string)",
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
            check_expr(module, env, arg);
            require_not_void(arg, "as a format argument");
        }
        return type_scalar(TY_STRING);
    }

    if (strcmp(name, "len") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'len' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (arg_type.kind != TY_ARRAY && arg_type.kind != TY_STRING) {
            ziman_error("type", arg->line, arg->col, "'len' expects an array or string, found %s", type_name(arg_type));
        }
        return type_scalar(TY_INT);
    }

    if (strcmp(name, "contains") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'contains' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *first_arg = e->as.call.args.items[0];
        Type first_type = check_expr(module, env, first_arg);
        Expr *second_arg = e->as.call.args.items[1];

        if (first_type.kind == TY_STRING) {
            Type second_type = check_expr(module, env, second_arg);
            if (second_type.kind != TY_STRING) {
                ziman_error("type", second_arg->line, second_arg->col, "argument 2 of 'contains' expects string, found %s", type_name(second_type));
            }
            return type_scalar(TY_BOOL);
        }
        if (first_type.kind == TY_ARRAY) {
            Type elem_type = *first_type.elem;
            if (!is_scalar_kind(elem_type.kind)) {
                ziman_error("type", first_arg->line, first_arg->col,
                            "'contains' requires an array of a comparable (scalar) type, found %s", type_name(first_type));
            }
            Type second_type = check_expr(module, env, second_arg);
            if (!type_equals(second_type, elem_type)) {
                ziman_error("type", second_arg->line, second_arg->col,
                            "argument 2 of 'contains' expects %s (the array's element type), found %s",
                            type_name(elem_type), type_name(second_type));
            }
            return type_scalar(TY_BOOL);
        }
        ziman_error("type", first_arg->line, first_arg->col, "'contains' expects a string or array as its first argument, found %s", type_name(first_type));
    }

    if (strcmp(name, "compare") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'compare' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *first = e->as.call.args.items[0];
        Expr *second = e->as.call.args.items[1];
        Type first_type = check_expr(module, env, first);
        Type second_type = check_expr(module, env, second);
        if (!type_equals(first_type, second_type)) {
            ziman_error("type", second->line, second->col, "'compare' needs two values of the same type, found %s and %s",
                        type_name(first_type), type_name(second_type));
        }
        if (!type_supports_ordering(module, first_type)) {
            ziman_error("type", first->line, first->col,
                        "'compare' expects int, float, bigint, decimal, byte, string, an array/Option/Result of those, a struct/enum with a "
                        "'<type>_compare' function ('#[derive(Ord)]' writes one), or a type parameter bounded by 'Ord', found %s",
                        type_name(first_type));
        }
        return type_scalar(TY_INT);
    }

    if (strcmp(name, "hash") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'hash' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (!type_supports_hashing(module, arg_type, 0)) {
            ziman_error("type", arg->line, arg->col,
                        "'hash' expects a type that supports '==' with hashable parts (int, float, bool, string, bigint, decimal, byte, arrays of those, "
                        "structs/enums/Option/Result made of them) or a type parameter bounded by 'Hash', found %s",
                        type_name(arg_type));
        }
        return type_scalar(TY_INT);
    }

    if (strcmp(name, "abs") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'abs' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (arg_type.kind != TY_INT && arg_type.kind != TY_FLOAT && arg_type.kind != TY_BIGINT && arg_type.kind != TY_DECIMAL) {
            ziman_error("type", arg->line, arg->col, "'abs' expects int, float, bigint, or decimal, found %s", type_name(arg_type));
        }
        return arg_type;
    }

    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly 2 arguments, found %d", name, e->as.call.args.count);
        }
        Expr *a_arg = e->as.call.args.items[0];
        Type a_type = check_expr(module, env, a_arg);
        if (a_type.kind != TY_INT && a_type.kind != TY_FLOAT && a_type.kind != TY_BIGINT && a_type.kind != TY_DECIMAL) {
            ziman_error("type", a_arg->line, a_arg->col, "'%s' expects int, float, bigint, or decimal, found %s", name, type_name(a_type));
        }
        Expr *b_arg = e->as.call.args.items[1];
        Type b_type = check_expr(module, env, b_arg);
        if (!type_equals(b_type, a_type)) {
            ziman_error("type", b_arg->line, b_arg->col, "'%s' expects both arguments to be the same type, found %s and %s", name, type_name(a_type), type_name(b_type));
        }
        return a_type;
    }

    if (strcmp(name, "push") == 0 || strcmp(name, "insert") == 0 || strcmp(name, "set") == 0) {
        int is_push = strcmp(name, "push") == 0;
        int expected_argc = is_push ? 2 : 3;
        if (e->as.call.args.count != expected_argc) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly %d argument(s), found %d", name, expected_argc, e->as.call.args.count);
        }

        Expr *arr_arg = e->as.call.args.items[0];
        Type arr_type = check_expr(module, env, arr_arg);
        if (arr_type.kind != TY_ARRAY) {
            ziman_error("type", arr_arg->line, arr_arg->col, "argument 1 of '%s' expects an array, found %s", name, type_name(arr_type));
        }

        int value_arg_index = is_push ? 1 : 2;
        if (!is_push) {
            Expr *idx_arg = e->as.call.args.items[1];
            Type idx_type = check_expr(module, env, idx_arg);
            if (idx_type.kind != TY_INT) {
                ziman_error("type", idx_arg->line, idx_arg->col, "argument 2 of '%s' expects int, found %s", name, type_name(idx_type));
            }
        }

        Expr *value_arg = e->as.call.args.items[value_arg_index];
        Type value_type = check_expr(module, env, value_arg);
        require_not_void(value_arg, "as an array element");
        if (!type_equals(value_type, *arr_type.elem)) {
            ziman_error("type", value_arg->line, value_arg->col,
                        "argument %d of '%s' expects %s (the array's element type), found %s",
                        value_arg_index + 1, name, type_name(*arr_type.elem), type_name(value_type));
        }
        return arr_type;
    }

    if (strcmp(name, "remove") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'remove' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *arr_arg = e->as.call.args.items[0];
        Type arr_type = check_expr(module, env, arr_arg);
        if (arr_type.kind != TY_ARRAY) {
            ziman_error("type", arr_arg->line, arr_arg->col, "argument 1 of 'remove' expects an array, found %s", type_name(arr_type));
        }
        Expr *idx_arg = e->as.call.args.items[1];
        Type idx_type = check_expr(module, env, idx_arg);
        if (idx_type.kind != TY_INT) {
            ziman_error("type", idx_arg->line, idx_arg->col, "argument 2 of 'remove' expects int, found %s", type_name(idx_type));
        }
        return arr_type;
    }

    if (strcmp(name, "is_ok") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'is_ok' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (arg_type.kind != TY_RESULT) {
            ziman_error("type", arg->line, arg->col, "'is_ok' expects a Result, found %s", type_name(arg_type));
        }
        return type_scalar(TY_BOOL);
    }

    if (strcmp(name, "unwrap") == 0 || strcmp(name, "unwrap_err") == 0) {
        int is_unwrap = strcmp(name, "unwrap") == 0;
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'%s' takes exactly 1 argument, found %d", name, e->as.call.args.count);
        }
        Expr *arg = e->as.call.args.items[0];
        Type arg_type = check_expr(module, env, arg);
        if (arg_type.kind != TY_RESULT) {
            ziman_error("type", arg->line, arg->col, "'%s' expects a Result, found %s", name, type_name(arg_type));
        }
        return is_unwrap ? *arg_type.elem : *arg_type.elem_err;
    }

    if (strcmp(name, "ok") == 0 || strcmp(name, "err") == 0) {
        /* Reachable only when nothing gave check_ok_err_call() (via
         * check_expr_expecting) an expected Result<T, E> to work with --
         * e.g. `print(ok(5))` or a bare `ok(5)` statement. There's no way
         * to infer the type this would need without that context. */
        ziman_error("type", e->line, e->col,
                    "cannot infer the type of '%s(...)' here; only legal as a val/var initializer with an "
                    "explicit Result<T, E> annotation, a struct literal field, a 'with' override, or a "
                    "function's returned value",
                    name);
    }

    if (strcmp(name, "channel_new") == 0) {
        /* Same reasoning as ok/err above, via check_channel_new_call. */
        ziman_error("type", e->line, e->col,
                    "cannot infer the type of 'channel_new()' here; only legal as a val/var initializer with an "
                    "explicit Channel<T> annotation, a struct literal field, a 'with' override, or a "
                    "function's returned value");
    }

    if (strcmp(name, "send") == 0) {
        if (e->as.call.args.count != 2) {
            ziman_error("type", e->line, e->col, "'send' takes exactly 2 arguments, found %d", e->as.call.args.count);
        }
        Expr *chan_arg = e->as.call.args.items[0];
        Type chan_type = check_expr(module, env, chan_arg);
        if (chan_type.kind != TY_CHANNEL) {
            ziman_error("type", chan_arg->line, chan_arg->col, "argument 1 of 'send' expects a Channel, found %s", type_name(chan_type));
        }
        Expr *val_arg = e->as.call.args.items[1];
        Type val_type = check_expr(module, env, val_arg);
        require_not_void(val_arg, "as a channel value");
        if (!type_equals(val_type, *chan_type.elem)) {
            ziman_error("type", val_arg->line, val_arg->col,
                        "argument 2 of 'send' expects %s (the channel's element type), found %s",
                        type_name(*chan_type.elem), type_name(val_type));
        }
        return type_scalar(TY_VOID);
    }

    if (strcmp(name, "receive") == 0) {
        if (e->as.call.args.count != 1) {
            ziman_error("type", e->line, e->col, "'receive' takes exactly 1 argument, found %d", e->as.call.args.count);
        }
        Expr *chan_arg = e->as.call.args.items[0];
        Type chan_type = check_expr(module, env, chan_arg);
        if (chan_type.kind != TY_CHANNEL) {
            ziman_error("type", chan_arg->line, chan_arg->col, "'receive' expects a Channel, found %s", type_name(chan_type));
        }
        return *chan_type.elem;
    }

    FunctionDecl *fn = find_function(module, name);
    if (fn == NULL) {
        /* Inside a #[derive] expansion, a missing <type>_to_json / _from_json
         * / _to_sql / _from_sql means a field's type provides no conversion
         * -- the one thing a derive can't supply for it -- so say how to. */
        static const char *const suffixes[] = {"_to_json", "_from_json", "_to_sql", "_from_sql"};
        if (ziman_error_context() != NULL) {
            size_t n = strlen(name);
            for (size_t k = 0; k < sizeof suffixes / sizeof suffixes[0]; k++) {
                size_t sl = strlen(suffixes[k]);
                if (n > sl && strcmp(name + n - sl, suffixes[k]) == 0) {
                    ziman_error("type", e->line, e->col,
                                "call to undefined function '%s' -- a derived conversion needs one for every field type: "
                                "give that type its own #[derive(%s)] (or write '%s' by hand and import it with 'use')",
                                name, (k < 2) ? "Json" : "Row", name);
                }
            }
        }
        ziman_error("type", e->line, e->col, "call to undefined function '%s'", name);
    }
    canonicalize_named_args(e, fn);
    TypeList *type_args = &e->as.call.type_args;
    /* Explicit type arguments everywhere, no inference -- same rules as
     * a generic struct literal or enum construction. */
    if (fn->type_params.count == 0 && type_args->count > 0) {
        ziman_error("type", e->line, e->col, "'%s' is not generic, but was given %d type argument(s)", name, type_args->count);
    }
    if (fn->type_params.count > 0 && type_args->count == 0) {
        ziman_error("type", e->line, e->col, "'%s' is generic and requires explicit type arguments, e.g. '%s<...>(...)'", name, name);
    }
    if (fn->type_params.count != type_args->count) {
        ziman_error("type", e->line, e->col, "'%s' takes %d type argument(s), found %d", name, fn->type_params.count, type_args->count);
    }
    for (int i = 0; i < type_args->count; i++) {
        check_type_well_formed(module, type_args->items[i], e->line, e->col);
    }
    check_type_args_satisfy_bounds(module, &fn->type_params, type_args, e->line, e->col);

    int is_variadic = fn->params.count > 0 && fn->params.items[fn->params.count - 1].is_variadic;
    int fixed_count = is_variadic ? fn->params.count - 1 : fn->params.count;

    if (is_variadic) {
        if (e->as.call.args.count < fixed_count) {
            ziman_error("type", e->line, e->col, "function '%s' expects at least %d argument(s), found %d", name, fixed_count, e->as.call.args.count);
        }
    } else if (e->as.call.args.count != fn->params.count) {
        ziman_error("type", e->line, e->col, "function '%s' expects %d argument(s), found %d", name, fn->params.count, e->as.call.args.count);
    }
    for (int i = 0; i < fixed_count; i++) {
        Type expected = substitute_type(fn->params.items[i].type, &fn->type_params, type_args);
        Type arg_type;
        if (!check_value_against_param_type(module, env, &e->as.call.args.items[i], expected, &arg_type)) {
            Expr *arg = e->as.call.args.items[i];
            if (is_option_type(expected)) {
                ziman_error("type", arg->line, arg->col,
                            "argument %d of '%s' expects %s or %s, found %s",
                            i + 1, name, type_name(expected), type_name(option_elem_type(expected)), type_name(arg_type));
            }
            if (expected.kind == TY_RESULT) {
                Type ok_type = *expected.elem;
                Type err_type = *expected.elem_err;
                if (type_equals(ok_type, err_type)) {
                    ziman_error("type", arg->line, arg->col,
                                "argument %d of '%s' expects %s or %s, found %s",
                                i + 1, name, type_name(expected), type_name(ok_type), type_name(arg_type));
                }
                ziman_error("type", arg->line, arg->col,
                            "argument %d of '%s' expects %s, %s, or %s, found %s",
                            i + 1, name, type_name(expected), type_name(ok_type), type_name(err_type), type_name(arg_type));
            }
            ziman_error("type", arg->line, arg->col,
                        "argument %d of '%s' expects %s, found %s",
                        i + 1, name, type_name(expected), type_name(arg_type));
        }
    }
    /* The trailing (variadic) arguments -- either zero or more individual
     * values of the element type, collected into an array at the call
     * site (see call_function), or, when there's exactly one trailing
     * argument, an already-built array of that element type passed
     * through directly. Which of the two a given call means is decided
     * purely by that argument's own static type -- sound with no
     * ambiguity, since Ziman has no implicit coercion and no
     * overloading to confuse it with. */
    if (is_variadic) {
        Type elem_type = substitute_type(fn->params.items[fixed_count].type, &fn->type_params, type_args);
        int tail_count = e->as.call.args.count - fixed_count;
        if (tail_count == 1) {
            Expr *arg = e->as.call.args.items[fixed_count];
            Type arg_type = check_expr(module, env, arg);
            require_not_void(arg, "as an argument");
            if (type_equals(arg_type, type_array(elem_type))) {
                e->as.call.variadic_tail_is_array_spread = 1;
            } else if (!type_equals(arg_type, elem_type)) {
                ziman_error("type", arg->line, arg->col,
                            "argument %d of '%s' expects %s or %s, found %s",
                            fixed_count + 1, name, type_name(elem_type), type_name(type_array(elem_type)), type_name(arg_type));
            }
        } else {
            for (int i = fixed_count; i < e->as.call.args.count; i++) {
                Expr *arg = e->as.call.args.items[i];
                Type arg_type = check_expr(module, env, arg);
                require_not_void(arg, "as an argument");
                if (!type_equals(arg_type, elem_type)) {
                    ziman_error("type", arg->line, arg->col,
                                "argument %d of '%s' expects %s, found %s",
                                i + 1, name, type_name(elem_type), type_name(arg_type));
                }
            }
        }
    }
    return substitute_type(fn->return_type, &fn->type_params, type_args);
}

/* `ok(x)`/`err(e)` can't fully infer their own type the way every other
 * call can -- `ok(x)`'s argument only tells us the success type, never the
 * error type (and vice versa for `err`). Legal wherever an expected type
 * is already available: a val/var's own declared type, a struct literal
 * field or `with` override (via check_expr_expecting), or a function's
 * declared return type (STMT_RETURN). Returns 1 and sets `e->resolved_type`
 * if `e` is exactly `ok(...)`/`err(...)` and `expected` is a Result<T, E>;
 * returns 0 otherwise, meaning the caller should fall back to plain
 * check_expr(). */
static int check_ok_err_call(Module *module, Env *env, Expr *e, Type expected) {
    if (e->kind != EXPR_CALL) return 0;
    int is_ok_call = strcmp(e->as.call.callee, "ok") == 0;
    int is_err_call = strcmp(e->as.call.callee, "err") == 0;
    if (!is_ok_call && !is_err_call) return 0;
    if (expected.kind != TY_RESULT) return 0;

    if (e->as.call.args.count != 1) {
        ziman_error("type", e->line, e->col, "'%s' takes exactly 1 argument, found %d", e->as.call.callee, e->as.call.args.count);
    }
    Expr *arg = e->as.call.args.items[0];
    Type arg_type = check_expr(module, env, arg);
    require_not_void(arg, "as a Result value");

    Type want = is_ok_call ? *expected.elem : *expected.elem_err;
    if (!type_equals(arg_type, want)) {
        ziman_error("type", arg->line, arg->col, "'%s' expects %s, found %s", e->as.call.callee, type_name(want), type_name(arg_type));
    }
    e->resolved_type = expected;
    return 1;
}

/* `channel_new()` has the same "can't infer its own type" problem `ok`/
 * `err` and empty array literals have, for the same reason: the call
 * itself (zero arguments) carries no information at all about what it's a
 * channel of. Same set of legal positions as check_ok_err_call. Returns 1
 * and sets `e->resolved_type` if `e` is exactly `channel_new()` and
 * `expected` is a Channel<T>; returns 0 otherwise. */
static int check_channel_new_call(Expr *e, Type expected) {
    if (e->kind != EXPR_CALL) return 0;
    if (strcmp(e->as.call.callee, "channel_new") != 0) return 0;
    if (expected.kind != TY_CHANNEL) return 0;
    if (e->as.call.args.count != 0) {
        ziman_error("type", e->line, e->col, "'channel_new' takes no arguments, found %d", e->as.call.args.count);
    }
    e->resolved_type = expected;
    return 1;
}

static Type check_cast(Module *module, Env *env, Expr *e) {
    Type from = check_expr(module, env, e->as.cast.operand);
    require_not_void(e->as.cast.operand, "as a cast operand");
    Type to = e->as.cast.target_type;
    int numeric_from = from.kind == TY_INT || from.kind == TY_FLOAT || from.kind == TY_BIGINT || from.kind == TY_DECIMAL;
    int numeric_to = to.kind == TY_INT || to.kind == TY_FLOAT || to.kind == TY_BIGINT || to.kind == TY_DECIMAL;
    /* byte only casts to/from int, kept separate from the numeric web
     * above rather than folded into it -- byte isn't a number to compute
     * with (see ast.h's TY_BYTE comment), so byte<->float/bigint/decimal
     * were never asked for and would just be unused interpreter.c
     * branches. int as byte saturates to [0, 255] rather than wrapping,
     * matching bignum_to_i64_saturating's "clamp, don't silently wrap"
     * precedent for narrowing a wider numeric type. */
    int byte_int_cast = (from.kind == TY_BYTE && to.kind == TY_INT) || (from.kind == TY_INT && to.kind == TY_BYTE);
    if (!(numeric_from && numeric_to) && !byte_int_cast) {
        ziman_error("type", e->line, e->col, "cannot cast %s to %s (only casts between int/float/bigint/decimal, or int/byte, are supported)", type_name(from), type_name(to));
    }
    return to;
}

static Type check_array_literal(Module *module, Env *env, Expr *e) {
    ExprList *elements = &e->as.array_lit.elements;
    if (elements->count == 0) {
        ziman_error("type", e->line, e->col,
                    "cannot infer the element type of an empty array literal; add a type annotation (e.g. val x: [int] = [])");
    }
    Type elem_type = check_expr(module, env, elements->items[0]);
    require_not_void(elements->items[0], "as an array element");
    for (int i = 1; i < elements->count; i++) {
        Expr *el = elements->items[i];
        Type t = check_expr(module, env, el);
        if (!type_equals(t, elem_type)) {
            ziman_error("type", el->line, el->col,
                        "array elements must all have the same type: element 0 is %s, element %d is %s",
                        type_name(elem_type), i, type_name(t));
        }
    }
    return type_array(elem_type);
}

/* The loop binders currently in scope, innermost last, so an index
 * expression can tell whether its index is the ELEMENT binder of a
 * `for x in arr` over that very array. `array` is the iterated
 * identifier for a single-binder array loop, NULL for every other kind
 * of binder (a range index, or a two-binder loop's variables) -- those
 * still get an entry so they shadow an outer element binder of the same
 * name. */
typedef struct {
    const char *binder;
    const char *array;
} LoopBinder;
enum { MAX_LOOP_BINDERS = 64 };
static LoopBinder g_loop_binders[MAX_LOOP_BINDERS];
static int g_loop_binder_count = 0;

static void push_loop_binder(const char *binder, const char *array) {
    if (g_loop_binder_count < MAX_LOOP_BINDERS) {
        g_loop_binders[g_loop_binder_count].binder = binder;
        g_loop_binders[g_loop_binder_count].array = array;
    }
    g_loop_binder_count++;
}

/* `for i in a { ... a[i] ... }`: in Ziman `i` is the ELEMENT here, but
 * models (and people) coming from Go/JS/Python read it as the index and
 * silently compute the wrong thing. Reject exactly this shape and say how
 * to write each intent. */
static void reject_element_used_as_index(Expr *e) {
    Expr *arr = e->as.index.array;
    Expr *idx = e->as.index.index;
    if (arr->kind != EXPR_IDENT || idx->kind != EXPR_IDENT) return;
    int n = g_loop_binder_count < MAX_LOOP_BINDERS ? g_loop_binder_count : MAX_LOOP_BINDERS;
    for (int i = n - 1; i >= 0; i--) {
        if (strcmp(g_loop_binders[i].binder, idx->as.ident_name) != 0) continue;
        if (g_loop_binders[i].array != NULL && strcmp(g_loop_binders[i].array, arr->as.ident_name) == 0) {
            ziman_error("type", idx->line, idx->col,
                        "'%s' is an element of '%s' here ('for %s in %s' binds each element, not its position), so '%s[%s]' indexes with a value; "
                        "to loop over positions write 'for %s in 0..len(%s)', or use 'for %s, item in %s' for both",
                        idx->as.ident_name, arr->as.ident_name, idx->as.ident_name, arr->as.ident_name,
                        arr->as.ident_name, idx->as.ident_name, idx->as.ident_name, arr->as.ident_name,
                        idx->as.ident_name, arr->as.ident_name);
        }
        return;
    }
}

static Type check_index(Module *module, Env *env, Expr *e) {
    reject_element_used_as_index(e);
    Type arr_type = check_expr(module, env, e->as.index.array);
    if (arr_type.kind != TY_ARRAY) {
        ziman_error("type", e->as.index.array->line, e->as.index.array->col, "cannot index into %s (not an array)", type_name(arr_type));
    }
    Type idx_type = check_expr(module, env, e->as.index.index);
    if (idx_type.kind != TY_INT) {
        ziman_error("type", e->as.index.index->line, e->as.index.index->col, "array index must be int, found %s", type_name(idx_type));
    }
    return *arr_type.elem;
}

/* Shared by struct-literal construction and `with`: does `field_name`
 * (from a FieldInit at line:col) name a real field on `decl`, and is this
 * the first time it's been named among `checked_so_far` entries? Returns
 * the matching field on success; exits on an unknown or duplicate name. */
static Param *resolve_field_init(StructDecl *decl, FieldInitList *inits, int index) {
    FieldInit *fi = &inits->items[index];
    Param *field = NULL;
    for (int j = 0; j < decl->fields.count; j++) {
        if (strcmp(decl->fields.items[j].name, fi->name) == 0) { field = &decl->fields.items[j]; break; }
    }
    if (field == NULL) {
        ziman_error("type", fi->line, fi->col, "no field '%s' on struct '%s'", fi->name, decl->name);
    }
    for (int k = 0; k < index; k++) {
        if (strcmp(inits->items[k].name, fi->name) == 0) {
            ziman_error("type", fi->line, fi->col, "field '%s' specified more than once", fi->name);
        }
    }
    return field;
}

/* Like check_expr, but tries every "can't infer its own type" case Ziman
 * has -- an empty array literal (`[]`), `ok(...)`/`err(...)`,
 * `channel_new()`, and the Option<T>/Result<T, E> bare-value auto-wrap
 * -- against `expected` first, before falling back to plain check_expr
 * for everything else (including any of those where `expected` doesn't
 * match what they need; those keep getting whatever error they'd
 * otherwise get, unchanged -- the auto-wrap case is the one exception,
 * see below). This is the same "expected type from context" idea
 * val/var declarations already use (see check_stmt's STMT_VAL/STMT_VAR
 * case) extended to the other positions where the expected type is just
 * as statically known: a struct literal field, a `with` override, and a
 * `return` value.
 *
 * Takes `Expr **e_slot` rather than `Expr *e`, unlike every other
 * check_* function, specifically so the auto-wrap case can replace the
 * expression in place (the same reason check_value_against_param_type
 * itself takes one) -- every other branch here only ever reads through
 * it, never reassigns it. On an auto-wrap mismatch, this deliberately
 * does NOT raise its own error the way check_value_against_param_type's
 * other caller (check_call) does: it returns the value's own actual
 * type instead, so each of *this* function's four callers keeps raising
 * its own already-existing, position-specific message ("field 'x' of
 * 'Y' expects...", "returned X but function is declared to return
 * Y", ...) exactly as it did before auto-wrap existed, just without the
 * richer "or T"/"or T, E" wording check_call's own version gives --
 * simpler than teaching four different messages that wording, and no
 * worse than what the mismatch case already said. */
static Type check_expr_expecting(Module *module, Env *env, Expr **e_slot, Type expected) {
    Expr *e = *e_slot;
    if (e->kind == EXPR_ARRAY_LIT && e->as.array_lit.elements.count == 0 && expected.kind == TY_ARRAY) {
        e->resolved_type = expected;
        return expected;
    }
    if (check_ok_err_call(module, env, e, expected)) {
        return expected;
    }
    if (check_channel_new_call(e, expected)) {
        return expected;
    }
    if (is_option_type(expected) || expected.kind == TY_RESULT) {
        Type arg_type;
        if (check_value_against_param_type(module, env, e_slot, expected, &arg_type)) {
            return expected;
        }
        return arg_type;
    }
    return check_expr(module, env, *e_slot);
}

static Type check_struct_literal(Module *module, Env *env, Expr *e) {
    const char *type_name_str = e->as.struct_lit.type_name;
    StructDecl *decl = find_struct(module, type_name_str);
    if (decl == NULL) {
        ziman_error("type", e->line, e->col, "no struct named '%s'", type_name_str);
    }
    TypeList *type_args = &e->as.struct_lit.type_args;
    /* Explicit type arguments everywhere, no inference: a generic struct's
     * construction must always spell out `Name<Arg, ...>`, and a
     * non-generic one must never spell any out. */
    if (decl->type_params.count == 0 && type_args->count > 0) {
        ziman_error("type", e->line, e->col, "'%s' is not generic, but was given %d type argument(s)", type_name_str, type_args->count);
    }
    if (decl->type_params.count > 0 && type_args->count == 0) {
        ziman_error("type", e->line, e->col, "'%s' is generic and requires explicit type arguments, e.g. '%s<...> { ... }'", type_name_str, type_name_str);
    }
    if (decl->type_params.count != type_args->count) {
        ziman_error("type", e->line, e->col, "'%s' takes %d type argument(s), found %d", type_name_str, decl->type_params.count, type_args->count);
    }
    for (int i = 0; i < type_args->count; i++) {
        check_type_well_formed(module, type_args->items[i], e->line, e->col);
    }
    check_type_args_satisfy_bounds(module, &decl->type_params, type_args, e->line, e->col);

    FieldInitList *inits = &e->as.struct_lit.fields;

    for (int i = 0; i < inits->count; i++) {
        Param *field = resolve_field_init(decl, inits, i);
        Type field_type = substitute_type(field->type, &decl->type_params, type_args);
        Type value_type = check_expr_expecting(module, env, &inits->items[i].value, field_type);
        require_not_void(inits->items[i].value, "as a field value");
        if (!type_equals(value_type, field_type)) {
            ziman_error("type", inits->items[i].value->line, inits->items[i].value->col,
                        "field '%s' of '%s' expects %s, found %s",
                        field->name, type_name_str, type_name(field_type), type_name(value_type));
        }
    }
    /* Construction requires every field, unlike `with` -- there's no base
     * value to fall back to for anything left unmentioned. */
    for (int j = 0; j < decl->fields.count; j++) {
        const char *field_name = decl->fields.items[j].name;
        int provided = 0;
        for (int i = 0; i < inits->count; i++) {
            if (strcmp(inits->items[i].name, field_name) == 0) { provided = 1; break; }
        }
        if (!provided) {
            ziman_error("type", e->line, e->col, "missing field '%s' in construction of '%s'", field_name, type_name_str);
        }
    }
    Type result = type_struct(type_name_str);
    result.type_args = *type_args;
    return result;
}

/* A top-level function used as a value (`apply(double, 3)`). Records the
 * FunctionDecl on the expression (`fn_ref`) for the interpreter and returns the
 * `fn(...) -> ...` type. Only reached when no local variable of that name
 * exists -- a local always wins. Not every function qualifies: a generic one
 * has no single type to give it (an explicit-type-argument form of reference
 * doesn't exist yet), and one with a variadic parameter can't be called with an
 * argument list of unknown length through a value. */
static int resolve_function_reference(Module *module, Expr *e, Type *out) {
    if (e->fn_ref_name == NULL) return 0;
    if (e->fn_ref_name[0] == '!') {
        ziman_error("type", e->line, e->col, "%s", e->fn_ref_name + 1);
    }
    FunctionDecl *fn = find_function(module, e->fn_ref_name);
    if (fn == NULL) return 0;
    if (fn->type_params.count > 0) {
        ziman_error("type", e->line, e->col,
                    "'%s' is generic, so it can't be used as a value yet -- wrap it in a non-generic function that fixes its type arguments",
                    fn->name);
    }
    for (int i = 0; i < fn->params.count; i++) {
        if (fn->params.items[i].is_variadic) {
            ziman_error("type", e->line, e->col,
                        "'%s' takes a variable number of arguments, so it can't be used as a value -- wrap it in a function with a fixed parameter list",
                        fn->name);
        }
    }
    e->fn_ref = fn;
    *out = function_value_type(fn);
    return 1;
}

static Type check_field_access(Module *module, Env *env, Expr *e) {
    if (e->as.field_access.base->kind == EXPR_IDENT && env_lookup(env, e->as.field_access.base->as.ident_name) == NULL) {
        Type ft;
        if (resolve_function_reference(module, e, &ft)) return ft;
    }
    Type base_type = check_expr(module, env, e->as.field_access.base);
    if (base_type.kind != TY_STRUCT) {
        ziman_error("type", e->as.field_access.base->line, e->as.field_access.base->col,
                    "cannot access field '%s' on %s (not a struct)", e->as.field_access.field_name, type_name(base_type));
    }
    StructDecl *decl = find_struct(module, base_type.struct_name);
    for (int i = 0; i < decl->fields.count; i++) {
        if (strcmp(decl->fields.items[i].name, e->as.field_access.field_name) == 0) {
            return substitute_type(decl->fields.items[i].type, &decl->type_params, &base_type.type_args);
        }
    }
    ziman_error("type", e->line, e->col, "no field '%s' on struct '%s'", e->as.field_access.field_name, base_type.struct_name);
    return type_scalar(TY_VOID);
}

static Type check_with_expr(Module *module, Env *env, Expr *e) {
    Type base_type = check_expr(module, env, e->as.with_expr.base);
    if (base_type.kind != TY_STRUCT) {
        ziman_error("type", e->as.with_expr.base->line, e->as.with_expr.base->col,
                    "'with' requires a struct value, found %s", type_name(base_type));
    }
    StructDecl *decl = find_struct(module, base_type.struct_name);
    FieldInitList *overrides = &e->as.with_expr.overrides;

    for (int i = 0; i < overrides->count; i++) {
        Param *field = resolve_field_init(decl, overrides, i);
        Type field_type = substitute_type(field->type, &decl->type_params, &base_type.type_args);
        Type value_type = check_expr_expecting(module, env, &overrides->items[i].value, field_type);
        require_not_void(overrides->items[i].value, "as a field value");
        if (!type_equals(value_type, field_type)) {
            ziman_error("type", overrides->items[i].value->line, overrides->items[i].value->col,
                        "field '%s' of '%s' expects %s, found %s",
                        field->name, base_type.struct_name, type_name(field_type), type_name(value_type));
        }
    }
    /* Unlike a struct literal, fields left unmentioned aren't an error --
     * they're kept from `base`. */
    return base_type;
}

/* Whether `t` is *some* instantiation of the enum `decl` names, ignoring
 * type arguments -- used on a consumption call (`is_X`/`unwrap_X`),
 * where the concrete instantiation is read off the argument's own
 * already-known type rather than written at the call site (see ast.h's
 * enum_access comment), so a full type_equals (which would also compare
 * type_args) is the wrong check here. */
static int is_enum_instance(Type t, EnumDecl *decl) {
    return t.kind == TY_ENUM && strcmp(t.enum_name, decl->name) == 0;
}

/* `EnumName[<Args>].member[(args)]` -- construction if `member` names one
 * of the enum's own variants, or a consumption call if it matches the
 * compiler-generated `is_<variant>`/`unwrap_<variant>` naming convention
 * for one of them (mirroring Result's is_ok/unwrap/unwrap_err exactly,
 * just per-enum instead of hardcoded once). A variant name always starts
 * uppercase and these two prefixes are always lowercase, so there's no way
 * for a real variant to collide with the generated-function convention. */
static Type check_enum_access(Module *module, Env *env, Expr *e) {
    const char *enum_name = e->as.enum_access.enum_name;
    const char *member = e->as.enum_access.member_name;
    EnumDecl *decl = find_enum(module, enum_name);
    if (decl == NULL) {
        ziman_error("type", e->line, e->col, "no enum named '%s'", enum_name);
    }
    TypeList *type_args = &e->as.enum_access.type_args;

    for (int i = 0; i < decl->variants.count; i++) {
        EnumVariant *v = &decl->variants.items[i];
        if (strcmp(v->name, member) != 0) continue;
        /* Construction: explicit type arguments everywhere, no
         * inference -- same rules as a generic struct literal. */
        if (decl->type_params.count == 0 && type_args->count > 0) {
            ziman_error("type", e->line, e->col, "'%s' is not generic, but was given %d type argument(s)", enum_name, type_args->count);
        }
        if (decl->type_params.count > 0 && type_args->count == 0) {
            ziman_error("type", e->line, e->col, "'%s' is generic and requires explicit type arguments, e.g. '%s<...>.%s'", enum_name, enum_name, member);
        }
        if (decl->type_params.count != type_args->count) {
            ziman_error("type", e->line, e->col, "'%s' takes %d type argument(s), found %d", enum_name, decl->type_params.count, type_args->count);
        }
        for (int ai = 0; ai < type_args->count; ai++) {
            check_type_well_formed(module, type_args->items[ai], e->line, e->col);
        }
        check_type_args_satisfy_bounds(module, &decl->type_params, type_args, e->line, e->col);

        if (v->has_payload) {
            if (!e->as.enum_access.has_parens || e->as.enum_access.args.count != 1) {
                ziman_error("type", e->line, e->col,
                            "variant '%s.%s' takes exactly one payload value, e.g. %s.%s(...)",
                            enum_name, member, enum_name, member);
            }
            Expr *arg = e->as.enum_access.args.items[0];
            Type arg_type = check_expr(module, env, arg);
            require_not_void(arg, "as a variant payload");
            Type payload_type = substitute_type(v->payload_type, &decl->type_params, type_args);
            if (!type_equals(arg_type, payload_type)) {
                ziman_error("type", arg->line, arg->col, "variant '%s.%s' expects %s, found %s",
                            enum_name, member, type_name(payload_type), type_name(arg_type));
            }
        } else if (e->as.enum_access.has_parens) {
            ziman_error("type", e->line, e->col,
                        "variant '%s.%s' takes no payload -- write '%s.%s' without parentheses",
                        enum_name, member, enum_name, member);
        }
        Type result = type_enum(decl->name);
        result.type_args = *type_args;
        return result;
    }

    static const char *IS_PREFIX = "is_";
    static const char *UNWRAP_PREFIX = "unwrap_";
    /* Consumption (is_X/unwrap_X): no type arguments at the call site --
     * the concrete instantiation comes from the argument's own already-
     * known type instead. Checked only once `member` actually looks like
     * a consumption call, so a genuinely unknown member (`Option.Nope`)
     * still falls through to the clearer "no variant or function" error
     * below instead of this one. */
    if (type_args->count > 0 &&
        (strncmp(member, IS_PREFIX, strlen(IS_PREFIX)) == 0 || strncmp(member, UNWRAP_PREFIX, strlen(UNWRAP_PREFIX)) == 0)) {
        ziman_error("type", e->line, e->col,
                    "'%s.%s' takes no type arguments -- they're inferred from the argument's own type", enum_name, member);
    }

    if (strncmp(member, IS_PREFIX, strlen(IS_PREFIX)) == 0) {
        const char *variant_part = member + strlen(IS_PREFIX);
        for (int i = 0; i < decl->variants.count; i++) {
            if (strcmp(decl->variants.items[i].name, variant_part) != 0) continue;
            if (e->as.enum_access.args.count != 1) {
                ziman_error("type", e->line, e->col, "'%s.%s' takes exactly 1 argument, found %d",
                            enum_name, member, e->as.enum_access.args.count);
            }
            Expr *arg = e->as.enum_access.args.items[0];
            Type arg_type = check_expr(module, env, arg);
            if (!is_enum_instance(arg_type, decl)) {
                ziman_error("type", arg->line, arg->col, "'%s.%s' expects a %s, found %s",
                            enum_name, member, decl->name, type_name(arg_type));
            }
            return type_scalar(TY_BOOL);
        }
    }
    if (strncmp(member, UNWRAP_PREFIX, strlen(UNWRAP_PREFIX)) == 0) {
        const char *variant_part = member + strlen(UNWRAP_PREFIX);
        for (int i = 0; i < decl->variants.count; i++) {
            EnumVariant *v = &decl->variants.items[i];
            if (strcmp(v->name, variant_part) != 0) continue;
            if (!v->has_payload) {
                ziman_error("type", e->line, e->col, "variant '%s.%s' has no payload to unwrap", enum_name, v->name);
            }
            if (e->as.enum_access.args.count != 1) {
                ziman_error("type", e->line, e->col, "'%s.%s' takes exactly 1 argument, found %d",
                            enum_name, member, e->as.enum_access.args.count);
            }
            Expr *arg = e->as.enum_access.args.items[0];
            Type arg_type = check_expr(module, env, arg);
            if (!is_enum_instance(arg_type, decl)) {
                ziman_error("type", arg->line, arg->col, "'%s.%s' expects a %s, found %s",
                            enum_name, member, decl->name, type_name(arg_type));
            }
            return substitute_type(v->payload_type, &decl->type_params, &arg_type.type_args);
        }
    }

    ziman_error("type", e->line, e->col, "enum '%s' has no variant or function '%s'", enum_name, member);
    return type_scalar(TY_VOID);
}

static Type check_expr(Module *module, Env *env, Expr *e) {
    Type t;
    switch (e->kind) {
        case EXPR_INT: t = type_scalar(TY_INT); break;
        case EXPR_FLOAT: t = type_scalar(TY_FLOAT); break;
        case EXPR_STRING: t = type_scalar(TY_STRING); break;
        case EXPR_BOOL: t = type_scalar(TY_BOOL); break;
        case EXPR_BIGINT: t = type_scalar(TY_BIGINT); break;
        case EXPR_DECIMAL: t = type_scalar(TY_DECIMAL); break;
        case EXPR_BYTE: t = type_scalar(TY_BYTE); break;
        case EXPR_IDENT: {
            Binding *b = env_lookup(env, e->as.ident_name);
            if (b == NULL) {
                Type ft;
                if (resolve_function_reference(module, e, &ft)) {
                    t = ft;
                    break;
                }
                for (size_t k = 0; k < sizeof BUILTINS_NO_NAMED_ARGS / sizeof BUILTINS_NO_NAMED_ARGS[0] && BUILTINS_NO_NAMED_ARGS[k] != NULL; k++) {
                    if (strcmp(e->as.ident_name, BUILTINS_NO_NAMED_ARGS[k]) == 0) {
                        ziman_error("type", e->line, e->col,
                                    "'%s' is a builtin, not a function value -- wrap it in a function of your own to pass it around", e->as.ident_name);
                    }
                }
                ziman_error("type", e->line, e->col, "use of undefined variable '%s'", e->as.ident_name);
            }
            t = b->type;
            break;
        }
        case EXPR_UNARY: t = check_unary(module, env, e); break;
        case EXPR_BINARY: t = check_binary(module, env, e); break;
        case EXPR_CALL: t = check_call(module, env, e); break;
        case EXPR_CAST: t = check_cast(module, env, e); break;
        case EXPR_ARRAY_LIT: t = check_array_literal(module, env, e); break;
        case EXPR_INDEX: t = check_index(module, env, e); break;
        case EXPR_STRUCT_LIT: t = check_struct_literal(module, env, e); break;
        case EXPR_FIELD_ACCESS: t = check_field_access(module, env, e); break;
        case EXPR_WITH: t = check_with_expr(module, env, e); break;
        case EXPR_ENUM_ACCESS: t = check_enum_access(module, env, e); break;
        case EXPR_TRY_UNWRAP:
            /* Reachable only when a `?` wasn't already intercepted by
             * one of the two statement forms it's legal in (a val/var's
             * entire initializer, or its own bare statement) -- see
             * check_try_unwrap and its two callers. */
            ziman_error("type", e->line, e->col,
                        "'?' can only be used as a val/var's entire initializer or as its own statement, not nested inside another expression");
            t = type_scalar(TY_VOID);
            break;
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

/* `defer f(args)`: type-checks the call exactly like an expression
 * statement (so named args, Option/Result auto-wrap, generics and
 * variadics all work), discards its result -- any type, a Result
 * included, which is the documented cost of a deferred cleanup call
 * having nowhere to report failure -- and builds the "replay" call the
 * interpreter runs at block exit.
 *
 * check_call has by now finalised the argument list (reordered named
 * arguments, inserted auto-wraps and defaults for omitted parameters),
 * so `call`'s argument nodes are the real ones. The interpreter
 * evaluates each into a hidden `$defer<N>$<i>` variable when the `defer`
 * statement runs; the replay call is a copy of `call` whose arguments
 * are just references to those variables -- which is why arguments
 * carry their `defer`-time values rather than whatever a `var` holds
 * later.
 *
 * A few hardcoded builtins can't be replayed this way because they look
 * at their argument *expressions* rather than just values: `format`
 * needs a string-literal template, `ok`/`err`/`channel_new` take their
 * type from context. They make no sense as a deferred cleanup anyway. */
static int g_defer_counter = 0;

static void check_defer(Module *module, Env *env, Stmt *s) {
    Expr *call = s->as.defer_stmt.call;
    static const char *const UNDEFERRABLE[] = {"format", "ok", "err", "channel_new"};
    for (size_t i = 0; i < sizeof(UNDEFERRABLE) / sizeof(UNDEFERRABLE[0]); i++) {
        if (strcmp(call->as.call.callee, UNDEFERRABLE[i]) == 0) {
            ziman_error("type", s->line, s->col, "'%s' cannot be deferred", UNDEFERRABLE[i]);
        }
    }
    check_expr(module, env, call);

    int n = call->as.call.args.count;
    int id = g_defer_counter++;
    Expr *replay = expr_new(EXPR_CALL, call->line, call->col);
    *replay = *call;
    replay->as.call.args.items = NULL;
    replay->as.call.args.count = 0;
    replay->as.call.args.capacity = 0;
    char **hidden = malloc(sizeof(char *) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        char name[48];
        snprintf(name, sizeof(name), "$defer%d$%d", id, i);
        hidden[i] = strdup(name);
        Expr *ref = expr_new(EXPR_IDENT, call->line, call->col);
        ref->as.ident_name = hidden[i];
        ref->resolved_type = call->as.call.args.items[i]->resolved_type;
        expr_list_push(&replay->as.call.args, ref);
    }
    s->as.defer_stmt.replay = replay;
    s->as.defer_stmt.hidden = hidden;
    s->as.defer_stmt.hidden_count = n;
}

static void check_stmt(Module *module, Env *env, Stmt *s, Type fn_return_type, int in_loop);

/* Type-checks `e` (always exactly an EXPR_TRY_UNWRAP node -- `inner?`)
 * against the enclosing function's own `fn_return_type`. The only two
 * legal call sites are STMT_VAL/STMT_VAR (binding the unwrapped value
 * to a name) and STMT_EXPR (a bare `expr?`, discarding it); any other
 * position instead reaches check_expr's own EXPR_TRY_UNWRAP case,
 * which always errors -- this function is never reached from there.
 *
 * `inner`'s type must be exactly `Option<T>` or `Result<T, E>`.
 *   - Option<T>: `fn_return_type` must be `Option<T2>` for *any* T2 --
 *     `None` carries no value to propagate, so T and T2 need no
 *     relationship at all.
 *   - Result<T, E>: `fn_return_type` must be `Result<T2, E>` with the
 *     *same* E -- `Err(e)` carries a real value that has to match.
 *
 * Returns the unwrapped type (T) -- what a val/var binds to, or the
 * (discarded) type of a bare `expr?` statement. Also fills in
 * `e->as.try_unwrap.propagated_type` with `fn_return_type` itself,
 * cached here because the interpreter needs the full, concrete
 * propagated type to build the runtime None/Err value and has no other
 * way to learn it (NativeFn-style context-passing doesn't apply to an
 * ordinary AST node) -- see ast.h's own comment on that field. */
static Type check_try_unwrap(Module *module, Env *env, Expr *e, Type fn_return_type) {
    Expr *inner = e->as.try_unwrap.inner;
    Type inner_type = check_expr(module, env, inner);
    require_not_void(inner, "with '?'");

    if (is_option_type(inner_type)) {
        if (!is_option_type(fn_return_type)) {
            ziman_error("type", e->line, e->col,
                        "'?' on an Option<T> value requires the enclosing function to return Option<...>, found %s",
                        type_name(fn_return_type));
        }
        e->as.try_unwrap.propagated_type = fn_return_type;
        Type t = option_elem_type(inner_type);
        e->resolved_type = t;
        return t;
    }
    if (inner_type.kind == TY_RESULT) {
        if (fn_return_type.kind != TY_RESULT || !type_equals(*inner_type.elem_err, *fn_return_type.elem_err)) {
            ziman_error("type", e->line, e->col,
                        "'?' on a Result<T, E> value requires the enclosing function to return Result<..., %s>, found %s",
                        type_name(*inner_type.elem_err), type_name(fn_return_type));
        }
        e->as.try_unwrap.propagated_type = fn_return_type;
        Type t = *inner_type.elem;
        e->resolved_type = t;
        return t;
    }
    ziman_error("type", inner->line, inner->col,
                "'?' can only be used on an Option<T> or Result<T, E> value, found %s", type_name(inner_type));
    return type_scalar(TY_VOID); /* unreachable */
}

/* Runs `block`'s statements directly in `scope` (no further scope nesting) --
 * shared by check_block (which makes its own fresh scope first) and
 * STMT_FOR (which needs its loop binder(s) defined in that same scope,
 * ahead of the body's own statements). `in_loop` is true whenever these
 * statements execute somewhere inside a while/for body -- threaded through
 * so a `break`/`continue` deeper inside (e.g. under an `if`) can still be
 * validated without re-deriving it from scratch. */
static void check_stmts_in_scope(Module *module, Env *scope, Stmt *block, Type fn_return_type, int in_loop) {
    for (int i = 0; i < block->as.block.stmts.count; i++) {
        check_stmt(module, scope, block->as.block.stmts.items[i], fn_return_type, in_loop);
    }
}

static void check_block(Module *module, Env *parent, Stmt *block, Type fn_return_type, int in_loop) {
    Env *scope = env_new(parent);
    check_stmts_in_scope(module, scope, block, fn_return_type, in_loop);
    env_free(scope);
}

/* ---- switch: exhaustive consumption of an enum or a Result ----
 *
 * `switch e { A(x) { ... } B { ... } }` is checked and then rewritten in
 * place into `{ val $swN = e; if Enum.is_A($swN) { val x = Enum.unwrap_A($swN); ... }
 * else { ... } }` -- the same is_X/unwrap_X calls a person could write by
 * hand, so generics, payload typing and the "does every path return" check
 * all come from machinery that already exists. What `switch` adds is the
 * exhaustiveness rule: every variant exactly once, none forgotten, so adding
 * a variant to an enum is a compile error at every switch over it. There is
 * deliberately no default arm -- that would reintroduce the silent case. */
static Expr *mk_ident_expr(const char *name, int line, int col) {
    Expr *e = expr_new(EXPR_IDENT, line, col);
    e->as.ident_name = strdup(name);
    return e;
}

static Expr *mk_enum_call_expr(const char *enum_name, const char *prefix, const char *variant, Expr *arg, int line, int col) {
    Expr *e = expr_new(EXPR_ENUM_ACCESS, line, col);
    e->as.enum_access.enum_name = strdup(enum_name);
    size_t n = strlen(prefix) + strlen(variant) + 1;
    e->as.enum_access.member_name = malloc(n);
    snprintf(e->as.enum_access.member_name, n, "%s%s", prefix, variant);
    expr_list_push(&e->as.enum_access.args, arg);
    e->as.enum_access.has_parens = 1;
    return e;
}

static Expr *mk_call_expr(const char *callee, Expr *arg, int line, int col) {
    Expr *e = expr_new(EXPR_CALL, line, col);
    e->as.call.callee = strdup(callee);
    expr_list_push(&e->as.call.args, arg);
    return e;
}

/* An arm's block with `val <binder> = <payload_expr>` put first. */
static void prepend_binder(Stmt *body, const char *binder, Expr *payload_expr, int line, int col) {
    Stmt *decl = stmt_new(STMT_VAL, line, col);
    decl->as.decl.name = strdup(binder);
    decl->as.decl.init = payload_expr;
    StmtList old = body->as.block.stmts;
    memset(&body->as.block.stmts, 0, sizeof body->as.block.stmts);
    stmt_list_push(&body->as.block.stmts, decl);
    for (int i = 0; i < old.count; i++) stmt_list_push(&body->as.block.stmts, old.items[i]);
}

static void check_switch(Module *module, Env *env, Stmt *s, Type fn_return_type, int in_loop) {
    static int switch_counter = 0;
    Expr *scrutinee = s->as.switch_stmt.scrutinee;
    SwitchArm *arms = s->as.switch_stmt.arms;
    int arm_count = s->as.switch_stmt.arm_count;

    char *hidden = malloc(32);
    snprintf(hidden, 32, "$sw%d", switch_counter++);
    Env *scope = env_new(env);
    Stmt *decl = stmt_new(STMT_VAL, s->line, s->col);
    decl->as.decl.name = hidden;
    decl->as.decl.init = scrutinee;
    check_stmt(module, scope, decl, fn_return_type, in_loop);
    Type t = env_lookup(scope, hidden)->type;

    EnumDecl *enum_decl = NULL;
    const char *const result_variants[2] = {"Ok", "Err"};
    int variant_count;
    if (t.kind == TY_ENUM) {
        enum_decl = find_enum(module, t.enum_name);
        if (enum_decl == NULL) ziman_error("type", scrutinee->line, scrutinee->col, "no enum named '%s'", t.enum_name);
        variant_count = enum_decl->variants.count;
    } else if (t.kind == TY_RESULT) {
        variant_count = 2;
    } else {
        ziman_error("type", scrutinee->line, scrutinee->col, "'switch' works on an enum or a Result, found %s", type_name(t));
        return;
    }

    int *seen = calloc((size_t)variant_count, sizeof(int));
    int *arm_variant = malloc(sizeof(int) * (size_t)arm_count);
    for (int a = 0; a < arm_count; a++) {
        SwitchArm *arm = &arms[a];
        int vi = -1;
        int has_payload = 1;
        for (int v = 0; v < variant_count; v++) {
            const char *name = enum_decl != NULL ? enum_decl->variants.items[v].name : result_variants[v];
            if (strcmp(name, arm->variant) == 0) {
                vi = v;
                has_payload = enum_decl != NULL ? enum_decl->variants.items[v].has_payload : 1;
                break;
            }
        }
        if (vi < 0) {
            char list[512] = "";
            for (int v = 0; v < variant_count; v++) {
                const char *name = enum_decl != NULL ? enum_decl->variants.items[v].name : result_variants[v];
                if (strlen(list) + strlen(name) + 3 < sizeof list) {
                    if (v > 0) strcat(list, ", ");
                    strcat(list, name);
                }
            }
            ziman_error("type", arm->line, arm->col, "%s has no variant '%s' (its variants: %s)", type_name(t), arm->variant, list);
        }
        if (seen[vi]) {
            ziman_error("type", arm->line, arm->col, "this 'switch' already has an arm for '%s'", arm->variant);
        }
        seen[vi] = 1;
        arm_variant[a] = vi;
        if (has_payload && arm->binder == NULL) {
            ziman_error("type", arm->line, arm->col, "variant '%s' carries a value: write '%s(name) { ... }' to bind it", arm->variant, arm->variant);
        }
        if (!has_payload && arm->binder != NULL) {
            ziman_error("type", arm->line, arm->col, "variant '%s' carries no value: write '%s { ... }' without a name", arm->variant, arm->variant);
        }
    }
    char missing[512] = "";
    for (int v = 0; v < variant_count; v++) {
        if (seen[v]) continue;
        const char *name = enum_decl != NULL ? enum_decl->variants.items[v].name : result_variants[v];
        if (strlen(missing) + strlen(name) + 3 < sizeof missing) {
            if (missing[0] != '\0') strcat(missing, ", ");
            strcat(missing, name);
        }
    }
    if (missing[0] != '\0') {
        ziman_error("type", s->line, s->col,
                    "this 'switch' over %s is not exhaustive: missing %s (add an arm for each variant; there is no default arm on purpose, so a new variant is a compile error here)",
                    type_name(t), missing);
    }

    /* Build: if <arm0> { ... } else if <arm1> { ... } else { <last arm> } */
    Stmt *chain = NULL;
    for (int a = arm_count - 1; a >= 0; a--) {
        SwitchArm *arm = &arms[a];
        int is_result = enum_decl == NULL;
        if (arm->binder != NULL) {
            Expr *payload;
            if (is_result) {
                payload = mk_call_expr(strcmp(arm->variant, "Ok") == 0 ? "unwrap" : "unwrap_err", mk_ident_expr(hidden, arm->line, arm->col), arm->line, arm->col);
            } else {
                payload = mk_enum_call_expr(t.enum_name, "unwrap_", arm->variant, mk_ident_expr(hidden, arm->line, arm->col), arm->line, arm->col);
            }
            prepend_binder(arm->body, arm->binder, payload, arm->line, arm->col);
        }
        if (a == arm_count - 1) {
            chain = arm->body;
            continue;
        }
        Stmt *ifs = stmt_new(STMT_IF, arm->line, arm->col);
        Expr *cond;
        if (is_result) {
            cond = mk_call_expr("is_ok", mk_ident_expr(hidden, arm->line, arm->col), arm->line, arm->col);
            if (strcmp(arm->variant, "Err") == 0) {
                Expr *neg = expr_new(EXPR_UNARY, arm->line, arm->col);
                neg->as.unary.op = OP_NOT;
                neg->as.unary.operand = cond;
                cond = neg;
            }
        } else {
            cond = mk_enum_call_expr(t.enum_name, "is_", arm->variant, mk_ident_expr(hidden, arm->line, arm->col), arm->line, arm->col);
        }
        ifs->as.if_stmt.cond = cond;
        ifs->as.if_stmt.then_block = arm->body;
        ifs->as.if_stmt.else_block = chain;
        chain = ifs;
    }
    check_stmt(module, scope, chain, fn_return_type, in_loop);
    env_free(scope);

    free(seen);
    free(arm_variant);
    memset(&s->as, 0, sizeof s->as);
    s->kind = STMT_BLOCK;
    stmt_list_push(&s->as.block.stmts, decl);
    stmt_list_push(&s->as.block.stmts, chain);
}

static void check_stmt(Module *module, Env *env, Stmt *s, Type fn_return_type, int in_loop) {
    switch (s->kind) {
        case STMT_SWITCH:
            check_switch(module, env, s, fn_return_type, in_loop);
            break;
        case STMT_VAL:
        case STMT_VAR: {
            Expr *init = s->as.decl.init;
            Type declared;
            if (s->as.decl.has_declared_type) {
                check_type_well_formed(module, s->as.decl.declared_type, s->line, s->col);
            }
            /* `val|var NAME = inner?` -- checked directly against
             * `fn_return_type`, not the declared type (there usually
             * isn't one; the unwrapped type is already fully determined
             * by `inner`'s own Option<T>/Result<T, E> type, with
             * nothing left to infer or disambiguate). A declared type
             * annotation is still allowed, and still checked against
             * the unwrapped type, exactly like the plain fallback below
             * already does for an ordinary initializer. */
            if (init->kind == EXPR_TRY_UNWRAP) {
                Type unwrapped = check_try_unwrap(module, env, init, fn_return_type);
                declared = unwrapped;
                if (s->as.decl.has_declared_type) {
                    declared = s->as.decl.declared_type;
                    if (!type_equals(declared, unwrapped)) {
                        ziman_error("type", init->line, init->col,
                                    "cannot initialize '%s' of declared type %s with a value of type %s",
                                    s->as.decl.name, type_name(declared), type_name(unwrapped));
                    }
                }
            } else if (init->kind == EXPR_ARRAY_LIT && init->as.array_lit.elements.count == 0) {
                /* An empty array literal (`[]`) has no elements to infer
                 * a type from -- it's only legal when a declared type
                 * gives its element type explicitly, checked here
                 * rather than inside the generic check_expr, which has
                 * no such context to work from. */
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
            } else if (s->as.decl.has_declared_type && check_ok_err_call(module, env, init, s->as.decl.declared_type)) {
                /* ok(...)/err(...) resolved against the declared type -- see
                 * check_ok_err_call's own comment for why this needs its own
                 * path instead of going through plain check_expr. */
                declared = s->as.decl.declared_type;
            } else if (s->as.decl.has_declared_type && check_channel_new_call(init, s->as.decl.declared_type)) {
                /* channel_new() resolved against the declared type -- same
                 * reasoning as ok(...)/err(...) above. */
                declared = s->as.decl.declared_type;
            } else if (s->as.decl.has_declared_type && (is_option_type(s->as.decl.declared_type) || s->as.decl.declared_type.kind == TY_RESULT)) {
                /* A bare Option<T>/Result<T, E> auto-wrap against the
                 * declared type -- same reasoning as ok(...)/err(...)
                 * above, via check_value_against_param_type directly
                 * (passing &s->as.decl.init, not the local `init` copy,
                 * so a successful wrap persists into the AST the
                 * interpreter later evaluates). */
                Type arg_type;
                if (check_value_against_param_type(module, env, &s->as.decl.init, s->as.decl.declared_type, &arg_type)) {
                    declared = s->as.decl.declared_type;
                } else {
                    ziman_error("type", init->line, init->col,
                                "cannot initialize '%s' of declared type %s with a value of type %s",
                                s->as.decl.name, type_name(s->as.decl.declared_type), type_name(arg_type));
                }
            } else {
                Type init_type = check_expr(module, env, init);
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
            Type value_type = check_expr(module, env, s->as.assign.value);
            require_not_void(s->as.assign.value, "in an assignment");
            if (!type_equals(value_type, b->type)) {
                ziman_error("type", s->as.assign.value->line, s->as.assign.value->col,
                            "cannot assign a value of type %s to '%s' (declared as %s)",
                            type_name(value_type), s->as.assign.name, type_name(b->type));
            }
            break;
        }
        case STMT_IF: {
            Type cond_type = check_expr(module, env, s->as.if_stmt.cond);
            if (cond_type.kind != TY_BOOL) {
                ziman_error("type", s->as.if_stmt.cond->line, s->as.if_stmt.cond->col, "'if' condition must be bool, found %s", type_name(cond_type));
            }
            check_block(module, env, s->as.if_stmt.then_block, fn_return_type, in_loop);
            if (s->as.if_stmt.else_block != NULL) {
                if (s->as.if_stmt.else_block->kind == STMT_IF) {
                    check_stmt(module, env, s->as.if_stmt.else_block, fn_return_type, in_loop);
                } else {
                    check_block(module, env, s->as.if_stmt.else_block, fn_return_type, in_loop);
                }
            }
            break;
        }
        case STMT_WHILE: {
            Type cond_type = check_expr(module, env, s->as.while_stmt.cond);
            if (cond_type.kind != TY_BOOL) {
                ziman_error("type", s->as.while_stmt.cond->line, s->as.while_stmt.cond->col, "'while' condition must be bool, found %s", type_name(cond_type));
            }
            check_block(module, env, s->as.while_stmt.body, fn_return_type, 1);
            break;
        }
        case STMT_FOR: {
            Env *scope = env_new(env);
            if (s->as.for_stmt.is_range) {
                Type start_type = check_expr(module, env, s->as.for_stmt.range_start);
                if (start_type.kind != TY_INT) {
                    ziman_error("type", s->as.for_stmt.range_start->line, s->as.for_stmt.range_start->col,
                                "'for' range start must be int, found %s", type_name(start_type));
                }
                Type end_type = check_expr(module, env, s->as.for_stmt.range_end);
                if (end_type.kind != TY_INT) {
                    ziman_error("type", s->as.for_stmt.range_end->line, s->as.for_stmt.range_end->col,
                                "'for' range end must be int, found %s", type_name(end_type));
                }
                env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(0));
            } else {
                Type iter_type = check_expr(module, env, s->as.for_stmt.iterable);
                if (iter_type.kind != TY_ARRAY) {
                    ziman_error("type", s->as.for_stmt.iterable->line, s->as.for_stmt.iterable->col,
                                "'for' can only iterate an array or a range, found %s", type_name(iter_type));
                }
                if (s->as.for_stmt.binder2 == NULL) {
                    env_define(scope, s->as.for_stmt.binder1, *iter_type.elem, 0, value_int(0));
                } else {
                    env_define(scope, s->as.for_stmt.binder1, type_scalar(TY_INT), 0, value_int(0));
                    env_define(scope, s->as.for_stmt.binder2, *iter_type.elem, 0, value_int(0));
                }
            }
            int binders_pushed_from = g_loop_binder_count;
            if (s->as.for_stmt.is_range || s->as.for_stmt.binder2 != NULL) {
                push_loop_binder(s->as.for_stmt.binder1, NULL);
                if (s->as.for_stmt.binder2 != NULL) push_loop_binder(s->as.for_stmt.binder2, NULL);
            } else {
                Expr *iterable = s->as.for_stmt.iterable;
                push_loop_binder(s->as.for_stmt.binder1, iterable->kind == EXPR_IDENT ? iterable->as.ident_name : NULL);
            }
            check_stmts_in_scope(module, scope, s->as.for_stmt.body, fn_return_type, 1);
            g_loop_binder_count = binders_pushed_from;
            env_free(scope);
            break;
        }
        case STMT_BREAK:
        case STMT_CONTINUE:
            if (!in_loop) {
                ziman_error("type", s->line, s->col, "'%s' outside of a loop", s->kind == STMT_BREAK ? "break" : "continue");
            }
            break;
        case STMT_SPAWN: {
            const char *callee = s->as.spawn_stmt.callee;
            FunctionDecl *fn = find_function(module, callee);
            if (fn == NULL) {
                ziman_error("type", s->line, s->col, "call to undefined function '%s'", callee);
            }
            if (fn->return_type.kind != TY_VOID) {
                ziman_error("type", s->line, s->col,
                            "'spawn' requires a function that returns nothing, found %s returning %s "
                            "(send a result back through a Channel instead)",
                            callee, type_name(fn->return_type));
            }
            TypeList *spawn_type_args = &s->as.spawn_stmt.type_args;
            if (fn->type_params.count == 0 && spawn_type_args->count > 0) {
                ziman_error("type", s->line, s->col, "'%s' is not generic, but was given %d type argument(s)", callee, spawn_type_args->count);
            }
            if (fn->type_params.count > 0) {
                if (spawn_type_args->count != fn->type_params.count) {
                    ziman_error("type", s->line, s->col, "'%s' is generic and takes %d type argument(s), written out: spawn %s<...>(...); found %d",
                                callee, fn->type_params.count, callee, spawn_type_args->count);
                }
                for (int ti = 0; ti < spawn_type_args->count; ti++) check_type_well_formed(module, spawn_type_args->items[ti], s->line, s->col);
                check_type_args_satisfy_bounds(module, &fn->type_params, spawn_type_args, s->line, s->col);
            }
            if (s->as.spawn_stmt.args.count != fn->params.count) {
                ziman_error("type", s->line, s->col, "function '%s' expects %d argument(s), found %d",
                            callee, fn->params.count, s->as.spawn_stmt.args.count);
            }
            for (int i = 0; i < s->as.spawn_stmt.args.count; i++) {
                Expr *arg = s->as.spawn_stmt.args.items[i];
                Type arg_type = check_expr(module, env, arg);
                require_not_void(arg, "as an argument");
                Type expected = fn->params.items[i].type;
                if (fn->type_params.count > 0) expected = substitute_type(expected, &fn->type_params, spawn_type_args);
                if (!type_equals(arg_type, expected)) {
                    ziman_error("type", arg->line, arg->col,
                                "argument %d of '%s' expects %s, found %s",
                                i + 1, callee, type_name(expected), type_name(arg_type));
                }
            }
            break;
        }
        case STMT_RETURN: {
            if (s->as.ret.value == NULL) {
                if (fn_return_type.kind != TY_VOID) {
                    ziman_error("type", s->line, s->col, "bare 'return' in a function declared to return %s", type_name(fn_return_type));
                }
            } else {
                /* check_expr_expecting resolves ok(...)/err(...)/
                 * channel_new()/an empty array literal/a bare Option<T>
                 * or Result<T, E> value against the function's own
                 * declared return type when the value is exactly one of
                 * those; falls back to plain check_expr otherwise. */
                Type value_type = check_expr_expecting(module, env, &s->as.ret.value, fn_return_type);
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
            /* A bare `inner?` statement -- checked the same way as
             * val/var's own `= inner?` form, just with the unwrapped
             * value discarded rather than bound to a name. */
            if (s->as.expr_stmt.expr->kind == EXPR_TRY_UNWRAP) {
                check_try_unwrap(module, env, s->as.expr_stmt.expr, fn_return_type);
            } else {
                check_expr(module, env, s->as.expr_stmt.expr);
            }
            break;
        }
        case STMT_DEFER:
            check_defer(module, env, s);
            break;
        case STMT_BLOCK:
            check_block(module, env, s, fn_return_type, in_loop);
            break;
    }
}

/* Recursively verifies that every struct/enum name mentioned by `t`
 * (including inside array element types) actually refers to a declared
 * one. Run over every struct field type, enum variant payload type, and
 * function param/return type before any function body is checked, so a
 * typo in a type reference is caught at one precise location instead of
 * cascading into confusing mismatches later (see typecheck()). A TY_STRUCT
 * here that doesn't resolve to a real struct might have actually meant an
 * enum -- the loader already reclassifies same-module references it can
 * tell apart (self_qualify_type), but this is still the first point that
 * can report a *genuinely* undefined name either way. TY_ENUM itself is
 * never produced by parse_type() directly (see ast.h's Type comment), so
 * reaching this function with one already means the loader resolved it. */
static void check_type_well_formed(Module *module, Type t, int line, int col) {
    if (t.kind == TY_ARRAY) {
        check_type_well_formed(module, *t.elem, line, col);
    } else if (t.kind == TY_RESULT) {
        check_type_well_formed(module, *t.elem, line, col);
        check_type_well_formed(module, *t.elem_err, line, col);
    } else if (t.kind == TY_STRUCT) {
        StructDecl *decl = find_struct(module, t.struct_name);
        if (decl == NULL) {
            ziman_error("type", line, col, "unknown struct or enum type '%s'", t.struct_name);
        }
        if (t.type_args.count != decl->type_params.count) {
            ziman_error("type", line, col, "'%s' takes %d type argument(s), found %d", t.struct_name, decl->type_params.count, t.type_args.count);
        }
        for (int i = 0; i < t.type_args.count; i++) {
            check_type_well_formed(module, t.type_args.items[i], line, col);
        }
        check_type_args_satisfy_bounds(module, &decl->type_params, &t.type_args, line, col);
    } else if (t.kind == TY_ENUM) {
        EnumDecl *decl = find_enum(module, t.enum_name);
        if (decl == NULL) {
            ziman_error("type", line, col, "unknown enum type '%s'", t.enum_name);
        }
        if (t.type_args.count != decl->type_params.count) {
            ziman_error("type", line, col, "'%s' takes %d type argument(s), found %d", t.enum_name, decl->type_params.count, t.type_args.count);
        }
        for (int i = 0; i < t.type_args.count; i++) {
            check_type_well_formed(module, t.type_args.items[i], line, col);
        }
        check_type_args_satisfy_bounds(module, &decl->type_params, &t.type_args, line, col);
    } else if (t.kind == TY_CHANNEL) {
        check_type_well_formed(module, *t.elem, line, col);
    } else if (t.kind == TY_FUNC) {
        for (int i = 0; i < t.type_args.count; i++) check_type_well_formed(module, t.type_args.items[i], line, col);
        check_type_well_formed(module, *t.elem, line, col);
    }
}

/* Type-checks every module-level `val` in file order (a later one may
 * reference an earlier one, same-module or already-`use`d, the same
 * "must already be declared" rule an ordinary local `val` enforces
 * within one function body -- see docs/DESIGN.md's "Global val" entry
 * for why this is a deliberate scope cut, not an oversight), building
 * `g_global_env` as it goes by reusing check_stmt's own STMT_VAL case
 * wholesale (via a synthetic Stmt) rather than re-deriving its declared-
 * vs-inferred type logic here. Must run before any check_function call,
 * since g_global_env is what every function's own scope is rooted in. */
static void check_global_vals(Module *module) {
    g_global_env = env_new(NULL);
    for (int i = 0; i < module->vals.count; i++) {
        GlobalValDecl *v = module->vals.items[i];
        Stmt synthetic = {0};
        synthetic.kind = STMT_VAL;
        synthetic.line = v->line;
        synthetic.col = v->col;
        synthetic.as.decl.name = v->name;
        synthetic.as.decl.declared_type = v->type;
        synthetic.as.decl.has_declared_type = v->has_declared_type;
        synthetic.as.decl.init = v->init;
        check_stmt(module, g_global_env, &synthetic, type_scalar(TY_VOID), 0);
    }
}

static void check_function(Module *module, FunctionDecl *fn) {
    Env *env = env_new(g_global_env);
    for (int i = 0; i < fn->params.count; i++) {
        Param *param = &fn->params.items[i];
        /* A variadic parameter's own declared type is the *element*
         * type (see ast.h's Param comment) -- inside the function's own
         * body it behaves as an ordinary array of that type, the same
         * value a caller's collected trailing arguments become at the
         * call site (see check_call/call_function). */
        Type bound_type = param->is_variadic ? type_array(param->type) : param->type;
        if (!env_define(env, param->name, bound_type, 0 /* params are immutable */, value_int(0))) {
            ziman_error("type", param->line, param->col, "duplicate parameter name '%s'", param->name);
        }
        /* A default value is only legal on an Option<T> parameter -- a
         * plain required parameter has no "absent" state a default
         * could ever stand in for, since it can never be omitted from a
         * call at all. Checked against g_global_env, not this
         * function's own `env` above: same restriction a global val's
         * own initializer already has (see docs/DESIGN.md's "Global
         * val" entry), since a default is sugar for one, evaluated once
         * before `main` runs -- long before any particular call, or any
         * of this function's *other* parameters, exist to reference. */
        if (param->default_expr != NULL) {
            if (!is_option_type(param->type)) {
                ziman_error("type", param->default_expr->line, param->default_expr->col,
                            "a default value is only legal for an Option<T> parameter (parameter '%s' is %s)",
                            param->name, type_name(param->type));
            }
            if (option_elem_type(param->type).kind == TY_TYPEVAR) {
                ziman_error("type", param->default_expr->line, param->default_expr->col,
                            "parameter '%s' can't have a default value -- its Option<T> isn't a concrete type yet here (T is this function's own generic type parameter, not known until a call instantiates it)",
                            param->name);
            }
            Type arg_type;
            if (!check_value_against_param_type(module, g_global_env, &param->default_expr, param->type, &arg_type)) {
                ziman_error("type", param->default_expr->line, param->default_expr->col,
                            "default value for parameter '%s' expects %s or %s, found %s",
                            param->name, type_name(param->type), type_name(option_elem_type(param->type)), type_name(arg_type));
            }
        }
    }
    /* Lets type_supports_equality's TY_TYPEVAR case (above) know whether a
     * bare, unsubstituted `T` referenced inside *this* function's own body
     * is bounded by `Eq` -- the one place a generic declaration's bound
     * actually gets exercised, rather than just enforced at instantiation
     * (see ROADMAP.md's "Generics" Stage 2/5). Set/cleared symmetrically
     * around this one call: sound as a plain variable rather than a
     * parameter threaded through every check_* function because the type
     * checker processes exactly one function body at a time, single-
     * threaded, with no nested function declarations to re-enter this
     * during. */
    current_decl_type_params = &fn->type_params;
    /* A native fn has no body to check or prove returns-on-every-path for --
     * its behavior is a contract with whatever C implementation gets
     * registered for it, not something this pass can see into. */
    if (!fn->is_native) {
        for (int i = 0; i < fn->body->as.block.stmts.count; i++) {
            check_stmt(module, env, fn->body->as.block.stmts.items[i], fn->return_type, 0);
        }
        if (fn->return_type.kind != TY_VOID && !stmt_always_returns(fn->body)) {
            ziman_error("type", fn->line, fn->col, "function '%s' does not return a value on every path", fn->name);
        }
    }
    current_decl_type_params = NULL;
    env_free(env);
}

void typecheck(Module *module) {
    /* Doesn't include parse_int/parse_float/parse_bigint/parse_decimal,
     * floor/ceil/round, decimal_round, concat, substring, split, trim,
     * to_upper, to_lower -- those moved to the prelude (see prelude.h),
     * so they're now ordinary FunctionDecls already merged into this
     * module's own function list, and the generic duplicate-function-
     * name check just below already rejects redefining one of those the
     * same way it rejects redefining any other function. */
    static const char *builtins[] = {"print", "str", "format", "len", "push", "insert", "set",
                                      "ok", "err", "is_ok", "unwrap", "unwrap_err",
                                      "contains", "abs", "min", "max", "remove",
                                      "channel_new", "send", "receive", "hash", "compare"};

    /* Struct names and field names within a struct must be unique --
     * checked before any field TYPE is validated, so every struct name a
     * field could legally reference is already known. */
    for (int i = 0; i < module->structs.count; i++) {
        StructDecl *sd = module->structs.items[i];
        for (int j = i + 1; j < module->structs.count; j++) {
            if (strcmp(sd->name, module->structs.items[j]->name) == 0) {
                ziman_error("type", module->structs.items[j]->line, module->structs.items[j]->col,
                            "struct '%s' is already defined", sd->name);
            }
        }
        for (int f = 0; f < sd->fields.count; f++) {
            for (int g = f + 1; g < sd->fields.count; g++) {
                if (strcmp(sd->fields.items[f].name, sd->fields.items[g].name) == 0) {
                    ziman_error("type", sd->fields.items[g].line, sd->fields.items[g].col,
                                "field '%s' is already defined in struct '%s'", sd->fields.items[g].name, sd->name);
                }
            }
        }
        /* A type parameter name colliding with a real struct/enum name
         * would make parse_struct_decl's reclassify_type_params silently
         * shadow every legitimate reference to that real type inside this
         * struct's own fields -- caught here instead of failing subtly
         * later. Duplicate parameter names have no per-name position (see
         * ast.h's TypeParamList), so both point at the struct's own line. */
        for (int tp = 0; tp < sd->type_params.count; tp++) {
            const char *param_name = sd->type_params.items[tp].name;
            for (int tq = tp + 1; tq < sd->type_params.count; tq++) {
                if (strcmp(param_name, sd->type_params.items[tq].name) == 0) {
                    ziman_error("type", sd->line, sd->col, "type parameter '%s' is already defined in struct '%s'", param_name, sd->name);
                }
            }
            if (find_struct(module, param_name) != NULL) {
                ziman_error("type", sd->line, sd->col, "type parameter '%s' in struct '%s' collides with an existing struct name", param_name, sd->name);
            }
            if (find_enum(module, param_name) != NULL) {
                ziman_error("type", sd->line, sd->col, "type parameter '%s' in struct '%s' collides with an existing enum name", param_name, sd->name);
            }
        }
    }
    for (int i = 0; i < module->structs.count; i++) {
        StructDecl *sd = module->structs.items[i];
        current_decl_type_params = &sd->type_params;
        for (int f = 0; f < sd->fields.count; f++) {
            Param *field = &sd->fields.items[f];
            check_type_well_formed(module, field->type, field->line, field->col);
        }
        current_decl_type_params = NULL;
    }

    /* Enum names must be unique among themselves and distinct from struct
     * names (both live in the same "type name" namespace conceptually,
     * even though tracked as two separate lists) -- checked before any
     * variant payload type is validated. Variant names only need to be
     * unique *within* their own enum, never globally: access is always
     * enum-qualified (`Shape.Circle`, never a bare `Circle`), so two
     * different enums may freely reuse the same variant name. */
    for (int i = 0; i < module->enums.count; i++) {
        EnumDecl *ed = module->enums.items[i];
        for (int j = i + 1; j < module->enums.count; j++) {
            if (strcmp(ed->name, module->enums.items[j]->name) == 0) {
                ziman_error("type", module->enums.items[j]->line, module->enums.items[j]->col,
                            "enum '%s' is already defined", ed->name);
            }
        }
        for (int s = 0; s < module->structs.count; s++) {
            if (strcmp(ed->name, module->structs.items[s]->name) == 0) {
                ziman_error("type", ed->line, ed->col, "'%s' is already defined as a struct", ed->name);
            }
        }
        for (int v = 0; v < ed->variants.count; v++) {
            for (int w = v + 1; w < ed->variants.count; w++) {
                if (strcmp(ed->variants.items[v].name, ed->variants.items[w].name) == 0) {
                    ziman_error("type", ed->variants.items[w].line, ed->variants.items[w].col,
                                "variant '%s' is already defined in enum '%s'", ed->variants.items[w].name, ed->name);
                }
            }
        }
        /* Same reasoning as the struct type-parameter checks above. */
        for (int tp = 0; tp < ed->type_params.count; tp++) {
            const char *param_name = ed->type_params.items[tp].name;
            for (int tq = tp + 1; tq < ed->type_params.count; tq++) {
                if (strcmp(param_name, ed->type_params.items[tq].name) == 0) {
                    ziman_error("type", ed->line, ed->col, "type parameter '%s' is already defined in enum '%s'", param_name, ed->name);
                }
            }
            if (find_struct(module, param_name) != NULL) {
                ziman_error("type", ed->line, ed->col, "type parameter '%s' in enum '%s' collides with an existing struct name", param_name, ed->name);
            }
            if (find_enum(module, param_name) != NULL) {
                ziman_error("type", ed->line, ed->col, "type parameter '%s' in enum '%s' collides with an existing enum name", param_name, ed->name);
            }
        }
    }
    for (int i = 0; i < module->enums.count; i++) {
        EnumDecl *ed = module->enums.items[i];
        current_decl_type_params = &ed->type_params;
        for (int v = 0; v < ed->variants.count; v++) {
            if (ed->variants.items[v].has_payload) {
                check_type_well_formed(module, ed->variants.items[v].payload_type,
                                        ed->variants.items[v].line, ed->variants.items[v].col);
            }
        }
        current_decl_type_params = NULL;
    }

    for (int i = 0; i < module->functions.count; i++) {
        FunctionDecl *fn = module->functions.items[i];
        ziman_set_error_context(fn->derived_from);
        for (size_t b = 0; b < sizeof(builtins) / sizeof(builtins[0]); b++) {
            if (strcmp(fn->name, builtins[b]) == 0) {
                ziman_error("type", fn->line, fn->col, "'%s' is a builtin function and cannot be redefined", fn->name);
            }
        }
        for (int j = i + 1; j < module->functions.count; j++) {
            if (strcmp(fn->name, module->functions.items[j]->name) == 0) {
                ziman_error("type", module->functions.items[j]->line, module->functions.items[j]->col,
                            "function '%s' is already defined", fn->name);
            }
        }
        /* Same reasoning as the struct/enum type-parameter checks above. */
        for (int tp = 0; tp < fn->type_params.count; tp++) {
            const char *param_name = fn->type_params.items[tp].name;
            for (int tq = tp + 1; tq < fn->type_params.count; tq++) {
                if (strcmp(param_name, fn->type_params.items[tq].name) == 0) {
                    ziman_error("type", fn->line, fn->col, "type parameter '%s' is already defined in function '%s'", param_name, fn->name);
                }
            }
            if (find_struct(module, param_name) != NULL) {
                ziman_error("type", fn->line, fn->col, "type parameter '%s' in function '%s' collides with an existing struct name", param_name, fn->name);
            }
            if (find_enum(module, param_name) != NULL) {
                ziman_error("type", fn->line, fn->col, "type parameter '%s' in function '%s' collides with an existing enum name", param_name, fn->name);
            }
        }
        current_decl_type_params = &fn->type_params;
        for (int pi = 0; pi < fn->params.count; pi++) {
            check_type_well_formed(module, fn->params.items[pi].type, fn->params.items[pi].line, fn->params.items[pi].col);
        }
        check_type_well_formed(module, fn->return_type, fn->line, fn->col);
        current_decl_type_params = NULL;
        ziman_set_error_context(NULL);
    }

    FunctionDecl *main_fn = find_function(module, "main");
    if (main_fn == NULL) {
        ziman_error("type", 0, 0, "no 'main' function found");
    }
    if (main_fn->params.count != 0 || main_fn->return_type.kind != TY_VOID) {
        ziman_error("type", main_fn->line, main_fn->col, "'main' must take no parameters and return nothing");
    }

    check_global_vals(module);
    for (int i = 0; i < module->functions.count; i++) {
        ziman_set_error_context(module->functions.items[i]->derived_from);
        check_function(module, module->functions.items[i]);
        ziman_set_error_context(NULL);
    }
}
