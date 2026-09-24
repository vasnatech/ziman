#define _POSIX_C_SOURCE 200809L
#include "native_crypto.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "native.h"

/* SHA-256 (FIPS 180-4), HMAC (RFC 2104), PBKDF2 (RFC 8018) and base64
 * (RFC 4648), implemented natively rather than in Ziman: PBKDF2 with a
 * few thousand iterations is tens of thousands of compression-function
 * calls, hopeless for a tree-walking interpreter, and SCRAM-SHA-256
 * (Postgres's default authentication) needs exactly that on every
 * connection. Self-contained on purpose -- no libcrypto dependency --
 * and checked against the published test vectors in tests/fixtures/
 * crypto_*. Not constant-time and not hardened: this exists to speak
 * SCRAM to a server, not to be a general-purpose crypto library. */

typedef struct {
    uint32_t h[8];
    uint64_t total; /* bytes hashed so far */
    unsigned char block[64];
    size_t used;
} Sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_compress(Sha256 *s, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(Sha256 *s) {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, init, sizeof(init));
    s->total = 0;
    s->used = 0;
}

static void sha256_update(Sha256 *s, const unsigned char *data, size_t n) {
    s->total += n;
    while (n > 0) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->block + s->used, data, take);
        s->used += take;
        data += take;
        n -= take;
        if (s->used == 64) {
            sha256_compress(s, s->block);
            s->used = 0;
        }
    }
}

static void sha256_final(Sha256 *s, unsigned char out[32]) {
    uint64_t bits = s->total * 8;
    unsigned char pad = 0x80;
    sha256_update(s, &pad, 1);
    unsigned char zero = 0;
    while (s->used != 56) sha256_update(s, &zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(s->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)s->h[i];
    }
}

static void sha256(const unsigned char *data, size_t n, unsigned char out[32]) {
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, out);
}

static void hmac_sha256(const unsigned char *key, size_t key_len, const unsigned char *data, size_t n, unsigned char out[32]) {
    unsigned char k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) sha256(key, key_len, k);
    else memcpy(k, key, key_len);
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    unsigned char inner[32];
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, ipad, 64);
    sha256_update(&s, data, n);
    sha256_final(&s, inner);
    sha256_init(&s);
    sha256_update(&s, opad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

/* ---- Value <-> byte buffer plumbing ---- */

static Type byte_array_type(void) { return type_array(type_scalar(TY_BYTE)); }

static unsigned char *bytes_of(ArrayObj *arr) {
    unsigned char *buf = malloc((size_t)(arr->count > 0 ? arr->count : 1));
    for (int i = 0; i < arr->count; i++) buf[i] = (unsigned char)arr->items[i].as.by;
    return buf;
}

static Value value_of_bytes(const unsigned char *buf, size_t n) {
    Value *items = malloc(sizeof(Value) * (n > 0 ? n : 1));
    for (size_t i = 0; i < n; i++) items[i] = value_byte(buf[i]);
    return value_array(byte_array_type(), array_obj_new(items, (int)n));
}

/* SHA-1 (FIPS 180-4), one shot. Only here because MySQL's
 * mysql_native_password scramble is defined in terms of it -- not for
 * anything that needs collision resistance. */
static void sha1(const unsigned char *data, size_t n, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t padded = ((n + 8) / 64 + 1) * 64;
    unsigned char *m = calloc(padded, 1);
    memcpy(m, data, n);
    m[n] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m[padded - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)m[off + 4 * i] << 24) | ((uint32_t)m[off + 4 * i + 1] << 16) | ((uint32_t)m[off + 4 * i + 2] << 8) | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (unsigned char)(h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(h[i] >> 8);
        out[4 * i + 3] = (unsigned char)h[i];
    }
}

static Value native_sha1(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    unsigned char *in = bytes_of(args[0].as.arr);
    unsigned char out[20];
    sha1(in, (size_t)args[0].as.arr->count, out);
    free(in);
    return value_of_bytes(out, 20);
}

/* bytes.f64_from_bytes(b, offset) / f32_from_bytes: the little-endian
 * IEEE-754 double / float at `offset` (MySQL's binary protocol sends
 * DOUBLE and FLOAT columns that way). */
static Value native_f64_from_bytes(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *arr = args[0].as.arr;
    int64_t off = args[1].as.i;
    if (off < 0 || off + 8 > arr->count) ziman_error("runtime", line, col, "f64_from_bytes: offset %lld out of range", (long long)off);
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--) bits = (bits << 8) | arr->items[off + i].as.by;
    double d;
    memcpy(&d, &bits, sizeof d);
    return value_float(d);
}

static Value native_f64_to_bytes(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    double d = args[0].as.f;
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    unsigned char out[8];
    for (int i = 0; i < 8; i++) out[i] = (unsigned char)(bits >> (8 * i));
    return value_of_bytes(out, 8);
}

static Value native_f32_from_bytes(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *arr = args[0].as.arr;
    int64_t off = args[1].as.i;
    if (off < 0 || off + 4 > arr->count) ziman_error("runtime", line, col, "f32_from_bytes: offset %lld out of range", (long long)off);
    uint32_t bits = 0;
    for (int i = 3; i >= 0; i--) bits = (bits << 8) | arr->items[off + i].as.by;
    float f;
    memcpy(&f, &bits, sizeof f);
    return value_float((double)f);
}

