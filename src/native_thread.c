#define _POSIX_C_SOURCE 200809L
#include "native_thread.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"
#include "stack.h"
#include "value.h"

/* One record per thread that has a name or a thread-local value. The records hang off a global list
 * (a garbage-collector root) rather than only off a `__thread` pointer, so the values in a thread's
 * slots stay reachable however the collector treats thread-local storage; ziman_thread_end takes a
 * finished thread's record off the list. A thread's own slots are touched by that thread only, so
 * only the list itself needs the lock. */
typedef struct {
    int64_t key;
    Value value;
} Slot;

typedef struct ThreadInfo {
    struct ThreadInfo *next;
    int64_t id;
    char *name;
    Slot *slots;
    int count, capacity;
} ThreadInfo;

static __thread ThreadInfo *t_info = NULL;
static ThreadInfo *g_threads = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_next_id = 1; /* the main thread is 0 */
static int64_t g_next_key = 1;

static ThreadInfo *make_info(int64_t id, const char *name) {
    ThreadInfo *t = calloc(1, sizeof(ThreadInfo));
    t->id = id;
    t->name = strdup(name);
    pthread_mutex_lock(&g_lock);
    t->next = g_threads;
    g_threads = t;
    pthread_mutex_unlock(&g_lock);
    return t;
}

void ziman_thread_begin(const char *function_name) {
    int64_t id = __atomic_fetch_add(&g_next_id, 1, __ATOMIC_RELAXED);
    char name[160];
    snprintf(name, sizeof name, "%s-%lld", function_name, (long long)id);
    t_info = make_info(id, name);
}

void ziman_thread_end(void) {
    ThreadInfo *t = t_info;
    if (t == NULL) return;
    pthread_mutex_lock(&g_lock);
    for (ThreadInfo **p = &g_threads; *p != NULL; p = &(*p)->next) {
        if (*p == t) { *p = t->next; break; }
    }
    pthread_mutex_unlock(&g_lock);
    t_info = NULL;
}

/* The calling thread's record, made on first use for a thread that has none (the main thread). */
static ThreadInfo *info(void) {
    if (t_info == NULL) {
        if (ziman_stack_is_main()) {
            t_info = make_info(0, "main");
        } else {
            int64_t id = __atomic_fetch_add(&g_next_id, 1, __ATOMIC_RELAXED);
            char name[32];
            snprintf(name, sizeof name, "thread-%lld", (long long)id);
            t_info = make_info(id, name);
        }
    }
    return t_info;
}

static Slot *find_slot(ThreadInfo *t, int64_t key) {
    for (int i = 0; i < t->count; i++) {
        if (t->slots[i].key == key) return &t->slots[i];
    }
    return NULL;
}

static Value native_thread_id(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_int(info()->id);
}

static Value native_thread_name(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_string(strdup(info()->name));
}

static Value native_thread_set_name(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    ThreadInfo *t = info();
    t->name = strdup(value_cstr(args[0]));
    return value_int(0);
}

static Value native_thread_is_main(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_bool(ziman_stack_is_main());
}

static Value native_thread_stack_remaining(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_int((int64_t)ziman_stack_remaining());
}

static Value native_thread_next_key(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    return value_int(__atomic_fetch_add(&g_next_key, 1, __ATOMIC_RELAXED));
}

static Value native_thread_slot_has(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return value_bool(find_slot(info(), args[0].as.i) != NULL);
}

static Value native_thread_slot_get(Value *args, int argc, int line, int col) {
    (void)argc;
    Slot *s = find_slot(info(), args[0].as.i);
    if (s == NULL) ziman_error("runtime", line, col, "thread.get: this thread has no value for that key");
    return s->value;
}

static Value native_thread_slot_set(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    ThreadInfo *t = info();
    Slot *s = find_slot(t, args[0].as.i);
    if (s == NULL) {
        if (t->count == t->capacity) {
            int cap = t->capacity == 0 ? 4 : t->capacity * 2;
            Slot *bigger = malloc((size_t)cap * sizeof(Slot));
            if (t->count > 0) memcpy(bigger, t->slots, (size_t)t->count * sizeof(Slot));
            t->slots = bigger;
            t->capacity = cap;
        }
        s = &t->slots[t->count++];
        s->key = args[0].as.i;
    }
    s->value = args[1];
    return value_int(0);
}

static Value native_thread_slot_remove(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    ThreadInfo *t = info();
    for (int i = 0; i < t->count; i++) {
        if (t->slots[i].key == args[0].as.i) {
            t->slots[i] = t->slots[t->count - 1];
            t->count--;
            break;
        }
    }
    return value_int(0);
}

void native_thread_register(void) {
    native_register("thread::id", native_thread_id);
    native_register("thread::name", native_thread_name);
    native_register("thread::set_name", native_thread_set_name);
    native_register("thread::is_main", native_thread_is_main);
    native_register("thread::stack_remaining", native_thread_stack_remaining);
    native_register("thread::next_key", native_thread_next_key);
    native_register("thread::slot_has", native_thread_slot_has);
    native_register("thread::slot_get", native_thread_slot_get);
    native_register("thread::slot_set", native_thread_slot_set);
    native_register("thread::slot_remove", native_thread_slot_remove);
}
