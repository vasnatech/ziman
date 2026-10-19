#ifndef ZIMAN_VALUE_H
#define ZIMAN_VALUE_H

/* Memory for bytes that can never hold a pointer (a string's text): under the
 * collector it is allocated "atomic", so it is never scanned for pointers --
 * cheaper to mark and immune to a stray text byte pattern looking like an
 * address. Without the collector it is plain malloc. */
#include "xalloc.h"
#ifdef ZIMAN_GC
#include <gc.h>
/* Checked like every other allocation here (src/xalloc.h): a packed array's buffer is sized by the
 * program, so it is one of the few that can genuinely fail, and it used to come back NULL and be
 * memcpy'd into -- a segfault under a memory limit where the rest of the interpreter now reports
 * (docs/reviews/tasks.md A20). */
#ifdef ZIMAN_ALLOC_PROFILE
#define zalloc_atomic(n) ziman_xcheck(ziman_alloc_atomic(__FILE__, __LINE__, (n)))
#else
#define zalloc_atomic(n) ziman_xcheck(GC_MALLOC_ATOMIC(n))
#endif
#else
#include <stdlib.h>
#define zalloc_atomic(n) ziman_xmalloc(n)
#endif

#include <pthread.h>
#include <stdint.h>

#include "ast.h"

typedef struct Value Value;

/* An array's backing storage. Immutable once constructed: nothing in the
 * interpreter ever changes `items`/`count` after array_obj_new() returns.
 * That invariant is what lets Value structs share an ArrayObj by pointer
 * (a plain shallow copy) and still get correct value semantics for free --
 * exactly like Ziman's strings already do (also shared by pointer, safe for
 * the same reason: nothing can mutate one through an alias). push/insert/set
 * never mutate their input; they always build a brand new ArrayObj. */
/* Room to grow that arrays made by `push` share: the buffer is `capacity` elements long and `used` of them are taken. An array whose length equals `used` is the
 * *tip* of the buffer, and pushing to it takes the next slot in place (a compare-and-swap on `used`, so two threads pushing to the same array cannot both win); any
 * other array over the buffer -- an older one, or a second push to the same one -- copies instead. Nothing ever changes an element an array can see. */
typedef struct ArrayStore {
    int capacity;
    int used; /* accessed with atomic operations */
} ArrayStore;

typedef struct ArrayObj {
    Value *boxed_items; /* one Value per element, or NULL when one of the packed buffers is used */
    /* Packed storage for an array of one scalar type: at most one of these is
     * non-NULL, holding `count` raw elements (a byte, a bool as 0/1, an int64, a
     * double) instead of a 16-byte-plus Value each. Invisible to programs. */
    uint8_t *bytes;    /* [byte] */
    uint8_t *bools;    /* [bool] */
    int64_t *ints;     /* [int] */
    double *floats;    /* [float] */
    int count;
    ArrayStore *store; /* spare room shared with the arrays pushed from this one, or NULL (a literal, a slice, a builder's result: no room, push copies) */
} ArrayObj;

typedef enum { PK_NONE = 0, PK_BYTE, PK_BOOL, PK_INT, PK_FLOAT } PackKind;

/* A struct's backing storage. Same immutable-once-built invariant and the
 * same payoff as ArrayObj: a Value can share one by plain pointer copy and
 * still get correct value semantics, since only `with` ever produces a new
 * StructObj (it never mutates the one it started from). `field_names` is
 * borrowed from the StructDecl (never freed, never duplicated per
 * instance) so this module doesn't need a Module* to print or look up
 * fields by name -- the interpreter, which does have one, resolves names
 * to slots once at construction time. */
typedef struct StructObj {
    char **field_names;
    Value *fields;
    int count;
} StructObj;

