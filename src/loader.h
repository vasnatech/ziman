#ifndef ZIMAN_LOADER_H
#define ZIMAN_LOADER_H

#include "ast.h"

/* Resolves `entry_path`'s transitive `use` graph, loading and parsing each
 * module at most once (deduped by its `use` path), detecting cycles, and
 * returns one flat, fully-qualified Module -- every cross-module reference
 * already rewritten to "<module_path>::<name>" and checked against `pub`
 * -- ready to hand to typecheck()/interpret() exactly like a single-file
 * Module always has been. Exits the process with a clear error (unresolved
 * module, missing file, circular use, private access) on failure.
 *
 * `project_root_override`, NULL for every ordinary caller (both of main.c's),
 * is the base every `use` path in the graph resolves against; NULL means
 * `entry_path`'s own directory, correct whenever `entry_path` is genuinely
 * where the project's `use` paths are written relative to. The one caller
 * that needs otherwise is lsp.c's own fallback: a file the project's real
 * entry doesn't reach (opened on its own, to still get *something* analyzed)
 * is rarely itself at the project root -- resolving its `use`s against its
 * own directory instead of the real one breaks every path written relative
 * to the latter, exactly the way a project's real entry never would. */
Module *load_module_graph(const char *entry_path, const char *project_root_override);

/* Text to read in place of the file at each path (unsaved editor buffers). The
 * arrays are borrowed, not copied. */
void loader_set_overrides(const char **paths, const char **texts, int count);

#endif
