#define _POSIX_C_SOURCE 200809L
#include "decimal.h"
#include "xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Decimal decimal_from_i64(int64_t v) {
    Decimal d;
    d.unscaled = bignum_from_i64(v);
    d.scale = 0;
    return d;
}

Decimal decimal_from_string(const char *s, int *ok) {
    *ok = 1;
    size_t len = strlen(s);
    size_t i = (len > 0 && s[0] == '-') ? 1 : 0;
    if (i >= len) {
        *ok = 0;
        return decimal_from_i64(0);
    }

    const char *dot = strchr(s + i, '.');
    char *digits_buf;
    int scale;
    if (dot == NULL) {
        digits_buf = ziman_xstrdup(s);
        scale = 0;
    } else {
        size_t int_len = (size_t)(dot - s); /* includes the leading '-', if any */
        size_t frac_len = len - int_len - 1;
        if (frac_len == 0) {
            *ok = 0;
            return decimal_from_i64(0);
        }
        digits_buf = ziman_xmalloc(int_len + frac_len + 1);
        memcpy(digits_buf, s, int_len);
        memcpy(digits_buf + int_len, dot + 1, frac_len);
        digits_buf[int_len + frac_len] = '\0';
        scale = (int)frac_len;
    }

    int digit_ok;
    BigNum unscaled = bignum_from_string(digits_buf, &digit_ok);
    free(digits_buf);
    if (!digit_ok) {
        *ok = 0;
        return decimal_from_i64(0);
    }
    Decimal d;
    d.unscaled = unscaled;
    d.scale = scale;
    return d;
}

char *decimal_to_string(Decimal d) {
    char *mag_str = bignum_to_string(bignum_abs(d.unscaled));
    size_t mag_len = strlen(mag_str);
    int sign = d.unscaled.sign;

    if (d.scale == 0) {
        char *out = ziman_xmalloc(mag_len + 2);
        size_t pos = 0;
        if (sign < 0) out[pos++] = '-';
        memcpy(out + pos, mag_str, mag_len);
        pos += mag_len;
        out[pos] = '\0';
        free(mag_str);
        return out;
    }

    /* Need at least scale+1 digits to place the decimal point -- pad
     * with leading zeros if the magnitude is shorter (e.g. unscaled=5,
     * scale=3 -> "005" -> "0.005"). */
    size_t needed = (size_t)d.scale + 1;
    char *padded;
    size_t padded_len;
    if (mag_len < needed) {
        padded_len = needed;
        padded = ziman_xmalloc(padded_len + 1);
        size_t zeros = needed - mag_len;
        memset(padded, '0', zeros);
        memcpy(padded + zeros, mag_str, mag_len);
        padded[padded_len] = '\0';
    } else {
        padded_len = mag_len;
        padded = ziman_xstrdup(mag_str);
    }
    free(mag_str);

    size_t int_len = padded_len - (size_t)d.scale;
    char *out = ziman_xmalloc(padded_len + 3); /* sign + '.' + NUL */
    size_t pos = 0;
    if (sign < 0) out[pos++] = '-';
    memcpy(out + pos, padded, int_len);
    pos += int_len;
    out[pos++] = '.';
    memcpy(out + pos, padded + int_len, (size_t)d.scale);
    pos += (size_t)d.scale;
    out[pos] = '\0';
    free(padded);
    return out;
}

Decimal decimal_negate(Decimal a) {
    Decimal d;
    d.unscaled = bignum_negate(a.unscaled);
    d.scale = a.scale;
    return d;
}

Decimal decimal_abs(Decimal a) {
    Decimal d;
    d.unscaled = bignum_abs(a.unscaled);
    d.scale = a.scale;
    return d;
}

/* Shared by +, -, and compare: brings both operands to the larger of
 * the two scales by multiplying the smaller-scale one's unscaled value
 * by the appropriate power of 10. */
static void align(Decimal a, Decimal b, BigNum *a_out, BigNum *b_out, int *scale_out) {
    if (a.scale == b.scale) {
        *a_out = a.unscaled;
        *b_out = b.unscaled;
        *scale_out = a.scale;
        return;
    }
    if (a.scale > b.scale) {
        *a_out = a.unscaled;
        *b_out = bignum_mul(b.unscaled, bignum_pow10(a.scale - b.scale));
        *scale_out = a.scale;
    } else {
        *a_out = bignum_mul(a.unscaled, bignum_pow10(b.scale - a.scale));
        *b_out = b.unscaled;
        *scale_out = b.scale;
    }
}

