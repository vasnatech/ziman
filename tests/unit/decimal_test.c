/* Standalone correctness test for src/decimal.c -- hand-verified
 * arithmetic (scale alignment, exact multiply, half-up-away-from-zero
 * rounding on division/round, exact remainder). Run via `make test-decimal`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/decimal.h"

static int failures = 0;

static Decimal mk(const char *s) {
    int ok;
    Decimal d = decimal_from_string(s, &ok);
    if (!ok) {
        printf("FAIL mk(%s): decimal_from_string rejected it\n", s);
        failures++;
    }
    return d;
}

static void expect_str(const char *label, Decimal d, const char *want) {
    char *got = decimal_to_string(d);
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got %s, want %s\n", label, got, want);
        failures++;
    } else {
        printf("PASS %s: %s\n", label, got);
    }
    free(got);
}

int main(void) {
    /* from_string / to_string round-trip */
    expect_str("19.99", mk("19.99"), "19.99");
    expect_str("-0.0825", mk("-0.0825"), "-0.0825");
    expect_str("20", mk("20"), "20");
    expect_str("0.005 (leading-zero padding)", mk("0.005"), "0.005");
    expect_str("-5", mk("-5"), "-5");
    expect_str("-.5 (no leading int digit)", mk("-.5"), "-0.5");

    int ok;
    decimal_from_string("5.", &ok);
    printf("%s rejects '5.' (no digits after '.')\n", ok == 0 ? "PASS" : "FAIL");
    if (ok != 0) failures++;
    decimal_from_string("", &ok);
    printf("%s rejects '' (empty)\n", ok == 0 ? "PASS" : "FAIL");
    if (ok != 0) failures++;
    decimal_from_string("-", &ok);
    printf("%s rejects '-' alone\n", ok == 0 ? "PASS" : "FAIL");
    if (ok != 0) failures++;
    decimal_from_string("12x34", &ok);
    printf("%s rejects '12x34' (garbage)\n", ok == 0 ? "PASS" : "FAIL");
    if (ok != 0) failures++;

    /* +/- with scale alignment */
    expect_str("1.5 + 0.25", decimal_add(mk("1.5"), mk("0.25")), "1.75");
    expect_str("-5 + 3", decimal_add(mk("-5"), mk("3")), "-2");
    expect_str("10.5 - 3", decimal_sub(mk("10.5"), mk("3")), "7.5");

    /* * needs no alignment, always exact */
    expect_str("1.5 * 0.25", decimal_mul(mk("1.5"), mk("0.25")), "0.375");

    /* / rounds half-up-away-from-zero to max(a.scale, b.scale) */
    expect_str("1.00 / 4 (exact)", decimal_div(mk("1.00"), mk("4")), "0.25");
    expect_str("10 / 3 (rounds down)", decimal_div(mk("10"), mk("3")), "3");
    expect_str("2 / 3 (rounds up)", decimal_div(mk("2"), mk("3")), "1");
    expect_str("-2 / 3 (rounds away from zero)", decimal_div(mk("-2"), mk("3")), "-1");

    /* % is an exact remainder (a - trunc(a/b)*b) */
    expect_str("10.5 % 3", decimal_mod(mk("10.5"), mk("3")), "1.5");

    /* == ignores trailing-zero scale differences (not Java's equals() gotcha) */
    printf("%s 1.0 == 1.00\n", decimal_compare(mk("1.0"), mk("1.00")) == 0 ? "PASS" : "FAIL");
    if (decimal_compare(mk("1.0"), mk("1.00")) != 0) failures++;
    printf("%s 1.0 != 1.01\n", decimal_compare(mk("1.0"), mk("1.01")) != 0 ? "PASS" : "FAIL");
    if (decimal_compare(mk("1.0"), mk("1.01")) == 0) failures++;

    /* round to a coarser scale, half-up */
    expect_str("round(1.2345, 2) (rounds down)", decimal_round(mk("1.2345"), 2), "1.23");
    expect_str("round(1.2365, 2) (rounds up)", decimal_round(mk("1.2365"), 2), "1.24");
    expect_str("round(1.5, 3) (pads with zeros, no rounding)", decimal_round(mk("1.5"), 3), "1.500");

    /* a genuinely large decimal, well beyond double's exact-integer range,
     * cross-checked the same way bignum_test.c verifies bigint: build a
     * huge exact value and confirm it survives a multiply/divide round trip. */
    Decimal big = mk("123456789012345678901234567890.123456789");
    Decimal doubled = decimal_add(big, big);
    Decimal halved = decimal_div(doubled, decimal_from_i64(2));
    printf("%s (big+big)/2 == big\n", decimal_compare(halved, big) == 0 ? "PASS" : "FAIL");
    if (decimal_compare(halved, big) != 0) failures++;

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
