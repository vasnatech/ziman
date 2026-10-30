# `jwt`

JSON Web Tokens (RFC 7519), in two families: **a shared secret** (HS256, HS384,
HS512) and **a P-256 key pair** (ES256, added 2026-10-29). `sign` makes a token from a claims
object, `verify` checks one and gives the claims back. Built on `crypto` (HMAC, ECDSA,
base64url, a constant-time comparison), `json` and `clock`.

**A key's type chooses its family, and that is a security property rather than a convenience.**
A shared secret is `[byte]`; an EC key is PEM `string`. The classic JWT attack is algorithm
confusion -- presenting a public key as if it were an HMAC secret, so a token the attacker
signed verifies -- and it needs one key parameter that both families read. There is none here,
so a token's `alg` header can only choose **within** the family the caller already chose: an
`HS256` token handed to a verifier holding a public key is refused by name, never MAC'd with the
PEM text as a secret. `tests/lang/jwt_es256` pins that and three more of the same shape.


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
compared in constant time; the token HAS an `exp` claim and it is not past; `nbf`, when there is one, is not in the future
(each with an optional leeway); and an `exp` or `nbf` that is there but is not a number is an error, not "no expiry". A
token without `exp` never expires, so `verify` refuses one unless `verify_claims(require_exp: false)` says a token may be
open-ended. `verify_claims` can also require the issuer (`iss`) and audience (`aud`) to be the ones this service expects,
which matters when several services share a key: without it a token issued for one is accepted by all. It does not check
`sub`: read it from the claims. Only the signature of the token as issued protects it, so keep the key secret and long
(32 bytes or more for HS256).


```
jwt.verify_claims(token: t, key: key, issuer: "health-record", audience: "web")
```

`use jwt` -- or `use jwt.Name` to bring one item in bare.

## Example

```
use json
use json.Json
use json.int_to_json
use jwt

// Signed tokens: a set of claims, an HMAC over them, and a verification that answers what the
// claims were. Every check takes the time as an argument rather than reading the clock, so an
// expiry can be tested rather than waited for.
fn demo() -> Result<unit, string> {
    val key = to_bytes("a-secret-of-at-least-32-bytes-long!!")
    print(jwt.algorithm_for_key(key))

    // `jwt.claims(subject, expires_in_seconds)` reads the clock; building the object by hand keeps
    // this example the same on every run.
    val claims = Json.JObject(Map<string, Json> {
        "sub": Json.JString("ada"),
        "iat": int_to_json(1700000000),
        "exp": int_to_json(1700003600)
    })
    val token = try jwt.sign(claims, key)
    print(len(split(token, ".")))

    // Verified well inside the lifetime: the claims come back as they went in.
    switch jwt.verify_at(token, key, 1700000100, 0) {
        Ok(back) { print(json.format(back)) }
        Err(e) { print(jwt.jwt_error_message(e)) }
    }

    // After `exp`, it is refused -- and the refusal says which check failed.
    if Err(e) = jwt.verify_at(token, key, 1700009999, 0) {
        print(jwt.jwt_error_message(e))
    }

    // Leeway covers two clocks that disagree slightly, rather than rejecting on a second.
    print(is_ok(jwt.verify_at(token, key, 1700003630, 60)))

    // A different key is a different signature, which is the whole point.
    if Err(e) = jwt.verify_at(token, to_bytes("a-different-secret-of-32-bytes-ok!!!"), 1700000100, 0) {
        print(jwt.jwt_error_message(e))
    }

    // So is an altered payload: the signature covers it.
    val tampered = concat(substring(token, 0, len(token) - 4), "AAAA")
    if Err(e) = jwt.verify_at(tampered, key, 1700000100, 0) {
        print(jwt.jwt_error_message(e))
    }
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("jwt example failed: {}\n", e)
    }
}
```

Output:

```
HS256
3
{"sub":"ada","iat":1700000000,"exp":1700003600}
the token has expired
true
the signature does not match
the signature does not match
```

### `JwtError`