static Value native_sha256(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    unsigned char *in = bytes_of(args[0].as.arr);
    unsigned char out[32];
    sha256(in, (size_t)args[0].as.arr->count, out);
    free(in);
    return value_of_bytes(out, 32);
}

static Value native_hmac_sha256(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    unsigned char *key = bytes_of(args[0].as.arr);
    unsigned char *data = bytes_of(args[1].as.arr);
    unsigned char out[32];
    hmac_sha256(key, (size_t)args[0].as.arr->count, data, (size_t)args[1].as.arr->count, out);
    free(key);
    free(data);
    return value_of_bytes(out, 32);
}

/* pbkdf2_hmac_sha256(password, salt, iterations, length) -> [byte] */
static Value native_pbkdf2(Value *args, int argc, int line, int col) {
    (void)argc;
    int64_t iterations = args[2].as.i;
    int64_t length = args[3].as.i;
    if (iterations < 1) ziman_error("runtime", line, col, "crypto.pbkdf2_hmac_sha256: iterations must be at least 1");
    if (length < 0 || length > 1 << 20) ziman_error("runtime", line, col, "crypto.pbkdf2_hmac_sha256: unreasonable output length %lld", (long long)length);
    unsigned char *pw = bytes_of(args[0].as.arr);
    size_t pw_len = (size_t)args[0].as.arr->count;
    unsigned char *salt = bytes_of(args[1].as.arr);
    size_t salt_len = (size_t)args[1].as.arr->count;

    unsigned char *out = malloc((size_t)(length > 0 ? length : 1));
    unsigned char *salt_i = malloc(salt_len + 4);
    memcpy(salt_i, salt, salt_len);
    size_t produced = 0;
    for (uint32_t block = 1; produced < (size_t)length; block++) {
        salt_i[salt_len] = (unsigned char)(block >> 24);
        salt_i[salt_len + 1] = (unsigned char)(block >> 16);
        salt_i[salt_len + 2] = (unsigned char)(block >> 8);
        salt_i[salt_len + 3] = (unsigned char)block;
        unsigned char u[32], t[32];
        hmac_sha256(pw, pw_len, salt_i, salt_len + 4, u);
        memcpy(t, u, 32);
        for (int64_t it = 1; it < iterations; it++) {
            unsigned char next[32];
            hmac_sha256(pw, pw_len, u, 32, next);
            memcpy(u, next, 32);
            for (int k = 0; k < 32; k++) t[k] ^= u[k];
        }
        size_t take = (size_t)length - produced;
        if (take > 32) take = 32;
        memcpy(out + produced, t, take);
        produced += take;
    }
    Value v = value_of_bytes(out, (size_t)length);
    free(pw); free(salt); free(salt_i); free(out);
    return v;
}

static Value native_bytes_xor(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *a = args[0].as.arr, *b = args[1].as.arr;
    if (a->count != b->count) ziman_error("runtime", line, col, "crypto.bytes_xor: lengths differ (%d vs %d)", a->count, b->count);
    Value *items = malloc(sizeof(Value) * (size_t)(a->count > 0 ? a->count : 1));
    for (int i = 0; i < a->count; i++) items[i] = value_byte(a->items[i].as.by ^ b->items[i].as.by);
    return value_array(byte_array_type(), array_obj_new(items, a->count));
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static Value native_base64_encode(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    unsigned char *in = bytes_of(args[0].as.arr);
    size_t n = (size_t)args[0].as.arr->count;
    char *out = malloc(((n + 2) / 3) * 4 + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    free(in);
    return value_string(out);
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static Value native_base64_decode(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type result_type = type_result(byte_array_type(), type_scalar(TY_STRING));
    const char *s = args[0].as.str.ptr;
    size_t n = args[0].as.str.len;
    if (n % 4 != 0) return value_result(result_type, result_obj_new(0, value_string(strdup("invalid base64: length is not a multiple of 4"))));
    unsigned char *out = malloc(n / 4 * 3 + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            if (s[i + k] == '=' && i + 4 == n && k >= 2) { v[k] = 0; pad++; continue; }
            v[k] = b64_val(s[i + k]);
            if (v[k] < 0 || pad > 0) {
                free(out);
                return value_result(result_type, result_obj_new(0, value_string(strdup("invalid base64: bad character"))));
            }
        }
        uint32_t x = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        out[o++] = (unsigned char)(x >> 16);
        if (pad < 2) out[o++] = (unsigned char)(x >> 8);
        if (pad < 1) out[o++] = (unsigned char)x;
    }
    Value bytes = value_of_bytes(out, o);
    free(out);
    return value_result(result_type, result_obj_new(1, bytes));
}

void native_crypto_register(void) {
    native_register("crypto::sha256", native_sha256);
    native_register("crypto::sha1", native_sha1);
    native_register("bytes::f64_from_bytes", native_f64_from_bytes);
    native_register("bytes::f32_from_bytes", native_f32_from_bytes);
    native_register("bytes::f64_to_bytes", native_f64_to_bytes);
    native_register("crypto::hmac_sha256", native_hmac_sha256);
    native_register("crypto::pbkdf2_hmac_sha256", native_pbkdf2);
    native_register("crypto::bytes_xor", native_bytes_xor);
    native_register("crypto::base64_encode", native_base64_encode);
    native_register("crypto::base64_decode", native_base64_decode);
}
