#ifndef ZIMAN_BIGNUM_H
#define ZIMAN_BIGNUM_H

#include <stdint.h>

/* Arbitrary-precision sign-magnitude integer arithmetic, backing both
 * `bigint` and `decimal`'s unscaled value. Immutable: every operation
 * returns a new BigNum, matching the "never mutate, always build a new
 * one" pattern ArrayObj/StructObj already use.
 *
 * Magnitude is stored as little-endian base-1e9 limbs (digits[0] is
 * least significant) so string conversion is nearly free and the
 * arithmetic stays easy to read/verify by hand, at some cost to raw
 * speed for very large numbers -- matching the project's own "not
 * chasing performance" stance (docs/DESIGN.md non-goals).
 *
 * `sign` is -1/0/1; `count == 0` always means exactly zero (with
 * sign == 0), and every operation normalizes away any leading
 * (most-significant) zero limb, so two equal values always have an
 * identical representation -- needed so equality can just compare
 * sign + limb arrays directly, no numeric normalization step required
 * at the call site. */

#define BIGNUM_BASE 1000000000 /* 10^9 -- limbs are always in [0, BASE) */

typedef struct {
    int sign;         /* -1, 0, or 1 */
    uint32_t *digits; /* little-endian base-1e9 limbs, heap, length `count` */
    int count;
} BigNum;

BigNum bignum_from_i64(int64_t v);

/* Parses a decimal string ("-"? digit+). `*ok` is set to 0 (and a
 * zero-value BigNum returned) on malformed input -- empty, non-digit
 * characters, or a lone "-" with nothing after it. Callers turn a
 * failure into a Result<..., string>, matching parse_int/parse_float. */
BigNum bignum_from_string(const char *s, int *ok);

char *bignum_to_string(BigNum a); /* heap, NUL-terminated */

BigNum bignum_negate(BigNum a);
BigNum bignum_abs(BigNum a);
BigNum bignum_add(BigNum a, BigNum b);
BigNum bignum_sub(BigNum a, BigNum b);
BigNum bignum_mul(BigNum a, BigNum b);

/* Truncating division: quotient truncates toward zero, remainder takes
 * the dividend's sign -- matches C's (and Ziman's existing int) `/`/`%`
 * semantics exactly, not Python's floor division. `b` must not be zero;
 * callers check that themselves and raise the same runtime error
 * int/float division by zero already does. */
void bignum_divmod(BigNum a, BigNum b, BigNum *quot, BigNum *rem);

int bignum_compare(BigNum a, BigNum b); /* -1, 0, or 1 */
int bignum_is_zero(BigNum a);

/* 10^n as a BigNum, n >= 0 -- used by decimal.c to shift a scale's worth
 * of digits when aligning two different scales or computing division to
 * an explicit target scale. */
BigNum bignum_pow10(int n);

/* Clamps to INT64_MIN/INT64_MAX if `a` doesn't fit -- used by `hash()`
 * and by an explicit `as int` cast, which is itself defined as
 * saturating rather than wrapping (see docs/SYNTAX.md's cast rules). */
int64_t bignum_to_i64_saturating(BigNum a);
double bignum_to_double(BigNum a);      /* used by `as float`; via the text, so any magnitude survives */

#endif
