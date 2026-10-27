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
 * parse(), right before it returns -- see docs/DESIGN.md's "Attributes
 * and derive" entry for the full design. */
void generate_derived_functions(Module *module);

/* `#![repository(Entity)]`: generates the CRUD functions `module` does not write by hand, from
 * `sd` -- the entity's struct, which lives in `entity_module` and is found by the loader, because
 * at the point generation runs it is the loader that knows which files are loaded. A function the
 * module defines itself is not generated; see the comment on the implementation. */
void generate_repository_functions(Module *module, StructDecl *sd, const char *entity_module);

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
