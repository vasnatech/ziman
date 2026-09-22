#ifndef ZIMAN_PRELUDE_H
#define ZIMAN_PRELUDE_H

/* Ziman's prelude: a small, fixed set of native functions that are
 * always available, bare and unqualified, in every program -- no `use`
 * needed, unlike every other native module (`io`, which needs `use io`
 * and `io.read_file(...)`). This is a deliberate, narrow exception to
 * "every cross-module reference is qualified": these functions were
 * already unqualified core builtins before they moved here (parse_int,
 * floor/ceil/round, concat, substring, split, trim, to_upper, to_lower),
 * plus a few new ones added alongside bigint/decimal that never had a
 * hardcoded home to begin with (parse_bigint, parse_decimal,
 * decimal_round) -- moving them here, instead of leaving them hardcoded
 * in typecheck.c/interpreter.c forever, is what actually lets that
 * dispatch machinery stop growing every time a new scalar type shows up,
 * without changing a single existing program's source.
 *
 * Mechanically, this is not a new loading concept -- the prelude's
 * source (prelude_source()) is parsed exactly like any other module and
 * its function declarations are merged directly into every program's
 * flat Module, the same way the entry module's own functions are (see
 * loader.c's load_module_graph) -- just always, and never self-qualified
 * (so `parse_int`, not `prelude::parse_int`), since nothing ever
 * addresses them through a qualifier. Everything downstream (type-
 * checking a call, calling a native function at runtime) already works
 * generically for any native fn -- see native.h -- so no new machinery
 * was needed there either. */

const char *prelude_source(void);

/* Registers every prelude function's C implementation under its bare
 * name (native_register, from native.h) -- call once at startup, before
 * any Ziman program runs, same as native_io_register(). */
void prelude_register(void);

#endif
