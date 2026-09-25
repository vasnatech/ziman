# Ziman core module reference

Auto-generated from each `lib/core/*.zm` or `lib/db/*.zm` file's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub enum`/
`pub fn`/`pub native fn`/`pub val`). This is a signature lookup, not an
explanation -- see docs/SYNTAX.md for full semantics and docs/DESIGN.md
for why each module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/SYNTAX.md's "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.

One exception, not listed below: `lib/prelude.zm` -- unlike every
module here, it's never `use`-able at all (see its own header comment
and docs/DESIGN.md's "Prelude" entry), since everything it declares
(`Option<T>`, plus a handful of native functions) is already bare and
unqualified in every program, the same way `print`/`str`/`parse_int`
are. See docs/SYNTAX.md's "Modules" section for what it contains.

## `clock`

One native function reading the system clock (`clock_gettime`) -- Ziman's other source of genuine non-determinism.

```
pub native fn now_raw() -> [int]

pub native fn now_iso() -> string

pub native fn now_http_date() -> string

pub native fn sleep_millis(ms: int)

```

## `collections`

Sorting, searching and the higher-order array helpers, written in pure Ziman. (`Map` and `Set` are built-in types, not part of this module.)

```
pub fn sort_indices<K: Ord>(keys: [K]) -> [int]

pub fn sort_indices_desc<K: Ord>(keys: [K]) -> [int]

pub fn sort<T: Ord>(items: [T]) -> [T]

pub fn sort_desc<T: Ord>(items: [T]) -> [T]

pub fn sort_by_keys<T, K: Ord>(items: [T], keys: [K]) -> Result<[T], string>

pub fn sort_by_keys_desc<T, K: Ord>(items: [T], keys: [K]) -> Result<[T], string>

pub fn sort_with<T>(items: [T], cmp: fn(T, T) -> int) -> [T]

pub fn is_sorted<T: Ord>(items: [T]) -> bool

pub fn lower_bound<T: Ord>(sorted: [T], target: T) -> int

pub fn upper_bound<T: Ord>(sorted: [T], target: T) -> int

pub fn binary_search<T: Ord>(sorted: [T], target: T) -> Option<int>

pub fn find<T>(items: [T], test: fn(T) -> bool) -> Option<T>

pub fn first<T>(items: [T]) -> Option<T>

pub fn last<T>(items: [T]) -> Option<T>

pub fn min_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> Option<T>

pub fn max_by<T, K: Ord>(items: [T], key: fn(T) -> K) -> Option<T>

pub fn reverse<T>(items: [T]) -> [T]

pub fn map<T, U>(items: [T], f: fn(T) -> U) -> [U]

pub fn filter<T>(items: [T], keep: fn(T) -> bool) -> [T]

pub fn flat_map<T, U>(items: [T], f: fn(T) -> [U]) -> [U]

pub fn fold<T, A>(items: [T], initial: A, f: fn(A, T) -> A) -> A

pub fn count<T>(items: [T], test: fn(T) -> bool) -> int

pub fn any<T>(items: [T], test: fn(T) -> bool) -> bool

pub fn all<T>(items: [T], test: fn(T) -> bool) -> bool

pub fn for_each<T>(items: [T], f: fn(T))

pub fn distinct<T: Eq + Hash>(items: [T]) -> [T]

pub fn group_by<T, K: Eq + Hash>(items: [T], key: fn(T) -> K) -> Map<K, [T]>

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

Hashes (SHA-1/256/512), HMAC, PBKDF2, a constant-time byte comparison, base64 and base64url, on OpenSSL. No ciphers or public-key operations.

```
pub native fn sha1(data: [byte]) -> [byte]

pub native fn sha256(data: [byte]) -> [byte]

pub native fn sha512(data: [byte]) -> [byte]

