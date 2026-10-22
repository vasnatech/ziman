#ifndef ZIMAN_NATIVE_H
#define ZIMAN_NATIVE_H

#include "value.h"

/* Every native function has this one fixed C shape, regardless of what
 * its `native fn` declaration says in Ziman -- see docs/DESIGN.md for why
 * that's deliberate. `line`/`col` are the call site, so a native function
 * can raise a ziman_error() that reads like any other runtime error. */
typedef Value (*NativeFn)(Value *args, int argc, int line, int col);

/* Registers `fn` under `qualified_name` -- the exact fully-qualified name
 * the loader produces for a native fn declaration (e.g. "fs::read_text").
 * Call once per native function, before any Ziman program runs. */
void native_register(const char *qualified_name, NativeFn fn);

/* Looks up a previously registered native function by its fully-qualified
 * name, or NULL if nothing was ever registered for it. */
NativeFn native_lookup(const char *qualified_name);

/* Calls a Ziman function value (a named function or a lambda) from native code: the
 * result of `fn(args...)`. Valid once a program is running. Implemented by the interpreter. */
Value native_call_closure(Value fn, Value *args, int argc, int line, int col);

/* Suspends Phase 3 collection on this thread for the enclosing region (docs/MEMORY.md, "one heap
 * per thread"; the counter and the reasoning are in interpreter.c beside `t_gc_unsafe_depth`).
 * Reentrant (a depth counter), per-thread, and it must be left exactly as many times as it is
 * entered.
 *
 * **For a native that holds Values no root source can see while something it calls may collect.**
 * `mapobj_entries` is the case this was exported for: it hands back a plain `malloc`'d array of
 * keys and values, which is neither an Env nor a frame -- and the custom roots are Env chains and
 * frames, not the C stack. `native_call_closure` already wraps its own call, so a native that only
 * calls back needs nothing; what it does not cover is an allocation *between* two such calls,
 * which `mapobj_set` in a filter loop is.
 *
 * **Honest about its standing: no failing case was reproducible without it.** Phase 3 is
 * mark-sweep rather than copying (MEMORY.md, 2026-09-30), so the hazard is a free and not a
 * relocation, and Boehm's own conservative stack scan plausibly keeps the source collection alive
 * through the native's `args` anyway. Tried and could not break: 20,000 entries with a predicate
 * allocating 40 strings and a builder per call, the source map an unbound temporary so no Env
 * roots it, three runs, correct answers with the guard removed. It is kept because the builtin
 * branch it replaces guarded this same loop and cites a real incident for the rule, and removing a
 * guard one cannot prove unnecessary is the riskier direction -- not because the failure was seen.
 *
 * Collection is deferred for the whole region, so wrap the smallest thing that is actually unsafe:
 * a loop over a large collection holds the whole loop's garbage. The builtin already paid that. */
void native_gc_unsafe_enter(void);
void native_gc_unsafe_leave(void);

#endif
