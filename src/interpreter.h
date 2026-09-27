#ifndef ZIMAN_INTERPRETER_H
#define ZIMAN_INTERPRETER_H

#include "ast.h"

/* Runs `main()`. Assumes `module` already passed typecheck(). Exits the
 * process with a runtime error (line:col + message) on things the type
 * checker can't catch statically (division by zero, etc). */
void interpret(Module *module);

/* Runs every `#[test]` function in `module` (see ast.h's
 * FunctionDecl.is_test/test_cases); see interpreter.c for the full
 * contract. Assumes `module` already passed typecheck(). Adds to
 * out_run and out_failed rather than resetting them, so `ziman test`
 * can tally one summary across several files. */
void interpret_tests(Module *module, const char *file_label, int *out_run, int *out_failed);

#endif
