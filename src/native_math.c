#define _POSIX_C_SOURCE 200809L
#include "native_math.h"

#include <math.h>

#include "bignum.h"
#include "decimal.h"
#include "error.h"
#include "native.h"
#include "value.h"

/* The `math` core module's natives: one-line wrappers over libm (already linked, see the
 * Makefile). Domain errors (sqrt of a negative number, log of zero or a negative number)
 * return NaN, the same as the C, Java and JavaScript standard libraries do, and the same
 * value `parse_float("nan")` already produces -- `compare<T: Ord>` already gives NaN a total
 * order (see "Ordering" in docs/syntax/), so it is an ordinary, well-supported float value
 * here, not a special case that needs a Result instead. See lib/core/math.zm. */

/* `abs`, `min` and `max`, which were the prelude's until 2026-10-22 and are the `math` module's
 * now. They were bare vocabulary -- `abs(x)`, `min(a, b)`, no `use` -- and moving them cost 76
 * call sites across four trees, so the reason is worth stating: the prelude is the names a program
 * gets without asking, and these three are arithmetic, which is what this module is. `math.floor`
 * and `math.round` were already here and no more specialised than `math.abs`.
 *
 * Registered as `math::abs` and friends, the qualified spelling every module native uses. One
 * symbol serves `abs`'s four declarations, as it did under the bare name: a scalar parameter gives
 * the symbol fallback no world prefix to build a per-type symbol from, and there would be nothing
 * to gain if it did. */
static Value native_math_abs(Value *args, int argc, int line, int col) {
    (void)argc;
    Value x = args[0];
    if (x.type->kind == TY_INT) {
        /* -9223372036854775808 has no positive counterpart in 64 bits, and this language reports
         * integer overflow rather than wrapping. `bigint` is the type with no such edge. */
        if (x.as.i == INT64_MIN) {
            ziman_error("runtime", line, col,
                        "integer overflow in 'abs' (int is 64-bit; use bigint for larger values)");
        }
        return value_int(x.as.i < 0 ? -x.as.i : x.as.i);
    }
    if (x.type->kind == TY_BIGINT) return value_bigint(bignum_abs(*x.as.bi));
    if (x.type->kind == TY_DECIMAL) return value_decimal(decimal_abs(*x.as.dec));
    return value_float(x.as.f < 0 ? -x.as.f : x.as.f);
}

/* `min`/`max`: variadic and `Ord`-bounded, not just the four numeric scalars these were once
 * limited to -- see docs/design/journal/'s "Variadic, Ord-bounded min/max" entry. One linear scan
 * calling `compare` per candidate, which for int/float/bigint/decimal/byte/string is already a
 * direct C comparison, so this needs no separate fast path for them the way `sort` gives a packed
 * array its own routine: a single comparison per element is not where sorting's O(n log n) cost
 * lives. A struct's or enum's own `<type>_compare` is reached exactly as `compare` and `sort`
 * reach it, through native_value_compare.
 *
 * **`args[0]` is one array whichever way the call was written**, because `collapse_variadic_args`
 * runs before a native is entered -- so the "two or more values, or one array as the whole list"
 * convention is handled before this sees anything, where the dispatched builtin this replaced had
 * to test `argc == 1 && args[0]` an array itself. That test was also the one piece of the old arm
 * that could not be right for every `T`: `Ord` admits `[int]`, so `min` of two arrays and `min` of
 * one array to spread looked identical to it. The collapse distinguishes them from the
 * declaration, which is the thing a declaration knows and an arm does not. */
static Value native_math_min_max(Value *args, int want_min, int line, int col) {
    const ArrayObj *values = args[0].as.arr;
    const char *name = want_min ? "min" : "max";
    if (values->count == 0) ziman_error("runtime", line, col, "'%s' of an empty array", name);
    Value best = array_get(values, 0);
    for (int i = 1; i < values->count; i++) {
        Value item = array_get(values, i);
        int c = native_value_compare(item, best, line, col);
        if (want_min ? c < 0 : c > 0) best = item;
    }
    return best;
}

