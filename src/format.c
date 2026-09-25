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
        } else if (c == '"' && i + 2 < len && src[i + 1] == '"' && src[i + 2] == '"') {
            /* a text block runs to its closing """ and is kept exactly as written */
            size_t j = i + 3;
            int start_line = line;
            while (j + 2 < len && !(src[j] == '"' && src[j + 1] == '"' && src[j + 2] == '"')) {
                if (src[j] == '\\' && j + 1 < len) j++;
                else if (src[j] == '\n') line++;
                j++;
            }
            if (j + 2 >= len) {
                snprintf(err, err_size, "line %d: unterminated text block", start_line);
                return 0;
            }
            it.kind = K_STRING;
            it.len = (int)(j + 3 - i);
            i = j + 3;
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

typedef struct {
    char kind;
    int opener_level;
    int angle;     /* unmatched '<' at this bracket level (a generic list, or a comparison) */
    int after_fn;  /* the bracket was opened right after the keyword `fn` */
} Open;

static int opens(const Item *t) { return is(t, "(") || is(t, "[") || is(t, "{"); }
static int closes(const Item *t) { return is(t, ")") || is(t, "]") || is(t, "}"); }

/* Where an item ended up in one emission pass, and what surrounds it. */
typedef struct {
    size_t start, end;   /* the item's text in the output */
    size_t line_start;   /* start of its output line */
    int depth;           /* bracket depth around it */
    char innermost;      /* innermost open bracket: '(' '[' '{' or 0 */
    int angle_open;
    int after_fn;
    int in_attr;         /* on a line that starts with `#` (an attribute) */
} ItemPos;

/* One pass: lays `items` out into `out`, recording positions. */
static void emit_pass(const Items *items, Buf *out, ItemPos *pos) {
    out->n = 0;
    put(out, "", 0);
    out->s[0] = '\0';
    Open *stack = malloc((size_t)(items->n + 1) * sizeof(Open));
    int depth = 0;
    int cur_level = 0;
    int prev_line_continues = 0; /* the previous code line ended with an operator */
    int cont_from_prev = 0;
    const Item *last_code = NULL, *before_last_code = NULL;
    int wrote_any = 0;
    int line_ends_with_open = 0;
    int line_is_use = 0;
    int line_is_attr = 0;
    size_t line_start = 0;

    for (int idx = 0; idx < items->n; idx++) {
        const Item *it = &items->v[idx];
        int new_line = it->nl_before > 0 || wrote_any == 0;

        if (new_line) {
            if (wrote_any) {
                put(out, "\n", 1);
                int next_closes = it->kind == K_PUNCT && closes(it);
                if (it->nl_before >= 2 && !line_ends_with_open && !next_closes) put(out, "\n", 1);
            }
            line_start = out->n;
            int level;
            int first_is_closer = it->kind == K_PUNCT && closes(it);
            if (first_is_closer && depth > 0) level = stack[depth - 1].opener_level;
            else level = depth == 0 ? 0 : stack[depth - 1].opener_level + 1;
            if (!first_is_closer && prev_line_continues && !cont_from_prev) { level += 1; cont_from_prev = 1; }
            else cont_from_prev = 0;
            cur_level = level;
            line_is_use = it->kind == K_WORD && word_is(it, "use");
            line_is_attr = is(it, "#");
            for (int i = 0; i < level; i++) put(out, "    ", 4);
            prev_line_continues = 0;
        } else if (it->kind == K_COMMENT) {
            int gap = it->spaces_before > 0 ? it->spaces_before : 1;
            for (int i = 0; i < gap; i++) put(out, " ", 1);
        } else if (last_code != NULL) {
            if (need_space(last_code, it, before_last_code, line_is_use)) put(out, " ", 1);
        }

        pos[idx].start = out->n;
        put(out, it->s, (size_t)it->len);
        pos[idx].end = out->n;
        pos[idx].line_start = line_start;
        pos[idx].depth = depth;
        pos[idx].innermost = depth > 0 ? stack[depth - 1].kind : 0;
        pos[idx].angle_open = depth > 0 ? stack[depth - 1].angle > 0 : 0;
        pos[idx].after_fn = depth > 0 ? stack[depth - 1].after_fn : 0;
        pos[idx].in_attr = line_is_attr;
        wrote_any = 1;

        if (it->kind != K_COMMENT) {
            if (opens(it)) {
                stack[depth].kind = it->s[0];
                stack[depth].opener_level = cur_level;
                stack[depth].angle = 0;
                stack[depth].after_fn = last_code != NULL && word_is(last_code, "fn");
                depth++;
            } else if (closes(it) && depth > 0) {
                depth--;
            } else if (depth > 0 && is(it, "<")) {
                stack[depth - 1].angle++;
            } else if (depth > 0 && is(it, ">") && stack[depth - 1].angle > 0) {
                stack[depth - 1].angle--;
            }
            before_last_code = last_code;
            last_code = it;
            prev_line_continues = is_binary_op(it) && !is(it, "->") ? 1 : is(it, "->");
            line_ends_with_open = opens(it);
        } else if (it->nl_before > 0 || idx == 0) {
            prev_line_continues = 0;
            line_ends_with_open = 0;
        }
    }
    if (wrote_any) put(out, "\n", 1);
    free(stack);
}

static size_t char_count(const char *s, size_t from, size_t to) {
    size_t n = 0;
    for (size_t i = from; i < to; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

/* Breaks lines that are wider than `width` (comments not counted) after a comma of
 * their outermost bracket group, by asking the next item to start a line. Only
 * commas whose bracket is a call, an array literal or a struct literal, and that are
 * not inside a generic list or a `fn(...)` type, are used -- the places the parser
 * accepts a newline. Returns whether anything changed. */
static int wrap_once(Items *items, const ItemPos *pos, const Buf *out, int width) {
    int changed = 0;
    int idx = 0;
    while (idx < items->n) {
        int end = idx;
        size_t ls = pos[idx].line_start;
        while (end < items->n && pos[end].line_start == ls) end++;
        /* items idx..end-1 are on one output line */
        int last_code = -1;
        for (int i = idx; i < end; i++) if (items->v[i].kind != K_COMMENT) last_code = i;
        if (last_code >= 0 && (int)char_count(out->s, ls, pos[last_code].end) > width && !pos[idx].in_attr) {
            int best = -1, best_depth = 1 << 30;
            int first = -1;
            for (int i = idx; i + 1 < end; i++) {
                const Item *c = &items->v[i];
                if (!is(c, ",") || pos[i].depth < 1 || pos[i].innermost == 0 || pos[i].angle_open || pos[i].after_fn) continue;
                if (items->v[i + 1].nl_before > 0 || items->v[i + 1].kind == K_COMMENT) continue;
                if (pos[i].depth < best_depth) { best_depth = pos[i].depth; best = -1; first = -1; }
                if (pos[i].depth == best_depth) {
                    if (first < 0) first = i;
                    if ((int)char_count(out->s, ls, pos[i].end) <= width) best = i;
                }
            }
            int at = best >= 0 ? best : first;
            if (at >= 0) {
                items->v[at + 1].nl_before = 1;
                changed = 1;
            }
        }
        idx = end;
    }
    return changed;
}

/* Lines a trailing comment ends: consecutive ones are aligned to one column. */
static char *align_trailing_comments(const Buf *out, const Items *items, const ItemPos *pos) {
    /* line start offsets */
    int n_lines = 0;
    for (size_t i = 0; i < out->n; i++) if (out->s[i] == '\n') n_lines++;
    size_t *starts = malloc((size_t)(n_lines + 1) * sizeof(size_t));
    size_t *cstart = malloc((size_t)(n_lines + 1) * sizeof(size_t));
    int line = 0;
    starts[0] = 0;
    for (size_t i = 0; i < out->n; i++) if (out->s[i] == '\n') starts[++line] = i + 1;
    for (int i = 0; i <= n_lines; i++) cstart[i] = (size_t)-1;
    int li = 0;
    for (int idx = 0; idx < items->n; idx++) {
        if (items->v[idx].kind != K_COMMENT || items->v[idx].nl_before > 0 || idx == 0) continue;
        while (li + 1 <= n_lines && starts[li + 1] <= pos[idx].start) li++;
        cstart[li] = pos[idx].start;
    }
    Buf res = {0};
    put(&res, "", 0);
    int i = 0;
    while (i < n_lines) {
        size_t line_end = starts[i + 1] - 1; /* the '\n' */
        if (cstart[i] == (size_t)-1) {
            put(&res, out->s + starts[i], line_end + 1 - starts[i]);
            i++;
            continue;
        }
        int j = i;
        size_t max_code = 0;
        while (j < n_lines && cstart[j] != (size_t)-1) {
            size_t code_end = cstart[j];
            while (code_end > starts[j] && out->s[code_end - 1] == ' ') code_end--;
            size_t w = char_count(out->s, starts[j], code_end);
            if (w > max_code) max_code = w;
            j++;
        }
        for (int k = i; k < j; k++) {
            size_t code_end = cstart[k];
            while (code_end > starts[k] && out->s[code_end - 1] == ' ') code_end--;
            put(&res, out->s + starts[k], code_end - starts[k]);
            size_t w = char_count(out->s, starts[k], code_end);
            for (size_t s = 0; s < max_code - w + 2; s++) put(&res, " ", 1);
            put(&res, out->s + cstart[k], starts[k + 1] - cstart[k]);
        }
        i = j;
    }
    free(starts);
    free(cstart);
    return res.s != NULL ? res.s : strdup("");
}

/* Sorts each run of consecutive `use` lines (no blank line, no comment line in between). */
static void sort_use_runs(Items *items) {
    int n = items->n;
    if (n == 0) return;
    int *line_of_first = malloc((size_t)(n + 1) * sizeof(int));
    int n_lines = 0;
    for (int i = 0; i < n; i++) if (i == 0 || items->v[i].nl_before > 0) line_of_first[n_lines++] = i;
    line_of_first[n_lines] = n;
    Item *result = malloc((size_t)n * sizeof(Item));
    int out_n = 0;
    int li = 0;
    while (li < n_lines) {
        int s = line_of_first[li];
        int is_use = items->v[s].kind == K_WORD && word_is(&items->v[s], "use");
        if (!is_use) {
            for (int i = s; i < line_of_first[li + 1]; i++) result[out_n++] = items->v[i];
            li++;
            continue;
        }
        int lj = li + 1;
        while (lj < n_lines) {
            int f = line_of_first[lj];
            if (!(items->v[f].kind == K_WORD && word_is(&items->v[f], "use") && items->v[f].nl_before == 1)) break;
            lj++;
        }
        int run = lj - li;
        char **keys = malloc((size_t)run * sizeof(char *));
        int *order = malloc((size_t)run * sizeof(int));
        for (int r = 0; r < run; r++) {
            int a = line_of_first[li + r], b = line_of_first[li + r + 1];
            size_t cap = 1;
            for (int i = a; i < b; i++) if (items->v[i].kind != K_COMMENT) cap += (size_t)items->v[i].len + 1;
            keys[r] = malloc(cap);
            keys[r][0] = '\0';
            for (int i = a; i < b; i++) {
                if (items->v[i].kind == K_COMMENT) continue;
                strncat(keys[r], items->v[i].s, (size_t)items->v[i].len);
                strcat(keys[r], " ");
            }
            order[r] = r;
        }
        for (int a = 1; a < run; a++) { /* stable insertion sort */
            int cur = order[a];
            int b = a - 1;
            while (b >= 0 && strcmp(keys[order[b]], keys[cur]) > 0) { order[b + 1] = order[b]; b--; }
            order[b + 1] = cur;
        }
        int first_nl = items->v[s].nl_before;
        for (int r = 0; r < run; r++) {
            int src = order[r];
            int a = line_of_first[li + src], b = line_of_first[li + src + 1];
            for (int i = a; i < b; i++) {
                Item it = items->v[i];
                if (i == a) it.nl_before = r == 0 ? first_nl : 1;
                result[out_n++] = it;
            }
        }
        for (int r = 0; r < run; r++) free(keys[r]);
        free(keys);
        free(order);
        li = lj;
    }
    memcpy(items->v, result, (size_t)n * sizeof(Item));
    free(result);
    free(line_of_first);
}

char *format_source(const char *src, size_t len, int width, char *err, size_t err_size) {
    Items items = {0};
    if (!scan(src, len, &items, err, err_size)) { free(items.v); return NULL; }
    sort_use_runs(&items);

    ItemPos *pos = malloc((size_t)(items.n + 1) * sizeof(ItemPos));
    Buf out = {0};
    for (int pass = 0; pass < 60; pass++) {
        emit_pass(&items, &out, pos);
        if (width <= 0 || !wrap_once(&items, pos, &out, width)) break;
    }
    char *text = align_trailing_comments(&out, &items, pos);
    size_t text_len = strlen(text);

    /* Self-check: the formatted text must contain exactly the same code tokens and comments. */
    Items again = {0};
    char scratch[128];
    int ok = scan(text, text_len, &again, scratch, sizeof scratch) && again.n == items.n;
    for (int i = 0; ok && i < items.n; i++) {
        ok = items.v[i].kind == again.v[i].kind && items.v[i].len == again.v[i].len &&
             memcmp(items.v[i].s, again.v[i].s, (size_t)items.v[i].len) == 0;
    }
    free(again.v);
    free(items.v);
    free(pos);
    free(out.s);
    if (!ok) {
        free(text);
        snprintf(err, err_size, "internal error: formatting would change the program's tokens (not written)");
        return NULL;
    }
    return text;
}
