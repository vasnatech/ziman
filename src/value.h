#ifndef ZIMAN_VALUE_H
#define ZIMAN_VALUE_H

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
typedef struct ArrayObj {
    Value *items;
    int count;
} ArrayObj;

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
        char *s; /* heap, NUL-terminated */
        ArrayObj *arr;
        StructObj *st;
        struct ResultObj *res;
        struct EnumObj *en;
        struct ChannelObj *ch;
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
typedef struct ChannelObj {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    Value *items;
    int count;
    int capacity;
} ChannelObj;

Value value_int(int64_t i);
Value value_float(double f);
Value value_bool(int b);
Value value_string(char *s);
Value value_array(Type array_type, ArrayObj *arr);
Value value_struct(Type struct_type, StructObj *st);
Value value_result(Type result_type, ResultObj *res);
Value value_enum(Type enum_type, EnumObj *en);
Value value_channel(Type channel_type, ChannelObj *ch);

ArrayObj *array_obj_new(Value *items, int count);
StructObj *struct_obj_new(char **field_names, Value *fields, int count);
ResultObj *result_obj_new(int is_ok, Value value);
EnumObj *enum_obj_new(char *enum_name, char *variant_name, int has_payload, Value payload);
ChannelObj *channel_obj_new(void);
void channel_send(ChannelObj *ch, Value v);
Value channel_receive(ChannelObj *ch);

int value_equals(Value a, Value b);

/* Renders a value the way `print` displays it (arrays as `[e1, e2, ...]`,
 * recursively). Returns a heap-allocated, NUL-terminated string owned by
 * the caller. */
char *value_display(Value v);

#endif
