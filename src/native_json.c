#define _POSIX_C_SOURCE 200809L
#include "native_json.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "iter.h"
#include "mapobj.h"
#include "native.h"

/* The `json` core module's parser and serializer, natively: json.parse_native(text, proto,
 * strict) and json.to_string_native(value). `proto` is any Json value; the values built here
 * take their enum name and type from it (a native cannot know the loader's qualified name for
 * `Json`). Error messages are the ones the module has always produced; positions are byte
 * offsets. See lib/core/json.zm. */

enum { MAX_DEPTH = 512 };

typedef struct {
    const char *s;
    size_t n;
    size_t p;
    int strict;
    Type json_type;
    char *enum_name;
    char err[320];
    int failed;
} Parser;

static Value make_json(Parser *ps, const char *variant, int has_payload, Value payload) {
    return value_enum(ps->json_type, enum_obj_new(ps->enum_name, (char *)variant, has_payload, payload));
}

static void fail(Parser *ps, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(Parser *ps, const char *fmt, ...) {
    if (ps->failed) return;
    ps->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->err, sizeof ps->err, fmt, ap);
    va_end(ap);
}

static void skip_ws(Parser *ps) {
    while (ps->p < ps->n) {
        char c = ps->s[ps->p];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *s, size_t n, size_t pos) {
    if (pos + 4 > n) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_value(s[pos + i]);
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

static size_t put_utf8(char *out, unsigned cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* A string starting at the opening quote; returns a malloc'd NUL-terminated buffer (and its
 * length) or NULL after recording an error. */
static char *parse_string_raw(Parser *ps, size_t *out_len) {
    const char *s = ps->s;
    size_t p = ps->p + 1;
    /* the decoded text is never longer than the raw text up to the closing quote */
    size_t end = p;
    while (end < ps->n && s[end] != '"') end += s[end] == '\\' ? 2 : 1;
    if (end > ps->n) end = ps->n;
    char *out = malloc(end - p + 8);
    size_t o = 0;
    while (1) {
        if (p >= ps->n) { free(out); fail(ps, "unterminated string"); return NULL; }
        char c = s[p];
        if (c == '"') { ps->p = p + 1; out[o] = '\0'; *out_len = o; return out; }
        if (c != '\\') { out[o++] = c; p++; continue; }
        if (p + 1 >= ps->n) { free(out); fail(ps, "unterminated escape sequence"); return NULL; }
        char e = s[p + 1];
        switch (e) {
            case '"': out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/'; break;
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case 'u': {
                int hi = hex4(s, ps->n, p + 2);
                if (hi < 0) { free(out); fail(ps, "invalid \\u escape at position %zu", p); return NULL; }
                unsigned cp = (unsigned)hi;
                size_t next = p + 6;
                if (hi >= 0xD800 && hi <= 0xDBFF) {
                    int lo = (next + 1 < ps->n && s[next] == '\\' && s[next + 1] == 'u') ? hex4(s, ps->n, next + 2) : -1;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((unsigned)(hi - 0xD800) << 10) + (unsigned)(lo - 0xDC00); next += 6; }
                    else { free(out); fail(ps, "unpaired surrogate in \\u escape at position %zu", p); return NULL; }
                } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                    free(out); fail(ps, "unpaired surrogate in \\u escape at position %zu", p); return NULL;
                } else if (hi == 0) {
                    free(out); fail(ps, "\\u0000 is not supported (a string cannot contain a 0 byte), at position %zu", p); return NULL;
                }
                o += put_utf8(out + o, cp);
                p = next;
                continue;
            }
            default:
                free(out);
                fail(ps, "unsupported escape '\\%c' at position %zu", e, p);
                return NULL;
        }
        p += 2;
    }
}

static Value parse_value(Parser *ps, int depth);

static int is_digit_c(char c) { return c >= '0' && c <= '9'; }

