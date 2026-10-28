# `crypto`

Hashes, message authentication, key derivation, password hashing, one authenticated
cipher and base64, on OpenSSL's libcrypto (already linked for TLS) and, for bcrypt, libxcrypt. The
primitives are native. Byte data is `[byte]`; a digest or key is returned as `[byte]` (use
`number.to_hex` for hex text).

The module is deliberately small: one cipher, no public-key operations. It exists so the
database drivers can authenticate (SCRAM-SHA-256 for Postgres, the SHA-1 / SHA-256 login
scrambles for MySQL), so programs can sign and verify data (see `jwt`), so they can store
passwords (`password_hash`, `password_verify`), and so a secret that must be read back exactly --
unlike a password, which only ever needs a yes-or-no verify -- can be kept encrypted at rest
(`aes_gcm_encrypt`/`aes_gcm_decrypt`); it grows when a real consumer needs more.

Error convention: a wrong *argument* (mismatched lengths, an out-of-range iteration count)
is a runtime error, like any other misuse; malformed *data* (`base64_decode` of text that is
not base64) is a `Err(string)`.

`use crypto` -- or `use crypto.Name` to bring one item in bare.

## Example

```
use crypto
use encoding
use number.to_hex

fn main() {
    val data = to_bytes("abc")
    print(to_hex(crypto.sha256(data)))
    print(to_hex(crypto.hmac_sha256(to_bytes("key"), data)))

    // Compare secrets in constant time, not with ==.
    print(crypto.bytes_equal(crypto.sha256(data), crypto.sha256(data)))

    print(encoding.base64_encode(data))
    // Two steps that can each fail -- the decode and the UTF-8 check -- in one condition, because
    // a later part of the chain sees what the earlier part bound.
    if Ok(raw) = encoding.base64_decode("YWJj") && Ok(text) = bytes_to_string(raw) {
        print(text)
    }
    print(is_ok(encoding.base64_decode("***")))

    // A password hash: many iterations, a salt, a fixed output length.
    val key = crypto.pbkdf2_hmac_sha256(to_bytes("secret"), to_bytes("salt"), 1000, 16)
    print(len(key))
}
```

Output:

```
ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
9c196e32dc0175f86f4b1cb89289d6619de6bee699e4c378e68309ed97a1a6ab
true
YWJj
abc
false
16
```

## Hashes

### `sha1`

```
pub native fn sha1(data: [byte]) -> [byte]
```

The SHA-1 digest (20 bytes) of `data`. SHA-1 is broken for collision resistance: this is
here for MySQL's login scramble and for old formats, not for anything new.

### `sha256`

```
pub native fn sha256(data: [byte]) -> [byte]
```

The SHA-256 digest (32 bytes) of `data`.

### `sha384`

```
pub native fn sha384(data: [byte]) -> [byte]
```

The SHA-384 digest (48 bytes) of `data`.

### `sha512`

```
pub native fn sha512(data: [byte]) -> [byte]
```

The SHA-512 digest (64 bytes) of `data`.

## Message authentication and keys

### `hmac_sha1`

```
pub native fn hmac_sha1(key: [byte], data: [byte]) -> [byte]
```

HMAC-SHA-1 of `data` under `key` (20 bytes): what one-time passwords (`otp`) are built on. Do not choose it for anything new.

### `hmac_sha256`

```
pub native fn hmac_sha256(key: [byte], data: [byte]) -> [byte]
```

HMAC-SHA-256 of `data` under `key` (32 bytes): a keyed digest that proves `data` came from
someone holding the key. Compare two MACs with `bytes_equal`, never with `==`.

### `hmac_sha384`

```
pub native fn hmac_sha384(key: [byte], data: [byte]) -> [byte]
```

HMAC-SHA-384 of `data` under `key` (48 bytes).

### `hmac_sha512`

```
pub native fn hmac_sha512(key: [byte], data: [byte]) -> [byte]
```

HMAC-SHA-512 of `data` under `key` (64 bytes).

### `pbkdf2_hmac_sha256`

```
pub native fn pbkdf2_hmac_sha256(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]
```

PBKDF2 with HMAC-SHA-256 (RFC 8018): a key of `length` bytes (0 to 1048576) derived from
`password` and `salt` by `iterations` (1 or more; use a few hundred thousand for password
storage) rounds. The slow, deliberate way to turn a password into a key or a hash to store.

### `pbkdf2_hmac_sha512`

```
pub native fn pbkdf2_hmac_sha512(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]
```

The same with HMAC-SHA-512.

## Password hashing

### `bcrypt_hash`

```
pub fn bcrypt_hash(password: string, cost: Option<int> = 10) -> Result<string, string>
```

