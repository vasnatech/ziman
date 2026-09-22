#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

typedef struct {
    TokenList tokens;
    int pos;
} Parser;

static Token *cur(Parser *p) { return &p->tokens.items[p->pos]; }
static Token *previous(Parser *p) { return &p->tokens.items[p->pos - 1]; }

static Token *advance_tok(Parser *p) {
    if (cur(p)->type != TOK_EOF) p->pos++;
    return previous(p);
}

static int check(Parser *p, TokenType type) { return cur(p)->type == type; }

static int match(Parser *p, TokenType type) {
    if (!check(p, type)) return 0;
    advance_tok(p);
    return 1;
}

static Token *expect(Parser *p, TokenType type, const char *what) {
    if (check(p, type)) return advance_tok(p);
    ziman_error("syntax", cur(p)->line, cur(p)->col, "expected %s, found %s", what, token_type_name(cur(p)->type));
    return NULL; /* unreachable */
}

static char *dup_token_text(Token *t) {
    char *s = malloc((size_t)t->length + 1);
    memcpy(s, t->start, (size_t)t->length);
    s[t->length] = '\0';
    return s;
}

static int starts_upper(const char *name) { return name[0] >= 'A' && name[0] <= 'Z'; }

/* Casing is grammar-significant, not just style: an uppercase identifier
 * immediately followed by '{' is always parsed as a struct literal (see
 * parse_primary), in every position including an `if`/`while` condition.
 * That's only sound if a value/function name can never start uppercase and
 * a struct name always does -- so both directions are enforced here, not
 * just documented. */
static void require_lower_start(Token *t, const char *what) {
    if (starts_upper(t->start)) {
        ziman_error("syntax", t->line, t->col, "%s must not start with an uppercase letter (uppercase is reserved for type names)", what);
    }
}

static void require_upper_start(Token *t, const char *what) {
    if (!starts_upper(t->start)) {
        ziman_error("syntax", t->line, t->col, "%s must start with an uppercase letter", what);
    }
}

/* Skips zero or more consecutive statement-separator newlines. */
static void skip_newlines(Parser *p) {
    while (match(p, TOK_NEWLINE)) {}
}

static Type parse_type(Parser *p);

/* `"<" type { "," type } ">"` -- a generic type's explicit type
 * arguments (`Box<int>`, `Pair<int, string>`). No inference anywhere in
 * Ziman's generics -- every instantiation writes these out. */
static TypeList parse_type_args(Parser *p) {
    TypeList args = {0};
    expect(p, TOK_LT, "'<'");
    type_list_push(&args, parse_type(p));
    while (match(p, TOK_COMMA)) {
        type_list_push(&args, parse_type(p));
    }
    expect(p, TOK_GT, "'>'");
    return args;
}

/* One-token lookahead for the one place generic-call syntax is genuinely
 * ambiguous with a real comparison: `identity<int>(5)` vs `x < y` both
 * start with `lowercase_ident <`. A struct/enum reference never has this
 * problem (an uppercase name can never be a real comparison operand --
 * see parse_type's/parse_primary's own TOK_LT branches), so this is only
 * needed for a call. Peeks at the token immediately *after* the `<`
 * (not yet consumed -- `p`'s current token is still the `<` itself) and
 * says yes only if it looks like the start of a type: a primitive
 * keyword, an uppercase identifier, `[`, `Result`, or `Channel`. A real
 * comparison's right-hand side can still coincidentally start with one
 * of these (`x < Point { ... }`, `x < [1, 2, 3]`), but `<`/`>` only
 * type-check between two numeric operands, so a false positive here
 * fails type-checking loudly rather than silently misbehaving. */
static int next_looks_like_generic_call(Parser *p) {
    Token *after_lt = &p->tokens.items[p->pos + 1];
    switch (after_lt->type) {
        case TOK_KW_INT: case TOK_KW_FLOAT: case TOK_KW_BOOL: case TOK_KW_STRING: case TOK_KW_BIGINT: case TOK_KW_DECIMAL:
        case TOK_KW_BYTE:
        case TOK_LBRACKET: case TOK_RESULT: case TOK_CHANNEL:
            return 1;
        case TOK_IDENT:
            return starts_upper(after_lt->start);
        default:
            return 0;
    }
}

static Type parse_type(Parser *p) {
    if (match(p, TOK_KW_INT)) return type_scalar(TY_INT);
    if (match(p, TOK_KW_FLOAT)) return type_scalar(TY_FLOAT);
    if (match(p, TOK_KW_BOOL)) return type_scalar(TY_BOOL);
    if (match(p, TOK_KW_STRING)) return type_scalar(TY_STRING);
    if (match(p, TOK_KW_BIGINT)) return type_scalar(TY_BIGINT);
    if (match(p, TOK_KW_DECIMAL)) return type_scalar(TY_DECIMAL);
    if (match(p, TOK_KW_BYTE)) return type_scalar(TY_BYTE);
    if (match(p, TOK_LBRACKET)) {
        Type elem = parse_type(p);
        expect(p, TOK_RBRACKET, "']'");
        return type_array(elem);
    }
    if (match(p, TOK_RESULT)) {
        expect(p, TOK_LT, "'<'");
        Type ok_type = parse_type(p);
        expect(p, TOK_COMMA, "','");
        Type err_type = parse_type(p);
        expect(p, TOK_GT, "'>'");
        return type_result(ok_type, err_type);
    }
    if (match(p, TOK_CHANNEL)) {
        expect(p, TOK_LT, "'<'");
        Type elem = parse_type(p);
        expect(p, TOK_GT, "'>'");
        return type_channel(elem);
    }
    if (check(p, TOK_IDENT)) {
        Token *t = advance_tok(p);
        /* `qualifier.TypeName` (a struct from an imported module) vs. a
         * plain `TypeName` -- the qualifier comes first and is lowercase,
         * so casing can't disambiguate here the way it does for a struct
         * literal; a following '.' is what marks this as qualified. */
        if (match(p, TOK_DOT)) {
            Token *type_tok = expect(p, TOK_IDENT, "a type name");
            require_upper_start(type_tok, "a type name");
            size_t len = (size_t)t->length + 1 + (size_t)type_tok->length + 1;
            char *qualified = malloc(len);
            snprintf(qualified, len, "%.*s.%.*s", t->length, t->start, type_tok->length, type_tok->start);
            Type ty = type_struct(qualified);
            free(qualified);
            /* `qualifier.Name<...>` -- a generic struct/enum reference
             * from another module. Whether `Name` is actually a struct or
             * an enum, and whether it's actually declared generic at all,
             * isn't known here any more than it is for the unqualified
             * case below -- validated later in typecheck.c. */
            if (check(p, TOK_LT)) {
                ty.type_args = parse_type_args(p);
            }
            return ty;
        }
        require_upper_start(t, "a type name");
        char *name = dup_token_text(t);
        Type ty = type_struct(name);
        free(name); /* type_struct() dups it into the Type itself */
        /* `Name<...>` -- a user-declared generic struct/enum reference.
         * Whether `Name` is actually a struct or an enum, and whether
         * it's actually declared generic at all, isn't known here (the
         * parser can't tell struct from enum, see ast.h's Type comment)
         * -- validated later in typecheck.c. */
        if (check(p, TOK_LT)) {
            ty.type_args = parse_type_args(p);
        }
        return ty;
    }
    ziman_error("syntax", cur(p)->line, cur(p)->col, "expected a type, found %s", token_type_name(cur(p)->type));
    return type_scalar(TY_VOID); /* unreachable */
}

