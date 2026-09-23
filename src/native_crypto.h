#ifndef ZIMAN_NATIVE_CRYPTO_H
#define ZIMAN_NATIVE_CRYPTO_H

/* Registers the `crypto` core module's natives: sha256, hmac_sha256,
 * pbkdf2_hmac_sha256, base64_encode/decode and bytes_xor. Called once
 * from main() before interpret() runs. See corelib/crypto.zm. */
void native_crypto_register(void);

#endif
