#ifndef ZIMAN_NATIVE_CLOCK_H
#define ZIMAN_NATIVE_CLOCK_H

/* Registers the `clock` native module's function (now_raw) with
 * native.c's table. Called once from main() before interpret() runs.
 * Bundled as a core module (lib/core/clock.zm, see corelib.h), so a
 * project needs no local copy of it to `use clock`. */
void native_clock_register(void);

#endif
