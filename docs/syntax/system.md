# Files, logging, cryptography and configuration

Part of the [v0 syntax reference](README.md).

## Files (`fs`)

`fs.read_text`/`write_text`/`write_text_atomic`/`append_text` and their `_bytes` twins;
`exists`/`is_dir`/`is_file`/`size`/`modified_seconds`/`list_dir` (sorted); `make_dir` (one level)
/`make_dirs` (like `mkdir -p`)/`rename`/`copy_file`; `remove` (a file or an *empty* directory) and
`remove_all` (a path and everything under it). `fs.lines(path)` reads a file lazily line by
line as an `Iter` (memory for one line, any file size); `fs.chunks(path, size)` the same for
byte blocks (`Iter<Result<[byte], string>>`); `fs.walk(dir)` finds files under a directory depth
first, sorted, without listing them all first (`Iter<Result<string, string>>`; directories not
listed, a symlink to one not followed).

- **Everything fallible returns `Result<_, FsError>`** — an enum of `NotFound`, `Denied`,
  `IsDirectory`, `NotDirectory`, `Exists` and `Other`, each carrying the OS's own message, so the
  three cases a program branches on are variants rather than substrings of `strerror`.
  `fs.fs_error_message(e)` is the text; the name keeps its prefix because `error_message`
  dispatches to `<type>_message` by name.
- **`remove_all` unlinks a symbolic link, it does not follow one.** It is implemented against
  `lstat` for that reason: `is_dir` calls `stat`, which follows a link, so the obvious Ziman
  implementation would descend a link pointing out of the tree and delete the target's contents.
  It is `Ok(unit)` for a path that is not there, which is what makes it usable as an unguarded
  cleanup; a failure stops and names the path that failed, there being no useful "mostly deleted"
  answer.
- **`fs.temp_dir()`** is `$TMPDIR` (without a trailing slash) or `/tmp` — a directory, not a
  unique path inside one, so combine it with something of your own via `path_join`.
- **`fs.path_join(pieces...)`/`file_name`/`parent_dir`/`extension`** are the path helpers, all
  pure string work with no filesystem access.

`fs.resolve(name) -> Option<string>` finds `name` by searching `ZIMAN_RESOURCE_PATH` (a
colon-separated list, first match wins, like `PATH`/a Java classpath). Unset, it returns
`Some(name)` unchanged; set with no match, `None`. `properties.load`/`migrate.run` both resolve
through this, so e.g. `ZIMAN_RESOURCE_PATH=src/main/resources` for an app and
`.../test/resources:.../main/resources` for its tests both find `application.properties` with
no code change.

## Logging (`log`)

No global logger (no mutable globals): build one with `log.logger(level: Level.Info, file: ...,
timestamps: ..., prefix: ...)` and pass it around. `log.debug/info/warn/error(logger, message)`
write `<timestamp> LEVEL prefix message` to stderr, or append to `file` (falling back to
stderr); below the logger's level is dropped. `with_prefix` makes a per-component logger,
`log.parse_level("warn")` reads a level from configuration; `log.format(level)` is its name
("DEBUG"/"INFO"/"WARN"/"ERROR", unpadded -- the text format pads its own columns) and
`log.rank(level)` its order.

`log.logger(..., json: true)` writes one JSON object per line instead -- `{"time":...,"level":
"info","prefix":...,"request_id":...,"message":...}` -- for a log something else reads. The level is
lowercased and the prefix trimmed, since the text format's column padding is presentation and has no
business in an indexed field; `prefix` and `request_id` are left out entirely when empty rather than
written as `null`. `with_request_id(logger, id)` stamps every line a logger writes from then on, so a
request's handler sets it once instead of threading it through every call -- the same shape as
`with_prefix`, and an empty id clears it. Escaping goes through `json`, so a message carrying
arbitrary bytes from a user is still a document another parser accepts (the text format's `\xNN` for
a control byte is deliberately not valid JSON, and a hand-rolled escaper here would reproduce the
invalid-UTF-8 bug A8 fixed).