struct Value {
    /* A pointer, not an embedded `Type`. Ziman is statically typed, so carrying a whole 64-byte
     * type descriptor inside the most-copied struct in the interpreter was paying at runtime for
     * what the checker already proved. This takes `Value` from 80 bytes to 24, and with it every
     * heap object that holds one -- EnumObj 104 -> 48, ResultObj 88 -> 32, both crossing a size
     * class, so the sweep phase walks and zeroes half as much.
     *
     * Worth about a fifth of the runtime, and mostly not for the reason it looks like: the
     * collector does move as intended (bytes walked by the mark phase 111.5 MB -> 19.5 MB), but
     * that is a few points of it. The rest is that every `Value` copy -- argument passing, array
     * elements, struct fields, every return -- moves 56 bytes less, and `type_scalar` stops
     * zeroing 64 bytes per arithmetic result and becomes a constant address. 24 bytes is also
     * register-friendly where 80 never was, which is why this does not hit the scalar-replacement
     * regression that unioning `Type`'s name fields did (docs/MEMORY.md records that attempt and
     * why a smaller struct is not automatically faster code).
     *
     * The pointee must outlive the value. Scalars are per-kind statics -- a designated initialiser
     * in `.rodata`, which is what makes them safe to point at from any thread without a lazily
     * filled table to race over -- and compound types are allocated by `type_ref`.
     *
     * Deduplicating the pointees is measured as worth nothing and is not worth retrying for
     * speed: a fixed-size interning table took a million `type_ref` lookups down to ten
     * allocations and the clock did not move (docs/MEMORY.md). */
    const Type *type;
    union {
        int64_t i;
        double f;
        int b; /* 0 or 1 */
        int by; /* 0-255, unsigned -- embedded, not heap, same reasoning
                  * as `b`: no union growth, since `i`/`f`/every pointer
                  * member here are already 8 bytes. */
        /* A string is a view: `len` bytes at `ptr`, immutable. It may be a
         * whole buffer or a slice of one (substring/trim/split share their
         * parent's bytes instead of copying). `term` says ptr[len] is a NUL,
         * which is what a native needs to hand the bytes to a C API without
         * copying -- see value_cstr. 16 bytes, so a Value grows from 72 to
         * 80; a string is limited to 4 GiB. */
        struct { const char *ptr; uint32_t len; uint32_t term; } str;
        BigNum *bi; /* heap; kept as a pointer (not embedded) so this union
                     * -- and every Value in the interpreter, not just
                     * bigint ones -- stays the same size as it was before
                     * bigint existed, since a union's size is always its
                     * largest member's. Never mutated once built, same as
                     * every other heap object here. */
        Decimal *dec; /* heap, same reasoning as `bi` */
        ArrayObj *arr;
        StructObj *st;
        struct ResultObj *res;
        struct EnumObj *en;
        struct ChannelObj *ch;
        struct MapObj *map; /* TY_MAP / TY_SET */
        struct ClosureObj *clo; /* TY_FUNC: a function plus the values it captured */
    } as;
};

/* A Result's backing storage. Same immutable-once-built invariant as
 * ArrayObj/StructObj: nothing ever mutates one after construction, so a
 * Value can share it by plain pointer copy safely. Embeds a real Value
 * (not a pointer to one) since Value is now fully defined -- this struct
 * has to come after it for that reason. */
typedef struct ResultObj {
    int is_ok; /* 1 = Ok, 0 = Err */
    Value value; /* the Ok payload if is_ok, the Err payload otherwise */
} ResultObj;

/* A function value: the function and, for a lambda, the values it captured
 * (immutable, copied when the lambda was evaluated). A reference to a named
 * top-level function has ncaptured == 0. */
typedef struct ClosureObj {
    FunctionDecl *fn;
    int ncaptured;
    Value *captured;
} ClosureObj;

/* A user-declared enum value's backing storage. Same immutable-once-built
 * invariant as the others. `enum_name`/`variant_name` are borrowed from the
 * EnumDecl (never freed, never duplicated per instance), same trick as
 * StructObj's `field_names` -- lets this module print/compare an enum
 * value without needing a Module* to look anything up by index. `payload`
 * is meaningful only when `has_payload` (a unit variant like `Point` has
 * nothing to store there). */
typedef struct EnumObj {
    char *enum_name;
    char *variant_name;
    int has_payload;
    Value payload;
} EnumObj;

/* A channel's backing storage: a thread-safe, unbounded FIFO queue.
 * Unlike ArrayObj/StructObj/EnumObj/ResultObj, this one is genuinely
 * mutable in place -- it's the one Ziman value that's a stateful,
 * identity-based handle rather than a plain data aggregate, which is also
 * why it's not comparable (see type_supports_equality in typecheck.c).
 * `send` never blocks (just enqueues); `receive` blocks on `cond` while
 * empty. Every other Value that ever flows through a channel is safe to
 * share across threads by plain pointer copy with no locking of its own
 * needed: nothing in this interpreter ever frees an ArrayObj/StructObj/
 * EnumObj/ResultObj (see docs/DESIGN.md), and nothing mutates one after
 * construction, so there's neither a use-after-free nor a data-race
 * hazard once a Value has crossed the channel's own lock. */
