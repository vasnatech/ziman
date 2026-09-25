#ifndef ZIMAN_NATIVE_CRYPTO_H
#define ZIMAN_NATIVE_CRYPTO_H

/* Registers the `crypto` core module's natives (OpenSSL-backed hashes, HMAC, PBKDF2,
 * constant-time compare, base64/base64url, XOR). Called once from main() before
 * interpret() runs. See lib/core/crypto.zm. */
void native_crypto_register(void);

#endif