static Expr *parse_expr(Parser *p);

/* Used by call arguments and array literals. Tolerates a newline after
 * each element/before each comma and after each comma, same "the last
 * token before it could end a statement" reasoning as parse_field_inits
 * above -- lets a long call or array literal span multiple lines. */
static ExprList parse_comma_separated(Parser *p, TokenType stop) {
    ExprList list = {0};
    if (!check(p, stop)) {
        expr_list_push(&list, parse_expr(p));
        skip_newlines(p);
        while (match(p, TOK_COMMA)) {
            skip_newlines(p);
            expr_list_push(&list, parse_expr(p));
            skip_newlines(p);
        }
    }
    return list;
}

/* `name: value` pairs, comma-separated -- used by both a struct literal's
 * fields and a `with` expression's overrides. A newline right after `{`
 * or right after a `,` is already silently swallowed by the lexer itself
 * (neither token can end a statement), but a newline right after a
 * field's *value* is a real, significant NEWLINE token whenever that
 * value's own last token can end a statement (e.g. `x: 1` followed by a
 * line break) -- skip_newlines() here is what lets the last field before
 * `,` or the closing brace sit on its own line, the same tolerance a
 * struct *declaration*'s fields already have. */
static FieldInitList parse_field_inits(Parser *p, TokenType stop) {
    FieldInitList list = {0};
    if (!check(p, stop)) {
        for (;;) {
            Token *name_tok = expect(p, TOK_IDENT, "a field name");
            expect(p, TOK_COLON, "':'");
            Expr *value = parse_expr(p);
            FieldInit fi;
            fi.name = dup_token_text(name_tok);
            fi.value = value;
            fi.line = name_tok->line;
            fi.col = name_tok->col;
            field_init_list_push(&list, fi);
            skip_newlines(p);
            if (!match(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
    }
    return list;
}

static Expr *parse_postfix(Parser *p);

static Expr *parse_primary(Parser *p) {
    Token *t = cur(p);

    if (match(p, TOK_INT_LIT)) {
        Expr *e = expr_new(EXPR_INT, t->line, t->col);
        e->as.int_val = strtoll(t->start, NULL, 10);
        return e;
    }
    if (match(p, TOK_FLOAT_LIT)) {
        Expr *e = expr_new(EXPR_FLOAT, t->line, t->col);
        e->as.float_val = strtod(t->start, NULL);
        return e;
    }
    if (match(p, TOK_BIGINT_LIT)) {
        Expr *e = expr_new(EXPR_BIGINT, t->line, t->col);
        /* `t`'s text includes the trailing 'n' suffix (`t->start` points
         * into the raw source, not a private NUL-terminated buffer the
         * way a string literal's does) -- strip it before handing the
         * digits to bignum_from_string, which needs a clean, NUL-
         * terminated digit string. */
        int digit_len = t->length - 1;
        char *digits = malloc((size_t)digit_len + 1);
        memcpy(digits, t->start, (size_t)digit_len);
        digits[digit_len] = '\0';
        int ok;
        e->as.bigint_val = bignum_from_string(digits, &ok);
        free(digits);
        if (!ok) {
            ziman_error("syntax", t->line, t->col, "internal error: lexer produced a malformed bigint literal");
        }
        return e;
    }
    if (match(p, TOK_DECIMAL_LIT)) {
        Expr *e = expr_new(EXPR_DECIMAL, t->line, t->col);
        /* Same reasoning as TOK_BIGINT_LIT above -- strip the trailing
         * 'd' suffix before handing the digits to decimal_from_string. */
        int digit_len = t->length - 1;
        char *digits = malloc((size_t)digit_len + 1);
        memcpy(digits, t->start, (size_t)digit_len);
        digits[digit_len] = '\0';
        int ok;
        e->as.decimal_val = decimal_from_string(digits, &ok);
        free(digits);
        if (!ok) {
            ziman_error("syntax", t->line, t->col, "internal error: lexer produced a malformed decimal literal");
        }
        return e;
    }
    if (match(p, TOK_BYTE_LIT)) {
        Expr *e = expr_new(EXPR_BYTE, t->line, t->col);
        /* Same "strip the trailing suffix" reasoning as TOK_BIGINT_LIT/
         * TOK_DECIMAL_LIT above, but byte is fixed-width -- a plain
         * strtol plus an explicit 0-255 range check, no bignum/decimal
         * parsing needed. */
        int digit_len = t->length - 1;
        char *digits = malloc((size_t)digit_len + 1);
        memcpy(digits, t->start, (size_t)digit_len);
        digits[digit_len] = '\0';
        long v = strtol(digits, NULL, 10);
        if (v < 0 || v > 255) {
            ziman_error("syntax", t->line, t->col, "byte literal %sb is out of range (must be 0-255)", digits);
        }
        free(digits);
        e->as.byte_val = (int)v;
        return e;
    }
    if (match(p, TOK_STRING_LIT)) {
        Expr *e = expr_new(EXPR_STRING, t->line, t->col);
        e->as.string_val = (char *)t->start; /* lexer already gave us a private, NUL-terminated buffer */
        return e;
    }
    if (match(p, TOK_TRUE)) {
        Expr *e = expr_new(EXPR_BOOL, t->line, t->col);
        e->as.bool_val = 1;
        return e;
    }
    if (match(p, TOK_FALSE)) {
        Expr *e = expr_new(EXPR_BOOL, t->line, t->col);
        e->as.bool_val = 0;
        return e;
    }
    if (match(p, TOK_IDENT)) {
        char *name = dup_token_text(t);
        /* `identity<int>(5)` -- a generic function call. Unlike a
         * struct/enum's `<`, this genuinely can't be told apart from a
         * real comparison (`x < y`) by casing alone, since a call's
         * callee is always lowercase -- see next_looks_like_generic_call. */
        if (!starts_upper(name) && check(p, TOK_LT) && next_looks_like_generic_call(p)) {
            TypeList type_args = parse_type_args(p);
            expect(p, TOK_LPAREN, "'('");
            ExprList args = parse_comma_separated(p, TOK_RPAREN);
            expect(p, TOK_RPAREN, "')'");
            Expr *e = expr_new(EXPR_CALL, t->line, t->col);
            e->as.call.callee = name;
            e->as.call.args = args;
            e->as.call.type_args = type_args;
            return e;
        }
        if (match(p, TOK_LPAREN)) {
            ExprList args = parse_comma_separated(p, TOK_RPAREN);
            expect(p, TOK_RPAREN, "')'");
            Expr *e = expr_new(EXPR_CALL, t->line, t->col);
            e->as.call.callee = name;
            e->as.call.args = args;
            return e;
        }
        /* An uppercase identifier immediately followed by '<' is always a
         * generic struct/enum's explicit type arguments (`Box<int> {
         * ... }`, `Option<int>.Some(5)`) -- unambiguous with a comparison
         * for the same reason struct-literal `{'/enum-access `.` already
         * are: an uppercase name can never be a real variable, so it can
         * never be the left operand of a real '<' comparison either.
         * Parsed once here, then dispatched below by whichever of '{'/'.'
         * follows. */
        TypeList type_args = {0};
        int has_type_args = 0;
        if (starts_upper(name) && check(p, TOK_LT)) {
            type_args = parse_type_args(p);
            has_type_args = 1;
        }
        /* An uppercase identifier immediately followed by '{' is always a
         * struct literal -- in every position, including an `if`/`while`
         * condition, never just here. See require_lower_start/
         * require_upper_start for the enforcement that makes this sound. */
        if (check(p, TOK_LBRACE) && starts_upper(name)) {
            advance_tok(p); /* '{' */
            FieldInitList fields = parse_field_inits(p, TOK_RBRACE);
            expect(p, TOK_RBRACE, "'}'");
            Expr *e = expr_new(EXPR_STRUCT_LIT, t->line, t->col);
            e->as.struct_lit.type_name = name;
            e->as.struct_lit.fields = fields;
            e->as.struct_lit.type_args = type_args;
            return e;
        }
        /* `EnumName[<Args>].Variant[(payload)]` (construction) or
         * `EnumName[<Args>].function(args)` (an is_X/unwrap_X consumption
         * call) -- both share this shape, disambiguated later by the type
         * checker (see EXPR_ENUM_ACCESS in ast.h). Unlike the
         * `qualifier.name` forms below, this needs no speculative
         * backtracking: a module qualifier is always lowercase (enforced
         * on `use ... as`), so an uppercase `name` immediately followed by
         * '.' can only ever mean this. */
        if (starts_upper(name) && check(p, TOK_DOT)) {
            advance_tok(p); /* '.' */
            Token *member_tok = expect(p, TOK_IDENT, "a variant or function name");
            char *member = dup_token_text(member_tok);
            ExprList args = {0};
            int has_parens = 0;
            if (match(p, TOK_LPAREN)) {
                has_parens = 1;
                args = parse_comma_separated(p, TOK_RPAREN);
                expect(p, TOK_RPAREN, "')'");
            }
            Expr *e = expr_new(EXPR_ENUM_ACCESS, t->line, t->col);
            e->as.enum_access.enum_name = name;
            e->as.enum_access.member_name = member;
            e->as.enum_access.args = args;
            e->as.enum_access.has_parens = has_parens;
            e->as.enum_access.type_args = type_args;
            return e;
        }
        if (has_type_args) {
            ziman_error("syntax", t->line, t->col, "expected '{' or '.' after type arguments");
        }
        /* `qualifier.name(args)` (a call into an imported module) or
         * `qualifier.Type { ... }` (a struct literal from one) -- but a
         * dot after a plain identifier is *usually* ordinary field access
         * on a local variable (`p.x`), which parse_postfix already handles.
         * There's no way to tell which one this is without looking past
         * the dot, so: tentatively consume it, and if what follows doesn't
         * match either qualified shape, back out and leave the dot for
         * parse_postfix to treat as plain field access. `name` is
         * guaranteed lowercase here (an uppercase `name` followed by '.'
         * was already handled, and returned from, above). */
        if (check(p, TOK_DOT)) {
            int saved_pos = p->pos;
            advance_tok(p); /* '.' */
            if (check(p, TOK_IDENT)) {
                Token *second = advance_tok(p);
                /* `qualifier.Type[<Args>] { ... }` (a struct literal from
                 * another module) or `qualifier.EnumName[<Args>].Member
                 * [(args)]` (enum access from another module, construction
                 * or an is_X/unwrap_X consumption call). An uppercase
                 * `second` can never mean anything else here -- casing
                 * rules rule out a real field or module-function name --
                 * so, same as the unqualified case above, this always
                 * commits once seen, no backtracking left to do. */
                if (starts_upper(second->start)) {
                    TypeList type_args = {0};
                    if (check(p, TOK_LT)) {
                        type_args = parse_type_args(p);
                    }
                    size_t len = strlen(name) + 1 + (size_t)second->length + 1;
                    char *qualified = malloc(len);
                    snprintf(qualified, len, "%s.%.*s", name, second->length, second->start);
                    free(name);
                    if (check(p, TOK_LBRACE)) {
                        advance_tok(p); /* '{' */
                        FieldInitList fields = parse_field_inits(p, TOK_RBRACE);
                        expect(p, TOK_RBRACE, "'}'");
                        Expr *e = expr_new(EXPR_STRUCT_LIT, t->line, t->col);
                        e->as.struct_lit.type_name = qualified;
                        e->as.struct_lit.fields = fields;
                        e->as.struct_lit.type_args = type_args;
                        return e;
                    }
                    if (check(p, TOK_DOT)) {
                        advance_tok(p); /* second '.' */
                        Token *member_tok = expect(p, TOK_IDENT, "a variant or function name");
                        char *member = dup_token_text(member_tok);
                        ExprList args = {0};
                        int has_parens = 0;
                        if (match(p, TOK_LPAREN)) {
                            has_parens = 1;
                            args = parse_comma_separated(p, TOK_RPAREN);
                            expect(p, TOK_RPAREN, "')'");
                        }
                        Expr *e = expr_new(EXPR_ENUM_ACCESS, t->line, t->col);
                        e->as.enum_access.enum_name = qualified;
                        e->as.enum_access.member_name = member;
                        e->as.enum_access.args = args;
                        e->as.enum_access.has_parens = has_parens;
                        e->as.enum_access.type_args = type_args;
                        return e;
                    }
                    ziman_error("syntax", second->line, second->col, "expected '{' or '.' after '%s'", qualified);
                }
                /* `qualifier.func<Type>(args)` -- a generic function call
                 * into another module. Same ambiguity, and same one-token
                 * lookahead fix, as the unqualified case above; `second`
                 * is guaranteed lowercase here (an uppercase one already
                 * returned above), so this is the only place left that
                 * needs it. */
                if (check(p, TOK_LT) && next_looks_like_generic_call(p)) {
                    TypeList type_args = parse_type_args(p);
                    expect(p, TOK_LPAREN, "'('");
                    ExprList args = parse_comma_separated(p, TOK_RPAREN);
                    expect(p, TOK_RPAREN, "')'");
                    size_t len = strlen(name) + 1 + (size_t)second->length + 1;
                    char *qualified = malloc(len);
                    snprintf(qualified, len, "%s.%.*s", name, second->length, second->start);
                    free(name);
                    Expr *e = expr_new(EXPR_CALL, t->line, t->col);
                    e->as.call.callee = qualified;
                    e->as.call.args = args;
                    e->as.call.type_args = type_args;
                    return e;
                }
                if (match(p, TOK_LPAREN)) {
                    ExprList args = parse_comma_separated(p, TOK_RPAREN);
                    expect(p, TOK_RPAREN, "')'");
                    size_t len = strlen(name) + 1 + (size_t)second->length + 1;
                    char *qualified = malloc(len);
                    snprintf(qualified, len, "%s.%.*s", name, second->length, second->start);
                    free(name);
                    Expr *e = expr_new(EXPR_CALL, t->line, t->col);
                    e->as.call.callee = qualified;
                    e->as.call.args = args;
                    return e;
                }
            }
            p->pos = saved_pos; /* not a qualified reference -- back out, leave '.' for parse_postfix */
        }
        Expr *e = expr_new(EXPR_IDENT, t->line, t->col);
        e->as.ident_name = name;
        return e;
    }
    if (match(p, TOK_LPAREN)) {
        Expr *inner = parse_expr(p);
        expect(p, TOK_RPAREN, "')'");
        return inner;
    }
    if (match(p, TOK_LBRACKET)) {
        ExprList elements = parse_comma_separated(p, TOK_RBRACKET);
        expect(p, TOK_RBRACKET, "']'");
        Expr *e = expr_new(EXPR_ARRAY_LIT, t->line, t->col);
        e->as.array_lit.elements = elements;
        return e;
    }
    if (match(p, TOK_WITH)) {
        Expr *base = parse_postfix(p);
        expect(p, TOK_LBRACE, "'{'");
        FieldInitList overrides = parse_field_inits(p, TOK_RBRACE);
        expect(p, TOK_RBRACE, "'}'");
        Expr *e = expr_new(EXPR_WITH, t->line, t->col);
        e->as.with_expr.base = base;
        e->as.with_expr.overrides = overrides;
        return e;
    }

    ziman_error("syntax", t->line, t->col, "expected an expression, found %s", token_type_name(t->type));
    return NULL; /* unreachable */
}

/* `primary` followed by any number of `[index]`/`.field` suffixes, e.g.
 * `matrix[i][j]` or `shapes[i].corner.x`, freely mixed. */
static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        if (check(p, TOK_LBRACKET)) {
            Token *br = advance_tok(p);
            Expr *idx = parse_expr(p);
            expect(p, TOK_RBRACKET, "']'");
            Expr *indexed = expr_new(EXPR_INDEX, br->line, br->col);
            indexed->as.index.array = e;
            indexed->as.index.index = idx;
            e = indexed;
        } else if (check(p, TOK_DOT)) {
            Token *dot = advance_tok(p);
            Token *field_tok = expect(p, TOK_IDENT, "a field name");
            Expr *access = expr_new(EXPR_FIELD_ACCESS, dot->line, dot->col);
            access->as.field_access.base = e;
            access->as.field_access.field_name = dup_token_text(field_tok);
            e = access;
        } else {
            break;
        }
    }
    return e;
}

static Expr *parse_unary(Parser *p) {
    if (check(p, TOK_BANG) || check(p, TOK_MINUS)) {
        Token *t = advance_tok(p);
        Op op = t->type == TOK_BANG ? OP_NOT : OP_NEG;
        Expr *operand = parse_unary(p);
        Expr *e = expr_new(EXPR_UNARY, t->line, t->col);
        e->as.unary.op = op;
        e->as.unary.operand = operand;
        return e;
    }
    return parse_postfix(p);
}

static Expr *parse_cast(Parser *p) {
    Expr *e = parse_unary(p);
    while (match(p, TOK_AS)) {
        Token *t = previous(p);
        Type target = parse_type(p);
        Expr *cast = expr_new(EXPR_CAST, t->line, t->col);
        cast->as.cast.operand = e;
        cast->as.cast.target_type = target;
        e = cast;
    }
    return e;
}

static Expr *parse_binary_level(Parser *p, Expr *(*next)(Parser *), const TokenType *ops, const Op *op_kinds, int op_count) {
    Expr *left = next(p);
    for (;;) {
        int matched = -1;
        for (int i = 0; i < op_count; i++) {
            if (check(p, ops[i])) { matched = i; break; }
        }
        if (matched < 0) break;
        Token *t = advance_tok(p);
        Expr *right = next(p);
        Expr *e = expr_new(EXPR_BINARY, t->line, t->col);
        e->as.binary.op = op_kinds[matched];
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

static Expr *parse_factor(Parser *p) {
    static const TokenType ops[] = {TOK_STAR, TOK_SLASH, TOK_PERCENT};
    static const Op kinds[] = {OP_MUL, OP_DIV, OP_MOD};
    return parse_binary_level(p, parse_cast, ops, kinds, 3);
}

static Expr *parse_term(Parser *p) {
    static const TokenType ops[] = {TOK_PLUS, TOK_MINUS};
    static const Op kinds[] = {OP_ADD, OP_SUB};
    return parse_binary_level(p, parse_factor, ops, kinds, 2);
}

static Expr *parse_comparison(Parser *p) {
    static const TokenType ops[] = {TOK_LT, TOK_GT, TOK_LTEQ, TOK_GTEQ};
    static const Op kinds[] = {OP_LT, OP_GT, OP_LE, OP_GE};
    return parse_binary_level(p, parse_term, ops, kinds, 4);
}

static Expr *parse_equality(Parser *p) {
    static const TokenType ops[] = {TOK_EQEQ, TOK_BANGEQ};
    static const Op kinds[] = {OP_EQ, OP_NEQ};
    return parse_binary_level(p, parse_comparison, ops, kinds, 2);
}

static Expr *parse_logic_and(Parser *p) {
    static const TokenType ops[] = {TOK_AMPAMP};
    static const Op kinds[] = {OP_AND};
    return parse_binary_level(p, parse_equality, ops, kinds, 1);
}

static Expr *parse_logic_or(Parser *p) {
    static const TokenType ops[] = {TOK_PIPEPIPE};
    static const Op kinds[] = {OP_OR};
    return parse_binary_level(p, parse_logic_and, ops, kinds, 1);
}

static Expr *parse_expr(Parser *p) { return parse_logic_or(p); }

static Stmt *parse_statement(Parser *p);

static Stmt *parse_block(Parser *p) {
    Token *open = expect(p, TOK_LBRACE, "'{'");
    Stmt *block = stmt_new(STMT_BLOCK, open->line, open->col);
    skip_newlines(p);
    while (!check(p, TOK_RBRACE)) {
        stmt_list_push(&block->as.block.stmts, parse_statement(p));
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return block;
}

static Stmt *parse_decl(Parser *p, StmtKind kind) {
    Token *kw = previous(p);
    Token *name_tok = expect(p, TOK_IDENT, "an identifier");
    require_lower_start(name_tok, "a variable name");
    Stmt *s = stmt_new(kind, kw->line, kw->col);
    s->as.decl.name = dup_token_text(name_tok);
    if (match(p, TOK_COLON)) {
        s->as.decl.declared_type = parse_type(p);
        s->as.decl.has_declared_type = 1;
    }
    expect(p, TOK_EQ, "'='");
    s->as.decl.init = parse_expr(p);
    return s;
}

/* `[pub] val NAME[: Type] = <expr>` at module top level -- same grammar as
 * parse_decl's local `val`, minus `var` (no mutable globals, see
 * docs/DESIGN.md's "Global val" entry) and minus being wrapped in a Stmt,
 * since a top-level declaration lives in Module.vals, not a function
 * body's statement list. */
static GlobalValDecl *parse_global_val_decl(Parser *p, int is_public) {
    Token *kw = expect(p, TOK_VAL, "'val'");
    Token *name_tok = expect(p, TOK_IDENT, "a global value name");
    require_lower_start(name_tok, "a global value name");
    GlobalValDecl *v = calloc(1, sizeof(GlobalValDecl));
    v->name = dup_token_text(name_tok);
    v->is_public = is_public;
    v->line = kw->line;
    v->col = kw->col;
    if (match(p, TOK_COLON)) {
        v->type = parse_type(p);
        v->has_declared_type = 1;
    }
    expect(p, TOK_EQ, "'='");
    v->init = parse_expr(p);
    return v;
}

static Stmt *parse_if(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_IF, kw->line, kw->col);
    s->as.if_stmt.cond = parse_expr(p);
    s->as.if_stmt.then_block = parse_block(p);
    /* `else { ... }` and `else if ... { ... }` chains stay on the same
     * line as the closing brace before them, so no newline-skipping here. */
    if (match(p, TOK_ELSE)) {
        if (match(p, TOK_IF)) {
            s->as.if_stmt.else_block = parse_if(p);
        } else {
            s->as.if_stmt.else_block = parse_block(p);
        }
    }
    return s;
}

static Stmt *parse_while(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_WHILE, kw->line, kw->col);
    s->as.while_stmt.cond = parse_expr(p);
    s->as.while_stmt.body = parse_block(p);
    return s;
}

/* `for binder1[, binder2] in expr [".." expr] { body }` -- see ast.h's
 * for_stmt comment for what each of the three resulting shapes means. */
static Stmt *parse_for(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_FOR, kw->line, kw->col);

    Token *binder1_tok = expect(p, TOK_IDENT, "a loop variable name");
    require_lower_start(binder1_tok, "a loop variable name");
    s->as.for_stmt.binder1 = dup_token_text(binder1_tok);

    if (match(p, TOK_COMMA)) {
        Token *binder2_tok = expect(p, TOK_IDENT, "a loop variable name");
        require_lower_start(binder2_tok, "a loop variable name");
        s->as.for_stmt.binder2 = dup_token_text(binder2_tok);
    }

    expect(p, TOK_IN, "'in'");
    Expr *first = parse_expr(p);
    if (match(p, TOK_DOTDOT)) {
        if (s->as.for_stmt.binder2 != NULL) {
            ziman_error("syntax", kw->line, kw->col,
                        "a range-based 'for' takes exactly one loop variable (the count), found a second one ('%s')",
                        s->as.for_stmt.binder2);
        }
        s->as.for_stmt.is_range = 1;
        s->as.for_stmt.range_start = first;
        s->as.for_stmt.range_end = parse_expr(p);
    } else {
        s->as.for_stmt.iterable = first;
    }

    s->as.for_stmt.body = parse_block(p);
    return s;
}

static Stmt *parse_return(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_RETURN, kw->line, kw->col);
    if (!check(p, TOK_NEWLINE) && !check(p, TOK_RBRACE)) {
        s->as.ret.value = parse_expr(p);
    }
    return s;
}

