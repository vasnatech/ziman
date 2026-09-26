# `jwt`

JSON Web Tokens (RFC 7519) signed with a shared secret: HS256, HS384 and HS512 (RFC 7518), the
algorithms that need only a key. `sign` makes a token from a claims object, `verify` checks one and gives the
claims back. Built on `crypto` (HMAC, base64url, a constant-time comparison), `json` and `clock`.


```
val key = unwrap(base64_decode(secret))
val token = unwrap(jwt.sign(jwt.with_claim(jwt.claims("ada@example.com", 3600), "role", Json.JString("admin")), key))
switch jwt.verify(token, key) {
    Ok(claims) { ... jwt.subject(claims) ... }
    Err(e) { print(jwt.jwt_error_message(e)) }
}
```

What `verify` checks: the token has three parts and a JSON header and payload; the header's `alg` is one of the three
HMAC algorithms (never `none`, and never an algorithm this module cannot use with a shared key); the signature matches,
compared in constant time; `exp` is not past and `nbf` is not in the future, each with an optional leeway. It does
not check `iss`, `aud` or `sub`: read them from the claims. Only the signature of the token as issued protects it, so
keep the key secret and long (32 bytes or more for HS256).

`use jwt` -- or `use jwt.Name` to bring one item in bare.

### `JwtError`

```
pub enum JwtError {
    Malformed(string)
    UnsupportedAlgorithm(string)
    BadSignature
    Expired
    NotYetValid
}
```

Why a token was rejected.

### `jwt_error_message`

```
pub fn jwt_error_message(e: JwtError) -> string
```

The text for a JwtError.

### `algorithm_for_key`

```
pub fn algorithm_for_key(key: [byte]) -> string
```

The HMAC algorithm that matches a key's size, as the common libraries choose it: HS512 for 64 bytes or more, HS384 for 48
or more, otherwise HS256.

### `claims`

```
pub fn claims(subject: string, expires_in_seconds: int) -> Json
```

A claims object for `subject` (the `sub` claim) issued now (`iat`) and expiring `expires_in_seconds` from now (`exp`).

### `with_claim`

```
pub fn with_claim(claims: Json, name: string, value: Json) -> Json
```

`claims` with one more claim (or `name` replaced): `with_claim(c, "role", Json.JString("admin"))`. `claims` that is not a
JSON object is returned as it is.

### `sign`

```
pub fn sign(claims: Json, key: [byte]) -> Result<string, string>
```

A token for `claims` (a JSON object), signed with `key` and the HMAC algorithm that matches its size
(`algorithm_for_key`). An Err for claims that are not an object.

### `sign_with`

```
pub fn sign_with(claims: Json, key: [byte], algorithm: string) -> Result<string, string>
```

The same with `algorithm` chosen: HS256, HS384 or HS512, an Err for anything else.

### `verify_at`

```
pub fn verify_at(token: string, key: [byte], now_seconds: int, leeway_seconds: int) -> Result<Json, JwtError>
```

Checks `token` against `key` as of `now_seconds` (Unix time) with `leeway_seconds` of allowed clock difference on `exp`
and `nbf`, and gives back its claims. `verify` is this with the current time; this one exists so a token can be checked at a
chosen time.

### `verify`

```
pub fn verify(token: string, key: [byte]) -> Result<Json, JwtError>
```

Checks `token` against `key` now and gives back its claims.

### `verify_leeway`

```
pub fn verify_leeway(token: string, key: [byte], leeway_seconds: int) -> Result<Json, JwtError>
```

The same, forgiving `leeway_seconds` of clock difference between the issuer and this machine on `exp` and `nbf`.

### `subject`

```
pub fn subject(claims: Json) -> Option<string>
```

The `sub` claim, when it is a string.
