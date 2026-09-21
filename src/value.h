#ifndef ZIMAN_VALUE_H
#define ZIMAN_VALUE_H

#include <stdint.h>

#include "ast.h"

typedef struct {
    Type type;
    union {
        int64_t i;
        double f;
        int b; /* 0 or 1 */
        char *s; /* heap, NUL-terminated */
    } as;
} Value;

Value value_int(int64_t i);
Value value_float(double f);
Value value_bool(int b);
Value value_string(char *s);

int value_equals(Value a, Value b);

/* Renders a value the way `print` displays it. Returns a heap-allocated,
 * NUL-terminated string owned by the caller. */
char *value_display(Value v);

#endif
