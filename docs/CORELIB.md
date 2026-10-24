# Ziman core module reference

Auto-generated from each `lib/core/*.zm` or `lib/db/*.zm` file's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub enum`/
`pub fn`/`pub native fn`/`pub val`). This is a signature lookup, not an
explanation -- see the manual in docs/lib/ for how to use each module, docs/SYNTAX.md for full semantics and docs/DESIGN.md
for why each module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/SYNTAX.md's "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.

One exception, not listed below: `lib/prelude/` -- unlike every
module here, it's never `use`-able at all (see its own header comment
and docs/DESIGN.md's "Prelude" entry), since everything it declares
(`Option<T>`, plus a handful of native functions) is already bare and
unqualified in every program, the same way `print`/`str`/`parse_int`
are. See docs/SYNTAX.md's "Modules" section for what it contains.

## `bits`

```
pub native fn byte_and(a: byte, b: byte) -> byte

pub native fn byte_or(a: byte, b: byte) -> byte

pub native fn byte_xor(a: byte, b: byte) -> byte

pub native fn byte_not(a: byte) -> byte

pub native fn byte_shift_left(a: byte, n: int) -> byte

pub native fn byte_shift_right(a: byte, n: int) -> byte

pub native fn byte_popcount(a: byte) -> int

pub native fn int_and(a: int, b: int) -> int

pub native fn int_or(a: int, b: int) -> int

pub native fn int_xor(a: int, b: int) -> int

pub native fn int_not(a: int) -> int

pub native fn int_shift_left(a: int, n: int) -> int

pub native fn int_shift_right(a: int, n: int) -> int

pub native fn bigint_and(a: bigint, b: bigint) -> bigint

pub native fn bigint_or(a: bigint, b: bigint) -> bigint

pub native fn bigint_xor(a: bigint, b: bigint) -> bigint

pub native fn bigint_not(a: bigint) -> bigint

pub native fn bigint_shift_left(a: bigint, n: int) -> bigint

pub native fn bigint_shift_right(a: bigint, n: int) -> bigint

pub native fn int_popcount(a: int) -> int

pub native fn bigint_popcount(a: bigint) -> int

pub native fn bigint_bit_length(a: bigint) -> int

pub struct BitSet {
    bits: int
}

pub fn bitset_new() -> BitSet

pub fn bitset_of_int(x: int) -> BitSet

pub fn bitset_to_int(bs: BitSet) -> int

pub fn bitset_set(bs: BitSet, i: int) -> BitSet

pub fn bitset_clear(bs: BitSet, i: int) -> BitSet

pub fn bitset_flip(bs: BitSet, i: int) -> BitSet

pub fn bitset_get(bs: BitSet, i: int) -> bool

pub fn bitset_and(a: BitSet, b: BitSet) -> BitSet

pub fn bitset_or(a: BitSet, b: BitSet) -> BitSet

pub fn bitset_xor(a: BitSet, b: BitSet) -> BitSet

pub fn bitset_and_not(a: BitSet, b: BitSet) -> BitSet

pub fn bitset_is_empty(bs: BitSet) -> bool

pub fn bitset_cardinality(bs: BitSet) -> int

pub fn bitset_length(bs: BitSet) -> int

pub fn bitset_to_string(bs: BitSet) -> string

pub struct BigBitSet {
    bits: bigint
}

pub fn big_bitset_new() -> BigBitSet

pub fn big_bitset_of_bigint(x: bigint) -> BigBitSet

pub fn big_bitset_to_bigint(bs: BigBitSet) -> bigint

pub fn big_bitset_set(bs: BigBitSet, i: int) -> BigBitSet

pub fn big_bitset_clear(bs: BigBitSet, i: int) -> BigBitSet

pub fn big_bitset_flip(bs: BigBitSet, i: int) -> BigBitSet

pub fn big_bitset_get(bs: BigBitSet, i: int) -> bool

pub fn big_bitset_and(a: BigBitSet, b: BigBitSet) -> BigBitSet

pub fn big_bitset_or(a: BigBitSet, b: BigBitSet) -> BigBitSet

pub fn big_bitset_xor(a: BigBitSet, b: BigBitSet) -> BigBitSet

pub fn big_bitset_and_not(a: BigBitSet, b: BigBitSet) -> BigBitSet

pub fn big_bitset_is_empty(bs: BigBitSet) -> bool

pub fn big_bitset_cardinality(bs: BigBitSet) -> int

pub fn big_bitset_length(bs: BigBitSet) -> int

pub fn big_bitset_to_string(bs: BigBitSet) -> string

```

## `clock`

One native function reading the system clock (`clock_gettime`) -- Ziman's other source of genuine non-determinism.

```
pub native fn now_raw() -> [int]

pub native fn now_iso() -> string

pub native fn now_http_date() -> string

pub native fn monotonic_millis() -> int

pub native fn sleep_millis(ms: int)

```

## `compress`

```
pub fn gzip(data: [byte]) -> [byte]

pub fn deflate(data: [byte]) -> [byte]

pub fn gunzip(data: [byte], max_size: int) -> Result<[byte], string>

pub fn inflate(data: [byte], max_size: int) -> Result<[byte], string>

pub fn decompress(data: [byte], max_size: int) -> Result<[byte], string>

pub fn inflate_raw(data: [byte], max_size: int) -> Result<[byte], string>

```

## `crypto`

Hashes (SHA-1/256/384/512), HMAC, PBKDF2, password hashing (bcrypt, Argon2id), a constant-time byte comparison, base64 and base64url, on OpenSSL. No ciphers or public-key operations.

```
pub native fn sha1(data: [byte]) -> [byte]

pub native fn sha256(data: [byte]) -> [byte]

pub native fn sha384(data: [byte]) -> [byte]

pub native fn sha512(data: [byte]) -> [byte]

pub native fn hmac_sha1(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha256(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha384(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha512(key: [byte], data: [byte]) -> [byte]

pub native fn pbkdf2_hmac_sha256(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]

pub native fn pbkdf2_hmac_sha512(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]

pub fn bcrypt_hash(password: string, cost: Option<int> = 10) -> Result<string, string>

pub native fn bcrypt_verify(password: string, hash: string) -> bool

pub native fn argon2id_raw(

pub fn argon2id_hash(

pub fn argon2id_verify(password: string, encoded: string) -> bool

pub fn password_hash(password: string) -> Result<string, string>

pub fn password_verify(password: string, stored: string) -> bool

pub fn password_needs_rehash(stored: string) -> bool

pub native fn aes_gcm_encrypt(key: [byte], plaintext: [byte]) -> Result<[byte], string>

pub native fn aes_gcm_decrypt(key: [byte], blob: [byte]) -> Result<[byte], string>

pub native fn bytes_xor(a: [byte], b: [byte]) -> [byte]

pub native fn bytes_equal(a: [byte], b: [byte]) -> bool

pub native fn base64_encode(data: [byte]) -> string

pub native fn base64_decode(s: string) -> Result<[byte], string>

pub native fn base64url_encode(data: [byte]) -> string

pub native fn base64url_decode(s: string) -> Result<[byte], string>

```

## `csv`

RFC 4180 CSV as a `Json` array of objects keyed by the header row (typed or string cells; TSV via the `_with` variants); a table writer that flattens one level.

```
pub fn parse_csv_rows(text: string) -> Result<[[string]], string>

pub fn parse_csv_rows_with(text: string, delimiter: string) -> Result<[[string]], string>

pub fn parse_csv(text: string) -> Result<Json, string>

pub fn parse_csv_with(text: string, delimiter: string) -> Result<Json, string>

pub fn parse_csv_typed(text: string) -> Result<Json, string>

pub fn parse_csv_typed_with(text: string, delimiter: string) -> Result<Json, string>

pub fn parse_csv_columns(text: string, columns: variadic string) -> Result<Json, string>

pub fn parse_csv_columns_typed(text: string, columns: variadic string) -> Result<Json, string>

pub fn records(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records_with(path: string, delimiter: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records_typed(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records_typed_with(path: string, delimiter: string) -> Result<Iter<Result<Json, string>>, string>

pub fn csv_rows_to_string(rows: [[string]]) -> string

pub fn csv_rows_to_string_with(rows: [[string]], delimiter: string) -> Result<string, string>

pub fn csv_to_string(rows: Json) -> Result<string, string>

pub fn csv_to_string_with(rows: Json, delimiter: string) -> Result<string, string>

```

## `fs`

```
pub enum FsError {
    NotFound(string)
    Denied(string)
    IsDirectory(string)
    NotDirectory(string)
    Exists(string)
    Other(string)
}

pub fn fs_error_message(e: FsError) -> string

pub fn read_text(path: string) -> Result<string, FsError>

pub fn lines(path: string) -> Result<Iter<Result<string, string>>, FsError>

pub fn chunks(path: string, size: int) -> Result<Iter<Result<[byte], string>>, FsError>

pub fn walk(dir: string) -> Result<Iter<Result<string, string>>, FsError>

pub fn write_text(path: string, text: string) -> Result<int, FsError>

pub fn append_text(path: string, text: string) -> Result<int, FsError>

pub fn write_text_atomic(path: string, text: string) -> Result<int, FsError>

pub fn read_bytes(path: string) -> Result<[byte], FsError>

pub fn write_bytes(path: string, data: [byte]) -> Result<int, FsError>

pub fn append_bytes(path: string, data: [byte]) -> Result<int, FsError>

pub native fn exists(path: string) -> bool

pub native fn is_dir(path: string) -> bool

pub native fn is_file(path: string) -> bool

pub fn size(path: string) -> Result<int, FsError>

pub fn modified_seconds(path: string) -> Result<int, FsError>

pub fn list_dir(path: string) -> Result<[string], FsError>

pub fn resolve(name: string) -> Option<string>

pub fn remove(path: string) -> Result<unit, FsError>

pub fn make_dir(path: string) -> Result<unit, FsError>

pub fn make_dirs(path: string) -> Result<unit, FsError>

pub fn rename(from: string, to: string) -> Result<unit, FsError>

pub fn copy_file(from: string, to: string) -> Result<int, FsError>

pub fn path_join(pieces: variadic string) -> string

pub fn file_name(path: string) -> string

pub fn parent_dir(path: string) -> string

pub fn extension(path: string) -> string

```