/* `spawn name(args)` or `spawn mod.name(args)` -- builds the exact same
 * flat, possibly-dotted callee string EXPR_CALL's qualified form already
 * does, so it flows through the loader's existing self-qualify/cross-
 * module resolution unchanged (see loader.c's STMT_SPAWN cases). Always a
 * statically-named function, never a first-class function value -- see
 * ast.h's spawn_stmt comment. */
static Stmt *parse_spawn(Parser *p) {
    Token *kw = previous(p);
    Token *name_tok = expect(p, TOK_IDENT, "a function name");
    char *callee = dup_token_text(name_tok);
    if (match(p, TOK_DOT)) {
        Token *second = expect(p, TOK_IDENT, "a function name");
        size_t len = strlen(callee) + 1 + (size_t)second->length + 1;
        char *qualified = malloc(len);
        snprintf(qualified, len, "%s.%.*s", callee, second->length, second->start);
        free(callee);
        callee = qualified;
    }
    expect(p, TOK_LPAREN, "'('");
    ExprList args = parse_comma_separated(p, TOK_RPAREN);
    expect(p, TOK_RPAREN, "')'");
    Stmt *s = stmt_new(STMT_SPAWN, kw->line, kw->col);
    s->as.spawn_stmt.callee = callee;
    s->as.spawn_stmt.args = args;
    return s;
}

