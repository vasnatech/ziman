#include <setjmp.h>
#include "parser.h"

#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "derive.h"
#include "stack.h"
#include "error.h"
#include "xalloc.h"

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

/* `ahead` tokens past the current one, without consuming anything -- the token list is always
 * TOK_EOF-terminated, so looking past the end just keeps reporting TOK_EOF rather than running off
 * the array. Needed to tell `#![...]` (a module attribute) from `#[...]` (a declaration's own). */
static int check_ahead(Parser *p, int ahead, TokenType type) {
    size_t at = (size_t)p->pos;
    for (int i = 0; i < ahead && p->tokens.items[at].type != TOK_EOF; i++) at++;
    return p->tokens.items[at].type == type;
}

static int match(Parser *p, TokenType type) {
    if (!check(p, type)) return 0;
    advance_tok(p);
    return 1;
}

/* Consumes and returns true only if the current token is a plain identifier spelled exactly
 * `word` -- a soft keyword, recognized by text at one specific grammar position rather than
 * reserved everywhere the way TOK_PUB/TOK_STRUCT/etc. are. Used for `internal` (see the
 * visibility modifier just below) so a program already using that word as a function, variable
 * or import name keeps compiling unchanged -- the same technique `unit` already uses a few
 * lines below (parse_type's own identifier-text check) for the identical reason. */
static int match_ident(Parser *p, const char *word) {
    size_t n = strlen(word);
    if (check(p, TOK_IDENT) && (size_t)cur(p)->length == n && memcmp(cur(p)->start, word, n) == 0) {
        advance_tok(p);
        return 1;
    }
    return 0;
}

/* Two token pairs that are a habit from another language rather than a typo, caught here because
 * this is where both of them surface whatever the surrounding construct is. Each was written by
 * five of the ten cold attempts in the 2026-10-09 benchmark pilot, and neither had a diagnostic:
 * `--` reported `expected 'fn', found '-'`, which says nothing about comments, and `=>` reported
 * whatever the construct happened to want next (docs/reviews/llm-benchmark-2026-10-09.md, F28). */
static int token_text_is(Token *t, const char *word) {
    size_t n = strlen(word);
    return t->type == TOK_IDENT && (size_t)t->length == n && memcmp(t->start, word, n) == 0;
}

static void report_habit_token_pair(Parser *p, const char *what) {
    Token *t = cur(p);
    Token *next = p->pos + 1 < p->tokens.count ? &p->tokens.items[p->pos + 1] : NULL;
    if (next == NULL) return;
    /* These four are scoped by what the parser was asking for, so the advice cannot fire in an
     * unrelated failure that happens to stop on the same word -- a value named `type` is legal, and
     * a division is legal. Frequencies are from the 2026-10-09 pilot's ten cold attempts
     * (docs/reviews/llm-benchmark-2026-10-09.md, tasks.md F28). */
    if (strcmp(what, "'{'") == 0 && t->type == TOK_SLASH && next->type == TOK_LBRACE) {
        /* `-> unit / {IO}`: an effect row, which four of ten cold attempts wrote. The bare message
         * named the '/' and left a reader unable to tell whether the syntax was wrong or the
         * feature absent. */
        ziman_error("syntax", t->line, t->col,
                    "Ziman has no effect annotations -- a function's type is just its return type, so drop the "
                    "'/ { ... }' (effects are not tracked in the type system at all)");
    }
    if (strcmp(what, "'{'") == 0 && t->type == TOK_EQ && token_text_is(next, "do")) {
        ziman_error("syntax", t->line, t->col,
                    "a function body is a brace block, not '= do ... end' -- write 'fn name(...) -> T { ... }'");
    }
    if (strcmp(what, "'fn'") == 0 && token_text_is(t, "import")) {
        ziman_error("syntax", t->line, t->col,
                    "Ziman has no 'import' -- a bundled module is brought in with 'use fs' (or 'use fs.read_text' "
                    "for one name), and the builtins like 'print' and 'len' need no 'use' at all");
    }
    if (strcmp(what, "'fn'") == 0 && token_text_is(t, "type")) {
        ziman_error("syntax", t->line, t->col,
                    "Ziman has no 'type' declaration -- a record is 'struct Point { x: int }' with one field per "
                    "line and no commas, and a tagged union is 'enum Shape { Circle(float) ... }'");
    }
    if (t->type == TOK_MINUS && next->type == TOK_MINUS) {
        ziman_error("syntax", t->line, t->col,
                    "'--' does not start a comment in Ziman -- use '//' to the end of the line (there is no block "
                    "comment form)");
    }
    if (t->type == TOK_EQ && next->type == TOK_GT) {
        ziman_error("syntax", t->line, t->col,
                    "Ziman has no '=>' -- a switch arm is written 'Variant(name) { ... }' with a block and no arrow, "
                    "and a lambda is 'fn(x) { x + 1 }'");
    }
}

static Token *expect(Parser *p, TokenType type, const char *what) {
    if (check(p, type)) return advance_tok(p);
    report_habit_token_pair(p, what);
    ziman_error("syntax", cur(p)->line, cur(p)->col, "expected %s, found %s", what, token_type_name(cur(p)->type));
    return NULL; /* unreachable */
}

