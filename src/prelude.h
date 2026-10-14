#ifndef ZIMAN_PRELUDE_H
#define ZIMAN_PRELUDE_H

#include "value.h"

/* Ziman's prelude: everything declared in lib/prelude.zm -- native
 * functions, but also ordinary Ziman types/functions/vals -- is always
 * available, bare and unqualified, in every program, no `use` needed,
 * unlike every other module (`fs`, which needs `use fs` and
 * `fs.read_text(...)`; even a bundled core module like `collections`
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

/* An ArrayBuilder's length, and one of its elements -- what `for x in builder` needs from the interpreter
 * (see exec_stmt's STMT_FOR). Both take the builder's own thread-ownership check, as every other call on one
 * does, so a builder read from the wrong thread fails there rather than here. */
int ziman_array_builder_length(Value ab);
Value ziman_array_builder_at(Value ab, int i);

/* The length of any of the four builders, for the bare `len` builtin: an ArrayBuilder's or
 * StringBuilder's element/byte count, a MapBuilder's entries or a SetBuilder's elements. Only
 * called once the typechecker has established the argument is one of them. */
int ziman_builder_length(Value v);

/* Whether a builder holds `needle`: a substring of a StringBuilder, a key of a MapBuilder, an
 * element of a SetBuilder, or -- the one O(n) case -- an element of an ArrayBuilder. Only called
 * once the typechecker has established the argument types. */
int ziman_builder_contains(Value v, Value needle);

/* A builder's contents copied out as the immutable value it corresponds to: an `[T]`, a
 * `Map<K, V>`, a `Set<T>` or a `string`. The caller sets the result's type from the call's
 * resolved type, as the per-type natives behind it already require. */
Value ziman_builder_snapshot(Value v);

/* Empties a builder in place, leaving it usable: the buffer kinds reset their length, the two trie
 * kinds take a fresh empty trie so that a snapshot taken earlier is undisturbed. */
void ziman_builder_clear(Value v);

#endif
