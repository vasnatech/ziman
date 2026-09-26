#define _POSIX_C_SOURCE 200809L
#include "native_crypto.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <crypt.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include "error.h"
#include "native.h"

/* The `crypto` core module's natives, on OpenSSL's libcrypto (already linked for TLS):
 * SHA-1 / SHA-256 / SHA-512, HMAC, PBKDF2, Argon2id, a constant-time comparison, base64 and base64url,
 * and a byte-wise XOR; and bcrypt, from libxcrypt (OpenSSL has none). See lib/core/crypto.zm. Hashing and PBKDF2 are native because a few
 * thousand iterations of a compression function is hopeless in a tree-walking interpreter. */

static Type byte_array_type(void) { return type_array(type_scalar(TY_BYTE)); }

static Value hash_value(const EVP_MD *md, Value data, int line, int col) {
    int n = 0;
    uint8_t *in = array_copy_bytes(data.as.arr, &n);
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    if (EVP_Digest(in, (size_t)n, out, &out_len, md, NULL) != 1) ziman_error("runtime", line, col, "crypto: hashing failed");
    free(in);
    return value_byte_array(out, out_len);
}

static Value native_sha1(Value *args, int argc, int line, int col) {
    (void)argc;
    return hash_value(EVP_sha1(), args[0], line, col);
}

static Value native_sha256(Value *args, int argc, int line, int col) {
    (void)argc;
    return hash_value(EVP_sha256(), args[0], line, col);
}

static Value native_sha384(Value *args, int argc, int line, int col) {
    (void)argc;
    return hash_value(EVP_sha384(), args[0], line, col);
}

static Value native_sha512(Value *args, int argc, int line, int col) {
    (void)argc;
    return hash_value(EVP_sha512(), args[0], line, col);
}

static Value hmac_value(const EVP_MD *md, Value key, Value data, int line, int col) {
    int kn = 0, dn = 0;
    uint8_t *k = array_copy_bytes(key.as.arr, &kn);
    uint8_t *d = array_copy_bytes(data.as.arr, &dn);
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    if (HMAC(md, k, kn, d, (size_t)dn, out, &out_len) == NULL) ziman_error("runtime", line, col, "crypto: HMAC failed");
    free(k);
    free(d);
    return value_byte_array(out, out_len);
}

static Value native_hmac_sha256(Value *args, int argc, int line, int col) {
    (void)argc;
    return hmac_value(EVP_sha256(), args[0], args[1], line, col);
}

static Value native_hmac_sha384(Value *args, int argc, int line, int col) {
    (void)argc;
    return hmac_value(EVP_sha384(), args[0], args[1], line, col);
}

static Value native_hmac_sha512(Value *args, int argc, int line, int col) {
    (void)argc;
    return hmac_value(EVP_sha512(), args[0], args[1], line, col);
}

static Value pbkdf2_value(const EVP_MD *md, const char *name, Value *args, int line, int col) {
    int64_t iterations = args[2].as.i;
    int64_t length = args[3].as.i;
    if (iterations < 1 || iterations > INT_MAX) ziman_error("runtime", line, col, "crypto.%s: iterations must be between 1 and %d, found %lld", name, INT_MAX, (long long)iterations);
    if (length < 0 || length > 1 << 20) ziman_error("runtime", line, col, "crypto.%s: output length must be between 0 and 1048576, found %lld", name, (long long)length);
    int pn = 0, sn = 0;
    uint8_t *pw = array_copy_bytes(args[0].as.arr, &pn);
    uint8_t *salt = array_copy_bytes(args[1].as.arr, &sn);
    unsigned char *out = malloc((size_t)(length > 0 ? length : 1));
    if (PKCS5_PBKDF2_HMAC((const char *)pw, pn, salt, sn, (int)iterations, md, (int)length, out) != 1) {
        ziman_error("runtime", line, col, "crypto.%s failed", name);
    }
    Value v = value_byte_array(out, (size_t)length);
    free(pw);
    free(salt);
    free(out);
    return v;
}

static Value native_pbkdf2_sha256(Value *args, int argc, int line, int col) {
    (void)argc;
    return pbkdf2_value(EVP_sha256(), "pbkdf2_hmac_sha256", args, line, col);
}

static Value native_pbkdf2_sha512(Value *args, int argc, int line, int col) {
    (void)argc;
    return pbkdf2_value(EVP_sha512(), "pbkdf2_hmac_sha512", args, line, col);
}

static Value native_bytes_xor(Value *args, int argc, int line, int col) {
    (void)argc;
    ArrayObj *a = args[0].as.arr, *b = args[1].as.arr;
    if (a->count != b->count) ziman_error("runtime", line, col, "crypto.bytes_xor: lengths differ (%d vs %d)", a->count, b->count);
    uint8_t *out = zalloc_atomic((size_t)(a->count > 0 ? a->count : 1));
    for (int i = 0; i < a->count; i++) out[i] = (uint8_t)(array_get(a, i).as.by ^ array_get(b, i).as.by);
    return value_array(byte_array_type(), array_obj_new_bytes(out, a->count));
}

