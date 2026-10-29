#define _POSIX_C_SOURCE 200809L
#include "native_crypto.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <crypt.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include "error.h"
#include "native.h"
#include "xalloc.h"

/* The `crypto` core module's natives, on OpenSSL's libcrypto (already linked for TLS):
 * SHA-1 / SHA-256 / SHA-512, HMAC, PBKDF2, Argon2id, AES-256-GCM, a constant-time comparison, base64 and
 * base64url, a byte-wise XOR and **ECDSA over P-256**; and bcrypt, from libxcrypt (OpenSSL has none). See
 * lib/core/crypto.zm. Hashing
 * and PBKDF2 are native because a few thousand iterations of a compression function is hopeless in a
 * tree-walking interpreter. */

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

static Value native_hmac_sha1(Value *args, int argc, int line, int col) {
    (void)argc;
    return hmac_value(EVP_sha1(), args[0], args[1], line, col);
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
    unsigned char *out = ziman_xmalloc((size_t)(length > 0 ? length : 1));
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
    return value_array(type_ref(byte_array_type()), array_obj_new_bytes(out, a->count));
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
    char *out = ziman_xmalloc(((size_t)(n + 2) / 3) * 4 + 1);
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
    return value_result(type_ref(result_type), result_obj_new(0, value_string(ziman_xstrdup(message))));
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
    unsigned char *out = ziman_xmalloc(n / 4 * 3 + 3);
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
    return value_result(type_ref(result_type), result_obj_new(1, bytes));
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
    return value_result(type_ref(type_result(type_scalar(TY_STRING), type_scalar(TY_STRING))), result_obj_new(1, value_string(ziman_xstrdup(text))));
}

static Value string_result_err(const char *message) {
    return value_result(type_ref(type_result(type_scalar(TY_STRING), type_scalar(TY_STRING))), result_obj_new(0, value_string(ziman_xstrdup(message))));
}

static char *copy_cstring(Value v) {
    char *out = ziman_xmalloc((size_t)v.as.str.len + 1);
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
    struct crypt_data *data = ziman_xcalloc(1, sizeof *data);
    char *hash = crypt_r(password, setting, data);
    Value out = (hash != NULL && hash[0] == '$') ? string_result_ok(hash) : string_result_err("bcrypt: hashing failed");
    OPENSSL_cleanse(data, sizeof *data);
    /* The copied length, not `strlen`: `copy_cstring` copied `args[0].as.str.len` bytes, and a
     * secret containing a NUL would leave its tail in the heap if the wipe stopped at the first
     * one. Wrong on principle whatever the reachability, and A49 only closed the JSON route in
     * (F132). */
    OPENSSL_cleanse(password, (size_t)args[0].as.str.len);
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
        struct crypt_data *data = ziman_xcalloc(1, sizeof *data);
        char *computed = crypt_r(password, hash, data);
        ok = computed != NULL && strlen(computed) == hn && CRYPTO_memcmp(computed, hash, hn) == 0;
        OPENSSL_cleanse(data, sizeof *data);
        free(data);
    }
    OPENSSL_cleanse(password, (size_t)args[0].as.str.len); /* see native_bcrypt */
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
        return value_result(type_ref(result_type), result_obj_new(0, value_string(ziman_xstrdup("argon2id: parameters out of range"))));
    }
    int pn = 0, sn = 0;
    uint8_t *pw = array_copy_bytes(args[0].as.arr, &pn);
    uint8_t *salt = array_copy_bytes(args[1].as.arr, &sn);
    uint32_t iter = (uint32_t)iterations, lane = (uint32_t)lanes, mem = (uint32_t)memory, threads = 1;
    unsigned char *out = ziman_xmalloc((size_t)length);
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
    Value v = ok ? value_result(type_ref(result_type), result_obj_new(1, value_byte_array(out, (size_t)length)))
                 : value_result(type_ref(result_type), result_obj_new(0, value_string(ziman_xstrdup("argon2id: the derivation failed (a salt of at least 8 bytes is required)"))));
    EVP_KDF_CTX_free(ctx);
    EVP_KDF_free(kdf);
    OPENSSL_cleanse(pw, (size_t)pn);
    free(pw);
    free(salt);
    free(out);
    return v;
}

/* ------------------------------------------------------------- authenticated encryption */

static const size_t aes_gcm_nonce_len = 12;
static const size_t aes_gcm_tag_len = 16;