static Stmt *parse_statement(Parser *p) {
    if (match(p, TOK_VAL)) return parse_decl(p, STMT_VAL);
    if (match(p, TOK_VAR)) return parse_decl(p, STMT_VAR);
    if (match(p, TOK_IF)) return parse_if(p);
    if (match(p, TOK_WHILE)) return parse_while(p);
    if (match(p, TOK_FOR)) return parse_for(p);
    if (match(p, TOK_BREAK)) { Token *kw = previous(p); return stmt_new(STMT_BREAK, kw->line, kw->col); }
    if (match(p, TOK_CONTINUE)) { Token *kw = previous(p); return stmt_new(STMT_CONTINUE, kw->line, kw->col); }
    if (match(p, TOK_SPAWN)) return parse_spawn(p);
    if (match(p, TOK_RETURN)) return parse_return(p);

    /* `IDENT = expr` (assignment) vs. any other expression statement
     * (currently only calls are useful here, e.g. `print(x)`). Both start
     * with an expression, so parse one and see what follows. */
    Token *start = cur(p);
    Expr *first = parse_expr(p);
    if (match(p, TOK_EQ)) {
        if (first->kind != EXPR_IDENT) {
            ziman_error("syntax", start->line, start->col, "left-hand side of assignment must be a variable name");
        }
        Stmt *s = stmt_new(STMT_ASSIGN, start->line, start->col);
        s->as.assign.name = first->as.ident_name;
        s->as.assign.value = parse_expr(p);
        return s;
    }
    Stmt *s = stmt_new(STMT_EXPR, start->line, start->col);
    s->as.expr_stmt.expr = first;
    return s;
}