```
pub enum JwtError {
    Malformed(string)
    UnsupportedAlgorithm(string)
    BadSignature
    Expired
    NotYetValid
    MissingClaim(string)
    WrongIssuer
    WrongAudience
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

```
pub fn sign(claims: Json, key: [byte], algorithm: string) -> Result<string, string>
```

The same with `algorithm` chosen: HS256, HS384 or HS512, an Err for anything else.

**`sign_with` until 2026-10-24.** The suffix was saying what the third argument already says,
and an algorithm is a `string` where nothing else here is -- unlike a bare `true` or a lone
`int`, a reader of `sign(claims, key, "HS512")` needs no name to tell them what it is.

```
pub fn sign(claims: Json, private_key_pem: string) -> Result<string, string>
```

**ES256**: a token signed with a P-256 private key as PEM text, so a holder can verify it
without being able to issue one. That is what a shared secret cannot do, and what a verifiable
credential needs (2026-10-29).

The key is a `string` where an HMAC key is `[byte]`, and that is deliberate: see `Key`. An EC key
is never a `[byte]` key here, so there is no call that hands a public key to the HMAC path.

```
pub fn sign(claims: Json, private_key_pem: string, algorithm: string) -> Result<string, string>
```

The same with the algorithm written out. `ES256` is the only one an EC key signs here -- P-256 is
the only curve `crypto` has -- and anything else is refused by name rather than ignored.

### `verify_at`

```
pub fn verify_at(token: string, key: [byte], now_seconds: int, leeway_seconds: int) -> Result<Json, JwtError>
```

Checks `token` against `key` as of `now_seconds` (Unix time) with `leeway_seconds` of allowed clock difference on `exp`
and `nbf`, and gives back its claims. `verify` is this with the current time; this one exists so a token can be checked at a
chosen time. The token must have an `exp`.

### `verify`

```
pub fn verify(token: string, key: [byte]) -> Result<Json, JwtError>
```

Checks `token` against `key` now and gives back its claims. The token must have an `exp` that is not past.

```
pub fn verify(token: string, public_key_pem: string) -> Result<Json, JwtError>
```

**ES256**: checks `token` against a P-256 **public** key as PEM text. The key verifies and
cannot issue, which is the whole reason this path exists.

A token naming any other algorithm is refused by name rather than verified another way -- see
`Key`, where that is the point rather than a precaution.

### `verify_at`

```
pub fn verify_at(
    token: string,
    public_key_pem: string,
    now_seconds: int,
    leeway_seconds: int
) -> Result<Json, JwtError>
```

`verify_at` for a public key: the same clock control, the same family rule.

### `verify_claims`

```
pub fn verify_claims(
    token: string,
    key: [byte],
    leeway_seconds: Option<int>,
    now: Option<int>,
    require_exp: Option<bool>,
    issuer: Option<string>,
    audience: Option<string>,
    algorithm: Option<string>
) -> Result<Json, JwtError>
```

`verify` with every check chosen, in a named call that leaves out what it does not need. `leeway_seconds` (default 0) is the
clock difference forgiven on `exp` and `nbf`; `now` (default the current time) is the Unix time to check as of;
`require_exp` (default true) says a token with no `exp` is refused (`MissingClaim("exp")`) -- pass false only for a token
that is meant never to expire; `issuer`, when given, must be the token's `iss` (`WrongIssuer`, or `MissingClaim("iss")` when
there is none); and `audience`, when given, must be the token's `aud` -- a string, or one of an array of strings
(`WrongAudience`, or `MissingClaim("aud")`). An `exp` or `nbf` that is not a number is `Malformed` whatever `require_exp` says.

```
pub fn verify_claims(
    token: string,
    public_key_pem: string,
    leeway_seconds: Option<int>,
    now: Option<int>,
    require_exp: Option<bool>,
    issuer: Option<string>,
    audience: Option<string>,
    algorithm: Option<string>
) -> Result<Json, JwtError>
```

`verify_claims` for a public key: every check the HMAC form offers, over the family a PEM key
can use. A credential is the case that wants `issuer`/`audience` most, since the point of
publishing a verifying key is that several parties hold it.

**This was `verify_claims_pem` until F239.** A named call that omitted optional parameters did
not resolve against an overload set, so declaring a second `verify_claims` broke every existing
`verify_claims(token: t, key: k, now: n)` with "no declaration takes 3 argument(s)" -- a gap in
overload resolution rather than a fact about JWTs, and this carried a name of its own to work
around it while `verify`/`verify_at`, which are called positionally, overloaded cleanly. With
the gap closed the name is the key's own: `key:` reaches the secret form and
`public_key_pem:` reaches this one, so the parameter a caller names chooses the family, exactly
as the type does for the positional pair.

### `subject`

```
pub fn subject(claims: Json) -> Option<string>
```

The `sub` claim, when it is a string.

### `issuer`

```
pub fn issuer(claims: Json) -> Option<string>
```

The `iss` claim, when it is a string.

### `audience`

```
pub fn audience(claims: Json) -> [string]
```

The audiences the `aud` claim names, in order: `["web"]` for `"aud": "web"`, the strings of an array, and `[]` when there is
no `aud` (or it is neither).
