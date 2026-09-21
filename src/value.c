#define _POSIX_C_SOURCE 200809L
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

Value value_int(int64_t i) { Value v; v.type = type_scalar(TY_INT); v.as.i = i; return v; }
Value value_float(double f) { Value v; v.type = type_scalar(TY_FLOAT); v.as.f = f; return v; }
Value value_bool(int b) { Value v; v.type = type_scalar(TY_BOOL); v.as.b = b; return v; }
Value value_string(char *s) { Value v; v.type = type_scalar(TY_STRING); v.as.s = s; return v; }

Value value_array(Type array_type, ArrayObj *arr) {
    Value v;
    v.type = array_type;
    v.as.arr = arr;
    return v;
}

ArrayObj *array_obj_new(Value *items, int count) {
    ArrayObj *a = malloc(sizeof(ArrayObj));
    a->items = items;
    a->count = count;
    return a;
}

Value value_struct(Type struct_type, StructObj *st) {
    Value v;
    v.type = struct_type;
    v.as.st = st;
    return v;
}

StructObj *struct_obj_new(char **field_names, Value *fields, int count) {
    StructObj *s = malloc(sizeof(StructObj));
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

ResultObj *result_obj_new(int is_ok, Value value) {
    ResultObj *r = malloc(sizeof(ResultObj));
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

EnumObj *enum_obj_new(char *enum_name, char *variant_name, int has_payload, Value payload) {
    EnumObj *e = malloc(sizeof(EnumObj));
    e->enum_name = enum_name;
    e->variant_name = variant_name;
    e->has_payload = has_payload;
    e->payload = payload;
    return e;
}

int value_equals(Value a, Value b) {
    /* Callers only compare same-typed values, and only when the type checker
     * (type_supports_equality in typecheck.c) has already established that
     * type is comparable: arrays only for a scalar element type, a struct
     * only when every field is (recursively), a Result only when both its
     * Ok and Err types are (recursively), and a user-declared enum only
     * when every payload-carrying variant's payload type is. */
    switch (a.type.kind) {
        case TY_INT: return a.as.i == b.as.i;
        case TY_FLOAT: return a.as.f == b.as.f;
        case TY_BOOL: return a.as.b == b.as.b;
        case TY_STRING: return strcmp(a.as.s, b.as.s) == 0;
        case TY_VOID: return 1;
        case TY_ARRAY: {
            ArrayObj *arr_a = a.as.arr;
            ArrayObj *arr_b = b.as.arr;
            if (arr_a->count != arr_b->count) return 0;
            for (int i = 0; i < arr_a->count; i++) {
                if (!value_equals(arr_a->items[i], arr_b->items[i])) return 0;
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
                if (!value_equals(st_a->fields[i], st_b->fields[i])) return 0;
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
            return value_equals(res_a->value, res_b->value);
        }
        case TY_ENUM: {
            /* Same reasoning as Result: different variants are never
             * equal regardless of payload. */
            EnumObj *en_a = a.as.en;
            EnumObj *en_b = b.as.en;
            if (strcmp(en_a->variant_name, en_b->variant_name) != 0) return 0;
            if (!en_a->has_payload) return 1;
            return value_equals(en_a->payload, en_b->payload);
        }
    }
    return 0;
}

char *value_display(Value v) {
    char buf[64];
    switch (v.type.kind) {
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
        case TY_ARRAY: {
            ArrayObj *arr = v.as.arr;
            size_t cap = 32, len = 0;
            char *out = malloc(cap);
            out[len++] = '[';
            for (int i = 0; i < arr->count; i++) {
                if (i > 0) {
                    if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
                    out[len++] = ',';
                    out[len++] = ' ';
                }
                char *piece = value_display(arr->items[i]);
                size_t piece_len = strlen(piece);
                while (len + piece_len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
                memcpy(out + len, piece, piece_len);
                len += piece_len;
                free(piece);
            }
            if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[len++] = ']';
            out[len] = '\0';
            return out;
        }
        case TY_STRUCT: {
            StructObj *st = v.as.st;
            const char *type_display_name = v.type.struct_name;
            size_t cap = 32, len = 0;
            char *out = malloc(cap);
            size_t name_len = strlen(type_display_name);
            while (len + name_len + 3 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + len, type_display_name, name_len);
            len += name_len;
            out[len++] = ' ';
            out[len++] = '{';
            out[len++] = ' ';
            for (int i = 0; i < st->count; i++) {
                if (i > 0) {
                    if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
                    out[len++] = ',';
                    out[len++] = ' ';
                }
                const char *field_name = st->field_names[i];
                char *piece = value_display(st->fields[i]);
                size_t field_len = strlen(field_name), piece_len = strlen(piece);
                while (len + field_len + piece_len + 4 > cap) { cap *= 2; out = realloc(out, cap); }
                memcpy(out + len, field_name, field_len);
                len += field_len;
                out[len++] = ':';
                out[len++] = ' ';
                memcpy(out + len, piece, piece_len);
                len += piece_len;
                free(piece);
            }
            if (len + 3 > cap) { cap *= 2; out = realloc(out, cap); }
            out[len++] = ' ';
            out[len++] = '}';
            out[len] = '\0';
            return out;
        }
        case TY_RESULT: {
            const char *tag = v.as.res->is_ok ? "Ok" : "Err";
            char *inner = value_display(v.as.res->value);
            size_t len = strlen(tag) + 1 + strlen(inner) + 1 + 1;
            char *out = malloc(len);
            snprintf(out, len, "%s(%s)", tag, inner);
            free(inner);
            return out;
        }
        case TY_ENUM: {
            EnumObj *en = v.as.en;
            if (!en->has_payload) return strdup(en->variant_name);
            char *inner = value_display(en->payload);
            size_t len = strlen(en->variant_name) + 1 + strlen(inner) + 1 + 1;
            char *out = malloc(len);
            snprintf(out, len, "%s(%s)", en->variant_name, inner);
            free(inner);
            return out;
        }
    }
    return strdup("");
}
