#ifndef ZIMAN_NATIVE_LOCALE_H
#define ZIMAN_NATIVE_LOCALE_H

#include "value.h"

/* Registers the raw locale natives `lib/core/locale.zm` is written on top of. */
void native_locale_register(void);

/* ---- what `src/prelude.c` uses ---------------------------------------------------------------
 *
 * The locale-aware half of `to_upper`, `to_lower` and `equals_ignore_case` lives here because the
 * handles do, while the declarations live in the prelude beside their locale-free siblings (an
 * overload set's members have to be adjacent -- tools/check_set_adjacency.py). What crosses the
 * boundary is the *operation*, not a `locale_t`: prelude.c then needs no POSIX locale headers and
 * no opinion about how a `Locale` is represented.
 *
 * `loc` points at a Ziman `Locale`, or is NULL to mean "the process default" -- which is what a
 * one-argument call means. */

/* Whether the process default is the invariant root locale, which it is until something sets
 * otherwise. `prelude.c` asks so that `to_upper(s)` can keep its ASCII fast path in the case that
 * is almost always the case -- root folding *is* ASCII folding, so there is nothing to gain by
 * going through libc for it, and nothing to lose by not. */
int ziman_locale_default_is_root(void);

/* `s` case-mapped whole: a fresh string, built by decoding each code point, asking
 * `towupper_l`/`towlower_l` for its mapping, and encoding the answer. `upper` picks the direction. */
Value ziman_locale_case_map(Value s, const Value *loc, int upper, int line, int col);

/* `a` and `b` equal with case folded under the locale. Compares code point by code point rather
 * than building two folded strings. */
int ziman_locale_equals_fold(Value a, Value b, const Value *loc, int line, int col);

#endif
