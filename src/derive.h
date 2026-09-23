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

#endif
