#ifndef ZIMAN_VALUE_H
#define ZIMAN_VALUE_H

/* Memory for bytes that can never hold a pointer (a string's text): under the
 * collector it is allocated "atomic", so it is never scanned for pointers --
 * cheaper to mark and immune to a stray text byte pattern looking like an
 * address. Without the collector it is plain malloc. */
#ifdef ZIMAN_GC
#include <gc.h>
#define zalloc_atomic(n) GC_MALLOC_ATOMIC(n)
#else
#include <stdlib.h>
#define zalloc_atomic(n) malloc(n)
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
    Type type;
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
struct Arena; /* opaque forward decl -- only a pointer is ever stored here (src/value_copy.c) */

/* A queued message plus, when non-NULL, the dedicated arena (src/value_copy.c's `value_copy_send`)
 * that the sender's own crossing-safe copy of `value` lives in -- a receiving function
 * (channel_receive/_timeout/try_receive/select2, value.c) must run `value_copy_receive` on `value`
 * and then `arena_destroy(owner_arena)` before handing the result to its own caller. NULL for an
 * ordinary `channel_send` (no crossing arena involved at all -- see value_copy.h). */
typedef struct {
    Value value;
    struct Arena *owner_arena;
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

Value value_int(int64_t i);
Value value_unit(void);
Value value_float(double f);
Value value_bool(int b);
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
Value value_byte(int b); /* caller's responsibility to have already range-checked 0-255 */
Value value_array(Type array_type, ArrayObj *arr);
Value value_struct(Type struct_type, StructObj *st);
Value value_result(Type result_type, ResultObj *res);
Value value_enum(Type enum_type, EnumObj *en);
Value value_channel(Type channel_type, ChannelObj *ch);
Value value_map(Type map_or_set_type, struct MapObj *m);
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
ChannelObj *channel_obj_new(int limit);
/* Blocks until sent or the channel is closed. Returns 1 if it was closed (nothing sent), 0 on
 * success -- the caller (interpreter.c) turns 1 into a runtime error, same as `channel_new`'s
 * own capacity check does for its argument.
 *
 * `owner_arena`: NULL for an ordinary send (v is safe to share as-is, e.g. prelude.c's own
 * `guard_finalizer`); otherwise the dedicated arena `value_copy_send` built `v` into, ownership
 * passed to the channel -- if `v` is never actually queued (the channel is already closed), this
 * destroys `owner_arena` itself before returning, so a value nobody will ever receive doesn't leak
 * its own crossing arena. */
int channel_send(ChannelObj *ch, Value v, struct Arena *owner_arena);
/* Blocks until a value arrives or the channel is closed and drained (already-queued values are
 * still delivered after close -- closing only means no *more* will be sent). Returns 1 with
 * *out set, or 0 if closed and empty.
 *
 * Transparently finishes a crossing-safe send (see `channel_send`'s own `owner_arena` comment and
 * value_copy.h): if the popped message carries one, `*out` is `value_copy_receive`'s own
 * independent copy and the arena is destroyed before this returns, never exposed to the caller --
 * every receive-side function behaves identically here, whether or not the specific message it
 * happened to pop needed this at all. */
int channel_receive(ChannelObj *ch, Value *out);
/* Same as channel_receive, but gives up after `millis` milliseconds: 0 either way (timed out, or
 * closed and empty) -- the two aren't distinguished, the same blur a `select` with a timeout
 * case has elsewhere. */
int channel_receive_timeout(ChannelObj *ch, int64_t millis, Value *out);
/* Non-blocking: 1 = sent, 0 = full or closed (channel_try_send distinguishes the two so
 * interpreter.c can raise on closed but return `false` on full, same reasoning `channel_send`
 * has for `send` vs a plain runtime error). `owner_arena`: same meaning as `channel_send`'s own --
 * destroyed here directly whenever the status isn't 0 (sent), for the identical "don't leak a
 * crossing arena nobody will ever receive" reason. */
int channel_try_send_status(ChannelObj *ch, Value v, struct Arena *owner_arena); /* 0 = sent, 1 = full, 2 = closed */
int channel_try_receive(ChannelObj *ch, Value *out);
int channel_length(ChannelObj *ch);
/* No more values will ever be sent. Idempotent (closing twice is harmless, unlike Go's channels).
 * Wakes every blocked send/receive so none of them wait forever for something that will never
 * come. */
void channel_close(ChannelObj *ch);
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

/* Renders a value the way `print` displays it (arrays as `[e1, e2, ...]`,
 * recursively). Returns a heap-allocated, NUL-terminated string owned by
 * the caller. */
char *value_display(Value v);

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
