#define _POSIX_C_SOURCE 200809L
#include "regex.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INSTRUCTIONS 20000
#define MAX_CODEPOINT 0x10FFFF

/* ------------------------------------------------------------------ UTF-8 */

/* Decodes the code point at `pos`; an invalid sequence is one byte, taken as itself. */
static int decode(const unsigned char *s, size_t len, size_t pos, uint32_t *cp) {
    unsigned char c = s[pos];
    if (c < 0x80) { *cp = c; return 1; }
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (n == 0 || pos + (size_t)n > len) { *cp = c; return 1; }
    uint32_t v = c & (0xFF >> (n + 1));
    for (int i = 1; i < n; i++) {
        if ((s[pos + (size_t)i] & 0xC0) != 0x80) { *cp = c; return 1; }
        v = (v << 6) | (s[pos + (size_t)i] & 0x3F);
    }
    *cp = v;
    return n;
}

static uint32_t fold(uint32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_word(uint32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/* ---------------------------------------------------------------- program */

typedef enum { I_CHAR, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_BOL, I_EOL, I_WORDB, I_NWORDB, I_MATCH } Op;

typedef struct { Op op; int x, y; uint32_t cp; } Inst;

typedef struct { uint32_t lo, hi; } Range;
typedef struct { Range *ranges; int count, cap; int negate; } CharClass;

struct Regex {
    Inst *prog;
    int n_inst;
    CharClass *classes;
    int n_classes;
    int n_groups;
    int icase, dotall, multiline;
};

/* ---------------------------------------------------------------- AST */

typedef enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REPEAT, N_GROUP, N_BOL, N_EOL, N_WORDB, N_NWORDB } NodeKind;

typedef struct Node {
    NodeKind kind;
    uint32_t cp;
    int idx;             /* class index / group index (-1 = non-capturing) */
    int min, max, greedy; /* N_REPEAT; max -1 = unbounded */
    struct Node *a, *b;
} Node;

typedef struct {
    const unsigned char *p;
    size_t len, pos;
    Regex *re;
    char *err;
    size_t err_size;
    int failed;
    int depth;
} Parser;

static void fail(Parser *ps, const char *msg) {
    if (!ps->failed) {
        snprintf(ps->err, ps->err_size, "%s at position %zu", msg, ps->pos);
        ps->failed = 1;
    }
}

static Node *node(NodeKind k) {
    Node *n = calloc(1, sizeof(Node));
    n->kind = k;
    return n;
}

static int class_new(Parser *ps) {
    Regex *re = ps->re;
    re->classes = realloc(re->classes, (size_t)(re->n_classes + 1) * sizeof(CharClass));
    memset(&re->classes[re->n_classes], 0, sizeof(CharClass));
    return re->n_classes++;
}

static void class_add(CharClass *c, uint32_t lo, uint32_t hi) {
    if (c->count == c->cap) {
        c->cap = c->cap == 0 ? 8 : c->cap * 2;
        c->ranges = realloc(c->ranges, (size_t)c->cap * sizeof(Range));
    }
    c->ranges[c->count].lo = lo;
    c->ranges[c->count].hi = hi;
    c->count++;
}

/* \d \w \s (lower case) or their complements (upper case), added to `c`. */
static void class_add_perl(CharClass *c, char kind) {
    switch (kind) {
        case 'd': class_add(c, '0', '9'); break;
        case 'w': class_add(c, '0', '9'); class_add(c, 'A', 'Z'); class_add(c, '_', '_'); class_add(c, 'a', 'z'); break;
        case 's': class_add(c, 9, 13); class_add(c, 32, 32); break;
        case 'D': class_add(c, 0, '0' - 1); class_add(c, '9' + 1, MAX_CODEPOINT); break;
        case 'W':
            class_add(c, 0, '0' - 1); class_add(c, '9' + 1, 'A' - 1); class_add(c, 'Z' + 1, '_' - 1);
            class_add(c, '_' + 1, 'a' - 1); class_add(c, 'z' + 1, MAX_CODEPOINT);
            break;
        case 'S': class_add(c, 0, 8); class_add(c, 14, 31); class_add(c, 33, MAX_CODEPOINT); break;
    }
}

static int class_matches(const CharClass *c, uint32_t cp, int icase) {
    int hit = 0;
    for (int i = 0; i < c->count && !hit; i++) {
        if (cp >= c->ranges[i].lo && cp <= c->ranges[i].hi) hit = 1;
    }
    if (!hit && icase) {
        uint32_t other = cp;
        if (cp >= 'a' && cp <= 'z') other = cp - 32;
        else if (cp >= 'A' && cp <= 'Z') other = cp + 32;
        if (other != cp) {
            for (int i = 0; i < c->count && !hit; i++) {
                if (other >= c->ranges[i].lo && other <= c->ranges[i].hi) hit = 1;
            }
        }
    }
    return c->negate ? !hit : hit;
}

/* ---------------------------------------------------------------- parser */

static Node *parse_alt(Parser *ps);

static int peek(Parser *ps) { return ps->pos < ps->len ? ps->p[ps->pos] : -1; }

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* After a backslash, for an escape that denotes a single code point. Returns
 * 1 and sets *cp, or 0 if the escape is not one (the caller handles classes and
 * assertions first). */
static int parse_escape_char(Parser *ps, uint32_t *cp) {
    int c = peek(ps);
    if (c < 0) { fail(ps, "trailing backslash"); return 0; }
    ps->pos++;
    switch (c) {
        case 'n': *cp = '\n'; return 1;
        case 'r': *cp = '\r'; return 1;
        case 't': *cp = '\t'; return 1;
        case 'f': *cp = '\f'; return 1;
        case 'v': *cp = '\v'; return 1;
        case '0': *cp = 0; return 1;
        case 'x': {
            int h1 = hexval(peek(ps));
            if (h1 < 0) { fail(ps, "bad \\x escape (expected two hex digits)"); return 0; }
            ps->pos++;
            int h2 = hexval(peek(ps));
            if (h2 < 0) { fail(ps, "bad \\x escape (expected two hex digits)"); return 0; }
            ps->pos++;
            *cp = (uint32_t)(h1 * 16 + h2);
            return 1;
        }
        default:
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '1' && c <= '9')) {
                fail(ps, c >= '1' && c <= '9' ? "backreferences are not supported" : "unknown escape");
                return 0;
            }
            *cp = (uint32_t)c; /* an escaped punctuation character stands for itself */
            return 1;
    }
}