static ParamList parse_params(Parser *p) {
    ParamList params = {0};
    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            Token *param_name = expect(p, TOK_IDENT, "a parameter name");
            require_lower_start(param_name, "a parameter name");
            expect(p, TOK_COLON, "':'");
            Type param_type = parse_type(p);
            Param param = {dup_token_text(param_name), param_type, param_name->line, param_name->col};
            param_list_push(&params, param);
            skip_newlines(p);
            if (!match(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
    }
    return params;
}

static TypeParamList parse_type_param_list(Parser *p);
static void reclassify_type_params(Type *t, TypeParamList *type_params);
static void reclassify_type_params_in_stmt(Stmt *s, TypeParamList *type_params);

static FunctionDecl *parse_function(Parser *p) {
    Token *kw = expect(p, TOK_FN, "'fn'");
    Token *name_tok = expect(p, TOK_IDENT, "a function name");
    require_lower_start(name_tok, "a function name");

    FunctionDecl *fn = calloc(1, sizeof(FunctionDecl));
    fn->name = dup_token_text(name_tok);
    fn->line = kw->line;
    fn->col = kw->col;

    if (check(p, TOK_LT)) {
        fn->type_params = parse_type_param_list(p);
    }

    expect(p, TOK_LPAREN, "'('");
    fn->params = parse_params(p);
    expect(p, TOK_RPAREN, "')'");

    fn->return_type = type_scalar(TY_VOID);
    if (match(p, TOK_ARROW)) {
        fn->return_type = parse_type(p);
    }

    if (fn->type_params.count > 0) {
        for (int i = 0; i < fn->params.count; i++) {
            reclassify_type_params(&fn->params.items[i].type, &fn->type_params);
        }
        reclassify_type_params(&fn->return_type, &fn->type_params);
    }

    fn->body = parse_block(p);
    if (fn->type_params.count > 0) {
        reclassify_type_params_in_stmt(fn->body, &fn->type_params);
    }
    return fn;
}

/* `native fn name(params) -> type`, no body at all -- its implementation is
 * a C function registered by module path + name, looked up at call time.
 * The type checker treats its signature exactly like any other function's
 * (see check_function's `is_native` early-return); the interpreter reports
 * a clear "no registered implementation" error if nothing was registered
 * for it (see interpreter.c) -- no real native module exists to back one
 * yet, this is the language mechanism only. */
static FunctionDecl *parse_native_function(Parser *p) {
    Token *kw = expect(p, TOK_NATIVE, "'native'");
    expect(p, TOK_FN, "'fn'");
    Token *name_tok = expect(p, TOK_IDENT, "a function name");
    require_lower_start(name_tok, "a function name");

    FunctionDecl *fn = calloc(1, sizeof(FunctionDecl));
    fn->name = dup_token_text(name_tok);
    fn->is_native = 1;
    fn->line = kw->line;
    fn->col = kw->col;

    expect(p, TOK_LPAREN, "'('");
    fn->params = parse_params(p);
    expect(p, TOK_RPAREN, "')'");

    fn->return_type = type_scalar(TY_VOID);
    if (match(p, TOK_ARROW)) {
        fn->return_type = parse_type(p);
    }
    return fn;
}

/* `"<" IDENT [":" bound {"+" bound}] { "," IDENT [":" bound {"+" bound}] }
 * ">"` -- a generic declaration's own formal type parameters
 * (`struct Box<T>`, `struct Map<K: Eq + Hash, V>`). Each name is
 * TYPE_IDENT-cased, same casing rule as any other type-like name. `Eq`
 * and `Hash` are currently the only recognized bounds, combinable with
 * `+` (reusing the arithmetic operator token -- a distinct, non-
 * expression grammatical position, same "as"/"/" reuse pattern already
 * used for casts and module paths) -- see ast.h's TypeParamBound comment
 * for why this isn't the start of a general trait system. */
static TypeParamList parse_type_param_list(Parser *p) {
    TypeParamList params = {0};
    expect(p, TOK_LT, "'<'");
    for (;;) {
        Token *name_tok = expect(p, TOK_IDENT, "a type parameter name");
        require_upper_start(name_tok, "a type parameter name");
        int bound = BOUND_NONE;
        if (match(p, TOK_COLON)) {
            for (;;) {
                Token *bound_tok = expect(p, TOK_IDENT, "a bound name");
                if (bound_tok->length == 2 && strncmp(bound_tok->start, "Eq", 2) == 0) {
                    bound |= BOUND_EQ;
                } else if (bound_tok->length == 4 && strncmp(bound_tok->start, "Hash", 4) == 0) {
                    bound |= BOUND_HASH;
                } else {
                    ziman_error("syntax", bound_tok->line, bound_tok->col,
                                "unknown bound '%.*s' (only 'Eq'/'Hash' are supported)", bound_tok->length, bound_tok->start);
                }
                if (!match(p, TOK_PLUS)) break;
            }
        }
        TypeParam param = {dup_token_text(name_tok), bound};
        type_param_list_push(&params, param);
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_GT, "'>'");
    return params;
}

/* Rewrites any TY_STRUCT node in `t` (recursively, through `[T]`/
 * `Result<T, E>`/`Channel<T>`/nested generic type arguments) that names
 * one of `type_params` into a TY_TYPEVAR referencing it. The parser
 * always provisionally builds TY_STRUCT for a bare TYPE_IDENT type
 * reference (see ast.h's Type comment, same reason struct-vs-enum needs
 * reclassifying), so a generic declaration's own field/variant/param
 * types referencing its own type parameters need this same "reclassify
 * once we know better" step, run right after each declaration's own
 * type_params are known. */
static void reclassify_type_params(Type *t, TypeParamList *type_params) {
    if (t->kind == TY_ARRAY) { reclassify_type_params(t->elem, type_params); return; }
    if (t->kind == TY_RESULT) {
        reclassify_type_params(t->elem, type_params);
        reclassify_type_params(t->elem_err, type_params);
        return;
    }
    if (t->kind == TY_CHANNEL) { reclassify_type_params(t->elem, type_params); return; }
    if (t->kind != TY_STRUCT) return;
    for (int i = 0; i < type_params->count; i++) {
        if (strcmp(t->struct_name, type_params->items[i].name) == 0) {
            char *name = t->struct_name;
            t->kind = TY_TYPEVAR;
            t->struct_name = NULL;
            t->type_var_name = name;
            return;
        }
    }
    /* Not itself a type parameter -- but might be a generic reference
     * (`Box<T>`) whose own type arguments need the same treatment. */
    for (int i = 0; i < t->type_args.count; i++) {
        reclassify_type_params(&t->type_args.items[i], type_params);
    }
}

/* Walks every Type value embedded inside an expression (a cast's target
 * type, or a call/struct-literal/enum-access's own explicit type
 * arguments) and reclassifies each one the same way a field/variant/
 * param type already is. Needed because a generic *function*'s own type
 * parameters can be referenced from inside its body too
 * (`Option<V>.Some(...)`, `x as V`), which parsing the body's statements
 * and expressions doesn't otherwise touch -- unlike a struct/enum
 * declaration, where every type-bearing spot is a field or variant
 * payload, reclassified right where it's parsed. Mirrors loader.c's
 * self_qualify_expr/self_qualify_stmt shape exactly (same node kinds,
 * same recursion), just touching Type nodes instead of qualifying names. */
static void reclassify_type_params_in_expr(Expr *e, TypeParamList *type_params) {
    switch (e->kind) {
        case EXPR_INT: case EXPR_FLOAT: case EXPR_BIGINT: case EXPR_DECIMAL: case EXPR_BYTE: case EXPR_STRING: case EXPR_BOOL: case EXPR_IDENT:
            break;
        case EXPR_UNARY:
            reclassify_type_params_in_expr(e->as.unary.operand, type_params);
            break;
        case EXPR_BINARY:
            reclassify_type_params_in_expr(e->as.binary.left, type_params);
            reclassify_type_params_in_expr(e->as.binary.right, type_params);
            break;
        case EXPR_CALL:
            for (int i = 0; i < e->as.call.type_args.count; i++) reclassify_type_params(&e->as.call.type_args.items[i], type_params);
            for (int i = 0; i < e->as.call.args.count; i++) reclassify_type_params_in_expr(e->as.call.args.items[i], type_params);
            break;
        case EXPR_CAST:
            reclassify_type_params_in_expr(e->as.cast.operand, type_params);
            reclassify_type_params(&e->as.cast.target_type, type_params);
            break;
        case EXPR_ARRAY_LIT:
            for (int i = 0; i < e->as.array_lit.elements.count; i++) reclassify_type_params_in_expr(e->as.array_lit.elements.items[i], type_params);
            break;
        case EXPR_INDEX:
            reclassify_type_params_in_expr(e->as.index.array, type_params);
            reclassify_type_params_in_expr(e->as.index.index, type_params);
            break;
        case EXPR_STRUCT_LIT:
            for (int i = 0; i < e->as.struct_lit.type_args.count; i++) reclassify_type_params(&e->as.struct_lit.type_args.items[i], type_params);
            for (int i = 0; i < e->as.struct_lit.fields.count; i++) reclassify_type_params_in_expr(e->as.struct_lit.fields.items[i].value, type_params);
            break;
        case EXPR_FIELD_ACCESS:
            reclassify_type_params_in_expr(e->as.field_access.base, type_params);
            break;
        case EXPR_WITH:
            reclassify_type_params_in_expr(e->as.with_expr.base, type_params);
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) reclassify_type_params_in_expr(e->as.with_expr.overrides.items[i].value, type_params);
            break;
        case EXPR_ENUM_ACCESS:
            for (int i = 0; i < e->as.enum_access.type_args.count; i++) reclassify_type_params(&e->as.enum_access.type_args.items[i], type_params);
            for (int i = 0; i < e->as.enum_access.args.count; i++) reclassify_type_params_in_expr(e->as.enum_access.args.items[i], type_params);
            break;
    }
}

