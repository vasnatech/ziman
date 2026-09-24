#ifndef ZIMAN_FORMAT_H
#define ZIMAN_FORMAT_H

#include <stddef.h>

/* Formats Ziman source: 4-space indentation from bracket nesting, one space
 * around binary operators and after commas, none inside brackets or before a
 * call's '(', at most one blank line in a row (and none just inside a block),
 * comments kept and re-indented, trailing whitespace removed. It works on the
 * text, not the syntax tree (the parser drops comments), so it never changes
 * where a line breaks and does not reflow long lines; it does not need the
 * source to be valid, only lexable. Returns a malloc'd NUL-terminated string,
 * or NULL with a message in `err` (an unterminated string literal, or an
 * internal self-check failure). `<` and `>` keep their original spacing, since
 * `a < b` and `List<int>` cannot be told apart without parsing. */
char *format_source(const char *src, size_t len, char *err, size_t err_size);

#endif
