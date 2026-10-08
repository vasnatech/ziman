#define _POSIX_C_SOURCE 200809L
#include "value.h"
#include <errno.h>
#include "stack.h"
#include "mapobj.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arena.h"
#include "bignum.h"
#include "decimal.h"
#include "error.h"
#include "gc_thread_heap.h"
#include "value_copy.h"
#include "xalloc.h"

Value value_closure(FunctionDecl *fn, Value *captured, int ncaptured) {
    Value v;
    ClosureObj *c = ziman_xmalloc(sizeof(ClosureObj));
    c->fn = fn;
    c->ncaptured = ncaptured;
    c->captured = captured;
    v.type = function_value_type(fn);
    /* function_value_type reads every parameter; a lambda's leading captured
     * ones are not part of its type. */
    if (ncaptured > 0) {
        TypeList params = {0};
        for (int i = ncaptured; i < fn->params.count; i++) type_list_push(&params, fn->params.items[i].type);
        v.type = type_func(params, fn->return_type);
    }
    v.as.clo = c;
    return v;
}

Value value_function(FunctionDecl *fn) {
    return value_closure(fn, NULL, 0);
}

Value value_string_view(const char *ptr, size_t len, int term) {
    if (len > 0xffffffffu) ziman_error("runtime", 0, 0, "string too long (limit is 4 GiB)");
    Value v;
    v.type = type_scalar(TY_STRING);
    v.as.str.ptr = ptr;
    v.as.str.len = (uint32_t)len;
    v.as.str.term = term ? 1 : 0;
    return v;
}

Value value_string(char *s) { return value_string_view(s, strlen(s), 1); }

/* Routed through the calling thread's own GcHeap (src/gc_thread_heap.c), not zalloc_atomic --
 * deliberately the first, narrowest allocation site Phase 3's real collector was wired to
 * (docs/DESIGN.md's "narrowest possible slice" entry), before any other constructor moves. Every
 * other TY_STRING-producing site in this file (value_string_view, value_string) still uses
 * zalloc_atomic or takes over an existing buffer -- not yet migrated. */
Value value_string_copy(const char *ptr, size_t len) {
    char *buf = gc_heap_alloc(ziman_thread_heap(), GC_ATOMIC, len + 1);
    memcpy(buf, ptr, len);
    buf[len] = '\0';
    return value_string_view(buf, len, 1);
}

long value_str_find(const char *hay, size_t hay_len, const char *needle, size_t needle_len, size_t from) {
    if (from > hay_len) from = hay_len;
    if (needle_len == 0) return (long)from;
    if (needle_len > hay_len) return -1;
    for (size_t i = from; i + needle_len <= hay_len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0) return (long)i;
    }
    return -1;
}

const char *value_cstr(Value v) {
    if (v.as.str.term) return v.as.str.ptr;
    char *buf = zalloc_atomic((size_t)v.as.str.len + 1);
    memcpy(buf, v.as.str.ptr, v.as.str.len);
    buf[v.as.str.len] = '\0';
    return buf;
}

Value value_bigint(BigNum n) {
    Value v;
    v.type = type_scalar(TY_BIGINT);
    v.as.bi = ziman_xmalloc(sizeof(BigNum));
    *v.as.bi = n;
    return v;
}

Value value_decimal(Decimal d) {
    Value v;
    v.type = type_scalar(TY_DECIMAL);
    v.as.dec = ziman_xmalloc(sizeof(Decimal));
    *v.as.dec = d;
    return v;
}


Value value_array(Type array_type, ArrayObj *arr) {
    Value v;
    v.type = array_type;
    v.as.arr = arr;
    return v;
}

/* The wrapper alone is routed through the calling thread's own GcHeap, the same "narrowest slice
 * within the slice" as struct_obj_new -- `items` stays on the ordinary allocator, and the other
 * ArrayObj constructors below (array_obj_new_packed, array_obj_view, array_push) are not migrated
 * yet either. gc_heap_alloc is now guaranteed always zero-filled (gc_heap.h/.c, fixed as part of
 * this same migration -- a reused chunk's own free-list link byte range was not being cleared,
 * which this constructor's reliance on calloc's implicit zero for bytes/bools/ints/floats/store
 * would otherwise have exposed the first time a reused slot landed here), so this needs no change
 * beyond the allocation call itself. */
ArrayObj *array_obj_new(Value *items, int count) {
    ArrayObj *a = gc_heap_alloc(ziman_thread_heap(), GC_ARRAY, sizeof(ArrayObj));
    a->boxed_items = items;
    a->count = count;
    return a;
}

ArrayObj *array_obj_new_bytes(uint8_t *bytes, int count) {
    return array_obj_new_packed(PK_BYTE, bytes, count);
}

PackKind array_pack_kind(const ArrayObj *a) {
    if (a->bytes != NULL) return PK_BYTE;
    if (a->bools != NULL) return PK_BOOL;
    if (a->ints != NULL) return PK_INT;
    if (a->floats != NULL) return PK_FLOAT;
    return PK_NONE;
}

