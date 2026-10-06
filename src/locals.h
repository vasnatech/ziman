#ifndef ZIMAN_LOCALS_H
#define ZIMAN_LOCALS_H

/* `--locals`: the variables in scope, with their values, under each line of a runtime error's stack
 * trace.
 *
 * The post-mortem half of a debugger, and deliberately only that half. A stack trace already says
 * where a program died; what it does not say is what the values were, which is the question you
 * actually have -- and unlike stepping through an interactive session, this arrives on the first
 * failure, with no setup, in a place you cannot attach to afterwards such as a CI run or a crash
 * someone else reported.
 *
 * Opt-in, for two reasons that both matter. A value can be large (an array of ten thousand
 * elements) and a program's variables can hold things that should not land in a log, so making it
 * automatic would both bloat every error and leak secrets by default. And it keeps the cost exactly
 * zero when unused: error.c holds a NULL function pointer until ziman_locals_enable installs one,
 * so a build that never passes --locals runs the same trace code it did before.
 *
 * Scope: each frame shows the variables visible at the point that frame called onward, innermost
 * block first, stopping at the global scope (globals are the same for every frame and would repeat
 * on each line). A name shadowed by an inner block is shown once, as the inner binding, which is
 * the one that was live. Frames with no env at all -- a native function has none -- show nothing
 * rather than an empty heading. */

/* Installs the renderer into error.c's trace builder. Call once, before running the program. */
void ziman_locals_enable(void);

#endif
