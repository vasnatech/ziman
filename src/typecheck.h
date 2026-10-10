#ifndef ZIMAN_TYPECHECK_H
#define ZIMAN_TYPECHECK_H

#include "ast.h"

/* Walks the whole module once, filling in Expr.resolved_type on every
 * node and exiting the process with a type error (line:col + message) on
 * the first mismatch found. Also enforces the "every non-void function
 * returns on all statically-reachable paths" rule. */
void typecheck(Module *module);

/* Levenshtein edit distance, shared so every "did you mean" in the compiler ranks candidates the
 * same way (see typecheck.c's own comment for why the cutoff is what it is). */
int ziman_name_distance(const char *a, const char *b);

/* The language server checks any file, not just a program: no `main` is required. */
void typecheck_skip_main_check(int skip);

/* `--lint`: after this, typecheck() also reports style findings in the user's
 * own (non-bundled, non-derived) code through ziman_warning(), and counts them. */
void typecheck_enable_lint(void);
int typecheck_lint_count(void);
int typecheck_lint_enabled(void);
void typecheck_lint_note(void);

#endif
