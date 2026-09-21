#define _POSIX_C_SOURCE 200809L
#include "value.h"

#include <stdio.h>
#include <string.h>

Value value_int(int64_t i) { Value v; v.type = TY_INT; v.as.i = i; return v; }
Value value_float(double f) { Value v; v.type = TY_FLOAT; v.as.f = f; return v; }
Value value_bool(int b) { Value v; v.type = TY_BOOL; v.as.b = b; return v; }
Value value_string(char *s) { Value v; v.type = TY_STRING; v.as.s = s; return v; }

int value_equals(Value a, Value b) {
    /* Callers only compare same-typed values -- enforced by the type checker. */
    switch (a.type) {
        case TY_INT: return a.as.i == b.as.i;
        case TY_FLOAT: return a.as.f == b.as.f;
        case TY_BOOL: return a.as.b == b.as.b;
        case TY_STRING: return strcmp(a.as.s, b.as.s) == 0;
        case TY_VOID: return 1;
    }
    return 0;
}

char *value_display(Value v) {
    char buf[64];
    switch (v.type) {
        case TY_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)v.as.i);
            return strdup(buf);
        case TY_FLOAT:
            snprintf(buf, sizeof(buf), "%g", v.as.f);
            return strdup(buf);
        case TY_BOOL:
            return strdup(v.as.b ? "true" : "false");
        case TY_STRING:
            return strdup(v.as.s);
        case TY_VOID:
            return strdup("void");
    }
    return strdup("");
}
