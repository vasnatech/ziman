#define _POSIX_C_SOURCE 200809L
#include "format.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { K_WORD, K_NUMBER, K_STRING, K_PUNCT, K_COMMENT } Kind;

typedef struct {
    Kind kind;
    const char *s;
    int len;
    int nl_before;      /* newlines since the previous item */
    int spaces_before;  /* blanks on the same line since the previous item (0 if a newline came first) */
} Item;

typedef struct { Item *v; int n, cap; } Items;

static void push(Items *it, Item x) {
    if (it->n == it->cap) {
        it->cap = it->cap ? it->cap * 2 : 256;
        it->v = realloc(it->v, (size_t)it->cap * sizeof(Item));
    }
    it->v[it->n++] = x;
}

/* ------------------------------------------------------------------ scan */

static int scan(const char *src, size_t len, Items *out, char *err, size_t err_size) {
    size_t i = 0;
    int nl = 0, spaces = 0;
    int line = 1;
    while (i < len) {
        char c = src[i];
        if (c == '\n') { nl++; spaces = 0; line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { if (nl == 0) spaces++; i++; continue; }
        Item it = { K_PUNCT, src + i, 1, nl, spaces };
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            size_t j = i;
            while (j < len && src[j] != '\n') j++;
            while (j > i && (src[j - 1] == ' ' || src[j - 1] == '\t' || src[j - 1] == '\r')) j--;
            it.kind = K_COMMENT;
            it.len = (int)(j - i);
            i = j;
        } else if (c == '"') {
            size_t j = i + 1;
            while (j < len && src[j] != '"' && src[j] != '\n') {
                if (src[j] == '\\' && j + 1 < len) j++;
                j++;
            }
            if (j >= len || src[j] != '"') {
                snprintf(err, err_size, "line %d: unterminated string literal", line);
                return 0;
            }
            it.kind = K_STRING;
            it.len = (int)(j + 1 - i);
            i = j + 1;
        } else if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < len && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            it.kind = K_WORD;
            it.len = (int)(j - i);
            i = j;
        } else if (isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < len && isdigit((unsigned char)src[j])) j++;
            if (j + 1 < len && src[j] == '.' && isdigit((unsigned char)src[j + 1])) {
                j++;
                while (j < len && isdigit((unsigned char)src[j])) j++;
            }
            if (j < len && (src[j] == 'n' || src[j] == 'd' || src[j] == 'b') && !(j + 1 < len && (isalnum((unsigned char)src[j + 1]) || src[j + 1] == '_'))) j++;
            it.kind = K_NUMBER;
            it.len = (int)(j - i);
            i = j;
        } else {
            static const char *two[] = {"==", "!=", "<=", ">=", "&&", "||", "->", ".."};
            int l = 1;
            for (size_t k = 0; k < sizeof two / sizeof two[0]; k++) {
                if (i + 1 < len && src[i] == two[k][0] && src[i + 1] == two[k][1]) l = 2;
            }
            it.len = l;
            i += (size_t)l;
        }
        push(out, it);
        nl = 0;
        spaces = 0;
    }
    return 1;
}

/* --------------------------------------------------------------- classify */

static int is(const Item *t, const char *p) {
    return t->kind == K_PUNCT && (int)strlen(p) == t->len && memcmp(t->s, p, (size_t)t->len) == 0;
}

static int word_is(const Item *t, const char *w) {
    return t->kind == K_WORD && (int)strlen(w) == t->len && memcmp(t->s, w, (size_t)t->len) == 0;
}

static const char *const KEYWORDS[] = {"fn", "val", "var", "if", "else", "while", "for", "in", "break", "continue", "spawn", "defer",
                                       "switch", "return", "as", "struct", "enum", "with", "pub", "use", "native"};

static int is_keyword(const Item *t) {
    if (t->kind != K_WORD) return 0;
    for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) {
        if (word_is(t, KEYWORDS[i])) return 1;
    }
    return 0;
}