static Node *parse_class(Parser *ps) {
    int idx = class_new(ps);
    CharClass *c = &ps->re->classes[idx];
    if (peek(ps) == '^') { c->negate = 1; ps->pos++; }
    int first = 1;
    for (;;) {
        int ch = peek(ps);
        if (ch < 0) { fail(ps, "unterminated character class"); return NULL; }
        if (ch == ']' && !first) { ps->pos++; break; }
        first = 0;
        uint32_t lo;
        int is_range_end_ok = 1;
        if (ch == '\\') {
            ps->pos++;
            int e = peek(ps);
            if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
                ps->pos++;
                c = &ps->re->classes[idx];
                class_add_perl(c, (char)e);
                continue;
            }
            if (e == 'b') { ps->pos++; lo = 8; is_range_end_ok = 1; }
            else if (!parse_escape_char(ps, &lo)) return NULL;
        } else {
            int w = decode(ps->p, ps->len, ps->pos, &lo);
            ps->pos += (size_t)w;
        }
        uint32_t hi = lo;
        if (peek(ps) == '-' && ps->pos + 1 < ps->len && ps->p[ps->pos + 1] != ']' && is_range_end_ok) {
            ps->pos++;
            if (peek(ps) == '\\') {
                ps->pos++;
                if (!parse_escape_char(ps, &hi)) return NULL;
            } else {
                int w = decode(ps->p, ps->len, ps->pos, &hi);
                ps->pos += (size_t)w;
            }
            if (hi < lo) { fail(ps, "invalid range in character class"); return NULL; }
        }
        c = &ps->re->classes[idx];
        class_add(c, lo, hi);
    }
    Node *n = node(N_CLASS);
    n->idx = idx;
    return n;
}

