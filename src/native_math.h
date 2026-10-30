#ifndef ZIMAN_NATIVE_MATH_H
#define ZIMAN_NATIVE_MATH_H

/* Registers the `math` native module's functions (sqrt, pow, exp, log, sin, cos, tan,
 * floor, ceil, round) with native.c's table. Called once from main() before interpret()
 * runs. See docs/syntax/'s "Numbers" section for math.zm -- bundled as a core module
 * (lib/core/math.zm, see corelib.h), so a project needs no local copy of it to `use math`. */
void native_math_register(void);

#endif
