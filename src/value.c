#define _POSIX_C_SOURCE 200809L
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bignum.h"
#include "decimal.h"
#include "error.h"

Value value_int(int64_t i) { Value v; v.type = type_scalar(TY_INT); v.as.i = i; return v; }
Value value_float(double f) { Value v; v.type = type_scalar(TY_FLOAT); v.as.f = f; return v; }
Value value_bool(int b) { Value v; v.type = type_scalar(TY_BOOL); v.as.b = b; return v; }
Value value_closure(FunctionDecl *fn, Value *captured, int ncaptured) {
    Value v;
    ClosureObj *c = malloc(sizeof(ClosureObj));
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

Value value_string_copy(const char *ptr, size_t len) {
    char *buf = zalloc_atomic(len + 1);
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
    v.as.bi = malloc(sizeof(BigNum));
    *v.as.bi = n;
    return v;
}

Value value_decimal(Decimal d) {
    Value v;
    v.type = type_scalar(TY_DECIMAL);
    v.as.dec = malloc(sizeof(Decimal));
    *v.as.dec = d;
    return v;
}

Value value_byte(int b) { Value v; v.type = type_scalar(TY_BYTE); v.as.by = b; return v; }

Value value_array(Type array_type, ArrayObj *arr) {
    Value v;
    v.type = array_type;
    v.as.arr = arr;
    return v;
}

ArrayObj *array_obj_new(Value *items, int count) {
    ArrayObj *a = malloc(sizeof(ArrayObj));
    a->boxed_items = items;
    a->bytes = NULL;
    a->count = count;
    return a;
}

ArrayObj *array_obj_new_bytes(uint8_t *bytes, int count) {
    ArrayObj *a = malloc(sizeof(ArrayObj));
    a->boxed_items = NULL;
    a->bytes = bytes;
    a->count = count;
    return a;
}

Value array_get(const ArrayObj *a, int i) {
    if (a->bytes != NULL) return value_byte(a->bytes[i]);
    return a->boxed_items[i];
}

ArrayObj *array_obj_view(const ArrayObj *a, int start, int end) {
    ArrayObj *v = malloc(sizeof(ArrayObj));
    v->boxed_items = a->boxed_items != NULL ? a->boxed_items + start : NULL;
    v->bytes = a->bytes != NULL ? a->bytes + start : NULL;
    v->count = end - start;
    return v;
}

int array_all_bytes(const Value *items, int count) {
    if (count == 0) return 0; /* an empty array stays boxed: nothing to pack, and no element to type it by */
    for (int i = 0; i < count; i++) {
        if (items[i].type.kind != TY_BYTE) return 0;
    }
    return 1;
}

ArrayObj *array_obj_pack_bytes(const Value *items, int count) {
    uint8_t *bytes = zalloc_atomic((size_t)(count > 0 ? count : 1));
    for (int i = 0; i < count; i++) bytes[i] = (uint8_t)items[i].as.by;
    return array_obj_new_bytes(bytes, count);
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

Value value_channel(Type channel_type, ChannelObj *ch) {
    Value v;
    v.type = channel_type;
    v.as.ch = ch;
    return v;
}

ChannelObj *channel_obj_new(int limit) {
    ChannelObj *ch = malloc(sizeof(ChannelObj));
    pthread_mutex_init(&ch->mutex, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    ch->items = NULL;
    ch->head = 0;
    ch->count = 0;
    ch->capacity = 0;
    ch->limit = limit;
    return ch;
}

/* The caller holds the lock. */
static void channel_push_locked(ChannelObj *ch, Value v) {
    if (ch->count == ch->capacity) {
        int new_capacity = ch->capacity == 0 ? 4 : ch->capacity * 2;
        Value *bigger = malloc((size_t)new_capacity * sizeof(Value));
        for (int i = 0; i < ch->count; i++) bigger[i] = ch->items[(ch->head + i) % ch->capacity];
        ch->items = bigger;
        ch->head = 0;
        ch->capacity = new_capacity;
    }
    ch->items[(ch->head + ch->count) % ch->capacity] = v;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
}

static Value channel_pop_locked(ChannelObj *ch) {
    Value v = ch->items[ch->head];
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    return v;
}

void channel_send(ChannelObj *ch, Value v) {
    pthread_mutex_lock(&ch->mutex);
    while (ch->limit > 0 && ch->count >= ch->limit) {
        pthread_cond_wait(&ch->not_full, &ch->mutex);
    }
    channel_push_locked(ch, v);
    pthread_mutex_unlock(&ch->mutex);
}

int channel_try_send(ChannelObj *ch, Value v) {
    pthread_mutex_lock(&ch->mutex);
    int ok = !(ch->limit > 0 && ch->count >= ch->limit);
    if (ok) channel_push_locked(ch, v);
    pthread_mutex_unlock(&ch->mutex);
    return ok;
}

Value channel_receive(ChannelObj *ch) {
    pthread_mutex_lock(&ch->mutex);
    while (ch->count == 0) {
        pthread_cond_wait(&ch->not_empty, &ch->mutex);
    }
    Value v = channel_pop_locked(ch);
    pthread_mutex_unlock(&ch->mutex);
    return v;
}

int channel_try_receive(ChannelObj *ch, Value *out) {
    pthread_mutex_lock(&ch->mutex);
    int ok = ch->count > 0;
    if (ok) *out = channel_pop_locked(ch);
    pthread_mutex_unlock(&ch->mutex);
    return ok;
}

int channel_length(ChannelObj *ch) {
    pthread_mutex_lock(&ch->mutex);
    int n = ch->count;
    pthread_mutex_unlock(&ch->mutex);
    return n;
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
        case TY_STRING: return a.as.str.len == b.as.str.len && memcmp(a.as.str.ptr, b.as.str.ptr, a.as.str.len) == 0;
        case TY_BIGINT: return bignum_compare(*a.as.bi, *b.as.bi) == 0;
        case TY_DECIMAL: return decimal_compare(*a.as.dec, *b.as.dec) == 0;
        case TY_BYTE: return a.as.by == b.as.by;
        case TY_VOID: return 1;
        case TY_ARRAY: {
            ArrayObj *arr_a = a.as.arr;
            ArrayObj *arr_b = b.as.arr;
            if (arr_a->count != arr_b->count) return 0;
            if (arr_a->bytes != NULL && arr_b->bytes != NULL) {
                return memcmp(arr_a->bytes, arr_b->bytes, (size_t)arr_a->count) == 0;
            }
            for (int i = 0; i < arr_a->count; i++) {
                if (!value_equals(array_get(arr_a, i), array_get(arr_b, i))) return 0;
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
        case TY_CHANNEL:
            ziman_error("runtime", 0, 0, "internal error: '=='/'!=' on Channel should have been rejected by the type checker");
            return 0;
        case TY_FUNC:
            ziman_error("runtime", 0, 0, "internal error: '=='/'!=' on a function value should have been rejected by the type checker");
            return 0;
        case TY_TYPEVAR:
            ziman_error("runtime", 0, 0, "internal error: a runtime value should never have an unsubstituted type variable as its type");
            return 0;
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
        case TY_BIGINT:
            return bignum_to_string(*v.as.bi);
        case TY_DECIMAL:
            return decimal_to_string(*v.as.dec);
        case TY_BYTE:
            snprintf(buf, sizeof(buf), "%d", v.as.by);
            return strdup(buf);
        case TY_STRING:
            {
                char *copy = zalloc_atomic((size_t)v.as.str.len + 1);
                memcpy(copy, v.as.str.ptr, v.as.str.len);
                copy[v.as.str.len] = '\0';
                return copy;
            }
        case TY_VOID:
            return strdup("void");
        case TY_ARRAY: {
            ArrayObj *arr = v.as.arr;
            size_t cap = 32, len = 0;
            char *out = zalloc_atomic(cap);
            out[len++] = '[';
            for (int i = 0; i < arr->count; i++) {
                if (i > 0) {
                    if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
                    out[len++] = ',';
                    out[len++] = ' ';
                }
                char *piece = value_display(array_get(arr, i));
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
            char *out = zalloc_atomic(cap);
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
            char *out = zalloc_atomic(len);
            snprintf(out, len, "%s(%s)", tag, inner);
            free(inner);
            return out;
        }
        case TY_ENUM: {
            EnumObj *en = v.as.en;
            if (!en->has_payload) return strdup(en->variant_name);
            char *inner = value_display(en->payload);
            size_t len = strlen(en->variant_name) + 1 + strlen(inner) + 1 + 1;
            char *out = zalloc_atomic(len);
            snprintf(out, len, "%s(%s)", en->variant_name, inner);
            free(inner);
            return out;
        }
        case TY_CHANNEL:
            return strdup("Channel");
        case TY_FUNC: {
            const char *name = v.as.clo->fn->name;
            size_t len = strlen(name) + 12;
            char *out = malloc(len);
            snprintf(out, len, "<function %s>", name);
            return out;
        }
        case TY_TYPEVAR:
            ziman_error("runtime", 0, 0, "internal error: a runtime value should never have an unsubstituted type variable as its type");
            return strdup("");
    }
    return strdup("");
}
