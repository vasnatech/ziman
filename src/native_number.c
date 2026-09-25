#define _POSIX_C_SOURCE 200809L
#include "native_number.h"

#include <stdint.h>
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

void native_number_register(void) {
    native_register("number::f64_from_bytes", native_f64_from_bytes);
    native_register("number::f32_from_bytes", native_f32_from_bytes);
    native_register("number::f64_to_bytes", native_f64_to_bytes);
    native_register("number::bytes_to_hex", native_bytes_to_hex);
    native_register("number::bytes_from_hex", native_bytes_from_hex);
}
