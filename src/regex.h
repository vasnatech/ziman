#ifndef ZIMAN_REGEX_H
#define ZIMAN_REGEX_H

#include <stddef.h>

/* A small regular-expression engine: Perl-flavoured syntax, no backreferences
 * and no lookaround, so matching is a Pike VM that runs in O(text x pattern)
 * time and can never blow up exponentially (a pattern from a request is safe to
 * compile). Exponential time is not the only way a pattern can be hostile,
 * though, so three limits bound the rest: nesting depth, instruction count, and
 * -- because the matcher's capture buffers are instructions x groups, a product
 * neither of the other two sees -- the memory a match would need, refused at
 * compile time (see regex.c's MAX_MATCH_BYTES, and docs/reviews/tasks.md A16 for
 * the wide pattern that segfaulted before it existed).
 * Patterns and text are UTF-8; offsets are byte offsets. Matching is
 * leftmost, with Perl's priority between alternatives and for greedy/lazy
 * quantifiers. A compiled Regex is immutable and may be shared between threads. */
typedef struct Regex Regex;

/* NULL on a bad pattern, with a message (including the position) in `err`. */
Regex *regex_compile(const char *pattern, size_t len, char *err, size_t err_size);

/* Number of capture groups, not counting the whole match. */
int regex_group_count(const Regex *re);

/* The name of capture group `group` (1-based) if it was written `(?P<name>...)`
 * or `(?<name>...)`, else NULL. */
const char *regex_group_name(const Regex *re, int group);

/* Searches `text` from byte offset `from` (>= 0, <= len). On a match returns 1
 * and fills `caps` with 2 * (groups + 1) byte offsets: [start, end) of the whole
 * match, then of each group, -1 for a group that did not take part. 0 for no
 * match, and -1 when one of the matcher's buffers could not be allocated --
 * which regex_compile has already bounded, so it means the machine is out of
 * memory, not that the pattern was too big. */
int regex_search(const Regex *re, const char *text, size_t len, size_t from, int *caps);

#endif
