#ifndef ZIMAN_NATIVE_RANDOM_H
#define ZIMAN_NATIVE_RANDOM_H

/* Registers the `random` native module's function (random_bytes) with
 * native.c's table. Called once from main() before interpret() runs.
 * See docs/SYNTAX.md's "Modules" section for random.zm -- bundled as a
 * core module (lib/core/random.zm, see corelib.h), so a project needs no
 * local copy of it to `use random`. */
void native_random_register(void);

#endif
