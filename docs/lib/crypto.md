# `crypto`

Hashes, message authentication, key derivation and base64, on OpenSSL's
libcrypto (already linked for TLS). Every function is native. Byte data is `[byte]`; a
digest or key is returned as `[byte]` (use `number.bytes_to_hex` for hex text).

The module is deliberately small: no ciphers, no public-key operations, no randomness (use
`random.random_bytes`). It exists so the database drivers can authenticate (SCRAM-SHA-256
for Postgres, the SHA-1 / SHA-256 login scrambles for MySQL) and so programs can sign and
verify data; it grows when a real consumer needs more.

Error convention: a wrong *argument* (mismatched lengths, an out-of-range iteration count)
is a runtime error, like any other misuse; malformed *data* (`base64_decode` of text that is
not base64) is a `Err(string)`.

`use crypto` -- or `use crypto.Name` to bring one item in bare.

## Example

```
use crypto
use number.bytes_to_hex

fn main() {
    val data = string_to_bytes("abc")
    print(bytes_to_hex(crypto.sha256(data)))
    print(bytes_to_hex(crypto.hmac_sha256(string_to_bytes("key"), data)))

    // Compare secrets in constant time, not with ==.
    print(crypto.bytes_equal(crypto.sha256(data), crypto.sha256(data)))

    print(crypto.base64_encode(data))
    print(unwrap(bytes_to_string(unwrap(crypto.base64_decode("YWJj")))))
    print(is_ok(crypto.base64_decode("***")))

    // A password hash: many iterations, a salt, a fixed output length.
    val key = crypto.pbkdf2_hmac_sha256(string_to_bytes("secret"), string_to_bytes("salt"), 1000, 16)
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

### `sha512`

```
pub native fn sha512(data: [byte]) -> [byte]
```

The SHA-512 digest (64 bytes) of `data`.

## Message authentication and keys

### `hmac_sha256`

```
pub native fn hmac_sha256(key: [byte], data: [byte]) -> [byte]
```

HMAC-SHA-256 of `data` under `key` (32 bytes): a keyed digest that proves `data` came from
someone holding the key. Compare two MACs with `bytes_equal`, never with `==`.

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

## Base64

### `base64_encode`

```
pub native fn base64_encode(data: [byte]) -> string
```

Standard base64 (RFC 4648) with `=` padding.

### `base64_decode`

```
pub native fn base64_decode(s: string) -> Result<[byte], string>
```

The bytes of standard base64 text. Strict: the length must be a multiple of 4, only the
alphabet plus trailing `=` padding is accepted, no whitespace. An Err otherwise.

### `base64url_encode`

```
pub native fn base64url_encode(data: [byte]) -> string
```

URL- and filename-safe base64 (`-` and `_`), without padding: what JWTs and URLs use.

### `base64url_decode`

```
pub native fn base64url_decode(s: string) -> Result<[byte], string>
```

The bytes of base64url text, with no padding (a `=` is an Err).
