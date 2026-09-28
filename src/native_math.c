#define _POSIX_C_SOURCE 200809L
#include "native_math.h"

#include <math.h>

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

static Value native_math_floor(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(floor(args[0].as.f));
}

static Value native_math_ceil(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(ceil(args[0].as.f));
}

static Value native_math_round(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_float(round(args[0].as.f));
}

void native_math_register(void) {
    native_register("math::sqrt", native_math_sqrt);
    native_register("math::pow", native_math_pow);
    native_register("math::exp", native_math_exp);
    native_register("math::log", native_math_log);
    native_register("math::sin", native_math_sin);
    native_register("math::cos", native_math_cos);
    native_register("math::tan", native_math_tan);
    native_register("math::floor", native_math_floor);
    native_register("math::ceil", native_math_ceil);
    native_register("math::round", native_math_round);
}
