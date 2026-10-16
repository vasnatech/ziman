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
 * generation, so a stale handle fails.
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
 * collector clears it when the object becomes unreachable. The Boehm contract says exactly this is
 * supported -- "`link` will point to a location (in a GC-allocated object or not) holding a
 * disguised pointer to `obj`. (A pointer inside an 'atomic' object is effectively disguised.)" --
 * and clears the link *before* finalization, so a handle stops resolving before its finalizer runs
 * rather than after.
 *
 * A lookup that succeeds is then safe for as long as the caller holds the pointer, which is the
 * other half of the argument: the pointer lands in a local, and the collector scans every thread's
 * stack and registers conservatively, so from that moment the object is rooted the ordinary way. A
 * collection cannot land between the load and the scan, because it stops the thread to do the
 * scanning.
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
