#ifndef ZIMAN_PRELUDE_H
#define ZIMAN_PRELUDE_H

/* Ziman's prelude: everything declared in lib/prelude.zm -- native
 * functions, but also ordinary Ziman types/functions/vals -- is always
 * available, bare and unqualified, in every program, no `use` needed,
 * unlike every other module (`io`, which needs `use io` and
 * `io.read_file(...)`; even a bundled core module like `collections`
 * still needs `use collections`). This is a deliberate, narrow exception
 * to "every cross-module reference is qualified": the native functions
 * here were already unqualified core builtins before they moved into
 * this file (parse_int, floor/ceil/round, concat, substring, split,
 * trim, to_upper, to_lower, plus a few added alongside bigint/decimal
 * that never had a hardcoded home to begin with) -- moving them here,
 * instead of leaving them hardcoded in typecheck.c/interpreter.c
 * forever, is what actually lets that dispatch machinery stop growing
 * every time a new scalar type shows up. `Option<T>` lives here for the
 * same "always needed, never worth a `use`" reason, despite being
 * ordinary Ziman source (an enum + one helper function) rather than
 * native code -- see lib/prelude.zm's own header comment for why
 * this is the one file in corelib that's never `use`-able, and
 * docs/DESIGN.md's "Prelude" entry for the fuller reasoning.
 *
 * Mechanically, this is not a new loading concept -- the prelude's
 * source (prelude_source()) is parsed exactly like any other module and
 * its declarations (functions, structs, enums, global vals) are merged
 * directly into every program's flat Module, the same way the entry
 * module's own declarations are (see loader.c's load_module_graph) --
 * just always, and never self-qualified (so `parse_int`, not
 * `prelude::parse_int`), since nothing ever addresses them through a
 * qualifier. Everything downstream (type-checking a call, calling a
 * native function at runtime, resolving a bare enum reference) already
 * works generically regardless of which module a declaration came from
 * -- see native.h -- so no new machinery was needed there either. */

const char *prelude_source(void);

/* Registers every prelude function's C implementation under its bare
 * name (native_register, from native.h) -- call once at startup, before
 * any Ziman program runs, same as native_io_register(). */
void prelude_register(void);

#endif