/* true when both arrays hold the same bytes; the comparison of equal-length arrays takes
 * the same time whatever the contents (the lengths themselves are not secret). */
static Value native_bytes_equal(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    int an = 0, bn = 0;
    uint8_t *a = array_copy_bytes(args[0].as.arr, &an);
    uint8_t *b = array_copy_bytes(args[1].as.arr, &bn);
    int equal = an == bn && CRYPTO_memcmp(a, b, (size_t)an) == 0;
    free(a);
    free(b);
    return value_bool(equal);
}

static const char B64_STANDARD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64_URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static Value base64_encode_with(Value data, const char *alphabet, int pad) {
    int n = 0;
    uint8_t *in = array_copy_bytes(data.as.arr, &n);
    char *out = malloc(((size_t)(n + 2) / 3) * 4 + 1);
    size_t o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = alphabet[(v >> 18) & 63];
        out[o++] = alphabet[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = alphabet[(v >> 6) & 63];
        else if (pad) out[o++] = '=';
        if (i + 2 < n) out[o++] = alphabet[v & 63];
        else if (pad) out[o++] = '=';
    }
    out[o] = '\0';
    free(in);
    return value_string(out);
}

static Value native_base64_encode(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return base64_encode_with(args[0], B64_STANDARD, 1);
}

static Value native_base64url_encode(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return base64_encode_with(args[0], B64_URL, 0);
}

static int alphabet_value(const char *alphabet, char c) {
    const char *p = c != '\0' ? strchr(alphabet, c) : NULL;
    return p != NULL ? (int)(p - alphabet) : -1;
}

static Value base64_error(Type result_type, const char *message) {
    return value_result(result_type, result_obj_new(0, value_string(strdup(message))));
}

/* Strict: no whitespace, no characters outside the alphabet, padding only at the very end.
 * `padded` requires a length that is a multiple of 4 and `=` padding; otherwise (base64url)
 * padding is not allowed and a final group of 2 or 3 characters is accepted. */
static Value base64_decode_with(Value text, const char *alphabet, int padded) {
    Type result_type = type_result(byte_array_type(), type_scalar(TY_STRING));
    const char *s = text.as.str.ptr;
    size_t n = text.as.str.len;
    if (padded && n % 4 != 0) return base64_error(result_type, "invalid base64: length is not a multiple of 4");
    if (!padded && n % 4 == 1) return base64_error(result_type, "invalid base64url: length is not valid");
    unsigned char *out = malloc(n / 4 * 3 + 3);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        size_t group = n - i < 4 ? n - i : 4;
        int v[4] = {0, 0, 0, 0};
        int pad = 0;
        for (size_t k = 0; k < group; k++) {
            if (padded && s[i + k] == '=' && i + 4 == n && k >= 2) { pad++; continue; }
            v[k] = alphabet_value(alphabet, s[i + k]);
            if (v[k] < 0 || pad > 0) {
                free(out);
                return base64_error(result_type, "invalid base64: bad character");
            }
        }
        size_t chars = group - (size_t)pad;
        uint32_t x = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        out[o++] = (unsigned char)(x >> 16);
        if (chars > 2) out[o++] = (unsigned char)(x >> 8);
        if (chars > 3) out[o++] = (unsigned char)x;
    }
    Value bytes = value_byte_array(out, o);
    free(out);
    return value_result(result_type, result_obj_new(1, bytes));
}

static Value native_base64_decode(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return base64_decode_with(args[0], B64_STANDARD, 1);
}

static Value native_base64url_decode(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    return base64_decode_with(args[0], B64_URL, 0);
}

/* ------------------------------------------------------------- password hashing */

static Value string_result_ok(const char *text) {
    return value_result(type_result(type_scalar(TY_STRING), type_scalar(TY_STRING)), result_obj_new(1, value_string(strdup(text))));
}

static Value string_result_err(const char *message) {
    return value_result(type_result(type_scalar(TY_STRING), type_scalar(TY_STRING)), result_obj_new(0, value_string(strdup(message))));
}

static char *copy_cstring(Value v) {
    char *out = malloc((size_t)v.as.str.len + 1);
    memcpy(out, v.as.str.ptr, (size_t)v.as.str.len);
    out[v.as.str.len] = '\0';
    return out;
}