static Value parse_number(Parser *ps) {
    const char *s = ps->s;
    size_t pos = ps->p, p = pos;
    int whole = 1;
    if (p < ps->n && s[p] == '-') p++;
    size_t int_start = p;
    while (p < ps->n && is_digit_c(s[p])) p++;
    if (p == int_start) { fail(ps, "invalid number at position %zu", pos); return value_int(0); }
    if (p < ps->n && s[p] == '.') {
        whole = 0;
        p++;
        size_t frac = p;
        while (p < ps->n && is_digit_c(s[p])) p++;
        if (p == frac) { fail(ps, "invalid number at position %zu", pos); return value_int(0); }
    }
    if (p < ps->n && (s[p] == 'e' || s[p] == 'E')) {
        whole = 0;
        p++;
        if (p < ps->n && (s[p] == '+' || s[p] == '-')) p++;
        size_t ex = p;
        while (p < ps->n && is_digit_c(s[p])) p++;
        if (p == ex) { fail(ps, "invalid number at position %zu", pos); return value_int(0); }
    }
    char *text = malloc(p - pos + 1);
    memcpy(text, s + pos, p - pos);
    text[p - pos] = '\0';
    ps->p = p;
    if (whole) {
        errno = 0;
        char *end;
        long long v = strtoll(text, &end, 10);
        if (errno == 0 && *end == '\0') { free(text); return make_json(ps, "JInt", 1, value_int((int64_t)v)); }
    }
    errno = 0;
    char *end;
    double d = strtod(text, &end);
    if (*end != '\0') { fail(ps, "invalid number '%s' at position %zu", text, pos); free(text); return value_int(0); }
    free(text);
    return make_json(ps, "JNumber", 1, value_float(d));
}

static Value parse_array(Parser *ps, int depth) {
    size_t pos = ps->p;
    (void)pos;
    ps->p++;
    skip_ws(ps);
    size_t cap = 8, n = 0;
    Value *items = malloc(cap * sizeof(Value));
    Type arr_type = type_array(ps->json_type);
    if (ps->p < ps->n && ps->s[ps->p] == ']') {
        ps->p++;
        return make_json(ps, "JArray", 1, value_array(arr_type, array_obj_new(items, 0)));
    }
    while (1) {
        Value v = parse_value(ps, depth + 1);
        if (ps->failed) return value_int(0);
        if (n == cap) { cap *= 2; items = realloc(items, cap * sizeof(Value)); }
        items[n++] = v;
        skip_ws(ps);
        if (ps->p >= ps->n) { fail(ps, "unterminated array"); return value_int(0); }
        char c = ps->s[ps->p];
        if (c == ',') { ps->p++; skip_ws(ps); }
        else if (c == ']') { ps->p++; return make_json(ps, "JArray", 1, value_array(arr_type, array_obj_new(items, (int)n))); }
        else { fail(ps, "expected ',' or ']' at position %zu", ps->p); return value_int(0); }
    }
}

static Value parse_object(Parser *ps, int depth) {
    ps->p++;
    skip_ws(ps);
    MapObj *m = mapobj_mut_new();
    Type map_type = type_map(type_scalar(TY_STRING), ps->json_type);
    if (ps->p < ps->n && ps->s[ps->p] == '}') {
        ps->p++;
        return make_json(ps, "JObject", 1, value_map(map_type, m));
    }
    while (1) {
        if (ps->p >= ps->n || ps->s[ps->p] != '"') { fail(ps, "expected string key at position %zu", ps->p); return value_int(0); }
        size_t key_pos = ps->p, klen = 0;
        char *key = parse_string_raw(ps, &klen);
        if (key == NULL) return value_int(0);
        Value kv = value_string_view(key, klen, 1);
        if (ps->strict) {
            Value ignored;
            if (mapobj_get(m, kv, &ignored)) { fail(ps, "duplicate key '%s' at position %zu", key, key_pos); return value_int(0); }
        }
        skip_ws(ps);
        if (ps->p >= ps->n || ps->s[ps->p] != ':') { fail(ps, "expected ':' at position %zu", ps->p); return value_int(0); }
        ps->p++;
        skip_ws(ps);
        Value v = parse_value(ps, depth + 1);
        if (ps->failed) return value_int(0);
        mapobj_set_mut(m, kv, v, 0);
        skip_ws(ps);
        if (ps->p >= ps->n) { fail(ps, "unterminated object"); return value_int(0); }
        char c = ps->s[ps->p];
        if (c == ',') { ps->p++; skip_ws(ps); }
        else if (c == '}') { ps->p++; return make_json(ps, "JObject", 1, value_map(map_type, m)); }
        else { fail(ps, "expected ',' or a closing brace at position %zu", ps->p); return value_int(0); }
    }
}