pub native fn hmac_sha256(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha512(key: [byte], data: [byte]) -> [byte]

pub native fn pbkdf2_hmac_sha256(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]

pub native fn pbkdf2_hmac_sha512(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]

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

pub fn parse_csv_columns(text: string, columns: [string]) -> Result<Json, string>

pub fn parse_csv_columns_typed(text: string, columns: [string]) -> Result<Json, string>

pub fn csv_rows_to_string(rows: [[string]]) -> string

pub fn csv_rows_to_string_with(rows: [[string]], delimiter: string) -> Result<string, string>

pub fn csv_to_string(rows: Json) -> Result<string, string>

pub fn csv_to_string_with(rows: Json, delimiter: string) -> Result<string, string>

```

## `fs`

```
pub native fn read_text(path: string) -> Result<string, string>

pub native fn write_text(path: string, text: string) -> Result<int, string>

pub native fn append_text(path: string, text: string) -> Result<int, string>

pub native fn write_text_atomic(path: string, text: string) -> Result<int, string>

pub native fn read_bytes(path: string) -> Result<[byte], string>

pub native fn write_bytes(path: string, data: [byte]) -> Result<int, string>

pub native fn append_bytes(path: string, data: [byte]) -> Result<int, string>

pub native fn exists(path: string) -> bool

pub native fn is_dir(path: string) -> bool

pub native fn is_file(path: string) -> bool

pub native fn size(path: string) -> Result<int, string>

pub native fn modified_seconds(path: string) -> Result<int, string>

pub native fn list_dir(path: string) -> Result<[string], string>

pub native fn remove(path: string) -> Result<int, string>

pub native fn make_dir(path: string) -> Result<int, string>

pub native fn make_dirs(path: string) -> Result<int, string>

pub native fn rename(from: string, to: string) -> Result<int, string>

pub native fn copy_file(from: string, to: string) -> Result<int, string>

pub fn path_join(pieces: string...) -> string

pub fn file_name(path: string) -> string

pub fn parent_dir(path: string) -> string

pub fn extension(path: string) -> string

```

## `httpclient`

```
pub struct BytesResponse {
    status: int
    headers: [Header]
    body: [byte]
}

pub enum ClientError {
    BadUrl(string)
    Network(string)
    Timeout
    BadResponse(string)
    TooLarge
    TooManyRedirects
}

pub fn error_message(e: ClientError) -> string

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

pub fn request(method: HttpMethod, url: string, headers: Option<[Header]>, body: Option<string>,

pub fn request_bytes(method: HttpMethod, url: string, headers: Option<[Header]>, body: Option<[byte]>,

pub fn request_with_jar(jar: CookieJar, method: HttpMethod, url: string, headers: Option<[Header]>,

pub fn get(url: string) -> Result<Response, ClientError>

pub fn head(url: string) -> Result<Response, ClientError>

pub fn delete(url: string) -> Result<Response, ClientError>

pub fn post_json(url: string, body: string) -> Result<Response, ClientError>

pub fn put_json(url: string, body: string) -> Result<Response, ClientError>

pub fn form_encode(fields: Map<string, string>) -> string

pub fn post_form(url: string, fields: Map<string, string>) -> Result<Response, ClientError>

```

## `http`

```
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

pub val status_request_timeout = 408

pub val status_conflict = 409

pub val status_payload_too_large = 413

pub val status_unprocessable_entity = 422

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
    body: string
}

pub struct Response {
    status: int
    headers: [Header]
    body: string
}

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
}

pub struct Exchange {
    request: Request
    session: Session
}

pub fn open_session(sock: Socket) -> Session

pub fn header(req: Request, name: string) -> Option<string>

pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>

pub fn percent_encode(s: string, plus_for_space: bool) -> string

pub fn read_request(session: Session) -> Result<Exchange, HttpError>

pub fn response(status: int, body: string) -> Response

pub fn with_header(r: Response, name: string, value: string) -> Response

pub fn json_response(status: int, body: string) -> Response

pub fn text_response(status: int, body: string) -> Response

pub fn bad_request(message: string) -> Response

pub fn not_found() -> Response

pub fn method_not_allowed(allowed: [string]) -> Response

pub fn error_response(e: HttpError) -> Response

pub fn write_response(sock: Socket, req: Request, resp: Response) -> Result<bool, HttpError>

pub fn write_error(sock: Socket, e: HttpError) -> Result<bool, HttpError>

pub fn path_params(pattern: string, path: string) -> Option<Params>

pub fn route(req: Request, method: HttpMethod, pattern: string) -> Option<Params>

pub fn param(p: Params, name: string) -> string

pub fn not_found_or_method_not_allowed(req: Request, routes: [string]) -> Response

pub fn query_param(req: Request, name: string) -> Option<string>

pub struct ServeOptions {
    idle_timeout_ms: int
    max_connections: int
    log_requests: bool
}

pub fn serve(listener: Listener, handler: fn(Request) -> Response, options: ServeOptions)

```

## `json`

A JSON parser/serializer (`Json`, a genuinely recursive enum), written in pure Ziman -- no native code needed. `\uXXXX` unicode escapes and `==`/`!=` on `Json` are the two known gaps.

```
pub enum Json {
    JNull
    JBool(bool)
    JInt(int)
    JNumber(float)
    JString(string)
    JArray([Json])
    JObject(Map<string, Json>)
}

pub fn parse_json(s: string) -> Result<Json, string>

pub fn parse_json_strict(s: string) -> Result<Json, string>

pub fn json_to_string(j: Json) -> string

pub fn json_object_get(j: Json, key: string) -> Option<Json>

pub fn json_get_path(j: Json, path: [string]) -> Option<Json>

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

pub fn int_from_json(j: Json) -> Result<int, string>

pub fn float_to_json(f: float) -> Json

pub fn float_from_json(j: Json) -> Result<float, string>

pub fn bool_to_json(b: bool) -> Json

pub fn bool_from_json(j: Json) -> Result<bool, string>

pub fn string_to_json(s: string) -> Json

pub fn string_from_json(j: Json) -> Result<string, string>

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
}

pub fn logger(level: Level, file: Option<string>, timestamps: Option<bool>, prefix: Option<string>) -> Logger

pub fn with_prefix(l: Logger, prefix: string) -> Logger

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

## `ndjson`

Newline-delimited JSON (one value per line) on top of `json`.

```
pub fn parse_ndjson(text: string) -> Result<[Json], string>

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

pub fn listen(host: string, port: int) -> Result<Listener, NetError>

pub fn listen_tls(host: string, port: int, cert_file: string, key_file: string) -> Result<Listener, NetError>

pub fn listener_port(l: Listener) -> int

pub fn accept(l: Listener) -> Result<Socket, NetError>

pub fn read(s: Socket, max: int) -> Result<[byte], NetError>

pub fn read_exact(s: Socket, count: int) -> Result<[byte], NetError>

pub fn write(s: Socket, data: [byte]) -> Result<int, NetError>

pub fn read_text(s: Socket, max: int) -> Result<string, NetError>

pub fn write_text(s: Socket, text: string) -> Result<int, NetError>

pub fn set_timeout(s: Socket, millis: int) -> Result<int, NetError>

pub fn start_tls(s: Socket, host: string) -> Result<int, NetError>

pub fn start_tls_with(s: Socket, host: string, verify: TlsVerify, ca_file: Option<string>) -> Result<int, NetError>

pub fn connect_tls(host: string, port: int) -> Result<Socket, NetError>

pub fn connect_tls_with(host: string, port: int, verify: TlsVerify,

pub fn idle_put(key: string, s: Socket) -> bool

pub fn idle_take(key: string) -> Option<Socket>

pub fn idle_clear()

pub fn close(s: Socket)

pub fn close_listener(l: Listener)

```

## `number`

Shared formatting/parsing across `int`/`float`/`bigint`/`decimal`/`byte`, written in pure Ziman on top of each type's own `str()`. `NumberFormat`/`parse_pattern` cover thousands separators and forced decimal places (a small subset of DecimalFormat-style patterns); `byte_to_hex`/`int_to_hex`/`bigint_to_hex` (and their `_from_hex` inverses) cover hex for the three integral types.

```
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

pub native fn write_stderr(text: string)

pub native fn install_shutdown_handler()

pub native fn shutdown_requested() -> bool

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

pub fn check_unknown(props: Properties, known: [string]) -> Result<bool, string>

pub fn report(props: Properties, keys: [string]) -> string

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

## `time`

Seven `java.time`-shaped types (`LocalDate`, `LocalTime`, `LocalDateTime`, `Duration`, `Instant`, `ZoneOffset`, `ZoneId`), written in pure Ziman on top of `clock`. `ZoneOffset` only -- no IANA-style `ZoneId` with DST rules. Every type has a fixed ISO-8601 `_to_string`/`_parse` pair; `LocalDate`/`LocalTime`/`LocalDateTime`/`Instant` also get a general `_format`/`_parse_pattern` pair taking a `DateTimeFormat` -- a pattern compiled once by `datetime_format_of` (see the `iso_*_format` vals, each compiled exactly once at program start) rather than a raw pattern string re-scanned on every call.

```
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

pub fn localdate_is_leap_year(year: int) -> bool

pub fn localdate_to_epoch_day(d: LocalDate) -> int

pub fn localdate_from_epoch_day(epoch_day: int) -> LocalDate

pub fn localdate_add_days(d: LocalDate, days: int) -> LocalDate

pub fn localdate_add_months(d: LocalDate, months: int) -> LocalDate

pub fn localdate_add_years(d: LocalDate, years: int) -> LocalDate

pub fn localdate_diff_days(a: LocalDate, b: LocalDate) -> int

pub fn localdate_compare(a: LocalDate, b: LocalDate) -> int

pub fn localdate_format(d: LocalDate, fmt: DateTimeFormat) -> string

pub fn localdate_parse_pattern(s: string, fmt: DateTimeFormat) -> Result<LocalDate, string>

pub fn localdate_from_string(s: string) -> Result<LocalDate, string>

pub fn localdate_to_string(d: LocalDate) -> string

pub struct LocalTime {
    hour: int
    minute: int
    second: int
    nano: int
}

pub fn localtime_of(hour: int, minute: int, second: int, nano: int) -> Result<LocalTime, string>

pub fn localtime_add_nanos(t: LocalTime, nanos: int) -> LocalTime

pub fn localtime_add_seconds(t: LocalTime, seconds: int) -> LocalTime

pub fn localtime_add_minutes(t: LocalTime, minutes: int) -> LocalTime

pub fn localtime_add_hours(t: LocalTime, hours: int) -> LocalTime

pub fn localtime_diff_nanos(a: LocalTime, b: LocalTime) -> int

pub fn localtime_compare(a: LocalTime, b: LocalTime) -> int

pub fn localtime_format(t: LocalTime, fmt: DateTimeFormat) -> string

pub fn localtime_parse_pattern(s: string, fmt: DateTimeFormat) -> Result<LocalTime, string>

pub fn localtime_from_string(s: string) -> Result<LocalTime, string>

pub fn localtime_to_string(t: LocalTime) -> string

pub struct Duration {
    seconds: int
    nanos: int
}

pub fn duration_of_seconds(seconds: int, nanos: int) -> Duration

pub fn duration_zero() -> Duration

pub fn duration_add(a: Duration, b: Duration) -> Duration

pub fn duration_negate(d: Duration) -> Duration

pub fn duration_subtract(a: Duration, b: Duration) -> Duration

pub fn duration_compare(a: Duration, b: Duration) -> int

pub fn duration_to_string(d: Duration) -> string

pub fn duration_from_string(s: string) -> Result<Duration, string>

pub struct LocalDateTime {
    date: LocalDate
    time: LocalTime
}

pub fn localdatetime_of(date: LocalDate, time: LocalTime) -> LocalDateTime

pub fn localdatetime_add_nanos(dt: LocalDateTime, nanos: int) -> LocalDateTime

pub fn localdatetime_add_seconds(dt: LocalDateTime, seconds: int) -> LocalDateTime

pub fn localdatetime_add_minutes(dt: LocalDateTime, minutes: int) -> LocalDateTime

pub fn localdatetime_add_hours(dt: LocalDateTime, hours: int) -> LocalDateTime

pub fn localdatetime_add_days(dt: LocalDateTime, days: int) -> LocalDateTime

pub fn localdatetime_add_months(dt: LocalDateTime, months: int) -> LocalDateTime

pub fn localdatetime_add_years(dt: LocalDateTime, years: int) -> LocalDateTime

pub fn localdatetime_compare(a: LocalDateTime, b: LocalDateTime) -> int

pub fn localdatetime_diff(a: LocalDateTime, b: LocalDateTime) -> Duration

pub fn localdatetime_format(dt: LocalDateTime, fmt: DateTimeFormat) -> string

pub fn localdatetime_parse_pattern(s: string, fmt: DateTimeFormat) -> Result<LocalDateTime, string>

pub fn localdatetime_from_string(s: string) -> Result<LocalDateTime, string>

pub fn localdatetime_to_string(dt: LocalDateTime) -> string

pub struct ZoneOffset {
    total_seconds: int
}

pub fn zoneoffset_utc() -> ZoneOffset

pub fn zoneoffset_of_seconds(total_seconds: int) -> Result<ZoneOffset, string>

pub fn zoneoffset_of_hours_minutes(hours: int, minutes: int) -> Result<ZoneOffset, string>

pub fn zoneoffset_compare(a: ZoneOffset, b: ZoneOffset) -> int

pub fn zoneoffset_to_string(z: ZoneOffset) -> string

pub fn zoneoffset_from_string(s: string) -> Result<ZoneOffset, string>

pub struct ZoneId {
    name: string
}

pub fn zoneid_utc() -> ZoneId

pub fn zoneid_to_offset(z: ZoneId) -> ZoneOffset

pub fn zoneid_to_string(z: ZoneId) -> string

pub fn zoneid_from_string(s: string) -> Result<ZoneId, string>

pub struct Instant {
    seconds: int
    nanos: int
}

pub fn instant_of(seconds: int, nanos: int) -> Instant

pub fn instant_now() -> Instant

pub fn instant_add(i: Instant, d: Duration) -> Instant

pub fn instant_subtract(i: Instant, d: Duration) -> Instant

pub fn instant_diff(a: Instant, b: Instant) -> Duration

pub fn instant_compare(a: Instant, b: Instant) -> int

pub fn instant_to_localdatetime(i: Instant, offset: ZoneOffset) -> LocalDateTime

pub fn localdatetime_to_instant(dt: LocalDateTime, offset: ZoneOffset) -> Instant

pub fn instant_format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat) -> string