## `json`

A JSON parser/serializer (`Json`, a genuinely recursive enum), written in pure Ziman -- no native code needed. `\uXXXX` unicode escapes are read; `==`/`!=` on `Json` is the one known gap.

```
pub enum Number {
    Int(int)
    Float(float)
    Bigint(bigint)
    Decimal(decimal)
}

pub enum Json {
    JNull
    JBool(bool)
    JNumber(Number)
    JString(string)
    JArray([Json])
    JObject(Map<string, Json>)
}

pub fn parse_json(s: string) -> Result<Json, string>

pub fn parse_json_strict(s: string) -> Result<Json, string>

pub fn parse_json_bytes(data: [byte]) -> Result<Json, string>

pub fn parse_json_bytes_strict(data: [byte]) -> Result<Json, string>

pub fn parse_json_exact(s: string) -> Result<Json, string>

pub fn parse_json_bytes_exact(data: [byte]) -> Result<Json, string>

pub fn json_to_bytes(j: Json) -> [byte]

pub fn json_to_string(j: Json) -> string

pub fn json_object_get(j: Json, key: string) -> Option<Json>

pub fn json_get_path(j: Json, path: variadic string) -> Option<Json>

pub fn json_require_field(j: Json, key: string) -> Result<Json, string>

pub fn json_require_string_field(j: Json, key: string) -> Result<string, string>

pub fn json_require_int_field(j: Json, key: string) -> Result<int, string>

pub fn json_require_float_field(j: Json, key: string) -> Result<float, string>

pub fn json_require_bool_field(j: Json, key: string) -> Result<bool, string>

pub fn json_to_map<V>(j: Json, decode: fn(Json) -> Result<V, string>) -> Result<Map<string, V>, string>

pub fn json_from_map<V>(m: Map<string, V>, encode: fn(V) -> Json) -> Json

pub fn json_to_string_map(j: Json) -> Result<Map<string, string>, string>

pub fn json_to_pretty_string(j: Json, indent: int) -> string

pub fn json_to_json(j: Json) -> Json

pub fn json_from_json(j: Json) -> Result<Json, string>

pub fn int_to_json(n: int) -> Json

pub fn json_number(j: Json) -> Option<Number>

pub fn int_from_json(j: Json) -> Result<int, string>

pub fn bigint_to_json(n: bigint) -> Json

pub fn bigint_from_json(j: Json) -> Result<bigint, string>

pub fn float_to_json(f: float) -> Json

pub fn float_from_json(j: Json) -> Result<float, string>

pub fn decimal_to_json(d: decimal) -> Json

pub fn decimal_from_json(j: Json) -> Result<decimal, string>

pub fn bool_to_json(b: bool) -> Json

pub fn bool_from_json(j: Json) -> Result<bool, string>

pub fn string_to_json(s: string) -> Json

pub fn string_from_json(j: Json) -> Result<string, string>

```

## `jwt`

JSON Web Tokens signed with a shared secret (HS256/384/512): sign, and verify with expiry checks.

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

pub fn jwt_error_message(e: JwtError) -> string

pub fn algorithm_for_key(key: [byte]) -> string

pub fn claims(subject: string, expires_in_seconds: int) -> Json

pub fn with_claim(claims: Json, name: string, value: Json) -> Json

pub fn sign(claims: Json, key: [byte]) -> Result<string, string>

pub fn sign_with(claims: Json, key: [byte], algorithm: string) -> Result<string, string>

pub fn verify_at(token: string, key: [byte], now_seconds: int, leeway_seconds: int) -> Result<Json, JwtError>

pub fn verify(token: string, key: [byte]) -> Result<Json, JwtError>

pub fn verify_leeway(token: string, key: [byte], leeway_seconds: int) -> Result<Json, JwtError>