A bcrypt hash of `password`: 60 characters starting `$2b$`, with a fresh random salt in it. `cost` is the base-2
logarithm of the rounds (4 to 31; 10 is the usual default, each step doubles the time). bcrypt reads only the first
72 bytes of the password.

### `bcrypt_verify`

```
pub native fn bcrypt_verify(password: string, hash: string) -> bool
```

Whether `hash` (a `$2a$`, `$2b$` or `$2y$` bcrypt hash, such as Spring Security's `BCryptPasswordEncoder` writes) is the
bcrypt hash of `password`. False for anything that is not a well-formed bcrypt hash. The comparison takes the same time
whatever the contents.

### `argon2id_raw`

```
pub native fn argon2id_raw(
    password: [byte],
    salt: [byte],
    memory_kib: int,
    iterations: int,
    parallelism: int,
    length: int
) -> Result<[byte], string>
```

Argon2id (RFC 9106) of `password` and `salt` (at least 8 bytes): `length` bytes (4 to 1024) using `memory_kib` KiB of
memory, `iterations` passes and `parallelism` lanes. The raw building block; `argon2id_hash` is the one to use.

### `argon2id_hash`

```
pub fn argon2id_hash(
    password: string,
    memory_kib: Option<int> = 19456,
    iterations: Option<int> = 2,
    parallelism: Option<int> = 1
) -> Result<string, string>
```

An Argon2id hash of `password` in the PHC string format (`$argon2id$v=19$m=19456,t=2,p=1$<salt>$<hash>`, the format
every Argon2 implementation reads and writes), with a fresh random salt. The defaults are OWASP's minimum for
Argon2id (19 MiB, 2 passes, 1 lane); raise `memory_kib` where the server can afford it.

### `argon2id_verify`

```
pub fn argon2id_verify(password: string, encoded: string) -> bool
```

Whether `encoded` (a PHC-format Argon2id string) is the hash of `password`. False for anything malformed. The
comparison takes the same time whatever the contents.

### `password_hash`

```
pub fn password_hash(password: string) -> Result<string, string>
```

A hash of `password` to store: Argon2id with the defaults of `argon2id_hash`.

### `password_verify`

```
pub fn password_verify(password: string, stored: string) -> bool
```

Whether `stored` (as `password_hash` or `bcrypt_hash` wrote it, or a Spring `BCryptPasswordEncoder` hash) is the hash
of `password`. The scheme is read from the text, so hashes of both kinds can live in one column.

### `password_needs_rehash`

```
pub fn password_needs_rehash(stored: string) -> bool
```

Whether `stored` should be replaced by a fresh `password_hash` (do it when the user has just logged in and their
password is in hand): it is a bcrypt hash, or an Argon2id one with weaker settings than today's defaults.

## Authenticated encryption

### `aes_gcm_encrypt`

```
pub native fn aes_gcm_encrypt(key: [byte], plaintext: [byte]) -> Result<[byte], string>
```

AES-256-GCM of `plaintext` under `key` (32 bytes) with a fresh random 12-byte nonce, returned as
`nonce || ciphertext || tag` (12 + len(plaintext) + 16 bytes) so `aes_gcm_decrypt` needs only the key
and this one blob. An `Err` only for a wrong key length; encryption itself cannot otherwise fail.

### `aes_gcm_decrypt`

```
pub native fn aes_gcm_decrypt(key: [byte], blob: [byte]) -> Result<[byte], string>
```

The plaintext `aes_gcm_encrypt` sealed into `blob` under `key`. An `Err` for a wrong key length, a
`blob` too short to hold a nonce and a tag, or a tag that does not match -- the wrong key, or `blob`
was altered.

## Byte arrays

### `bytes_xor`

```
pub native fn bytes_xor(a: [byte], b: [byte]) -> [byte]
```

The byte-wise XOR of two arrays of equal length (a runtime error if the lengths differ).
Ziman has no bitwise operators.

### `bytes_equal`

```
pub native fn bytes_equal(a: [byte], b: [byte]) -> bool
```

True when both arrays hold the same bytes. For arrays of the same length it takes the same
time whatever the contents, so it is safe for comparing a MAC or a token an attacker is
guessing; `==` is not (it stops at the first difference).

## Base64, moved to `encoding`

### `base64_encode`

```
pub fn base64_encode(data: [byte]) -> string
```

### `base64_decode`

```
pub fn base64_decode(s: string) -> Result<[byte], string>
```

### `base64url_encode`

```
pub fn base64url_encode(data: [byte]) -> string
```

### `base64url_decode`

```
pub fn base64url_decode(s: string) -> Result<[byte], string>
```