pub fn instant_parse_pattern(s: string, fmt: DateTimeFormat) -> Result<Instant, string>

pub fn instant_from_string(s: string) -> Result<Instant, string>

pub fn instant_to_string(i: Instant) -> string

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

## `toml`

TOML 1.0 to and from a `Json` tree. Dates stay strings; `inf`/`nan` are rejected.

```
pub fn parse_toml(text: string) -> Result<Json, string>

pub fn toml_to_string(j: Json) -> Result<string, string>

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

## `yaml`

A strict YAML 1.2 subset to and from a `Json` tree: block and flow collections, quoted/plain/block scalars. Anchors, tags, merge keys and multiple documents are errors.

```
pub fn parse_yaml(text: string) -> Result<Json, string>

pub fn yaml_to_string(j: Json) -> string

```

## `dburl`

```
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

pub fn param(u: DbUrl, name: string) -> Option<string>

pub fn parse(url: string, prefixes: [string], default_port: int) -> Result<DbUrl, DbError>

```

## `db`

```
pub struct PoolState {
    idle: Channel<Connection>
    size: int
}

pub enum Connection {
    Sqlite(sqlite.Connection)
    Postgres(pg.Connection)
    Mysql(mysql.Connection)
    Pool(PoolState)
}

pub fn open(url: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: [SqlValue]) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: [SqlValue]) -> Result<[Row], DbError>