static void reclassify_type_params_in_stmt(Stmt *s, TypeParamList *type_params) {
    switch (s->kind) {
        case STMT_VAL: case STMT_VAR:
            if (s->as.decl.has_declared_type) reclassify_type_params(&s->as.decl.declared_type, type_params);
            reclassify_type_params_in_expr(s->as.decl.init, type_params);
            break;
        case STMT_ASSIGN:
            reclassify_type_params_in_expr(s->as.assign.value, type_params);
            break;
        case STMT_IF:
            reclassify_type_params_in_expr(s->as.if_stmt.cond, type_params);
            reclassify_type_params_in_stmt(s->as.if_stmt.then_block, type_params);
            if (s->as.if_stmt.else_block != NULL) reclassify_type_params_in_stmt(s->as.if_stmt.else_block, type_params);
            break;
        case STMT_WHILE:
            reclassify_type_params_in_expr(s->as.while_stmt.cond, type_params);
            reclassify_type_params_in_stmt(s->as.while_stmt.body, type_params);
            break;
        case STMT_FOR:
            if (s->as.for_stmt.is_range) {
                reclassify_type_params_in_expr(s->as.for_stmt.range_start, type_params);
                reclassify_type_params_in_expr(s->as.for_stmt.range_end, type_params);
            } else {
                reclassify_type_params_in_expr(s->as.for_stmt.iterable, type_params);
            }
            reclassify_type_params_in_stmt(s->as.for_stmt.body, type_params);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
        case STMT_SPAWN:
            for (int i = 0; i < s->as.spawn_stmt.args.count; i++) reclassify_type_params_in_expr(s->as.spawn_stmt.args.items[i], type_params);
            break;
        case STMT_RETURN:
            if (s->as.ret.value != NULL) reclassify_type_params_in_expr(s->as.ret.value, type_params);
            break;
        case STMT_EXPR:
            reclassify_type_params_in_expr(s->as.expr_stmt.expr, type_params);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) reclassify_type_params_in_stmt(s->as.block.stmts.items[i], type_params);
            break;
    }
}