static Value bytes_result_err(Type result_type, const char *message) {
    return value_result(type_ref(result_type), result_obj_new(0, value_string(ziman_xstrdup(message))));
}

/* aes_gcm_encrypt(key, plaintext): AES-256-GCM with a fresh random nonce, returned as
 * `nonce || ciphertext || tag`. An Err only for a wrong key length. */
static Value native_aes_gcm_encrypt(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type result_type = type_result(byte_array_type(), type_scalar(TY_STRING));
    int kn = 0, pn = 0;
    uint8_t *key = array_copy_bytes(args[0].as.arr, &kn);
    uint8_t *plaintext = array_copy_bytes(args[1].as.arr, &pn);
    if (kn != 32) {
        free(key);
        free(plaintext);
        return bytes_result_err(result_type, "aes_gcm_encrypt: key must be exactly 32 bytes");
    }
    unsigned char nonce[12];
    if (RAND_bytes(nonce, (int)aes_gcm_nonce_len) != 1) {
        free(key);
        free(plaintext);
        return bytes_result_err(result_type, "aes_gcm_encrypt: no random bytes available");
    }
    unsigned char *ciphertext = ziman_xmalloc((size_t)(pn > 0 ? pn : 1));
    unsigned char tag[16];
    int len = 0, ciphertext_len = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = ctx != NULL
        && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)aes_gcm_nonce_len, NULL) == 1
        && EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1
        && EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, pn) == 1;
    if (ok) {
        ciphertext_len = len;
        ok = EVP_EncryptFinal_ex(ctx, ciphertext + ciphertext_len, &len) == 1;
        ciphertext_len += len;
        ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)aes_gcm_tag_len, tag) == 1;
    }
    Value v;
    if (ok) {
        size_t total = aes_gcm_nonce_len + (size_t)ciphertext_len + aes_gcm_tag_len;
        unsigned char *out = ziman_xmalloc(total);
        memcpy(out, nonce, aes_gcm_nonce_len);
        memcpy(out + aes_gcm_nonce_len, ciphertext, (size_t)ciphertext_len);
        memcpy(out + aes_gcm_nonce_len + (size_t)ciphertext_len, tag, aes_gcm_tag_len);
        v = value_result(type_ref(result_type), result_obj_new(1, value_byte_array(out, total)));
        free(out);
    } else {
        v = bytes_result_err(result_type, "aes_gcm_encrypt: encryption failed");
    }
    if (ctx != NULL) EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, (size_t)kn);
    free(key);
    free(plaintext);
    free(ciphertext);
    return v;
}

/* aes_gcm_decrypt(key, blob): the reverse of aes_gcm_encrypt, checking the tag. An Err for a wrong key
 * length, a blob too short to hold a nonce and a tag, or a tag that does not match (the wrong key, or the
 * blob was altered). */
static Value native_aes_gcm_decrypt(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type result_type = type_result(byte_array_type(), type_scalar(TY_STRING));
    int kn = 0, bn = 0;
    uint8_t *key = array_copy_bytes(args[0].as.arr, &kn);
    uint8_t *blob = array_copy_bytes(args[1].as.arr, &bn);
    if (kn != 32) {
        free(key);
        free(blob);
        return bytes_result_err(result_type, "aes_gcm_decrypt: key must be exactly 32 bytes");
    }
    if ((size_t)bn < aes_gcm_nonce_len + aes_gcm_tag_len) {
        free(key);
        free(blob);
        return bytes_result_err(result_type, "aes_gcm_decrypt: too short to contain a nonce and a tag");
    }
    unsigned char *nonce = blob;
    unsigned char *ciphertext = blob + aes_gcm_nonce_len;
    int ciphertext_len = (int)((size_t)bn - aes_gcm_nonce_len - aes_gcm_tag_len);
    unsigned char *tag = blob + (size_t)bn - aes_gcm_tag_len;
    unsigned char *plaintext = ziman_xmalloc((size_t)(ciphertext_len > 0 ? ciphertext_len : 1));
    int len = 0, plaintext_len = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = ctx != NULL
        && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)aes_gcm_nonce_len, NULL) == 1
        && EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1
        && EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) == 1;
    if (ok) {
        plaintext_len = len;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)aes_gcm_tag_len, tag) == 1
            && EVP_DecryptFinal_ex(ctx, plaintext + plaintext_len, &len) == 1;
        if (ok) plaintext_len += len;
    }
    Value v = ok
        ? value_result(type_ref(result_type), result_obj_new(1, value_byte_array(plaintext, (size_t)plaintext_len)))
        : bytes_result_err(result_type, "aes_gcm_decrypt: decryption failed (the wrong key, or the data was altered)");
    if (ctx != NULL) EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, (size_t)kn);
    free(key);
    free(blob);
    free(plaintext);
    return v;
}

