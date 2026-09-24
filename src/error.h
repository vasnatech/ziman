#ifndef ZIMAN_ERROR_H
#define ZIMAN_ERROR_H

#include <setjmp.h>

/*
 * All compile/runtime errors funnel through here. v0 just prints a
 * human-readable line to stderr and exits(1) -- structured/machine-parseable
 * diagnostics are deferred (see docs/DESIGN.md).
 */
void ziman_error(const char *phase, int line, int col, const char *fmt, ...);

/* --- reporting several errors in one run -------------------------------------
 * While a recovery point is active (the parser between declarations, the type
 * checker around each statement and function), ziman_error prints the
 * diagnostic, counts it and jumps to the innermost recovery point instead of
 * exiting, so the caller can carry on with the next declaration/statement. With
 * none active (running the program, or code that does not recover) it exits as
 * before. After `ziman_recovery_push(&jb)`, `setjmp(jb) == 0` is the normal path;
 * a non-zero return means an error was reported (the point is already popped).
 * A normal exit from the guarded code must call ziman_recovery_pop(). */
void ziman_recovery_push(jmp_buf *jb);
void ziman_recovery_pop(void);
/* Jump to the innermost recovery point without printing anything (used to skip a
 * statement that depends on one that already failed). */
void ziman_abort_quietly(void);
int ziman_error_count(void);
/* If any error was recorded: prints "ziman: N errors" (when more than one) and
 * exits 1. Call after a phase that may have recovered. */
void ziman_finish_errors(void);
/* The file diagnostics are about, for the `file` field and for text output of an
 * error in a file other than the entry file. NULL when unknown. */
void ziman_set_error_file(const char *file);
void ziman_set_entry_file(const char *file);

/* While non-NULL, every error is reported as "... at L:C (in <context>): ..."
 * -- how code the compiler generated (a #[derive] expansion) says which
 * declaration it came from, since its own positions point at the
 * declaration rather than into an invisible file. Pass NULL to clear. */
void ziman_set_error_context(const char *context);
const char *ziman_error_context(void);

/* With this on, every error is one JSON object on stderr instead of prose:
 *   {"severity":"error","phase":"type","line":8,"col":1,
 *    "context":"derive(Json) for 'Point'" | null,"message":"..."}
 * -- the same record the text form is rendered from. */
void ziman_set_json_errors(int on);

/* A lint finding: printed and counted, never fatal. Text form is
 * "file:line:col: warning: message"; with JSON errors on it is
 *   {"severity":"warning","phase":"lint","file":"...","line":L,"col":C,
 *    "context":null,"message":"..."}. */
void ziman_warning(const char *file, int line, int col, const char *fmt, ...);

#endif
