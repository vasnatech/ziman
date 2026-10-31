#ifndef ZIMAN_DERIVE_H
#define ZIMAN_DERIVE_H

#include "ast.h"

/* Scans `module`'s own structs/enums for a `#[derive(...)]` attribute and,
 * for each recognized derive target, synthesizes the corresponding
 * function(s) as real Ziman source text, lexes+parses that text with the
 * same pipeline this file itself came from, and appends the resulting
 * FunctionDecls to `module->functions` -- so everything downstream (the
 * loader's self-qualify/cross-reference passes, typecheck, the
 * interpreter) sees a derived function exactly like a hand-written one,
 * with zero special-casing needed anywhere else. Called once, from
 * parse(), right before it returns -- see docs/design/journal/'s "Attributes
 * and derive" entry for the full design. */
void generate_derived_functions(Module *module);

/* `#![repository(Entity)]`: generates the CRUD functions `module` does not write by hand, from
 * `sd` -- the entity's struct, which lives in `entity_module` and is found by the loader, because
 * at the point generation runs it is the loader that knows which files are loaded. A function the
 * module defines itself is not generated; see the comment on the implementation. */
void generate_repository_functions(Module *module, StructDecl *sd, const char *entity_module);

/* Runs a project's own module generator for `#![derive(X)]`, X not Repository. Called from the
 * loader after imports resolve, which is what makes X resolvable. */
void generate_project_module(Module *module);

/* `#[repository]` on a bodyless `pub fn`: fills in the body of every **derived query** in
 * `module` from `sd`, the `#![repository(Entity)]` entity's struct -- the function's own name is
 * its predicate (`find_by_id_and_patient_id`) and its return type is its cardinality. See
 * docs/design/derived-queries.md, and `generate_one_derived_query` for the refusals.
 *
 * Called right after `generate_repository_functions`, over the same struct, because the imports
 * and the `<entity>_meta` val it depends on are that one's doing. `sd` is NULL when the module
 * has no `#![repository(..)]` at all, which is exactly what a `#[repository]` function there is
 * refused for. */
void generate_derived_queries(Module *module, StructDecl *sd);

/* "LocalDate" -> "local_date": the naming rule a derive's generated
 * functions use, and that `Ord` dispatch uses to find a type's
 * `<snake>_compare`. The caller owns the result. */
char *ziman_pascal_to_snake(const char *name);

/* "path::LocalDate" -> "path::local_date_compare", "Patient" -> "patient_compare":
 * where a struct/enum's `Ord` function lives. The caller owns the result. */
char *ziman_compare_function_name(const char *type_name);
/* The same with the type name lowercased and no underscores inserted
 * ("LocalDate" -> "localdate_compare"): the spelling the bundled `time` types
 * already use. A type's `Ord` function may be spelled either way. */
char *ziman_compare_function_name_flat(const char *type_name);

/* "NetError" -> "net_error_message": where the `error_message` builtin dispatches. The caller
 * owns the result. */
char *ziman_message_function_name(const char *type_name);

/* When non-zero, every generated snippet is also printed to stdout, headed
 * by a `// derive(...) for '...'` comment (ziman --emit-derived). */
extern int g_emit_derived;

#endif

/* Checks every field attribute in the module against what the loaded generators declare. Called
 * from `generate_derived_functions`; see its comment for why this is not the parser's job. */
void ziman_check_field_attributes(Module *module);
