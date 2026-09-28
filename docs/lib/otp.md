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

### `generate_secret`

```
pub fn generate_secret() -> string
```

The secret of 20 random bytes (the size RFC 4226 recommends) as base32 text, 32 characters, the way it is shown to a user and stored.

### `base32_encode`

```
pub fn base32_encode(data: [byte]) -> string
```

`data` as base32 (RFC 4648) without `=` padding.

### `base32_decode`

```
pub fn base32_decode(text: string) -> Result<[byte], string>
```

base32 text as bytes: upper or lower case, with or without `=` padding, spaces and dashes ignored (as people write a secret down). An Err
for any other character.

### `hotp`

```
pub fn hotp(key: [byte], counter: int, digits: int) -> string
```

The HOTP value (RFC 4226) for `counter`, `digits` (6 to 9) long with leading zeros: HMAC-SHA-1, then the "dynamic truncation".

### `totp`

```
pub fn totp(key: [byte], unix_seconds: int) -> string
```

The six-digit code an authenticator app shows at `unix_seconds` (30-second steps from the epoch).

### `totp_matching_step`

```
pub fn totp_matching_step(key: [byte], code: string, unix_seconds: int, window: int) -> Option<int>
```

The 30-second step number `code` matches, at `unix_seconds` or within `window` steps of it either way (1
allows for a clock a few seconds off), or `None` when it matches none: what a caller needs to reject a
replay of a step already spent (store the step a login accepted; refuse a future call that matches the
same one). The comparison takes the same time whatever the code, and every step in the window is checked
even after a match, so the time taken does not say which step (or whether one) matched.

### `totp_valid`

```
pub fn totp_valid(key: [byte], code: string, unix_seconds: int, window: int) -> bool
```

Whether `code` is the code at `unix_seconds` or within `window` 30-second steps of it either way (1 allows for a clock a few seconds
off). The comparison takes the same time whatever the code.

### `provisioning_uri`

```
pub fn provisioning_uri(issuer: string, account: string, secret: string) -> string
```

The `otpauth://` address an authenticator app takes as a QR code to add the account: `issuer` (the name of your service) and
`account` (the user's email) label it, `secret` is the base32 secret.