size_t pack_elem_size(PackKind k) {
    switch (k) {
        case PK_BYTE:
        case PK_BOOL: return 1;
        case PK_INT: return sizeof(int64_t);
        case PK_FLOAT: return sizeof(double);
        default: return 0;
    }
}

ArrayObj *array_obj_new_packed(PackKind k, void *data, int count) {
    ArrayObj *a = ziman_xcalloc(1, sizeof(ArrayObj));
    switch (k) {
        case PK_BYTE: a->bytes = data; break;
        case PK_BOOL: a->bools = data; break;
        case PK_INT: a->ints = data; break;
        case PK_FLOAT: a->floats = data; break;
        default: break;
    }
    a->count = count;
    return a;
}

Value array_get(const ArrayObj *a, int i) {
    if (a->bytes != NULL) return value_byte(a->bytes[i]);
    if (a->ints != NULL) return value_int(a->ints[i]);
    if (a->floats != NULL) return value_float(a->floats[i]);
    if (a->bools != NULL) return value_bool(a->bools[i]);
    return a->boxed_items[i];
}

ArrayObj *array_obj_view(const ArrayObj *a, int start, int end) {
    ArrayObj *v = ziman_xcalloc(1, sizeof(ArrayObj));
    v->boxed_items = a->boxed_items != NULL ? a->boxed_items + start : NULL;
    v->bytes = a->bytes != NULL ? a->bytes + start : NULL;
    v->bools = a->bools != NULL ? a->bools + start : NULL;
    v->ints = a->ints != NULL ? a->ints + start : NULL;
    v->floats = a->floats != NULL ? a->floats + start : NULL;
    v->count = end - start;
    return v;
}

