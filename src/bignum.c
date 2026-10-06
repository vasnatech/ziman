#define _POSIX_C_SOURCE 200809L
#include "bignum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- magnitude-only helpers (ignore sign entirely) ---- */

static uint32_t *mag_trim(uint32_t *digits, int *count) {
    while (*count > 0 && digits[*count - 1] == 0) (*count)--;
    return digits;
}

static int mag_compare(const uint32_t *a, int na, const uint32_t *b, int nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (int i = na - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static uint32_t *mag_add(const uint32_t *a, int na, const uint32_t *b, int nb, int *out_count) {
    int n = (na > nb ? na : nb) + 1;
    uint32_t *out = calloc((size_t)n, sizeof(uint32_t));
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t sum = carry;
        if (i < na) sum += a[i];
        if (i < nb) sum += b[i];
        out[i] = (uint32_t)(sum % BIGNUM_BASE);
        carry = sum / BIGNUM_BASE;
    }
    int count = n;
    mag_trim(out, &count);
    *out_count = count;
    return out;
}

/* Requires a >= b as magnitudes -- callers always compare first. */
static uint32_t *mag_sub(const uint32_t *a, int na, const uint32_t *b, int nb, int *out_count) {
    uint32_t *out = calloc((size_t)(na > 0 ? na : 1), sizeof(uint32_t));
    int64_t borrow = 0;
    for (int i = 0; i < na; i++) {
        int64_t diff = (int64_t)a[i] - borrow - (i < nb ? (int64_t)b[i] : 0);
        if (diff < 0) {
            diff += BIGNUM_BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        out[i] = (uint32_t)diff;
    }
    int count = na;
    mag_trim(out, &count);
    *out_count = count;
    return out;
}

/* How many limbs of scratch the magnitude helpers below keep on the stack before falling back to
 * an allocation. 64 limbs is 576 decimal digits -- far past every `decimal` value this language can
 * hold, and past any integer an ordinary program multiplies. */
#define MAG_SCRATCH_LIMBS 64

/* `limb * b`, written into caller-provided `out` (which must have room for `nb + 1` limbs) and
 * returning the trimmed limb count -- the allocation-free form of `mag_mul` for the one shape that
 * dominates division. `mag_divmod`'s binary search below computes exactly this product once per
 * bisection step, about thirty times per dividend limb, and each `mag_mul` call cost two
 * allocations to materialise a result read once and thrown away. Semantics match
 * `mag_mul(&limb, limb == 0 ? 0 : 1, b, nb, ...)` exactly, including the zero case's count of 0.
 * Limbs above the returned count are left as they were: every reader here (`mag_compare`,
 * `mag_sub`) is bounded by the count, which is what makes one buffer reusable across probes. */
static int mag_mul_limb(uint32_t *out, const uint32_t *b, int nb, uint32_t limb) {
    if (limb == 0 || nb == 0) return 0;
    uint64_t carry = 0;
    for (int j = 0; j < nb; j++) {
        uint64_t cur = (uint64_t)limb * (uint64_t)b[j] + carry;
        out[j] = (uint32_t)(cur % BIGNUM_BASE);
        carry = cur / BIGNUM_BASE;
    }
    int count = nb;
    while (carry > 0) {
        out[count++] = (uint32_t)(carry % BIGNUM_BASE);
        carry /= BIGNUM_BASE;
    }
    mag_trim(out, &count);
    return count;
}

static uint32_t *mag_mul(const uint32_t *a, int na, const uint32_t *b, int nb, int *out_count) {
    if (na == 0 || nb == 0) {
        *out_count = 0;
        return calloc(1, sizeof(uint32_t));
    }
    int n = na + nb;
    /* The accumulator is pure scratch -- copied into `out` and dropped -- so it comes off the stack
     * whenever it fits. That matters more than it looks: `free` is a no-op in a collector build
     * (zgc.h), so every multiply used to hand Boehm one more block to sweep for nothing. The cap
     * covers any product up to MAG_SCRATCH_LIMBS*9 decimal digits, which is every `decimal` this
     * language has and all but enormous integers; anything larger still allocates. */
    uint64_t scratch[MAG_SCRATCH_LIMBS];
    uint64_t *acc = n <= MAG_SCRATCH_LIMBS ? scratch : calloc((size_t)n, sizeof(uint64_t));
    if (acc == scratch) memset(acc, 0, (size_t)n * sizeof(uint64_t));
    for (int i = 0; i < na; i++) {
        if (a[i] == 0) continue;
        uint64_t carry = 0;
        int j;
        for (j = 0; j < nb; j++) {
            uint64_t cur = acc[i + j] + (uint64_t)a[i] * (uint64_t)b[j] + carry;
            acc[i + j] = cur % BIGNUM_BASE;
            carry = cur / BIGNUM_BASE;
        }
        int k = i + j;
        while (carry > 0) {
            uint64_t cur = acc[k] + carry;
            acc[k] = cur % BIGNUM_BASE;
            carry = cur / BIGNUM_BASE;
            k++;
        }
    }
    uint32_t *out = calloc((size_t)n, sizeof(uint32_t));
    for (int i = 0; i < n; i++) out[i] = (uint32_t)acc[i];
    if (acc != scratch) free(acc);
    int count = n;
    mag_trim(out, &count);
    *out_count = count;
    return out;
}

/* Long division, most-significant limb first: bring the next dividend
 * limb down into a running remainder, then binary-search the largest
 * quotient digit q in [0, BASE) with q*divisor <= remainder. O(na *
 * log(BASE) * nb) -- schoolbook-simple and easy to verify by hand,
 * rather than the faster (and much fiddlier to get exactly right)
 * normalized-estimate-plus-correction algorithms real bignum libraries
 * use. `nb` must be > 0 (caller already rejected division by zero). */
static void mag_divmod(const uint32_t *a, int na, const uint32_t *b, int nb,
                        uint32_t **out_quot, int *out_quot_count,
                        uint32_t **out_rem, int *out_rem_count) {
    uint32_t *quot = calloc((size_t)(na > 0 ? na : 1), sizeof(uint32_t));
    uint32_t *rem = calloc(1, sizeof(uint32_t));
    int rem_count = 0;

    /* One product buffer for the whole division, reused by every bisection probe and by the
     * subtract below -- see mag_mul_limb for what this replaced. `nb + 1` is the most limbs a
     * single-limb product can have; the extra limb keeps mag_mul_limb's carry loop in bounds. */
    uint32_t prod_stack[MAG_SCRATCH_LIMBS];
    uint32_t *prod = nb + 2 <= MAG_SCRATCH_LIMBS ? prod_stack : calloc((size_t)nb + 2, sizeof(uint32_t));

    for (int i = na - 1; i >= 0; i--) {
        uint32_t *shifted = calloc((size_t)(rem_count + 1), sizeof(uint32_t));
        shifted[0] = a[i];
        for (int k = 0; k < rem_count; k++) shifted[k + 1] = rem[k];
        free(rem);
        rem = shifted;
        rem_count = rem_count + 1;
        mag_trim(rem, &rem_count);

        int64_t lo = 0, hi = BIGNUM_BASE - 1, q = 0;
        while (lo <= hi) {
            int64_t mid = lo + (hi - lo) / 2;
            int prod_count = mag_mul_limb(prod, b, nb, (uint32_t)mid);
            int cmp = mag_compare(prod, prod_count, rem, rem_count);
            if (cmp <= 0) {
                q = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        quot[i] = (uint32_t)q;

        if (q > 0) {
            int prod_count = mag_mul_limb(prod, b, nb, (uint32_t)q);
            int new_rem_count;
            uint32_t *diff = mag_sub(rem, rem_count, prod, prod_count, &new_rem_count);
            free(rem);
            rem = diff;
            rem_count = new_rem_count;
        }
    }

    if (prod != prod_stack) free(prod);
    int quot_count = na;
    mag_trim(quot, &quot_count);
    *out_quot = quot;
    *out_quot_count = quot_count;
    *out_rem = rem;
    *out_rem_count = rem_count;
}

/* ---- sign-aware BigNum API ---- */

static BigNum make(int sign, uint32_t *digits, int count) {
    BigNum n;
    if (count == 0) {
        free(digits);
        n.sign = 0;
        n.digits = NULL;
        n.count = 0;
        return n;
    }
    n.sign = sign;
    n.digits = digits;
    n.count = count;
    return n;
}

BigNum bignum_from_i64(int64_t v) {
    if (v == 0) return make(0, NULL, 0);
    int sign = v < 0 ? -1 : 1;
    /* -INT64_MIN overflows int64_t; do the magnitude split in uint64_t. */
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    uint32_t *digits = calloc(3, sizeof(uint32_t)); /* max ~1.8e19 needs 3 limbs of 1e9 */
    int count = 0;
    while (mag > 0) {
        digits[count++] = (uint32_t)(mag % BIGNUM_BASE);
        mag /= BIGNUM_BASE;
    }
    return make(sign, digits, count);
}

BigNum bignum_from_string(const char *s, int *ok) {
    *ok = 1;
    int sign = 1;
    size_t i = 0;
    if (s[0] == '-') {
        sign = -1;
        i = 1;
    }
    size_t len = strlen(s);
    if (i >= len) {
        *ok = 0;
        return make(0, NULL, 0);
    }
    for (size_t j = i; j < len; j++) {
        if (s[j] < '0' || s[j] > '9') {
            *ok = 0;
            return make(0, NULL, 0);
        }
    }
    int ndigits = (int)(len - i);
    int count = (ndigits + 8) / 9;
    uint32_t *digits = calloc((size_t)(count > 0 ? count : 1), sizeof(uint32_t));
    /* Fill limbs from least-significant (the string's tail) backward. */
    int limb = 0;
    for (int end = (int)len; end > (int)i; end -= 9) {
        int start = end - 9;
        if (start < (int)i) start = (int)i;
        char buf[10];
        int n = end - start;
        memcpy(buf, s + start, (size_t)n);
        buf[n] = '\0';
        digits[limb++] = (uint32_t)strtoul(buf, NULL, 10);
    }
    mag_trim(digits, &count);
    if (count == 0) sign = 0;
    return make(sign, digits, count);
}

char *bignum_to_string(BigNum a) {
    if (a.count == 0) return strdup("0");
    size_t cap = (size_t)a.count * 9 + 2;
    char *out = malloc(cap);
    size_t len = 0;
    if (a.sign < 0) out[len++] = '-';
    len += (size_t)snprintf(out + len, cap - len, "%u", a.digits[a.count - 1]);
    for (int i = a.count - 2; i >= 0; i--) {
        len += (size_t)snprintf(out + len, cap - len, "%09u", a.digits[i]);
    }
    return out;
}

BigNum bignum_negate(BigNum a) {
    if (a.sign == 0) return a;
    uint32_t *digits = malloc((size_t)a.count * sizeof(uint32_t));
    memcpy(digits, a.digits, (size_t)a.count * sizeof(uint32_t));
    return make(-a.sign, digits, a.count);
}

BigNum bignum_abs(BigNum a) {
    if (a.sign >= 0) return a;
    return bignum_negate(a);
}

BigNum bignum_add(BigNum a, BigNum b) {
    if (a.sign == 0) return b;
    if (b.sign == 0) return a;
    if (a.sign == b.sign) {
        int count;
        uint32_t *digits = mag_add(a.digits, a.count, b.digits, b.count, &count);
        return make(a.sign, digits, count);
    }
    int cmp = mag_compare(a.digits, a.count, b.digits, b.count);
    if (cmp == 0) return make(0, NULL, 0);
    int count;
    if (cmp > 0) {
        uint32_t *digits = mag_sub(a.digits, a.count, b.digits, b.count, &count);
        return make(a.sign, digits, count);
    }
    uint32_t *digits = mag_sub(b.digits, b.count, a.digits, a.count, &count);
    return make(b.sign, digits, count);
}

BigNum bignum_sub(BigNum a, BigNum b) {
    return bignum_add(a, bignum_negate(b));
}

BigNum bignum_mul(BigNum a, BigNum b) {
    if (a.sign == 0 || b.sign == 0) return make(0, NULL, 0);
    int count;
    uint32_t *digits = mag_mul(a.digits, a.count, b.digits, b.count, &count);
    return make(a.sign * b.sign, digits, count);
}

void bignum_divmod(BigNum a, BigNum b, BigNum *quot, BigNum *rem) {
    if (a.sign == 0) {
        *quot = make(0, NULL, 0);
        *rem = make(0, NULL, 0);
        return;
    }
    int qcount, rcount;
    uint32_t *qdigits, *rdigits;
    mag_divmod(a.digits, a.count, b.digits, b.count, &qdigits, &qcount, &rdigits, &rcount);
    *quot = make(qcount == 0 ? 0 : a.sign * b.sign, qdigits, qcount);
    *rem = make(rcount == 0 ? 0 : a.sign, rdigits, rcount); /* remainder takes the dividend's sign */
}

int bignum_compare(BigNum a, BigNum b) {
    if (a.sign != b.sign) return a.sign < b.sign ? -1 : 1;
    if (a.sign == 0) return 0;
    int cmp = mag_compare(a.digits, a.count, b.digits, b.count);
    return a.sign > 0 ? cmp : -cmp;
}

int bignum_is_zero(BigNum a) {
    return a.sign == 0;
}

/* Via the decimal text, exactly as decimal_to_double does, because `strtod` is the one conversion
 * that already handles every magnitude correctly -- including rounding to the nearest double and
 * answering infinity for a value past DBL_MAX. The obvious shortcut, an int64 round trip, was what
 * this replaced: it saturated 123456789012345678901234567890 to 9223372036854775807 and so reported
 * 9.2e18 for a number whose magnitude is 1.2e29. Losing a float's worth of low digits is inherent to
 * `as float`; losing the magnitude is not. */
double bignum_to_double(BigNum a) {
    char *s = bignum_to_string(a);
    double v = strtod(s, NULL);
    free(s);
    return v;
}

int64_t bignum_to_i64_saturating(BigNum a) {
    if (a.sign == 0) return 0;
    /* INT64_MAX is 9223372036854775807 -- 3 limbs of base 1e9 at most,
     * so anything with more than 3 limbs is unconditionally out of
     * range; no need to even compare magnitudes in that case. */
    if (a.count > 3) return a.sign > 0 ? INT64_MAX : INT64_MIN;

    /* Deliberately avoids __int128 (a GCC/Clang extension that would
     * trip -Wpedantic) -- INT64_MIN's own magnitude
     * (9223372036854775808) already doesn't fit in int64_t, so the
     * range check and the accumulation both have to work in uint64_t
     * (which comfortably holds it) and convert to signed only at the
     * very end. */
    int ok;
    BigNum max_mag = bignum_from_i64(INT64_MAX);
    BigNum min_mag = bignum_from_string("9223372036854775808", &ok);
    BigNum a_mag = bignum_abs(a);
    int out_of_range = a.sign > 0 ? mag_compare(a_mag.digits, a_mag.count, max_mag.digits, max_mag.count) > 0
                                   : mag_compare(a_mag.digits, a_mag.count, min_mag.digits, min_mag.count) > 0;
    free(max_mag.digits);
    free(min_mag.digits);
    if (out_of_range) return a.sign > 0 ? INT64_MAX : INT64_MIN;

    uint64_t mag = 0;
    for (int i = a.count - 1; i >= 0; i--) {
        mag = mag * BIGNUM_BASE + a.digits[i];
    }
    if (a.sign > 0) return (int64_t)mag;
    if (mag == 9223372036854775808ULL) return INT64_MIN; /* exact boundary -- negating would overflow */
    return -(int64_t)mag;
}

BigNum bignum_pow10(int n) {
    if (n == 0) return bignum_from_i64(1);
    char *s = malloc((size_t)n + 2);
    s[0] = '1';
    memset(s + 1, '0', (size_t)n);
    s[n + 1] = '\0';
    int ok;
    BigNum result = bignum_from_string(s, &ok);
    free(s);
    return result;
}
