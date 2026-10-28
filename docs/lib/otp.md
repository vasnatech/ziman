# `otp`

Time-based one-time passwords (RFC 6238, over RFC 4226's HOTP) of the kind authenticator apps show: six digits that
change every 30 seconds, from a secret shared once, by QR code or by typing it. Pure Ziman on top of `crypto` and `random`.


```
val secret = otp.generate_secret()                       // "JBSWY3DPEHPK3PXP..." to show the user, base32
val uri = otp.provisioning_uri("My App", "ada@example.com", secret)   // otpauth://totp/... for a QR code
val key = try otp.base32_decode(secret)                     // the bytes a code is computed from
otp.totp_valid(key, typed_code, now_seconds, 1)          // true for this 30-second step and one either side
```

Only the parameters every authenticator app supports: HMAC-SHA-1, six digits, a 30-second step.

`use otp` -- or `use otp.Name` to bring one item in bare.

## Example

```
use encoding
use otp

// Time-based one-time passwords, the second factor an authenticator app shows. Every function here
// takes the time as an argument rather than reading the clock, which is what makes a login flow
// testable: the same key and the same second always give the same code.
fn demo() -> Result<unit, string> {
    // A shared secret is exchanged in base32, because that is what a QR code and a phone agree on.
    val key = try encoding.base32_decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")
    print(encoding.base32_encode(key))

    // HOTP counts events: the counter is whatever the two sides agree it is.
    print(otp.hotp(key, 0, 6))
    print(otp.hotp(key, 1, 6))
    print(otp.hotp(key, 1, 8))

    // TOTP is HOTP over a counter derived from the clock, in thirty-second steps -- so these two
    // seconds fall in the same step and give the same code, and the third does not.
    print(otp.totp(key, 1700000000))
    print(otp.totp(key, 1700000005) == otp.totp(key, 1700000000))
    print(otp.totp(key, 1700000010) == otp.totp(key, 1700000000))

    // Verification allows a window of steps either side, because the two clocks are never exactly
    // together. The matching step says how far out the other side was, which is worth logging.
    val code = otp.totp(key, 1700000000)
    print(otp.totp_valid(key, code, 1700000000, 1))
    print(otp.totp_valid(key, code, 1700000025, 1))
    print(otp.totp_valid(key, code, 1700000600, 1))
    print(otp.totp_matching_step(key, code, 1700000025, 1))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("otp example failed: {}\n", e)
    }
}
```

Output:

```
GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ
755224
287082
94287082
921300
true
false
true
true
false
Some(56666666)
```

### `generate_secret`

```
pub fn generate_secret() -> string
```

The secret of 20 random bytes (the size RFC 4226 recommends) as base32 text, 32 characters, the way it is shown to a user and stored.

### `base32_encode`

```
pub fn base32_encode(data: [byte]) -> string
```

### `base32_decode`

```
pub fn base32_decode(text: string) -> Result<[byte], string>
```

### `hotp`

```
pub fn hotp(key: [byte], counter: int, digits: int) -> string
```

The HOTP value (RFC 4226) for `counter`, `digits` long with leading zeros: HMAC-SHA-1, then the
"dynamic truncation".

**`digits` must be 6 to 9, and that is now enforced** rather than only documented. Dynamic
truncation yields a 31-bit number, at most 2147483647, so nine digits is the most that can carry
real entropy: a tenth could only ever be 0, 1 or 2, which makes `digits: 10` a code that looks
stronger than it is. Below six is weaker than the RFC allows at all, `0` and a negative both used
to answer the single character `"0"`, and `20` reached an integer overflow inside the modulus
loop and reported *that* -- a message about `*` on line 110, naming nothing the caller wrote.

### `totp`

```
pub fn totp(key: [byte], unix_seconds: int) -> string
```

The six-digit code an authenticator app shows at `unix_seconds` (30-second steps from the epoch).

```
pub fn totp(key: [byte], unix_seconds: int, digits: int) -> string
```

**The same with `digits` chosen**, 6 to 9. RFC 6238 allows more than six and its own test vectors
are eight, which this could not produce until 2026-10-24: `hotp(key, unix_seconds / 30, 8)` did
it by hand, but the *verifiers* below went through the six-digit `totp`, so an eight-digit code
could be generated and then not checked. All three take `digits` now.

### `totp_matching_step`

```
pub fn totp_matching_step(key: [byte], code: string, unix_seconds: int, window: int) -> Option<int>
```

The 30-second step number `code` matches, at `unix_seconds` or within `window` steps of it either way (1
allows for a clock a few seconds off), or `None` when it matches none: what a caller needs to reject a
replay of a step already spent (store the step a login accepted; refuse a future call that matches the
same one). The comparison takes the same time whatever the code, and every step in the window is checked
even after a match, so the time taken does not say which step (or whether one) matched.

```
pub fn totp_matching_step(
    key: [byte],
    code: string,
    unix_seconds: int,
    window: int,
    digits: int
) -> Option<int>
```

The same for a code of `digits` digits.

### `totp_valid`

```
pub fn totp_valid(key: [byte], code: string, unix_seconds: int, window: int) -> bool
```

Whether `code` is the code at `unix_seconds` or within `window` 30-second steps of it either way (1 allows for a clock a few seconds
off). The comparison takes the same time whatever the code.

```
pub fn totp_valid(key: [byte], code: string, unix_seconds: int, window: int, digits: int) -> bool
```

The same for a code of `digits` digits.

### `provisioning_uri`

```
pub fn provisioning_uri(issuer: string, account: string, secret: string) -> string
```

The `otpauth://` address an authenticator app takes as a QR code to add the account: `issuer` (the name of your service) and
`account` (the user's email) label it, `secret` is the base32 secret.