static int match_literal(Parser *ps, const char *lit) {
    size_t len = strlen(lit);
    if (ps->p + len <= ps->n && memcmp(ps->s + ps->p, lit, len) == 0) { ps->p += len; return 1; }
    fail(ps, "expected '%s' at position %zu", lit, ps->p);
    return 0;
}

static Value parse_value(Parser *ps, int depth) {
    skip_ws(ps);
    if (ps->p >= ps->n) { fail(ps, "unexpected end of input"); return value_int(0); }
    if (depth > MAX_DEPTH) { fail(ps, "nesting is too deep at position %zu", ps->p); return value_int(0); }
    char c = ps->s[ps->p];
    if (c == '{') return parse_object(ps, depth);
    if (c == '[') return parse_array(ps, depth);
    if (c == '"') {
        size_t len = 0;
        char *str = parse_string_raw(ps, &len);
        if (str == NULL) return value_int(0);
        return make_json(ps, "JString", 1, value_string_view(str, len, 1));
    }
    if (c == 't' || c == 'f') {
        int is_true = c == 't';
        if (!match_literal(ps, is_true ? "true" : "false")) return value_int(0);
        return make_json(ps, "JBool", 1, value_bool(is_true));
    }
    if (c == 'n') {
        if (!match_literal(ps, "null")) return value_int(0);
        return make_json(ps, "JNull", 0, value_int(0));
    }
    if (c == '-' || is_digit_c(c)) return parse_number(ps);
    fail(ps, "unexpected character '%c' at position %zu", c, ps->p);
    return value_int(0);
}

/* Parses `n` bytes at `text` as one JSON value: Result<Json, string>. `sample` is any Json value,
 * which tells the parser the enum's type and qualified name. */
static Value parse_json_text(const char *text, size_t n, Value sample, int strict) {
    Parser ps;
    memset(&ps, 0, sizeof ps);
    ps.s = text;
    ps.n = n;
    ps.json_type = sample.type;
    ps.enum_name = sample.as.en->enum_name;
    ps.strict = strict;
    Type string_type = type_scalar(TY_STRING);
    Type result_type = type_result(ps.json_type, string_type);
    Value v = parse_value(&ps, 0);
    if (!ps.failed) {
        skip_ws(&ps);
        if (ps.p != ps.n) fail(&ps, "trailing content at position %zu", ps.p);
    }
    if (ps.failed) return value_result(result_type, result_obj_new(0, value_string(strdup(ps.err))));
    return value_result(result_type, result_obj_new(1, v));
}

static Value native_json_parse(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return parse_json_text(args[0].as.str.ptr, args[0].as.str.len, args[1], args[2].as.b);
}

/* ndjson.records_raw(path, sample): a lazy iterator over the JSON values of a newline-delimited file,
 * each item a Result<Json, string>; blank lines are skipped and a bad line is an Err naming its number. */
typedef struct {
    LineSource *lines;
    Value sample;
} RecordsState;

static int records_next(IterObj *it, Value *out) {
    RecordsState *st = it->state;
    Type str = type_scalar(TY_STRING);
    for (;;) {
        const char *line;
        size_t len;
        int64_t number;
        char err[256];
        int r = line_source_next(st->lines, &line, &len, &number, err, sizeof err);
        if (r == 0) return 0;
        Type json_type = st->sample.type;
        if (r < 0) {
            *out = value_result(type_result(json_type, str), result_obj_new(0, value_string(strdup(err))));
            return 1;
        }
        size_t a = 0;
        while (a < len && (line[a] == ' ' || line[a] == '\t')) a++;
        if (a == len) continue;
        Value parsed = parse_json_text(line, len, st->sample, 0);
        if (parsed.as.res->is_ok) {
            *out = parsed;
        } else {
            const char *msg = value_cstr(parsed.as.res->value);
            char buf[400];
            snprintf(buf, sizeof buf, "line %lld: %s", (long long)number, msg);
            *out = value_result(type_result(json_type, str), result_obj_new(0, value_string(strdup(buf))));
        }
        return 1;
    }
}

