#ifndef ZIMAN_ERROR_H
#define ZIMAN_ERROR_H

#include <setjmp.h>

/*
 * All compile/runtime errors funnel through here. v0 just prints a
 * human-readable line to stderr and exits(1) -- structured/machine-parseable
 * diagnostics are deferred (see docs/DESIGN.md).
 */
void ziman_error(const char *phase, int line, int col, const char *fmt, ...);

/* --- a runtime error that ends one thread, not the program ---------------------
 * A `spawn`ed thread arms a boundary before it runs: while one is armed on the calling thread, a *runtime* error is not printed and does not exit -- it is stored as
 * text in `message` ("runtime error at 3:5: index 1 out of bounds ...") and the thread jumps to `jb`, where the caller decides what to do (call an `on_error`
 * handler, print it, run the thread's deferred calls). The boundary is disarmed by the jump, so an error while handling it is fatal unless the handler arms
 * another. `setjmp(b.jb) == 0` is the normal path; `ziman_thread_boundary_leave()` must be called on a normal finish. Errors of other phases, and errors on a
 * thread with no boundary (the main one), behave as before. */
typedef struct ThreadBoundary {
    jmp_buf jb;
    char *message;
} ThreadBoundary;
void ziman_thread_boundary_enter(ThreadBoundary *b);
void ziman_thread_boundary_leave(void);

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
/* The file diagnostics are about, for the `file` field and, in text output, always
 * named alongside the position (this used to be omitted for the entry file; now
 * every file is named the same way, so which file an error is in never depends on
 * which one happened to be given on the command line). NULL when unknown (a
 * bundled core-library module has no real file of its own, so its errors stay
 * unnamed on purpose -- see docs/DESIGN.md). */
void ziman_set_error_file(const char *file);

/* Registers `text` as `file`'s source, so a later error at a line in `file`
 * (`ziman_flush_errors`, text mode only) can print that line with a caret under
 * the reported column. Called once per real project file as it is read
 * (`load_module_graph`/`load_module` in loader.c); a file never registered (a
 * bundled core module, whose errors carry no file at all) just prints without an
 * excerpt, unchanged. `text` is kept for the life of the process, matching every
 * other compile-time buffer here -- see ast.h's own no-free-policy note. */
void ziman_register_source(const char *file, const char *text);

/* --- a call-frame stack, for a trace on a fatal runtime error --------------------
 * call_function (interpreter.c) pushes a frame on entry -- the callee's own name and
 * source file, and the position of the call that invoked it (a position in the
 * *caller*) -- and pops it before returning, so the current thread's frames describe
 * its whole call chain, innermost last. A runtime error walks them, innermost first,
 * into the message ziman_error builds -- both the direct-print path and the one that
 * longjmps to a thread boundary, since the message is built once, at the point of the
 * error, before either happens.
 *
 * Entering a ThreadBoundary resets the count to 0: the frames of a call chain that a
 * fatal error already unwound past every pop for belong to a dynamic extent that is
 * over, not to whatever a freshly armed boundary is about to run (an `on_error`
 * handler, a deferred call) -- see ziman_thread_boundary_enter. */
void ziman_push_frame(const char *name, const char *file, int call_line, int call_col);
void ziman_pop_frame(void);

/* The current thread's call chain as text, innermost first, one indented "at f
 * (file:line:col)" per line, lines joined by "\n" with neither a leading nor a
 * trailing one -- "" when there are none. For a caller reporting a failure it already
 * has the bare message for (a spawned thread with no `on_error` handler: see
 * thread_failed in interpreter.c), printed on its own line(s) after that message.
 * Never called with an active boundary's message still in flight -- that message is
 * deliberately left bare (see ziman_error's own comment on why) since a Ziman
 * program's own `on_error` handler receives it as an ordinary string, not a report
 * meant for a terminal. Malloc'd; the caller frees it. */
char *ziman_trace_string(void);

/* Frees this thread's own frame-stack buffer, mirroring the interpreter's own
 * per-thread pools (t_defer_envs, t_env_pool) -- called when a spawned thread ends so
 * a long-running program spawning many short-lived threads doesn't accumulate one
 * abandoned uncollectable buffer per thread that ever ran. */
void ziman_call_stack_release(void);

/* The language server collects diagnostics instead of printing them: while a sink
 * is set, ziman_error hands each one to it (nothing is written) and jumps to the
 * innermost recovery point -- or, past the error limit or from
 * ziman_finish_errors, to the outermost. The caller must have a recovery point
 * active around the whole analysis. */
typedef void (*ZimanErrorSink)(const char *phase, const char *file, int line, int col, const char *context, const char *message);
void ziman_set_error_sink(ZimanErrorSink sink);
void ziman_reset_errors(void);

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
