#include "lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

typedef struct {
    const char *src;
    int pos;
    int line;
    int col;
    TokenList tokens;
    /* Whether the most recently emitted significant (non-newline) token can
     * end a statement -- if it can, a following newline becomes a real
     * TOK_NEWLINE; otherwise the newline is swallowed. This is a purely
     * lexical rule (like Go's automatic semicolon insertion), never an
     * indentation rule -- see docs/DESIGN.md. */
    int prev_can_end_stmt;
} Lexer;

static void push_token(Lexer *lx, TokenType type, const char *start, int length, int line, int col) {
    TokenList *list = &lx->tokens;
    if (list->count == list->capacity) {
        list->capacity = list->capacity == 0 ? 64 : list->capacity * 2;
        list->items = realloc(list->items, (size_t)list->capacity * sizeof(Token));
    }
    list->items[list->count].type = type;
    list->items[list->count].start = start;
    list->items[list->count].length = length;
    list->items[list->count].line = line;
    list->items[list->count].col = col;
    list->count++;
}

static int can_end_statement(TokenType type) {
    switch (type) {
        case TOK_INT_LIT:
        case TOK_FLOAT_LIT:
        case TOK_STRING_LIT:
        case TOK_IDENT:
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_RETURN: /* bare `return` with no expression */
        case TOK_RPAREN:
        case TOK_RBRACE:
        case TOK_RBRACKET:
        case TOK_KW_INT:
        case TOK_KW_FLOAT:
        case TOK_KW_BOOL:
        case TOK_KW_STRING:
            return 1;
        default:
            return 0;
    }
}

static char peek(Lexer *lx) { return lx->src[lx->pos]; }
static char peek_next(Lexer *lx) { return lx->src[lx->pos] == '\0' ? '\0' : lx->src[lx->pos + 1]; }

static char advance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    return c;
}

static void emit(Lexer *lx, TokenType type, const char *start, int length, int line, int col) {
    push_token(lx, type, start, length, line, col);
    if (type != TOK_NEWLINE) {
        lx->prev_can_end_stmt = can_end_statement(type);
    }
}

static void emit_newline_if_significant(Lexer *lx, int line, int col) {
    if (lx->prev_can_end_stmt) {
        push_token(lx, TOK_NEWLINE, lx->src + lx->pos, 0, line, col);
        lx->prev_can_end_stmt = 0;
    }
}

typedef struct {
    const char *word;
    TokenType type;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"fn", TOK_FN},         {"val", TOK_VAL},       {"var", TOK_VAR},
    {"if", TOK_IF},         {"else", TOK_ELSE},     {"while", TOK_WHILE},
    {"return", TOK_RETURN}, {"true", TOK_TRUE},     {"false", TOK_FALSE},
    {"as", TOK_AS},         {"int", TOK_KW_INT},    {"float", TOK_KW_FLOAT},
    {"bool", TOK_KW_BOOL},  {"string", TOK_KW_STRING},
    {"struct", TOK_STRUCT}, {"with", TOK_WITH},
};

static TokenType ident_or_keyword(const char *start, int length) {
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        size_t kw_len = strlen(KEYWORDS[i].word);
        if ((int)kw_len == length && strncmp(KEYWORDS[i].word, start, (size_t)length) == 0) {
            return KEYWORDS[i].type;
        }
    }
    return TOK_IDENT;
}

static void lex_string(Lexer *lx) {
    int line = lx->line, col = lx->col;
    advance(lx); /* opening quote */
    /* We rewrite the escaped string into a freshly allocated buffer and
     * store it via a synthetic Token whose start/length point at that
     * buffer, so the parser doesn't need to re-process escapes. */
    size_t cap = 16, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        char c = peek(lx);
        if (c == '\0' || c == '\n') {
            ziman_error("lexical", line, col, "unterminated string literal");
        }
        if (c == '"') {
            advance(lx);
            break;
        }
        char out;
        if (c == '\\') {
            advance(lx);
            char esc = peek(lx);
            switch (esc) {
                case 'n': out = '\n'; break;
                case 't': out = '\t'; break;
                case '"': out = '"'; break;
                case '\\': out = '\\'; break;
                default:
                    ziman_error("lexical", lx->line, lx->col, "unknown escape sequence '\\%c'", esc);
                    return; /* unreachable */
            }
            advance(lx);
        } else {
            out = advance(lx);
        }
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        buf[len++] = out;
    }
    buf[len] = '\0';
    push_token(lx, TOK_STRING_LIT, buf, (int)len, line, col);
    lx->prev_can_end_stmt = 1;
}

static void lex_number(Lexer *lx) {
    int line = lx->line, col = lx->col;
    const char *start = lx->src + lx->pos;
    while (isdigit((unsigned char)peek(lx))) advance(lx);

    int is_float = 0;
    if (peek(lx) == '.' && isdigit((unsigned char)peek_next(lx))) {
        is_float = 1;
        advance(lx); /* '.' */
        while (isdigit((unsigned char)peek(lx))) advance(lx);
    }

    int length = (int)((lx->src + lx->pos) - start);
    emit(lx, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT, start, length, line, col);
}

