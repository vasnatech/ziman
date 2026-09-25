#ifndef ZIMAN_LOADER_H
#define ZIMAN_LOADER_H

#include "ast.h"

/* Resolves `entry_path`'s transitive `use` graph, loading and parsing each
 * module at most once (deduped by its `use` path), detecting cycles, and
 * returns one flat, fully-qualified Module -- every cross-module reference
 * already rewritten to "<module_path>::<name>" and checked against `pub`
 * -- ready to hand to typecheck()/interpret() exactly like a single-file
 * Module always has been. Exits the process with a clear error (unresolved
 * module, missing file, circular use, private access) on failure. */
Module *load_module_graph(const char *entry_path);

/* Text to read in place of the file at each path (unsaved editor buffers). The
 * arrays are borrowed, not copied. */
void loader_set_overrides(const char **paths, const char **texts, int count);

#endif
