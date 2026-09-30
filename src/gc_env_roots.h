#ifndef ZIMAN_GC_ENV_ROOTS_H
#define ZIMAN_GC_ENV_ROOTS_H

#include "arena.h"
#include "env.h"

/* A second, necessary root source alongside `gc_scan_stack_roots` -- found while working out what a
 * complete root set for a real collection cycle actually requires (docs/MEMORY.md), not anticipated
 * in the shape stated when `gc_heap_collect` was first built.
 *
 * An `Env` (env.h) is not itself allocated in any thread's `GcHeap` arena -- `env_new`/
 * `env_pool_acquire` use the ordinary allocator (`GC_MALLOC`/`malloc`), a completely separate heap.
 * So the moment a `Value` is stored into a binding (`env_define`), its own pointer fields move off
 * the C stack (where `gc_scan_stack_roots` could see them as raw words) into `Env.items`' own
 * heap-allocated storage -- memory the conservative stack scan never looks at, since it only reads
 * stack bytes, never dereferences a pointer it finds. Every ordinary variable in the interpreter is
 * affected, not just `interpreter.c`'s `t_defer_envs`/`t_env_pool`, which is where this gap was
 * originally (and too narrowly) expected to live.
 *
 * `gc_mark_env_chain` marks every `Value` bound in `env` and every scope it can reach through
 * `->parent`, up to and including the root it eventually terminates at (`g_global_env`, or `NULL`
 * for a chain with no parent at all). `caps`/`defers` are not walked: `caps` is the type checker's
 * own field, always empty for an interpreter-built `Env` (see env.h's own doc comment), and
 * `defers` holds `Expr*` (permanent AST, evaluated later against this same `env`'s own bindings
 * when it runs -- see interpreter.c's `run_defers`), never a `Value` of its own to mark. */
void gc_mark_env_chain(const Arena *a, const Env *env);

#endif