static int parse_int(Parser *ps, int *out) {
    int c = peek(ps);
    if (c < '0' || c > '9') return 0;
    long v = 0;
    while ((c = peek(ps)) >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        if (v > 1000) { fail(ps, "repetition count is too large (limit 1000)"); return 0; }
        ps->pos++;
    }
    *out = (int)v;
    return 1;
}

static Node *parse_atom(Parser *ps) {
    int c = peek(ps);
    if (c == '(') {
        ps->pos++;
        int group = -2;
        if (peek(ps) == '?') {
            if (ps->pos + 1 < ps->len && ps->p[ps->pos + 1] == ':') {
                ps->pos += 2;
                group = -1;
            } else {
                fail(ps, "unsupported group syntax (only (?:...) and leading (?i)(?s)(?m) flags)");
                return NULL;
            }
        }
        if (group == -2) group = ++ps->re->n_groups;
        if (++ps->depth > 200) { fail(ps, "pattern is nested too deeply"); return NULL; }
        Node *inner = parse_alt(ps);
        ps->depth--;
        if (ps->failed) return NULL;
        if (peek(ps) != ')') { fail(ps, "missing closing parenthesis"); return NULL; }
        ps->pos++;
        Node *g = node(N_GROUP);
        g->idx = group;
        g->a = inner;
        return g;
    }
    if (c == '[') { ps->pos++; return parse_class(ps); }
    if (c == '.') { ps->pos++; return node(N_ANY); }
    if (c == '^') { ps->pos++; return node(N_BOL); }
    if (c == '$') { ps->pos++; return node(N_EOL); }
    if (c == '\\') {
        ps->pos++;
        int e = peek(ps);
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            ps->pos++;
            int idx = class_new(ps);
            class_add_perl(&ps->re->classes[idx], (char)e);
            Node *n = node(N_CLASS);
            n->idx = idx;
            return n;
        }
        if (e == 'b') { ps->pos++; return node(N_WORDB); }
        if (e == 'B') { ps->pos++; return node(N_NWORDB); }
        uint32_t cp;
        if (!parse_escape_char(ps, &cp)) return NULL;
        Node *n = node(N_CHAR);
        n->cp = cp;
        return n;
    }
    if (c == '*' || c == '+' || c == '?') { fail(ps, "nothing to repeat"); return NULL; }
    if (c == '{') { fail(ps, "nothing to repeat (write \\{ for a literal brace)"); return NULL; }
    uint32_t cp;
    int w = decode(ps->p, ps->len, ps->pos, &cp);
    ps->pos += (size_t)w;
    Node *n = node(N_CHAR);
    n->cp = cp;
    return n;
}

static Node *parse_repeat(Parser *ps) {
    Node *atom = parse_atom(ps);
    if (atom == NULL) return NULL;
    for (;;) {
        int c = peek(ps);
        int min, max;
        if (c == '*') { min = 0; max = -1; ps->pos++; }
        else if (c == '+') { min = 1; max = -1; ps->pos++; }
        else if (c == '?') { min = 0; max = 1; ps->pos++; }
        else if (c == '{') {
            size_t save = ps->pos;
            ps->pos++;
            int lo, hi;
            if (!parse_int(ps, &lo)) {
                if (ps->failed) return NULL;
                ps->pos = save; /* not a repetition: a literal brace follows */
                return atom;
            }
            hi = lo;
            if (peek(ps) == ',') {
                ps->pos++;
                if (peek(ps) == '}') hi = -1;
                else if (!parse_int(ps, &hi)) { fail(ps, "bad repetition"); return NULL; }
            }
            if (peek(ps) != '}') { fail(ps, "bad repetition (missing '}')"); return NULL; }
            ps->pos++;
            if (hi != -1 && hi < lo) { fail(ps, "repetition maximum is below the minimum"); return NULL; }
            min = lo; max = hi;
        } else {
            return atom;
        }
        int greedy = 1;
        if (peek(ps) == '?') { greedy = 0; ps->pos++; }
        if (atom->kind == N_BOL || atom->kind == N_EOL || atom->kind == N_WORDB || atom->kind == N_NWORDB) {
            fail(ps, "nothing to repeat");
            return NULL;
        }
        Node *r = node(N_REPEAT);
        r->a = atom;
        r->min = min;
        r->max = max;
        r->greedy = greedy;
        atom = r;
    }
}

