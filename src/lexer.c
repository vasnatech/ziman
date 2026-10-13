#include "lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "xalloc.h"

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
        list->items = ziman_xrealloc(list->items, (size_t)list->capacity * sizeof(Token));
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
        case TOK_BIGINT_LIT:
        case TOK_DECIMAL_LIT:
        case TOK_BYTE_LIT:
        case TOK_STRING_LIT:
        case TOK_IDENT:
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_RETURN: /* bare `return` with no expression */
        case TOK_BREAK:
        case TOK_CONTINUE:
        case TOK_RPAREN:
        case TOK_RBRACE:
        case TOK_RBRACKET:
        case TOK_GT: /* closes `Result<T, E>` -- a native fn or struct field
                      * ending in one needs a newline recognized right after it.
                      * Trade-off: a `>` comparison can no longer be broken
                      * across a line without parens; nothing does this today
                      * and it's discouraged style regardless ("write it like
                      * Go"). */
        case TOK_KW_INT:
        case TOK_KW_FLOAT:
        case TOK_KW_BOOL:
        case TOK_KW_STRING:
            return 1;
        default:
            return 0;
    }
}

/* Recorded by lex() for `--api`'s doc comments; see lexer.h. One table, reset per lex(), because
 * only the one module being printed is ever asked about. */
static CommentLine *g_comments = NULL;
static int g_comment_count = 0, g_comment_cap = 0;

const CommentLine *lex_comments(int *count) {
    *count = g_comment_count;
    return g_comments;
}

