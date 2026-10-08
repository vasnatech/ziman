#define _POSIX_C_SOURCE 200809L
#include "native_bits.h"

#include <stdint.h>
#include <stdlib.h>

#include "bignum.h"
#include "error.h"
#include "native.h"
#include "xalloc.h"

/* The `bits` core module's natives: bitwise and/or/xor/not/shift on `byte`, `int` and `bigint`.
 * See lib/core/bits.zm and docs/DESIGN.md's "A bits module" entry for why `bigint`'s half
 * needs a whole binary-conversion layer that `int`'s and `byte`'s native ops do not. */

static void check_shift_amount(int64_t n, const char *name, int line, int col) {
    if (n < 0 || n > 63) {
        ziman_error("runtime", line, col, "%s: shift amount %lld out of range (0 to 63)", name, (long long)n);
    }
}

static void check_byte_shift_amount(int64_t n, const char *name, int line, int col) {
    if (n < 0 || n > 7) {
        ziman_error("runtime", line, col, "%s: shift amount %lld out of range (0 to 7)", name, (long long)n);
    }
}

/* ---- byte: unsigned, no sign to worry about -- and/or/xor/not are plain C bitwise ops on an
 * `int` holding 0-255, shift_right is a logical (not arithmetic) shift for the same reason. */

static Value native_byte_and(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_byte(args[0].as.by & args[1].as.by);
}

static Value native_byte_or(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_byte(args[0].as.by | args[1].as.by);
}

static Value native_byte_xor(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_byte(args[0].as.by ^ args[1].as.by);
}

static Value native_byte_not(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_byte((~args[0].as.by) & 0xFF);
}

static Value native_byte_shift_left(Value *args, int argc, int line, int col) {
    (void)argc;
    check_byte_shift_amount(args[1].as.i, "byte_shift_left", line, col);
    return value_byte((args[0].as.by << args[1].as.i) & 0xFF);
}

static Value native_byte_shift_right(Value *args, int argc, int line, int col) {
    (void)argc;
    check_byte_shift_amount(args[1].as.i, "byte_shift_right", line, col);
    return value_byte(args[0].as.by >> args[1].as.i);
}

static Value native_byte_popcount(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int(__builtin_popcount((unsigned)args[0].as.by));
}

static Value native_int_and(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int(args[0].as.i & args[1].as.i);
}

static Value native_int_or(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int(args[0].as.i | args[1].as.i);
}

static Value native_int_xor(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int(args[0].as.i ^ args[1].as.i);
}

static Value native_int_not(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int(~args[0].as.i);
}

/* Arithmetic (sign-extending): -8 >> 1 is -4, not the 2's-complement bit pattern's unsigned
 * value. Floors, unlike `/` which truncates toward zero -- shift_right(-3, 1) is -2, not -1
 * (-3 / 2). GCC and Clang both implement C's implementation-defined signed `>>` this way on
 * every platform this project targets (and C23 makes it mandatory), so this is not relying on
 * anything the rest of the codebase doesn't already lean on (int_to_hex's own two's-complement
 * framing, for one). */
/* The left shift goes through `uint64_t`, which the right one above does not need to. Bit 63 is
 * why: check_shift_amount permits an amount of exactly 63, so `int_shift_left(1, 63)` shifts a 1
 * into the sign bit of a signed 64-bit value, and C11 6.5.7p4 leaves that undefined -- UBSan says
 * so ("left shift of 1 by 63 places cannot be represented in type 'long int'"). It worked, and the
 * reason it worked was not `-fwrapv`: that flag covers signed overflow in arithmetic and was
 * measured to make no difference here, the same bit pattern coming out under -O2, -O3, -fwrapv and
 * -fstrict-overflow alike. It worked because GCC documents signed `<<` as acting on the bit
 * pattern, an extension no other compiler owes us -- so this converts, which is defined
 * everywhere, costs no instructions, and leaves bit 63 giving the same INT64_MIN it always did
 * (docs/reviews/tasks.md A21). */
