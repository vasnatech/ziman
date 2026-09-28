#define _POSIX_C_SOURCE 200809L
#include "native_math.h"

#include <math.h>

#include "decimal.h"
#include "error.h"
#include "native.h"
#include "value.h"

/* The `math` core module's natives: one-line wrappers over libm (already linked, see the
 * Makefile). Domain errors (sqrt of a negative number, log of zero or a negative number)
 * return NaN, the same as the C, Java and JavaScript standard libraries do, and the same
 * value `parse_float("nan")` already produces -- `compare<T: Ord>` already gives NaN a total
 * order (see "Ordering" in docs/SYNTAX.md), so it is an ordinary, well-supported float value
 * here, not a special case that needs a Result instead. See lib/core/math.zm. */

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
 * the prelude (bare, no `use`) before this module; see docs/DESIGN.md. */
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

static Value native_math_decimal_round(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_decimal(decimal_round(*args[0].as.dec, (int)args[1].as.i));
}

static Value native_math_decimal_floor(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_decimal(decimal_floor(*args[0].as.dec, (int)args[1].as.i));
}

static Value native_math_decimal_ceil(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
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
    native_register("math::sqrt", native_math_sqrt);
    native_register("math::pow", native_math_pow);
    native_register("math::exp", native_math_exp);
    native_register("math::log", native_math_log);
    native_register("math::sin", native_math_sin);
    native_register("math::cos", native_math_cos);
    native_register("math::tan", native_math_tan);
    native_register("math::floor_float", native_math_floor_float);
    native_register("math::ceil_float", native_math_ceil_float);
    native_register("math::round_float", native_math_round_float);
    native_register("math::floor", native_math_floor);
    native_register("math::ceil", native_math_ceil);
    native_register("math::round", native_math_round);
    native_register("math::decimal_round", native_math_decimal_round);
    native_register("math::decimal_floor", native_math_decimal_floor);
    native_register("math::decimal_ceil", native_math_decimal_ceil);
    native_register("math::decimal_pow", native_math_decimal_pow);
}