pub fn verify_claims(

pub fn subject(claims: Json) -> Option<string>

pub fn issuer(claims: Json) -> Option<string>

pub fn audience(claims: Json) -> [string]

```

## `locale`

**Experimental** (see docs/DESIGN.md "Experimental modules"). 

```
pub fn of(asked: string) -> Locale

pub fn name_of(l: Locale) -> string

pub fn root() -> Locale

pub fn system() -> Locale

pub fn default() -> Locale

pub fn set_default(l: Locale)

pub fn use_system() -> Locale

pub fn month_name(l: Locale, month: int, full: bool) -> string

pub fn day_name(l: Locale, day: int, full: bool) -> string

pub fn decimal_separator(l: Locale) -> string

pub fn thousands_separator(l: Locale) -> string

```

## `log`

```
pub enum Level {
    Debug
    Info
    Warn
    Error
}

pub struct Logger {
    level: Level
    file: Option<string>
    timestamps: bool
    prefix: string
    console: bool
    max_bytes: int
    keep_files: int
    // One JSON object per line instead of the text format, for a log a machine reads. See `write`
    // for the field names and why the escaping goes through `json` rather than `single_line`.
    json: bool
    // Carried on the logger rather than passed to every call, so a request's handler sets it once
    // (`with_request_id`) and every line it logs afterwards carries it -- the same shape as
    // `prefix`, and the reason this is a field and not a parameter. Empty means no id, and the
    // field is then left out of the object rather than written as null, which is friendlier to
    // whatever indexes the log.
    request_id: string
}

pub fn logger(

pub fn with_prefix(l: Logger, prefix: string) -> Logger

pub fn with_request_id(l: Logger, id: string) -> Logger

pub fn level_rank(level: Level) -> int

pub fn level_name(level: Level) -> string

pub fn level_parse(text: string) -> Result<Level, string>

pub fn enabled(l: Logger, level: Level) -> bool

pub fn write(l: Logger, level: Level, message: string)

pub fn debug(l: Logger, message: string) { write(l, Level.Debug, message) }

pub fn info(l: Logger, message: string) { write(l, Level.Info, message) }

pub fn warn(l: Logger, message: string) { write(l, Level.Warn, message) }

pub fn error(l: Logger, message: string) { write(l, Level.Error, message) }

```

## `markdown`

**Experimental** (see docs/DESIGN.md "Experimental modules"). Markdown as a document tree: a reader over a declared subset, a writer, and the escaping that makes a generated document safe. The writer is the point -- `|` in a table cell, the `*_[]` family in text, a leading `#` or `1.`, a code span fenced longer than whatever is inside it -- so nothing a caller puts in a string can change the structure of the output. Markdown has no syntax errors, so a reader cannot refuse what it does not handle: `parse_markdown` reads anything unsupported as text, and `parse_markdown_strict` returns an Err naming the line instead.

```
pub enum Align {
    AlignNone
    AlignLeft
    AlignRight
    AlignCenter
}

pub struct Link {
    text: [Inline]
    destination: string
    title: Option<string>
}

pub enum Inline {
    Text(string)
    Emphasis([Inline])
    Strong([Inline])
    Code(string)
    Link(Link)
    Image(Link)
    SoftBreak
    HardBreak
}

pub struct Heading {
    level: int
    content: [Inline]
}

pub struct CodeBlock {
    info: string
    text: string
}

pub struct ListItem {
    blocks: [Block]
}

pub struct List {
    ordered: bool
    start: int
    tight: bool
    items: [ListItem]
}

pub struct Table {
    headers: [[Inline]]
    alignments: [Align]
    rows: [[[Inline]]]
}

pub enum Block {
    Heading(Heading)
    Paragraph([Inline])
    CodeBlock(CodeBlock)
    BlockQuote([Block])
    List(List)
    Table(Table)
    ThematicBreak
}

pub struct Document {
    blocks: [Block]
}

pub fn document(blocks: [Block]) -> Document

pub fn heading(level: int, text: string) -> Block

pub fn paragraph(text: string) -> Block

pub fn code_block(info: string, text: string) -> Block

pub fn link(text: string, destination: string) -> Inline

pub fn image(alt: string, destination: string) -> Inline

pub fn text_table(headers: [string], alignments: [Align], rows: [[string]]) -> Result<Block, string>

pub fn text_list(ordered: bool, items: [string]) -> Block

pub fn markdown_to_string(doc: Document) -> Result<string, string>

pub fn markdown_to_bytes(doc: Document) -> Result<[byte], string>

pub fn block_to_string(block: Block) -> Result<string, string>

pub fn flatten_cell(text: string) -> string

pub fn parse_markdown(text: string) -> Result<Document, string>

pub fn parse_markdown_strict(text: string) -> Result<Document, string>

pub fn parse_markdown_bytes(data: [byte]) -> Result<Document, string>

pub fn text_of(doc: Document) -> string

```

## `math`

```
pub val pi = 3.14159265358979323846

pub val e = 2.71828182845904523536

pub native fn abs(x: int) -> int

pub native fn abs(x: float) -> float

pub native fn abs(x: bigint) -> bigint

pub native fn abs(x: decimal) -> decimal

pub native fn min<T: Ord>(values: variadic T) -> T

pub native fn max<T: Ord>(values: variadic T) -> T

pub native fn sqrt(x: float) -> float

pub native fn pow(base: float, exponent: float) -> float

pub native fn exp(x: float) -> float

pub native fn log(x: float) -> float

pub native fn sin(x: float) -> float

pub native fn cos(x: float) -> float

pub native fn tan(x: float) -> float

pub native fn asin(x: float) -> float

pub native fn acos(x: float) -> float

pub native fn atan(x: float) -> float

pub native fn atan2(y: float, x: float) -> float

pub native fn floor(x: float) -> int

pub native fn ceil(x: float) -> int

pub native fn round(x: float) -> int

pub native fn floor_float(x: float) -> float

pub native fn ceil_float(x: float) -> float

pub native fn round_float(x: float) -> float

pub native fn decimal_round(d: decimal, scale: int) -> decimal

pub native fn decimal_floor(d: decimal, scale: int) -> decimal

pub native fn decimal_ceil(d: decimal, scale: int) -> decimal

pub native fn decimal_pow(base: decimal, exponent: int) -> decimal

pub fn is_nan(x: float) -> bool

```

## `metrics`

```
pub fn counter(name: string, help: string) -> bool

pub fn gauge(name: string, help: string) -> bool

pub fn histogram(name: string, help: string, buckets: [float]) -> bool

pub fn increment(name: string)

pub fn add(name: string, delta: float)

pub fn set(name: string, value: float)

pub fn gauge_add(name: string, delta: float)

pub fn gauge_add_with(name: string, delta: float, labels: Map<string, string>)

pub fn observe(name: string, value: float)

pub fn increment_with(name: string, labels: Map<string, string>)

pub fn add_with(name: string, delta: float, labels: Map<string, string>)

pub fn set_with(name: string, value: float, labels: Map<string, string>)

pub fn observe_with(name: string, value: float, labels: Map<string, string>)

pub struct Series {
    handle: int
}

pub fn series(name: string, labels: Map<string, string>) -> Series

pub fn series_of(key_name: string, key: string) -> Series

pub fn plain_series(name: string) -> Series

pub fn increment_at(s: Series)

pub fn add_at(s: Series, delta: float)

pub fn set_at(s: Series, value: float)

pub fn observe_at(s: Series, value: float)

pub fn gauge_add_at(s: Series, delta: float)

pub fn render() -> string

pub fn reset()

pub fn record_runtime()

```

## `mime`

Media types (`type/subtype; name=value`): parse, write, match with wildcards, and the type of a file by its extension.

```
pub struct MediaType {
    type: string
    subtype: string
    params: Map<string, string>
}

pub fn application_json() -> MediaType

pub fn application_ndjson() -> MediaType

pub fn application_yaml() -> MediaType

pub fn application_toml() -> MediaType

pub fn application_xml() -> MediaType

pub fn application_form() -> MediaType

pub fn application_octet_stream() -> MediaType

pub fn application_pdf() -> MediaType

pub fn application_zip() -> MediaType

pub fn application_gzip() -> MediaType

pub fn text_plain() -> MediaType

pub fn text_html() -> MediaType

pub fn text_csv() -> MediaType

pub fn image_png() -> MediaType

pub fn image_jpeg() -> MediaType

pub fn image_gif() -> MediaType

pub fn image_svg() -> MediaType

pub fn parse(text: string) -> Result<MediaType, string>

pub fn essence(m: MediaType) -> string

pub fn to_string(m: MediaType) -> string

pub fn param(m: MediaType, name: string) -> Option<string>

pub fn charset(m: MediaType) -> Option<string>

pub fn with_param(m: MediaType, name: string, value: string) -> MediaType

pub fn suffix(m: MediaType) -> Option<string>

pub fn is_json(m: MediaType) -> bool

pub fn is_text(m: MediaType) -> bool

pub fn matches(pattern: MediaType, candidate: MediaType) -> bool

pub fn of_extension(extension: string) -> Option<MediaType>

pub fn of_path(path: string) -> Option<MediaType>

```

## `ndjson`

Newline-delimited JSON (one value per line) on top of `json`.

```
pub fn parse_ndjson(text: string) -> Result<[Json], string>

pub fn records(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn ndjson_to_string(items: [Json]) -> string

```

## `net`

```
pub enum NetError {
    ConnectionRefused(string)
    HostNotFound(string)
    Timeout(string)
    Closed(string)
    Io(string)
    Tls(string)
}

pub enum TlsVerify {
    Off
    Chain
    Full
}

pub struct Socket {
    handle: int
}

pub struct Listener {
    handle: int
}

pub fn net_error_message(e: NetError) -> string

pub fn connect(host: string, port: int) -> Result<Socket, NetError>

pub fn connect_timeout(host: string, port: int, timeout_ms: int) -> Result<Socket, NetError>

pub fn connect_unix(path: string) -> Result<Socket, NetError>

pub fn listen_unix(path: string) -> Result<Listener, NetError>

pub fn listen(host: string, port: int) -> Result<Listener, NetError>

pub fn listen_tls(host: string, port: int, cert_file: string, key_file: string) -> Result<Listener, NetError>

pub struct TlsServerOptions {
    cert_file: string
    key_file: string
    client_ca_file: Option<string>
    require_client_cert: bool
    alpn: [string]
}

pub fn tls_server_options(cert_file: string, key_file: string) -> TlsServerOptions

pub fn listen_tls_with_options(host: string, port: int, options: TlsServerOptions) -> Result<Listener, NetError>

pub fn listener_port(l: Listener) -> int

pub fn peer_address(s: Socket) -> Option<string>

pub fn wait_readable(sockets: [Socket], timeout_ms: int) -> [int]

pub fn accept(l: Listener) -> Result<Socket, NetError>

pub fn read(s: Socket, max: int) -> Result<[byte], NetError>

pub fn read_exact(s: Socket, count: int) -> Result<[byte], NetError>

pub fn write(s: Socket, data: [byte]) -> Result<int, NetError>

pub fn read_text(s: Socket, max: int) -> Result<string, NetError>

pub fn write_text(s: Socket, text: string) -> Result<int, NetError>

pub fn lines(s: Socket) -> Iter<Result<string, NetError>>

pub fn lines_with(s: Socket, max_line_bytes: int) -> Iter<Result<string, NetError>>

pub fn set_timeout(s: Socket, millis: int) -> Result<unit, NetError>

pub fn start_tls(s: Socket, host: string) -> Result<unit, NetError>

pub fn start_tls_with(s: Socket, host: string, verify: TlsVerify, ca_file: Option<string>) -> Result<unit, NetError>

pub struct TlsClientOptions {
    verify: TlsVerify
    ca_file: Option<string>
    cert_file: Option<string>
    key_file: Option<string>
    alpn: [string]
}

pub fn tls_client_options() -> TlsClientOptions

pub fn start_tls_with_options(s: Socket, host: string, options: TlsClientOptions) -> Result<unit, NetError>

pub fn connect_tls(host: string, port: int) -> Result<Socket, NetError>

pub fn connect_tls_with(

pub fn connect_tls_with_options(host: string, port: int, options: TlsClientOptions) -> Result<Socket, NetError>

pub fn negotiated_protocol(s: Socket) -> Option<string>

pub fn peer_certificate_subject(s: Socket) -> Option<string>

pub fn close(s: Socket)

pub fn close_listener(l: Listener)

```

## `number`

Shared formatting/parsing across `int`/`float`/`bigint`/`decimal`/`byte`, written in pure Ziman on top of each type's own `str()`. `NumberFormat`/`parse_pattern` cover thousands separators and forced decimal places (a small subset of DecimalFormat-style patterns); `byte_to_hex`/`int_to_hex`/`bigint_to_hex` (and their `_from_hex` inverses) cover hex for the three integral types.

```
pub val byte_min = 0b

pub val byte_max = 255b

pub val int_min = -9223372036854775808

pub val int_max = 9223372036854775807

pub val float_min = -1.7976931348623157e+308

pub val float_max = 1.7976931348623157e+308

pub val float_min_positive = 5e-324

pub native fn float_to_text(x: float) -> string

pub native fn byte_to_hex(b: byte) -> string

pub native fn byte_from_hex(s: string) -> Result<byte, string>

pub native fn int_to_hex(x: int) -> string

pub native fn int_from_hex(s: string) -> Result<int, string>

pub native fn bytes_to_hex(data: [byte]) -> string

pub native fn bytes_from_hex(s: string) -> Result<[byte], string>

pub fn bigint_to_hex(n: bigint) -> string

pub fn bigint_from_hex(s: string) -> Result<bigint, string>

pub fn parse_byte(s: string) -> Result<byte, string>

pub struct NumberFormat {
    thousands_separator: string
    decimal_separator: string
    decimal_places: Option<int>
}

pub fn number_format_for(l: Locale) -> NumberFormat

pub fn number_format_plain() -> NumberFormat

pub fn parse_pattern(pattern: string) -> NumberFormat

pub fn format_number(s: string, fmt: NumberFormat) -> string

pub fn parse_number(s: string, fmt: NumberFormat) -> string

pub fn format_int(x: int, fmt: NumberFormat) -> string

pub fn format_float(x: float, fmt: NumberFormat) -> string

pub fn format_bigint(x: bigint, fmt: NumberFormat) -> string

pub fn format_decimal(x: decimal, fmt: NumberFormat) -> string

pub fn format_byte(x: byte, fmt: NumberFormat) -> string

pub native fn u16_to_bytes(n: int) -> [byte]

pub native fn u32_to_bytes(n: int) -> [byte]

pub native fn u16_from_bytes(b: [byte], offset: int) -> int

pub native fn u32_from_bytes(b: [byte], offset: int) -> int

pub native fn i32_from_bytes(b: [byte], offset: int) -> int

pub native fn i64_from_bytes(b: [byte], offset: int) -> int

pub native fn f64_from_bytes(b: [byte], offset: int) -> float

pub native fn f32_from_bytes(b: [byte], offset: int) -> float

pub native fn f64_to_bytes(f: float) -> [byte]

```

## `os`

```
pub fn getenv(name: string) -> Option<string>

pub native fn set_env(name: string, value: string) -> bool

pub native fn args() -> [string]

pub fn cwd() -> Result<string, string>

pub native fn pid() -> int

pub native fn collect_garbage()

pub native fn collector_enabled() -> bool

pub native fn open_descriptors() -> int

pub native fn exit(code: int)

pub fn fatal(message: string)

pub native fn write_stderr(text: string)

pub native fn install_shutdown_handler()

pub native fn shutdown_requested() -> bool

pub native fn cpu_count() -> int

pub native fn hostname() -> string

pub native fn os_name() -> string

pub native fn os_arch() -> string

pub native fn load_average() -> Result<[float], string>

pub native fn uptime_seconds() -> Result<int, string>

pub native fn memory_total() -> Result<int, string>

pub native fn memory_available() -> Result<int, string>

pub native fn process_memory() -> Result<int, string>

pub native fn disk_total(path: string) -> Result<int, string>

pub native fn disk_free(path: string) -> Result<int, string>

pub native fn disk_used(path: string) -> Result<int, string>

pub struct CpuTimes {
    busy: int
    total: int
}

pub fn cpu_times() -> Result<CpuTimes, string>

pub fn cpu_usage_between(before: CpuTimes, after: CpuTimes) -> float

pub fn cpu_usage(sample_millis: int) -> Result<float, string>

pub native fn process_cpu_seconds() -> Result<float, string>

pub native fn process_uptime_seconds() -> Result<int, string>

pub native fn thread_count() -> int

```

## `otp`

Time-based one-time passwords (RFC 6238): the six-digit codes of an authenticator app -- generate a secret, a provisioning URI, and check a code.

```
pub fn generate_secret() -> string

pub fn base32_encode(data: [byte]) -> string

pub fn base32_decode(text: string) -> Result<[byte], string>

pub fn hotp(key: [byte], counter: int, digits: int) -> string

pub fn totp(key: [byte], unix_seconds: int) -> string

pub fn totp_matching_step(key: [byte], code: string, unix_seconds: int, window: int) -> Option<int>

pub fn totp_valid(key: [byte], code: string, unix_seconds: int, window: int) -> bool

pub fn provisioning_uri(issuer: string, account: string, secret: string) -> string

```

## `process`

```
pub struct Process {
    handle: int
}

pub struct EnvVar {
    name: string
    value: string
}

pub fn env(name: string, value: string) -> EnvVar

pub fn start(

pub fn wait(p: Process) -> Result<int, string>

pub fn terminate(p: Process) -> Result<unit, string>

pub fn kill(p: Process) -> Result<unit, string>

pub fn interrupt(p: Process) -> Result<unit, string>

pub fn wait_for(p: Process, millis: int) -> Result<Option<int>, string>

pub fn release(p: Process)

pub fn pid(p: Process) -> int

pub fn run(

pub fn output(

```

## `properties`

```
pub struct Entry {
    key: string
    value: string
    source: string
}

pub struct Properties {
    entries: [Entry]
    profile: string
}

pub fn lookup(props: Properties, key: string) -> Option<Entry>

pub fn parse(text: string, source: string) -> Result<Properties, string>

pub fn load(name: string) -> Result<Properties, string>

pub fn entry_string(e: Entry) -> Result<string, string>

pub fn entry_int(e: Entry) -> Result<int, string>

pub fn entry_float(e: Entry) -> Result<float, string>

pub fn entry_bool(e: Entry) -> Result<bool, string>

pub fn get_string(props: Properties, key: string) -> Result<string, string>

pub fn get_int(props: Properties, key: string) -> Result<int, string>

pub fn get_float(props: Properties, key: string) -> Result<float, string>

pub fn get_bool(props: Properties, key: string) -> Result<bool, string>

pub fn key(prefix: string, name: string) -> string

pub fn check_unknown(props: Properties, known: variadic string) -> Result<bool, string>

pub fn report(props: Properties, keys: variadic string) -> string

```

## `random`

One native function reading real OS entropy (`/dev/urandom`) -- Ziman's first source of genuine non-determinism.

```
pub native fn random_bytes(n: int) -> [byte]

```

## `regex`

```
pub struct Regex {
    handle: int
    groups: int
    pattern: string
    names: [string]
}

pub struct Match {
    start: int
    end: int
    text: string
    groups: [Option<string>]
    names: [string]
}

pub fn compile(pattern: string) -> Result<Regex, string>

pub fn is_match(re: Regex, text: string) -> bool

pub fn find_from(re: Regex, text: string, from: int) -> Option<Match>

pub fn find(re: Regex, text: string) -> Option<Match>

pub fn find_all(re: Regex, text: string) -> [Match]

pub fn group(m: Match, n: int) -> Option<string>

pub fn group_named(m: Match, name: string) -> Option<string>

pub fn replace(re: Regex, text: string, replacement: string) -> string

pub fn replace_all(re: Regex, text: string, replacement: string) -> string

pub fn split(re: Regex, text: string) -> [string]

pub fn escape(text: string) -> string

```

## `test`

```
pub fn fail(message: string)

pub fn assert(cond: bool)

pub fn assert_false(cond: bool)

pub fn assert_eq<T: Eq>(expected: T, actual: T)

pub fn assert_ne<T: Eq>(unexpected: T, actual: T)

pub fn assert_lt<T: Ord>(actual: T, bound: T)

pub fn assert_le<T: Ord>(actual: T, bound: T)

pub fn assert_gt<T: Ord>(actual: T, bound: T)

pub fn assert_ge<T: Ord>(actual: T, bound: T)

pub fn assert_some<T>(opt: Option<T>) -> T

pub fn assert_none<T>(opt: Option<T>)

pub fn assert_ok<T, E>(r: Result<T, E>) -> T

pub fn assert_err<T, E>(r: Result<T, E>) -> E

```

## `thread`

```
pub native fn id() -> int

pub native fn name() -> string

pub native fn set_name(name: string)

pub native fn is_main() -> bool

pub native fn stack_remaining() -> int

pub struct ThreadLocal<T> {
    id: int
    init: fn() -> T
}

pub fn local<T>(init: fn() -> T) -> ThreadLocal<T>

pub fn get<T>(key: ThreadLocal<T>) -> T

pub fn set<T>(key: ThreadLocal<T>, value: T)

pub fn has<T>(key: ThreadLocal<T>) -> bool

pub fn reset<T>(key: ThreadLocal<T>)

```

## `toml`

**Experimental** (see docs/DESIGN.md "Experimental modules"). TOML 1.0 to and from a `Json` tree. Dates stay strings; `inf`/`nan` are rejected.

```
pub fn parse_toml(text: string) -> Result<Json, string>

pub fn toml_to_string(j: Json) -> Result<string, string>

```

## `url`

```
pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>

pub fn percent_encode(s: string, plus_for_space: bool) -> string

```

## `uuid`

RFC 4122 v4 UUIDs, written in pure Ziman on top of `random` -- version/variant bits set with plain integer arithmetic, since Ziman has no bitwise operators.

```
pub struct Uuid {
    bytes: [byte]
}

pub fn uuid_new() -> Uuid

pub fn uuid_new_v7() -> Uuid

pub fn uuid_nil() -> Uuid

pub fn uuid_to_string(u: Uuid) -> string

pub fn uuid_from_string(s: string) -> Result<Uuid, string>

pub fn uuid_version(u: Uuid) -> int

pub fn uuid_to_json(u: Uuid) -> Json

pub fn uuid_from_json(j: Json) -> Result<Uuid, string>

pub fn uuid_to_sql(u: Uuid) -> SqlValue

pub fn uuid_from_sql(v: SqlValue) -> Result<Uuid, DbError>

pub fn uuid_compare(a: Uuid, b: Uuid) -> int

```

## `xml`

**Experimental** (see docs/DESIGN.md "Experimental modules"). XML 1.0 as a document tree: a reader, a writer and a small path finder, in pure Ziman. Not a `Json` tree -- XML interleaves text with elements, separates attributes from children and identifies a name by its namespace URI, none of which survive a map. There is no DTD at all, so `<!DOCTYPE>` is a parse error and entity-expansion and external-entity attacks have nowhere to stand; size, depth and attribute-count limits are in `XmlLimits`.

```
pub struct Name {
    namespace: Option<string>
    local: string
    prefix: Option<string>
}

pub struct Attribute {
    name: Name
    value: string
}

pub struct Instruction {
    target: string
    data: string
}

pub enum Node {
    Element(Element)
    Text(string)
    CData(string)
    Comment(string)
    ProcessingInstruction(Instruction)
}

pub struct Element {
    name: Name
    attributes: [Attribute]
    children: [Node]
}

pub struct Declaration {
    version: string
    encoding: Option<string>
    standalone: Option<bool>
}

pub struct Document {
    declaration: Option<Declaration>
    prolog: [Node]
    root: Element
    epilog: [Node]
}

pub struct XmlLimits {
    max_bytes: int
    max_depth: int
    max_attributes: int
}

pub fn xml_limits() -> XmlLimits

pub fn name(local: string) -> Name

pub fn ns_name(uri: string, prefix: string, local: string) -> Name

pub fn element(n: Name, attributes: [Attribute], children: [Node]) -> Element

pub fn empty(local: string) -> Element

pub fn text_element(local: string, content: string) -> Element

pub fn opt_element(local: string, value: Option<string>) -> Option<Node>

pub fn req_element(local: string, value: string) -> Option<Node>

pub fn attribute(local: string, value: string) -> Attribute

pub fn document(root: Element) -> Document

pub fn same_name(a: Name, b: Name) -> bool

pub fn attr(e: Element, local: string) -> Option<string>

pub fn attr_ns(e: Element, uri: string, local: string) -> Option<string>

pub fn attr_or(e: Element, local: string, fallback: string) -> string

pub fn children(e: Element) -> [Element]

pub fn children_named(e: Element, local: string) -> [Element]

pub fn child(e: Element, local: string) -> Option<Element>

pub fn text_of(e: Element) -> string

pub fn find(e: Element, path: string) -> [Element]

pub fn find_ns(e: Element, uri: string, path: string) -> [Element]

pub fn find_first(e: Element, path: string) -> Option<Element>

pub fn parse_xml(text: string) -> Result<Document, string>

pub fn parse_xml_with(text: string, limits: XmlLimits) -> Result<Document, string>

pub fn parse_xml_bytes(data: [byte]) -> Result<Document, string>

pub fn parse_xml_bytes_with(data: [byte], limits: XmlLimits) -> Result<Document, string>

pub fn xml_to_string(doc: Document) -> Result<string, string>

pub fn xml_to_string_pretty(doc: Document, indent: string) -> Result<string, string>

pub fn xml_to_bytes(doc: Document) -> Result<[byte], string>

pub fn element_to_string(e: Element) -> Result<string, string>

```

## `yaml`

**Experimental** (see docs/DESIGN.md "Experimental modules"). A strict YAML 1.2 subset to and from a `Json` tree: block and flow collections, quoted/plain/block scalars. Anchors, tags, merge keys and multiple documents are errors.

```
pub fn parse_yaml(text: string) -> Result<Json, string>

pub fn yaml_to_string(j: Json) -> string

```

## `datasource`

```
pub enum Connection {
    Sqlite(sqlite.Connection)
    Postgres(pg.Connection)
    Mysql(mysql.Connection)
    Pool(PoolState)
}

pub fn dialect(conn: Connection) -> string

pub fn open(url: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn query_one(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Option<Row>, DbError>

pub fn begin(conn: Connection) -> Result<unit, DbError>

pub fn commit(conn: Connection) -> Result<unit, DbError>

pub fn rollback(conn: Connection) -> Result<unit, DbError>

pub fn open_pool(url: string, size: int) -> Result<Connection, DbError>

pub fn open_pool_with(url: string, size: int, check_after_seconds: int) -> Result<Connection, DbError>

pub fn acquire(conn: Connection) -> Result<Connection, DbError>

pub fn acquire_timeout(conn: Connection, timeout_ms: int) -> Result<Connection, DbError>

pub fn release(conn: Connection, taken: Connection)

pub fn discard(conn: Connection, taken: Connection)

pub fn with_transaction<R>(conn: Connection, work: fn(Connection) -> Result<R, DbError>) -> Result<R, DbError>

```

## `db`

```
pub enum SqlValue {
    Null
    Int(int)
    Float(float)
    Text(string)
    Bool(bool)
    Bytes([byte])
}

pub enum DbError {
    NotFound(string)
    Constraint(string)
    Connection(string)
    Sql(string)
}

pub struct Row {
    columns: [string]
    values: [SqlValue]
}

pub fn db_error_message(e: DbError) -> string

pub fn row_get(row: Row, column: string) -> Result<SqlValue, DbError>

pub fn sql_as_text(v: SqlValue) -> Result<string, DbError>

pub fn sql_as_int(v: SqlValue) -> Result<int, DbError>

pub fn sql_as_bool(v: SqlValue) -> Result<bool, DbError>

pub fn sql_as_float(v: SqlValue) -> Result<float, DbError>

pub fn sql_as_decimal(v: SqlValue) -> Result<decimal, DbError>

pub fn sql_lift<T>(r: Result<T, string>, what: string) -> Result<T, DbError>

pub struct UrlParam {
    name: string
    value: string
}

pub struct DbUrl {
    user: string
    password: string
    host: string
    port: int
    database: string
    params: [UrlParam]
}

pub fn percent_decode(s: string) -> Result<string, DbError>

pub fn url_param(u: DbUrl, name: string) -> Option<string>

pub fn parse_url(url: string, prefixes: [string], default_port: int) -> Result<DbUrl, DbError>

pub struct QueryResult {
    rows: [Row]
    affected: int
}

pub fn open_rows(

```

## `migrate`

Versioned schema migrations: a directory of V<n>__<name>.sql files applied in order, once each, in one transaction, with checksums to catch an edited migration.

```
pub struct Migration {
    version: int
    name: string
    file: string
    sql: string
    checksum: string
}

pub fn checksum(sql: string) -> string

pub fn migration(version: int, name: string, sql: string) -> Migration

pub struct TypeToken {
    token: string
    sqlite: string
    postgres: string
    mysql: string
}

pub val type_tokens = [

pub fn substitute(sql: string, dialect: string, file: string) -> Result<string, DbError>

pub fn split_statements(sql: string) -> [string]

pub fn load(dir: string) -> Result<[Migration], DbError>

pub fn load_for(dir: string, dialect: string) -> Result<[Migration], DbError>

pub fn apply(conn: Connection, migrations: [Migration]) -> Result<int, DbError>

pub fn run(conn: Connection, dir: string) -> Result<int, DbError>

```

## `mysql`

**Experimental** (see docs/DESIGN.md "Experimental modules"). A MySQL/MariaDB client, the third engine behind `datasource`: the text and binary protocols, `mysql_native_password`/`caching_sha2_password` authentication, TLS. No compressed protocol, LOCAL INFILE, the RSA key exchange, or prepared-statement reuse.

```
pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}

pub fn connect(url: string) -> Result<Connection, DbError>

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn close(conn: Connection)

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

```

## `pg`

**Experimental** (see docs/DESIGN.md "Experimental modules"). A PostgreSQL client, the second engine behind `datasource`: the simple and extended query protocols, trust/cleartext/SCRAM-SHA-256 authentication, TLS. No MD5 auth, Kerberos, client certificates, COPY or LISTEN/NOTIFY.

```
pub native fn float_to_text(f: float) -> string

pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}

pub fn connect(url: string) -> Result<Connection, DbError>

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn close(conn: Connection)

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

```

## `sqlite`

```
pub struct Connection {
    handle: int
}

pub fn open(path: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

```

## `client`

```
pub enum ClientError {
    BadUrl(string)
    Network(string)
    Timeout
    BadResponse(string)
    TooLarge
    TooManyRedirects
}

pub fn client_error_message(e: ClientError) -> string

pub struct Url {
    secure: bool
    host: string
    port: int
    target: string
    userinfo: string
}

pub fn parse_url(url: string) -> Result<Url, string>

pub fn response_header(r: Response, name: string) -> Option<string>

pub fn close_idle()

pub struct Cookie {
    name: string
    value: string
    domain: string
    path: string
    secure: bool
    host_only: bool
    expires: int
}

pub struct CookieJar {
    cookies: [Cookie]
}

pub fn cookie_jar() -> CookieJar

pub struct JarResponse {
    response: Response
    jar: CookieJar
}

pub fn jar_store(jar: CookieJar, u: Url, headers: [Header]) -> CookieJar

pub fn jar_header(jar: CookieJar, u: Url) -> Option<string>

pub fn request(

pub fn request_with_jar(

pub fn get(url: string) -> Result<Response, ClientError>

pub fn head(url: string) -> Result<Response, ClientError>

pub fn delete(url: string) -> Result<Response, ClientError>

pub fn post_json(url: string, body: string) -> Result<Response, ClientError>

pub fn put_json(url: string, body: string) -> Result<Response, ClientError>

pub fn form_encode(fields: Map<string, string>) -> string

pub fn post_form(url: string, fields: Map<string, string>) -> Result<Response, ClientError>

pub struct StreamResponse {
    status: int
    headers: [Header]
    body: Iter<Result<[byte], ClientError>>
}

pub fn stream(

```

## `http`

```
pub native fn is_token(s: string) -> bool

pub native fn is_field_value(s: string) -> bool

pub val status_continue = 100

pub val status_ok = 200

pub val status_created = 201

pub val status_accepted = 202

pub val status_no_content = 204

pub val status_moved_permanently = 301

pub val status_found = 302

pub val status_not_modified = 304

pub val status_bad_request = 400

pub val status_unauthorized = 401

pub val status_forbidden = 403

pub val status_not_found = 404

pub val status_method_not_allowed = 405

pub val status_not_acceptable = 406

pub val status_request_timeout = 408

pub val status_conflict = 409

pub val status_payload_too_large = 413

pub val status_unsupported_media_type = 415

pub val status_unprocessable_entity = 422

pub val status_too_many_requests = 429

pub val status_header_fields_too_large = 431

pub val status_internal_server_error = 500

pub val status_not_implemented = 501

pub val status_service_unavailable = 503

pub enum HttpMethod {
    Get
    Head
    Post
    Put
    Patch
    Delete
    Options
    Other(string)
}

pub fn method_parse(name: string) -> HttpMethod

pub fn method_name(m: HttpMethod) -> string

pub struct Header {
    name: string
    value: string
}

pub struct Request {
    method: HttpMethod
    target: string
    path: string
    query: string
    version: string
    headers: [Header]
    body: [byte]
    // The address at the other end of the connection this request arrived on, as text ("192.0.2.7" or
    // "2001:db8::1"), with no port. `None` on a Request that did not come off a socket -- one built by
    // hand in a test, or by `write_error`'s stub -- which is the honest answer rather than a placeholder.
    //
    // **The transport peer, and only that.** Not `X-Forwarded-For`: that header is written by whatever
    // is in front, and a client that is not behind a proxy can put anything in it, so a server reading
    // it by default would let every caller choose its own identity -- backwards for the thing an
    // address is wanted for, which is telling one client from another in order to rate-limit (B10). An
    // application behind a proxy it trusts reads the header itself, knowing how many hops it owns;
    // `server.forwarded_for` is there to help once that decision has been made explicitly.
    //
    // A field rather than a synthetic header, deliberately: a header could be spoofed by a client
    // sending that exact name unless the parser stripped it, and relying on that is the same subtlety
    // this field exists to avoid. Where the value comes from should be visible in the type.
    peer: Option<string>
}

pub struct Response {
    status: int
    headers: [Header]
    body: [byte]
}

pub fn response_text(r: Response) -> Result<string, string>

pub fn request_text(req: Request) -> Result<string, string>

pub fn header(req: Request, name: string) -> Option<string>

pub struct TraceParent {
    // 32 lowercase hex characters, the same for every hop of one request.
    trace_id: string
    // 16 lowercase hex characters, this hop's own. A callee makes a child rather than reusing it.
    span_id: string
    // The one flag anything acts on: whether the trace was sampled for recording.
    sampled: bool
}

pub val traceparent_header_name = "traceparent"

pub fn traceparent_parse(value: string) -> Option<TraceParent>

pub fn traceparent_new() -> TraceParent

pub fn traceparent_child(p: TraceParent) -> TraceParent

pub fn traceparent_value(p: TraceParent) -> string

pub fn traceparent_of(req: Request) -> TraceParent

pub fn traceparent_header_for(p: TraceParent) -> Header

pub fn header_value(headers: [Header], name: string) -> Option<string>

pub struct CookiePair {
    name: string
    value: string
}

pub fn request_cookies(req: Request) -> [CookiePair]

pub fn request_cookie(req: Request, name: string) -> Option<string>

pub struct SetCookie {
    name: string
    value: string
    path: string
    domain: string
    max_age_seconds: int
    secure: bool
    http_only: bool
    same_site: string
}

pub fn session_cookie(name: string, value: string) -> SetCookie

pub fn set_cookie_header(c: SetCookie) -> Header

pub fn delete_cookie_header(name: string, path: string, domain: string) -> Header

pub fn content_type(headers: [Header]) -> Option<mime.MediaType>

pub fn quality(headers: [Header], media: mime.MediaType) -> float

pub fn accepts(headers: [Header], media: mime.MediaType) -> bool

pub fn best_media(headers: [Header], candidates: [mime.MediaType]) -> Option<mime.MediaType>

pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>

pub fn percent_encode(s: string, plus_for_space: bool) -> string

```

## `server`

```
pub struct Params {
    names: [string]
    values: [string]
}

pub enum HttpError {
    BadRequest(string)
    BodyTooLarge
    HeadersTooLarge
    NotImplemented(string)
    Timeout
    Closed
    Io(string)
}

pub struct Session {
    sock: Socket
    buffer: [byte]
    timeout_ms: int
    head_timeout_ms: int
    fresh: bool
    // The largest request body `read_request` will accept, or 0 for this module's 1 MiB default.
    // `serve` fills it from `ServeOptions.max_body_bytes`; the `open_session*` constructors leave it
    // 0, so a caller reading requests off its own socket keeps the default until it says otherwise.
    //
    // It is here rather than only on `ServeOptions` because `read_request(session)` is the public
    // entry point and the North Star backend calls it directly -- a limit the server knows has to
    // reach it somehow, and widening that signature would break every such caller (D21, D22).
    max_body_bytes: int
}

pub struct Exchange {
    request: Request
    session: Session
}

pub fn open_session(sock: Socket) -> Session

pub fn open_session_with_timeout(sock: Socket, timeout_ms: int) -> Session

pub fn open_session_with_timeouts(sock: Socket, timeout_ms: int, head_timeout_ms: int) -> Session

pub fn read_request(session: Session) -> Result<Exchange, HttpError>

pub fn response(status: int, body: [byte]) -> Response

pub fn with_header(r: Response, name: string, value: string) -> Response

pub fn media_response(status: int, body: [byte], media: mime.MediaType) -> Response

pub fn json_response(status: int, body: string) -> Response

pub fn text_response(status: int, body: string) -> Response

pub fn bad_request(message: string) -> Response

pub fn not_found() -> Response

pub fn method_not_allowed(allowed: variadic string) -> Response

pub fn error_response(e: HttpError) -> Response

pub fn error_status(e: HttpError) -> int

pub fn error_text(e: HttpError) -> string

pub fn write_response(sock: Socket, req: Request, response: Response) -> Result<bool, HttpError>

pub fn compressed(req: Request, resp: Response, min_bytes: int) -> Response

pub fn write_error(sock: Socket, e: HttpError) -> Result<bool, HttpError>

pub fn write_error_with(

pub fn path_params(pattern: string, path: string) -> Option<Params>

pub fn route(req: Request, method: HttpMethod, pattern: string) -> Option<Params>

pub fn param(p: Params, name: string) -> string

pub fn not_found_or_method_not_allowed(req: Request, routes: [string]) -> Response

pub fn query_param(req: Request, name: string) -> Option<string>

pub struct Part {
    name: string
    filename: Option<string>
    content_type: Option<string>
    body: [byte]
}

pub val multipart_max_parts = 128

pub fn multipart_boundary(req: Request) -> Option<string>

pub fn multipart(req: Request) -> Result<[Part], HttpError>

pub fn multipart_with(req: Request, max_parts: int) -> Result<[Part], HttpError>

pub fn part(parts: [Part], name: string) -> Option<Part>

pub fn part_text(parts: [Part], name: string) -> Result<Option<string>, string>

pub struct Endpoint<C> {
    method: HttpMethod
    pattern: string
    handler: fn(C, Request, Params) -> Response
}

pub struct Decoder<I> {
    media: mime.MediaType
    decode: fn([byte]) -> Result<I, string>
}

pub struct Encoder<O> {
    media: mime.MediaType
    encode: fn(O) -> [byte]
}

pub fn decoder<I>(media: mime.MediaType, decode: fn([byte]) -> Result<I, string>) -> Decoder<I>

pub fn text_decoder<I>(media: mime.MediaType, decode: fn(string) -> Result<I, string>) -> Decoder<I>

pub fn encoder<O>(media: mime.MediaType, encode: fn(O) -> [byte]) -> Encoder<O>

pub fn json_decoder<I>(from_json: fn(Json) -> Result<I, string>) -> Decoder<I>

pub fn json_encoder<O>(to_json: fn(O) -> Json) -> Encoder<O>

pub fn json_list_encoder<O>(to_json: fn(O) -> Json) -> Encoder<[O]>

pub fn text_encoder<O>(media: mime.MediaType, encode: fn(O) -> string) -> Encoder<O>

pub enum Denied {
    Unauthenticated(string)
    Forbidden(string)
    Failed(string)
}

pub struct Api<E> {
    failure: fn(E) -> Response
    fault: fn(int, string) -> Response
}

pub fn secure_handler<C, U, O, E>(

pub fn secure_path_handler<C, U, P, O, E>(

pub fn secure_query_handler<C, U, Q, O, E>(

pub fn secure_body_handler<C, U, I, O, E>(

pub fn secure_path_body_handler<C, U, P, I, O, E>(

pub fn public_handler<C, O, E>(

pub fn public_path_handler<C, P, O, E>(

pub fn public_body_handler<C, I, O, E>(

pub fn public_path_body_handler<C, P, I, O, E>(

pub struct Routes<C> {
    endpoints: [Endpoint<C>]
    patterns: [Pattern]
    by_method: Map<string, [int]>
    by_length: Map<int, [int]>
    // Whether `dispatch` records a metric per request. Set by `metered_routes`, not by `routes`:
    // the matched pattern is known here and nowhere else, which is the one label worth having and
    // the reason this lives on the table rather than in `ServeOptions`.
    meter: bool
}

pub fn rate_allow(bucket: string, key: string, limit: int, window_seconds: int) -> bool

pub fn rate_allow_at(bucket: string, key: string, limit: int, window_seconds: int, now_seconds: int) -> bool

pub fn rate_reset()

pub fn open_connections_of(address: string) -> int

pub fn forwarded_for(req: Request, trusted_hops: int) -> Option<string>

pub fn declare_metrics()

pub fn metrics_response() -> Response

pub fn metrics_handler<C>(ctx: C, req: Request, params: Params) -> Response

pub fn routes<C>(endpoints: [Endpoint<C>]) -> Routes<C>

pub fn metered_routes<C>(endpoints: [Endpoint<C>]) -> Routes<C>

pub struct CorsPolicy {
    allowed_origins: [string]
    allowed_methods: string
    allowed_headers: string
    max_age_seconds: int
    allow_credentials: bool
}

pub fn cors_policy(origins: [string]) -> CorsPolicy

pub fn cors_preflight(policy: CorsPolicy, req: Request, fault: fn(int, string) -> Response) -> Option<Response>

pub fn cors_apply(policy: CorsPolicy, req: Request, answer: Response) -> Response

pub fn dispatch<C>(table: Routes<C>, ctx: C, req: Request, fault: fn(int, string) -> Response) -> Response

pub struct RequestLog {
    method: HttpMethod
    target: string
    status: int
    duration_ms: int
    response_bytes: int
    peer: Option<string>
}

pub struct ServeOptions {
    idle_timeout_ms: int
    header_timeout_ms: int
    // The most requests served at once -- not the most clients connected. A connection holds one
    // of these only while it is actually serving a request; between requests it is parked with the
    // poller and holds nothing, so a server can hold far more keep-alive clients than this number
    // (`net.wait_readable`, and `park_loop` below).
    //
    // It used to mean the other thing, and the difference is worth knowing if you are carrying a
    // size forward: a connection held a slot for its whole life, so under keep-alive an occupant
    // kept one through `idle_timeout_ms` of doing nothing and a 65th client against a cap of 64
    // waited for a *timeout* rather than for a request. 256 keep-alive clients against a cap of 64
    // took one full idle timeout whatever the request count -- 31.5s -- where they now take
    // 1.5-2.0s and a cap of 64 measures the same as a cap of 256 (docs/reviews/tasks.md F139).
    //
    // So size it by the concurrency of *work* you want to admit, which is what a thread-per-request
    // server can usefully run at once, and let the client count follow from sockets.
    max_connections: int
    log_requests: bool
    // The largest request body to accept, or 0 for the library's 1 MiB default. A body over it is
    // refused with 413 before the handler is reached, and the limit applies to what arrives on the
    // wire -- the encoded body, `multipart` boundaries included.
    max_body_bytes: int
    // How to render a rejection the server makes before the handler is ever reached -- a 400 for a malformed
    // request line, a 408 when the client stops mid-request, a 413 for too large a body, a 431 for too large a
    // header block, a 501 for a method the parser will not take. `None` uses the library's own JSON shape.
    //
    // Pass the same renderer the application gives `dispatch`, and a client sees one error shape from the whole
    // server instead of two: a browser that cannot read the body of its own 413 because it arrived in a
    // different shape with none of the application's headers is the reason this exists. Whatever it returns is
    // written as-is, so an HSTS header it adds is on the rejection too.
    //
    // It gets a status and a sentence, not the Request: these failures happen while the request is being read,
    // so there may be no parsed request to pass -- which is also why a per-request header like
    // `Access-Control-Allow-Origin` cannot be set here, there being no `Origin` to answer.
    fault: Option<fn(int, string) -> Response>
    // Called once per finished exchange, with everything the server knows about it. This is the
    // composable half of request logging: `log_requests` gives the library's one fixed line, and
    // this gives the application the fields to write its own -- through the `log` module, as JSON,
    // with a request id, or wherever its logs actually go. The two are independent; setting both
    // prints the built-in line *and* calls this.
    //
    // A field on the options, not a wrapper around `dispatch` as the roadmap first imagined: a
    // wrapper would have to return a closure and allocate one per request, which is the same reason
    // the CORS pair is two plain functions (tasks.md F59). One function value is stored here once.
    //
    // **Only completed exchanges.** A request the server rejects before parsing one -- a malformed
    // request line, a client that stops mid-head, a body over the cap -- never reaches this, because
    // there is no request to describe; the same reason `fault` above takes a status and a sentence
    // rather than a `Request`. Those rejections are `fault`'s to render.
    //
    // Typed `fn(RequestLog)` and not `fn(RequestLog) -> unit`: a void function's type *is* the
    // former, so an application writes `fn note(entry: RequestLog) { ... }` and passes it, with no
    // `-> unit` to remember. This is the library's first callback that returns nothing, so the
    // spelling is set here.
    on_request: Option<fn(RequestLog)>
    // Whether to record `http_open_connections` and the two byte counters. Per-request metrics are
    // separate, on the route table (`metered_routes`), because only something that matched the request
    // knows the pattern to label it with -- and a pattern is the only label worth having.
    metrics: bool
    // Gzip a response when the client offers it and the body is at least 1 KiB -- see `compressed`,
    // which this simply turns on. Off by default on purpose: compressing a response that carries a
    // secret alongside attacker-influenced text leaks it through the length (BREACH), and an API
    // whose login answer holds a JWT is that shape. Ask for it per server rather than inheriting it.
    compress_responses: bool
    // How many connections one address may hold at once. 0 is off, and is the default a server should keep
    // unless it is reached directly by clients: behind a reverse proxy every connection carries the
    // proxy's address, so any cap would be a cap on the proxy.
    //
    // This is the half a request throttle cannot cover. A client that opens connections and never sends a
    // request is counted by no rate limit, and can fill every `max_connections` slot;
    // `header_timeout_ms` frees such a slot within seconds, which bounds how long the flood holds one, not
    // how many it holds at once. A refusal here closes the socket without writing anything -- answering
    // would mean writing to a flooding client from the accept loop, where one slow read stalls every other
    // connection's accept.
    //
    // The address is the socket's, necessarily: at accept time there is no request yet, so there is no
    // `X-Forwarded-For` to consult even if it were trustworthy. That is the same reason the default is off.
    max_connections_per_client: int
}

pub fn serve_options() -> ServeOptions

pub fn serve(listener: Listener, handler: fn(Request) -> Response, options: ServeOptions)

```

## `schedule`

```
pub enum Schedule {
    // Start a run every N milliseconds, the first one immediately.
    EveryMillis(int)
    // Run once a day, at this many seconds past 00:00:00 UTC.
    DailyAtUtc(int)
    // Run whenever the UTC wall clock matches a cron pattern (`cron`).
    Cron(CronSpec)
}

pub struct CronSpec {
    seconds: [bool]
    minutes: [bool]
    hours: [bool]
    days_of_month: [bool]
    months: [bool]
    days_of_week: [bool]
    dom_restricted: bool
    dow_restricted: bool
}

pub fn every_millis(ms: int) -> Result<Schedule, string>

pub fn daily_at_utc(hour: int, minute: int) -> Result<Schedule, string>

pub fn cron(expr: string) -> Result<Schedule, string>

pub struct Job {
    name: string
    schedule: Schedule
    run: fn() -> Result<unit, string>
}

pub fn job(name: string, schedule: Schedule, run: fn() -> Result<unit, string>) -> Job

pub fn run_until(jobs: [Job], stop: Channel<unit>, on_error: fn(string, string))

pub fn cron_matches_at(spec: CronSpec, epoch_second: int) -> bool

```

## `time`

Seven `java.time`-shaped types (`LocalDate`, `LocalTime`, `LocalDateTime`, `Duration`, `Instant`, `ZoneOffset`, `ZoneId`), written in pure Ziman on top of `clock`. `ZoneId` reads the IANA zone files (see `tzdata`). Every type has a fixed ISO-8601 `_to_string`/`_parse` pair; `LocalDate`/`LocalTime`/`LocalDateTime`/`Instant` also get a general `_format`/`_parse_pattern` pair taking a `DateTimeFormat` -- a pattern compiled once by `datetime_format_of` (see the `iso_*_format` vals, each compiled exactly once at program start) rather than a raw pattern string re-scanned on every call.

```
pub fn is_leap_year(year: int) -> bool

pub struct DateTimeFormat {
    parts: [DateTimePart]
    width: int
}

pub fn datetime_format_of(pattern: string) -> DateTimeFormat

pub val iso_date_format = datetime_format_of("yyyy-MM-dd")

pub val iso_time_format = datetime_format_of("HH:mm:ss")

pub val iso_time_nanos_format = datetime_format_of("HH:mm:ss.SSSSSSSSS")

pub val iso_datetime_format = datetime_format_of("yyyy-MM-ddTHH:mm:ss")

pub val iso_datetime_nanos_format = datetime_format_of("yyyy-MM-ddTHH:mm:ss.SSSSSSSSS")

pub struct LocalDate {
    year: int
    month: int
    day: int
}

pub fn localdate_of(year: int, month: int, day: int) -> Result<LocalDate, string>

pub fn to_epoch_day(d: LocalDate) -> int

pub fn localdate_from_epoch_day(epoch_day: int) -> LocalDate

pub fn add_days(d: LocalDate, days: int) -> LocalDate

pub fn add_months(d: LocalDate, months: int) -> LocalDate

pub fn add_years(d: LocalDate, years: int) -> LocalDate

pub fn diff_days(a: LocalDate, b: LocalDate) -> int

pub fn localdate_compare(a: LocalDate, b: LocalDate) -> int

pub fn format(d: LocalDate, fmt: DateTimeFormat) -> string

pub fn format(d: LocalDate, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_localdate(s: string, fmt: DateTimeFormat) -> Result<LocalDate, string>

pub fn parse_localdate(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalDate, string>

pub fn parse_localdate(s: string) -> Result<LocalDate, string>

pub fn format(d: LocalDate) -> string

pub struct LocalTime {
    hour: int
    minute: int
    second: int
    nano: int
}

pub fn localtime_of(hour: int, minute: int, second: int, nano: int) -> Result<LocalTime, string>

pub fn add_nanos(t: LocalTime, nanos: int) -> LocalTime

pub fn add_seconds(t: LocalTime, seconds: int) -> LocalTime

pub fn add_minutes(t: LocalTime, minutes: int) -> LocalTime

pub fn add_hours(t: LocalTime, hours: int) -> LocalTime

pub fn diff_nanos(a: LocalTime, b: LocalTime) -> int

pub fn localtime_compare(a: LocalTime, b: LocalTime) -> int

pub fn format(t: LocalTime, fmt: DateTimeFormat) -> string

pub fn format(t: LocalTime, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_time(s: string, fmt: DateTimeFormat) -> Result<LocalTime, string>

pub fn parse_time(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalTime, string>

pub fn parse_time(s: string) -> Result<LocalTime, string>

pub fn format(t: LocalTime) -> string

pub struct Duration {
    seconds: int
    nanos: int
}

pub fn duration_of_seconds(seconds: int, nanos: int) -> Duration

pub fn duration_zero() -> Duration

pub fn add(a: Duration, b: Duration) -> Duration

pub fn negate(d: Duration) -> Duration

pub fn subtract(a: Duration, b: Duration) -> Duration

pub fn duration_compare(a: Duration, b: Duration) -> int

pub fn format(d: Duration) -> string

pub fn parse_duration(s: string) -> Result<Duration, string>

pub struct LocalDateTime {
    date: LocalDate
    time: LocalTime
}

pub fn localdatetime_of(date: LocalDate, time: LocalTime) -> LocalDateTime

pub fn add_nanos(dt: LocalDateTime, nanos: int) -> LocalDateTime

pub fn add_seconds(dt: LocalDateTime, seconds: int) -> LocalDateTime

pub fn add_minutes(dt: LocalDateTime, minutes: int) -> LocalDateTime

pub fn add_hours(dt: LocalDateTime, hours: int) -> LocalDateTime

pub fn add_days(dt: LocalDateTime, days: int) -> LocalDateTime

pub fn add_months(dt: LocalDateTime, months: int) -> LocalDateTime

pub fn add_years(dt: LocalDateTime, years: int) -> LocalDateTime

pub fn localdatetime_compare(a: LocalDateTime, b: LocalDateTime) -> int

pub fn diff(a: LocalDateTime, b: LocalDateTime) -> Duration

pub fn format(dt: LocalDateTime, fmt: DateTimeFormat) -> string

pub fn format(dt: LocalDateTime, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_datetime(s: string, fmt: DateTimeFormat) -> Result<LocalDateTime, string>

pub fn parse_datetime(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalDateTime, string>

pub fn parse_datetime(s: string) -> Result<LocalDateTime, string>

pub fn format(dt: LocalDateTime) -> string

pub struct ZoneOffset {
    total_seconds: int
}

pub fn zoneoffset_utc() -> ZoneOffset

pub fn zoneoffset_of_seconds(total_seconds: int) -> Result<ZoneOffset, string>

pub fn zoneoffset_of_hours_minutes(hours: int, minutes: int) -> Result<ZoneOffset, string>

pub fn zoneoffset_compare(a: ZoneOffset, b: ZoneOffset) -> int

pub fn format(z: ZoneOffset) -> string

pub fn parse_offset(s: string) -> Result<ZoneOffset, string>

pub struct ZoneId {
    name: string
    rules: ZoneRules
}

pub fn zoneid_utc() -> ZoneId

pub fn zoneid_of(name: string) -> Result<ZoneId, string>

pub fn zoneid_offset_at(z: ZoneId, i: Instant) -> ZoneOffset

pub fn zoneid_is_dst_at(z: ZoneId, i: Instant) -> bool

pub fn zoneid_abbreviation_at(z: ZoneId, i: Instant) -> string

pub fn format(z: ZoneId) -> string

pub fn parse_zone(s: string) -> Result<ZoneId, string>

pub struct Instant {
    seconds: int
    nanos: int
}

pub fn instant_of(seconds: int, nanos: int) -> Instant

pub fn instant_now() -> Instant

pub fn add(i: Instant, d: Duration) -> Instant

pub fn subtract(i: Instant, d: Duration) -> Instant

pub fn diff(a: Instant, b: Instant) -> Duration

pub fn instant_compare(a: Instant, b: Instant) -> int

pub fn instant_to_localdatetime(i: Instant, offset: ZoneOffset) -> LocalDateTime

pub fn localdatetime_to_instant(dt: LocalDateTime, offset: ZoneOffset) -> Instant

pub fn localdatetime_now(offset: ZoneOffset) -> LocalDateTime

pub fn localdate_now(offset: ZoneOffset) -> LocalDate

pub fn localtime_now(offset: ZoneOffset) -> LocalTime

pub fn instant_to_localdatetime_in(i: Instant, z: ZoneId) -> LocalDateTime

pub fn localdatetime_to_instant_in(dt: LocalDateTime, z: ZoneId) -> Instant

pub fn format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat) -> string

pub fn format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_instant(s: string, fmt: DateTimeFormat) -> Result<Instant, string>

pub fn parse_instant(s: string, fmt: DateTimeFormat, l: Locale) -> Result<Instant, string>

pub fn parse_instant(s: string) -> Result<Instant, string>

pub fn format(i: Instant) -> string

pub fn local_date_to_json(d: LocalDate) -> Json

pub fn local_date_from_json(j: Json) -> Result<LocalDate, string>

pub fn local_time_to_json(t: LocalTime) -> Json

pub fn local_time_from_json(j: Json) -> Result<LocalTime, string>

pub fn duration_to_json(d: Duration) -> Json

pub fn duration_from_json(j: Json) -> Result<Duration, string>

pub fn local_date_time_to_json(dt: LocalDateTime) -> Json

pub fn local_date_time_from_json(j: Json) -> Result<LocalDateTime, string>

pub fn zone_offset_to_json(z: ZoneOffset) -> Json

pub fn zone_offset_from_json(j: Json) -> Result<ZoneOffset, string>

pub fn zone_id_to_json(z: ZoneId) -> Json

pub fn zone_id_from_json(j: Json) -> Result<ZoneId, string>

pub fn instant_to_json(i: Instant) -> Json

pub fn instant_from_json(j: Json) -> Result<Instant, string>

pub fn local_date_to_sql(v: LocalDate) -> SqlValue

pub fn local_date_from_sql(v: SqlValue) -> Result<LocalDate, DbError>

pub fn local_time_to_sql(v: LocalTime) -> SqlValue

pub fn local_time_from_sql(v: SqlValue) -> Result<LocalTime, DbError>

pub fn duration_to_sql(v: Duration) -> SqlValue

pub fn duration_from_sql(v: SqlValue) -> Result<Duration, DbError>

pub fn local_date_time_to_sql(v: LocalDateTime) -> SqlValue

pub fn local_date_time_from_sql(v: SqlValue) -> Result<LocalDateTime, DbError>

pub fn zone_offset_to_sql(v: ZoneOffset) -> SqlValue

pub fn zone_offset_from_sql(v: SqlValue) -> Result<ZoneOffset, DbError>

pub fn zone_id_to_sql(v: ZoneId) -> SqlValue

pub fn zone_id_from_sql(v: SqlValue) -> Result<ZoneId, DbError>

pub fn instant_to_sql(v: Instant) -> SqlValue

pub fn instant_from_sql(v: SqlValue) -> Result<Instant, DbError>

```

## `zone`

The IANA time zone database, read from the operating system's zone files, for `time`'s named zones (`zoneid_of`).

```
pub struct ZoneType {
    utoff: int
    dst: bool
    name: string
}

pub struct RuleDate {
    kind: int
    month: int
    week: int
    weekday: int
    day: int
    seconds: int
}

pub struct PosixRule {
    standard: ZoneType
    daylight: ZoneType
    has_dst: bool
    start: RuleDate
    end: RuleDate
}

pub struct ZoneRules {
    transitions: [int]
    type_of: [int]
    utoffs: [int]
    dsts: [bool]
    names: [string]
    has_rule: bool
    rule: PosixRule
}

pub fn parse_posix_rule(text: string) -> Result<PosixRule, string>

pub val no_rule = PosixRule {

pub fn parse_rules(data: [byte]) -> Result<ZoneRules, string>

pub fn load_rules(name: string) -> Result<ZoneRules, string>

pub fn type_at(rules: ZoneRules, t: int) -> ZoneType

```
