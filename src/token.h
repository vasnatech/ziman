#ifndef ZIMAN_TOKEN_H
#define ZIMAN_TOKEN_H

typedef enum {
    TOK_EOF,
    TOK_NEWLINE,

    /* literals */
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,
    TOK_IDENT,

    /* keywords */
    TOK_FN,
    TOK_VAL,
    TOK_VAR,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_RETURN,
    TOK_TRUE,
    TOK_FALSE,
    TOK_AS,
    TOK_STRUCT,
    TOK_WITH,
    TOK_PUB,
    TOK_USE,
    TOK_NATIVE,
    TOK_RESULT,
    TOK_KW_INT,
    TOK_KW_FLOAT,
    TOK_KW_BOOL,
    TOK_KW_STRING,

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
    TOK_ARROW, /* -> */

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

#endif
