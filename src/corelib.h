#ifndef ZIMAN_CORELIB_H
#define ZIMAN_CORELIB_H

/* Bundled core modules -- ordinary Ziman source, one file per module
 * under lib/core/ (fs.zm, json.zm and the rest), embedded
 * directly into the ziman binary at build time (see the Makefile's rule
 * generating src/<name>_source.h from each lib/core/<name>.zm), so
 * `use collections`/`use fs` work from any project with no local copy
 * of the module and no install-time filesystem search path.
 *
 * Unlike the prelude (prelude.h), a core module is NOT merged into every
 * program: it's only parsed and loaded when a program actually `use`s
 * it, and a whole-module `use` stays qualified (`fs.read_text`,
 * never a bare `HashMap`) exactly like any other module -- this is a
 * stdlib, not language-level machinery. (A bare name is still possible
 * via a per-name import, `use fs.read_text` -- see loader.c's
 * resolve_named_imports -- but that's a separate, general mechanism,
 * not something core modules get for free.) See loader.c's load_module:
 * a project-relative file of the same name always wins if one exists,
 * so this is strictly a fallback for when the project has none.
 */

const char *corelib_source_for(const char *use_path);

/* **A bundled module may be several files**, which is what a module too big to read in one sitting
 * does: `lib/http/server/` is ten files that are one `server`. The parts share one namespace and
 * are merged by `load_module`, each parsed on its own so its line numbers and its file name are
 * its own -- the same treatment the prelude's ten parts get, generalised out of prelude.c.
 *
 * `corelib_source_for` answers NULL for such a module, so a caller that reads one source is forced
 * to notice rather than silently loading a tenth of it. Zero parts means an ordinary one-file
 * module.
 *
 * **A part may not `use`**: the module's imports live in its first file, for the reason a prelude
 * part may not either -- an import is a property of the module, and merging N import lists raises
 * which-part-owns-it questions that one list does not have. */
/* Every part of `use_path` joined, for a caller that wants the module's text as a whole rather
 * than to parse it: `--api` lists signatures, and the language server caches a file to open. For
 * a one-file module it is that file. **Not for parsing** -- `load_module` reads the parts one by
 * one so that each keeps its own line numbers, which a join destroys. */
char *corelib_whole_source_for(const char *use_path);

/* The file a one-file bundled module was embedded from, for a diagnostic inside it. NULL for a
 * multi-file one, whose parts each have their own (`corelib_part_name`). */
const char *corelib_path_for(const char *use_path);

int corelib_part_count(const char *use_path);
const char *corelib_part_name(const char *use_path, int i);
const char *corelib_part_source(const char *use_path, int i);

/* The bundled modules, for `ziman --api` with no module named: a reader (or a model) with no
 * documentation open has otherwise no way to ask what exists, which the 2026-10-09 benchmark
 * measured as the single largest consumer of its fix budget. */
int corelib_module_count(void);
const char *corelib_module_name(int i);

#endif
