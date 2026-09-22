/* Standalone correctness test for src/bignum.c -- validated against
 * well-known published factorial values and cross-checked relationships
 * between them. Lives outside src/ (which the Makefile wildcards into
 * the ziman binary) since this file has its own main() that would
 * collide with main.c's at link time. Run via `make test-bignum`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/bignum.h"

static int failures = 0;

static void expect_str(const char *label, BigNum n, const char *want) {
    char *got = bignum_to_string(n);
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got %s, want %s\n", label, got, want);
        failures++;
    } else {
        printf("PASS %s: %s\n", label, got);
    }
    free(got);
}

int main(void) {
    /* round-trip small/edge int64 values */
    expect_str("from_i64(0)", bignum_from_i64(0), "0");
    expect_str("from_i64(1)", bignum_from_i64(1), "1");
    expect_str("from_i64(-1)", bignum_from_i64(-1), "-1");
    expect_str("from_i64(INT64_MAX)", bignum_from_i64(9223372036854775807LL), "9223372036854775807");
    expect_str("from_i64(INT64_MIN)", bignum_from_i64(-9223372036854775807LL - 1), "-9223372036854775808");

    /* from_string round-trip, including a value far past int64 range */
    int ok;
    BigNum big = bignum_from_string("123456789012345678901234567890", &ok);
    printf("%s from_string ok flag = %d\n", ok ? "PASS" : "FAIL", ok);
    if (!ok) failures++;
    expect_str("from_string round-trip", big, "123456789012345678901234567890");

    BigNum bad = bignum_from_string("12x34", &ok);
    printf("%s from_string rejects garbage, ok=%d\n", ok == 0 ? "PASS" : "FAIL", ok);
    if (ok != 0) failures++;
    (void)bad;

    /* addition / subtraction, including sign crossing */
    BigNum a = bignum_from_i64(999999999);
    BigNum one = bignum_from_i64(1);
    expect_str("999999999 + 1 (limb carry)", bignum_add(a, one), "1000000000");

    BigNum neg5 = bignum_from_i64(-5);
    BigNum pos3 = bignum_from_i64(3);
    expect_str("-5 + 3", bignum_add(neg5, pos3), "-2");
    expect_str("3 + -5", bignum_add(pos3, neg5), "-2");
    expect_str("-5 - -5", bignum_sub(neg5, neg5), "0");

    /* factorials, cross-checked against well-known published values */
    BigNum fact = bignum_from_i64(1);
    BigNum facts[31];
    facts[0] = fact;
    for (int i = 1; i <= 30; i++) {
        fact = bignum_mul(fact, bignum_from_i64(i));
        facts[i] = fact;
    }
    expect_str("10!", facts[10], "3628800");
    expect_str("20!", facts[20], "2432902008176640000");
    expect_str("25!", facts[25], "15511210043330985984000000");
    expect_str("30!", facts[30], "265252859812191058636308480000000");

    /* division: 30! / 25! should be exactly 26*27*28*29*30 = 17100720, remainder 0 */
    BigNum quot, rem;
    bignum_divmod(facts[30], facts[25], &quot, &rem);
    expect_str("30! / 25!", quot, "17100720");
    expect_str("30! % 25! (exact)", rem, "0");

    /* division with a genuine remainder, negative operands (truncating semantics) */
    BigNum seven = bignum_from_i64(7), two = bignum_from_i64(2);
    bignum_divmod(seven, two, &quot, &rem);
    expect_str("7 / 2", quot, "3");
    expect_str("7 % 2", rem, "1");

    BigNum neg7 = bignum_from_i64(-7);
    bignum_divmod(neg7, two, &quot, &rem);
    expect_str("-7 / 2 (truncate toward zero)", quot, "-3");
    expect_str("-7 % 2 (remainder takes dividend's sign)", rem, "-1");

    /* compare */
    printf("%s compare(25!, 30!) < 0\n", bignum_compare(facts[25], facts[30]) < 0 ? "PASS" : "FAIL");
    if (bignum_compare(facts[25], facts[30]) >= 0) failures++;
    printf("%s compare(30!, 30!) == 0\n", bignum_compare(facts[30], facts[30]) == 0 ? "PASS" : "FAIL");
    if (bignum_compare(facts[30], facts[30]) != 0) failures++;

    /* saturating conversion */
    printf("%s to_i64_saturating(30!) == INT64_MAX: %lld\n",
           bignum_to_i64_saturating(facts[30]) == 9223372036854775807LL ? "PASS" : "FAIL",
           (long long)bignum_to_i64_saturating(facts[30]));
    if (bignum_to_i64_saturating(facts[30]) != 9223372036854775807LL) failures++;
    printf("%s to_i64_saturating(-30!) == INT64_MIN\n",
           bignum_to_i64_saturating(bignum_negate(facts[30])) == (-9223372036854775807LL - 1) ? "PASS" : "FAIL");
    if (bignum_to_i64_saturating(bignum_negate(facts[30])) != (-9223372036854775807LL - 1)) failures++;
    expect_str("to_i64_saturating round-trip for a small value",
               bignum_from_i64(bignum_to_i64_saturating(bignum_from_i64(42))), "42");

    /* multiply-then-divide round trip on a genuinely large, non-factorial
     * multi-limb value, stressing multiplication and the long-division
     * binary search on both a large dividend and a large divisor. */
    BigNum x = bignum_from_string("98765432109876543210987654321", &ok);
    BigNum y = bignum_mul(x, x);
    bignum_divmod(y, x, &quot, &rem);
    printf("%s x*x / x == x\n", bignum_compare(quot, x) == 0 ? "PASS" : "FAIL");
    if (bignum_compare(quot, x) != 0) failures++;
    expect_str("x*x %% x == 0", rem, "0");

    BigNum x_plus_1 = bignum_add(x, one);
    bignum_divmod(y, x_plus_1, &quot, &rem);
    /* y = x*x, dividing by (x+1): quotient is x-1, remainder is x
     * (since x*x = (x+1)*(x-1) + 1*x... check: (x+1)(x-1) = x^2-1, so
     * x^2 = (x+1)(x-1) + 1, remainder should be 1, not x -- corrected). */
    expect_str("x*x / (x+1)", quot, "98765432109876543210987654320");
    expect_str("x*x % (x+1)", rem, "1");

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
