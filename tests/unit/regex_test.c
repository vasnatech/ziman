/* Standalone test for src/regex.c, which has no dependency on the rest of the interpreter and so can
 * be exercised on its own -- the reason this file exists is that the engine's two failure modes are
 * both about size rather than about matching, and neither is reachable from a fixture without making
 * the fixture huge.
 *
 * What it pins down (docs/reviews/tasks.md A16):
 *   - the epsilon-closure walk does not recurse: a pattern with 1,000 capture groups used to need
 *     about 32 MB of C stack and now needs none, so this runs in a unit test's ordinary stack;
 *   - a pattern too big to match is refused at compile time with a message that names the size;
 *   - the advice that message gives -- rewrite the groups as (?:...) -- actually works;
 *   - match semantics are unchanged by the rewrite: leftmost-first alternation, greedy and lazy
 *     quantifiers, group offsets, and an empty group;
 *   - a failed allocation inside the matcher returns -1 rather than reporting "no match" or
 *     crashing, which is what `make test-regex` wraps malloc to force.
 *
 * Run via `make test-regex`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/regex.h"

void *__real_malloc(size_t n);

static int fail_after = -1; /* -1 disables; otherwise the number of mallocs still to succeed */

void *__wrap_malloc(size_t n) {
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    return __real_malloc(n);
}

static int failures = 0;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

/* `unit` repeated `times` times. */
static char *repeated(const char *unit, int times) {
    size_t ulen = strlen(unit);
    char *s = __real_malloc(ulen * (size_t)times + 1);
    for (int i = 0; i < times; i++) memcpy(s + (size_t)i * ulen, unit, ulen);
    s[ulen * (size_t)times] = '\0';
    return s;
}

/* Matches `pattern` against `text` and checks the first `n` capture offsets. */
static void expect_caps(const char *pattern, const char *text, int n, const int *want, const char *what) {
    char err[256] = {0};
    Regex *re = regex_compile(pattern, strlen(pattern), err, sizeof err);
    if (re == NULL) {
        printf("FAIL %s -- pattern refused: %s\n", what, err);
        failures++;
        return;
    }
    int ncap = 2 * (regex_group_count(re) + 1);
    int *caps = __real_malloc((size_t)ncap * sizeof(int));
    int matched = regex_search(re, text, strlen(text), 0, caps);
    if (matched != 1) {
        printf("FAIL %s -- no match\n", what);
        failures++;
    } else {
        int bad = 0;
        for (int i = 0; i < n; i++) if (caps[i] != want[i]) bad = 1;
        if (bad) {
            printf("FAIL %s -- got", what);
            for (int i = 0; i < n; i++) printf(" %d", caps[i]);
            printf(", wanted");
            for (int i = 0; i < n; i++) printf(" %d", want[i]);
            printf("\n");
            failures++;
        } else {
            printf("PASS %s\n", what);
        }
    }
    free(caps);
    regex_free(re);
}

