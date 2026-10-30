#ifndef ZIMAN_GC_LOOKUP_H
#define ZIMAN_GC_LOOKUP_H

#include "arena.h"

/* Resolves `candidate`, an arbitrary address that might be a pointer into `a`, to the payload start
 * of the gc_alloc'd object it falls within -- or NULL if it doesn't correspond to any object `a`
 * currently holds (an ordinary integer that happens to look like an address, a stale or misaligned
 * leftover, anything simply out of range). Exists for the one gap `src/gc_copy.h`'s "never
 * deduplicated" design left open: a conservative scan of a thread's own C stack (still to be built --
 * see docs/MEMORY.md's Phase 3 design) finds bare, untyped words with no length or kind attached, so
 * unlike `gc_copy.c` (which always starts from an already-typed `Value`, `len`/`count` in hand, and
 * so never needs to ask "what is this address"), a stack scan's own candidate words need exactly
 * this question answered before anything can be traced from them.
 *
 * An EXACT match to some object's own payload start resolves to itself. An INTERIOR address --
 * strictly inside a payload's declared span, not at its start -- also resolves to that object's
 * start, needed for the one shape in this interpreter's Value representation that can expose a raw
 * interior pointer with nothing wrapping it: a string view (`Value.as.str.ptr`, see value.h) shares
 * a parent buffer at an offset, so a stack word holding one must still resolve to the *whole*
 * underlying buffer, not fail to match just because it does not sit at that buffer's own start.
 * Every other heap-referencing Value field (arr/st/res/en/clo/bi/dec/map/ch) is a pointer to its own
 * wrapper object's own exact gc_alloc'd start, never an offset into one -- see gc_copy.h's own header
 * comment -- so exact-match resolution alone is enough for anything reached by dereferencing an
 * already-found object; interior-pointer support here exists only to make the *first*, raw-stack-word
 * discovery of such a root correct too.
 *
 * Answers both from the allocation-start bitmap each arena block keeps (`arena.h`): an exact match
 * is one bit test -- "did an allocation start one header-span before `candidate`" -- and an
 * interior address costs a short backwards scan of that bitmap for the nearest start. Only
 * meaningful for an arena where every allocation went through `gc_alloc`, since that is what makes
 * "an allocation started here" and "an object's header is here" the same statement.
 *
 * This replaced walking the containing block's objects one at a time from its content start, which
 * was correct and needed no index at all, but made a call O(number of objects in the block). That
 * was knowingly accepted when nothing measured it and the only caller was a stack scan making a
 * handful of calls per collection (`gc_mark.c`'s `is_gc_heap_object` says as much where it calls
 * this: "a real, deliberate cost, not overlooked ... measured before optimized"). It then became
 * the single largest cost in the interpreter under concurrent load: 827 objects walked per answer,
 * 115 million object steps in one cursor-streaming benchmark run, 37% of all CPU time in it --
 * which the bitmap removes entirely, cutting that run's CPU from 5.6s to 3.5s. See docs/design/journal/
 * for why its *elapsed* time does not move (that benchmark waits on a database, not on CPU). */
const void *gc_lookup(const Arena *a, const void *candidate);

#endif
