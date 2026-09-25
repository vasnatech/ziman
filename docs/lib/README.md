# Bundled library manual

One page per module. Every page is generated from the module's own source -- its header
comment is the introduction and each `pub` item's comment is its entry -- so it is kept
true by `make corelib-reference`. For the language itself see [SYNTAX.md](../SYNTAX.md); for
a bare signature list see [CORELIB.md](../CORELIB.md).

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
| [`uuid`](uuid.md) | UUIDs (RFC 4122): random version 4, time-ordered version 7, and the canonical text form. |
| [`regex`](regex.md) | Regular expressions: a native engine that runs in time linear in the text (no backreferences), with patterns compiled once and cached. |
| [`compress`](compress.md) | Gzip and zlib (deflate) compression, on zlib. |
| [`crypto`](crypto.md) | Hashes, message authentication, key derivation and base64, on OpenSSL's libcrypto. |

## Time

| Module | What it is |
|---|---|
| [`time`](time.md) | Dates, times, durations and instants, modeled on java.time and written in ordinary Ziman. |
| [`clock`](clock.md) | The system clock and sleeping, the two things Ziman cannot do in pure code. |

## System

| Module | What it is |
|---|---|
| [`fs`](fs.md) | The filesystem: files, directories and paths. |
| [`os`](os.md) | The process and its environment. |
| [`log`](log.md) | Levelled logging to stderr or to a file. |
| [`random`](random.md) | Randomness from the operating system. |

## Networking

| Module | What it is |
|---|---|
| [`net`](net.md) | Blocking TCP: connect, listen/accept, read, write, close. |

## HTTP

| Module | What it is |
|---|---|
| [`http`](http.md) | What an HTTP server and an HTTP client have in common. |
| [`server`](server.md) | An HTTP/1.1 *server* toolkit, written in Ziman on top of `net`. |
| [`client`](client.md) | An HTTP/1.1 *client*, written in Ziman on top of `net`, so a Ziman program can call another service. |

## Databases

| Module | What it is |
|---|---|
| [`sql`](sql.md) | The value/row/error vocabulary shared by the `db` driver module and by anything that maps its own types to and from SQL. |
| [`db`](db.md) | A database driver layer, the shape of JDBC. |
| [`sqlite`](sqlite.md) | The SQLite driver behind `db`. |
| [`pg`](pg.md) | A PostgreSQL client, the second backend behind the `db` module. |
| [`mysql`](mysql.md) | A MySQL / MariaDB client, the third engine behind the `db` module. |
| [`dburl`](dburl.md) | Parsing of database URLs shared by the network drivers. |

## Always available

| Module | What it is |
|---|---|
| [`prelude`](prelude.md) | The functions and types every Ziman program has without a `use`. |