int main(void) {
    /* Semantics, which the rewrite from recursion to a worklist had to preserve exactly: the order
     * threads are added in is what makes a match leftmost-first. */
    { const int want[] = {2, 7, 3, 6}; expect_caps("a(b*)c", "xxabbbc", 4, want, "greedy group offsets"); }
    { const int want[] = {0, 1, 0, 1}; expect_caps("(a|ab)", "ab", 4, want, "alternation prefers the first branch"); }
    { const int want[] = {0, 4, 0, 3}; expect_caps("(a+?)b", "aaab", 4, want, "a lazy quantifier still has to reach b"); }
    { const int want[] = {0, 2, 1, 2}; expect_caps("(?:x)(y)", "xy", 4, want, "a non-capturing group takes no slot"); }
    { const int want[] = {0, 0, 0, 0}; expect_caps("()", "zz", 4, want, "an empty group matches empty"); }

    /* Deep without recursion: 1,000 groups is 2,000 I_SAVE instructions, which cost about 16 KB of
     * C stack each while the closure walk recursed. A unit test's stack is 8 MB. */
    {
        char *wide = repeated("()", 1000);
        char err[256] = {0};
        Regex *re = regex_compile(wide, strlen(wide), err, sizeof err);
        ok(re != NULL, "1,000 capture groups compile");
        if (re != NULL) {
            int ncap = 2 * (regex_group_count(re) + 1);
            int *caps = __real_malloc((size_t)ncap * sizeof(int));
            ok(regex_search(re, "zz", 2, 0, caps) == 1, "1,000 capture groups match without recursing");
            free(caps);
            regex_free(re);
        }
        free(wide);
    }

    /* Too big to match, refused while it is still an error rather than a dead process. */
    {
        char *huge = repeated("()", 6000);
        char err[256] = {0};
        Regex *re = regex_compile(huge, strlen(huge), err, sizeof err);
        ok(re == NULL, "6,000 capture groups are refused");
        ok(strstr(err, "too much memory to match") != NULL, "the refusal says what went wrong");
        ok(strstr(err, "64 MB") != NULL, "the refusal names the limit");
        free(huge);

        /* ...and the way out that the message recommends. */
        char *noncap = repeated("(?:)", 6000);
        memset(err, 0, sizeof err);
        Regex *re2 = regex_compile(noncap, strlen(noncap), err, sizeof err);
        ok(re2 != NULL, "the same width as non-capturing groups is accepted");
        regex_free(re2);
        free(noncap);
    }

    /* A failed allocation is -1, not 0: "no match" would be a wrong answer, and the old code did
     * not look. Every allocation in regex_search is tried in turn. */
    {
        const char *p = "a(b*)c";
        char err[256] = {0};
        Regex *re = regex_compile(p, strlen(p), err, sizeof err);
        int ncap = 2 * (regex_group_count(re) + 1);
        int *caps = __real_malloc((size_t)ncap * sizeof(int));
        int wrong = 0;
        for (int nth = 0; nth < 8; nth++) {
            fail_after = nth;
            int r = regex_search(re, "abbc", 4, 0, caps);
            fail_after = -1;
            if (r != -1 && r != 1) wrong++;   /* 1 once the failing allocation is past the matcher's own */
            if (r == 0) wrong++;              /* never "no match" */
        }
        ok(wrong == 0, "a failed allocation in the matcher is reported, never read as no-match");
        free(caps);
        regex_free(re);
    }

    /* The work ceiling (A28). The memory budget does not bound time: these patterns all compile,
     * and the expensive one is expensive per input byte rather than large. The counter has to be
     * carried across the scan, because `find_all`'s shape is one search per match position and each
     * one on its own is cheap -- that is how this used to get through. */
    {
        char *subject = repeated("ab", 100);
        int refused = 0, allowed = 0;
        const int counts[] = {200, 1000}; /* 200 is inside the ceiling, 1,000 is well past it; the
                                           * boundary on a 200-byte subject sits between 400 and 600 */
        for (unsigned ci = 0; ci < sizeof counts / sizeof *counts; ci++) {
            int groups = counts[ci];
            char *pat = repeated("()", groups);
            char err[256] = {0};
            Regex *re = regex_compile(pat, strlen(pat), err, sizeof err);
            if (re != NULL) {
                int ncap = 2 * (regex_group_count(re) + 1);
                int *caps = __real_malloc((size_t)ncap * sizeof(int));
                uint64_t spent = 0;
                size_t from = 0, len = strlen(subject);
                int r = 1;
                while (from <= len && (r = regex_search_budgeted(re, subject, len, from, caps, &spent)) == 1) {
                    size_t end = (size_t)caps[1];
                    if (caps[1] == caps[0]) end++;
                    from = end;
                }
                if (r == -2) refused++; else allowed++;
                free(caps);
                regex_free(re);
            }
            free(pat);
        }
        ok(allowed == 1, "a scan with 200 capture groups stays inside the work ceiling");
        ok(refused == 1, "a scan with 1,000 capture groups is refused by the work ceiling");
        free(subject);
    }

    printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