static Value native_math_min(Value *args, int argc, int line, int col) {
    (void)argc;
    return native_math_min_max(args, 1, line, col);
}

static Value native_math_max(Value *args, int argc, int line, int col) {
    (void)argc;
    return native_math_min_max(args, 0, line, col);
}

static Value native_math_sqrt(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(sqrt(args[0].as.f));
}

static Value native_math_pow(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(pow(args[0].as.f, args[1].as.f));
}

static Value native_math_exp(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(exp(args[0].as.f));
}

static Value native_math_log(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(log(args[0].as.f));
}

/* Base 2 and base 10 logarithms, which are *not* `log(x) / log(2.0)`.
 *
 * That form is what a caller writes without these, and it is measurably less accurate: libm's
 * `log2`/`log10` are exact for an exact power of their base, where the division is not --
 * `log(1000.0) / log(10.0)` is 2.9999999999999996 on this machine, so `floor` of it is 2 and a
 * digit-counting loop built on it is off by one. Domain behaviour matches `log`: a negative is
 * NaN and zero is -infinity, reported as values rather than errors (see the module header). */
static Value native_math_log2(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(log2(args[0].as.f));
}

static Value native_math_log10(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(log10(args[0].as.f));
}

/* -1, 0 or 1, for all four numeric types -- one symbol serving four declarations by inspecting the
 * value, exactly as `abs` does, so it needs no per-type symbol from the fallback.
 *
 * **NaN is a runtime error rather than 0.** The answer has to be an `int`, and every candidate is
 * a lie: 0 would make `sign(nan) == sign(0.0)`, and -1 or 1 would claim an ordering NaN does not
 * have. `is_nan(x)` is the question to ask first, and the message says so. Both infinities have a
 * perfectly good sign and get it. */
static Value native_math_sign(Value *args, int argc, int line, int col) {
    (void)argc;
    Value x = args[0];
    if (x.type->kind == TY_INT) return value_int(x.as.i < 0 ? -1 : (x.as.i > 0 ? 1 : 0));
    if (x.type->kind == TY_BIGINT) return value_int(x.as.bi->sign);
    if (x.type->kind == TY_DECIMAL) return value_int(x.as.dec->unscaled.sign);
    if (x.as.f != x.as.f) {
        ziman_error("runtime", line, col,
                    "sign: NaN has no sign (it is not less than, equal to or greater than zero) -- "
                    "test with 'math.is_nan(x)' first");
    }
    return value_int(x.as.f < 0.0 ? -1 : (x.as.f > 0.0 ? 1 : 0));
}

static Value native_math_sin(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(sin(args[0].as.f));
}

static Value native_math_cos(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(cos(args[0].as.f));
}

static Value native_math_tan(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(tan(args[0].as.f));
}

/* The inverse trigonometric three. `atan2` takes y before x, as C, Java and every other standard
 * library does -- the order looks wrong and is the one everybody copies, so it is not changed here.
 * `asin`/`acos` outside -1..1 return NaN, which is this file's existing convention for a domain
 * error (see its header comment); nothing traps. */
static Value native_math_asin(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(asin(args[0].as.f));
}

static Value native_math_acos(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(acos(args[0].as.f));
}

static Value native_math_atan(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(atan(args[0].as.f));
}

static Value native_math_atan2(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(atan2(args[0].as.f, args[1].as.f));
}

static Value native_math_floor_float(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(floor(args[0].as.f));
}

static Value native_math_ceil_float(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(ceil(args[0].as.f));
}

static Value native_math_round_float(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(round(args[0].as.f));
}

/* The int-returning roundings and `decimal`'s directional roundings and integer power. They lived in
 * the prelude (bare, no `use`) before this module; see docs/design/journal/. */
static Value native_math_floor(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int((int64_t)floor(args[0].as.f));
}

static Value native_math_ceil(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int((int64_t)ceil(args[0].as.f));
}

static Value native_math_round(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_int((int64_t)round(args[0].as.f));
}

