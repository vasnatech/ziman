#define _POSIX_C_SOURCE 200809L
#include "regex.h"
#include "regex_unicode.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* xalloc-exempt: this file allocates with plain malloc/calloc and checks for NULL itself, rather
 * than through ziman_xmalloc and friends, because it is the one place with something better to do
 * than exit. Its buffers are the only ones here sized by user input up to MAX_MATCH_BYTES, so they
 * are the only ones that can fail on a healthy machine -- and regex_search can report that as an
 * ordinary Ziman runtime error (see its -1 return and native_regex.c) instead of taking a server
 * down. See src/xalloc.h, and tools/check_xalloc.py, which reads this marker. */

#define MAX_INSTRUCTIONS 20000
#define MAX_CODEPOINT 0x10FFFF

/* The ceiling on what one match may allocate, checked once at compile time (see the end of
 * regex_compile) because that is where a pattern is still reportable as an error rather than as a
 * dead process. MAX_INSTRUCTIONS alone does not bound it: the matcher's capture buffers are
 * `n_inst * 2 * (n_groups + 1)` ints, so the *product* is what grows, and a pattern of 6,000 empty
 * groups -- 18,002 instructions, inside the instruction limit, and wide rather than nested, so the
 * nesting limit does not see it either -- asked for about 1.1 GB and got a SIGSEGV instead
 * (docs/reviews/tasks.md A16). 64 MB is far above any real pattern: a 200-character pattern with
 * 20 capture groups needs about 35 KB. */
#define MAX_MATCH_BYTES (64u * 1024u * 1024u)

/* A ceiling on the *work* one match may do, which MAX_MATCH_BYTES is a poor proxy for: a pattern
 * just inside the memory budget cost about 9 ms per input byte, roughly 2.5 CPU-hours for a 1 MB
 * subject in a single request (docs/reviews/tasks.md A28). Counted in capture-vector int-copies,
 * which is what the matcher actually spends its time on -- `ncap` ints per thread parked and per
 * I_SAVE.
 *
 * Shaped as a floor plus a rate rather than a flat number, because the two costs are different:
 * the first position pays a full epsilon closure whatever the subject's length, so a flat cap would
 * punish a short subject for a big pattern, while the per-byte term is what a long subject
 * legitimately spends. Measured over this repo's own patterns against a 1 MB subject, real work is
 * **7 to 34 copies per byte** (`\w+` 7, `(.*)dog` 12, a 3-named-group date 28, `(\w+)@(\w+)\.com`
 * 34). The pathological end is four to five orders of magnitude away: 400 empty groups on 200 bytes
 * is 647,226 per byte and 1,000 empty groups is 4,030,056. So 100,000 per byte leaves ~3,000x
 * headroom over anything real and still refuses the shape this task is about, and the floor of 256M
 * copies -- about 1 GB of memory traffic -- is set to let a legal-but-enormous pattern complete its
 * closure on a short subject (400 groups on 200 bytes needs 1.29e8 and is allowed; 1,000 groups
 * needs 8.06e8 and is not).
 *
 * PCRE2 is the precedent for having such a limit at all -- `match_limit`, `depth_limit`,
 * `heap_limit`, each settable per match. This one is not settable; if that is ever wanted, the rate
 * is the number to expose, not the floor. */
#define MATCH_WORK_FLOOR (256u * 1000u * 1000u)
#define MATCH_WORK_PER_BYTE 100000u

/* One thread of the matcher: an instruction and the capture offsets it reached there with. Declared
 * up here, away from the matcher itself, because regex_compile's size check needs sizeof(Thread)
 * and sizeof(Work) to say what a match would cost. */
typedef struct { int pc; int *caps; } Thread;

/* One entry of add_thread's explicit worklist: the instruction to visit and the captures in force
 * on the way to it. */
typedef struct { int pc; int *caps; } Work;

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
/* `oom` is how `class_add` reports a failed growth: it has no parser to fail through and returns
 * void to fifteen call sites, so it keeps the buffer it already has, drops the range, and raises
 * this -- which `parse_class` and the two shorthand paths check before the class is used. A dropped
 * range can never affect a match, because the flag refuses the whole compile (F127). */
typedef struct { Range *ranges; int count, cap; int negate; int oom; } CharClass;