/* ------------------------------------------------------------- ECDSA over P-256 (ES256)
 *
 * **The first asymmetric signing in the language** (2026-10-29). Everything above this line is
 * symmetric -- a MAC or a cipher under a shared key -- and the thing a shared key cannot do is let
 * someone verify without also being able to sign. That is what a verifiable credential needs, and
 * `jwt`'s ES256 is the first caller.
 *
 * Three decisions, all visible in these signatures:
 *
 * **Keys are PEM text.** It is what `openssl ecparam` writes, what a deployment keeps in a file or
 * a secret, and what OpenSSL parses without help. A JWK is a `Json` object and belongs in Ziman,
 * built from `ecdsa_p256_public_point` -- so the C holds no JSON and the JWK's spelling can change
 * without touching it.
 *
 * **A signature is raw `r || s`, 64 bytes, not DER.** That is what JWS requires (RFC 7515 §3.3),
 * and it is the form that cannot be mistaken for the other: a DER signature's length varies with
 * the integers, so a caller who got the wrong one finds out at the far end. The conversion to and
 * from OpenSSL's DER is done here, in the one place that knows both.
 *
 * **The curve is checked, not assumed.** A P-384 key signs happily and produces 96 bytes; taking
 * the first 64 would be a valid-looking signature of nothing. A key on another curve is refused by
 * name.
 */

static const char *p256_group = "prime256v1";
static const size_t p256_coord_len = 32;

/* Whether `pkey` is an EC key on P-256. Checked for every operation: see the header. */
static int is_p256(EVP_PKEY *pkey) {
    if (pkey == NULL || EVP_PKEY_get_base_id(pkey) != EVP_PKEY_EC) return 0;
    char group[64] = {0};
    size_t len = 0;
    if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, &len) != 1) return 0;
    return strcmp(group, p256_group) == 0 || strcmp(group, "P-256") == 0;
}

/* Reads a PEM private key, or a PEM public key when `private_key` is 0. */
static EVP_PKEY *read_pem(const char *pem, int private_key) {
    BIO *bio = BIO_new_mem_buf(pem, -1);
    if (bio == NULL) return NULL;
    EVP_PKEY *pkey = private_key ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL)
                                 : PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return pkey;
}

/* ecdsa_p256_generate(): a fresh private key as PKCS#8 PEM. */
static Value native_ecdsa_p256_generate(Value *args, int argc, int line, int col) {
    (void)args; (void)argc; (void)line; (void)col;
    EVP_PKEY *pkey = EVP_EC_gen(p256_group);
    if (pkey == NULL) return string_result_err("ecdsa_p256_generate: key generation failed");
    BIO *bio = BIO_new(BIO_s_mem());
    int ok = bio != NULL && PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL) == 1;
    Value v;
    if (ok) {
        char *data = NULL;
        long n = BIO_get_mem_data(bio, &data);
        char *out = ziman_xmalloc((size_t)n + 1);
        memcpy(out, data, (size_t)n);
        out[n] = '\0';
        v = string_result_ok(out);
        free(out);
    } else {
        v = string_result_err("ecdsa_p256_generate: writing the key failed");
    }
    if (bio != NULL) BIO_free(bio);
    EVP_PKEY_free(pkey);
    return v;
}