ArrayObj *array_push(const ArrayObj *old, Value x) {
    PackKind k = array_pack_kind(old);
    if (old->count == 0 && k == PK_NONE) k = pack_kind_of_type(x.type.kind); /* a `[]` that grows by push becomes packed on its first element */
    size_t esz = k == PK_NONE ? sizeof(Value) : pack_elem_size(k);
    int n = old->count;
    /* take the next slot of the buffer in place, when `old` is the tip and there is room */
    if (old->store != NULL && n < old->store->capacity) {
        int expected = n;
        if (__atomic_compare_exchange_n(&old->store->used, &expected, n + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            ArrayObj *grown = ziman_xcalloc(1, sizeof(ArrayObj));
            *grown = *old;
            grown->count = n + 1;
            if (k == PK_NONE) grown->boxed_items[n] = x;
            else pack_store(k, k == PK_BYTE ? (void *)grown->bytes : k == PK_BOOL ? (void *)grown->bools : k == PK_INT ? (void *)grown->ints : (void *)grown->floats, n, x);
            return grown;
        }
    }
    /* copy into a buffer with room for about as many again */
    int capacity = n < 4 ? 8 : (n <= 1073741823 ? n * 2 : n + 1);
    unsigned char *buf = k == PK_NONE ? ziman_xcalloc((size_t)capacity, esz) : zalloc_atomic(esz * (size_t)capacity);
    if (n > 0) {
        const void *src = k == PK_NONE ? (const void *)old->boxed_items : k == PK_BYTE ? (const void *)old->bytes : k == PK_BOOL ? (const void *)old->bools
                          : k == PK_INT ? (const void *)old->ints : (const void *)old->floats;
        memcpy(buf, src, esz * (size_t)n);
    }
    ArrayObj *grown = ziman_xcalloc(1, sizeof(ArrayObj));
    switch (k) {
        case PK_NONE: grown->boxed_items = (Value *)buf; grown->boxed_items[n] = x; break;
        case PK_BYTE: grown->bytes = buf; pack_store(k, buf, n, x); break;
        case PK_BOOL: grown->bools = buf; pack_store(k, buf, n, x); break;
        case PK_INT: grown->ints = (int64_t *)buf; pack_store(k, buf, n, x); break;
        case PK_FLOAT: grown->floats = (double *)buf; pack_store(k, buf, n, x); break;
    }
    grown->count = n + 1;
    grown->store = ziman_xcalloc(1, sizeof(ArrayStore));
    grown->store->capacity = capacity;
    grown->store->used = n + 1;
    return grown;
}

PackKind pack_kind_of_type(TypeKind k) {
    switch (k) {
        case TY_BYTE: return PK_BYTE;
        case TY_BOOL: return PK_BOOL;
        case TY_INT: return PK_INT;
        case TY_FLOAT: return PK_FLOAT;
        default: return PK_NONE;
    }
}

void pack_store(PackKind k, void *buf, int idx, Value v) {
    switch (k) {
        case PK_BYTE: ((uint8_t *)buf)[idx] = (uint8_t)v.as.by; break;
        case PK_BOOL: ((uint8_t *)buf)[idx] = v.as.b ? 1 : 0; break;
        case PK_INT: ((int64_t *)buf)[idx] = v.as.i; break;
        case PK_FLOAT: ((double *)buf)[idx] = v.as.f; break;
        default: break;
    }
}

ArrayObj *array_try_pack(const Value *items, int count) {
    if (count == 0) return NULL; /* an empty array stays boxed: no element to type it by */
    PackKind k = pack_kind_of_type(items[0].type.kind);
    if (k == PK_NONE) return NULL;
    for (int i = 1; i < count; i++) {
        if (pack_kind_of_type(items[i].type.kind) != k) return NULL;
    }
    void *buf = zalloc_atomic(pack_elem_size(k) * (size_t)count);
    for (int i = 0; i < count; i++) pack_store(k, buf, i, items[i]);
    return array_obj_new_packed(k, buf, count);
}

ArrayObj *array_packed_edit(const ArrayObj *old, int mode, int at, Value x) {
    PackKind k = array_pack_kind(old);
    size_t esz = pack_elem_size(k);
    const unsigned char *src = k == PK_BYTE ? old->bytes : k == PK_BOOL ? old->bools : k == PK_INT ? (const unsigned char *)old->ints : (const unsigned char *)old->floats;
    int n = old->count;
    int new_count = mode == 0 || mode == 1 ? n + 1 : mode == 3 ? n - 1 : n;
    unsigned char *buf = zalloc_atomic(esz * (size_t)(new_count > 0 ? new_count : 1));
    switch (mode) {
        case 0: /* push */
            memcpy(buf, src, esz * (size_t)n);
            pack_store(k, buf, n, x);
            break;
        case 1: /* insert */
            memcpy(buf, src, esz * (size_t)at);
            pack_store(k, buf, at, x);
            memcpy(buf + esz * (size_t)(at + 1), src + esz * (size_t)at, esz * (size_t)(n - at));
            break;
        case 2: /* set */
            memcpy(buf, src, esz * (size_t)n);
            pack_store(k, buf, at, x);
            break;
        default: /* remove */
            memcpy(buf, src, esz * (size_t)at);
            memcpy(buf + esz * (size_t)at, src + esz * (size_t)(at + 1), esz * (size_t)(n - at - 1));
            break;
    }
    return array_obj_new_packed(k, buf, new_count);
}

Value value_int_array(const int64_t *buf, size_t n) {
    int64_t *ints = zalloc_atomic(n > 0 ? n * sizeof(int64_t) : 1);
    if (n > 0) memcpy(ints, buf, n * sizeof(int64_t));
    return value_array(type_array(type_scalar(TY_INT)), array_obj_new_packed(PK_INT, ints, (int)n));
}

Value value_byte_array(const uint8_t *buf, size_t n) {
    uint8_t *bytes = zalloc_atomic(n > 0 ? n : 1);
    if (n > 0) memcpy(bytes, buf, n);
    return value_array(type_array(type_scalar(TY_BYTE)), array_obj_new_bytes(bytes, (int)n));
}

uint8_t *array_copy_bytes(const ArrayObj *a, int *out_len) {
    uint8_t *out = zalloc_atomic((size_t)a->count + 1);
    if (a->bytes != NULL) {
        memcpy(out, a->bytes, (size_t)a->count);
    } else {
        for (int i = 0; i < a->count; i++) out[i] = (uint8_t)a->boxed_items[i].as.by;
    }
    out[a->count] = 0;
    if (out_len != NULL) *out_len = a->count;
    return out;
}

Value value_struct(Type struct_type, StructObj *st) {
    Value v;
    v.type = struct_type;
    v.as.st = st;
    return v;
}

/* The wrapper alone is routed through the calling thread's own GcHeap -- `fields` (built by every
 * caller before this runs) is not yet migrated, deliberately: the next narrow slice after
 * value_string_copy (docs/DESIGN.md), not the whole of "array/struct construction" at once. Mixing
 * a gc_heap-allocated wrapper with a not-yet-migrated fields buffer is exactly the shape
 * gc_mark.c's is_gc_heap_object fix exists to handle correctly (see its own incident writeup);
 * every caller that builds `fields` via more than one call is also now guarded by
 * gc_unsafe_enter/leave (interpreter.c), the fix the *other* incident writeup this same migration
 * attempt found needed first. */
StructObj *struct_obj_new(char **field_names, Value *fields, int count) {
    StructObj *s = gc_heap_alloc(ziman_thread_heap(), GC_STRUCT, sizeof(StructObj));
    s->field_names = field_names;
    s->fields = fields;
    s->count = count;
    return s;
}

Value value_result(Type result_type, ResultObj *res) {
    Value v;
    v.type = result_type;
    v.as.res = res;
    return v;
}

/* Migrated to gc_heap_alloc (the wrapper only -- `value` is held by reference, nothing further to
 * allocate here) continuing the same "narrowest possible slice" sequence as value_string_copy/
 * struct_obj_new/array_obj_new: the mark phase (gc_mark_result, gc_mark.c) has already handled this
 * kind correctly, gated by is_gc_heap_object, since the header-corruption bug was fixed -- this is
 * the first time anything actually allocates one this way. */
ResultObj *result_obj_new(int is_ok, Value value) {
    ResultObj *r = gc_heap_alloc(ziman_thread_heap(), GC_RESULT, sizeof(ResultObj));
    r->is_ok = is_ok;
    r->value = value;
    return r;
}

Value value_enum(Type enum_type, EnumObj *en) {
    Value v;
    v.type = enum_type;
    v.as.en = en;
    return v;
}

/* Migrated to gc_heap_alloc (the wrapper only), same reasoning as result_obj_new just above --
 * enum_name/variant_name are borrowed from the permanent AST (EnumDecl), never this object's own to
 * free, so nothing else here needs migrating alongside it. */
EnumObj *enum_obj_new(char *enum_name, char *variant_name, int has_payload, Value payload) {
    EnumObj *e = gc_heap_alloc(ziman_thread_heap(), GC_ENUM, sizeof(EnumObj));
    e->enum_name = enum_name;
    e->variant_name = variant_name;
    e->has_payload = has_payload;
    e->payload = payload;
    return e;
}

Value value_map(Type map_or_set_type, struct MapObj *m) {
    Value v;
    v.type = map_or_set_type;
    v.as.map = m;
    return v;
}

Value value_channel(Type channel_type, ChannelObj *ch) {
    Value v;
    v.type = channel_type;
    v.as.ch = ch;
    return v;
}

ChannelObj *channel_obj_new(int limit) {
    ChannelObj *ch = ziman_xmalloc(sizeof(ChannelObj));
    pthread_mutex_init(&ch->mutex, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    ch->items = NULL;
    ch->head = 0;
    ch->count = 0;
    ch->capacity = 0;
    ch->limit = limit;
    ch->closed = 0;
    return ch;
}

/* The caller holds the lock. */
static void channel_push_locked(ChannelObj *ch, Value v, int needs_receive) {
    if (ch->count == ch->capacity) {
        int new_capacity = ch->capacity == 0 ? 4 : ch->capacity * 2;
        ChannelMsg *bigger = ziman_xmalloc((size_t)new_capacity * sizeof(ChannelMsg));
        for (int i = 0; i < ch->count; i++) bigger[i] = ch->items[(ch->head + i) % ch->capacity];
        ch->items = bigger;
        ch->head = 0;
        ch->capacity = new_capacity;
    }
    ch->items[(ch->head + ch->count) % ch->capacity].value = v;
    ch->items[(ch->head + ch->count) % ch->capacity].needs_receive = needs_receive;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
}

static ChannelMsg channel_pop_locked(ChannelObj *ch) {
    ChannelMsg m = ch->items[ch->head];
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    return m;
}

/* Finishes a popped message outside the channel's own lock (this may run value_copy_receive's own
 * allocation work, no reason to hold up every other send/receive on this channel for that) -- see
 * `channel_send`'s own `needs_receive` comment and value_copy.h for what this is closing out. */
static Value channel_finalize_received(ChannelMsg m) {
    if (!m.needs_receive) return m.value;
    return value_copy_receive(m.value);
}
/* ---- deadlock detection ----
 *
 * A spawned thread that dies before it sends used to leave its reader blocked in `receive` for ever,
 * with no output and no exit: tests/fixtures/error_stack_trace_spawned_thread.zm relies on the
 * harness timeout for exactly that reason and says so in its own comment, and a scratch program from
 * the 2026-10-05 review was still hung 55 minutes later. The thread's error had already been printed,
 * so the information was there -- the program simply never ended.
 *
 * The test is the one Go uses: if every live thread is blocked on a channel, no thread can ever send,
 * so no blocked thread can ever wake. Rather than track which channels have which peers -- which
 * means reference-counting senders per channel, and getting it wrong means either a false deadlock or
 * the hang back again -- each blocked thread waits with a timeout and re-asks that global question.
 * A blocked thread is doing nothing anyway, so a wakeup every CHANNEL_DEADLOCK_POLL_MS costs nothing
 * worth measuring.
 *
 * `strikes` requires the condition to hold across two consecutive polls before anything is raised.
 * The two counters are plain atomics rather than a lock, so a reader can see them mid-update; what
 * that can produce is a transient false positive, and insisting the answer is the same ~100ms later
 * removes it without introducing a second lock to order against the channel mutex.
 *
 * Only the indefinite waits take part. A thread in `receive_timeout` will wake on its own and may
 * then send, so it is not blocked in the sense that matters; a thread computing, sleeping or in a
 * syscall is live and not blocked, which makes this conservative -- it reports a deadlock only when
 * every thread really is waiting on a channel. */
#define CHANNEL_DEADLOCK_POLL_MS 100

static int g_live_threads = 1; /* the main thread; spawned ones are counted by the interpreter */
static int g_blocked_threads = 0;

void ziman_channel_thread_started(void) { __atomic_fetch_add(&g_live_threads, 1, __ATOMIC_RELAXED); }
void ziman_channel_thread_ended(void) { __atomic_fetch_sub(&g_live_threads, 1, __ATOMIC_RELAXED); }

/* Waits on `cond` for up to one poll interval, with ch->mutex held. Returns 1 when every live thread
 * has been blocked on a channel for two consecutive polls. */
static int wait_or_deadlock(ChannelObj *ch, pthread_cond_t *cond, int *strikes) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)CHANNEL_DEADLOCK_POLL_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(cond, &ch->mutex, &deadline);
    if (rc != ETIMEDOUT) { *strikes = 0; return 0; }
    int live = __atomic_load_n(&g_live_threads, __ATOMIC_RELAXED);
    int blocked = __atomic_load_n(&g_blocked_threads, __ATOMIC_RELAXED);
    if (blocked >= live && live > 0) (*strikes)++;
    else *strikes = 0;
    return *strikes >= 2;
}

