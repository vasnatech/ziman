#define _POSIX_C_SOURCE 200809L
#include "native_number.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"

/* The `number` core module's natives: the little-endian IEEE-754 floats MySQL's binary
 * protocol carries, and hex text for byte arrays. See lib/core/number.zm. */

static Value native_f64_from_bytes(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *arr = args[0].as.arr;
    int64_t off = args[1].as.i;
    if (off < 0 || off + 8 > arr->count) ziman_error("runtime", line, col, "f64_from_bytes: offset %lld out of range", (long long)off);
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--) bits = (bits << 8) | array_get(arr, (int)(off + i)).as.by;
    double d;
    memcpy(&d, &bits, sizeof d);
    return value_float(d);
}

static Value native_f32_from_bytes(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *arr = args[0].as.arr;
    int64_t off = args[1].as.i;
    if (off < 0 || off + 4 > arr->count) ziman_error("runtime", line, col, "f32_from_bytes: offset %lld out of range", (long long)off);
    uint32_t bits = 0;
    for (int i = 3; i >= 0; i--) bits = (bits << 8) | array_get(arr, (int)(off + i)).as.by;
    float f;
    memcpy(&f, &bits, sizeof f);
    return value_float((double)f);
}

static Value native_f64_to_bytes(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    double d = args[0].as.f;
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    unsigned char out[8];
    for (int i = 0; i < 8; i++) out[i] = (unsigned char)(bits >> (8 * i));
    return value_byte_array(out, 8);
}