/* ecdsa_p256_public_pem(private_key_pem): the matching public key, to publish or to verify with. */
static Value native_ecdsa_p256_public_pem(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    char *pem = copy_cstring(args[0]);
    EVP_PKEY *pkey = read_pem(pem, 1);
    free(pem);
    if (pkey == NULL) return string_result_err("ecdsa_p256_public_pem: not a PEM private key");
    if (!is_p256(pkey)) {
        EVP_PKEY_free(pkey);
        return string_result_err("ecdsa_p256_public_pem: the key is not on P-256");
    }
    BIO *bio = BIO_new(BIO_s_mem());
    int ok = bio != NULL && PEM_write_bio_PUBKEY(bio, pkey) == 1;
    Value v;
    if (ok) {
        char *data = NULL;
        long n = BIO_get_mem_data(bio, &data);
        char *out = ziman_xmalloc((size_t)n + 1);
        memcpy(out, data, (size_t)n);
        out[n] = '\0';
        v = string_result_ok(out);
        free(out);
    } else {
        v = string_result_err("ecdsa_p256_public_pem: writing the key failed");
    }
    if (bio != NULL) BIO_free(bio);
    EVP_PKEY_free(pkey);
    return v;
}

/* ecdsa_p256_public_point(pem): the public point as `x || y`, 64 bytes -- what a JWK's `x` and `y`
 * are, each base64url of 32 bytes. Takes a private or a public PEM, since a private key carries
 * its point. The leading 0x04 of the uncompressed form is dropped: it says "uncompressed", which
 * a fixed-width pair already says. */
static Value native_ecdsa_p256_public_point(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type result_type = type_result(byte_array_type(), type_scalar(TY_STRING));
    char *pem = copy_cstring(args[0]);
    EVP_PKEY *pkey = read_pem(pem, 1);
    if (pkey == NULL) pkey = read_pem(pem, 0);
    free(pem);
    if (pkey == NULL) return bytes_result_err(result_type, "ecdsa_p256_public_point: not a PEM key");
    if (!is_p256(pkey)) {
        EVP_PKEY_free(pkey);
        return bytes_result_err(result_type, "ecdsa_p256_public_point: the key is not on P-256");
    }
    unsigned char point[133];
    size_t n = 0;
    int ok = EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof point, &n) == 1;
    EVP_PKEY_free(pkey);
    if (!ok || n != 1 + 2 * p256_coord_len || point[0] != 0x04) {
        return bytes_result_err(result_type, "ecdsa_p256_public_point: the key has no uncompressed public point");
    }
    return value_result(type_ref(result_type),
                        result_obj_new(1, value_byte_array(point + 1, 2 * p256_coord_len)));
}

/* ecdsa_p256_sign(private_key_pem, data): SHA-256 then ECDSA, answering `r || s`. */
static Value native_ecdsa_p256_sign(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type result_type = type_result(byte_array_type(), type_scalar(TY_STRING));
    char *pem = copy_cstring(args[0]);
    int dn = 0;
    uint8_t *data = array_copy_bytes(args[1].as.arr, &dn);
    EVP_PKEY *pkey = read_pem(pem, 1);
    free(pem);
    if (pkey == NULL) {
        free(data);
        return bytes_result_err(result_type, "ecdsa_p256_sign: not a PEM private key");
    }
    if (!is_p256(pkey)) {
        EVP_PKEY_free(pkey);
        free(data);
        return bytes_result_err(result_type, "ecdsa_p256_sign: the key is not on P-256");
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char *der = NULL;
    size_t der_len = 0;
    int ok = ctx != NULL
        && EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey) == 1
        && EVP_DigestSign(ctx, NULL, &der_len, data, (size_t)dn) == 1;
    if (ok) {
        der = ziman_xmalloc(der_len);
        ok = EVP_DigestSign(ctx, der, &der_len, data, (size_t)dn) == 1;
    }
    Value v = bytes_result_err(result_type, "ecdsa_p256_sign: signing failed");
    if (ok) {
        /* DER (two ASN.1 integers, variable width) to the fixed 32+32 JWS wants. */
        const unsigned char *walk = der;
        ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &walk, (long)der_len);
        if (sig != NULL) {
            unsigned char raw[64];
            const BIGNUM *r = ECDSA_SIG_get0_r(sig);
            const BIGNUM *sv = ECDSA_SIG_get0_s(sig);
            if (BN_bn2binpad(r, raw, (int)p256_coord_len) == (int)p256_coord_len
                && BN_bn2binpad(sv, raw + p256_coord_len, (int)p256_coord_len) == (int)p256_coord_len) {
                v = value_result(type_ref(result_type), result_obj_new(1, value_byte_array(raw, sizeof raw)));
            }
            ECDSA_SIG_free(sig);
        }
    }
    if (ctx != NULL) EVP_MD_CTX_free(ctx);
    free(der);
    free(data);
    EVP_PKEY_free(pkey);
    return v;
}