static Node *parse_cat(Parser *ps) {
    Node *head = NULL;
    for (;;) {
        int c = peek(ps);
        if (c < 0 || c == '|' || c == ')') break;
        Node *r = parse_repeat(ps);
        if (r == NULL) return NULL;
        if (head == NULL) {
            head = r;
        } else {
            Node *cat = node(N_CAT);
            cat->a = head;
            cat->b = r;
            head = cat;
        }
    }
    return head != NULL ? head : node(N_EMPTY);
}

static Node *parse_alt(Parser *ps) {
    Node *left = parse_cat(ps);
    if (left == NULL) return NULL;
    while (peek(ps) == '|') {
        ps->pos++;
        Node *right = parse_cat(ps);
        if (right == NULL) return NULL;
        Node *alt = node(N_ALT);
        alt->a = left;
        alt->b = right;
        left = alt;
    }
    return left;
}

/* ---------------------------------------------------------------- emit */

typedef struct { Regex *re; int cap; int failed; char *err; size_t err_size; } Emit;

static int emit(Emit *em, Op op, int x, int y, uint32_t cp) {
    Regex *re = em->re;
    if (re->n_inst >= MAX_INSTRUCTIONS) {
        if (!em->failed) { snprintf(em->err, em->err_size, "pattern is too large"); em->failed = 1; }
        return 0;
    }
    if (re->n_inst == em->cap) {
        em->cap = em->cap == 0 ? 64 : em->cap * 2;
        re->prog = realloc(re->prog, (size_t)em->cap * sizeof(Inst));
    }
    re->prog[re->n_inst].op = op;
    re->prog[re->n_inst].x = x;
    re->prog[re->n_inst].y = y;
    re->prog[re->n_inst].cp = cp;
    return re->n_inst++;
}

static void gen(Emit *em, const Node *n);

static void gen_repeat_once(Emit *em, const Node *n) { gen(em, n->a); }

static void gen(Emit *em, const Node *n) {
    if (em->failed) return;
    Regex *re = em->re;
    switch (n->kind) {
        case N_EMPTY: break;
        case N_CHAR: emit(em, I_CHAR, 0, 0, n->cp); break;
        case N_ANY: emit(em, I_ANY, 0, 0, 0); break;
        case N_CLASS: emit(em, I_CLASS, n->idx, 0, 0); break;
        case N_BOL: emit(em, I_BOL, 0, 0, 0); break;
        case N_EOL: emit(em, I_EOL, 0, 0, 0); break;
        case N_WORDB: emit(em, I_WORDB, 0, 0, 0); break;
        case N_NWORDB: emit(em, I_NWORDB, 0, 0, 0); break;
        case N_CAT: gen(em, n->a); gen(em, n->b); break;
        case N_ALT: {
            int split = emit(em, I_SPLIT, 0, 0, 0);
            if (em->failed) return;
            re->prog[split].x = re->n_inst;
            gen(em, n->a);
            int jmp = emit(em, I_JMP, 0, 0, 0);
            if (em->failed) return;
            re->prog[split].y = re->n_inst;
            gen(em, n->b);
            re->prog[jmp].x = re->n_inst;
            break;
        }
        case N_GROUP:
            if (n->idx >= 0) emit(em, I_SAVE, 2 * n->idx, 0, 0);
            gen(em, n->a);
            if (n->idx >= 0) emit(em, I_SAVE, 2 * n->idx + 1, 0, 0);
            break;
        case N_REPEAT: {
            for (int i = 0; i < n->min && !em->failed; i++) gen_repeat_once(em, n);
            if (n->max == -1) {
                /* L1: split L2, L3; L2: x; jmp L1; L3: */
                int split = emit(em, I_SPLIT, 0, 0, 0);
                if (em->failed) return;
                int body = re->n_inst;
                gen_repeat_once(em, n);
                emit(em, I_JMP, split, 0, 0);
                if (em->failed) return;
                int after = re->n_inst;
                re->prog[split].x = n->greedy ? body : after;
                re->prog[split].y = n->greedy ? after : body;
            } else {
                int opt = n->max - n->min;
                int *splits = opt > 0 ? malloc((size_t)opt * sizeof(int)) : NULL;
                for (int i = 0; i < opt && !em->failed; i++) {
                    splits[i] = emit(em, I_SPLIT, 0, 0, 0);
                    if (em->failed) break;
                    re->prog[splits[i]].x = re->n_inst;
                    gen_repeat_once(em, n);
                }
                if (!em->failed) {
                    for (int i = 0; i < opt; i++) {
                        int body = splits[i] + 1;
                        int after = re->n_inst;
                        re->prog[splits[i]].x = n->greedy ? body : after;
                        re->prog[splits[i]].y = n->greedy ? after : body;
                    }
                }
                free(splits);
            }
            break;
        }
    }
}

