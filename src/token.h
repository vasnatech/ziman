#ifndef ZIMAN_TOKEN_H
#define ZIMAN_TOKEN_H

typedef enum {
    TOK_EOF,
    TOK_NEWLINE,

    /* literals */
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_BIGINT_LIT, /* digits followed immediately by 'n', e.g. `12345678901234567890n` */
    TOK_DECIMAL_LIT, /* digits (optionally with a '.') followed immediately by 'd', e.g. `19.99d`, `20d` */
    TOK_BYTE_LIT, /* digits followed immediately by 'b', e.g. `200b`; range-checked 0-255 by the parser */
    TOK_STRING_LIT,
    TOK_IDENT,

    /* keywords */
    TOK_FN,
    TOK_VAL,
    TOK_VAR,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_SPAWN,
    TOK_DEFER,
    TOK_TRY,
    TOK_SWITCH,
    TOK_RETURN,
    TOK_TRUE,
    TOK_FALSE,
    TOK_AS,
    TOK_STRUCT,
    TOK_ENUM,
    TOK_WITH,
    TOK_PUB,
    TOK_USE,
    TOK_NATIVE,
    TOK_BUILTIN,
    TOK_VARIADIC, /* 'variadic' -- precedes the last parameter's type */
    TOK_RESULT,
    TOK_CHANNEL,
    TOK_MAP,
    TOK_SET,
    TOK_KW_INT,
    TOK_KW_FLOAT,
    TOK_KW_BOOL,
    TOK_KW_STRING,
    TOK_KW_BIGINT,
    TOK_KW_DECIMAL,
    TOK_KW_BYTE,

    /* punctuation */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_COLON,
    TOK_DOT,
    TOK_DOTDOT, /* .. -- only legal directly after 'in' in a for-loop header */
    TOK_ARROW, /* -> */
    TOK_QUESTION, /* ? -- postfix Option<T>/Result<T, E> unwrap-or-propagate */
    TOK_HASH, /* # -- starts a #[...] attribute, e.g. #[derive(Json)] */

    /* operators */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,      /* = */
    TOK_EQEQ,    /* == */
    TOK_BANGEQ,  /* != */
    TOK_LT,
    TOK_GT,
    TOK_LTEQ,
    TOK_GTEQ,
    TOK_AMPAMP,  /* && */
    TOK_PIPEPIPE,/* || */
    TOK_BANG,    /* ! */
} TokenType;

typedef struct {
    TokenType type;
    const char *start; /* pointer into the source buffer, not NUL-terminated */
    int length;
    int line;
    int col;
} Token;

const char *token_type_name(TokenType type);

/* The binding keyword of some other language, written where Ziman wants `val`/`var`: returns the
 * canonical word (`"let"`, `"const"`, `"mut"`, `"auto"`) for a `name` of `length` bytes, else NULL.
 * One list with two callers, each catching a different position: parser.c rewrites `let x = 1` into
 * the `val` it meant, so one wrong word does not fan out into an error per mention of the name
 * (docs/reviews/tasks.md F95), and typecheck.c answers a bare `let` reaching an expression. */
const char *ziman_foreign_decl_keyword(const char *name, int length);

#endif