struct Regex {
    Inst *prog;
    int n_inst;
    CharClass *classes;
    int n_classes;
    int n_groups;
    char **group_names; /* [0..n_groups]; NULL for a group without a name */
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
    /* Every Node this parse has made, in the order it made them. The parser is the owner, not the
     * tree: a parse that fails abandons nodes that were never linked into anything a tree walk
     * could reach, and that is what leaked -- 600 bytes for one malformed pattern, every time,
     * since a failed compile is not cached (docs/reviews/tasks.md A26). With one list there is one
     * place that frees them, reached identically whether the parse succeeded or not, and no tree
     * walk at all -- so this also retires the recursion A25 had to flatten in free_node. */
    Node **nodes;
    int n_nodes, cap_nodes;
} Parser;

static void fail(Parser *ps, const char *msg) {
    if (!ps->failed) {
        snprintf(ps->err, ps->err_size, "%s at position %zu", msg, ps->pos);
        ps->failed = 1;
    }
}

static Node *node(Parser *ps, NodeKind k) {
    Node *n = calloc(1, sizeof(Node));  /* xalloc-ok: NULL -> fail(ps) on the next line */
    if (n == NULL) { fail(ps, "out of memory"); return NULL; }
    if (ps->n_nodes == ps->cap_nodes) {
        int cap = ps->cap_nodes == 0 ? 64 : ps->cap_nodes * 2;
        Node **fresh = realloc(ps->nodes, (size_t)cap * sizeof(Node *));  /* xalloc-ok: temp, then fail(ps) on the next line */
        if (fresh == NULL) { free(n); fail(ps, "out of memory"); return NULL; }
        ps->nodes = fresh;
        ps->cap_nodes = cap;
    }
    ps->nodes[ps->n_nodes++] = n;
    n->kind = k;
    return n;
}

/* Frees every node this parse made, and the list. Called exactly once per regex_compile, on both
 * the failure and the success path -- see Parser.nodes. */
static void free_nodes(Parser *ps) {
    for (int i = 0; i < ps->n_nodes; i++) free(ps->nodes[i]);
    free(ps->nodes);
    ps->nodes = NULL;
    ps->n_nodes = ps->cap_nodes = 0;
}

/* -1 when the class table cannot grow. `p = realloc(p, n)` here lost the old pointer on failure and
 * the next line wrote through NULL -- the shape of A20's `sb_grow` bug, which `check_xalloc.py`
 * exists to catch and could not see because this file is exempt from it wholesale (F127). */
static int class_new(Parser *ps) {
    Regex *re = ps->re;
    CharClass *fresh = realloc(re->classes, (size_t)(re->n_classes + 1) * sizeof(CharClass));  /* xalloc-ok: temp, then fail(ps) and -1 */
    if (fresh == NULL) { fail(ps, "out of memory"); return -1; }
    re->classes = fresh;
    memset(&re->classes[re->n_classes], 0, sizeof(CharClass));
    return re->n_classes++;
}