static void free_node(Node *n) {
    if (n == NULL) return;
    free_node(n->a);
    free_node(n->b);
    free(n);
}

Regex *regex_compile(const char *pattern, size_t len, char *err, size_t err_size) {
    Regex *re = calloc(1, sizeof(Regex));
    Parser ps;
    memset(&ps, 0, sizeof ps);
    ps.p = (const unsigned char *)pattern;
    ps.len = len;
    ps.re = re;
    ps.err = err;
    ps.err_size = err_size;

    /* Leading flag group: (?i) (?s) (?m), alone or combined ((?im)). */
    while (ps.pos + 2 < len && pattern[ps.pos] == '(' && pattern[ps.pos + 1] == '?') {
        size_t q = ps.pos + 2;
        int i = 0, s = 0, m = 0;
        while (q < len && (pattern[q] == 'i' || pattern[q] == 's' || pattern[q] == 'm')) {
            if (pattern[q] == 'i') i = 1;
            if (pattern[q] == 's') s = 1;
            if (pattern[q] == 'm') m = 1;
            q++;
        }
        if (q > ps.pos + 2 && q < len && pattern[q] == ')') {
            re->icase |= i; re->dotall |= s; re->multiline |= m;
            ps.pos = q + 1;
        } else {
            break;
        }
    }

    Node *root = parse_alt(&ps);
    if (!ps.failed && root != NULL && ps.pos < len) fail(&ps, "unmatched closing parenthesis");
    if (ps.failed || root == NULL) {
        free_node(root);
        free(re->prog);
        for (int i = 0; i < re->n_classes; i++) free(re->classes[i].ranges);
        free(re->classes);
        free(re);
        return NULL;
    }

    Emit em = {re, 0, 0, err, err_size};
    emit(&em, I_SAVE, 0, 0, 0);
    gen(&em, root);
    emit(&em, I_SAVE, 1, 0, 0);
    emit(&em, I_MATCH, 0, 0, 0);
    free_node(root);
    if (em.failed) {
        free(re->prog);
        for (int i = 0; i < re->n_classes; i++) free(re->classes[i].ranges);
        free(re->classes);
        free(re);
        return NULL;
    }
    return re;
}

int regex_group_count(const Regex *re) { return re->n_groups; }

/* ---------------------------------------------------------------- matcher */

typedef struct { int pc; int *caps; } Thread;

typedef struct {
    Thread *t;
    int n;
    int *capbuf;
    unsigned gen;
} ThreadList;

typedef struct {
    const Regex *re;
    const unsigned char *text;
    size_t len;
    int ncap;
    unsigned *mark;
} Vm;

static int assertion_holds(const Vm *vm, Op op, size_t pos) {
    const unsigned char *s = vm->text;
    switch (op) {
        case I_BOL: return pos == 0 || (vm->re->multiline && s[pos - 1] == '\n');
        case I_EOL: return pos == vm->len || (vm->re->multiline && s[pos] == '\n');
        case I_WORDB:
        case I_NWORDB: {
            int before = pos > 0 && is_word(s[pos - 1]);
            int after = pos < vm->len && is_word(s[pos]);
            return (op == I_WORDB) == (before != after);
        }
        default: return 1;
    }
}