static Value native_int_shift_left(Value *args, int argc, int line, int col) {
    (void)argc;
    check_shift_amount(args[1].as.i, "int_shift_left", line, col);
    return value_int((int64_t)((uint64_t)args[0].as.i << args[1].as.i));
}

static Value native_int_shift_right(Value *args, int argc, int line, int col) {
    (void)argc;
    check_shift_amount(args[1].as.i, "int_shift_right", line, col);
    return value_int(args[0].as.i >> args[1].as.i);
}

/* ---- bigint: infinite two's complement over a base-2^32 binary view ------------------
 *
 * `bignum.c` stores sign-magnitude in base-1e9 decimal limbs (fast string conversion, no binary
 * view). To match Java's `BigInteger`/Python's `int` exactly -- and(-1n, 5n) is 5n, because a
 * negative number conceptually has infinitely many leading 1-bits -- every operation here:
 *   1. converts each operand's magnitude to little-endian base-2^32 words (repeated divmod by
 *      2^32, reversed by Horner's method with the existing bignum_mul/bignum_add -- the same
 *      "not chasing performance" tradeoff the decimal-limb choice itself already made),
 *   2. brings a negative operand into two's-complement form over a common, padded word count
 *      (invert then add 1 -- negate_words, used both ways: bringing a negative operand in, and
 *      bringing a negative *result* back out to sign-magnitude),
 *   3. does the op word by word, and
 *   4. reads the top word's sign bit to know whether to negate the result back out.
 * `not` needs none of this: `not(x) = -(x + 1)` is Java's own definition, already exact via
 * plain bignum arithmetic. */

#define WORD_BASE_I64 4294967296LL /* 2^32 */

/* Little-endian base-2^32 words of `a`'s magnitude (sign ignored). `*out_count` may be 0 (a
 * caller-owned, possibly-NULL-if-zero-length buffer) when `a` is zero. */
static uint32_t *bignum_magnitude_to_words(BigNum a, int *out_count) {
    BigNum base = bignum_from_i64(WORD_BASE_I64);
    BigNum rest = bignum_abs(a);
    int cap = 4, count = 0;
    uint32_t *words = ziman_xmalloc((size_t)cap * sizeof(uint32_t));
    while (!bignum_is_zero(rest)) {
        BigNum q, r;
        bignum_divmod(rest, base, &q, &r);
        if (count == cap) {
            cap *= 2;
            words = ziman_xrealloc(words, (size_t)cap * sizeof(uint32_t));
        }
        words[count++] = (uint32_t)bignum_to_i64_saturating(r); /* r is always < 2^32: exact, no saturation */
        rest = q;
    }
    *out_count = count;
    return words;
}

/* The inverse of bignum_magnitude_to_words: a non-negative BigNum from little-endian base-2^32
 * words, most significant first (Horner's method). */
static BigNum words_to_bignum_magnitude(const uint32_t *words, int count) {
    BigNum base = bignum_from_i64(WORD_BASE_I64);
    BigNum result = bignum_from_i64(0);
    for (int i = count - 1; i >= 0; i--) {
        result = bignum_mul(result, base);
        result = bignum_add(result, bignum_from_i64((int64_t)words[i]));
    }
    return result;
}

/* In place: `w[0..len)` becomes its own two's-complement negation (`~w + 1`) over exactly
 * `len` words. The one operation both directions need -- negating a non-negative magnitude
 * gives its negative's two's-complement form; negating a negative two's-complement value a
 * second time gives its magnitude back. */