static void lex_ident(Lexer *lx) {
    int line = lx->line, col = lx->col;
    const char *start = lx->src + lx->pos;
    while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
    int length = (int)((lx->src + lx->pos) - start);
    emit(lx, ident_or_keyword(start, length), start, length, line, col);
}

TokenList lex(const char *source) {
    Lexer lx = {0};
    lx.src = source;
    lx.pos = 0;
    lx.line = 1;
    lx.col = 1;
    lx.prev_can_end_stmt = 0;

    for (;;) {
        char c = peek(&lx);

        if (c == '\0') {
            emit_newline_if_significant(&lx, lx.line, lx.col);
            push_token(&lx, TOK_EOF, lx.src + lx.pos, 0, lx.line, lx.col);
            break;
        }

        if (c == ' ' || c == '\t' || c == '\r') {
            advance(&lx);
            continue;
        }

        if (c == '\n') {
            int line = lx.line, col = lx.col;
            advance(&lx);
            emit_newline_if_significant(&lx, line, col);
            continue;
        }

        if (c == '/' && peek_next(&lx) == '/') {
            while (peek(&lx) != '\n' && peek(&lx) != '\0') advance(&lx);
            continue;
        }

        if (isdigit((unsigned char)c)) {
            lex_number(&lx);
            continue;
        }

        if (isalpha((unsigned char)c) || c == '_') {
            lex_ident(&lx);
            continue;
        }

        if (c == '"') {
            lex_string(&lx);
            continue;
        }

        int line = lx.line, col = lx.col;
        const char *start = lx.src + lx.pos;

        switch (c) {
            case '(': advance(&lx); emit(&lx, TOK_LPAREN, start, 1, line, col); continue;
            case ')': advance(&lx); emit(&lx, TOK_RPAREN, start, 1, line, col); continue;
            case '{': advance(&lx); emit(&lx, TOK_LBRACE, start, 1, line, col); continue;
            case '}': advance(&lx); emit(&lx, TOK_RBRACE, start, 1, line, col); continue;
            case '[': advance(&lx); emit(&lx, TOK_LBRACKET, start, 1, line, col); continue;
            case ']': advance(&lx); emit(&lx, TOK_RBRACKET, start, 1, line, col); continue;
            case ',': advance(&lx); emit(&lx, TOK_COMMA, start, 1, line, col); continue;
            case ':': advance(&lx); emit(&lx, TOK_COLON, start, 1, line, col); continue;
            case '.': advance(&lx); emit(&lx, TOK_DOT, start, 1, line, col); continue;
            case '+': advance(&lx); emit(&lx, TOK_PLUS, start, 1, line, col); continue;
            case '*': advance(&lx); emit(&lx, TOK_STAR, start, 1, line, col); continue;
            case '/': advance(&lx); emit(&lx, TOK_SLASH, start, 1, line, col); continue;
            case '%': advance(&lx); emit(&lx, TOK_PERCENT, start, 1, line, col); continue;
            case '-':
                advance(&lx);
                if (peek(&lx) == '>') { advance(&lx); emit(&lx, TOK_ARROW, start, 2, line, col); }
                else emit(&lx, TOK_MINUS, start, 1, line, col);
                continue;
            case '=':
                advance(&lx);
                if (peek(&lx) == '=') { advance(&lx); emit(&lx, TOK_EQEQ, start, 2, line, col); }
                else emit(&lx, TOK_EQ, start, 1, line, col);
                continue;
            case '!':
                advance(&lx);
                if (peek(&lx) == '=') { advance(&lx); emit(&lx, TOK_BANGEQ, start, 2, line, col); }
                else emit(&lx, TOK_BANG, start, 1, line, col);
                continue;
            case '<':
                advance(&lx);
                if (peek(&lx) == '=') { advance(&lx); emit(&lx, TOK_LTEQ, start, 2, line, col); }
                else emit(&lx, TOK_LT, start, 1, line, col);
                continue;
            case '>':
                advance(&lx);
                if (peek(&lx) == '=') { advance(&lx); emit(&lx, TOK_GTEQ, start, 2, line, col); }
                else emit(&lx, TOK_GT, start, 1, line, col);
                continue;
            case '&':
                advance(&lx);
                if (peek(&lx) == '&') { advance(&lx); emit(&lx, TOK_AMPAMP, start, 2, line, col); continue; }
                ziman_error("lexical", line, col, "unexpected character '&' (did you mean '&&'?)");
                continue;
            case '|':
                advance(&lx);
                if (peek(&lx) == '|') { advance(&lx); emit(&lx, TOK_PIPEPIPE, start, 2, line, col); continue; }
                ziman_error("lexical", line, col, "unexpected character '|' (did you mean '||'?)");
                continue;
            default:
                ziman_error("lexical", line, col, "unexpected character '%c'", c);
        }
    }

    return lx.tokens;
}

void token_list_free(TokenList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}
