#ifndef ZIMAN_VALUE_H
#define ZIMAN_VALUE_H

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

struct Value {
    Type type;
    union {
        int64_t i;
        double f;
        int b; /* 0 or 1 */
        char *s; /* heap, NUL-terminated */
        ArrayObj *arr;
    } as;
};

Value value_int(int64_t i);
Value value_float(double f);
Value value_bool(int b);
Value value_string(char *s);
Value value_array(Type array_type, ArrayObj *arr);

ArrayObj *array_obj_new(Value *items, int count);

int value_equals(Value a, Value b);

/* Renders a value the way `print` displays it (arrays as `[e1, e2, ...]`,
 * recursively). Returns a heap-allocated, NUL-terminated string owned by
 * the caller. */
char *value_display(Value v);

#endif
