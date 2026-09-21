#ifndef ZIMAN_NATIVE_H
#define ZIMAN_NATIVE_H

#include "value.h"

/* Every native function has this one fixed C shape, regardless of what
 * its `native fn` declaration says in Ziman -- see docs/DESIGN.md for why
 * that's deliberate. `line`/`col` are the call site, so a native function
 * can raise a ziman_error() that reads like any other runtime error. */
typedef Value (*NativeFn)(Value *args, int argc, int line, int col);

/* Registers `fn` under `qualified_name` -- the exact fully-qualified name
 * the loader produces for a native fn declaration (e.g. "io::read_file").
 * Call once per native function, before any Ziman program runs. */
void native_register(const char *qualified_name, NativeFn fn);

/* Looks up a previously registered native function by its fully-qualified
 * name, or NULL if nothing was ever registered for it. */
NativeFn native_lookup(const char *qualified_name);

#endif
