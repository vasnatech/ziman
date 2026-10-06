#ifndef ZIMAN_PROFILE_H
#define ZIMAN_PROFILE_H

/* `--profile`: which of the user's own functions the program spends its time in.
 *
 * Sampling, not instrumenting. A SIGPROF timer fires on CPU time and the handler records the
 * innermost Ziman function running at that instant (error.c's frame stack, which the interpreter
 * already maintains unconditionally for error traces -- so the sampler adds no bookkeeping of its
 * own to the call path). Aggregation and sorting happen afterwards, in ordinary code.
 *
 * Instrumenting every call was the alternative and is the wrong tool here: two clock reads per call
 * is tens of nanoseconds against a call that costs a few hundred, so the functions it would report
 * as hottest are the ones it slowed down most -- a profile of the profiler. Sampling costs one
 * store per sample, a thousand times a second, and is wrong only in the harmless direction (it
 * needs enough samples to be sure about a small function, and says so in the report).
 *
 * What it reports is *self* time: each sample is attributed to the innermost frame only, so a
 * function's figure is time spent in its own statements, not in its callees. That is the figure
 * that answers "which function do I make faster", which is what a profiler is for; an inclusive
 * figure would need the handler to walk the whole stack per sample, and is deliberately left out
 * until something needs it. Native and core-library functions push frames like any other, so a
 * program whose time really goes into `mapb_set` or `read_text` is told so rather than being shown
 * only its own names. Samples taken while no Ziman function is running at all -- startup, the type
 * checker, a collection that happens to be in progress -- are counted separately rather than
 * silently blamed on whatever ran last. */

/* Installs the SIGPROF handler and starts the interval timer. Call once, immediately before running
 * the program. */
void ziman_profile_start(void);

/* Stops the timer and prints the profile to stderr, hottest first. stderr rather than stdout so a
 * profiled program's own output is untouched -- `ziman --profile prog.zm > data.txt` still yields
 * exactly what the program printed, and the report stays visible on the terminal. Call once, after the program has
 * finished. Does nothing if ziman_profile_start was never called. */
void ziman_profile_report(void);

/* Brackets a garbage collection, so samples taken inside one are reported as collection time
 * instead of being blamed on whichever function happened to allocate.
 *
 * Without this the profiler actively misleads. A 400k-insert MapBuilder loop first reported as
 * `build_map` 48.6% / `mapb_set` 51.4%, inviting the reader to go and optimise a `for` loop that
 * does an add and a call. A flat `perf` profile of the same program puts GC_mark_from at 27.8% --
 * the collector, running inside the loop because that is where the allocation was. (All `GC_*`
 * symbols together come to 47.3%, but that is not the figure to use: it includes GC_malloc_kind and
 * GC_build_fl, which are the allocator doing work on `mapb_set`'s behalf and belong to it.) With
 * the bracket the same program reports `mapb_set` 48.4%, `build_map` 29.5%, and a collection row at
 * 22.1%. What confirms the split is real rather than moved around: giving Boehm a heap large enough
 * that it never collects makes the collection row vanish while `build_map`'s absolute sample count
 * barely moves (56 against 60), so its remaining share is genuinely its own loop, argument
 * evaluation and call setup. Nesting is counted rather than flagged, so a collection triggered while
 * another is in progress still leaves the count correct on the way out.
 *
 * Both collectors need a different hook. Ziman's own mark-sweep is called from a place the
 * interpreter controls (maybe_collect), so it brackets the call directly. Boehm collects from inside
 * whatever allocation hit its threshold, which no call site can bracket, so ziman_profile_start
 * registers GC_set_on_collection_event and the GC_EVENT_START/GC_EVENT_END pair drives these same
 * two functions. Cheap enough to call unconditionally: an increment of a global, skipped entirely
 * when no profile is running. */
void ziman_profile_gc_enter(void);
void ziman_profile_gc_leave(void);

#endif