static Value native_bytes_to_hex(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    int n = 0;
    uint8_t *in = array_copy_bytes(args[0].as.arr, &n);
    static const char digits[] = "0123456789abcdef";
    char *out = malloc((size_t)n * 2 + 1);
    for (int i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
    free(in);
    return value_string(out);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static Value native_bytes_from_hex(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type result_type = type_result(type_array(type_scalar(TY_BYTE)), type_scalar(TY_STRING));
    const char *s = args[0].as.str.ptr;
    size_t n = args[0].as.str.len;
    if (n % 2 != 0) return value_result(result_type, result_obj_new(0, value_string(strdup("invalid hex: odd number of digits"))));
    unsigned char *out = malloc(n / 2 + 1);
    for (size_t i = 0; i < n; i += 2) {
        int hi = hex_nibble(s[i]), lo = hex_nibble(s[i + 1]);
        if (hi < 0 || lo < 0) {
            free(out);
            return value_result(result_type, result_obj_new(0, value_string(strdup("invalid hex: bad digit"))));
        }
        out[i / 2] = (unsigned char)(hi * 16 + lo);
    }
    Value bytes = value_byte_array(out, n / 2);
    free(out);
    return value_result(result_type, result_obj_new(1, bytes));
}

/* ---- float text, hex of ints and bytes, big-endian integers -------------------------- */
static Value err_result_of(Type ok_type, const char *message) {
    return value_result(type_result(ok_type, type_scalar(TY_STRING)), result_obj_new(0, value_string(strdup(message))));
}

/* The shortest decimal text that reads back as the same double, in plain notation for
 * magnitudes from 1e-7 up to 1e21 and in scientific notation (1.5e+300) outside that. */
static Value native_float_to_text(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    double d = args[0].as.f;
    if (d != d) return value_string(strdup("nan"));
    if (isinf(d)) return value_string(strdup(d < 0 ? "-inf" : "inf"));
    if (d == 0) return value_string(strdup(signbit(d) ? "-0" : "0"));
    char e[48];
    for (int p = 0; p <= 16; p++) {
        snprintf(e, sizeof e, "%.*e", p, d);
        if (strtod(e, NULL) == d) break;
    }
    /* e is like "-1.2345e+05": sign, digit, optional ".digits", e, exponent */
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
    return value_string(strdup(out));
}

static int hex_digit_at(char c) { return hex_nibble(c); }

/* int_to_hex(x): lowercase hex; a negative int is its 64-bit two's complement (16 digits). */
static Value native_int_to_hex(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    char buf[24];
    snprintf(buf, sizeof buf, "%llx", (unsigned long long)args[0].as.i);
    return value_string(strdup(buf));
}

static Value native_int_from_hex(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type it = type_scalar(TY_INT);
    const char *s = args[0].as.str.ptr;
    size_t n = args[0].as.str.len;
    if (n == 0) return err_result_of(it, "invalid int hex: empty string");
    if (n > 16) {
        char msg[200];
        snprintf(msg, sizeof msg, "invalid int hex '%.*s': more than 16 hex digits (64 bits)", (int)n, s);
        return err_result_of(it, msg);
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d = hex_digit_at(s[i]);
        if (d < 0) {
            char msg[64];
            snprintf(msg, sizeof msg, "invalid hex digit '%c'", s[i]);
            return err_result_of(it, msg);
        }
        v = v * 16 + (uint64_t)d;
    }
    return value_result(type_result(it, type_scalar(TY_STRING)), result_obj_new(1, value_int((int64_t)v)));
}

static Value native_byte_to_hex(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    char buf[3];
    snprintf(buf, sizeof buf, "%02x", args[0].as.by);
    return value_string(strdup(buf));
}

static Value native_byte_from_hex(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type bt = type_scalar(TY_BYTE);
    const char *s = args[0].as.str.ptr;
    if (args[0].as.str.len != 2) {
        char msg[200];
        snprintf(msg, sizeof msg, "invalid byte hex '%.*s': expected exactly 2 hex digits", (int)args[0].as.str.len, s);
        return err_result_of(bt, msg);
    }
    int hi = hex_digit_at(s[0]), lo = hex_digit_at(s[1]);
    if (hi < 0 || lo < 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "invalid hex digit '%c'", hi < 0 ? s[0] : s[1]);
        return err_result_of(bt, msg);
    }
    return value_result(type_result(bt, type_scalar(TY_STRING)), result_obj_new(1, value_byte(hi * 16 + lo)));
}

static Value be_to_bytes(Value n, int width, const char *name, int line, int col) {
    int64_t v = n.as.i;
    int64_t max = width == 2 ? 65535 : 4294967295LL;
    if (v < 0 || v > max) ziman_error("runtime", line, col, "%s: %lld is out of range (0 to %lld)", name, (long long)v, (long long)max);
    unsigned char out[4];
    for (int i = 0; i < width; i++) out[i] = (unsigned char)(v >> (8 * (width - 1 - i)));
    return value_byte_array(out, (size_t)width);
}

static Value be_from_bytes(Value *args, int width, const char *name, int line, int col) {
    ArrayObj *arr = args[0].as.arr;
    int64_t off = args[1].as.i;
    if (off < 0 || off + width > arr->count) ziman_error("runtime", line, col, "%s: offset %lld out of range for an array of length %d", name, (long long)off, arr->count);
    int64_t v = 0;
    for (int i = 0; i < width; i++) v = (v << 8) | array_get(arr, (int)(off + i)).as.by;
    return value_int(v);
}

static Value native_u16_to_bytes(Value *args, int argc, int line, int col) { (void)argc; return be_to_bytes(args[0], 2, "u16_to_bytes", line, col); }
static Value native_u32_to_bytes(Value *args, int argc, int line, int col) { (void)argc; return be_to_bytes(args[0], 4, "u32_to_bytes", line, col); }
static Value native_u16_from_bytes(Value *args, int argc, int line, int col) { (void)argc; return be_from_bytes(args, 2, "u16_from_bytes", line, col); }
static Value native_u32_from_bytes(Value *args, int argc, int line, int col) { (void)argc; return be_from_bytes(args, 4, "u32_from_bytes", line, col); }

void native_number_register(void) {
    native_register("number::f64_from_bytes", native_f64_from_bytes);
    native_register("number::f32_from_bytes", native_f32_from_bytes);
    native_register("number::f64_to_bytes", native_f64_to_bytes);
    native_register("number::bytes_to_hex", native_bytes_to_hex);
    native_register("number::bytes_from_hex", native_bytes_from_hex);
    native_register("number::float_to_text", native_float_to_text);
    native_register("number::int_to_hex", native_int_to_hex);
    native_register("number::int_from_hex", native_int_from_hex);
    native_register("number::byte_to_hex", native_byte_to_hex);
    native_register("number::byte_from_hex", native_byte_from_hex);
    native_register("number::u16_to_bytes", native_u16_to_bytes);
    native_register("number::u32_to_bytes", native_u32_to_bytes);
    native_register("number::u16_from_bytes", native_u16_from_bytes);
    native_register("number::u32_from_bytes", native_u32_from_bytes);
}
