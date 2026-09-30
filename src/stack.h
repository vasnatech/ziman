#ifndef ZIMAN_STACK_H
#define ZIMAN_STACK_H

#include <stddef.h>

/* Stack depth. The interpreter (and the parser and type checker before it) recurse in C, so deep recursion in a Ziman program used to run off the end of the C
 * stack and crash with no message. Every thread now knows where its stack ends and each recursive step checks how much is left; when it is nearly used up the
 * program stops with a normal runtime error. The limit is therefore bytes of stack, not a count of calls (a Ziman call costs 3.5-11 KB of C stack depending on
 * the shape of the code), and it is computed at run time: from the stack the thread really has (read once, when the thread starts), and the sizes of the
 * stacks are chosen at startup from the memory the process may use, the number of threads it expects and the options below.
 *
 *   --stack-mb=N / ZIMAN_STACK_MB          the main interpreter thread's stack, in MB
 *   --thread-stack-mb=N / ZIMAN_THREAD_STACK_MB   the stack of each `spawn`ed thread, in MB
 *   --threads=N / ZIMAN_THREADS            how many threads to plan for (default 64); the memory set aside for stacks is shared between them
 *
 * A flag wins over its environment variable, which wins over the computed size. */

/* The lowest stack address the current thread may use before the limit trips (its stack's low end plus a margin); NULL until the thread has been measured. */
extern __thread char *ziman_stack_limit;

/* Slow path of the check: measures the thread the first time, and raises the "stack depth exceeded" runtime error when the stack is nearly used up. */
void ziman_stack_slow(void);

/* One compare on the fast path: the address of a local is (near enough) the stack pointer. */
#define ZIMAN_STACK_CHECK()                                                                            \
    do {                                                                                               \
        char ziman_probe_;                                                                             \
        if (__builtin_expect(ziman_stack_limit == NULL || &ziman_probe_ < ziman_stack_limit, 0)) ziman_stack_slow(); \
    } while (0)

/* Options, before anything runs. `main_mb`, `thread_mb` and `threads` are 0 when not given (the environment, then the computed value, apply). */
void ziman_stack_configure(long main_mb, long thread_mb, long threads);

/* The stack sizes to create threads with, in bytes: computed from the memory available (refreshed at most once a second for `spawn`, since memory changes but a
 * thread's stack cannot once it exists) unless an override is set. */
size_t ziman_main_stack_bytes(void);
size_t ziman_spawn_stack_bytes(void);

/* Marks the calling thread as the interpreter's main thread (for the error message); every other thread measured is a spawned one. */
void ziman_stack_mark_main(void);

/* Whether the calling thread was marked with ziman_stack_mark_main. */
int ziman_stack_is_main(void);

/* Bytes of stack the calling thread has left before the "stack depth exceeded" error. */
size_t ziman_stack_remaining(void);

/* The calling thread's own real stack region -- `*out_low` the lowest address it may use,
 * `*out_high` one past the highest (stacks grow down on every platform this runs on, so a scan
 * looking for live roots runs from near the current stack pointer up toward `*out_high`, never
 * down toward `*out_low`, since anything below the current stack pointer is already-returned,
 * stale space from calls that are no longer active). For Phase 3's conservative root-finding
 * (docs/MEMORY.md, src/gc_scan.c) -- deliberately not `ziman_stack_limit`, which is the recursion-
 * depth guard's own margin-adjusted limit, not the stack's real bounds. Measures the thread on
 * first call, same as the recursion guard does; returns 0 (leaving both untouched) if the thread
 * cannot be measured (see measure_this_thread's own comment on why that is possible at all), 1 on
 * success. */
int ziman_stack_bounds(char **out_low, char **out_high);

/* Runs `fn(arg)` on a new thread with a stack of `ziman_main_stack_bytes()` (halving the size until the system agrees, and running it on the calling thread
 * if no size works) and waits for it; returns fn's result. */
void *ziman_run_on_big_stack(void *(*fn)(void *), void *arg);

/* Creates a detached thread for `spawn` with `ziman_spawn_stack_bytes()`; 0 on success, an errno value if no thread could be made. */
int ziman_spawn_thread(void *(*fn)(void *), void *arg);

#endif