static void records_release(IterObj *it) {
    RecordsState *st = it->state;
    line_source_close(st->lines);
}

static Value native_ndjson_records_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type str = type_scalar(TY_STRING);
    Type iter_type = type_struct("Iter");
    const char *path = value_cstr(args[0]);
    LineSource *ls = line_source_open(path);
    if (ls == NULL) {
        char msg[1024];
        snprintf(msg, sizeof msg, "cannot read '%s': %s", path, strerror(errno));
        return value_result(type_result(iter_type, str), result_obj_new(0, value_string(strdup(msg))));
    }
    RecordsState *st = calloc(1, sizeof(RecordsState));
    st->lines = ls;
    st->sample = args[1];
    IterObj *it = calloc(1, sizeof(IterObj));
    it->next = records_next;
    it->release = records_release;
    it->state = st;
    return value_result(type_result(iter_type, str), result_obj_new(1, iter_make(it)));
}

/* ---- serializer ---- */

typedef struct {
    char *buf;
    size_t len, cap;
} Out;

static void out_put(Out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        while (o->len + n + 1 > o->cap) o->cap = o->cap ? o->cap * 2 : 256;
        o->buf = realloc(o->buf, o->cap);
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void out_str(Out *o, const char *s) { out_put(o, s, strlen(s)); }

static void put_json_string(Out *o, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    out_put(o, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': out_str(o, "\\\""); break;
            case '\\': out_str(o, "\\\\"); break;
            case '\n': out_str(o, "\\n"); break;
            case '\t': out_str(o, "\\t"); break;
            case '\r': out_str(o, "\\r"); break;
            case '\b': out_str(o, "\\b"); break;
            case '\f': out_str(o, "\\f"); break;
            default:
                if (c < 0x20) { char e[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0}; out_put(o, e, 6); }
                else out_put(o, s + i, 1);
        }
    }
    out_put(o, "\"", 1);
}

/* The shortest text that reads back as the same double. */
static void put_float(Out *o, double d) {
    if (d != d || isinf(d)) { out_str(o, "null"); return; } /* JSON has no NaN or Infinity */
    char buf[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    out_str(o, buf);
}

static void write_json(Out *o, Value j) {
    const char *variant = j.as.en->variant_name;
    Value payload = j.as.en->payload;
    if (strcmp(variant, "JNull") == 0) out_str(o, "null");
    else if (strcmp(variant, "JBool") == 0) out_str(o, payload.as.b ? "true" : "false");
    else if (strcmp(variant, "JInt") == 0) { char b[32]; snprintf(b, sizeof b, "%lld", (long long)payload.as.i); out_str(o, b); }
    else if (strcmp(variant, "JNumber") == 0) put_float(o, payload.as.f);
    else if (strcmp(variant, "JString") == 0) put_json_string(o, payload.as.str.ptr, payload.as.str.len);
    else if (strcmp(variant, "JArray") == 0) {
        out_put(o, "[", 1);
        ArrayObj *a = payload.as.arr;
        for (int i = 0; i < a->count; i++) {
            if (i > 0) out_put(o, ",", 1);
            write_json(o, array_get(a, i));
        }
        out_put(o, "]", 1);
    } else {
        Value *keys, *vals;
        mapobj_entries(payload.as.map, &keys, &vals);
        out_put(o, "{", 1);
        for (int i = 0; i < payload.as.map->count; i++) {
            if (i > 0) out_put(o, ",", 1);
            put_json_string(o, keys[i].as.str.ptr, keys[i].as.str.len);
            out_put(o, ":", 1);
            write_json(o, vals[i]);
        }
        out_put(o, "}", 1);
    }
}

static Value native_json_to_string(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Out o = {0};
    write_json(&o, args[0]);
    char *s = malloc(o.len + 1);
    memcpy(s, o.buf, o.len);
    s[o.len] = '\0';
    free(o.buf);
    return value_string(s);
}

void native_json_register(void) {
    native_register("ndjson::records_raw", native_ndjson_records_raw);
    native_register("json::parse_native", native_json_parse);
    native_register("json::to_string_native", native_json_to_string);
}