Decimal decimal_add(Decimal a, Decimal b) {
    BigNum au, bu;
    int scale;
    align(a, b, &au, &bu, &scale);
    Decimal d;
    d.unscaled = bignum_add(au, bu);
    d.scale = scale;
    return d;
}

Decimal decimal_sub(Decimal a, Decimal b) {
    BigNum au, bu;
    int scale;
    align(a, b, &au, &bu, &scale);
    Decimal d;
    d.unscaled = bignum_sub(au, bu);
    d.scale = scale;
    return d;
}

Decimal decimal_mul(Decimal a, Decimal b) {
    Decimal d;
    d.unscaled = bignum_mul(a.unscaled, b.unscaled);
    d.scale = a.scale + b.scale;
    return d;
}

/* `base` raised to a non-negative integer `exponent` -- exact, the same way `decimal_mul`
 * is: unlike a fractional exponent (which is `sqrt`-shaped and irrational for almost every
 * input, so has no exact decimal answer at all), a whole-number power is repeated
 * multiplication, computed here by squaring (O(log exponent) multiplications) rather than a
 * plain loop. `exponent < 0` is the caller's job to reject (see prelude.c's wrapper) -- it
 * would need a division, which, like `decimal_div` itself, has to round to some scale, and
 * there's no one right scale to pick here the way `decimal_div`'s own two operands supply
 * one; composing `numerator / decimal_pow(base, -exponent)` already works today, `/`
 * picking its own target scale (max of its two operands', see decimal_div) from whatever
 * scale `numerator` was given -- e.g. `1.0000d / decimal_pow(2d, 3)` is `0.1250`, but a bare
 * `1d` (scale 0) would round the same division down to a whole number, `0`. */
Decimal decimal_pow(Decimal base, int exponent) {
    Decimal result = decimal_from_i64(1);
    Decimal b = base;
    while (exponent > 0) {
        if (exponent & 1) result = decimal_mul(result, b);
        b = decimal_mul(b, b);
        exponent >>= 1;
    }
    return result;
}

/* Shared core for decimal_div/decimal_mod/decimal_round: divides
 * `numerator`/`denominator` (already scaled so the quotient comes out
 * at the intended scale) and rounds the truncating quotient half-up
 * based on the remainder, using `round_sign` (the true mathematical
 * sign of the division, which a truncated-to-zero quotient can't tell
 * you on its own) to decide which direction "away from zero" is. */
static BigNum divide_rounding_half_up(BigNum numerator, BigNum denominator, int round_sign) {
    BigNum quot, rem;
    bignum_divmod(numerator, denominator, &quot, &rem);
    BigNum rem_doubled = bignum_add(bignum_abs(rem), bignum_abs(rem));
    if (bignum_compare(rem_doubled, bignum_abs(denominator)) >= 0) {
        BigNum one = bignum_from_i64(1);
        quot = round_sign < 0 ? bignum_sub(quot, one) : bignum_add(quot, one);
    }
    return quot;
}

Decimal decimal_div(Decimal a, Decimal b) {
    int target_scale = a.scale > b.scale ? a.scale : b.scale;
    int exponent = target_scale + b.scale - a.scale;
    BigNum numerator = a.unscaled;
    BigNum denominator = b.unscaled;
    if (exponent >= 0) {
        numerator = bignum_mul(numerator, bignum_pow10(exponent));
    } else {
        denominator = bignum_mul(denominator, bignum_pow10(-exponent));
    }
    int round_sign = (numerator.sign == 0 || denominator.sign == 0) ? 0 : numerator.sign * denominator.sign;
    Decimal d;
    d.unscaled = divide_rounding_half_up(numerator, denominator, round_sign);
    d.scale = target_scale;
    return d;
}

Decimal decimal_mod(Decimal a, Decimal b) {
    /* Integer part of a/b (scale 0), truncated exactly -- no rounding
     * involved, since this is only used to compute an exact remainder,
     * not a displayed quotient. */
    int exponent = b.scale - a.scale;
    BigNum numerator = a.unscaled;
    BigNum denominator = b.unscaled;
    if (exponent >= 0) {
        numerator = bignum_mul(numerator, bignum_pow10(exponent));
    } else {
        denominator = bignum_mul(denominator, bignum_pow10(-exponent));
    }
    BigNum trunc_quot, unused_rem;
    bignum_divmod(numerator, denominator, &trunc_quot, &unused_rem);
    Decimal int_quot;
    int_quot.unscaled = trunc_quot;
    int_quot.scale = 0;
    return decimal_sub(a, decimal_mul(int_quot, b));
}

