#ifndef ZIMAN_CORELIB_H
#define ZIMAN_CORELIB_H

/* Bundled core modules -- ordinary Ziman source, one file per module
 * under lib/core/ (currently collections.zm and fs.zm), embedded
 * directly into the ziman binary at build time (see the Makefile's rule
 * generating src/<name>_source.h from each lib/core/<name>.zm), so
 * `use collections`/`use fs` work from any project with no local copy
 * of the module and no install-time filesystem search path.
 *
 * Unlike the prelude (prelude.h), a core module is NOT merged into every
 * program: it's only parsed and loaded when a program actually `use`s
 * it, and a whole-module `use` stays qualified (`collections.HashMap`,
 * never a bare `HashMap`) exactly like any other module -- this is a
 * stdlib, not language-level machinery. (A bare name is still possible
 * via a per-name import, `use collections.HashMap` -- see loader.c's
 * resolve_named_imports -- but that's a separate, general mechanism,
 * not something core modules get for free.) See loader.c's load_module:
 * a project-relative file of the same name always wins if one exists,
 * so this is strictly a fallback for when the project has none.
 */

const char *corelib_source_for(const char *use_path);

#endif