/* ---- the decimal roundings, two arities each -------------------------------------------------
 *
 * One symbol per operation serves both `floor(d)` and `floor(d, scale)`, because the declaration-
 * to-symbol fallback reads parameter one's type and both members are a `decimal` -- the same
 * single-registration shape `to_upper`'s two arities use (src/prelude.c).
 *
 * **The one-argument form answers a `bigint`, not an `int`.** A `decimal` is arbitrary-precision,
 * so its whole part can exceed 2^63 and an `int` would have to either overflow or lie; the `float`
 * members answer an `int` because a `float` cannot hold a whole number that large to begin with.
 * It is computed as the scale-0 rounding followed by an exact conversion: at scale 0 there is
 * nothing left for `decimal_to_bignum_trunc` to truncate, so the floor/ceil/half-away choice is
 * made by the decimal rounding and not quietly re-made by the conversion. */
static Value native_math_decimal_round(Value *args, int argc, int line, int col) {
    (void)line; (void)col;
    if (argc == 1) return value_bigint(decimal_to_bignum_trunc(decimal_round(*args[0].as.dec, 0)));
    return value_decimal(decimal_round(*args[0].as.dec, (int)args[1].as.i));
}

static Value native_math_decimal_floor(Value *args, int argc, int line, int col) {
    (void)line; (void)col;
    if (argc == 1) return value_bigint(decimal_to_bignum_trunc(decimal_floor(*args[0].as.dec, 0)));
    return value_decimal(decimal_floor(*args[0].as.dec, (int)args[1].as.i));
}

static Value native_math_decimal_ceil(Value *args, int argc, int line, int col) {
    (void)line; (void)col;
    if (argc == 1) return value_bigint(decimal_to_bignum_trunc(decimal_ceil(*args[0].as.dec, 0)));
    return value_decimal(decimal_ceil(*args[0].as.dec, (int)args[1].as.i));
}

static Value native_math_decimal_pow(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t exponent = args[1].as.i;
    if (exponent < 0) {
        ziman_error("runtime", line, col,
                    "decimal_pow: exponent must be non-negative, found %lld (for a negative one, divide "
                    "a decimal of your target scale by decimal_pow(base, -exponent) instead, e.g. "
                    "1.0000d / math.decimal_pow(base, -exponent))",
                    (long long)exponent);
    }
    return value_decimal(decimal_pow(*args[0].as.dec, (int)exponent));
}

void native_math_register(void) {
    native_register("math::abs", native_math_abs);
    native_register("math::min", native_math_min);
    native_register("math::max", native_math_max);
    native_register("math::sqrt", native_math_sqrt);
    /* **No bare `math::pow`/`floor`/`ceil`/`round`.** Each of those names now has a `float`
     * member and a `decimal` one, and `native_lookup(fn->name)` is consulted before the
     * world-prefix fallback -- so a bare registration would be found first and run the float
     * implementation on a decimal. The same trap `char_at` and `last_index_of` fell into.
     * `abs`/`min`/`max` keep theirs: one C function serves all their members by inspecting the
     * value, so there is nothing for the fallback to tell apart. */
    native_register("math::float_pow", native_math_pow);
    native_register("math::exp", native_math_exp);
    native_register("math::log", native_math_log);
    native_register("math::log2", native_math_log2);
    native_register("math::log10", native_math_log10);
    native_register("math::sign", native_math_sign);
    native_register("math::sin", native_math_sin);
    native_register("math::cos", native_math_cos);
    native_register("math::tan", native_math_tan);
    native_register("math::asin", native_math_asin);
    native_register("math::acos", native_math_acos);
    native_register("math::atan", native_math_atan);
    native_register("math::atan2", native_math_atan2);
    native_register("math::floor_float", native_math_floor_float);
    native_register("math::ceil_float", native_math_ceil_float);
    native_register("math::round_float", native_math_round_float);
    native_register("math::float_floor", native_math_floor);
    native_register("math::float_ceil", native_math_ceil);
    native_register("math::float_round", native_math_round);
    native_register("math::decimal_round", native_math_decimal_round);
    native_register("math::decimal_floor", native_math_decimal_floor);
    native_register("math::decimal_ceil", native_math_decimal_ceil);
    native_register("math::decimal_pow", native_math_decimal_pow);
}