int decimal_compare(Decimal a, Decimal b) {
    BigNum au, bu;
    int scale;
    align(a, b, &au, &bu, &scale);
    return bignum_compare(au, bu);
}

int decimal_is_zero(Decimal a) {
    return bignum_is_zero(a.unscaled);
}

Decimal decimal_normalize(Decimal a) {
    if (bignum_is_zero(a.unscaled)) return decimal_from_i64(0);
    if (a.scale == 0) return a;
    BigNum ten = bignum_from_i64(10);
    BigNum cur = a.unscaled;
    int scale = a.scale;
    while (scale > 0) {
        BigNum quot, rem;
        bignum_divmod(cur, ten, &quot, &rem);
        if (!bignum_is_zero(rem)) break;
        cur = quot;
        scale--;
    }
    Decimal d;
    d.unscaled = cur;
    d.scale = scale;
    return d;
}

Decimal decimal_round(Decimal a, int scale) {
    if (scale >= a.scale) {
        Decimal d;
        d.unscaled = bignum_mul(a.unscaled, bignum_pow10(scale - a.scale));
        d.scale = scale;
        return d;
    }
    BigNum divisor = bignum_pow10(a.scale - scale);
    Decimal d;
    d.unscaled = divide_rounding_half_up(a.unscaled, divisor, a.unscaled.sign);
    d.scale = scale;
    return d;
}

/* decimal_round's own two one-directional cousins: `bignum_divmod` truncates toward zero
 * (see its own comment), so a nonzero remainder means the truncated quotient overshot
 * toward zero from the true value -- floor corrects that only when `a` is negative (where
 * truncating already rounded up, toward zero, and floor wants the more negative neighbor);
 * ceil only when `a` is positive (truncating already rounded down, and ceil wants the more
 * positive neighbor). Scaling up (`scale >= a.scale`) is exact either way, same as round's. */
Decimal decimal_floor(Decimal a, int scale) {
    if (scale >= a.scale) {
        Decimal d;
        d.unscaled = bignum_mul(a.unscaled, bignum_pow10(scale - a.scale));
        d.scale = scale;
        return d;
    }
    BigNum divisor = bignum_pow10(a.scale - scale);
    BigNum quot, rem;
    bignum_divmod(a.unscaled, divisor, &quot, &rem);
    if (!bignum_is_zero(rem) && a.unscaled.sign < 0) {
        quot = bignum_sub(quot, bignum_from_i64(1));
    }
    Decimal d;
    d.unscaled = quot;
    d.scale = scale;
    return d;
}

Decimal decimal_ceil(Decimal a, int scale) {
    if (scale >= a.scale) {
        Decimal d;
        d.unscaled = bignum_mul(a.unscaled, bignum_pow10(scale - a.scale));
        d.scale = scale;
        return d;
    }
    BigNum divisor = bignum_pow10(a.scale - scale);
    BigNum quot, rem;
    bignum_divmod(a.unscaled, divisor, &quot, &rem);
    if (!bignum_is_zero(rem) && a.unscaled.sign > 0) {
        quot = bignum_add(quot, bignum_from_i64(1));
    }
    Decimal d;
    d.unscaled = quot;
    d.scale = scale;
    return d;
}

double decimal_to_double(Decimal a) {
    char *s = decimal_to_string(a);
    double v = strtod(s, NULL);
    free(s);
    return v;
}

Decimal decimal_from_double_trunc(double v) {
    /* Fixed at 10 decimal places -- %f never uses scientific notation
     * (unlike %g), so this is always a well-formed decimal_from_string
     * input regardless of v's magnitude. Normalized afterward so a
     * "clean" double like 3.14 (really ~3.1400000000000001 in binary)
     * casts to "3.14", not "3.1400000000" -- the trailing zeros are an
     * artifact of the fixed precision this conversion goes through, not
     * meaningful digits worth keeping. */
    char buf[512];
    snprintf(buf, sizeof(buf), "%.10f", v);
    int ok;
    return decimal_normalize(decimal_from_string(buf, &ok));
}

BigNum decimal_to_bignum_trunc(Decimal a) {
    if (a.scale == 0) return a.unscaled;
    BigNum divisor = bignum_pow10(a.scale);
    BigNum quot, rem;
    bignum_divmod(a.unscaled, divisor, &quot, &rem);
    return quot;
}

Decimal decimal_from_bignum(BigNum n) {
    Decimal d;
    d.unscaled = n;
    d.scale = 0;
    return d;
}