/* A queued message plus whether `value` is a crossing-safe copy (src/value_copy.c's
 * `value_copy_send`) that the receiving side still owes a `value_copy_receive` on before handing it
 * to its own caller -- which every receiving function (channel_receive/_timeout/try_receive/select2,
 * value.c) does transparently. 0 for an ordinary `channel_send` of a value that is safe to share as
 * it stands (prelude.c's own `guard_finalizer`). Nothing here owns memory: a crossing copy lives on
 * Boehm's shared heap and is reachable through this very struct, so a message nobody ever receives
 * is reclaimed by the collector rather than needing anyone to release it (see value_copy.h, and
 * docs/DESIGN.md for the three distinct ceilings the arena-per-crossing design it replaced hit). */
typedef struct {
    Value value;
    int needs_receive;
} ChannelMsg;

typedef struct ChannelObj {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    ChannelMsg *items; /* a ring buffer: `head` is the oldest element, `count` how many */
    int head;
    int count;
    int capacity;  /* size of `items` */
    int limit;     /* 0 = unbounded; otherwise `send` blocks while `count == limit` */
    int closed;    /* set by channel_close; see its own comment */
} ChannelObj;

/* The scalar constructors are `static inline` for the same reason `type_scalar` is (ast.h): they
 * are the most frequently executed functions in the interpreter -- one per literal, per arithmetic
 * result, per comparison -- and with no LTO in this build each was an out-of-line call returning an
 * 80-byte `Value` through a hidden pointer. `value_int` alone was 30% of a recursive-fib profile.
 * They are one assignment each; there is nothing for a call to amortise. */
static inline Value value_int(int64_t i) {
    Value v;
    v.type = type_scalar_ref(TY_INT);
    v.as.i = i;
    return v;
}

static inline Value value_unit(void) {
    Value v;
    v.type = type_scalar_ref(TY_UNIT);
    v.as.i = 0;
    return v;
}

static inline Value value_float(double f) {
    Value v;
    v.type = type_scalar_ref(TY_FLOAT);
    v.as.f = f;
    return v;
}

static inline Value value_bool(int b) {
    Value v;
    v.type = type_scalar_ref(TY_BOOL);
    v.as.b = b;
    return v;
}
/* Takes ownership of a NUL-terminated heap buffer. */
Value value_string(char *s);
/* A function value: a reference to a named top-level function (no capture). */
Value value_function(FunctionDecl *fn);
/* A function value that carries `ncaptured` values, bound to the decl's first
 * `ncaptured` parameters on each call. `captured` is taken over. */
Value value_closure(FunctionDecl *fn, Value *captured, int ncaptured);
/* A view of `len` bytes at `ptr` -- no copy. `term` is 1 only if ptr[len] is
 * a NUL. */
Value value_string_view(const char *ptr, size_t len, int term);
/* A fresh, NUL-terminated copy of `len` bytes at `ptr`. */
Value value_string_copy(const char *ptr, size_t len);
/* The string's bytes as a NUL-terminated C string: the view itself when it
 * is already terminated, else a fresh copy. Bytes after an embedded NUL are
 * invisible to the C side. */
const char *value_cstr(Value v);
/* Byte offset of the first `needle` at or after `from` in the haystack, or
 * -1. An empty needle matches at `from` (clamped to the length). */
long value_str_find(const char *hay, size_t hay_len, const char *needle, size_t needle_len, size_t from);
Value value_bigint(BigNum n); /* copies `n` onto the heap; caller keeps owning `n`'s own digits */
Value value_decimal(Decimal d); /* likewise, copies `d` onto the heap */
/* Caller's responsibility to have already range-checked 0-255. Inline, see value_int above. */
static inline Value value_byte(int b) {
    Value v;
    v.type = type_scalar_ref(TY_BYTE);
    v.as.by = b;
    return v;
}
Value value_array(const Type *array_type, ArrayObj *arr);
Value value_struct(const Type *struct_type, StructObj *st);
Value value_result(const Type *result_type, ResultObj *res);
Value value_enum(const Type *enum_type, EnumObj *en);
Value value_channel(const Type *channel_type, ChannelObj *ch);
Value value_map(const Type *map_or_set_type, struct MapObj *m);
/* The hash `hash()` returns before the sign bit is cleared: equal values hash equal. */
uint64_t value_hash(Value v);

ArrayObj *array_obj_new(Value *items, int count);
/* `old` with `x` added at the end. Amortised O(1) when pushes follow one another (`a = push(a, x)` in a loop): see ArrayStore. */
ArrayObj *array_push(const ArrayObj *old, Value x);
/* A packed [byte]: `bytes` (one real byte per element, allocated by the caller
 * with zalloc_atomic or pointing into another packed array) is used as is. */