static void record_comment(int line, const char *text, int length) {
    if (g_comment_count == g_comment_cap) {
        g_comment_cap = g_comment_cap ? g_comment_cap * 2 : 64;
        g_comments = ziman_xrealloc(g_comments, (size_t)g_comment_cap * sizeof *g_comments);
    }
    g_comments[g_comment_count].line = line;
    g_comments[g_comment_count].text = text;
    g_comments[g_comment_count].length = length;
    g_comment_count++;
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

/* The character `n` positions ahead, or '\0' at the end. */
static char peek_at(Lexer *lx, int n) {
    for (int i = 0; i < n; i++) {
        if (lx->src[lx->pos + i] == '\0') return '\0';
    }
    return lx->src[lx->pos + n];
}

/* Whether `at` is the first non-whitespace character on its line. */
static int line_starts_here(Lexer *lx, const char *at) {
    const char *p = at;
    while (p > lx->src && (p[-1] == ' ' || p[-1] == '\t')) p--;
    return p == lx->src || p[-1] == '\n';
}

/* Whether the logical line holding `at` opens a `type` declaration (`type X = ...`, or
 * `pub type X = ...`). Needed because a `|` is fatal here in the lexer, so `type Light = Red |
 * Green | Amber` never reaches the parser's own "Ziman has no 'type' declaration" message, which
 * already names `enum` -- the reader saw "did you mean '||'?" instead, a boolean operator where a
 * variant type was meant (tasks.md F51). */
static int line_opens_type_decl(Lexer *lx, const char *at) {
    const char *p = at;
    while (p > lx->src && p[-1] != '\n') p--;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "pub", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
        p += 3;
        while (*p == ' ' || *p == '\t') p++;
    }
    return strncmp(p, "type", 4) == 0 && (p[4] == ' ' || p[4] == '\t');
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
    {"for", TOK_FOR},       {"in", TOK_IN},
    {"break", TOK_BREAK},   {"continue", TOK_CONTINUE}, {"spawn", TOK_SPAWN}, {"defer", TOK_DEFER}, {"try", TOK_TRY}, {"switch", TOK_SWITCH},
    {"return", TOK_RETURN}, {"true", TOK_TRUE},     {"false", TOK_FALSE},
    {"as", TOK_AS},         {"int", TOK_KW_INT},    {"float", TOK_KW_FLOAT},
    {"bool", TOK_KW_BOOL},  {"string", TOK_KW_STRING}, {"bigint", TOK_KW_BIGINT}, {"decimal", TOK_KW_DECIMAL},
    {"byte", TOK_KW_BYTE},
    {"struct", TOK_STRUCT}, {"enum", TOK_ENUM}, {"with", TOK_WITH},
    {"pub", TOK_PUB},       {"use", TOK_USE},       {"native", TOK_NATIVE},
    {"Result", TOK_RESULT}, {"Channel", TOK_CHANNEL}, {"Map", TOK_MAP}, {"Set", TOK_SET},
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

/* A text block: `"""`, a newline, the text, then `"""`. The first newline and the one before the
 * closing delimiter are not part of the value; the indentation shared by every non-blank line (and
 * by the closing delimiter's own line when it holds only whitespace) is removed, and trailing
 * blanks are trimmed from each line. Escapes are the ordinary ones, decoded after that. */
static int escape_value(char esc, char *out) {
    switch (esc) {
        case 'n': *out = '\n'; return 1;
        case 't': *out = '\t'; return 1;
        case 'r': *out = '\r'; return 1;
        case '"': *out = '"'; return 1;
        case '\\': *out = '\\'; return 1;
        default: return 0;
    }
}

static void lex_text_block(Lexer *lx) {
    int line = lx->line, col = lx->col;
    advance(lx); advance(lx); advance(lx); /* opening """ */
    while (peek(lx) == ' ' || peek(lx) == '\t' || peek(lx) == '\r') advance(lx);
    if (peek(lx) != '\n') {
        ziman_error("lexical", line, col, "a text block's opening \"\"\" must end its line");
    }
    advance(lx);

    /* the raw text up to the closing delimiter, escapes untouched */
    size_t cap = 64, rawlen = 0;
    char *raw = ziman_xmalloc(cap);
    for (;;) {
        char c = peek(lx);
        if (c == '\0') ziman_error("lexical", line, col, "unterminated text block");
        if (c == '"' && lx->src[lx->pos + 1] == '"' && lx->src[lx->pos + 2] == '"') {
            advance(lx); advance(lx); advance(lx);
            break;
        }
        if (rawlen + 2 >= cap) { cap *= 2; raw = ziman_xrealloc(raw, cap); }
        raw[rawlen++] = advance(lx);
        if (c == '\\') {
            if (peek(lx) == '\0') ziman_error("lexical", line, col, "unterminated text block");
            raw[rawlen++] = advance(lx);
        }
    }
    raw[rawlen] = '\0';

    /* split into lines; the last one is the text before the closing delimiter */
    size_t nlines = 1;
    for (size_t i = 0; i < rawlen; i++) if (raw[i] == '\n') nlines++;
    char **ls = ziman_xmalloc(nlines * sizeof(char *));
    size_t *ln = ziman_xmalloc(nlines * sizeof(size_t));
    size_t k = 0, start = 0;
    for (size_t i = 0; i <= rawlen; i++) {
        if (i == rawlen || raw[i] == '\n') {
            size_t end = i;
            if (end > start && raw[end - 1] == '\r') end--;
            ls[k] = raw + start;
            ln[k] = end - start;
            k++;
            start = i + 1;
        }
    }
    /* a closing line holding only blanks contributes its indent and is not a text line */
    size_t last = nlines - 1;
    int closer_alone = 1;
    for (size_t i = 0; i < ln[last]; i++) if (ls[last][i] != ' ' && ls[last][i] != '\t') closer_alone = 0;
    size_t nfull = closer_alone ? last : nlines;
    size_t indent = (size_t)-1;
    for (size_t i = 0; i < nlines; i++) {
        int blank = 1;
        size_t lead = 0;
        for (size_t j = 0; j < ln[i]; j++) {
            if (ls[i][j] == ' ' || ls[i][j] == '\t') { if (blank) lead++; }
            else blank = 0;
        }
        if (blank && !(i == last && closer_alone)) continue;
        if (blank) lead = ln[i];
        if (lead < indent) indent = lead;
    }
    if (indent == (size_t)-1) indent = 0;

    char *buf = ziman_xmalloc(rawlen + 1);
    size_t len = 0;
    for (size_t i = 0; i < nfull; i++) {
        size_t from = ln[i] >= indent ? indent : ln[i];
        size_t end = ln[i];
        while (end > from && (ls[i][end - 1] == ' ' || ls[i][end - 1] == '\t')) end--;
        for (size_t j = from; j < end; j++) {
            char c = ls[i][j];
            if (c == '\\') {
                char v = 0;
                j++;
                if (j >= end || !escape_value(ls[i][j], &v)) {
                    ziman_error("lexical", line, col, "unknown escape sequence in a text block");
                }
                buf[len++] = v;
            } else {
                buf[len++] = c;
            }
        }
        if (i + 1 < nfull) buf[len++] = '\n';
    }
    buf[len] = '\0';
    free(raw); free(ls); free(ln);
    push_token(lx, TOK_STRING_LIT, buf, (int)len, line, col);
    lx->prev_can_end_stmt = 1;
}

static void lex_string(Lexer *lx) {
    int line = lx->line, col = lx->col;
    advance(lx); /* opening quote */
    /* We rewrite the escaped string into a freshly allocated buffer and
     * store it via a synthetic Token whose start/length point at that
     * buffer, so the parser doesn't need to re-process escapes. */
    size_t cap = 16, len = 0;
    char *buf = ziman_xmalloc(cap);
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
                case 'r': out = '\r'; break;
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
            buf = ziman_xrealloc(buf, cap);
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

    /* An exponent (`1e3`, `2.5E-2`, `1e+9`) makes the literal a float, whatever its mantissa. It is only read when digits
     * follow, so `1e` and `1else` still lex as a number and then a name. */
    int has_exponent = 0;
    if (peek(lx) == 'e' || peek(lx) == 'E') {
        int at = (int)lx->pos + 1;
        if (lx->src[at] == '+' || lx->src[at] == '-') at++;
        if (isdigit((unsigned char)lx->src[at])) {
            has_exponent = 1;
            is_float = 1;
            while ((int)lx->pos < at) advance(lx);
            while (isdigit((unsigned char)peek(lx))) advance(lx);
            char after = peek(lx);
            if ((after == 'd' || after == 'n' || after == 'b') && !isalnum((unsigned char)peek_next(lx)) && peek_next(lx) != '_')
                ziman_error("lexical", line, col,
                            "an exponent literal is a float; it cannot take a '%c' suffix (write a decimal as 15000d, not 1.5e4d)", after);
        }
    }

    /* `n`/`d`/`b` suffix -- an arbitrary-precision `bigint` literal
     * (`12345678901234567890n`, whole-number-shaped only -- never
     * `19.99n`, bigint has no fractional part), a `decimal` literal
     * (`19.99d`, `20d` -- either shape), or a `byte` literal (`200b`,
     * whole-number-shaped only, same reasoning as bigint -- a byte has
     * no fractional part either). Range-checked against 0-255 by the
     * parser, not here (the lexer doesn't know the digit count in
     * advance without parsing them anyway). None of the three is
     * consumed if another identifier character immediately follows, so
     * this never swallows part of an unrelated identifier that happens
     * to start with 'n'/'d'/'b' right after some digits. */
    int is_bigint = 0, is_decimal = 0, is_byte = 0;
    int next_is_ident_char = isalnum((unsigned char)peek_next(lx)) || peek_next(lx) == '_';
    if (!is_float && peek(lx) == 'n' && !next_is_ident_char) {
        is_bigint = 1;
        advance(lx); /* 'n' */
    } else if (!has_exponent && peek(lx) == 'd' && !next_is_ident_char) {
        is_decimal = 1;
        advance(lx); /* 'd' */
    } else if (!is_float && peek(lx) == 'b' && !next_is_ident_char) {
        is_byte = 1;
        advance(lx); /* 'b' */
    }

    int length = (int)((lx->src + lx->pos) - start);
    TokenType type = is_bigint ? TOK_BIGINT_LIT : is_decimal ? TOK_DECIMAL_LIT : is_byte ? TOK_BYTE_LIT : (is_float ? TOK_FLOAT_LIT : TOK_INT_LIT);
    emit(lx, type, start, length, line, col);
}

static void lex_ident(Lexer *lx) {
    int line = lx->line, col = lx->col;
    const char *start = lx->src + lx->pos;
    while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
    int length = (int)((lx->src + lx->pos) - start);
    emit(lx, ident_or_keyword(start, length), start, length, line, col);
}

TokenList lex(const char *source) {
    g_comment_count = 0;
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
            int comment_line = lx.line;
            advance(&lx); advance(&lx);
            while (peek(&lx) == ' ') advance(&lx);
            const char *text = lx.src + lx.pos;
            while (peek(&lx) != '\n' && peek(&lx) != '\0') advance(&lx);
            record_comment(comment_line, text, (int)((lx.src + lx.pos) - text));
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
            if (lx.src[lx.pos + 1] == '"' && lx.src[lx.pos + 2] == '"') {
                lex_text_block(&lx);
            } else {
                lex_string(&lx);
            }
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
            case '?': advance(&lx); emit(&lx, TOK_QUESTION, start, 1, line, col); continue;
            case '#': advance(&lx); emit(&lx, TOK_HASH, start, 1, line, col); continue;
            case '.':
                advance(&lx);
                if (peek(&lx) == '.') {
                    advance(&lx);
                    /* `a..=b` is the Rust inclusive range, and it is never valid here: `..` may be
                     * followed by a third `.` (the variadic `xs: int...`) but never by `=`. Saying so
                     * in the lexer also ends the cascade -- round 1 of the 2026-10-12 benchmark had
                     * `for i in 1..=20` report "expected an expression, found '='" and then a bogus
                     * "expected 'fn'" on the following line, and four later runs avoided ranges
                     * altogether rather than risk an off-by-one the compiler would not settle
                     * (tasks.md F93). The semantics are the part worth saying: docs/SYNTAX.md has
                     * had "`a..b` is exclusive of `b`" all along, and a program may not read it. */
                    if (peek(&lx) == '=') {
                        ziman_error("lexical", line, col,
                                    "Ziman has no '..=' -- a range is written 'a..b' and stops before 'b', so an "
                                    "inclusive end is written 'a..b + 1' ('for i in 0..n' runs n times, from 0 to n - 1)");
                    }
                    emit(&lx, TOK_DOTDOT, start, 2, line, col);
                }
                else emit(&lx, TOK_DOT, start, 1, line, col);
                continue;
            case '+': advance(&lx); emit(&lx, TOK_PLUS, start, 1, line, col); continue;
            case '*': advance(&lx); emit(&lx, TOK_STAR, start, 1, line, col); continue;
            case '/': advance(&lx); emit(&lx, TOK_SLASH, start, 1, line, col); continue;
            case '%': advance(&lx); emit(&lx, TOK_PERCENT, start, 1, line, col); continue;
            case '-':
                /* `-- a comment`, where the `--` opens a line. Five of the ten cold attempts in the
                 * 2026-10-09 benchmark pilot wrote comments this way (docs/reviews/tasks.md F28).
                 * The parser catches the pair when it stops a declaration, but inside a function
                 * body `-- a note` is a legal double negation of `a`, so by the time anything fails
                 * the message is about an undefined variable and the comment is invisible.
                 *
                 * Scoped to a line's first non-whitespace deliberately: that is where a comment
                 * goes, and it leaves every arithmetic use alone. `x - -y` has a space and is not
                 * two adjacent dashes; `- -x` on its own line likewise; `--x` as a decrement is
                 * handled in parse_statement. A trailing `x = 1  -- why` is still missed, which is
                 * the honest limit of a lexical rule here.
                 *
                 * The whitespace after `--` matters: a comment is written `-- note`, while `--x`
                 * with no space is far more likely an attempt at prefix decrement, and answering
                 * that with advice about comments would be worse than the silence it gets today
                 * (it parses as a double negation). Prefix decrement has no diagnostic of its own;
                 * the postfix `x--` does, in parse_statement. */
                if (peek_at(&lx, 1) == '-' && (peek_at(&lx, 2) == ' ' || peek_at(&lx, 2) == '\t') &&
                    line_starts_here(&lx, start)) {
                    ziman_error("lexical", line, col,
                                "'--' does not start a comment in Ziman -- use '//' to the end of the line (there "
                                "is no block comment form)");
                }
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
                /* Both of these were measured in the 2026-10-10 benchmark wave, in two of ten cold
                 * attempts, and both used to get "did you mean '||'?" -- the one reading that
                 * cannot be right for either (tasks.md F51). */
                if (peek(&lx) == '>') {
                    ziman_error("lexical", line, col,
                                "there is no '|>' pipeline operator in Ziman -- nest the calls, as "
                                "'collections.sum(collections.map(evens, triple))', or give each step its own "
                                "'val', as 'val evens = collections.filter(xs, is_even)'");
                }
                if (line_opens_type_decl(&lx, start)) {
                    ziman_error("lexical", line, col,
                                "a variant type is not written with '|' -- Ziman has no 'type' declaration at "
                                "all: a tagged union is 'enum Light { Red, Green, Amber }' (read one back with "
                                "'switch', one arm per variant), and a record is 'struct Point { x: int }'");
                }
                ziman_error("lexical", line, col,
                            "unexpected character '|' -- Ziman has no single-'|' operator: '||' is boolean or, "
                            "bitwise or is 'bits.int_or(a, b)' or 'bits.byte_or' after 'use bits', a variant type "
                            "is 'enum Name { A, B }', and there is no '|>' pipeline");
                continue;
            default:
                if (c == ';') {
                    ziman_error("lexical", line, col, "unexpected character ';' -- Ziman statements end at a newline; loops are 'for i in 0..n' (which stops before n) and 'while cond'");
                }
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