/* bcrypt_native(password, cost): a `$2b$` hash with a fresh random salt, or an Err. */
static Value native_bcrypt(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    int64_t cost = args[1].as.i;
    if (cost < 4 || cost > 31) return string_result_err("bcrypt cost must be between 4 and 31");
    char *password = copy_cstring(args[0]);
    unsigned char rnd[16];
    if (RAND_bytes(rnd, (int)sizeof rnd) != 1) return string_result_err("bcrypt: no random bytes available");
    char setting[64];
    if (crypt_gensalt_rn("$2b$", (unsigned long)cost, (const char *)rnd, (int)sizeof rnd, setting, (int)sizeof setting) == NULL) {
        return string_result_err("bcrypt: could not build a salt");
    }
    struct crypt_data *data = calloc(1, sizeof *data);
    char *hash = crypt_r(password, setting, data);
    Value out = (hash != NULL && hash[0] == '$') ? string_result_ok(hash) : string_result_err("bcrypt: hashing failed");
    OPENSSL_cleanse(data, sizeof *data);
    OPENSSL_cleanse(password, strlen(password));
    free(data);
    free(password);
    return out;
}

/* bcrypt_verify(password, hash): whether `hash` (`$2a$`, `$2b$` or `$2y$`) is the bcrypt hash of `password`. */
static Value native_bcrypt_verify(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    size_t hn = (size_t)args[1].as.str.len;
    if (hn != 60) return value_bool(0);
    char *password = copy_cstring(args[0]);
    char *hash = copy_cstring(args[1]);
    int ok = 0;
    if (hash[0] == '$' && hash[1] == '2') {
        struct crypt_data *data = calloc(1, sizeof *data);
        char *computed = crypt_r(password, hash, data);
        ok = computed != NULL && strlen(computed) == hn && CRYPTO_memcmp(computed, hash, hn) == 0;
        OPENSSL_cleanse(data, sizeof *data);
        free(data);
    }
    OPENSSL_cleanse(password, strlen(password));
    free(password);
    free(hash);
    return value_bool(ok);
}

/* argon2id_raw(password, salt, memory_kib, iterations, parallelism, length): the raw Argon2id output (RFC 9106). */
static Value native_argon2id_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type result_type = type_result(byte_array_type(), type_scalar(TY_STRING));
    int64_t memory = args[2].as.i, iterations = args[3].as.i, lanes = args[4].as.i, length = args[5].as.i;
    if (memory < 8 || memory > (1 << 22) || iterations < 1 || iterations > 1000 || lanes < 1 || lanes > 64 || length < 4 || length > 1024) {
        return value_result(result_type, result_obj_new(0, value_string(strdup("argon2id: parameters out of range"))));
    }
    int pn = 0, sn = 0;
    uint8_t *pw = array_copy_bytes(args[0].as.arr, &pn);
    uint8_t *salt = array_copy_bytes(args[1].as.arr, &sn);
    uint32_t iter = (uint32_t)iterations, lane = (uint32_t)lanes, mem = (uint32_t)memory, threads = 1;
    unsigned char *out = malloc((size_t)length);
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, pw, (size_t)pn),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt, (size_t)sn),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iter),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lane),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &mem),
        OSSL_PARAM_construct_end(),
    };
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "ARGON2ID", NULL);
    EVP_KDF_CTX *ctx = kdf != NULL ? EVP_KDF_CTX_new(kdf) : NULL;
    int ok = ctx != NULL && EVP_KDF_derive(ctx, out, (size_t)length, params) == 1;
    Value v = ok ? value_result(result_type, result_obj_new(1, value_byte_array(out, (size_t)length)))
                 : value_result(result_type, result_obj_new(0, value_string(strdup("argon2id: the derivation failed (a salt of at least 8 bytes is required)"))));
    EVP_KDF_CTX_free(ctx);
    EVP_KDF_free(kdf);
    OPENSSL_cleanse(pw, (size_t)pn);
    free(pw);
    free(salt);
    free(out);
    return v;
}

void native_crypto_register(void) {
    native_register("crypto::sha1", native_sha1);
    native_register("crypto::sha256", native_sha256);
    native_register("crypto::sha384", native_sha384);
    native_register("crypto::sha512", native_sha512);
    native_register("crypto::hmac_sha256", native_hmac_sha256);
    native_register("crypto::hmac_sha384", native_hmac_sha384);
    native_register("crypto::hmac_sha512", native_hmac_sha512);
    native_register("crypto::pbkdf2_hmac_sha256", native_pbkdf2_sha256);
    native_register("crypto::pbkdf2_hmac_sha512", native_pbkdf2_sha512);
    native_register("crypto::bcrypt_native", native_bcrypt);
    native_register("crypto::bcrypt_verify", native_bcrypt_verify);
    native_register("crypto::argon2id_raw", native_argon2id_raw);
    native_register("crypto::bytes_xor", native_bytes_xor);
    native_register("crypto::bytes_equal", native_bytes_equal);
    native_register("crypto::base64_encode", native_base64_encode);
    native_register("crypto::base64_decode", native_base64_decode);
    native_register("crypto::base64url_encode", native_base64url_encode);
    native_register("crypto::base64url_decode", native_base64url_decode);
}
