#ifndef ZIMAN_DECIMAL_H
#define ZIMAN_DECIMAL_H

#include "bignum.h"

/* Arbitrary-precision decimal, built directly on bignum.h -- exactly
 * Java's `BigDecimal` shape: an unscaled integer magnitude plus a scale
 * (digits after the decimal point). value = unscaled * 10^-scale.
 * `scale` is always >= 0 -- Ziman's `decimal` never needs Java's
 * negative-scale generalization (representing a value like 1.2e20
 * without expanding all the trailing zeros), since nothing here is
 * trying to be a general scientific-notation type. Immutable, same
 * "every operation builds a new value" policy as BigNum itself. */
typedef struct {
    BigNum unscaled;
    int scale;
} Decimal;

Decimal decimal_from_i64(int64_t v);

/* Parses "-"? digit+ ("." digit+)?  -- `*ok` is set to 0 (and a
 * zero-value Decimal returned) on malformed input: empty, a lone "-",
 * non-digit characters, or a "." with no digits after it (`"5."` is
 * rejected, same strictness the lexer already applies to a float
 * literal). Callers turn a failure into a Result<..., string>, matching
 * parse_int/parse_float/parse_bigint. */
Decimal decimal_from_string(const char *s, int *ok);

char *decimal_to_string(Decimal d); /* heap, NUL-terminated; no trailing '.' when scale == 0 */

Decimal decimal_negate(Decimal a);
Decimal decimal_abs(Decimal a);

/* +/- align to the larger of the two scales first (e.g. 1.5 + 0.25 -> both
 * treated as scale 2), so the result's scale is exact, never rounded. */
Decimal decimal_add(Decimal a, Decimal b);
Decimal decimal_sub(Decimal a, Decimal b);

/* * needs no alignment -- multiplies the unscaled values and adds the
 * scales directly, always exact (1.5 * 0.25 = 0.375, scale 3). */
Decimal decimal_mul(Decimal a, Decimal b);

/* `base` raised to a non-negative integer `exponent`, exact (repeated multiplication, by
 * squaring). `exponent < 0` is the caller's job to reject -- see decimal.c's own comment. */
Decimal decimal_pow(Decimal base, int exponent);

/* Decimal division doesn't always terminate, so this can't be exact the
 * way add/subtract/multiply are: rounds to a target scale of
 * max(a.scale, b.scale), half-up (the "how humans round money"
 * convention -- ties round away from zero, not banker's rounding).
 * `b` must not be zero; callers
 * check that themselves, same as bignum_divmod. */
Decimal decimal_div(Decimal a, Decimal b);

/* a % b = a - trunc(a / b, scale 0) * b -- an *exact* remainder (the
 * truncated integer quotient needs no rounding, so unlike `/` this never
 * loses precision), mirroring int/float's own `%` shape. `b` must not be
 * zero. */
Decimal decimal_mod(Decimal a, Decimal b);

/* Compares numeric value, aligning scales first -- 1.0 and 1.00 compare
 * equal here (and via decimal_compare(a,b) == 0 for `==`), deliberately
 * diverging from Java's well-known BigDecimal.equals() gotcha, where
 * scale participates in equals() but not compareTo(). */
int decimal_compare(Decimal a, Decimal b); /* -1, 0, or 1 */
int decimal_is_zero(Decimal a);

/* Strips trailing zero digits from the unscaled value, reducing scale to
 * the minimal representation of the same numeric value (zero always
 * normalizes to scale 0, regardless of its original scale). Needed
 * anywhere two numerically-equal decimals must also produce identical
 * output from something that isn't decimal_compare/`==` itself -- e.g.
 * `hash(1.0d)` must equal `hash(1.00d)`, which raw field comparison on
 * `{unscaled, scale}` can't guarantee (unlike BigNum, where equal values
 * are already guaranteed to share one normalized representation). */
Decimal decimal_normalize(Decimal a);

/* Rounds `a` to exactly `scale` decimal places, half-up. `scale >=
 * a.scale` only pads with exact zeros (no rounding needed or possible).
 * This is decimal's equivalent of float's floor/ceil/round -- except
 * decimal is already exact, so "rounding" means picking a coarser scale
 * to represent the same kind of value in, not fixing up binary
 * imprecision. */
Decimal decimal_round(Decimal a, int scale);

/* decimal_round's two one-directional cousins: rounds toward negative/positive infinity
 * instead of to the nearest, same as float's own floor/ceil relative to round. */
Decimal decimal_floor(Decimal a, int scale);
Decimal decimal_ceil(Decimal a, int scale);

double decimal_to_double(Decimal a);            /* used by `as float` */
Decimal decimal_from_double_trunc(double v);    /* used by `as decimal`, fixed at 10 decimal places */

/* `as bigint`/`as int` (via bignum_to_i64_saturating on the result) --
 * truncates the fractional part, exactly like `as int` on a float
 * already truncates rather than rounds. */
BigNum decimal_to_bignum_trunc(Decimal a);
Decimal decimal_from_bignum(BigNum n); /* exact -- scale 0 */

#endif