pub fn query_one(conn: Connection, sql: string, params: [SqlValue]) -> Result<Option<Row>, DbError>

pub fn begin(conn: Connection) -> Result<int, DbError>

pub fn commit(conn: Connection) -> Result<int, DbError>

pub fn rollback(conn: Connection) -> Result<int, DbError>

pub fn open_pool(url: string, size: int) -> Result<Connection, DbError>

pub fn acquire(conn: Connection) -> Result<Connection, DbError>

pub fn release(conn: Connection, taken: Connection)

pub fn with_transaction<R>(conn: Connection, work: fn(Connection) -> Result<R, DbError>) -> Result<R, DbError>

```

## `mysql`

```
pub struct QueryResult {
    rows: [Row]
    affected: int
}

pub struct RunRequest {
    sql: string
    params: [SqlValue]
    reply_to: Channel<Result<QueryResult, DbError>>
}

pub enum Request {
    Run(RunRequest)
    Close
}

pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}

pub fn connect(url: string) -> Result<Connection, DbError>

pub fn execute(conn: Connection, sql: string, params: [SqlValue]) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: [SqlValue]) -> Result<[Row], DbError>

pub fn close(conn: Connection)

```

## `pg`

```
pub struct QueryResult {
    rows: [Row]
    affected: int
}

pub struct RunRequest {
    sql: string
    params: [SqlValue]
    reply_to: Channel<Result<QueryResult, DbError>>
}

pub enum Request {
    Run(RunRequest)
    Close
}

pub native fn float_to_text(f: float) -> string

pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}

pub fn connect(url: string) -> Result<Connection, DbError>

pub fn execute(conn: Connection, sql: string, params: [SqlValue]) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: [SqlValue]) -> Result<[Row], DbError>

pub fn close(conn: Connection)

```

## `sqlite`

```
pub struct Connection {
    handle: int
}

pub fn open(path: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: [SqlValue]) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: [SqlValue]) -> Result<[Row], DbError>

```

## `sql`

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

pub fn sql_lift<T>(r: Result<T, string>, what: string) -> Result<T, DbError>

```