/* ecdsa_p256_verify(public_key_pem, data, signature): whether `r || s` signs `data` under the key.
 * **A bool, not a Result**: every way this can answer no -- a wrong signature, the wrong key, a
 * malformed key, a signature of the wrong length -- means the same thing to a caller, which is
 * that the bytes are not to be trusted. */
static Value native_ecdsa_p256_verify(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    char *pem = copy_cstring(args[0]);
    int dn = 0, sn = 0;
    uint8_t *data = array_copy_bytes(args[1].as.arr, &dn);
    uint8_t *raw = array_copy_bytes(args[2].as.arr, &sn);
    EVP_PKEY *pkey = read_pem(pem, 0);
    free(pem);
    int ok = 0;
    if (pkey != NULL && is_p256(pkey) && sn == (int)(2 * p256_coord_len)) {
        ECDSA_SIG *sig = ECDSA_SIG_new();
        BIGNUM *r = BN_bin2bn(raw, (int)p256_coord_len, NULL);
        BIGNUM *s = BN_bin2bn(raw + p256_coord_len, (int)p256_coord_len, NULL);
        if (sig != NULL && r != NULL && s != NULL && ECDSA_SIG_set0(sig, r, s) == 1) {
            unsigned char *der = NULL;
            int der_len = i2d_ECDSA_SIG(sig, &der);
            if (der_len > 0) {
                EVP_MD_CTX *ctx = EVP_MD_CTX_new();
                ok = ctx != NULL
                    && EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pkey) == 1
                    && EVP_DigestVerify(ctx, der, (size_t)der_len, data, (size_t)dn) == 1;
                if (ctx != NULL) EVP_MD_CTX_free(ctx);
                OPENSSL_free(der);
            }
            ECDSA_SIG_free(sig);
        } else {
            if (sig != NULL) ECDSA_SIG_free(sig);
            BN_free(r);
            BN_free(s);
        }
    }
    if (pkey != NULL) EVP_PKEY_free(pkey);
    free(data);
    free(raw);
    return value_bool(ok);
}

void native_crypto_register(void) {
    native_register("crypto::sha1", native_sha1);
    native_register("crypto::sha256", native_sha256);
    native_register("crypto::sha384", native_sha384);
    native_register("crypto::sha512", native_sha512);
    native_register("crypto::hmac_sha1", native_hmac_sha1);
    native_register("crypto::hmac_sha256", native_hmac_sha256);
    native_register("crypto::hmac_sha384", native_hmac_sha384);
    native_register("crypto::hmac_sha512", native_hmac_sha512);
    native_register("crypto::pbkdf2_hmac_sha256", native_pbkdf2_sha256);
    native_register("crypto::pbkdf2_hmac_sha512", native_pbkdf2_sha512);
    native_register("crypto::bcrypt_native", native_bcrypt);
    native_register("crypto::bcrypt_verify", native_bcrypt_verify);
    native_register("crypto::argon2id_raw", native_argon2id_raw);
    native_register("crypto::aes_gcm_encrypt", native_aes_gcm_encrypt);
    native_register("crypto::aes_gcm_decrypt", native_aes_gcm_decrypt);
    native_register("crypto::bytes_xor", native_bytes_xor);
    native_register("crypto::bytes_equal", native_bytes_equal);
    native_register("crypto::ecdsa_p256_generate", native_ecdsa_p256_generate);
    native_register("crypto::ecdsa_p256_public_pem", native_ecdsa_p256_public_pem);
    native_register("crypto::ecdsa_p256_public_point", native_ecdsa_p256_public_point);
    native_register("crypto::ecdsa_p256_sign", native_ecdsa_p256_sign);
    native_register("crypto::ecdsa_p256_verify", native_ecdsa_p256_verify);
    /* **Registered under `encoding`, not `crypto`** (2026-10-24). The declarations moved to
     * `lib/core/encoding.zm`, which gathers base 2/8/16/32/64 in one place; `crypto`'s four are
     * `#[deprecated]` delegates now, so the C stays here beside the other OpenSSL work while the
     * vocabulary lives with its siblings. */
    native_register("encoding::base64_encode", native_base64_encode);
    native_register("encoding::base64_decode", native_base64_decode);
    native_register("encoding::base64url_encode", native_base64url_encode);
    native_register("encoding::base64url_decode", native_base64url_decode);
}