static void negate_words(uint32_t *w, int len) {
    uint64_t carry = 1;
    for (int i = 0; i < len; i++) {
        uint64_t s = (uint64_t)(~w[i]) + carry;
        w[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

typedef enum { BIT_AND, BIT_OR, BIT_XOR } BitOp;

static BigNum bignum_bitop(BigNum a, BigNum b, BitOp op) {
    int la, lb;
    uint32_t *wa = bignum_magnitude_to_words(a, &la);
    uint32_t *wb = bignum_magnitude_to_words(b, &lb);
    int longer = la > lb ? la : lb;
    int L = longer + 1; /* one padding word, so the sign bit always has room to be meaningful */

    uint32_t *ta = ziman_xcalloc((size_t)L, sizeof(uint32_t));
    uint32_t *tb = ziman_xcalloc((size_t)L, sizeof(uint32_t));
    for (int i = 0; i < la; i++) ta[i] = wa[i];
    for (int i = 0; i < lb; i++) tb[i] = wb[i];
    if (a.sign < 0) negate_words(ta, L);
    if (b.sign < 0) negate_words(tb, L);

    uint32_t *tr = ziman_xmalloc((size_t)L * sizeof(uint32_t));
    for (int i = 0; i < L; i++) {
        switch (op) {
            case BIT_AND: tr[i] = ta[i] & tb[i]; break;
            case BIT_OR:  tr[i] = ta[i] | tb[i]; break;
            case BIT_XOR: tr[i] = ta[i] ^ tb[i]; break;
        }
    }
    free(wa); free(wb); free(ta); free(tb);

    int negative = (tr[L - 1] & 0x80000000u) != 0;
    if (negative) negate_words(tr, L); /* bring a negative two's-complement result back to a magnitude */

    BigNum mag = words_to_bignum_magnitude(tr, L);
    free(tr);
    return negative ? bignum_negate(mag) : mag;
}

/* 2^n via exponentiation by squaring on the existing bignum_mul -- O(log n) multiplications,
 * reusing tested arithmetic rather than writing a second, word-level shift routine. */
static BigNum bignum_pow2(int n) {
    BigNum base = bignum_from_i64(2);
    BigNum result = bignum_from_i64(1);
    while (n > 0) {
        if (n & 1) result = bignum_mul(result, base);
        base = bignum_mul(base, base);
        n >>= 1;
    }
    return result;
}

static int check_bigint_shift_amount(Value v, const char *name, int line, int col) {
    int64_t n = v.as.i;
    if (n < 0) ziman_error("runtime", line, col, "%s: shift amount %lld must not be negative", name, (long long)n);
    if (n > INT32_MAX) ziman_error("runtime", line, col, "%s: shift amount %lld is too large", name, (long long)n);
    return (int)n;
}

static Value native_bigint_and(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bigint(bignum_bitop(*args[0].as.bi, *args[1].as.bi, BIT_AND));
}

static Value native_bigint_or(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bigint(bignum_bitop(*args[0].as.bi, *args[1].as.bi, BIT_OR));
}

static Value native_bigint_xor(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bigint(bignum_bitop(*args[0].as.bi, *args[1].as.bi, BIT_XOR));
}

/* not(x) = -(x + 1) -- Java's own BigInteger.not() definition, exact via plain arithmetic;
 * needs none of the binary-conversion machinery above. */
static Value native_bigint_not(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bigint(bignum_negate(bignum_add(*args[0].as.bi, bignum_from_i64(1))));
}

static Value native_bigint_shift_left(Value *args, int argc, int line, int col) {
    (void)argc;
    int n = check_bigint_shift_amount(args[1], "bigint_shift_left", line, col);
    return value_bigint(bignum_mul(*args[0].as.bi, bignum_pow2(n)));
}

/* Floors (rounds toward negative infinity), unlike `/` which truncates toward zero --
 * bignum_divmod already truncates, so a negative dividend with a nonzero remainder needs the
 * quotient nudged down by one, the same floor-adjustment int_shift_right's own doc comment
 * describes for the native int case. */
static Value native_bigint_shift_right(Value *args, int argc, int line, int col) {
    (void)argc;
    int n = check_bigint_shift_amount(args[1], "bigint_shift_right", line, col);
    BigNum x = *args[0].as.bi;
    BigNum divisor = bignum_pow2(n);
    BigNum q, r;
    bignum_divmod(x, divisor, &q, &r);
    if (x.sign < 0 && !bignum_is_zero(r)) q = bignum_sub(q, bignum_from_i64(1));
    return value_bigint(q);
}

/* ---- popcount and bit-length, for BitSet.cardinality/length and BigBitSet's own -----
 *
 * Both awkward (int_popcount) or outright inefficient (a huge bigint's bit-length, looping bit
 * by bit with no known upper bound to stop at) to write correctly in pure Ziman, so they get a
 * native home here rather than being built from the bitwise primitives above. */

static Value native_int_popcount(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int(__builtin_popcountll((uint64_t)args[0].as.i));
}

/* Meaningful only for a non-negative bigint -- exactly `BigBitSet`'s own invariant (a set of
 * indices, never negative), so the boundary this draws is the one that matters in practice.
 * A negative bigint's popcount is infinite under two's complement (infinitely many 1-bits),
 * so there is no finite answer to give instead of rejecting it. */
static Value native_bigint_popcount(Value *args, int argc, int line, int col) {
    (void)argc;
    BigNum x = *args[0].as.bi;
    if (x.sign < 0) ziman_error("runtime", line, col, "bigint_popcount: undefined for a negative bigint");
    int count;
    uint32_t *words = bignum_magnitude_to_words(x, &count);
    int64_t total = 0;
    for (int i = 0; i < count; i++) total += __builtin_popcount(words[i]);
    free(words);
    return value_int(total);
}

/* The position of the highest set bit, plus one (0 for zero itself) -- same definition as
 * Java's `BigInteger.bitLength()` for a non-negative value. Same non-negative boundary as
 * bigint_popcount, for the same reason. */
static Value native_bigint_bit_length(Value *args, int argc, int line, int col) {
    (void)argc;
    BigNum x = *args[0].as.bi;
    if (x.sign < 0) ziman_error("runtime", line, col, "bigint_bit_length: undefined for a negative bigint");
    int count;
    uint32_t *words = bignum_magnitude_to_words(x, &count);
    int hi = count - 1;
    while (hi >= 0 && words[hi] == 0) hi--; /* bignum_magnitude_to_words never leaves a nonzero top word, but stay defensive */
    if (hi < 0) {
        free(words);
        return value_int(0);
    }
    int bits_in_top = 32 - __builtin_clz(words[hi]);
    free(words);
    return value_int((int64_t)hi * 32 + bits_in_top);
}

void native_bits_register(void) {
    native_register("bits::byte_and", native_byte_and);
    native_register("bits::byte_or", native_byte_or);
    native_register("bits::byte_xor", native_byte_xor);
    native_register("bits::byte_not", native_byte_not);
    native_register("bits::byte_shift_left", native_byte_shift_left);
    native_register("bits::byte_shift_right", native_byte_shift_right);
    native_register("bits::byte_popcount", native_byte_popcount);
    native_register("bits::int_and", native_int_and);
    native_register("bits::int_or", native_int_or);
    native_register("bits::int_xor", native_int_xor);
    native_register("bits::int_not", native_int_not);
    native_register("bits::int_shift_left", native_int_shift_left);
    native_register("bits::int_shift_right", native_int_shift_right);
    native_register("bits::bigint_and", native_bigint_and);
    native_register("bits::bigint_or", native_bigint_or);
    native_register("bits::bigint_xor", native_bigint_xor);
    native_register("bits::bigint_not", native_bigint_not);
    native_register("bits::bigint_shift_left", native_bigint_shift_left);
    native_register("bits::bigint_shift_right", native_bigint_shift_right);
    native_register("bits::int_popcount", native_int_popcount);
    native_register("bits::bigint_popcount", native_bigint_popcount);
    native_register("bits::bigint_bit_length", native_bigint_bit_length);
}