static StructDecl *parse_struct_decl(Parser *p) {
    Token *kw = expect(p, TOK_STRUCT, "'struct'");
    Token *name_tok = expect(p, TOK_IDENT, "a struct name");
    require_upper_start(name_tok, "a struct name");

    StructDecl *sd = calloc(1, sizeof(StructDecl));
    sd->name = dup_token_text(name_tok);
    sd->line = kw->line;
    sd->col = kw->col;

    if (check(p, TOK_LT)) {
        sd->type_params = parse_type_param_list(p);
    }

    expect(p, TOK_LBRACE, "'{'");
    skip_newlines(p);
    while (!check(p, TOK_RBRACE)) {
        Token *field_name = expect(p, TOK_IDENT, "a field name");
        require_lower_start(field_name, "a field name");
        expect(p, TOK_COLON, "':'");
        Type field_type = parse_type(p);
        if (sd->type_params.count > 0) reclassify_type_params(&field_type, &sd->type_params);
        Param field = {dup_token_text(field_name), field_type, field_name->line, field_name->col};
        param_list_push(&sd->fields, field);
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return sd;
}

/* `enum Name { Variant1[(type)] Variant2[(type)] ... }`. A variant name is
 * TYPE_IDENT-cased like a struct name (it's a nested type-like identifier,
 * always referenced qualified as `Name.Variant`); its optional payload is a
 * single TYPE in parens, never a param list or a struct-literal-shaped
 * block -- Ziman has no tuple type, so a variant carries at most one value,
 * same as `Result<T, E>`'s `Ok`/`Err`. See docs/SYNTAX.md "Enums". */
static EnumDecl *parse_enum_decl(Parser *p) {
    Token *kw = expect(p, TOK_ENUM, "'enum'");
    Token *name_tok = expect(p, TOK_IDENT, "an enum name");
    require_upper_start(name_tok, "an enum name");

    EnumDecl *ed = calloc(1, sizeof(EnumDecl));
    ed->name = dup_token_text(name_tok);
    ed->line = kw->line;
    ed->col = kw->col;

    if (check(p, TOK_LT)) {
        ed->type_params = parse_type_param_list(p);
    }

    expect(p, TOK_LBRACE, "'{'");
    skip_newlines(p);
    while (!check(p, TOK_RBRACE)) {
        Token *variant_tok = expect(p, TOK_IDENT, "a variant name");
        require_upper_start(variant_tok, "a variant name");
        EnumVariant variant = {0};
        variant.name = dup_token_text(variant_tok);
        variant.line = variant_tok->line;
        variant.col = variant_tok->col;
        if (match(p, TOK_LPAREN)) {
            variant.payload_type = parse_type(p);
            if (ed->type_params.count > 0) reclassify_type_params(&variant.payload_type, &ed->type_params);
            variant.has_payload = 1;
            expect(p, TOK_RPAREN, "')'");
        }
        enum_variant_list_push(&ed->variants, variant);
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return ed;
}

/* `use path/to/mod [as alias]`, or `use path/to/mod.Name [as alias]` to
 * import one specific declaration instead of the whole module. The path
 * is a sequence of identifiers joined by '/' (reusing the existing
 * division-operator token -- `use`'s path grammar is a distinct,
 * non-expression context, so there's no ambiguity with division). Not
 * resolved to a real file (or a real declaration, for the per-name form)
 * yet -- that's the loader's job, still ahead; this just records what
 * was written. */
static UseDecl parse_use_decl(Parser *p) {
    Token *kw = expect(p, TOK_USE, "'use'");

    size_t cap = 64, len = 0;
    char *path = malloc(cap);
    path[0] = '\0';

    Token *last_seg = expect(p, TOK_IDENT, "a module path segment");
    size_t seg_len = (size_t)last_seg->length;
    if (seg_len + 1 > cap) { cap = seg_len + 1; path = realloc(path, cap); }
    memcpy(path, last_seg->start, seg_len);
    len = seg_len;
    path[len] = '\0';

    while (match(p, TOK_SLASH)) {
        last_seg = expect(p, TOK_IDENT, "a module path segment");
        seg_len = (size_t)last_seg->length;
        size_t needed = len + 1 + seg_len + 1;
        if (needed > cap) { cap = needed; path = realloc(path, cap); }
        path[len] = '/';
        memcpy(path + len + 1, last_seg->start, seg_len);
        len += 1 + seg_len;
        path[len] = '\0';
    }

    UseDecl u;
    u.path = path;
    u.line = kw->line;

    if (match(p, TOK_DOT)) {
        /* Per-name import: `use collections.HashMap`. Casing isn't
         * checked against what the name actually turns out to be (the
         * loader hasn't resolved it yet, and doesn't need to -- the
         * target's own declaration already enforced its casing when it
         * was declared). An `as` alias only needs to match the imported
         * name's own casing, which is checkable right here with no
         * resolution at all: it's the same "uppercase is always a type,
         * lowercase never is" invariant every other binder enforces. */
        Token *member = expect(p, TOK_IDENT, "an imported name");
        u.qualifier = NULL;
        u.imported_name = dup_token_text(member);
        if (match(p, TOK_AS)) {
            Token *alias = expect(p, TOK_IDENT, "an import alias");
            if (starts_upper(alias->start) != starts_upper(member->start)) {
                ziman_error("syntax", alias->line, alias->col,
                            "an import alias must match the imported name's casing ('%.*s' vs '%.*s')",
                            (int)alias->length, alias->start, (int)member->length, member->start);
            }
            u.local_name = dup_token_text(alias);
        } else {
            u.local_name = u.imported_name; /* shared pointer -- AST nodes are never freed */
        }
        u.col = kw->col;
        return u;
    }

    if (match(p, TOK_AS)) {
        Token *alias = expect(p, TOK_IDENT, "a module alias");
        require_lower_start(alias, "a module alias");
        u.qualifier = dup_token_text(alias);
    } else {
        require_lower_start(last_seg, "a module qualifier (the path's last segment)");
        u.qualifier = dup_token_text(last_seg);
    }
    u.imported_name = NULL;
    u.local_name = NULL;
    u.col = kw->col;
    return u;
}

Module *parse(TokenList tokens) {
    Parser p = {tokens, 0};
    Module *module = calloc(1, sizeof(Module));

    skip_newlines(&p);
    while (!check(&p, TOK_EOF)) {
        if (check(&p, TOK_USE)) {
            use_list_push(&module->uses, parse_use_decl(&p));
            skip_newlines(&p);
            continue;
        }
        int is_public = match(&p, TOK_PUB);
        if (check(&p, TOK_STRUCT)) {
            StructDecl *sd = parse_struct_decl(&p);
            sd->is_public = is_public;
            struct_list_push(&module->structs, sd);
        } else if (check(&p, TOK_ENUM)) {
            EnumDecl *ed = parse_enum_decl(&p);
            ed->is_public = is_public;
            enum_list_push(&module->enums, ed);
        } else if (check(&p, TOK_NATIVE)) {
            FunctionDecl *fn = parse_native_function(&p);
            fn->is_public = is_public;
            function_list_push(&module->functions, fn);
        } else if (check(&p, TOK_VAL)) {
            GlobalValDecl *v = parse_global_val_decl(&p, is_public);
            global_val_list_push(&module->vals, v);
        } else {
            FunctionDecl *fn = parse_function(&p);
            fn->is_public = is_public;
            function_list_push(&module->functions, fn);
        }
        skip_newlines(&p);
    }

    return module;
}
