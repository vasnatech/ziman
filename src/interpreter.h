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

/* Forces a real Phase 3 collection on the calling thread's own GcHeap right now, ignoring the
 * usual growth threshold -- for `os.collect_garbage()` (native_os.c), so that an explicit request
 * to collect garbage also cleans up this thread's own gc_heap-allocated content, not just Boehm's.
 * Skipped (a no-op) if the calling thread currently has a temporary buffer in progress (see
 * interpreter.c's own gc_unsafe_enter/leave) -- an explicit request is not an exception to the
 * same safety rule the automatic trigger already follows; `os.collect_garbage()` itself is never
 * called from within such a region (it is always its own complete call, by the time it runs), but
 * a future caller of this function directly might not be, so the check stays here rather than
 * being assumed away. */
void ziman_force_collect(void);

#endif