static void report_deadlock(void) {
    ziman_error("runtime", 0, 0,
                "deadlock: every thread is blocked on a channel, so nothing can ever send "
                "(a spawned thread that was going to send may have ended -- any error it printed is above)");
}


int channel_send(ChannelObj *ch, Value v, int needs_receive) {
    pthread_mutex_lock(&ch->mutex);
    if (!ch->closed && ch->limit > 0 && ch->count >= ch->limit) {
        int strikes = 0;
        __atomic_fetch_add(&g_blocked_threads, 1, __ATOMIC_RELAXED);
        while (!ch->closed && ch->limit > 0 && ch->count >= ch->limit) {
            if (wait_or_deadlock(ch, &ch->not_full, &strikes)) {
                __atomic_fetch_sub(&g_blocked_threads, 1, __ATOMIC_RELAXED);
                pthread_mutex_unlock(&ch->mutex);
                report_deadlock();
                return 0;
            }
        }
        __atomic_fetch_sub(&g_blocked_threads, 1, __ATOMIC_RELAXED);
    }
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return 1;
    }
    channel_push_locked(ch, v, needs_receive);
    pthread_mutex_unlock(&ch->mutex);
    return 0;
}

int channel_try_send_status(ChannelObj *ch, Value v, int needs_receive) {
    pthread_mutex_lock(&ch->mutex);
    int status;
    if (ch->closed) {
        status = 2;
    } else if (ch->limit > 0 && ch->count >= ch->limit) {
        status = 1;
    } else {
        channel_push_locked(ch, v, needs_receive);
        status = 0;
    }
    pthread_mutex_unlock(&ch->mutex);
    return status;
}

