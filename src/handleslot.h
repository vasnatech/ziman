#ifndef ZIMAN_HANDLESLOT_H
#define ZIMAN_HANDLESLOT_H

#include <stddef.h>
#include <stdint.h>

/* A handle table whose validation is a load and two compares, with no lock and no hash.
 *
 * What it replaces, and why. `src/handles.h`'s `HandleSet` answers "is this handle live" by hashing
 * the object's address and probing a sharded open-addressed table under that shard's mutex. The
 * mutex is cheap (2.5ns uncontended, measured) but the probe is not: it is a dependent load into a
 * table touched rarely enough to be cold, and the whole check costs 10-14ns on *every* builder
 * call -- measured by short-circuiting it, which took `MutableMap len` from 36 to 22 ns/op and
 * `contains` from 35 to 25. That file's own comment names this design as "the real answer and a
 * much larger change".
 *
 * A handle here is not the object's address but an index into a slot table plus a generation
 * counter, packed into one int64: the low 32 bits index, the next 31 count how many times that slot
 * has been handed out, and the sign bit stays clear so the value is a well-behaved Ziman `int`.
 * Validation bounds-checks the index, loads the slot, and compares the generation. Nothing is
 * hashed and nothing is locked.
 *
 * The generation is what closes a hazard the address-keyed design has: a stale handle whose object
 * died and whose *address* was then reused by a new object would validate, silently naming the
 * wrong thing (src/handles.c records this as reachable in principle). A reused slot has a different
 * generation, so a stale handle fails. That property is the part of this design worth keeping, and
 * it survives the correction below.
 *
 * ** The part that needs care: what roots the object. ** Today the handle *is* the address, and an
 * integer indistinguishable from a pointer is exactly what keeps the object alive through the
 * conservative collector -- `handles.c` explains this at length, and `native_net.c`'s `handle_use`
 * depends on it. An index roots nothing, so the slot has to hold the pointer instead; and a slot
 * holding a *strong* pointer would keep every handle ever created alive, which is the leak
 * `handles.c` deliberately refuses (its finalizers exist to close a handle nobody holds).
 *
 * So a slot holds a **weak** pointer: the slots live in `zalloc_atomic` memory, which the collector
 * does not scan, and each is registered with `GC_general_register_disappearing_link` so the
 * collector clears it when the object becomes unreachable.
 *
 * ** THIS MODULE IS NOT USABLE AS IT STANDS, AND NOTHING USES IT. ** The paragraphs above describe
 * the intended design; the two paragraphs below record why it does not work, found 2026-10-16
 * while rewiring the first caller. Read them before wiring anything up.
 *
 * **A weak slot roots nothing, which is the whole problem, not a detail of it.** If a Ziman value
 * holds only an index, then nothing anywhere roots the object: the slot is deliberately unscanned,
 * so the collector sees a `MutableString` buffer as unreachable the moment it is created. The first
 * collection takes every live builder in the program and the next call on one fails with "not a
 * valid MutableString handle" -- not a race, just the ordinary case. `tests/c/handleslot_test.c`
 * passes 21 assertions without noticing, because a unit test never runs a collection; that is what
 * its own note about the weak half being unexercised was standing in front of.
 *
 * **And the lock-free lookup is unsound even once something does root the object.** Boehm's own
 * documentation for `GC_general_register_disappearing_link` says accessing the disguised pointer
 * "generally requires that the allocator lock is held, at least in the reader mode ... Otherwise a
 * strong pointer could be recreated between the time the collector decides to reclaim the object
 * and the link is cleared." That window is real in this build: `GC_stopped_mark` restarts the world
 * (vendor/bdwgc/alloc.c) and only then does `GC_finish_collection` run -- its own comment says
 * "Assumes mark bits are consistent, but the world is otherwise running" -- with `GC_finalize`, and
 * so `GC_make_disappearing_links_disappear`, called from inside it. A lookup landing in that window
 * loads a pointer to an object already marked unreachable, and marking is over, so nothing the
 * caller then does roots it: it is swept while in use, and for a socket or a cursor its finalizer
 * closes the descriptor underneath. Taking the allocator lock per lookup is the documented remedy
 * and it costs more than the 10-14ns this module exists to save, so it is not one.
 *
 * **What a sound version looks like.** Keep the object rooted exactly as it is rooted today -- the
 * address in a scanned Ziman field -- and use the index *only* to validate. The wrapper value then
 * carries two fields, say `{ addr, handle }`: `addr` is never dereferenced and exists to root, and
 * `handle` is the index+generation this table checks. That restores both properties at once: the
 * object is reachable for as long as a Ziman value names it, so the collector never condemns it
 * while a caller holds it and the race window cannot be entered; and a fabricated or stale handle
 * is still rejected by the generation compare. The cost is one extra word per handle value and a
 * Ziman-visible second field on the wrapper structs (`net.Listener`, `MutableString`, ...), which
 * is the part worth agreeing before building: those structs are ordinary `pub struct`s a program
 * can write out by hand, as `src/handles.c` records.
 *
 * Capacity is fixed rather than grown, so that readers need no lock and no version counter: a
 * block, once allocated, is never moved or freed. HANDLESLOT_BLOCKS * HANDLESLOT_BLOCK slots is the
 * ceiling, and `handleslot_add` answers 0 past it rather than pretending. */

#define HANDLESLOT_BLOCK 1024
#define HANDLESLOT_BLOCKS 4096

typedef struct HandleSlots HandleSlots;

/* Allocates the table. One per kind of handle, mirroring `HandleSet`'s one-per-kind use. */
HandleSlots *handleslot_table(void);

/* Takes a slot for `p` and answers the handle naming it, or 0 if the table is full. `p` is held
 * weakly: this does not keep it alive, and the handle stops resolving once it is collected. */
int64_t handleslot_add(HandleSlots *t, void *p);

/* `p` if `handle` still names it, or NULL -- for a handle that was never valid, whose slot has been
 * reused since (the generation differs), or whose object has been collected. */
void *handleslot_get(const HandleSlots *t, int64_t handle);

/* Gives the slot back, so `handle` and every copy of it stop resolving at once. 1 if it was live.
 * The generation moves on, so the slot's next tenant cannot be mistaken for this one. */
int handleslot_release(HandleSlots *t, int64_t handle);

/* Live handles, for a test or a diagnostic. */
size_t handleslot_live(const HandleSlots *t);

#endif
