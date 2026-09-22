#ifndef ZIMAN_CORELIB_H
#define ZIMAN_CORELIB_H

/* Bundled core modules -- ordinary Ziman source, one file per module
 * under corelib/, embedded directly into the ziman binary at build time
 * (see the Makefile's rule generating src/collections_source.h from
 * corelib/collections.zm), so `use collections` works from any project
 * with no local copy of the module and no install-time filesystem
 * search path.
 *
 * Unlike the prelude (prelude.h), a core module is NOT merged into every
 * program: it's only parsed and loaded when a program actually `use`s
 * it, and it stays qualified (`collections.HashMap`, never a bare
 * `HashMap`) exactly like any other module -- this is a stdlib, not
 * language-level machinery. See loader.c's load_module: a project-
 * relative file of the same name always wins if one exists, so this is
 * strictly a fallback for when the project has none.
 */

const char *corelib_source_for(const char *use_path);

#endif