static char *dup_token_text(Token *t) {
    char *s = ziman_xmalloc((size_t)t->length + 1);
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
        /* When this fires on a *type* position the bare rule is misleading, because the built-in
         * types really are lowercase: a reader who writes `u64`, is told to capitalise, and then
         * meets "unknown struct or enum type 'U64'" has been walked away from the answer, which is
         * `int`. That two-error dead end was the largest consumer of the fix budget in the
         * 2026-10-09 benchmark pilot (docs/reviews/llm-benchmark-2026-10-09.md, tasks.md F28). The
         * rule is true of names the program declares, so say that, and name the exceptions. */
        if (strcmp(what, "a type name") == 0) {
            ziman_error("syntax", t->line, t->col,
                        "a struct or enum name must start with an uppercase letter -- but the built-in types are "
                        "lowercase (int, float, bool, string, byte, bigint, decimal), so '%.*s' may just need one of "
                        "those instead", t->length, t->start);
        }
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
 * keyword, an uppercase identifier, `[`, `Result`, `Channel`, or `fn`.
 *
 * `fn` is here for the same reason `[` is, and is no more ambiguous: a
 * lambda is never a numeric operand, so `x < fn(...)` is not a real
 * comparison in any program that type-checks. Its absence made a
 * function type parse everywhere *except* as the first type argument of
 * a generic call -- `mapb_new<string, fn(int) -> int>()` worked (the
 * `string` got the lookahead over the line and `parse_type` handled the
 * rest), while `ab_new<fn(int) -> int>()` did not, and failed as
 * `expected a parameter name, found 'int'` from the lambda parser
 * several tokens away from the real problem. There was no workaround in
 * that position either: dropping the type argument reaches the
 * inference error, which says to "write it explicitly, e.g.
 * 'ab_new<...>(...)'" -- the exact thing the parser was refusing. So an
 * ArrayBuilder of callbacks was inexpressible. (tasks.md F45) A real
 * comparison's right-hand side can still coincidentally start with one
 * of these (`x < Point { ... }`, `x < [1, 2, 3]`), but `<`/`>` only
 * type-check between two numeric operands, so a false positive here
 * fails type-checking loudly rather than silently misbehaving.
 *
 * A lowercase identifier right after `<` needs one more look before
 * giving up on it: `qualifier.TypeName` (a cross-module type reference,
 * e.g. `ab_new<repository.Patient>()`) also starts with a lowercase
 * ident. Unlike a bare lowercase ident, this is *not* genuinely
 * ambiguous with a real comparison -- `x < y.z` is only ever a real
 * field access (`z` lowercase) since a struct field name can never
 * start uppercase, while a qualified type's second segment can never
 * start lowercase (both enforced at parse time elsewhere), so checking
 * for '.' then an uppercase ident two tokens further is a safe,
 * unambiguous test, not another heuristic. Found missing when
 * `#[derive(Json)]`-generated code first needed
 * `ab_new<some_module.SomeType>()` for a struct field whose element
 * type lives in another module -- nothing before that had ever written
 * a generic call with a qualified type argument. */
static int next_looks_like_generic_call(Parser *p) {
    Token *after_lt = &p->tokens.items[p->pos + 1];
    switch (after_lt->type) {
        case TOK_KW_INT: case TOK_KW_FLOAT: case TOK_KW_BOOL: case TOK_KW_STRING: case TOK_KW_BIGINT: case TOK_KW_DECIMAL:
        case TOK_KW_BYTE:
        case TOK_LBRACKET: case TOK_RESULT: case TOK_CHANNEL: case TOK_MAP: case TOK_SET:
        case TOK_FN:
            return 1;
        case TOK_IDENT:
            if (starts_upper(after_lt->start)) return 1;
            if (after_lt->length == 4 && memcmp(after_lt->start, "unit", 4) == 0) return 1;
            if (p->pos + 3 < p->tokens.count) {
                Token *dot = &p->tokens.items[p->pos + 2];
                Token *type_name = &p->tokens.items[p->pos + 3];
                return dot->type == TOK_DOT && type_name->type == TOK_IDENT && starts_upper(type_name->start);
            }
            return 0;
        default:
            return 0;
    }
}

/* Guarded like parse_unary and parse_block: a type is recursive syntax too (`[[[...]]]`,
 * `fn(fn(...))`, `Result<Result<...>>`), and without the check a deep one runs off the end of
 * the C stack instead of reporting it. The language server is the reason this matters more than
 * the depth suggests: `lsp_main` returns before main.c reaches ziman_run_on_big_stack, so it
 * works on whatever `ulimit -s` gives (8 MB typically) and no --stack-mb can raise it, which
 * made a 68 KB document a SIGSEGV with no diagnostic rather than a reported error. */
static Type parse_type(Parser *p) {
    ZIMAN_STACK_CHECK();
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
    /* `fn(A, B) -> R` -- a function type; the arrow and return type are left
     * off for a function that returns nothing. */
    if (match(p, TOK_FN)) {
        expect(p, TOK_LPAREN, "'('");
        TypeList params = {0};
        if (!check(p, TOK_RPAREN)) {
            for (;;) {
                type_list_push(&params, parse_type(p));
                if (!match(p, TOK_COMMA)) break;
            }
        }
        expect(p, TOK_RPAREN, "')'");
        Type ret = type_scalar(TY_VOID);
        if (match(p, TOK_ARROW)) {
            ret = parse_type(p);
        }
        return type_func(params, ret);
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
    if (match(p, TOK_MAP)) {
        expect(p, TOK_LT, "'<'");
        Type key = parse_type(p);
        expect(p, TOK_COMMA, "','");
        Type value = parse_type(p);
        expect(p, TOK_GT, "'>'");
        return type_map(key, value);
    }
    if (match(p, TOK_SET)) {
        expect(p, TOK_LT, "'<'");
        Type elem = parse_type(p);
        expect(p, TOK_GT, "'>'");
        return type_set(elem);
    }
    /* `unit` is a word, not a reserved keyword: a variable, field or parameter may be called `unit`, so it means the type only where a type
     * is expected (and the value only where no `unit` is in scope, see typecheck.c) */
    if (check(p, TOK_IDENT) && cur(p)->length == 4 && memcmp(cur(p)->start, "unit", 4) == 0) {
        advance_tok(p);
        return type_scalar(TY_UNIT);
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
            char *qualified = ziman_xmalloc(len);
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
        /* `List(Int)`, `List[Int]`, `Array[T]`, `Vec[T]` -- generic type *application* in the shape
         * other languages use. Ziman writes an array as `[T]` and applies type arguments with
         * angle brackets, and the old errors here were a positional `expected ')', found '('` that
         * named neither. Two runs of the 2026-10-09 benchmark pilot found their way to `[T]` by
         * probing rather than by being told, and both filed it as the last remaining gap
         * (docs/reviews/llm-benchmark-2026-10-09.md, tasks.md F28). */
        if (check(p, TOK_LPAREN) || check(p, TOK_LBRACKET)) {
            ziman_error("syntax", cur(p)->line, cur(p)->col,
                        "a type's arguments go in angle brackets, not '%s' -- write '%.*s<...>'; and there is no "
                        "List/Array/Vec type, because an array of T is written '[T]' (so '[int]', not 'List(int)')",
                        check(p, TOK_LPAREN) ? "(" : "[", t->length, t->start);
        }
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
static ParamList parse_params(Parser *p);
static Stmt *parse_block(Parser *p);
static Stmt *parse_if(Parser *p);
static Stmt *parse_if_value(Parser *p);
static Stmt *parse_switch(Parser *p);
static void yieldify(Stmt *s);

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

/* Parse result for a call's argument list -- see parse_call_args.
 * `names` is NULL for a positional call, else one entry per `args`
 * item, same order. Handed straight into Expr.as.call's `args`/
 * `arg_names` fields at each of this function's call sites. */
typedef struct {
    ExprList args;
    char **names;
} CallArgs;

/* A call's argument list: either all-positional (`f(1, 2)`) or
 * all-named (`f(x: 1, y: 2)`), reusing the exact `name: value` shape a
 * struct literal's fields already have (see parse_field_inits) rather
 * than inventing a second one. Which form a call uses is decided by its
 * *first* argument alone -- if it starts with `IDENT ':'`, every
 * remaining argument is required to as well; mixing the two forms in
 * one call is a syntax error. An argument that merely starts with an
 * identifier but isn't followed by ':' (`f(x)`, `f(x + 1)`) is always
 * ordinary positional -- the ':' lookahead, with backtracking when it's
 * absent, is enough to tell the two apart with no other ambiguity. */
static CallArgs parse_call_args(Parser *p, TokenType stop) {
    CallArgs result = {0};
    if (check(p, stop)) {
        return result;
    }
    int is_named = -1; /* -1 = undecided until the first argument */
    for (;;) {
        char *name = NULL;
        if (check(p, TOK_IDENT)) {
            int saved_pos = p->pos;
            Token *name_tok = advance_tok(p);
            if (match(p, TOK_COLON)) {
                name = dup_token_text(name_tok);
            } else {
                p->pos = saved_pos;
            }
        }
        int this_named = name != NULL;
        if (is_named == -1) {
            is_named = this_named;
        } else if (is_named != this_named) {
            ziman_error("syntax", cur(p)->line, cur(p)->col,
                        "a call's arguments must be either all positional or all named, not a mix");
        }
        expr_list_push(&result.args, parse_expr(p));
        if (is_named) {
            result.names = ziman_xrealloc(result.names, (size_t)result.args.count * sizeof(char *));
            result.names[result.args.count - 1] = name;
        }
        skip_newlines(p);
        if (!match(p, TOK_COMMA)) break;
        skip_newlines(p);
    }
    return result;
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

static int continues_expression(const Token *after) {
    return after->type == TOK_PLUS || after->type == TOK_MINUS || after->type == TOK_STAR || after->type == TOK_SLASH ||
           after->type == TOK_PERCENT || after->type == TOK_EQEQ || after->type == TOK_BANGEQ || after->type == TOK_LT ||
           after->type == TOK_GT || after->type == TOK_LTEQ || after->type == TOK_GTEQ || after->type == TOK_AMPAMP ||
           after->type == TOK_PIPEPIPE || after->type == TOK_DOT || after->type == TOK_LBRACKET || after->type == TOK_AS ||
           after->type == TOK_QUESTION;
}

static Expr *parse_primary(Parser *p) {
    Token *t = cur(p);

    if (match(p, TOK_INT_LIT)) {
        Expr *e = expr_new(EXPR_INT, t->line, t->col);
        errno = 0;
        e->as.int_val = strtoll(t->start, NULL, 10);
        if (errno == ERANGE) {
            if (strtoull(t->start, NULL, 10) == 9223372036854775808ULL) {
                e->as.int_val = INT64_MIN;
                e->int_min_literal = 1;
            } else {
                ziman_error("syntax", t->line, t->col,
                            "integer literal %.*s is out of range for int (the largest is 9223372036854775807; write %.*sn for a bigint)",
                            t->length, t->start, t->length, t->start);
            }
        }
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
        char *digits = ziman_xmalloc((size_t)digit_len + 1);
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
        char *digits = ziman_xmalloc((size_t)digit_len + 1);
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
        char *digits = ziman_xmalloc((size_t)digit_len + 1);
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
        /* `match value { ... }` is what most languages write; Ziman has no
         * `match`. A real call would be `match(...)`, so an identifier or a
         * literal right after the name means the switch-shaped form. */
        if (strcmp(name, "match") == 0 && (check(p, TOK_IDENT) || check(p, TOK_INT_LIT) || check(p, TOK_STRING_LIT) || check(p, TOK_TRUE) || check(p, TOK_FALSE))) {
            ziman_error("syntax", t->line, t->col,
                        "Ziman has no 'match' -- use 'switch value { Variant(name) { ... } ... }' over an enum, an Option or a Result "
                        "(one arm per variant, no default arm), or 'if Enum.is_Variant(v)' to test one variant");
        }
        /* `} elif cond {` -- Python's spelling, and C's `else if` is what Ziman wants. Guarded on the
         * closing brace before it, so an ordinary value that happens to be named `elif` is left
         * alone, the same caution the `match` check above takes. */
        if ((strcmp(name, "elif") == 0 || strcmp(name, "elsif") == 0 || strcmp(name, "elseif") == 0) &&
            p->pos > 1 && p->tokens.items[p->pos - 2].type == TOK_RBRACE) {
            ziman_error("syntax", t->line, t->col,
                        "Ziman has no '%s' -- write '} else if cond {', with the 'else' on the same line as the "
                        "brace that closes the previous 'if'", name);
        }
        /* `f"hi {n}"` -- an f-string. A real call to a function named `f` is `f(...)`, so a string
         * literal directly after the name is unambiguous. */
        if (strcmp(name, "f") == 0 && check(p, TOK_STRING_LIT)) {
            ziman_error("syntax", t->line, t->col,
                        "Ziman has no string interpolation -- use 'format(\"hi {}\", n)' to build the string, or "
                        "'printf(\"hi {}\\n\", n)' to write it straight out ('{}' is the placeholder, and '{:.2}' "
                        "fixes a float's decimals)");
        }
        /* `identity<int>(5)` -- a generic function call. Unlike a
         * struct/enum's `<`, this genuinely can't be told apart from a
         * real comparison (`x < y`) by casing alone, since a call's
         * callee is always lowercase -- see next_looks_like_generic_call. */
        if (!starts_upper(name) && check(p, TOK_LT) && next_looks_like_generic_call(p)) {
            TypeList type_args = parse_type_args(p);
            if (!check(p, TOK_LPAREN)) {
                /* `identity<int>` with no call: a generic function used as a value,
                 * its type arguments written out. */
                Expr *e = expr_new(EXPR_IDENT, t->line, t->col);
                e->as.ident_name = name;
                e->fn_ref_type_args = type_args;
                return e;
            }
            expect(p, TOK_LPAREN, "'('");
            CallArgs args = parse_call_args(p, TOK_RPAREN);
            expect(p, TOK_RPAREN, "')'");
            Expr *e = expr_new(EXPR_CALL, t->line, t->col);
            e->as.call.callee = name;
            e->as.call.args = args.args;
            e->as.call.arg_names = args.names;
            e->as.call.type_args = type_args;
            return e;
        }
        if (match(p, TOK_LPAREN)) {
            CallArgs args = parse_call_args(p, TOK_RPAREN);
            expect(p, TOK_RPAREN, "')'");
            Expr *e = expr_new(EXPR_CALL, t->line, t->col);
            e->as.call.callee = name;
            e->as.call.local_name = name;
            e->as.call.args = args.args;
            e->as.call.arg_names = args.names;
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
                    TypeList qualified_type_args = {0};
                    if (check(p, TOK_LT)) {
                        qualified_type_args = parse_type_args(p);
                    }
                    size_t len = strlen(name) + 1 + (size_t)second->length + 1;
                    char *qualified = ziman_xmalloc(len);
                    snprintf(qualified, len, "%s.%.*s", name, second->length, second->start);
                    free(name);
                    if (check(p, TOK_LBRACE)) {
                        advance_tok(p); /* '{' */
                        FieldInitList fields = parse_field_inits(p, TOK_RBRACE);
                        expect(p, TOK_RBRACE, "'}'");
                        Expr *e = expr_new(EXPR_STRUCT_LIT, t->line, t->col);
                        e->as.struct_lit.type_name = qualified;
                        e->as.struct_lit.fields = fields;
                        e->as.struct_lit.type_args = qualified_type_args;
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
                        e->as.enum_access.type_args = qualified_type_args;
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
                    TypeList qualified_type_args = parse_type_args(p);
                    if (!check(p, TOK_LPAREN)) {
                        /* `mod.identity<int>` with no call: a generic function of another
                         * module used as a value. */
                        Expr *base = expr_new(EXPR_IDENT, t->line, t->col);
                        base->as.ident_name = name;
                        Expr *fe = expr_new(EXPR_FIELD_ACCESS, t->line, t->col);
                        fe->as.field_access.base = base;
                        fe->as.field_access.field_name = dup_token_text(second);
                        fe->fn_ref_type_args = qualified_type_args;
                        return fe;
                    }
                    expect(p, TOK_LPAREN, "'('");
                    CallArgs args = parse_call_args(p, TOK_RPAREN);
                    expect(p, TOK_RPAREN, "')'");
                    size_t len = strlen(name) + 1 + (size_t)second->length + 1;
                    char *qualified = ziman_xmalloc(len);
                    snprintf(qualified, len, "%s.%.*s", name, second->length, second->start);
                    free(name);
                    Expr *e = expr_new(EXPR_CALL, t->line, t->col);
                    e->as.call.callee = qualified;
                    e->as.call.args = args.args;
                    e->as.call.arg_names = args.names;
                    e->as.call.type_args = qualified_type_args;
                    return e;
                }
                if (match(p, TOK_LPAREN)) {
                    CallArgs args = parse_call_args(p, TOK_RPAREN);
                    expect(p, TOK_RPAREN, "')'");
                    size_t len = strlen(name) + 1 + (size_t)second->length + 1;
                    char *qualified = ziman_xmalloc(len);
                    snprintf(qualified, len, "%s.%.*s", name, second->length, second->start);
                    free(name);
                    Expr *e = expr_new(EXPR_CALL, t->line, t->col);
                    e->as.call.callee = qualified;
                    e->as.call.args = args.args;
                    e->as.call.arg_names = args.names;
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
    /* `Map<K, V> { k: v, ... }` / `Set<T> { a, b, ... }` -- built as a call to the
     * builtins `$map_lit`/`$set_lit` (no source identifier can contain `$`), with the
     * explicit type arguments as the call's and the entries flattened (k1, v1, k2, ...)
     * into its arguments. `Map<K, V> {}` is the empty map. */
    if (check(p, TOK_MAP) || check(p, TOK_SET)) {
        int is_map = check(p, TOK_MAP);
        advance_tok(p);
        TypeList type_args = parse_type_args(p);
        int want = is_map ? 2 : 1;
        if (type_args.count != want) {
            ziman_error("syntax", t->line, t->col, "%s takes %d type argument(s), found %d", is_map ? "Map" : "Set", want, type_args.count);
        }
        expect(p, TOK_LBRACE, "'{'");
        skip_newlines(p);
        ExprList entries = {0};
        while (!check(p, TOK_RBRACE)) {
            expr_list_push(&entries, parse_expr(p));
            if (is_map) {
                expect(p, TOK_COLON, "':' between a map key and its value");
                skip_newlines(p);
                expr_list_push(&entries, parse_expr(p));
            }
            skip_newlines(p);
            if (!match(p, TOK_COMMA)) break;
            skip_newlines(p);
        }
        expect(p, TOK_RBRACE, "'}'");
        Expr *e = expr_new(EXPR_CALL, t->line, t->col);
        e->as.call.callee = is_map ? "$map_lit" : "$set_lit"; /* never freed or written through */
        e->as.call.args = entries;
        e->as.call.type_args = type_args;
        return e;
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

    if (match(p, TOK_FN)) {
        /* `fn(x: int) -> int { ... }`: a lambda. Parameter and return types are
         * written out like a named function's; a missing `->` means it returns
         * nothing. What it captures from the enclosing function is worked out
         * by the type checker. */
        /* A name here means someone wrote a named function inside another one. Ziman has no nested
         * functions, and the token-level complaint ("expected '(', found identifier") says nothing about
         * that or about the lambda that replaces it. */
        if (cur(p)->type == TOK_IDENT) {
            ziman_error("syntax", t->line, t->col,
                        "a function cannot be declared inside another function -- either move '%.*s' out to the "
                        "top level, or bind a lambda to a val: 'val %.*s = fn(...) -> T { ... }'",
                        cur(p)->length, cur(p)->start, cur(p)->length, cur(p)->start);
        }
        FunctionDecl *fn = ziman_xcalloc(1, sizeof(FunctionDecl));
        fn->name = (char *)"lambda";
        fn->line = t->line;
        fn->col = t->col;
        expect(p, TOK_LPAREN, "'('");
        if (!check(p, TOK_RPAREN)) {
            for (;;) {
                Token *name = expect(p, TOK_IDENT, "a parameter name");
                require_lower_start(name, "a parameter name");
                Param param = {dup_token_text(name), type_scalar(TY_VOID), 0, NULL, NULL, name->line, name->col, 0};
                if (match(p, TOK_COLON)) param.type = parse_type(p);
                else param.type_omitted = 1;
                param_list_push(&fn->params, param);
                skip_newlines(p);
                if (!match(p, TOK_COMMA)) break;
                skip_newlines(p);
            }
        }
        expect(p, TOK_RPAREN, "')'");
        fn->return_type = type_scalar(TY_VOID);
        if (match(p, TOK_ARROW)) fn->return_type = parse_type(p);
        else fn->return_omitted = 1;
        fn->body = parse_block(p);
        Expr *e = expr_new(EXPR_LAMBDA, t->line, t->col);
        e->lambda_decl = fn;
        return e;
    }
    if (t->type == TOK_IF || t->type == TOK_SWITCH) {
        /* An `if`/`switch` inside an expression (an argument, an operand, an
         * element). At the start of a statement they are statements, and as the
         * right-hand side of `val`/`var`/assignment/`return` they are
         * STMT_VALUE_BLOCKs (see parse_value_block); this is every other place. */
        advance_tok(p);
        Stmt *inner;
        if (t->type == TOK_IF) {
            inner = parse_if_value(p);
            if (inner->as.if_stmt.else_block == NULL) {
                ziman_error("syntax", inner->line, inner->col, "an 'if' used as a value needs an 'else' (every path must produce a value)");
            }
        } else {
            inner = parse_switch(p);
        }
        yieldify(inner);
        Token *after = cur(p);
        if (continues_expression(after)) {
            ziman_error("syntax", after->line, after->col,
                        "wrap the '%s' in parentheses to use its value as an operand: (%s ...) %s ...",
                        t->type == TOK_IF ? "if" : "switch", t->type == TOK_IF ? "if" : "switch", token_type_name(after->type));
        }
        Expr *e = expr_new(EXPR_VALUE_BLOCK, t->line, t->col);
        e->as.value_block.inner = inner;
        return e;
    }
    /* A common mistake lands here, and the surrounding tokens say exactly which, so say it rather than
     * naming the token that happened to stop the parser (D2 item 18).
     *
     * A trailing comma: the comma was consumed, an expression was expected, and a closing bracket turned
     * up instead. Ziman does not allow one, and "expected an expression, found ']'" leaves the reader
     * looking at the bracket rather than the comma before it. (`k++`/`k--` are caught in
     * parse_statement instead -- see there for why not here.) */
    if (p->pos > 0 && previous(p)->type == TOK_COMMA &&
        (t->type == TOK_RBRACKET || t->type == TOK_RPAREN || t->type == TOK_RBRACE)) {
        ziman_error("syntax", previous(p)->line, previous(p)->col,
                    "a trailing comma is not allowed before %s -- remove it", token_type_name(t->type));
    }
    /* An `else` that starts a line. In most C-family languages `}` and `else` may be on separate
     * lines; here a statement ends at the newline, so the `}` already closed the `if` and this
     * `else` begins a statement of its own -- which is why the bare message named the token rather
     * than the habit (docs/reviews/tasks.md C19). */
    if (t->type == TOK_ELSE) {
        ziman_error("syntax", t->line, t->col,
                    "an 'else' has to be on the same line as the brace that closes its 'if' ('} else {') -- a "
                    "statement ends at the newline, so the '}' above already finished the 'if' and this 'else' "
                    "has nothing to attach to");
    }
    /* `{"a": 1}` -- the map literal nearly every other language spells this way. Ziman has `Map`
     * and `Set` literals, but the type arguments are always written, because `{}` alone cannot say
     * what an empty one holds. */
    if (t->type == TOK_LBRACE) {
        ziman_error("syntax", t->line, t->col,
                    "a '{' cannot start an expression -- a map or set literal is written with its type arguments, "
                    "as 'Map<string, int> { \"a\": 1 }' or 'Set<int> { 1, 2 }' (always written, since '{}' alone "
                    "cannot say what an empty one holds)");
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
        } else if (check(p, TOK_QUESTION)) {
            Token *q = cur(p);
            ziman_error("syntax", q->line, q->col, "there is no postfix '?': write 'try <expression>' in front of the Option or Result instead (e.g. 'val x = try parse(s)')");
        } else {
            break;
        }
    }
    return e;
}

static Expr *parse_unary(Parser *p) {
    ZIMAN_STACK_CHECK();
    if (check(p, TOK_TRY)) {
        Token *t = advance_tok(p);
        Expr *operand = parse_unary(p);
        Expr *e = expr_new(EXPR_TRY_UNWRAP, t->line, t->col);
        e->as.try_unwrap.inner = operand;
        return e;
    }
    if (check(p, TOK_BANG) || check(p, TOK_MINUS)) {
        Token *t = advance_tok(p);
        Op op = t->type == TOK_BANG ? OP_NOT : OP_NEG;
        Expr *operand = parse_unary(p);
        if (op == OP_NEG && operand->kind == EXPR_INT && operand->int_min_literal) {
            operand->int_min_literal = 0; /* `-9223372036854775808`: the smallest int, not a negated overflow */
            operand->line = t->line;
            operand->col = t->col;
            return operand;
        }
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

/* ---- hoisting `try` out of expressions ------------------------------------------------------------------------------------------------------------------
 * `ok(try parse_int(s) + 1)` becomes `val $t1 = parse_int(s)?` followed by `ok($t1 + 1)`. Whatever the statement evaluates before the `try` (a call, an index that
 * may fail) moves into a temporary ahead of it too, so the order of effects is the one written. Only the positions that run exactly once and unconditionally are
 * handled: a val/var initializer, an assignment, an expression statement, a return value, an `if` condition, a `switch` subject and a `for` range or iterable. A
 * `try` anywhere else (an `else if` or `while` condition, the right side of `&&`/`||`, a `defer`/`spawn` argument, an `if` or `switch` used as a value) stays put and
 * the type checker refuses it. A lambda is its own function: its body is hoisted on its own. */

static int g_try_temp_counter = 0;
static char *dup_text(const char *text);

static int expr_has_try(const Expr *e);

static int fields_have_try(const FieldInitList *fields) {
    for (int i = 0; i < fields->count; i++) {
        if (expr_has_try(fields->items[i].value)) return 1;
    }
    return 0;
}

static int expr_has_try(const Expr *e) {
    switch (e->kind) {
        case EXPR_TRY_UNWRAP: return 1;
        case EXPR_UNARY: return expr_has_try(e->as.unary.operand);
        case EXPR_BINARY: return expr_has_try(e->as.binary.left) || expr_has_try(e->as.binary.right);
        case EXPR_CAST: return expr_has_try(e->as.cast.operand);
        case EXPR_FIELD_ACCESS: return expr_has_try(e->as.field_access.base);
        case EXPR_INDEX: return expr_has_try(e->as.index.array) || expr_has_try(e->as.index.index);
        case EXPR_CALL:
            for (int i = 0; i < e->as.call.args.count; i++) if (expr_has_try(e->as.call.args.items[i])) return 1;
            return 0;
        case EXPR_ENUM_ACCESS:
            for (int i = 0; i < e->as.enum_access.args.count; i++) if (expr_has_try(e->as.enum_access.args.items[i])) return 1;
            return 0;
        case EXPR_ARRAY_LIT:
            for (int i = 0; i < e->as.array_lit.elements.count; i++) if (expr_has_try(e->as.array_lit.elements.items[i])) return 1;
            return 0;
        case EXPR_STRUCT_LIT: return fields_have_try(&e->as.struct_lit.fields);
        case EXPR_WITH: return expr_has_try(e->as.with_expr.base) || fields_have_try(&e->as.with_expr.overrides);
        default: return 0;
    }
}

/* True when evaluating `e` cannot fail or have an effect, so it may stay where it is while a later `try` runs first. */
static int expr_is_pure(const Expr *e) {
    switch (e->kind) {
        case EXPR_INT: case EXPR_FLOAT: case EXPR_STRING: case EXPR_BOOL: case EXPR_UNIT: case EXPR_BIGINT: case EXPR_DECIMAL: case EXPR_BYTE:
        case EXPR_IDENT: case EXPR_LAMBDA:
            return 1;
        case EXPR_UNARY: return expr_is_pure(e->as.unary.operand);
        case EXPR_BINARY:
            return e->as.binary.op != OP_DIV && e->as.binary.op != OP_MOD && expr_is_pure(e->as.binary.left) && expr_is_pure(e->as.binary.right);
        case EXPR_FIELD_ACCESS: return expr_is_pure(e->as.field_access.base);
        case EXPR_ARRAY_LIT:
            for (int i = 0; i < e->as.array_lit.elements.count; i++) if (!expr_is_pure(e->as.array_lit.elements.items[i])) return 0;
            return 1;
        case EXPR_STRUCT_LIT:
            for (int i = 0; i < e->as.struct_lit.fields.count; i++) if (!expr_is_pure(e->as.struct_lit.fields.items[i].value)) return 0;
            return 1;
        case EXPR_ENUM_ACCESS: return e->as.enum_access.args.count == 0;
        default: return 0;
    }
}

/* Puts every child of `e` that is evaluated, in order, into `slots`; returns how many. `&&` and `||` contribute only their left operand: the right one runs
 * conditionally, so a `try` in it is not hoisted. */
static int expr_child_slots(Expr *e, Expr ****slots) {
    int n = 0, cap = 4;
    Expr ***v = ziman_xmalloc((size_t)cap * sizeof(Expr **));
#define ADD_SLOT(s) do { if (n == cap) { cap *= 2; v = ziman_xrealloc(v, (size_t)cap * sizeof(Expr **)); } v[n++] = (s); } while (0)
    switch (e->kind) {
        case EXPR_TRY_UNWRAP: ADD_SLOT(&e->as.try_unwrap.inner); break;
        case EXPR_UNARY: ADD_SLOT(&e->as.unary.operand); break;
        case EXPR_BINARY:
            ADD_SLOT(&e->as.binary.left);
            if (e->as.binary.op != OP_AND && e->as.binary.op != OP_OR) ADD_SLOT(&e->as.binary.right);
            break;
        case EXPR_CAST: ADD_SLOT(&e->as.cast.operand); break;
        case EXPR_FIELD_ACCESS: ADD_SLOT(&e->as.field_access.base); break;
        case EXPR_INDEX: ADD_SLOT(&e->as.index.array); ADD_SLOT(&e->as.index.index); break;
        case EXPR_CALL: for (int i = 0; i < e->as.call.args.count; i++) ADD_SLOT(&e->as.call.args.items[i]); break;
        case EXPR_ENUM_ACCESS: for (int i = 0; i < e->as.enum_access.args.count; i++) ADD_SLOT(&e->as.enum_access.args.items[i]); break;
        case EXPR_ARRAY_LIT: for (int i = 0; i < e->as.array_lit.elements.count; i++) ADD_SLOT(&e->as.array_lit.elements.items[i]); break;
        case EXPR_STRUCT_LIT: for (int i = 0; i < e->as.struct_lit.fields.count; i++) ADD_SLOT(&e->as.struct_lit.fields.items[i].value); break;
        case EXPR_WITH:
            ADD_SLOT(&e->as.with_expr.base);
            for (int i = 0; i < e->as.with_expr.overrides.count; i++) ADD_SLOT(&e->as.with_expr.overrides.items[i].value);
            break;
        default: break;
    }
#undef ADD_SLOT
    *slots = v;
    return n;
}

/* `val $tN = init` appended to `out`; returns the identifier that now stands for `init`. */
static Expr *hoist_into_temp(StmtList *out, Expr *init) {
    char name[32];
    snprintf(name, sizeof name, "$t%d", ++g_try_temp_counter);
    Stmt *decl = stmt_new(STMT_VAL, init->line, init->col);
    decl->as.decl.name = dup_text(name);
    decl->as.decl.init = init;
    stmt_list_push(out, decl);
    Expr *ref = expr_new(EXPR_IDENT, init->line, init->col);
    ref->as.ident_name = dup_text(name);
    return ref;
}

/* Rewrites `*slot` so that no prefix `try` is left in it, appending what has to run first to `out`. `whole` is true when `*slot` is the entire initializer of a
 * val/var or a whole expression statement, where a `try` needs no temporary of its own: `val x = try f()` is `val x = f()?`. `later` is true when something that
 * the statement evaluates after `*slot` holds a `try`: then all of `*slot` runs before it, so what is left of it that is not pure moves into a temporary too. */
static void hoist_expr(Expr **slot, StmtList *out, int whole, int later) {
    Expr *e = *slot;
    if (!expr_has_try(e)) return;
    Expr ***kids;
    int n = expr_child_slots(e, &kids);
    int last_try = -1;
    for (int i = 0; i < n; i++) {
        if (expr_has_try(*kids[i])) last_try = i;
    }
    int last = later ? n - 1 : last_try;
    for (int i = 0; i <= last; i++) {
        int child_later = later || i < last_try;
        if (expr_has_try(*kids[i])) hoist_expr(kids[i], out, 0, child_later);
        else if (!expr_is_pure(*kids[i])) *kids[i] = hoist_into_temp(out, *kids[i]);
    }
    free(kids);
    if (e->kind == EXPR_TRY_UNWRAP) {
        if (!whole) *slot = hoist_into_temp(out, e);
    } else if (later && !expr_is_pure(e)) {
        *slot = hoist_into_temp(out, e);
    }
}

/* `*slot` is evaluated before something that holds a `try` (`later`): hoist what it holds. */
static void hoist_operand(Expr **slot, StmtList *out, int later) {
    if (expr_has_try(*slot)) hoist_expr(slot, out, 0, later);
    else if (later && !expr_is_pure(*slot)) *slot = hoist_into_temp(out, *slot);
}

static void hoist_stmt(Stmt *s, StmtList *out) {
    switch (s->kind) {
        case STMT_VAL: case STMT_VAR: hoist_expr(&s->as.decl.init, out, 1, 0); break;
        case STMT_EXPR: hoist_expr(&s->as.expr_stmt.expr, out, 1, 0); break;
        case STMT_ASSIGN: hoist_expr(&s->as.assign.value, out, 0, 0); break;
        case STMT_RETURN: if (s->as.ret.value != NULL) hoist_expr(&s->as.ret.value, out, 0, 0); break;
        case STMT_IF: hoist_expr(&s->as.if_stmt.cond, out, 0, 0); break;
        case STMT_SWITCH: hoist_expr(&s->as.switch_stmt.scrutinee, out, 0, 0); break;
        case STMT_FOR:
            if (s->as.for_stmt.is_range) {
                hoist_operand(&s->as.for_stmt.range_start, out, expr_has_try(s->as.for_stmt.range_end));
                hoist_expr(&s->as.for_stmt.range_end, out, 0, 0);
            } else {
                hoist_expr(&s->as.for_stmt.iterable, out, 0, 0);
            }
            break;
        default: break;
    }
}

static Stmt *parse_statement(Parser *p);

static Stmt *parse_block(Parser *p) {
    ZIMAN_STACK_CHECK();
    Token *open = expect(p, TOK_LBRACE, "'{'");
    Stmt *block = stmt_new(STMT_BLOCK, open->line, open->col);
    skip_newlines(p);
    /* One statement at a time behind a recovery point, the same shape the top level already uses for one
     * declaration at a time. Without this the parser recovered only at declaration boundaries, so two
     * mistakes in one function cost two compile cycles -- which for a file of any size is most of them,
     * and is what D2 item 27 measured as eight mistakes costing eight runs.
     *
     * Statements are newline-delimited here, so the synchronisation point is the next line: skip the rest
     * of the line the failed statement started on and carry on. That is imperfect by nature -- a statement
     * spanning several lines resumes mid-expression and may complain again -- but a second, poorer message
     * after a real one costs the reader much less than being shown one mistake at a time. The top-level
     * recovery remains the backstop when a block's braces end up unbalanced.
     *
     * TOK_EOF ends the loop as well as `}`: after recovery the parser can reach the end of the file with
     * the brace never found, and without this the loop would spin there for ever. */
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        jmp_buf jb;
        volatile int start_pos = p->pos;
        ziman_recovery_push(&jb);
        if (setjmp(jb) != 0) {
            /* The line parsing actually stopped on, not the one the statement began on. For a statement
             * spanning several lines those differ, and synchronising on the opening line leaves the
             * position back at the offending construct, which then reports the identical error a second
             * time -- `concat_arrays<int>(\n [1, 2,],\n ...)` did exactly that before this. */
            int failed_line = cur(p)->line;
            if (p->pos <= start_pos) p->pos = start_pos + 1; /* always make progress, or this loops */
            while (!check(p, TOK_EOF) && !check(p, TOK_RBRACE) && cur(p)->line == failed_line) advance_tok(p);
            skip_newlines(p);
            continue;
        }
        Stmt *s = parse_statement(p);
        ziman_recovery_pop();
        hoist_stmt(s, &block->as.block.stmts);
        stmt_list_push(&block->as.block.stmts, s);
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return block;
}

static Stmt *parse_if(Parser *p);
static Stmt *parse_if_value(Parser *p);
static Stmt *parse_switch(Parser *p);

/* In an `if`/`switch` used as a value, each arm's last statement is its value:
 * a bare expression becomes a STMT_YIELD, and a trailing `if`/`switch` is
 * converted the same way, recursively (an `else if` chain, or a nested value).
 * Anything else that ends an arm (return, break, continue) is left alone -- the
 * type checker requires it to leave rather than fall off the end. */
static void yieldify(Stmt *s) {
    switch (s->kind) {
        case STMT_BLOCK: {
            int n = s->as.block.stmts.count;
            if (n == 0) return;
            Stmt *last = s->as.block.stmts.items[n - 1];
            if (last->kind == STMT_EXPR) {
                Stmt *y = stmt_new(STMT_YIELD, last->line, last->col);
                y->as.yield_stmt.value = last->as.expr_stmt.expr;
                s->as.block.stmts.items[n - 1] = y;
            } else if (last->kind == STMT_IF || last->kind == STMT_SWITCH) {
                yieldify(last);
            }
            return;
        }
        case STMT_IF:
            yieldify(s->as.if_stmt.then_block);
            if (s->as.if_stmt.else_block != NULL) yieldify(s->as.if_stmt.else_block);
            return;
        case STMT_SWITCH:
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) yieldify(s->as.switch_stmt.arms[i].body);
            return;
        default:
            return;
    }
}

/* If the next token starts an `if` or `switch`, parses it as a value and wraps
 * it in a STMT_VALUE_BLOCK standing in for a `val`/`var`/assignment/`return`;
 * otherwise returns NULL and consumes nothing. */
static Stmt *parse_value_block(Parser *p, StmtKind target, Token *start, char *name, int has_declared_type, Type declared_type) {
    Stmt *inner;
    if (match(p, TOK_IF)) {
        inner = parse_if_value(p);
        if (inner->as.if_stmt.else_block == NULL) {
            ziman_error("syntax", inner->line, inner->col, "an 'if' used as a value needs an 'else' (every path must produce a value)");
        }
    } else if (match(p, TOK_SWITCH)) {
        inner = parse_switch(p);
    } else {
        return NULL;
    }
    yieldify(inner);
    if (continues_expression(cur(p))) {
        Token *after = cur(p);
        ziman_error("syntax", after->line, after->col,
                    "an 'if'/'switch' used as a value must be the whole right-hand side here; wrap it in parentheses to use it as an operand: (if ...) %s ...",
                    token_type_name(after->type));
    }
    Stmt *s = stmt_new(STMT_VALUE_BLOCK, start->line, start->col);
    s->as.value_block.target = target;
    s->as.value_block.name = name;
    s->as.value_block.has_declared_type = has_declared_type;
    s->as.value_block.declared_type = declared_type;
    s->as.value_block.inner = inner;
    return s;
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
    Stmt *value = parse_value_block(p, kind, kw, s->as.decl.name, s->as.decl.has_declared_type, s->as.decl.declared_type);
    if (value != NULL) return value;
    s->as.decl.init = parse_expr(p);
    return s;
}

/* `[pub] val NAME[: Type] = <expr>` at module top level -- same grammar as
 * parse_decl's local `val`, minus `var` (no mutable globals, see
 * docs/DESIGN.md's "Global val" entry) and minus being wrapped in a Stmt,
 * since a top-level declaration lives in Module.vals, not a function
 * body's statement list. */
static GlobalValDecl *parse_global_val_decl(Parser *p, Visibility vis) {
    Token *kw = expect(p, TOK_VAL, "'val'");
    Token *name_tok = expect(p, TOK_IDENT, "a global value name");
    require_lower_start(name_tok, "a global value name");
    GlobalValDecl *v = ziman_xcalloc(1, sizeof(GlobalValDecl));
    v->name = dup_token_text(name_tok);
    v->visibility = vis;
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

/* ---- `if Some(x) = expr { ... }`: a condition that tests a variant and binds its payload ----
 *
 * The condition is a chain of `&&`-joined parts, each either an ordinary boolean or a pattern `Variant(name) = expr`
 * (`Variant = expr` to test without binding, `_` to ignore the payload). It is desugared here, in the parser, to
 * nested ordinary `if`s over two synthetic calls, `$is_variant(tmp, "Variant")` and `$unwrap_variant(tmp, "Variant")`,
 * which the type checker resolves from the type of `tmp` (an enum, or Result's Ok/Err) -- so no later pass knows this
 * construct exists. Each part is evaluated only if the parts before it held, and a name bound by one is visible in the
 * parts after it and in the block. An `else` runs when any part failed, through a hidden flag set just before the block.
 * It is a statement only: as a value (`val x = if Some(y) = z ...`) it is an error. */

static char *dup_text(const char *text) {
    size_t n = strlen(text) + 1;
    char *out = ziman_xmalloc(n);
    memcpy(out, text, n);
    return out;
}

typedef struct {
    int is_pattern;
    char *variant;
    char *binder; /* NULL when the pattern binds nothing */
    Expr *expr;   /* the boolean, or the right-hand side of the pattern */
    int line, col;
} IfPart;

static int g_if_bind_counter = 0;

static Expr *parse_mk_string(const char *text, int line, int col) {
    Expr *e = expr_new(EXPR_STRING, line, col);
    e->as.string_val = dup_text(text);
    return e;
}

static Expr *parse_mk_ident(const char *name, int line, int col) {
    Expr *e = expr_new(EXPR_IDENT, line, col);
    e->as.ident_name = dup_text(name);
    return e;
}

static Expr *parse_mk_variant_call(const char *callee, const char *hidden, const char *variant, int line, int col) {
    Expr *e = expr_new(EXPR_CALL, line, col);
    e->as.call.callee = dup_text(callee);
    expr_list_push(&e->as.call.args, parse_mk_ident(hidden, line, col));
    expr_list_push(&e->as.call.args, parse_mk_string(variant, line, col));
    return e;
}

/* Whether the tokens from here to the `{` that opens the block hold a lone `=` outside any brackets: the mark of a
 * pattern, since an assignment is not an expression. */
static int condition_has_pattern(Parser *p) {
    int depth = 0;
    for (int i = p->pos; i < p->tokens.count; i++) {
        TokenType t = p->tokens.items[i].type;
        if (t == TOK_EOF || t == TOK_NEWLINE) return 0;
        if (t == TOK_LPAREN || t == TOK_LBRACKET) depth++;
        else if (t == TOK_RPAREN || t == TOK_RBRACKET) depth--;
        else if (t == TOK_LBRACE && depth == 0) return 0;
        else if (t == TOK_EQ && depth == 0) return 1;
    }
    return 0;
}

static int at_pattern(Parser *p) {
    Token *t = cur(p);
    if (t->type != TOK_IDENT || t->length == 0 || !(t->start[0] >= 'A' && t->start[0] <= 'Z')) return 0;
    Token *n = &p->tokens.items[p->pos + 1];
    if (n->type == TOK_EQ) return 1;
    return n->type == TOK_LPAREN && p->tokens.items[p->pos + 2].type == TOK_IDENT && p->tokens.items[p->pos + 3].type == TOK_RPAREN &&
           p->tokens.items[p->pos + 4].type == TOK_EQ;
}

static Stmt *parse_if_mode(Parser *p, int allow_binding);

static Stmt *parse_if_binding(Parser *p, Token *kw, int allow_binding) {
    if (!allow_binding) {
        ziman_error("syntax", kw->line, kw->col, "an 'if' that binds a pattern (`if Some(x) = ...`) is a statement; it cannot be used as a value");
    }
    IfPart *parts = NULL;
    int n = 0;
    for (;;) {
        parts = ziman_xrealloc(parts, (size_t)(n + 1) * sizeof(IfPart));
        IfPart *part = &parts[n++];
        memset(part, 0, sizeof *part);
        part->line = cur(p)->line;
        part->col = cur(p)->col;
        if (at_pattern(p)) {
            Token *variant = advance_tok(p);
            part->is_pattern = 1;
            part->variant = dup_token_text(variant);
            if (match(p, TOK_LPAREN)) {
                Token *binder = advance_tok(p);
                if (!(binder->length == 1 && binder->start[0] == '_')) part->binder = dup_token_text(binder);
                expect(p, TOK_RPAREN, "')'");
            }
            expect(p, TOK_EQ, "'='");
            part->expr = parse_equality(p);
        } else {
            part->expr = parse_equality(p);
        }
        if (!match(p, TOK_AMPAMP)) break;
    }
    if (check(p, TOK_PIPEPIPE)) {
        ziman_error("syntax", cur(p)->line, cur(p)->col, "'||' cannot join a pattern condition; only '&&' can (or wrap the boolean in parentheses)");
    }
    Stmt *then_block = parse_block(p);
    Stmt *else_stmt = NULL;
    if (match(p, TOK_ELSE)) {
        else_stmt = match(p, TOK_IF) ? parse_if_mode(p, allow_binding) : parse_block(p);
    }

    int id = g_if_bind_counter++;
    if (n == 1 && else_stmt != NULL) {
        /* one pattern with an else: `{ val tmp = e; if is(tmp) { val x = unwrap(tmp); then } else { else } }`, plain nested
         * ifs, so the checks that every path returns see through it */
        IfPart *part = &parts[0];
        char hidden[48];
        snprintf(hidden, sizeof hidden, "$ifb%d_0", id);
        Stmt *body = stmt_new(STMT_BLOCK, part->line, part->col);
        if (part->binder != NULL) {
            Stmt *decl = stmt_new(STMT_VAL, part->line, part->col);
            decl->as.decl.name = part->binder;
            decl->as.decl.init = parse_mk_variant_call("$unwrap_variant", hidden, part->variant, part->line, part->col);
            stmt_list_push(&body->as.block.stmts, decl);
        }
        stmt_list_push(&body->as.block.stmts, then_block);
        Stmt *tmp = stmt_new(STMT_VAL, part->line, part->col);
        tmp->as.decl.name = dup_text(hidden);
        tmp->as.decl.init = part->expr;
        Stmt *ifs = stmt_new(STMT_IF, part->line, part->col);
        ifs->as.if_stmt.cond = parse_mk_variant_call("$is_variant", hidden, part->variant, part->line, part->col);
        ifs->as.if_stmt.then_block = body;
        ifs->as.if_stmt.else_block = else_stmt;
        Stmt *block = stmt_new(STMT_BLOCK, part->line, part->col);
        stmt_list_push(&block->as.block.stmts, tmp);
        stmt_list_push(&block->as.block.stmts, ifs);
        return block;
    }
    char flag[48];
    snprintf(flag, sizeof flag, "$ifm%d", id);

    /* the innermost block: the user's, after recording that every part held */
    Stmt *inner = then_block;
    if (else_stmt != NULL) {
        Stmt *wrapped = stmt_new(STMT_BLOCK, then_block->line, then_block->col);
        Stmt *mark = stmt_new(STMT_ASSIGN, then_block->line, then_block->col);
        mark->as.assign.name = dup_text(flag);
        Expr *yes = expr_new(EXPR_BOOL, then_block->line, then_block->col);
        yes->as.bool_val = 1;
        mark->as.assign.value = yes;
        stmt_list_push(&wrapped->as.block.stmts, mark);
        stmt_list_push(&wrapped->as.block.stmts, then_block);
        inner = wrapped;
    }
    for (int i = n - 1; i >= 0; i--) {
        IfPart *part = &parts[i];
        if (!part->is_pattern) {
            Stmt *ifs = stmt_new(STMT_IF, part->line, part->col);
            ifs->as.if_stmt.cond = part->expr;
            ifs->as.if_stmt.then_block = inner;
            Stmt *block = stmt_new(STMT_BLOCK, part->line, part->col);
            stmt_list_push(&block->as.block.stmts, ifs);
            inner = block;
            continue;
        }
        char hidden[48];
        snprintf(hidden, sizeof hidden, "$ifb%d_%d", id, i);
        Stmt *body = stmt_new(STMT_BLOCK, part->line, part->col);
        if (part->binder != NULL) {
            Stmt *decl = stmt_new(STMT_VAL, part->line, part->col);
            decl->as.decl.name = part->binder;
            decl->as.decl.init = parse_mk_variant_call("$unwrap_variant", hidden, part->variant, part->line, part->col);
            stmt_list_push(&body->as.block.stmts, decl);
        }
        stmt_list_push(&body->as.block.stmts, inner);
        Stmt *tmp = stmt_new(STMT_VAL, part->line, part->col);
        tmp->as.decl.name = dup_text(hidden);
        tmp->as.decl.init = part->expr;
        Stmt *ifs = stmt_new(STMT_IF, part->line, part->col);
        ifs->as.if_stmt.cond = parse_mk_variant_call("$is_variant", hidden, part->variant, part->line, part->col);
        ifs->as.if_stmt.then_block = body;
        Stmt *block = stmt_new(STMT_BLOCK, part->line, part->col);
        stmt_list_push(&block->as.block.stmts, tmp);
        stmt_list_push(&block->as.block.stmts, ifs);
        inner = block;
    }
    if (else_stmt == NULL) return inner;

    Stmt *outer = stmt_new(STMT_BLOCK, kw->line, kw->col);
    Stmt *flag_decl = stmt_new(STMT_VAR, kw->line, kw->col);
    flag_decl->as.decl.name = dup_text(flag);
    Expr *no = expr_new(EXPR_BOOL, kw->line, kw->col);
    no->as.bool_val = 0;
    flag_decl->as.decl.init = no;
    stmt_list_push(&outer->as.block.stmts, flag_decl);
    stmt_list_push(&outer->as.block.stmts, inner);
    Stmt *otherwise = stmt_new(STMT_IF, kw->line, kw->col);
    Expr *neg = expr_new(EXPR_UNARY, kw->line, kw->col);
    neg->as.unary.op = OP_NOT;
    neg->as.unary.operand = parse_mk_ident(flag, kw->line, kw->col);
    otherwise->as.if_stmt.cond = neg;
    /* `then_block` must always be a real STMT_BLOCK -- every other producer of one (an ordinary
     * `{ ... }`) already guarantees this, and check_block (typecheck.c) reads `block->as.block`
     * through the union with no kind check, unlike else_block's own STMT_IF-aware handling. `else
     * if ...` (not `else { ... }`) hands back a bare STMT_IF here, not a block -- wrap it, rather
     * than reinterpreting its own if_stmt fields as a stmt list's count/items and reading garbage. */
    if (else_stmt->kind == STMT_BLOCK) {
        otherwise->as.if_stmt.then_block = else_stmt;
    } else {
        Stmt *wrapped_else = stmt_new(STMT_BLOCK, else_stmt->line, else_stmt->col);
        stmt_list_push(&wrapped_else->as.block.stmts, else_stmt);
        otherwise->as.if_stmt.then_block = wrapped_else;
    }
    stmt_list_push(&outer->as.block.stmts, otherwise);
    return outer;
}

static Stmt *parse_if_mode(Parser *p, int allow_binding) {
    Token *kw = previous(p);
    if (condition_has_pattern(p)) return parse_if_binding(p, kw, allow_binding);
    Stmt *s = stmt_new(STMT_IF, kw->line, kw->col);
    s->as.if_stmt.cond = parse_expr(p);
    s->as.if_stmt.then_block = parse_block(p);
    /* `else { ... }` and `else if ... { ... }` chains stay on the same
     * line as the closing brace before them, so no newline-skipping here. */
    if (match(p, TOK_ELSE)) {
        if (match(p, TOK_IF)) {
            s->as.if_stmt.else_block = parse_if_mode(p, allow_binding);
        } else {
            s->as.if_stmt.else_block = parse_block(p);
        }
    }
    return s;
}

static Stmt *parse_if(Parser *p) { return parse_if_mode(p, 1); }
static Stmt *parse_if_value(Parser *p) { return parse_if_mode(p, 0); }

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

/* `switch expr { Variant[(binder)] { body } ... }` -- see ast.h's
 * switch_stmt comment. Variant names are bare (the scrutinee's type says
 * which enum they belong to); casing tells them from binders. */
static Stmt *parse_switch(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_SWITCH, kw->line, kw->col);
    s->as.switch_stmt.scrutinee = parse_expr(p);
    expect(p, TOK_LBRACE, "'{'");
    skip_newlines(p);
    int capacity = 0;
    while (!check(p, TOK_RBRACE)) {
        Token *variant_tok = expect(p, TOK_IDENT, "a variant name");
        require_upper_start(variant_tok, "a variant name");
        SwitchArm arm;
        memset(&arm, 0, sizeof arm);
        arm.line = variant_tok->line;
        arm.col = variant_tok->col;
        arm.variant = dup_token_text(variant_tok);
        if (match(p, TOK_LPAREN)) {
            Token *binder_tok = expect(p, TOK_IDENT, "a name for the variant's payload");
            require_lower_start(binder_tok, "a payload name");
            arm.binder = dup_token_text(binder_tok);
            expect(p, TOK_RPAREN, "')'");
        }
        arm.body = parse_block(p);
        if (s->as.switch_stmt.arm_count == capacity) {
            capacity = capacity == 0 ? 4 : capacity * 2;
            s->as.switch_stmt.arms = ziman_xrealloc(s->as.switch_stmt.arms, sizeof(SwitchArm) * (size_t)capacity);
        }
        s->as.switch_stmt.arms[s->as.switch_stmt.arm_count++] = arm;
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    if (s->as.switch_stmt.arm_count == 0) {
        ziman_error("syntax", kw->line, kw->col, "a 'switch' needs at least one arm");
    }
    return s;
}

static Stmt *parse_return(Parser *p) {
    Token *kw = previous(p);
    Stmt *s = stmt_new(STMT_RETURN, kw->line, kw->col);
    if (!check(p, TOK_NEWLINE) && !check(p, TOK_RBRACE)) {
        Type no_type;
        memset(&no_type, 0, sizeof no_type);
        Stmt *value = parse_value_block(p, STMT_RETURN, kw, NULL, 0, no_type);
        if (value != NULL) return value;
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
        char *qualified = ziman_xmalloc(len);
        snprintf(qualified, len, "%s.%.*s", callee, second->length, second->start);
        free(callee);
        callee = qualified;
    }
    TypeList type_args = {0};
    if (check(p, TOK_LT) && next_looks_like_generic_call(p)) {
        type_args = parse_type_args(p);
    }
    expect(p, TOK_LPAREN, "'('");
    ExprList args = parse_comma_separated(p, TOK_RPAREN);
    expect(p, TOK_RPAREN, "')'");
    Stmt *s = stmt_new(STMT_SPAWN, kw->line, kw->col);
    s->as.spawn_stmt.callee = callee;
    s->as.spawn_stmt.args = args;
    s->as.spawn_stmt.type_args = type_args;
    /* `on_error` is a word here, not a keyword: `spawn f(x) on_error handler` */
    if (check(p, TOK_IDENT) && cur(p)->length == 8 && memcmp(cur(p)->start, "on_error", 8) == 0) {
        advance_tok(p);
        s->as.spawn_stmt.on_error = parse_expr(p);
    }
    return s;
}

/* `defer <call>` -- a single function call, its result (if any) thrown
 * away. Only a plain call is accepted: not `x?`, not a block. See
 * docs/DESIGN.md's "defer" entry for why the form is this narrow. */
static Stmt *parse_defer(Parser *p) {
    Token *kw = previous(p);
    Expr *call = parse_expr(p);
    if (call->kind != EXPR_CALL) {
        ziman_error("syntax", kw->line, kw->col, "'defer' takes a single function call, e.g. `defer close(conn)`");
    }
    Stmt *s = stmt_new(STMT_DEFER, kw->line, kw->col);
    s->as.defer_stmt.call = call;
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
    if (match(p, TOK_DEFER)) return parse_defer(p);
    if (match(p, TOK_SWITCH)) return parse_switch(p);
    if (match(p, TOK_RETURN)) return parse_return(p);

    /* `IDENT = expr` (assignment) vs. any other expression statement
     * (currently only calls are useful here, e.g. `print(x)`). Both start
     * with an expression, so parse one and see what follows. */
    Token *start = cur(p);
    /* `k++` / `k--`, checked before parsing rather than from the expression parser's failure, because the
     * two reach different places and only one of them fails: `k++` runs out of operand and lands in
     * primary()'s "expected an expression", while `k--` parses as `k - (-print(...))` and surfaces much
     * later as "cannot use a void value as an operand", pointing at the wrong line entirely. Spotting the
     * shape here catches both and says the same thing about them. Ziman has no increment operator. */
    if (start->type == TOK_IDENT &&
        ((check_ahead(p, 1, TOK_PLUS) && check_ahead(p, 2, TOK_PLUS)) ||
         (check_ahead(p, 1, TOK_MINUS) && check_ahead(p, 2, TOK_MINUS)))) {
        int plus = check_ahead(p, 1, TOK_PLUS);
        ziman_error("syntax", start->line, start->col,
                    "there is no '%s' operator -- write '%.*s = %.*s %c 1' (and declare it with 'var', not 'val')",
                    plus ? "++" : "--", start->length, start->start, start->length, start->start,
                    plus ? '+' : '-');
    }
    /* `let x: int = 1` / `const`, `mut`. The un-annotated `let x = 1` already gets a good message
     * from the type checker, but with an annotation the parse fails first -- on the ':' -- so that
     * message never runs, and the most common binding habit loses its fix exactly when a reader has
     * written the fuller form. The 2026-10-09 benchmark holdout found it (tasks.md F28): "expected
     * an expression, found ':'", with no mention of `let` at all. */
    if (start->type == TOK_IDENT && check_ahead(p, 1, TOK_IDENT) && check_ahead(p, 2, TOK_COLON) &&
        (token_text_is(start, "let") || token_text_is(start, "const") || token_text_is(start, "mut"))) {
        ziman_error("syntax", start->line, start->col,
                    "Ziman has no '%.*s' -- declare with 'val' (never reassigned) or 'var' (may be), and the type "
                    "comes after the name: 'val x: int = 1'", start->length, start->start);
    }
    Expr *first = parse_expr(p);
    if (match(p, TOK_EQ)) {
        if (first->kind != EXPR_IDENT) {
            /* The shape of the left-hand side says which mistake it is, and both have a one-line repair
             * that "must be a variable name" does not hint at. Arrays and structs are values here, so
             * neither is assigned into -- a copy is made with a different element or field. Worth naming
             * because these are two of the four most likely day-one mistakes (D2 item 18). */
            if (first->kind == EXPR_INDEX) {
                ziman_error("syntax", start->line, start->col,
                            "an array element cannot be assigned: arrays are values, not mutable containers -- "
                            "write 'arr = set(arr, i, value)' (and see ArrayBuilder for building one up in a loop)");
            }
            if (first->kind == EXPR_FIELD_ACCESS) {
                ziman_error("syntax", start->line, start->col,
                            "a field cannot be assigned: a struct is a value -- write 'p = with p { field: value }' "
                            "to make a copy with that field changed");
            }
            ziman_error("syntax", start->line, start->col, "left-hand side of assignment must be a variable name");
        }
        Stmt *s = stmt_new(STMT_ASSIGN, start->line, start->col);
        s->as.assign.name = first->as.ident_name;
        Type no_type;
        memset(&no_type, 0, sizeof no_type);
        Stmt *value = parse_value_block(p, STMT_ASSIGN, start, first->as.ident_name, 0, no_type);
        if (value != NULL) return value;
        s->as.assign.value = parse_expr(p);
        return s;
    }
    Stmt *s = stmt_new(STMT_EXPR, start->line, start->col);
    s->as.expr_stmt.expr = first;
    return s;
}

/* Builds a fresh, globally unique global-env key for a parameter's
 * default value (see ast.h's Param.default_key) -- `$` can never appear
 * in a real Ziman identifier (see the lexer's identifier rule), so
 * appending it guarantees this can never collide with any name a
 * program could actually write, without needing the key to be
 * human-meaningful at all. A single process-lifetime counter, not one
 * scoped to this Parser instance, since a multi-file program parses
 * each file through its own fresh Parser and every file's keys still
 * have to be unique against each other once merged into one flat
 * g_global_env. */
static int g_param_default_counter = 0;

static char *param_default_key(const char *param_name, int name_len) {
    char *key = ziman_xmalloc((size_t)name_len + 32);
    sprintf(key, "%.*s$default$%d", name_len, param_name, g_param_default_counter++);
    return key;
}

/* A trailing `...` after a parameter's type marks it variadic --
 * lexed as an ordinary `TOK_DOTDOT` (the same token a for-loop range's
 * `..` already produces) immediately followed by a `TOK_DOT`, rather
 * than a dedicated token: no lexer change needed, and the two can
 * never be confused, since a range's `..` only ever appears in a
 * `for x in start..end` header, a position this never overlaps with.
 * Only the last parameter may be variadic -- checked here, not left
 * for typecheck to catch later, since it's a purely syntactic rule
 * (nothing about types or scope is involved). A trailing `= expr`
 * instead marks a default value -- legal on any parameter syntactically
 * (typecheck.c's check_function is what restricts it to Option<T>
 * parameters, since that's a type-level rule, not a syntactic one),
 * except a variadic parameter, rejected right here since the two can
 * never combine meaningfully. */
static ParamList parse_params(Parser *p) {
    ParamList params = {0};
    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            Token *param_name = expect(p, TOK_IDENT, "a parameter name");
            require_lower_start(param_name, "a parameter name");
            expect(p, TOK_COLON, "':'");
            Type param_type = parse_type(p);
            int is_variadic = 0;
            if (check(p, TOK_DOTDOT)) {
                int saved_pos = p->pos;
                advance_tok(p); /* '..' */
                if (match(p, TOK_DOT)) {
                    is_variadic = 1;
                } else {
                    p->pos = saved_pos; /* not '...' after all -- leave it */
                }
            }
            Expr *default_expr = NULL;
            char *default_key = NULL;
            if (match(p, TOK_EQ)) {
                if (is_variadic) {
                    ziman_error("syntax", param_name->line, param_name->col, "a variadic parameter cannot have a default value");
                }
                default_expr = parse_expr(p);
                default_key = param_default_key(param_name->start, param_name->length);
            }
            Param param = {dup_token_text(param_name), param_type, is_variadic, default_expr, default_key, param_name->line, param_name->col, 0};
            param_list_push(&params, param);
            skip_newlines(p);
            if (!match(p, TOK_COMMA)) break;
            if (is_variadic) {
                ziman_error("syntax", param.line, param.col, "a variadic parameter ('...') must be the last parameter");
            }
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

    FunctionDecl *fn = ziman_xcalloc(1, sizeof(FunctionDecl));
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

/* `native fn name[<TypeParams>](params) -> type`, no body at all -- its
 * implementation is a C function registered by module path + name,
 * looked up at call time. The type checker treats its signature exactly
 * like any other function's (see check_function's `is_native` early-
 * return); the interpreter reports a clear "no registered
 * implementation" error if nothing was registered for it (see
 * interpreter.c) -- no real native module exists to back one yet, this
 * is the language mechanism only. Optional type parameters mirror
 * parse_function's own handling exactly (`<T>`, `reclassify_type_params`
 * over params/return_type so a bare `T` inside either one is recognized
 * as this declaration's own type parameter, not an unrelated type name)
 * -- minus the body-specific step, since a native fn has none to walk. A
 * generic native fn's C implementation never needs to know which
 * concrete type a call instantiated it with to do its own job: Value is
 * already runtime-type-erased regardless (see docs/DESIGN.md's
 * "Generics" entry), the same reason ordinary generic structs/enums
 * need no monomorphization either. */
static FunctionDecl *parse_native_function(Parser *p) {
    Token *kw = expect(p, TOK_NATIVE, "'native'");
    expect(p, TOK_FN, "'fn'");
    Token *name_tok = expect(p, TOK_IDENT, "a function name");
    require_lower_start(name_tok, "a function name");

    FunctionDecl *fn = ziman_xcalloc(1, sizeof(FunctionDecl));
    fn->name = dup_token_text(name_tok);
    fn->is_native = 1;
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
                } else if (bound_tok->length == 3 && strncmp(bound_tok->start, "Ord", 3) == 0) {
                    bound |= BOUND_ORD;
                } else {
                    ziman_error("syntax", bound_tok->line, bound_tok->col,
                                "unknown bound '%.*s' (only 'Eq', 'Hash' and 'Ord' are supported)", bound_tok->length, bound_tok->start);
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
    ZIMAN_STACK_CHECK();
    if (t->kind == TY_ARRAY) { reclassify_type_params(t->elem, type_params); return; }
    if (t->kind == TY_RESULT) {
        reclassify_type_params(t->elem, type_params);
        reclassify_type_params(t->elem_err, type_params);
        return;
    }
    if (t->kind == TY_CHANNEL || t->kind == TY_SET) { reclassify_type_params(t->elem, type_params); return; }
    if (t->kind == TY_MAP) {
        reclassify_type_params(t->elem, type_params);
        reclassify_type_params(t->elem_err, type_params);
        return;
    }
    if (t->kind == TY_FUNC) {
        for (int i = 0; i < t->type_args.count; i++) reclassify_type_params(&t->type_args.items[i], type_params);
        reclassify_type_params(t->elem, type_params);
        return;
    }
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
    for (int i = 0; i < e->fn_ref_type_args.count; i++) reclassify_type_params(&e->fn_ref_type_args.items[i], type_params);
    switch (e->kind) {
        case EXPR_INT: case EXPR_FLOAT: case EXPR_BIGINT: case EXPR_DECIMAL: case EXPR_BYTE: case EXPR_STRING: case EXPR_BOOL: case EXPR_UNIT: case EXPR_IDENT:
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
        case EXPR_TRY_UNWRAP:
            reclassify_type_params_in_expr(e->as.try_unwrap.inner, type_params);
            break;
        case EXPR_VALUE_BLOCK:
            reclassify_type_params_in_stmt(e->as.value_block.inner, type_params);
            break;
        case EXPR_LAMBDA: {
            FunctionDecl *lam = e->lambda_decl;
            for (int i = 0; i < lam->params.count; i++) reclassify_type_params(&lam->params.items[i].type, type_params);
            reclassify_type_params(&lam->return_type, type_params);
            reclassify_type_params_in_stmt(lam->body, type_params);
            break;
        }
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
            for (int i = 0; i < s->as.spawn_stmt.type_args.count; i++) reclassify_type_params(&s->as.spawn_stmt.type_args.items[i], type_params);
            for (int i = 0; i < s->as.spawn_stmt.args.count; i++) reclassify_type_params_in_expr(s->as.spawn_stmt.args.items[i], type_params);
            if (s->as.spawn_stmt.on_error != NULL) reclassify_type_params_in_expr(s->as.spawn_stmt.on_error, type_params);
            break;
        case STMT_RETURN:
            if (s->as.ret.value != NULL) reclassify_type_params_in_expr(s->as.ret.value, type_params);
            break;
        case STMT_EXPR:
            reclassify_type_params_in_expr(s->as.expr_stmt.expr, type_params);
            break;
        case STMT_DEFER:
            reclassify_type_params_in_expr(s->as.defer_stmt.call, type_params);
            break;
        case STMT_SWITCH:
            reclassify_type_params_in_expr(s->as.switch_stmt.scrutinee, type_params);
            for (int i = 0; i < s->as.switch_stmt.arm_count; i++) reclassify_type_params_in_stmt(s->as.switch_stmt.arms[i].body, type_params);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmts.count; i++) reclassify_type_params_in_stmt(s->as.block.stmts.items[i], type_params);
            break;
        case STMT_VALUE_BLOCK:
            if (s->as.value_block.has_declared_type) reclassify_type_params(&s->as.value_block.declared_type, type_params);
            reclassify_type_params_in_stmt(s->as.value_block.inner, type_params);
            break;
        case STMT_YIELD:
            reclassify_type_params_in_expr(s->as.yield_stmt.value, type_params);
            break;
    }
}

static StructDecl *parse_struct_decl(Parser *p) {
    Token *kw = expect(p, TOK_STRUCT, "'struct'");
    Token *name_tok = expect(p, TOK_IDENT, "a struct name");
    require_upper_start(name_tok, "a struct name");

    StructDecl *sd = ziman_xcalloc(1, sizeof(StructDecl));
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
        Param field = {dup_token_text(field_name), field_type, 0, NULL, NULL, field_name->line, field_name->col, 0};
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

    EnumDecl *ed = ziman_xcalloc(1, sizeof(EnumDecl));
    ed->name = dup_token_text(name_tok);
    ed->line = kw->line;
    ed->col = kw->col;

    if (check(p, TOK_LT)) {
        ed->type_params = parse_type_param_list(p);
    }

    expect(p, TOK_LBRACE, "'{'");
    skip_newlines(p);
    while (!check(p, TOK_RBRACE)) {
        int is_from = 0;
        while (check(p, TOK_HASH)) {
            expect(p, TOK_HASH, "'#'");
            expect(p, TOK_LBRACKET, "'['");
            Token *attr = expect(p, TOK_IDENT, "an attribute name");
            if (attr->length != 4 || strncmp(attr->start, "from", 4) != 0) {
                ziman_error("syntax", attr->line, attr->col, "unknown variant attribute '%.*s' (the only one is #[from])", attr->length, attr->start);
            }
            is_from = 1;
            expect(p, TOK_RBRACKET, "']'");
            skip_newlines(p);
        }
        Token *variant_tok = expect(p, TOK_IDENT, "a variant name");
        require_upper_start(variant_tok, "a variant name");
        EnumVariant variant = {0};
        variant.is_from = is_from;
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
    char *path = ziman_xmalloc(cap);
    path[0] = '\0';

    Token *last_seg = expect(p, TOK_IDENT, "a module path segment");
    size_t seg_len = (size_t)last_seg->length;
    if (seg_len + 1 > cap) { cap = seg_len + 1; path = ziman_xrealloc(path, cap); }
    memcpy(path, last_seg->start, seg_len);
    len = seg_len;
    path[len] = '\0';

    while (match(p, TOK_SLASH)) {
        last_seg = expect(p, TOK_IDENT, "a module path segment");
        seg_len = (size_t)last_seg->length;
        size_t needed = len + 1 + seg_len + 1;
        if (needed > cap) { cap = needed; path = ziman_xrealloc(path, cap); }
        path[len] = '/';
        memcpy(path + len + 1, last_seg->start, seg_len);
        len += 1 + seg_len;
        path[len] = '\0';
    }

    UseDecl u;
    u.path = path;
    u.line = kw->line;
    u.has_alias = 0;
    u.alias_line = 0;
    u.alias_col = 0;

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
            u.has_alias = 1;
            u.alias_line = alias->line;
            u.alias_col = alias->col;
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
        u.has_alias = 1;
        u.alias_line = alias->line;
        u.alias_col = alias->col;
    } else {
        require_lower_start(last_seg, "a module qualifier (the path's last segment)");
        u.qualifier = dup_token_text(last_seg);
    }
    u.imported_name = NULL;
    u.local_name = NULL;
    u.col = kw->col;
    return u;
}

/* `#[ item { , item } ]`, one or more bracketed groups, each written on
 * its own line directly above the struct/enum/fn it applies to -- e.g.
 * `#[derive(Json)]`, `#[derive(Json, Hash)]`, `#[test]`,
 * `#[test(cases: add_cases)]`. `item` is `IDENT` alone (a bare flag,
 * unused by anything yet unless it's `thread_bound` or `test`) or
 * `IDENT ( IDENT { , IDENT } )` (`derive(...)`'s own shape) except for
 * `test(...)`, whose one argument is `cases: IDENT` rather than a bare
 * ident list. This is a general, reusable mechanism -- only `derive`,
 * `thread_bound`, `test` and `deprecated` are actually interpreted right
 * now (`derive` by src/derive.c, which reads `StructDecl.derives`/
 * `EnumDecl.derives`; `deprecated` by typecheck.c's own
 * `lint_deprecated_use`); any other item name is parsed (so the grammar
 * doesn't need to change later) but silently has no effect yet. See docs/DESIGN.md's
 * "Attributes and derive" entry. */
typedef struct {
    char **names;
    int count;
    int capacity;
    int thread_bound; /* `#[thread_bound]` was among the attributes */
    int thread_key;   /* `#[thread_key]` was among the attributes */
    int is_test;       /* `#[test]` or `#[test(cases: ...)]` was among the attributes */
    char *test_cases;  /* `#[test(cases: NAME)]`'s NAME, or NULL for a plain `#[test]` */
    int deprecated;                  /* `#[deprecated]` or `#[deprecated("...")]` was among them */
    const char *deprecated_message;  /* the `("...")` text, or NULL for the bare form */
} DeriveList;

static void derive_list_push(DeriveList *list, char *name) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->names = ziman_xrealloc(list->names, sizeof(char *) * (size_t)list->capacity);
    }
    list->names[list->count++] = name;
}

static DeriveList parse_attributes(Parser *p) {
    DeriveList derives = {0};
    while (check(p, TOK_HASH)) {
        if (check_ahead(p, 1, TOK_BANG)) {
            /* `#![...]` attaches to the whole module, so it is only meaningful before any
             * declaration -- parse_module_attributes has already consumed every one that was.
             * Reaching here means this one is too late, which is worth saying outright rather than
             * leaving as "expected '[', found '!'". */
            ziman_error("syntax", cur(p)->line, cur(p)->col,
                        "a module attribute '#![...]' applies to the whole file, so it must come before "
                        "the first declaration -- for this declaration alone, write '#[...]' without the '!'");
        }
        expect(p, TOK_HASH, "'#'");
        expect(p, TOK_LBRACKET, "'['");
        do {
            Token *item_name = expect(p, TOK_IDENT, "an attribute name");
            int is_derive = item_name->length == 6 && strncmp(item_name->start, "derive", 6) == 0;
            int is_test = item_name->length == 4 && strncmp(item_name->start, "test", 4) == 0;
            if (is_test) {
                derives.is_test = 1;
                if (match(p, TOK_LPAREN)) {
                    Token *key = expect(p, TOK_IDENT, "'cases'");
                    if (key->length != 5 || strncmp(key->start, "cases", 5) != 0) {
                        ziman_error("syntax", key->line, key->col,
                                    "unknown #[test(...)] argument '%.*s' (the only one is 'cases')", key->length, key->start);
                    }
                    expect(p, TOK_COLON, "':'");
                    Token *cases_name = expect(p, TOK_IDENT, "the name of the cases array or function");
                    derives.test_cases = dup_token_text(cases_name);
                    expect(p, TOK_RPAREN, "')'");
                }
            } else if (item_name->length == 10 && strncmp(item_name->start, "deprecated", 10) == 0) {
                /* The one attribute that takes a *string* argument rather than identifiers, which
                 * is why it is handled before the generic `(ident, ...)` branch below: a message is
                 * prose for whoever hits the warning, not a name the compiler resolves. */
                derives.deprecated = 1;
                if (match(p, TOK_LPAREN)) {
                    Token *msg = expect(p, TOK_STRING_LIT, "a message in quotes, or no argument at all");
                    /* The lexer already handed us a private, NUL-terminated, unescaped buffer for a
                     * string literal (see parse_primary's own EXPR_STRING case). */
                    derives.deprecated_message = (const char *)msg->start;
                    expect(p, TOK_RPAREN, "')'");
                }
            } else if (match(p, TOK_LPAREN)) {
                do {
                    Token *arg = expect(p, TOK_IDENT, "an attribute argument");
                    if (is_derive) derive_list_push(&derives, dup_token_text(arg));
                } while (match(p, TOK_COMMA));
                expect(p, TOK_RPAREN, "')'");
            } else if (item_name->length == 12 && strncmp(item_name->start, "thread_bound", 12) == 0) {
                derives.thread_bound = 1;
            } else if (item_name->length == 10 && strncmp(item_name->start, "thread_key", 10) == 0) {
                derives.thread_key = 1;
            }
        } while (match(p, TOK_COMMA));
        expect(p, TOK_RBRACKET, "']'");
        skip_newlines(p);
    }
    return derives;
}

/* `#![name]`, the module-level form: it attaches to the file it appears in rather than to the
 * declaration after it (the same inner/outer `#!`/`#` split Rust draws), so it only makes sense
 * before any declaration and is only looked for there. `experimental` is the one name interpreted;
 * anything else is refused by name rather than ignored, since a silently-ignored attribute is how
 * `#[deprecated]` managed to parse-and-do-nothing for as long as it did. */
static void parse_module_attributes(Parser *p, Module *module) {
    while (check(p, TOK_HASH) && check_ahead(p, 1, TOK_BANG)) {
        expect(p, TOK_HASH, "'#'");
        expect(p, TOK_BANG, "'!'");
        expect(p, TOK_LBRACKET, "'['");
        do {
            Token *name = expect(p, TOK_IDENT, "a module attribute name");
            if (name->length == 12 && strncmp(name->start, "experimental", 12) == 0) {
                module->experimental = 1;
            } else if (name->length == 10 && strncmp(name->start, "deprecated", 10) == 0) {
                ziman_error("syntax", name->line, name->col,
                            "#![deprecated] is not supported yet -- what a deprecated *module* should warn about "
                            "is still undecided (docs/ROADMAP.md); #[deprecated] on each function works today");
            } else {
                ziman_error("syntax", name->line, name->col,
                            "unknown module attribute '%.*s' (the only one is 'experimental')", name->length, name->start);
            }
        } while (match(p, TOK_COMMA));
        expect(p, TOK_RBRACKET, "']'");
        skip_newlines(p);
    }
}

Module *parse(TokenList tokens) {
    Parser p = {tokens, 0};
    Module *module = ziman_xcalloc(1, sizeof(Module));

    skip_newlines(&p);
    parse_module_attributes(&p, module);
    while (!check(&p, TOK_EOF)) {
        /* One declaration at a time, so several syntax errors in a file are all
         * reported: after an error, skip to the next line that starts a top-level
         * declaration (a keyword in column 1) and carry on. */
        jmp_buf jb;
        volatile int start_pos = p.pos;
        ziman_recovery_push(&jb);
        if (setjmp(jb) != 0) {
            if (p.pos <= start_pos) p.pos = start_pos + 1;
            while (!check(&p, TOK_EOF)) {
                Token *t = cur(&p);
                int starts_decl = t->col == 1 && (t->type == TOK_FN || t->type == TOK_STRUCT || t->type == TOK_ENUM || t->type == TOK_USE ||
                                                  t->type == TOK_PUB || t->type == TOK_VAL || t->type == TOK_NATIVE || t->type == TOK_HASH ||
                                                  (t->type == TOK_IDENT && t->length == 8 && memcmp(t->start, "internal", 8) == 0));
                if (starts_decl) break;
                advance_tok(&p);
            }
            skip_newlines(&p);
            continue;
        }
        if (check(&p, TOK_USE)) {
            use_list_push(&module->uses, parse_use_decl(&p));
            ziman_recovery_pop();
            skip_newlines(&p);
            continue;
        }
        DeriveList derives = {0};
        if (check(&p, TOK_HASH)) {
            derives = parse_attributes(&p);
        }
        /* `pub` is a reserved word; `internal` is a soft one (match_ident, above) so a program
         * already using "internal" as a function/variable/import name -- the health-record
         * backend's own `service.internal` helper, for one -- keeps compiling unchanged. The two
         * are mutually exclusive by construction: matching `pub` first means `internal` is never
         * even checked once `pub` already matched. */
        Visibility vis = match(&p, TOK_PUB) ? VIS_PUBLIC : (match_ident(&p, "internal") ? VIS_INTERNAL : VIS_PRIVATE);
        if (check(&p, TOK_STRUCT)) {
            StructDecl *sd = parse_struct_decl(&p);
            sd->visibility = vis;
            sd->derives = derives.names;
            sd->derive_count = derives.count;
            sd->thread_bound = derives.thread_bound;
            sd->thread_key = derives.thread_key;
            if (derives.is_test) {
                ziman_error("syntax", sd->line, sd->col, "#[test] applies to a function, not to struct '%s'", sd->name);
            }
            if (derives.deprecated) {
                ziman_error("syntax", sd->line, sd->col,
                            "#[deprecated] applies to a function; nothing reads it on struct '%s' yet (docs/ROADMAP.md)", sd->name);
            }
            struct_list_push(&module->structs, sd);
        } else if (check(&p, TOK_ENUM)) {
            EnumDecl *ed = parse_enum_decl(&p);
            ed->visibility = vis;
            ed->derives = derives.names;
            ed->derive_count = derives.count;
            if (derives.thread_bound) {
                ziman_error("syntax", ed->line, ed->col, "#[thread_bound] applies to a struct, not to enum '%s'", ed->name);
            }
            if (derives.thread_key) {
                ziman_error("syntax", ed->line, ed->col, "#[thread_key] applies to a struct, not to enum '%s'", ed->name);
            }
            if (derives.is_test) {
                ziman_error("syntax", ed->line, ed->col, "#[test] applies to a function, not to enum '%s'", ed->name);
            }
            if (derives.deprecated) {
                ziman_error("syntax", ed->line, ed->col,
                            "#[deprecated] applies to a function; nothing reads it on enum '%s' yet (docs/ROADMAP.md)", ed->name);
            }
            enum_list_push(&module->enums, ed);
        } else if (derives.count > 0 || derives.thread_bound || derives.thread_key) {
            ziman_error("syntax", cur(&p)->line, cur(&p)->col,
                        "an attribute must be followed by a struct or enum declaration");
        } else if (check(&p, TOK_NATIVE)) {
            FunctionDecl *fn = parse_native_function(&p);
            fn->visibility = vis;
            fn->deprecated = derives.deprecated;
            fn->deprecated_message = derives.deprecated_message;
            if (derives.is_test) {
                ziman_error("syntax", fn->line, fn->col, "#[test] cannot apply to a native function ('%s')", fn->name);
            }
            function_list_push(&module->functions, fn);
        } else if (check(&p, TOK_VAL)) {
            GlobalValDecl *v = parse_global_val_decl(&p, vis);
            if (derives.is_test) {
                ziman_error("syntax", v->line, v->col, "#[test] applies to a function, not to a val ('%s')", v->name);
            }
            if (derives.deprecated) {
                ziman_error("syntax", v->line, v->col,
                            "#[deprecated] applies to a function; nothing reads it on a val ('%s') yet (docs/ROADMAP.md)", v->name);
            }
            global_val_list_push(&module->vals, v);
        } else {
            FunctionDecl *fn = parse_function(&p);
            fn->visibility = vis;
            fn->is_test = derives.is_test;
            fn->test_cases = derives.test_cases;
            fn->deprecated = derives.deprecated;
            fn->deprecated_message = derives.deprecated_message;
            function_list_push(&module->functions, fn);
        }
        ziman_recovery_pop();
        skip_newlines(&p);
    }

    /* A file with syntax errors is not checked further: what is missing would
     * make the next phases report errors that are only echoes. */
    ziman_finish_errors();
    generate_derived_functions(module);
    ziman_finish_errors();

    return module;
}