/* Something a binary operator can follow: a value, a name, or a closing bracket. */
static int ends_operand(const Item *t) {
    if (t->kind == K_NUMBER || t->kind == K_STRING) return 1;
    if (t->kind == K_WORD) return !is_keyword(t);
    return is(t, ")") || is(t, "]") || is(t, "}") || is(t, "?");
}

static int is_binary_op(const Item *t) {
    static const char *ops[] = {"+", "*", "/", "%", "==", "!=", "<=", ">=", "&&", "||", "=", "->"};
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) if (is(t, ops[i])) return 1;
    return is(t, "-");
}

/* Whether a space goes between two items on the same line. `prev_prev` decides
 * whether a '-' is unary. */
static int is_op_char(const Item *t) {
    return t->kind == K_PUNCT && t->len == 1 && strchr("+-*/%=<>!&|", t->s[0]) != NULL;
}

static int need_space(const Item *p, const Item *c, const Item *pp, int in_use_line) {
    int orig = c->spaces_before > 0;
    /* `use models/user`: a path, not a division */
    if (in_use_line && (is(p, "/") || is(c, "/"))) return 0;
    /* two operator characters written together (`++`, `=-`) stay together, so a
     * formatter never turns one token pair into another */
    if (!orig && is_op_char(p) && is_op_char(c)) return 0;
    /* `string...` (a variadic type) ends in '.', but what follows it is not a member name */
    if (is(p, ".") && pp != NULL && is(pp, "..") ) return !is(c, ")") && !is(c, ",");
    /* nothing before these */
    if (is(c, ",") || is(c, ")") || is(c, "]") || is(c, "?") || is(c, ":") || is(c, ".") || is(c, "..")) return 0;
    if (is(p, "(") || is(p, "[") || is(p, ".") || is(p, "..") || is(p, "#") || is(p, "!")) return 0;
    if (is(p, "?")) return !(is(c, "}"));
    /* braces */
    if (is(p, "{")) return !is(c, "}");
    if (is(c, "}")) return !is(p, "{");
    if (is(c, "{")) return 1;
    if (is(p, "}")) return 1;
    /* '(' and '[' */
    if (is(c, "(")) {
        if (is(p, ">")) return orig;                                  /* f<T>(x) or a > (b) */
        if (p->kind == K_WORD && !is_keyword(p)) return 0;             /* a call */
        if (word_is(p, "fn")) return 0;                                /* fn(A) -> R, a lambda */
        if (is(p, ")") || is(p, "]")) return 0;
        if (is(p, "-") && pp != NULL && !ends_operand(pp)) return 0;   /* unary minus */
        return 1;
    }
    if (is(c, "[")) {
        if (is(p, "<")) return orig;
        if (ends_operand(p) && !is(p, "}")) return 0;                  /* indexing */
        return 1;
    }
    /* angle brackets: generic or comparison, cannot tell -- keep the source's spacing */
    if (is(p, "<") || is(p, ">") || is(c, "<") || is(c, ">")) return orig;
    /* unary minus */
    if (is(p, "-") && (pp == NULL || !ends_operand(pp))) return 0;
    if (is(c, "-") && !ends_operand(p)) return 1;
    if (is(c, "!")) return 1;
    if (is(p, ",") || is(p, ":")) return 1;
    if (is_binary_op(p) || is_binary_op(c)) return 1;
    if ((p->kind == K_WORD || p->kind == K_NUMBER || p->kind == K_STRING) &&
        (c->kind == K_WORD || c->kind == K_NUMBER || c->kind == K_STRING)) return 1;
    if (is(p, ")") || is(p, "]")) return c->kind == K_WORD || c->kind == K_NUMBER || c->kind == K_STRING;
    return orig;
}

/* ------------------------------------------------------------------ emit */