ArrayObj *array_obj_new_bytes(uint8_t *bytes, int count);
/* Element `i` (0 <= i < count, unchecked) as a Value, whichever storage backs it. */
Value array_get(const ArrayObj *a, int i);
/* A zero-copy view of elements [start, end) sharing `a`'s storage (boxed or packed). */
ArrayObj *array_obj_view(const ArrayObj *a, int start, int end);
/* Which packed representation `a` uses (PK_NONE for a boxed array). */
PackKind array_pack_kind(const ArrayObj *a);
PackKind pack_kind_of_type(TypeKind k); /* PK_NONE for anything that is not byte/bool/int/float */
void pack_store(PackKind k, void *buf, int idx, Value v);
size_t pack_elem_size(PackKind k);
/* A packed ArrayObj of `count` elements over `data` (taken over). */
ArrayObj *array_obj_new_packed(PackKind k, void *data, int count);
/* Packs `items` if there is at least one element and they are all byte, bool,
 * int or float Values (all the same kind); NULL otherwise -- the caller keeps
 * the boxed array. */
ArrayObj *array_try_pack(const Value *items, int count);
/* A new packed array from `old` (packed): mode 0 push `x`, 1 insert `x` at `at`,
 * 2 set element `at` to `x`, 3 remove element `at`. */
ArrayObj *array_packed_edit(const ArrayObj *old, int mode, int at, Value x);
ArrayObj *array_swapped(const ArrayObj *old, int i, int j);
/* Packed [int] / [float] Values holding a copy of `buf[0..n)`. */
Value value_int_array(const int64_t *buf, size_t n);
/* A fresh, writable copy of the bytes of a [byte] array, NUL-terminated for
 * convenience; `*out_len` gets the count. */
uint8_t *array_copy_bytes(const ArrayObj *a, int *out_len);
/* A packed [byte] Value holding a copy of `buf[0..n)`. */
Value value_byte_array(const uint8_t *buf, size_t n);
StructObj *struct_obj_new(char **field_names, Value *fields, int count);
ResultObj *result_obj_new(int is_ok, Value value);
EnumObj *enum_obj_new(char *enum_name, char *variant_name, int has_payload, Value payload);

/* `Option.None`, as one shared constant rather than an allocation per call -- the same move
 * `type_option_ref` (src/ast.c) made for the `Option` *type*, for the same reason and with the same
 * limit. `type_option_ref`'s own comment says a `mutable_map_get` "did three heap allocations where
 * one (the `EnumObj`) is the only one carrying per-call information". That is true of `Some`, which
 * holds the value found; it is not true of `None`, which carries no payload and so holds nothing
 * that distinguishes one instance from another.
 *
 * Returned non-const because `value_enum` takes `EnumObj *` for the AST's sake; nothing ever writes
 * through it, which is what makes the sharing sound. Checked rather than assumed: every reader
 * gates on `has_payload` before touching `payload` (equality, `display`, both copy paths, the mark
 * phase), equality compares `variant_name` with `strcmp` rather than by pointer, and nothing
 * mutates an `EnumObj` after construction at all.
 *
 * The collector never writes to it either, which is the part that would fault on a `.rodata`
 * object: `gc_mark_enum` only marks an `EnumObj` that `is_gc_heap_object` confirms lives in the
 * arena (a bitmap test, no header read), the sweep walks only its own arena, and `gc_copy_enum`
 * reads. A value crossing a `spawn`/`Channel` boundary is copied into the outbound arena as
 * before, so a thread never shares this with another one by accident -- it does not need to, since
 * the constant is immutable and in every thread's address space already. */
EnumObj *enum_obj_option_none(void);
ChannelObj *channel_obj_new(int limit);
/* Blocks until sent or the channel is closed. Returns 1 if it was closed (nothing sent), 0 on
 * success -- the caller (interpreter.c) turns 1 into a runtime error, same as `channel_new`'s
 * own capacity check does for its argument.
 *
 * `needs_receive`: 0 for an ordinary send of a value safe to share as it stands (prelude.c's own
 * `guard_finalizer`); 1 for a `value_copy_send` copy, which the receiving side re-copies into its
 * own allocators. Nothing is owned either way -- a value never actually queued (the channel is
 * already closed) is simply dropped, and the collector reclaims it. */
int channel_send(ChannelObj *ch, Value v, int needs_receive);
/* Blocks until a value arrives or the channel is closed and drained (already-queued values are
 * still delivered after close -- closing only means no *more* will be sent). Returns 1 with
 * *out set, or 0 if closed and empty.
 *
 * Transparently finishes a crossing-safe send (see `channel_send`'s own `needs_receive` comment and
 * value_copy.h): if the popped message is flagged, `*out` is `value_copy_receive`'s own independent
 * copy rather than the sender's -- every receive-side function behaves identically here, whether or
 * not the specific message it happened to pop needed this at all. */
