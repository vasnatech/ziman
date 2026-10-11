# Bundled library manual

One page per module. Every page is generated from the module's own source -- its header
comment is the introduction and each `pub` item's comment is its entry -- so it is kept
true by `make corelib-reference`. For the language itself see [SYNTAX.md](../SYNTAX.md); for
a bare signature list see [CORELIB.md](../CORELIB.md), and for which module uses which see
[dependencies.md](dependencies.md).

Any program can `use` these with no local file. Names are flat: a module is called by its file
name (`use db`, `use pg`), whatever folder it lives in. A file of your own with the same name
wins over the bundled one.

Everything that can fail returns a `Result`, and an absent value is an `Option`; nothing here
throws. See [SYNTAX.md](../SYNTAX.md) for `?`, `Result` and `Option`.

## Data and formats

| Module | What it is |
|---|---|
| [`collections`](collections.md) | Sorting, searching and higher-order helpers for arrays, in ordinary Ziman. |
| [`json`](json.md) | JSON as a `Json` tree: a native parser and serializer, and Ziman helpers for reading and building trees. |
| [`ndjson`](ndjson.md) | Newline-delimited JSON: one JSON value per line, written on top of `json`. |
| [`csv`](csv.md) | CSV (RFC 4180) as `Json`: a table is a `Json` array of objects, one per data row and keyed by the header row, so any `#[derive(Json)]` type decodes from a row with its own `_from_json` and encodes back. |
| [`toml`](toml.md) | TOML 1.0 on top of `json`: a document is a `Json` object, so any `#[derive(Json)]` type decodes from it with its own `_from_json`. |
| [`yaml`](yaml.md) | A strict YAML 1.2 subset on top of `json`. |
| [`properties`](properties.md) | Spring-Boot-style configuration. |
| [`number`](number.md) | Numbers as text and as bytes: formatting and parsing with separators, hex, shortest-text floats, and the fixed-width big-endian integers binary protocols use. |
| [`math`](math.md) | Floating-point math beyond the four basic operators, the rounding functions. |
| [`bits`](bits.md) | Bitwise and/or/xor/not/shift on `byte`, `int` and `bigint`, and the two scalar-backed sets they back, `BitSet`. |
| [`uuid`](uuid.md) | UUIDs (RFC 4122): random version 4, time-ordered version 7, and the canonical text form. |
| [`regex`](regex.md) | Regular expressions: a native engine that runs in time linear in the text (no backreferences), with patterns compiled once and cached. |
| [`compress`](compress.md) | Gzip and zlib (deflate) compression, on zlib. |
| [`crypto`](crypto.md) | Hashes, message authentication, key derivation, password hashing, one authenticated cipher and base64, on OpenSSL's libcrypto. |
| [`jwt`](jwt.md) | JSON Web Tokens (RFC 7519) signed with a shared secret: HS256, HS384 and HS512 (RFC 7518), the algorithms that need only a key. |
| [`otp`](otp.md) | Time-based one-time passwords (RFC 6238, over RFC 4226's HOTP) of the kind authenticator apps show: six digits that change every 30 seconds, from a secret shared once, by QR code or by typing it. |
| [`mime`](mime.md) | Media types (MIME types, RFC 6838): `application/json`, `text/html; charset=utf-8`. |

## Time

| Module | What it is |
|---|---|
| [`time`](time.md) | Dates, times, durations and instants, modeled on java.time and written in ordinary Ziman. |
| [`zone`](zone.md) | The IANA time zone database, read from the operating system's compiled zone files. |

## System

| Module | What it is |
|---|---|
| [`fs`](fs.md) | The filesystem: files, directories and paths. |
| [`os`](os.md) | The process and its environment. |
| [`process`](process.md) | Starting another program, waiting for it, signalling it. |
| [`thread`](thread.md) | What a thread can ask about itself, and values that are different on every thread. |
| [`log`](log.md) | Levelled logging to stderr or to a file. |
| [`metrics`](metrics.md) | Counters, gauges and histograms that the threads of a server share, and a render of them in the Prometheus text format for something to scrape. |
| [`random`](random.md) | Randomness from the operating system. |
| [`clock`](clock.md) | The system clock and sleeping, the two things Ziman cannot do in pure code. |

## Networking

| Module | What it is |
|---|---|
| [`net`](net.md) | Blocking TCP: connect, listen/accept, read, write, close. |
| [`url`](url.md) | The percent-encoding both `http` and `db` need, in one place. |

## HTTP

| Module | What it is |
|---|---|
| [`http`](http.md) | What an HTTP server and an HTTP client have in common. |
| [`server`](server.md) | An HTTP/1.1 *server* toolkit, written in Ziman on top of `net`. |
| [`client`](client.md) | An HTTP/1.1 *client*, written in Ziman on top of `net`, so a Ziman program can call another service. |

## Databases

| Module | What it is |
|---|---|
| [`db`](db.md) | What every database module shares. |
| [`datasource`](datasource.md) | The module a program calls to use a database, the shape of JDBC. |
| [`migrate`](migrate.md) | Versioned schema migrations: a directory of plain `.sql` files, applied in order, once each. |
| [`sqlite`](sqlite.md) | The SQLite driver behind `datasource`. |
| [`pg`](pg.md) | A PostgreSQL client, the second backend behind the `datasource` module. |
| [`mysql`](mysql.md) | A MySQL / MariaDB client, the third engine behind the `datasource` module. |

## Testing

| Module | What it is |
|---|---|
| [`test`](test.md) | Assertions for #[test] functions. |

## Always available

| Module | What it is |
|---|---|
| [`prelude`](prelude.md) | The functions and types every Ziman program has without a `use`. |
