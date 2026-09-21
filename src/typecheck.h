#ifndef ZIMAN_TYPECHECK_H
#define ZIMAN_TYPECHECK_H

#include "ast.h"

/* Walks the whole module once, filling in Expr.resolved_type on every
 * node and exiting the process with a type error (line:col + message) on
 * the first mismatch found. Also enforces the "every non-void function
 * returns on all statically-reachable paths" rule. */
void typecheck(Module *module);

#endif
