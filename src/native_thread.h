#ifndef ZIMAN_NATIVE_THREAD_H
#define ZIMAN_NATIVE_THREAD_H

/* Registers the `thread` core module's natives (id, name, set_name, is_main, stack_remaining and
 * the slot functions behind `thread.ThreadLocal<T>`). See lib/core/thread.zm. */
void native_thread_register(void);

/* Called by a spawned thread as it starts and finishes (interpreter.c's spawn_thread_entry):
 * begin gives it an id and a name (`<function>-<id>`); end drops its record and every
 * thread-local value it held, so they can be collected. The main thread needs neither -- its
 * record is made the first time it is asked for. */
void ziman_thread_begin(const char *function_name);
void ziman_thread_end(void);

/* This thread's thread-local values, as gc_heap roots. See the definition in native_thread.c. */
struct Arena;
void ziman_thread_mark_slots(const struct Arena *a);

#endif