A logger with `file` can rotate: `log.logger(..., max_bytes: 10485760, keep_files: 5)`. A file
at `max_bytes` is renamed `<file>.1` before the next write to a fresh `<file>` (`.1`→`.2`, ... up
to `.<keep_files>`, the one past that deleted; `keep_files: 0` just deletes). Without
`max_bytes` (default), nothing rotates. Safe with several threads logging to one file -- a lock
around rotation, size re-checked under it.

## Cryptography

The `crypto` core module is hashes, MAC, key derivation, password hashing and base64 on
OpenSSL's libcrypto (already linked for TLS) and, for bcrypt, libxcrypt; `[byte]` in and out.

- **Hashes**: `sha1` (broken -- legacy formats and MySQL's login only), `sha256`, `sha512`.
- **Keyed**: `hmac_sha256`, `hmac_sha512`, `pbkdf2_hmac_sha256`/`pbkdf2_hmac_sha512(password,
  salt, iterations, length)` (password to key or storable hash).
- **Passwords**: `password_hash(password)` (Argon2id, PHC format, fresh random salt inside),
  `password_verify(password, stored)` (checks Argon2id or bcrypt `$2a$`/`$2b$`/`$2y$`, the
  hashes Spring Security's `BCryptPasswordEncoder` writes), `password_needs_rehash(stored)`
  (true for bcrypt or weaker Argon2id). Parts are public too: `bcrypt_hash(password:, cost:)`/
  `bcrypt_verify`, `argon2id_hash(password:, memory_kib:, iterations:, parallelism:)`/
  `argon2id_verify`/`argon2id_raw`. Comparisons are constant-time.
- **Bytes**: `bytes_xor` (no bitwise operators in the language) and `bytes_equal`
  (constant-time -- use for MACs/tokens, not `==`).
- **base64**: `base64_encode`/`base64_decode` (standard, padded, strict);
  `base64url_encode`/`base64url_decode` (URL-safe, unpadded -- JWTs). Hex is in `number`
  (`number.to_hex`/`bytes_from_hex`).
- **Errors**: a wrong argument (mismatched lengths, an out-of-range iteration count) is a
  runtime error; malformed data (`base64_decode`, `bytes_from_hex`) is `Err(string)`.
- Not a general crypto library -- no ciphers, no public-key operations.

### JSON Web Tokens (`jwt`)

`jwt` signs/verifies with a shared secret (HS256/384/512), on `crypto`/`json`/`clock`.

```ziman fragment
use crypto.base64_decode
use json.Json
use jwt

val secret = "c2hhcmVkLXNlY3JldC1vZi0zMi1ieXRlcy0xMjM0NTY="   // 32 random bytes, base64
val key = unwrap(base64_decode(secret))
val claims = jwt.with_claim(jwt.claims("ada@example.com", 3600), "role", Json.JString("admin"))
val token = unwrap(jwt.sign(claims, key))
switch jwt.verify(token, key) {
    Ok(c) { print(unwrap_Some(jwt.subject(c))) }
    Err(e) { print(error_message(e)) }
}
```

- **`claims(subject, expires_in_seconds)`** gives `sub`/`iat`/`exp`; `with_claim` adds more.
  `sign` picks the algorithm from key size (32/48/64 bytes -> HS256/384/512); a third argument,
  `sign(claims, key, "HS512")`, chooses explicitly -- one name, told apart by arity (it was
  `sign_with` until 2026-10-24).
- **`verify(token, key)`** returns claims or a `JwtError` (`Malformed`, `UnsupportedAlgorithm`,
  `BadSignature`, `Expired`, `NotYetValid`, `MissingClaim(name)`, `WrongIssuer`,
  `WrongAudience`). Refuses `alg: none` and non-HS algorithms, compares the signature
  constant-time, checks `exp`/`nbf`; `verify_at(token, key, now, leeway)` chooses the clock.
  **A token must have `exp`** (else `MissingClaim("exp")`); a non-numeric `exp`/`nbf`
  (`"exp": "never"`) is `Malformed`, never "no expiry".
- **`verify_claims`** is `verify` with every check named: `jwt.verify_claims(token:, key:,
  issuer:, audience:)`, plus `leeway_seconds` (0), `now` (current time), `require_exp` (true --
  `false` only for an open-ended token) and `algorithm` (unpinned). A given `issuer` must equal
  `iss`, a given `audience` must equal or be one of `aud`; absence is `MissingClaim("iss"/"aud")`
  -- what stops a token for one service being accepted by another sharing the key. A given
  `algorithm` must equal the header's `alg`, checked *before* the MAC is computed
  (`UnsupportedAlgorithm` otherwise), so a deployment signing HS512 can refuse a valid HS256
  token over the same key. **It is opt-in, and not a vulnerability being closed**: the attack
  that makes a trusted `alg` header dangerous needs a mix of symmetric and asymmetric
  algorithms, and with HS256/384/512 as the whole set, switching `alg` still needs the key. `jwt.issuer`/`audience(claims)`
  read them (audience always a list); `sub` is `jwt.subject`.
- Tokens from other libraries verify and vice versa (checked against RFC 7515's example).

### Base-N encodings (`encoding`)

One `<base>_encode(data: [byte]) -> string` and `<base>_decode(text) -> Result<[byte], string>`
per base -- `encoding.base2_encode`, `base8_`, `base16_`, `base32_`, `base64_` and `base64url_`. Added 2026-10-24, gathering what had been spread
across three modules: base64 in `crypto`, base32 in `otp`, hex in `number`. The old names are
`#[deprecated]` delegates, so nothing breaks and `--lint` says where each went.

- **The five split into two families, and it matters when you cut a string.** `base2`, `base8` and
  `base16` are **fixed width per byte** -- 8, 3 and 2 digits, each byte rendered on its own -- so
  the text length is always the byte count times the width, and a cut on any digit boundary still
  decodes. `base32` and `base64` are **RFC 4648 bit packing**, 5 or 6 bits taken across byte
  boundaries, so neither holds and the last character carries leftover bits.
- **`base8` is the one with no RFC**, and the per-byte reading is a choice: three octal digits hold
  nine bits where a byte has eight, so `400` to `777` name no byte and `base8_decode` rejects them.
  Per-byte makes it a sibling of `base2`/`base16` rather than a third packing scheme.
- **`base16_encode` is `number.to_hex`** for `[byte]`, which is why both exist: `number`'s also
  takes an `int`, a `bigint` and a single `byte`, which a byte-stream encoder has no business doing.
  One implementation, two doors with different jobs.
- **`base64url`** is RFC 4648 §5: `-` and `_` for `+` and `/`, no padding -- what JWT uses.
  `base64_encode([251b, 255b])` is `+/8=` where `base64url_encode` is `-_8`.
- Every one of the 256 byte values round-trips through all five, pinned by
  `tests/lang/encoding_bases.zm`.

### One-time passwords (`otp`)

`otp` makes/checks TOTP codes (RFC 6238: HMAC-SHA-1, 30-second steps), pure Ziman on `crypto`.

```ziman fragment
use otp

val typed_code = "492039"
val now_seconds = 1760000000
val secret = otp.generate_secret()                                   // base32, shown to the user once
val key = unwrap(otp.base32_decode(secret))
val ok = otp.totp_valid(key, typed_code, now_seconds, 1)             // this step and one either side
```

`hotp(key, counter, digits)`/`totp(key, unix_seconds)` give the codes; `provisioning_uri("My
App", email, secret)` builds an `otpauth://totp/...` URI for a QR code; `base32_encode`/`_decode`
are public. Checks compare constant-time, and every step in the window is checked even after a
match so the time taken does not say which step matched; tests use the RFC 4226/6238 vectors.
**Discarding a `totp_valid` answer is a compile error** -- it is a pure function with no other
effect, and throwing away the one thing a verification tells you is the mistake worth catching.

- **`digits` must be 6 to 9**, enforced rather than only documented. Dynamic truncation (RFC 4226
  §5.3) yields a 31-bit number, at most 2147483647, so nine is the most that carries real entropy
  -- a tenth digit could only ever be 0, 1 or 2, which makes a longer code look stronger than it
  is. Out of range is a runtime error naming the bound.
- **`totp`, `totp_valid` and `totp_matching_step` each take an optional `digits`** as a last
  argument, six when omitted. RFC 6238's own test vectors are eight digits, and until 2026-10-24
  this module could *generate* an eight-digit code (through `hotp` by hand) but not verify one,
  both verifiers going through the six-digit `totp`.

## Configuration (`properties`, `os`)

`os.get_env(name) -> Option<string>` reads an environment variable (`Some("")` set-but-empty,
`None` unset); `properties` builds Spring-Boot-style configuration on it.

The rest of `os` is for ending cleanly: `os.exit(code)` flushes stdout/stderr then exits;
`os.write_stderr(text)` (no newline added); `os.install_shutdown_handler()` makes SIGINT/SIGTERM
set a flag instead of killing the process, `os.shutdown_requested() -> bool` reads it, and
`net.accept` errors within ~0.25s of the signal so an accept loop can stop, drain, and exit:

```ziman ignore: `{ ... }` stands in for what the loop does with each client
os.install_shutdown_handler()
while !os.shutdown_requested() {
    val client = net.accept(listener)
    if is_ok(client) { ... }
}
```

The signal can arrive on any thread, so only `accept` notices it automatically -- other blocking
work needs its own arrangement (a socket timeout, say).

```ziman
use properties

#[derive(Properties)]
struct ServerConfig { port: int  idle_timeout_ms: int  banner: Option<string> }

fn load_config() -> Result<ServerConfig, string> {
    val loaded = try properties.load("application")     // application.properties (+ -<profile>)
    return server_config_from_properties(loaded, "server")
}
```

- **Format**: `key=value` lines, `#` comments only at line start, lowercase dotted kebab-case
  keys, trimmed values, no quoting/escapes/inline comments. A duplicate key, uppercase key, or
  malformed line is an error naming `file:line`.
- **Loading**: `load("application")` reads `application.properties` (required), then
  `application-<profile>.properties` if a profile is active (env `PROFILE_ACTIVE`, else
  `profile.active` in the base file; a missing profile file errors). `parse(text, source)` is
  the pure form. Both file names resolve through `fs.resolve` first, so `ZIMAN_RESOURCE_PATH`
  finds them wherever it points, unchanged code otherwise.
- **Precedence**: environment (`server.port` -> `SERVER_PORT`) > profile file > base file.
- **Placeholders** (resolved once at load/parse, so a bad one fails startup): `${name}`/
  `${name:default}` resolve against properties, then the environment, then the default (which
  may contain `:`); unresolved or cyclic is an error.
- **Getters**: `get_string/int/float/bool`, each over two shapes -- a `Properties` plus a required
  key, or an `Entry` you already have from `lookup(props, key) -> Option<Entry>`, which is how an
  optional key is read. One name per type, told apart by the arguments (they were `entry_*` and
  `get_*` until 2026-10-24); the type suffix stays because all four take the same arguments and
  differ only in what they answer, and `#[derive(Properties)]` builds the name from the field's
  type. Errors name the key and source (`server.
  port: '80x' is not an integer (application.properties:3)`).
- **`check_unknown(props, known_keys)`** fails on keys outside the list, suggesting the closest
  match; `profile.active` is always allowed.
- **`report(props, keys)`** returns effective values with sources for a startup log; secrets
  (keys containing `password`/`secret`/`token`/`key`, and URL credentials) are masked.
- **`#[derive(Properties)]`** on a struct of `int`/`float`/`bool`/`string` (or `Option` of
  those) generates `<snake>_from_properties(props, prefix) -> Result<T, string>` and
  `<snake>_keys(prefix) -> [string]`. `idle_timeout_ms` binds to `<prefix>.idle-timeout-ms`;
  `Option<T>` fields are optional, others required. Needs only `use properties`.