static void class_add(CharClass *c, uint32_t lo, uint32_t hi) {
    if (c->count == c->cap) {
        int want = c->cap == 0 ? 8 : c->cap * 2;
        Range *fresh = realloc(c->ranges, (size_t)want * sizeof(Range));  /* xalloc-ok: temp, then c->oom, which parse_class checks */
        if (fresh == NULL) { c->oom = 1; return; }
        c->ranges = fresh;
        c->cap = want;
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

/* `c->ranges` is sorted and merged by class_finalize, so this is a binary search
 * (a \p{L} class has hundreds of ranges). */
static int in_ranges(const CharClass *c, uint32_t cp) {
    int lo = 0, hi = c->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < c->ranges[mid].lo) hi = mid - 1;
        else if (cp > c->ranges[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

static int class_matches(const CharClass *c, uint32_t cp, int icase) {
    int hit = in_ranges(c, cp);
    if (!hit && icase) {
        uint32_t other = cp;
        if (cp >= 'a' && cp <= 'z') other = cp - 32;
        else if (cp >= 'A' && cp <= 'Z') other = cp + 32;
        if (other != cp) hit = in_ranges(c, other);
    }
    return c->negate ? !hit : hit;
}

static int range_cmp(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->lo < y->lo ? -1 : (x->lo > y->lo ? 1 : 0);
}

static void class_finalize(CharClass *c) {
    if (c->count < 2) return;
    qsort(c->ranges, (size_t)c->count, sizeof(Range), range_cmp);
    int out = 0;
    for (int i = 1; i < c->count; i++) {
        if (c->ranges[i].lo <= c->ranges[out].hi + 1 && c->ranges[out].hi != 0xFFFFFFFFu) {
            if (c->ranges[i].hi > c->ranges[out].hi) c->ranges[out].hi = c->ranges[i].hi;
        } else {
            c->ranges[++out] = c->ranges[i];
        }
    }
    c->count = out + 1;
}

/* Adds the complement of the (sorted, disjoint) ranges `rs` over 0..MAX_CODEPOINT. */
static void class_add_complement(CharClass *c, const uint32_t (*rs)[2], int n) {
    uint32_t next = 0;
    for (int i = 0; i < n; i++) {
        if (rs[i][0] > next) class_add(c, next, rs[i][0] - 1);
        next = rs[i][1] + 1;
    }
    if (next <= MAX_CODEPOINT) class_add(c, next, MAX_CODEPOINT);
}

/* POSIX classes, ASCII only. */
typedef struct { const char *name; uint32_t r[6][2]; int n; } PosixClass;
static const PosixClass POSIX_CLASSES[] = {
    {"alpha", {{'A', 'Z'}, {'a', 'z'}}, 2},
    {"digit", {{'0', '9'}}, 1},
    {"alnum", {{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}, 3},
    {"upper", {{'A', 'Z'}}, 1},
    {"lower", {{'a', 'z'}}, 1},
    {"space", {{9, 13}, {32, 32}}, 2},
    {"blank", {{9, 9}, {32, 32}}, 2},
    {"punct", {{33, 47}, {58, 64}, {91, 96}, {123, 126}}, 4},
    {"xdigit", {{'0', '9'}, {'A', 'F'}, {'a', 'f'}}, 3},
    {"word", {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}, 4},
    {"cntrl", {{0, 31}, {127, 127}}, 2},
    {"print", {{32, 126}}, 1},
    {"graph", {{33, 126}}, 1},
};

static const UnicodeSet *find_unicode_set(const char *name, size_t len) {
    for (int i = 0; i < UNICODE_SET_COUNT; i++) {
        if (strlen(UNICODE_SETS[i].name) == len && memcmp(UNICODE_SETS[i].name, name, len) == 0) return &UNICODE_SETS[i];
    }
    return NULL;
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

/* After `\p` or `\P` (already consumed): `{Name}`, `{^Name}` or a one-letter
 * name (`\pL`). Adds the set (or its complement) to class `idx`. */
static int parse_property(Parser *ps, int idx, int negated) {
    const char *name;
    size_t len;
    if (peek(ps) == '{') {
        size_t start = ps->pos + 1;
        size_t end = start;
        while (end < ps->len && ps->p[end] != '}') end++;
        if (end >= ps->len) { fail(ps, "unterminated \\p{...}"); return 0; }
        name = (const char *)ps->p + start;
        len = end - start;
        ps->pos = end + 1;
    } else if (peek(ps) >= 0) {
        name = (const char *)ps->p + ps->pos;
        len = 1;
        ps->pos++;
    } else {
        fail(ps, "\\p needs a property name");
        return 0;
    }
    if (len > 0 && name[0] == '^') { negated = !negated; name++; len--; }
    const UnicodeSet *set = find_unicode_set(name, len);
    if (set == NULL) { fail(ps, "unknown Unicode property (categories such as L, Lu, N, Nd, P, S, Z, or Latin, Greek, Cyrillic, Han, ...)"); return 0; }
    CharClass *c = &ps->re->classes[idx];
    if (negated) {
        class_add_complement(c, set->ranges, set->count);
    } else {
        for (int i = 0; i < set->count; i++) class_add(c, set->ranges[i][0], set->ranges[i][1]);
    }
    return 1;
}

/* `[:name:]` inside a bracket class; `pos` is at the `[`. */
static int parse_posix(Parser *ps, int idx) {
    size_t start = ps->pos + 2;
    size_t end = start;
    while (end + 1 < ps->len && !(ps->p[end] == ':' && ps->p[end + 1] == ']')) end++;
    if (end + 1 >= ps->len) { fail(ps, "unterminated POSIX class"); return 0; }
    const char *name = (const char *)ps->p + start;
    size_t len = end - start;
    int negated = 0;
    if (len > 0 && name[0] == '^') { negated = 1; name++; len--; }
    for (size_t i = 0; i < sizeof POSIX_CLASSES / sizeof POSIX_CLASSES[0]; i++) {
        if (strlen(POSIX_CLASSES[i].name) == len && memcmp(POSIX_CLASSES[i].name, name, len) == 0) {
            CharClass *c = &ps->re->classes[idx];
            if (negated) class_add_complement(c, (const uint32_t (*)[2])POSIX_CLASSES[i].r, POSIX_CLASSES[i].n);
            else for (int k = 0; k < POSIX_CLASSES[i].n; k++) class_add(c, POSIX_CLASSES[i].r[k][0], POSIX_CLASSES[i].r[k][1]);
            ps->pos = end + 2;
            return 1;
        }
    }
    fail(ps, "unknown POSIX class (alpha, digit, alnum, upper, lower, space, blank, punct, xdigit, word, cntrl, print, graph)");
    return 0;
}

static Node *parse_class(Parser *ps) {
    int idx = class_new(ps);
    if (idx < 0) return NULL;
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
        if (ch == '[' && ps->pos + 1 < ps->len && ps->p[ps->pos + 1] == ':') {
            if (!parse_posix(ps, idx)) return NULL;
            continue;
        }
        if (ch == '\\') {
            ps->pos++;
            int e = peek(ps);
            if (e == 'p' || e == 'P') {
                ps->pos++;
                if (!parse_property(ps, idx, e == 'P')) return NULL;
                continue;
            }
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
    if (c->oom) { fail(ps, "out of memory"); return NULL; }
    Node *n = node(ps, N_CLASS);
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
        char *group_name = NULL;
        if (peek(ps) == '?') {
            size_t q = ps->pos + 1;
            if (q < ps->len && ps->p[q] == ':') {
                ps->pos += 2;
                group = -1;
            } else if (q < ps->len && (ps->p[q] == '<' || (ps->p[q] == 'P' && q + 1 < ps->len && ps->p[q + 1] == '<'))) {
                /* (?P<name>...) or (?<name>...); `(?<=` and `(?<!` are lookbehind */
                size_t s = ps->p[q] == 'P' ? q + 2 : q + 1;
                if (s < ps->len && (ps->p[s] == '=' || ps->p[s] == '!')) {
                    fail(ps, "lookaround is not supported");
                    return NULL;
                }
                size_t e2 = s;
                while (e2 < ps->len && (isalnum(ps->p[e2]) || ps->p[e2] == '_')) e2++;
                if (e2 == s || e2 >= ps->len || ps->p[e2] != '>' || isdigit(ps->p[s])) {
                    fail(ps, "bad group name (letters, digits and _, not starting with a digit, then '>')");
                    return NULL;
                }
                group_name = malloc(e2 - s + 1);  /* xalloc-ok: NULL -> fail(ps) on the next line */
                /* A fifth site, which the task did not name and the new compile-time OOM test in
                 * tests/unit/regex_test.c found on its first run: unchecked, and the `memcpy` below
                 * writes straight through it (F127). */
                if (group_name == NULL) { fail(ps, "out of memory"); return NULL; }
                memcpy(group_name, ps->p + s, e2 - s);
                group_name[e2 - s] = '\0';
                for (int g = 1; g <= ps->re->n_groups; g++) {
                    if (ps->re->group_names != NULL && ps->re->group_names[g] != NULL && strcmp(ps->re->group_names[g], group_name) == 0) {
                        fail(ps, "duplicate group name");
                        free(group_name); /* not stored in group_names, so nothing else owns it */
                        return NULL;
                    }
                }
                ps->pos = e2 + 1;
            } else if (q < ps->len && (ps->p[q] == '=' || ps->p[q] == '!')) {
                fail(ps, "lookaround is not supported");
                return NULL;
            } else {
                fail(ps, "unsupported group syntax (only (?:...), (?P<name>...) and leading (?i)(?s)(?m) flags)");
                return NULL;
            }
        }
        if (group == -2) {
            group = ++ps->re->n_groups;
            char **fresh_names = realloc(ps->re->group_names, (size_t)(ps->re->n_groups + 1) * sizeof(char *));  /* xalloc-ok: temp, then fail(ps) */
            if (fresh_names == NULL) { free(group_name); fail(ps, "out of memory"); return NULL; }
            ps->re->group_names = fresh_names;
            ps->re->group_names[group] = group_name;
            ps->re->group_names[0] = NULL;
        }
        if (++ps->depth > 200) { fail(ps, "pattern is nested too deeply"); return NULL; }
        Node *inner = parse_alt(ps);
        ps->depth--;
        if (ps->failed) return NULL;
        if (peek(ps) != ')') { fail(ps, "missing closing parenthesis"); return NULL; }
        ps->pos++;
        Node *g = node(ps, N_GROUP);
        g->idx = group;
        g->a = inner;
        return g;
    }
    if (c == '[') { ps->pos++; return parse_class(ps); }
    if (c == '.') { ps->pos++; return node(ps, N_ANY); }
    if (c == '^') { ps->pos++; return node(ps, N_BOL); }
    if (c == '$') { ps->pos++; return node(ps, N_EOL); }
    if (c == '\\') {
        ps->pos++;
        int e = peek(ps);
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            ps->pos++;
            int idx = class_new(ps);
            if (idx < 0) return NULL;
            class_add_perl(&ps->re->classes[idx], (char)e);
            if (ps->re->classes[idx].oom) { fail(ps, "out of memory"); return NULL; }
            Node *n = node(ps, N_CLASS);
            n->idx = idx;
            return n;
        }
        if (e == 'p' || e == 'P') {
            ps->pos++;
            int idx = class_new(ps);
            if (idx < 0) return NULL;
            if (!parse_property(ps, idx, e == 'P')) return NULL;
            if (ps->re->classes[idx].oom) { fail(ps, "out of memory"); return NULL; }
            Node *n = node(ps, N_CLASS);
            n->idx = idx;
            return n;
        }
        if (e == 'b') { ps->pos++; return node(ps, N_WORDB); }
        if (e == 'B') { ps->pos++; return node(ps, N_NWORDB); }
        uint32_t cp;
        if (!parse_escape_char(ps, &cp)) return NULL;
        Node *n = node(ps, N_CHAR);
        n->cp = cp;
        return n;
    }
    if (c == '*' || c == '+' || c == '?') { fail(ps, "nothing to repeat"); return NULL; }
    if (c == '{') { fail(ps, "nothing to repeat (write \\{ for a literal brace)"); return NULL; }
    uint32_t cp;
    int w = decode(ps->p, ps->len, ps->pos, &cp);
    ps->pos += (size_t)w;
    Node *n = node(ps, N_CHAR);
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
        Node *r = node(ps, N_REPEAT);
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
            Node *cat = node(ps, N_CAT);
            cat->a = head;
            cat->b = r;
            head = cat;
        }
    }
    return head != NULL ? head : node(ps, N_EMPTY);
}

static Node *parse_alt(Parser *ps) {
    Node *left = parse_cat(ps);
    if (left == NULL) return NULL;
    while (peek(ps) == '|') {
        ps->pos++;
        Node *right = parse_cat(ps);
        if (right == NULL) return NULL;
        Node *alt = node(ps, N_ALT);
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
        int want = em->cap == 0 ? 64 : em->cap * 2;
        Inst *fresh = realloc(re->prog, (size_t)want * sizeof(Inst));  /* xalloc-ok: temp, then em->failed */
        if (fresh == NULL) {
            if (!em->failed) { snprintf(em->err, em->err_size, "out of memory"); em->failed = 1; }
            return 0;
        }
        re->prog = fresh;
        em->cap = want;
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
        case N_CAT: {
            /* A concatenation is a left-leaning chain, one N_CAT per atom (parse_cat), so recursing
             * into `n->a` here cost one frame per *character* of the pattern -- and `gen` descends
             * that whole spine before emitting a single instruction, which is why
             * MAX_INSTRUCTIONS could not see it and the post-compile MAX_MATCH_BYTES was never
             * reached: a 20,000-character pattern segfaulted on a 1 MB stack, and a spawned worker
             * at ZIMAN_THREAD_STACK_MB=1 took the whole process down (docs/reviews/tasks.md A25).
             * Walking the spine in a loop instead leaves `gen`'s recursion bounded by the pattern's
             * *nesting*, which the parser already caps at 200 -- the same move as A16's, one
             * function over, and it refuses no pattern that used to compile.
             *
             * Order is unchanged: the chain's leftmost leaf is emitted first, then each right-hand
             * child on the way back up, which is exactly what `gen(n->a); gen(n->b)` did. */
            int links = 0;
            for (const Node *w = n; w->kind == N_CAT; w = w->a) links++;
            const Node **rights = malloc((size_t)links * sizeof(*rights));  /* xalloc-ok: NULL -> em->failed below */
            if (rights == NULL) {
                if (!em->failed) { snprintf(em->err, em->err_size, "out of memory compiling the pattern"); em->failed = 1; }
                return;
            }
            const Node *left = n;
            for (int i = 0; i < links; i++) { rights[i] = left->b; left = left->a; }
            gen(em, left);
            for (int i = links - 1; i >= 0 && !em->failed; i--) gen(em, rights[i]);
            free(rights);
            break;
        }
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
                int *splits = opt > 0 ? malloc((size_t)opt * sizeof(int)) : NULL;  /* xalloc-ok: NULL with opt > 0 -> em->failed below */
                /* A sixth unchecked site: `opt > 0` with a failed malloc left this NULL and the
                 * loop below wrote through it. `opt == 0` is the legitimate NULL and skips the
                 * loop, which is why the two cases have to be told apart rather than one check
                 * covering both (F127). */
                if (opt > 0 && splits == NULL) {
                    if (!em->failed) { snprintf(em->err, em->err_size, "out of memory"); em->failed = 1; }
                    return;
                }
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


/* Everything a half-built Regex owns. The three refusal paths in regex_compile used to free
 * different subsets of it -- the parse failure and the "pattern is too large" failure left
 * `group_names` and every name string behind, which for `(?P<nN>a|b)` x 6000 is **82,898 bytes**
 * (48,008 direct plus 34,890 indirect, measured with valgrind), and a failed compile is not cached,
 * so a client sending a different malformed pattern per request leaked that every time, in the path
 * src/regex.h advertises as safe for request input (docs/reviews/tasks.md A26). One function now,
 * so the three paths cannot drift apart again. */
static void free_partial_regex(Regex *re) {
    if (re == NULL) return;
    free(re->prog);
    for (int i = 0; i < re->n_classes; i++) free(re->classes[i].ranges);
    free(re->classes);
    if (re->group_names != NULL) {
        for (int i = 0; i <= re->n_groups; i++) free(re->group_names[i]);
        free(re->group_names);
    }
    free(re);
}

/* Releases a compiled pattern. The same work as free_partial_regex above, which is the half-built
 * case; this is the finished one, and they are kept apart because a caller should not have to know
 * that a failed compile and a finished one own the same things.
 *
 * Note what this means per build, because it is not the same thing twice: under `nogc` it really
 * frees, and under the default build `zgc.h` has made `free` a no-op, so it does nothing and the
 * collector reclaims the pattern when nothing references it. That is why the pattern cache in
 * native_regex.c does *not* call this on eviction -- see its own comment (docs/reviews/tasks.md
 * A35). */
void regex_free(Regex *re) {
    free_partial_regex(re);
}

Regex *regex_compile(const char *pattern, size_t len, char *err, size_t err_size) {
    Regex *re = calloc(1, sizeof(Regex));  /* xalloc-ok: NULL -> returns with a message */
    /* A seventh unchecked site, found by narrowing the exemption rather than by the task's list:
     * everything below dereferences `re` (F127). */
    if (re == NULL) {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
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
        free_nodes(&ps);
        free_partial_regex(re);
        return NULL;
    }

    for (int i = 0; i < re->n_classes; i++) class_finalize(&re->classes[i]);

    Emit em = {re, 0, 0, err, err_size};
    emit(&em, I_SAVE, 0, 0, 0);
    gen(&em, root);
    emit(&em, I_SAVE, 1, 0, 0);
    emit(&em, I_MATCH, 0, 0, 0);
    free_nodes(&ps);
    if (em.failed) {
        free_partial_regex(re);
        return NULL;
    }

    /* What matching this would allocate, refused here rather than attempted later -- see
     * MAX_MATCH_BYTES. Counted in 64-bit arithmetic because the product is exactly what can
     * overflow an int: two capture buffers, the scratch add_thread copies into, the worklist, the
     * two thread arrays, the mark array and the two caller-sized capture arrays. */
    uint64_t ncap = 2u * ((uint64_t)re->n_groups + 1u);
    uint64_t n_inst = (uint64_t)re->n_inst;
    uint64_t needed = 3u * n_inst * ncap * sizeof(int)        /* two capbufs and the save scratch */
                    + 2u * n_inst * sizeof(Thread)           /* the two thread arrays */
                    + (n_inst + 2u) * sizeof(Work)           /* add_thread's worklist */
                    + n_inst * sizeof(unsigned)              /* the mark array */
                    + 2u * ncap * sizeof(int);               /* `initial` and `best` */
    if (needed > MAX_MATCH_BYTES) {
        snprintf(err, err_size,
                 "pattern needs too much memory to match: %llu MB for %d capture groups over %d instructions "
                 "(the limit is %u MB; use (?:...) for a group whose text you do not need)",
                 (unsigned long long)((needed + 1048575u) / 1048576u), re->n_groups, re->n_inst,
                 (unsigned)(MAX_MATCH_BYTES / 1048576u));
        free_partial_regex(re);
        return NULL;
    }
    return re;
}

int regex_group_count(const Regex *re) { return re->n_groups; }

const char *regex_group_name(const Regex *re, int group) {
    if (group < 1 || group > re->n_groups || re->group_names == NULL) return NULL;
    return re->group_names[group];
}

/* ---------------------------------------------------------------- matcher */


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
    Work *work;      /* n_inst + 2 entries: see add_thread for why that bounds it */
    int *savepool;   /* n_inst * ncap ints, bump-allocated per add_thread call */
    /* Capture-vector int-copies done so far, against the ceiling for this subject (see
     * MATCH_WORK_FLOOR). A *pointer*, because the counter has to outlive one search: `find_all`
     * scans a subject with one regex_search per match position, and the expensive shape spreads its
     * cost across all of them -- 201 calls each comfortably inside a per-call budget, 8e8 copies in
     * total. The limit belongs to the operation the caller asked for, not to one of its steps.
     * Written only by the scanning thread, so no atomics. */
    uint64_t *spent;
    uint64_t allowed;
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

/* Adds pc and everything reachable from it by epsilon transitions to `l` -- the standard
 * closure walk of a Thompson/Pike VM, iterative rather than recursive.
 *
 * It recursed until 2026-10-08, once per instruction, and each I_SAVE frame carried a
 * variable-length array of the captures. For `("()" * 6000)` that is about 96 KB a frame to a
 * depth of 12,000: roughly 1.1 GB of C stack, which segfaults at every size --stack-mb allows,
 * including the 768 MB ceiling, and from pure Ziman code (docs/reviews/tasks.md A16). Depth now
 * lives in `vm->work` on the heap, and regex_compile has already refused any pattern whose
 * buffers would exceed MAX_MATCH_BYTES, so neither can run away.
 *
 * Thread order is preserved exactly, which matters because the first thread to reach I_MATCH wins
 * and that is what makes the match leftmost-first. Three things together give that, and the third
 * is the one that is easy to lose: entries pop LIFO; I_SPLIT pushes y before x, so x and its whole
 * subtree are visited before y; and **`vm->mark[p]` is set when an entry is popped, not when it is
 * pushed.** Marking on push is also LIFO and also pushes y first, so it looks equivalent and is
 * not: it prunes a pc that a *higher-priority* path would have reached later, and produces
 * different captures. An adversarial review measured the difference -- a mark-on-push variant
 * disagrees with this one on 45,626 of 1,500,958 outputs, the smallest case being `c*?|` against
 * "cb xa" (this engine and Python's re both say 0 0; mark-on-push says 0 1). It is the obvious way
 * to shrink the worklist, so it is named here to be refused (docs/reviews/tasks.md A29).
 *
 * Two bounds, both from that same mark, which admits each instruction once per generation. A popped
 * entry is processed once and pushes at most two, which is net **+1** per instruction -- so
 * `top <= 1 + n_inst` and `n_inst + 2` slots can never be exceeded. (The first version of this
 * comment said `2 * n_inst + 2`, adding the pushes without subtracting the pop; the allocation was
 * correct and twice the size it needed, which also inflated the MAX_MATCH_BYTES estimate -- A30.)
 * At most n_inst of those pops are I_SAVE, so the bump allocator below cannot pass n_inst * ncap
 * ints -- a bound that is exactly tight, measured at 0.9995 of the allocation -- and it is reset on
 * entry because nothing it hands out survives the walk: the only thing kept is the copy into
 * l->capbuf. */
static void add_thread(const Vm *vm, ThreadList *l, int pc, int *caps, size_t pos) {
    size_t top = 0, save_used = 0;
    vm->work[top].pc = pc;
    vm->work[top].caps = caps;
    top++;
    while (top > 0) {
        top--;
        int p = vm->work[top].pc;
        int *c = vm->work[top].caps;
        if (vm->mark[p] == l->gen) continue;
        vm->mark[p] = l->gen;
        const Inst *in = &vm->re->prog[p];
        switch (in->op) {
            case I_JMP:
                vm->work[top].pc = in->x;
                vm->work[top].caps = c;
                top++;
                break;
            case I_SPLIT:
                vm->work[top].pc = in->y;
                vm->work[top].caps = c;
                top++;
                vm->work[top].pc = in->x;
                vm->work[top].caps = c;
                top++;
                break;
            case I_SAVE: {
                *vm->spent += (uint64_t)vm->ncap;
                int *saved = vm->savepool + save_used;
                save_used += (size_t)vm->ncap;
                memcpy(saved, c, (size_t)vm->ncap * sizeof(int));
                saved[in->x] = (int)pos;
                vm->work[top].pc = p + 1;
                vm->work[top].caps = saved;
                top++;
                break;
            }
            case I_BOL: case I_EOL: case I_WORDB: case I_NWORDB:
                if (assertion_holds(vm, in->op, pos)) {
                    vm->work[top].pc = p + 1;
                    vm->work[top].caps = c;
                    top++;
                }
                break;
            default: {
                *vm->spent += (uint64_t)vm->ncap;
                int *slot = l->capbuf + (size_t)l->n * (size_t)vm->ncap;
                memcpy(slot, c, (size_t)vm->ncap * sizeof(int));
                l->t[l->n].pc = p;
                l->t[l->n].caps = slot;
                l->n++;
                break;
            }
        }
    }
}

/* -1 when a buffer could not be allocated, which regex_compile's MAX_MATCH_BYTES check makes a
 * real out-of-memory rather than a big pattern; the caller turns it into a runtime error. */
int regex_search(const Regex *re, const char *text, size_t len, size_t from, int *out_caps) {
    uint64_t spent = 0;
    return regex_search_budgeted(re, text, len, from, out_caps, &spent);
}

int regex_search_budgeted(const Regex *re, const char *text, size_t len, size_t from, int *out_caps,
                          uint64_t *spent) {
    Vm vm;
    vm.re = re;
    vm.text = (const unsigned char *)text;
    vm.len = len;
    vm.ncap = 2 * (re->n_groups + 1);
    vm.spent = spent;
    vm.allowed = (uint64_t)MATCH_WORK_FLOOR + (uint64_t)len * (uint64_t)MATCH_WORK_PER_BYTE;
    size_t ncap_bytes = (size_t)vm.ncap * sizeof(int);
    size_t capbuf_bytes = (size_t)re->n_inst * ncap_bytes;
    vm.mark = calloc((size_t)re->n_inst, sizeof(unsigned));  /* xalloc-ok: checked together with the rest of the matcher's buffers below */
    vm.work = malloc(((size_t)re->n_inst + 2u) * sizeof(Work));  /* xalloc-ok: checked together with the rest of the matcher's buffers below */
    vm.savepool = malloc(capbuf_bytes);  /* xalloc-ok: checked together with the rest of the matcher's buffers below */

    ThreadList lists[2];
    for (int i = 0; i < 2; i++) {
        lists[i].t = malloc((size_t)re->n_inst * sizeof(Thread));  /* xalloc-ok: checked together with the rest of the matcher's buffers below */
        lists[i].capbuf = malloc(capbuf_bytes);  /* xalloc-ok: checked together with the rest of the matcher's buffers below */
        lists[i].n = 0;
        lists[i].gen = 0;
    }
    int *initial = malloc(ncap_bytes);  /* xalloc-ok: checked together with the rest of the matcher's buffers below */
    int *best = malloc(ncap_bytes);  /* xalloc-ok: checked together with the rest of the matcher's buffers below */
    if (vm.mark == NULL || vm.work == NULL || vm.savepool == NULL || initial == NULL || best == NULL ||
        lists[0].t == NULL || lists[0].capbuf == NULL || lists[1].t == NULL || lists[1].capbuf == NULL) {
        for (int i = 0; i < 2; i++) { free(lists[i].t); free(lists[i].capbuf); }
        free(initial); free(best); free(vm.mark); free(vm.work); free(vm.savepool);
        return -1;
    }
    int matched = 0;
    unsigned gen_counter = 0;

    ThreadList *cur = &lists[0], *next = &lists[1];
    cur->gen = ++gen_counter;
    cur->n = 0;

    size_t pos = from;
    int over_budget = 0;
    for (;;) {
        /* Once per position, not once per copy: a comparison per input byte instead of per int
         * copied, and the most it can overshoot by is one position's worth of work. */
        if (*vm.spent > vm.allowed) { over_budget = 1; break; }
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

    if (over_budget) {
        for (int i = 0; i < 2; i++) { free(lists[i].t); free(lists[i].capbuf); }
        free(initial);
        free(best);
        free(vm.mark);
        free(vm.work);
        free(vm.savepool);
        return -2;
    }
    if (matched) memcpy(out_caps, best, (size_t)vm.ncap * sizeof(int));
    for (int i = 0; i < 2; i++) { free(lists[i].t); free(lists[i].capbuf); }
    free(initial);
    free(best);
    free(vm.mark);
    free(vm.work);
    free(vm.savepool);
    return matched;
}