int channel_receive(ChannelObj *ch, Value *out);
/* Same as channel_receive, but gives up after `millis` milliseconds: 0 either way (timed out, or
 * closed and empty) -- the two aren't distinguished, the same blur a `select` with a timeout
 * case has elsewhere. */
int channel_receive_timeout(ChannelObj *ch, int64_t millis, Value *out);
/* Non-blocking: 1 = sent, 0 = full or closed (channel_try_send distinguishes the two so
 * interpreter.c can raise on closed but return `false` on full, same reasoning `channel_send`
 * has for `send` vs a plain runtime error). `needs_receive`: same meaning as `channel_send`'s own. */
int channel_try_send_status(ChannelObj *ch, Value v, int needs_receive); /* 0 = sent, 1 = full, 2 = closed */
int channel_try_receive(ChannelObj *ch, Value *out);
int channel_length(ChannelObj *ch);
/* No more values will ever be sent. Idempotent (closing twice is harmless, unlike Go's channels).
 * Wakes every blocked send/receive so none of them wait forever for something that will never
 * come. */
void channel_close(ChannelObj *ch);

/* Counts the threads that could still send on a channel, so that a thread blocked in `receive` can
 * tell the difference between "nobody has sent yet" and "nobody ever will". The interpreter calls
 * these around a `spawn`: started before pthread_create, so a thread is counted before it can run
 * (counting it from inside the new thread would leave a window in which it is live but invisible, and
 * a deadlock would be declared while it was still starting up), and ended on the thread's way out as
 * well as when pthread_create itself fails. The main thread is counted from the start. See value.c's
 * deadlock-detection comment. */
void ziman_channel_thread_started(void);
void ziman_channel_thread_ended(void);
/* Waits on two channels of possibly different element types at once, whichever gets a value
 * first: *out_first is set to 1 for `a`, 0 for `b`, and *out to the value. Built by polling both
 * with channel_try_receive and a short sleep between rounds -- Ziman's channels are plain
 * userspace mutex+condvar, with no OS-level or interpreter-runtime primitive to block on either
 * of two independent ones at once the way a real `select` needs to -- so this has the same
 * unbounded-wait characteristic `channel_receive` already has (if both channels are closed and
 * stay empty forever, this polls forever) plus a small, bounded scheduling latency (up to the
 * poll interval) that a from-scratch runtime-level select would not have. */
void channel_select2(ChannelObj *a, ChannelObj *b, int *out_first, Value *out);

int value_equals(Value a, Value b);

/* Equality as a collection key, and as `contains` asks it: the same comparison value_equals makes
 * except that NaN equals NaN. Used by mapobj.c's own key lookup and by `contains`, so that a float
 * key can be found again after it is stored and so that `compare`, which already answers 0 for two
 * NaNs, agrees with the collections. `==`/`!=` keep value_equals and stay IEEE. See value.c's
 * TY_FLOAT case for the full reasoning. */
int value_key_equals(Value a, Value b);

/* Renders a value the way `print` displays it (arrays as `[e1, e2, ...]`,
 * recursively). Returns a heap-allocated, NUL-terminated string owned by
 * the caller. */
char *value_display(Value v);

/* A handle type's own renderer, so that it prints as the value it is building rather than as the
 * one-int struct it is. `nested` is set when it sits inside another value, where it must render a
 * summary -- that is what keeps a cycle unfollowable. See the registry in value.c.
 * `value_display_nested` is what a renderer calls for its contents. */
void value_register_handle_display(const char *type_name, char *(*render)(Value v, int nested));
char *value_display_nested(Value v);

/* The shortest decimal text that reads back as the same double, in plain notation for
 * magnitudes from 1e-7 up to 1e21 and in scientific notation (1.5e+300) outside that;
 * "nan"/"inf"/"-inf" for the non-finite values. Shared by value_display's TY_FLOAT case
 * (str/print/format) and number.float_to_text -- see docs/DESIGN.md's entry on the two
 * no longer disagreeing. Whole-number magnitudes come back with no '.' or 'e' at all
 * ("10", not "10.0"): float_to_text's own contract, unchanged; value_display adds its
 * own ".0" on top when it wants a float to always look like one. Returns a
 * heap-allocated, NUL-terminated string owned by the caller. */
char *float_shortest_text(double d);

#endif
