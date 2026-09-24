#ifndef ZIMAN_ERROR_H
#define ZIMAN_ERROR_H

/*
 * All compile/runtime errors funnel through here. v0 just prints a
 * human-readable line to stderr and exits(1) -- structured/machine-parseable
 * diagnostics are deferred (see docs/DESIGN.md).
 */
void ziman_error(const char *phase, int line, int col, const char *fmt, ...);

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
