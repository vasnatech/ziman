#ifndef ZIMAN_GENERATORS_H
#define ZIMAN_GENERATORS_H

#include "ast.h"

/* The bridge from `#[derive(X)]` to a generator written in Ziman.
 *
 * `lib/core/derive_*.zm` are the generators; each declares `#![generator(X)]` and exports
 * `generate(d: meta.Decl) -> Result<string, string>`. This loads them as a program of their own,
 * initialises their globals, reifies a declaration as a `meta.Decl` and calls one -- answering
 * the source text to splice, exactly as a C generator's `cb.data` is.
 *
 * **The generator program is loaded, checked and initialised before the user program's load
 * begins**, which is what makes one `g_global_env` enough: this is sequential, not re-entrant.
 * See ziman_init_module_globals (interpreter.h) and docs/design/user-generators.md §6.
 *
 * Answers NULL when no Ziman generator claims `target`, which is how the four C generators keep
 * running while the port finishes. A generator that *refuses* or *fails* raises a `ziman_error`
 * at `line`/`col` -- the declaration carrying the attribute -- rather than answering NULL. */
char *ziman_generate_in_ziman(const char *target, StructDecl *sd, EnumDecl *ed, const char *arg, int line, int col);

/* Whether any Ziman generator claims `target`, without running it: `derive.c` asks before
 * deciding which path a derive takes. */
int ziman_has_ziman_generator(const char *target);

#endif