typedef struct { char *s; size_t n, cap; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

typedef struct { char kind; int opener_level; } Open;

static int opens(const Item *t) { return is(t, "(") || is(t, "[") || is(t, "{"); }
static int closes(const Item *t) { return is(t, ")") || is(t, "]") || is(t, "}"); }

char *format_source(const char *src, size_t len, char *err, size_t err_size) {
    Items items = {0};
    if (!scan(src, len, &items, err, err_size)) { free(items.v); return NULL; }

    Buf out = {0};
    put(&out, "", 0);
    Open *stack = malloc((size_t)(items.n + 1) * sizeof(Open));
    int depth = 0;
    int cur_level = 0;          /* indentation level of the line being written */
    int at_line_start = 1;
    int prev_line_continues = 0; /* the previous code line ended with an operator */
    int cont_from_prev = 0;
    const Item *last_code = NULL, *before_last_code = NULL;
    int wrote_any = 0;
    int line_ends_with_open = 0; /* the line just finished ended with an opening bracket */
    int line_is_use = 0;         /* the current line starts with `use` */

    for (int idx = 0; idx < items.n; idx++) {
        Item *it = &items.v[idx];
        int new_line = it->nl_before > 0 || wrote_any == 0;
        if (wrote_any == 0) new_line = 1;

        if (new_line) {
            if (wrote_any) {
                put(&out, "\n", 1);
                /* one blank line survives, but not just inside a block */
                int next_closes = it->kind == K_PUNCT && closes(it);
                if (it->nl_before >= 2 && !line_ends_with_open && !next_closes) put(&out, "\n", 1);
            }
            /* indentation */
            int level;
            int first_is_closer = it->kind == K_PUNCT && closes(it);
            if (first_is_closer && depth > 0) level = stack[depth - 1].opener_level;
            else level = depth == 0 ? 0 : stack[depth - 1].opener_level + 1;
            if (!first_is_closer && prev_line_continues && !cont_from_prev) { level += 1; cont_from_prev = 1; }
            else cont_from_prev = 0;
            cur_level = level;
            line_is_use = it->kind == K_WORD && word_is(it, "use");
            for (int i = 0; i < level; i++) put(&out, "    ", 4);
            at_line_start = 1;
            prev_line_continues = 0;
        } else if (it->kind == K_COMMENT) {
            int gap = it->spaces_before > 0 ? it->spaces_before : 1;
            for (int i = 0; i < gap; i++) put(&out, " ", 1);
            at_line_start = 0;
        } else if (last_code != NULL) {
            if (need_space(last_code, it, before_last_code, line_is_use)) put(&out, " ", 1);
        }
        (void)at_line_start;

        put(&out, it->s, (size_t)it->len);
        wrote_any = 1;

        if (it->kind != K_COMMENT) {
            if (opens(it)) {
                stack[depth].kind = it->s[0];
                stack[depth].opener_level = cur_level;
                depth++;
            } else if (closes(it) && depth > 0) {
                depth--;
            }
            before_last_code = last_code;
            last_code = it;
            /* does this line (so far) end with an operator that continues onto the next? */
            prev_line_continues = is_binary_op(it) && !is(it, "->") ? 1 : is(it, "->");
            line_ends_with_open = opens(it);
        } else {
            /* a trailing comment does not change what the code line ends with */
        }
        if (it->kind == K_COMMENT && (idx == 0 || items.v[idx].nl_before > 0)) {
            /* an own-line comment: the line "ends with" whatever the previous code did not */
            prev_line_continues = 0;
            line_ends_with_open = 0;
        }
    }
    if (wrote_any) put(&out, "\n", 1);

    /* Self-check: the formatted text must contain exactly the same code tokens and comments. */
    Items again = {0};
    char scratch[128];
    int ok = scan(out.s, out.n, &again, scratch, sizeof scratch) && again.n == items.n;
    for (int i = 0; ok && i < items.n; i++) {
        ok = items.v[i].kind == again.v[i].kind && items.v[i].len == again.v[i].len &&
             memcmp(items.v[i].s, again.v[i].s, (size_t)items.v[i].len) == 0;
    }
    free(again.v);
    free(items.v);
    free(stack);
    if (!ok) {
        free(out.s);
        snprintf(err, err_size, "internal error: formatting would change the program's tokens (not written)");
        return NULL;
    }
    return out.s != NULL ? out.s : strdup("");
}