int channel_receive(ChannelObj *ch, Value *out) {
    pthread_mutex_lock(&ch->mutex);
    if (ch->count == 0 && !ch->closed) {
        int strikes = 0;
        __atomic_fetch_add(&g_blocked_threads, 1, __ATOMIC_RELAXED);
        while (ch->count == 0 && !ch->closed) {
            if (wait_or_deadlock(ch, &ch->not_empty, &strikes)) {
                __atomic_fetch_sub(&g_blocked_threads, 1, __ATOMIC_RELAXED);
                pthread_mutex_unlock(&ch->mutex); /* before raising: ziman_error may longjmp to a thread boundary */
                report_deadlock();
                return 0;
            }
        }
        __atomic_fetch_sub(&g_blocked_threads, 1, __ATOMIC_RELAXED);
    }
    if (ch->count == 0) {
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    ChannelMsg m = channel_pop_locked(ch);
    pthread_mutex_unlock(&ch->mutex);
    *out = channel_finalize_received(m);
    return 1;
}

int channel_receive_timeout(ChannelObj *ch, int64_t millis, Value *out) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    if (millis < 0) millis = 0;
    deadline.tv_sec += (time_t)(millis / 1000);
    deadline.tv_nsec += (long)(millis % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&ch->mutex);
    while (ch->count == 0 && !ch->closed) {
        if (pthread_cond_timedwait(&ch->not_empty, &ch->mutex, &deadline) != 0) break;
    }
    if (ch->count == 0) {
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    ChannelMsg m = channel_pop_locked(ch);
    pthread_mutex_unlock(&ch->mutex);
    *out = channel_finalize_received(m);
    return 1;
}

int channel_try_receive(ChannelObj *ch, Value *out) {
    pthread_mutex_lock(&ch->mutex);
    int ok = ch->count > 0;
    ChannelMsg m;
    if (ok) m = channel_pop_locked(ch);
    pthread_mutex_unlock(&ch->mutex);
    if (ok) *out = channel_finalize_received(m);
    return ok;
}

void channel_close(ChannelObj *ch) {
    pthread_mutex_lock(&ch->mutex);
    ch->closed = 1;
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    pthread_mutex_unlock(&ch->mutex);
}

void channel_select2(ChannelObj *a, ChannelObj *b, int *out_first, Value *out) {
    for (;;) {
        if (channel_try_receive(a, out)) { *out_first = 1; return; }
        if (channel_try_receive(b, out)) { *out_first = 0; return; }
        struct timespec pause = {0, 1000000L}; /* 1 ms */
        nanosleep(&pause, NULL);
    }
}

int channel_length(ChannelObj *ch) {
    pthread_mutex_lock(&ch->mutex);
    int n = ch->count;
    pthread_mutex_unlock(&ch->mutex);
    return n;
}

static int equals_inner(Value a, Value b, int nan_reflexive) {
    ZIMAN_STACK_CHECK();
    /* Callers only compare same-typed values, and only when the type checker
     * (type_supports_equality in typecheck.c) has already established that
     * type is comparable: arrays only for a scalar element type, a struct
     * only when every field is (recursively), a Result only when both its
     * Ok and Err types are (recursively), and a user-declared enum only
     * when every payload-carrying variant's payload type is. */
    switch (a.type.kind) {
        case TY_INT: return a.as.i == b.as.i;
        case TY_FLOAT:
            /* `nan_reflexive` is the whole reason this function is shared. For `==` the answer is
             * IEEE's: NaN equals nothing, itself included, which is what every float-having language
             * means by `==` and what docs/SYNTAX.md promises. For a collection key it has to be the
             * other answer, because a key that is not equal to itself cannot be found again: the
             * trie files the second NaN as a distinct key (same hash, unequal), so a Set<float> ends
             * up holding two of them while `contains` denies both, and a Map<float, _> grows without
             * bound while `map_get` returns None. Java draws the line in exactly this place --
             * `Double.equals` is reflexive on NaN where `==` is not -- and it is the only way
             * `compare`, which already answers 0 for two NaNs, can agree with key lookup.
             *
             * -0.0 needs no special case: `==` already says it equals 0.0, and value_hash
             * (interpreter.c) already normalises it so the two hash alike, so both paths agree. */
            if (nan_reflexive && a.as.f != a.as.f && b.as.f != b.as.f) return 1;
            return a.as.f == b.as.f;
        case TY_BOOL: return a.as.b == b.as.b;
        case TY_STRING: return a.as.str.len == b.as.str.len && memcmp(a.as.str.ptr, b.as.str.ptr, a.as.str.len) == 0;
        case TY_BIGINT: return bignum_compare(*a.as.bi, *b.as.bi) == 0;
        case TY_DECIMAL: return decimal_compare(*a.as.dec, *b.as.dec) == 0;
        case TY_BYTE: return a.as.by == b.as.by;
        case TY_VOID: return 1;
        case TY_UNIT: return 1;
        case TY_ARRAY: {
            ArrayObj *arr_a = a.as.arr;
            ArrayObj *arr_b = b.as.arr;
            if (arr_a->count != arr_b->count) return 0;
            if (arr_a->bytes != NULL && arr_b->bytes != NULL) {
                return memcmp(arr_a->bytes, arr_b->bytes, (size_t)arr_a->count) == 0;
            }
            if (arr_a->ints != NULL && arr_b->ints != NULL) {
                return memcmp(arr_a->ints, arr_b->ints, (size_t)arr_a->count * sizeof(int64_t)) == 0;
            }
            if (arr_a->bools != NULL && arr_b->bools != NULL) {
                return memcmp(arr_a->bools, arr_b->bools, (size_t)arr_a->count) == 0;
            }
            for (int i = 0; i < arr_a->count; i++) {
                if (!equals_inner(array_get(arr_a, i), array_get(arr_b, i), nan_reflexive)) return 0;
            }
            return 1;
        }
        case TY_STRUCT: {
            /* Same declared struct type on both sides (enforced by the type
             * checker), so field count and order already match -- fields
             * are always stored in declaration order regardless of the
             * literal's write order (see EXPR_STRUCT_LIT in interpreter.c),
             * making a plain positional walk safe. */
            StructObj *st_a = a.as.st;
            StructObj *st_b = b.as.st;
            for (int i = 0; i < st_a->count; i++) {
                if (!equals_inner(st_a->fields[i], st_b->fields[i], nan_reflexive)) return 0;
            }
            return 1;
        }
        case TY_RESULT: {
            /* Different variants (Ok vs Err) are never equal, regardless of
             * payload -- same as any other sum-type-shaped equality (e.g.
             * Rust's derived PartialEq for an enum). */
            ResultObj *res_a = a.as.res;
            ResultObj *res_b = b.as.res;
            if (res_a->is_ok != res_b->is_ok) return 0;
            return equals_inner(res_a->value, res_b->value, nan_reflexive);
        }
        case TY_ENUM: {
            /* Same reasoning as Result: different variants are never
             * equal regardless of payload. */
            EnumObj *en_a = a.as.en;
            EnumObj *en_b = b.as.en;
            if (strcmp(en_a->variant_name, en_b->variant_name) != 0) return 0;
            if (!en_a->has_payload) return 1;
            return equals_inner(en_a->payload, en_b->payload, nan_reflexive);
        }
        case TY_CHANNEL:
            ziman_error("runtime", 0, 0, "internal error: '=='/'!=' on Channel should have been rejected by the type checker");
            return 0;
        case TY_MAP:
        case TY_SET: {
            /* Same entries in any order: same size, and every key of `a` present in `b` (with an equal value). */
            if (a.as.map->count != b.as.map->count) return 0;
            int is_map = a.type.kind == TY_MAP;
            Value *keys, *vals = NULL;
            mapobj_entries(a.as.map, &keys, is_map ? &vals : NULL);
            for (int i = 0; i < a.as.map->count; i++) {
                Value other;
                if (!mapobj_get(b.as.map, keys[i], &other)) return 0;
                if (is_map && !equals_inner(vals[i], other, nan_reflexive)) return 0;
            }
            return 1;
        }
        case TY_FUNC:
            ziman_error("runtime", 0, 0, "internal error: '=='/'!=' on a function value should have been rejected by the type checker");
            return 0;
        case TY_TYPEVAR:
            ziman_error("runtime", 0, 0, "internal error: a runtime value should never have an unsubstituted type variable as its type");
            return 0;
    }
    return 0;
}


/* `==` and `!=`: IEEE semantics, so NaN equals nothing. */
int value_equals(Value a, Value b) { return equals_inner(a, b, 0); }

/* Equality for a collection key, and for `contains`: as above but reflexive on NaN. See the
 * TY_FLOAT case for why the two have to differ, and value.h for which callers want which. */
int value_key_equals(Value a, Value b) { return equals_inner(a, b, 1); }

char *float_shortest_text(double d) {
    if (d != d) return ziman_xstrdup("nan");
    if (isinf(d)) return ziman_xstrdup(d < 0 ? "-inf" : "inf");
    if (d == 0) return ziman_xstrdup(signbit(d) ? "-0" : "0");
    char e[48];
    for (int p = 0; p <= 16; p++) {
        snprintf(e, sizeof e, "%.*e", p, d);
        if (strtod(e, NULL) == d) break;
    }
    /* e is like "-1.2345e+05": sign, digit, optional ".digits", e, exponent. */
    char digits[24];
    int nd = 0, neg = 0;
    const char *q = e;
    if (*q == '-') { neg = 1; q++; }
    for (; *q && *q != 'e'; q++) if (*q != '.') digits[nd++] = *q;
    digits[nd] = '\0';
    while (nd > 1 && digits[nd - 1] == '0') digits[--nd] = '\0';
    int exp10 = atoi(q + 1);
    char out[96];
    size_t o = 0;
    if (neg) out[o++] = '-';
    if (exp10 >= 21 || exp10 < -7) {
        out[o++] = digits[0];
        if (nd > 1) { out[o++] = '.'; memcpy(out + o, digits + 1, (size_t)nd - 1); o += (size_t)nd - 1; }
        o += (size_t)snprintf(out + o, sizeof out - o, "e%c%02d", exp10 < 0 ? '-' : '+', abs(exp10));
    } else if (exp10 >= 0) {
        for (int i = 0; i <= exp10; i++) out[o++] = i < nd ? digits[i] : '0';
        if (nd > exp10 + 1) { out[o++] = '.'; memcpy(out + o, digits + exp10 + 1, (size_t)(nd - exp10 - 1)); o += (size_t)(nd - exp10 - 1); }
    } else {
        out[o++] = '0'; out[o++] = '.';
        for (int i = 0; i < -exp10 - 1; i++) out[o++] = '0';
        memcpy(out + o, digits, (size_t)nd); o += (size_t)nd;
    }
    out[o] = '\0';
    return ziman_xstrdup(out);
}

char *value_display(Value v) {
    ZIMAN_STACK_CHECK();
    char buf[64];
    switch (v.type.kind) {
        case TY_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)v.as.i);
            return ziman_xstrdup(buf);
        case TY_FLOAT: {
            /* float_shortest_text's own contract leaves a whole-number magnitude with no
             * '.' or 'e' ("10"), same text an int of the same value would have; a value
             * displayed this way -- unlike float_to_text's, meant for exact numeric
             * round-tripping -- should always still look like a float, so one is added
             * here when the text wouldn't otherwise have one. */
            char *text = float_shortest_text(v.as.f);
            if (strpbrk(text, ".e") == NULL && strcmp(text, "nan") != 0 && strcmp(text, "inf") != 0 &&
                strcmp(text, "-inf") != 0) {
                size_t len = strlen(text);
                char *with_point = zalloc_atomic(len + 3);
                memcpy(with_point, text, len);
                with_point[len] = '.';
                with_point[len + 1] = '0';
                with_point[len + 2] = '\0';
                return with_point;
            }
            return text;
        }
        case TY_BOOL:
            return ziman_xstrdup(v.as.b ? "true" : "false");
        case TY_UNIT:
            return ziman_xstrdup("unit");
        case TY_BIGINT:
            return bignum_to_string(*v.as.bi);
        case TY_DECIMAL:
            return decimal_to_string(*v.as.dec);
        case TY_BYTE:
            snprintf(buf, sizeof(buf), "%d", v.as.by);
            return ziman_xstrdup(buf);
        case TY_STRING:
            {
                char *copy = zalloc_atomic((size_t)v.as.str.len + 1);
                memcpy(copy, v.as.str.ptr, v.as.str.len);
                copy[v.as.str.len] = '\0';
                return copy;
            }
        case TY_VOID:
            return ziman_xstrdup("void");
        case TY_ARRAY: {
            ArrayObj *arr = v.as.arr;
            size_t cap = 32, len = 0;
            char *out = zalloc_atomic(cap);
            out[len++] = '[';
            for (int i = 0; i < arr->count; i++) {
                if (i > 0) {
                    if (len + 2 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
                    out[len++] = ',';
                    out[len++] = ' ';
                }
                char *piece = value_display(array_get(arr, i));
                size_t piece_len = strlen(piece);
                while (len + piece_len + 2 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
                memcpy(out + len, piece, piece_len);
                len += piece_len;
                free(piece);
            }
            if (len + 2 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
            out[len++] = ']';
            out[len] = '\0';
            return out;
        }
        case TY_STRUCT: {
            StructObj *st = v.as.st;
            const char *type_display_name = v.type.struct_name;
            size_t cap = 32, len = 0;
            char *out = zalloc_atomic(cap);
            size_t name_len = strlen(type_display_name);
            while (len + name_len + 3 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
            memcpy(out + len, type_display_name, name_len);
            len += name_len;
            out[len++] = ' ';
            out[len++] = '{';
            out[len++] = ' ';
            for (int i = 0; i < st->count; i++) {
                if (i > 0) {
                    if (len + 2 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
                    out[len++] = ',';
                    out[len++] = ' ';
                }
                const char *field_name = st->field_names[i];
                char *piece = value_display(st->fields[i]);
                size_t field_len = strlen(field_name), piece_len = strlen(piece);
                while (len + field_len + piece_len + 4 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
                memcpy(out + len, field_name, field_len);
                len += field_len;
                out[len++] = ':';
                out[len++] = ' ';
                memcpy(out + len, piece, piece_len);
                len += piece_len;
                free(piece);
            }
            if (len + 3 > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
            out[len++] = ' ';
            out[len++] = '}';
            out[len] = '\0';
            return out;
        }
        case TY_RESULT: {
            const char *tag = v.as.res->is_ok ? "Ok" : "Err";
            char *inner = value_display(v.as.res->value);
            size_t len = strlen(tag) + 1 + strlen(inner) + 1 + 1;
            char *out = zalloc_atomic(len);
            snprintf(out, len, "%s(%s)", tag, inner);
            free(inner);
            return out;
        }
        case TY_ENUM: {
            EnumObj *en = v.as.en;
            if (!en->has_payload) return ziman_xstrdup(en->variant_name);
            char *inner = value_display(en->payload);
            size_t len = strlen(en->variant_name) + 1 + strlen(inner) + 1 + 1;
            char *out = zalloc_atomic(len);
            snprintf(out, len, "%s(%s)", en->variant_name, inner);
            free(inner);
            return out;
        }
        case TY_MAP:
        case TY_SET: {
            int is_map = v.type.kind == TY_MAP;
            Value *keys, *vals = NULL;
            mapobj_entries(v.as.map, &keys, is_map ? &vals : NULL);
            size_t cap = 32, len = 0;
            char *out = zalloc_atomic(cap);
            out[len++] = '{';
            for (int i = 0; i < v.as.map->count; i++) {
                char *k = value_display(keys[i]);
                char *val = is_map ? value_display(vals[i]) : NULL;
                size_t need = strlen(k) + (val != NULL ? strlen(val) : 0) + 8;
                while (len + need > cap) { cap *= 2; out = ziman_xrealloc(out, cap); }
                if (i > 0) { out[len++] = ','; out[len++] = ' '; }
                memcpy(out + len, k, strlen(k));
                len += strlen(k);
                if (val != NULL) {
                    out[len++] = ':';
                    out[len++] = ' ';
                    memcpy(out + len, val, strlen(val));
                    len += strlen(val);
                }
            }
            if (len + 2 > cap) { cap += 2; out = ziman_xrealloc(out, cap); }
            out[len++] = '}';
            out[len] = '\0';
            return out;
        }
        case TY_CHANNEL:
            return ziman_xstrdup("Channel");
        case TY_FUNC: {
            const char *name = v.as.clo->fn->name;
            size_t len = strlen(name) + 12;
            char *out = ziman_xmalloc(len);
            snprintf(out, len, "<function %s>", name);
            return out;
        }
        case TY_TYPEVAR:
            ziman_error("runtime", 0, 0, "internal error: a runtime value should never have an unsubstituted type variable as its type");
            return ziman_xstrdup("");
    }
    return ziman_xstrdup("");
}