static void add_thread(const Vm *vm, ThreadList *l, int pc, int *caps, size_t pos) {
    if (vm->mark[pc] == l->gen) return;
    vm->mark[pc] = l->gen;
    const Inst *in = &vm->re->prog[pc];
    switch (in->op) {
        case I_JMP: add_thread(vm, l, in->x, caps, pos); return;
        case I_SPLIT:
            add_thread(vm, l, in->x, caps, pos);
            add_thread(vm, l, in->y, caps, pos);
            return;
        case I_SAVE: {
            int tmp[2 * (vm->ncap + 1)];
            memcpy(tmp, caps, (size_t)vm->ncap * sizeof(int));
            tmp[in->x] = (int)pos;
            add_thread(vm, l, pc + 1, tmp, pos);
            return;
        }
        case I_BOL: case I_EOL: case I_WORDB: case I_NWORDB:
            if (assertion_holds(vm, in->op, pos)) add_thread(vm, l, pc + 1, caps, pos);
            return;
        default: {
            int *slot = l->capbuf + (size_t)l->n * (size_t)vm->ncap;
            memcpy(slot, caps, (size_t)vm->ncap * sizeof(int));
            l->t[l->n].pc = pc;
            l->t[l->n].caps = slot;
            l->n++;
            return;
        }
    }
}

int regex_search(const Regex *re, const char *text, size_t len, size_t from, int *out_caps) {
    Vm vm;
    vm.re = re;
    vm.text = (const unsigned char *)text;
    vm.len = len;
    vm.ncap = 2 * (re->n_groups + 1);
    vm.mark = calloc((size_t)re->n_inst, sizeof(unsigned));

    ThreadList lists[2];
    for (int i = 0; i < 2; i++) {
        lists[i].t = malloc((size_t)re->n_inst * sizeof(Thread));
        lists[i].capbuf = malloc((size_t)re->n_inst * (size_t)vm.ncap * sizeof(int));
        lists[i].n = 0;
        lists[i].gen = 0;
    }
    int *initial = malloc((size_t)vm.ncap * sizeof(int));
    int *best = malloc((size_t)vm.ncap * sizeof(int));
    int matched = 0;
    unsigned gen_counter = 0;

    ThreadList *cur = &lists[0], *next = &lists[1];
    cur->gen = ++gen_counter;
    cur->n = 0;

    size_t pos = from;
    for (;;) {
        if (!matched) {
            for (int i = 0; i < vm.ncap; i++) initial[i] = -1;
            add_thread(&vm, cur, 0, initial, pos);
        }
        if (cur->n == 0) {
            if (matched || pos >= len) break;
            /* No thread alive here (an assertion such as \b or ^ failed): try the next position. */
            uint32_t skipped;
            pos += (size_t)decode(vm.text, len, pos, &skipped);
            cur->gen = ++gen_counter; /* a fresh generation, or the start pcs would look already visited */
            continue;
        }

        next->n = 0;
        next->gen = ++gen_counter;
        uint32_t cp = 0;
        int w = 0;
        if (pos < len) w = decode(vm.text, len, pos, &cp);

        for (int i = 0; i < cur->n; i++) {
            const Inst *in = &re->prog[cur->t[i].pc];
            int *caps = cur->t[i].caps;
            int step = 0;
            switch (in->op) {
                case I_MATCH:
                    memcpy(best, caps, (size_t)vm.ncap * sizeof(int));
                    matched = 1;
                    i = cur->n; /* lower-priority threads are cut off */
                    break;
                case I_CHAR:
                    step = pos < len && (re->icase ? fold(cp) == fold(in->cp) : cp == in->cp);
                    break;
                case I_ANY:
                    step = pos < len && (re->dotall || cp != '\n');
                    break;
                case I_CLASS:
                    step = pos < len && class_matches(&re->classes[in->x], cp, re->icase);
                    break;
                default: break;
            }
            if (step) add_thread(&vm, next, cur->t[i].pc + 1, caps, pos + (size_t)w);
        }
        ThreadList *tmp = cur; cur = next; next = tmp;
        if (pos >= len) break;
        pos += (size_t)w;
    }

    if (matched) memcpy(out_caps, best, (size_t)vm.ncap * sizeof(int));
    for (int i = 0; i < 2; i++) { free(lists[i].t); free(lists[i].capbuf); }
    free(initial);
    free(best);
    free(vm.mark);
    return matched;
}
