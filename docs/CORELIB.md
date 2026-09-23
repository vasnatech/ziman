# Ziman core module reference

Auto-generated from each `corelib/*.zm` file's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub enum`/
`pub fn`/`pub native fn`/`pub val`). This is a signature lookup, not an
explanation -- see docs/SYNTAX.md for full semantics and docs/DESIGN.md
for why each module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/SYNTAX.md's "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.

One exception, not listed below: `corelib/prelude.zm` -- unlike every
module here, it's never `use`-able at all (see its own header comment
and docs/DESIGN.md's "Prelude" entry), since everything it declares
(`Option<T>`, plus a handful of native functions) is already bare and
unqualified in every program, the same way `print`/`str`/`parse_int`
are. See docs/SYNTAX.md's "Modules" section for what it contains.

## `clock`

One native function reading the system clock (`clock_gettime`) -- Ziman's other source of genuine non-determinism.

```
pub native fn now_raw() -> [int]

```

## `collections`

`HashMap<K, V>`, `HashSet<T>` -- generic containers, written in pure Ziman on top of the language's own `Eq`/`Hash` bounds.

```
pub struct HashMap<K: Eq + Hash, V> {
    buckets: [[Entry<K, V>]]
    count: int
}

pub fn hashmap_new<K: Eq + Hash, V>() -> HashMap<K, V>

pub fn hashmap_size<K: Eq + Hash, V>(m: HashMap<K, V>) -> int

pub fn hashmap_get<K: Eq + Hash, V>(m: HashMap<K, V>, key: K) -> Option<V>

pub fn hashmap_has<K: Eq + Hash, V>(m: HashMap<K, V>, key: K) -> bool

pub fn hashmap_keys<K: Eq + Hash, V>(m: HashMap<K, V>) -> [K]

pub fn hashmap_values<K: Eq + Hash, V>(m: HashMap<K, V>) -> [V]

pub fn hashmap_set<K: Eq + Hash, V>(m: HashMap<K, V>, key: K, value: V) -> HashMap<K, V>

pub fn hashmap_remove<K: Eq + Hash, V>(m: HashMap<K, V>, key: K) -> HashMap<K, V>

pub struct HashSet<T: Eq + Hash> {
    buckets: [[T]]
    count: int
}

pub fn hashset_new<T: Eq + Hash>() -> HashSet<T>

pub fn hashset_size<T: Eq + Hash>(s: HashSet<T>) -> int

pub fn hashset_contains<T: Eq + Hash>(s: HashSet<T>, item: T) -> bool

pub fn hashset_items<T: Eq + Hash>(s: HashSet<T>) -> [T]

pub fn hashset_add<T: Eq + Hash>(s: HashSet<T>, item: T) -> HashSet<T>

pub fn hashset_remove<T: Eq + Hash>(s: HashSet<T>, item: T) -> HashSet<T>

```

## `db`

```
pub struct Connection {
    handle: int
}

pub fn open(url: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: [SqlValue]) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: [SqlValue]) -> Result<[Row], DbError>

pub fn query_one(conn: Connection, sql: string, params: [SqlValue]) -> Result<Option<Row>, DbError>

pub fn begin(conn: Connection) -> Result<int, DbError>

pub fn commit(conn: Connection) -> Result<int, DbError>

pub fn rollback(conn: Connection) -> Result<int, DbError>

```

## `io`

File I/O. The first native module -- proved the `native fn` mechanism end to end.

```
pub native fn read_file(path: string) -> Result<string, string>

pub native fn write_file(path: string, contents: string)

```

## `json`

A JSON parser/serializer (`Json`, a genuinely recursive enum), written in pure Ziman -- no native code needed. `\uXXXX` unicode escapes and `==`/`!=` on `Json` are the two known gaps.

```
pub struct JField {
    key: string
    value: Json
}

pub enum Json {
    JNull
    JBool(bool)
    JNumber(float)
    JString(string)
    JArray([Json])
    JObject([JField])
}

pub fn parse_json(s: string) -> Result<Json, string>

pub fn json_to_string(j: Json) -> string

pub fn json_object_get(j: Json, key: string) -> Option<Json>

pub fn json_require_field(j: Json, key: string) -> Result<Json, string>

pub fn json_require_string_field(j: Json, key: string) -> Result<string, string>

pub fn json_to_json(j: Json) -> Json

pub fn json_from_json(j: Json) -> Result<Json, string>

```

## `number`

Shared formatting/parsing across `int`/`float`/`bigint`/`decimal`/`byte`, written in pure Ziman on top of each type's own `str()`. `NumberFormat`/`parse_pattern` cover thousands separators and forced decimal places (a small subset of DecimalFormat-style patterns); `byte_to_hex`/`int_to_hex`/`bigint_to_hex` (and their `_from_hex` inverses) cover hex for the three integral types.

```
pub fn byte_to_hex(b: byte) -> string

pub fn byte_from_hex(s: string) -> Result<byte, string>

pub fn bigint_to_hex(n: bigint) -> string

pub fn bigint_from_hex(s: string) -> Result<bigint, string>

pub fn int_to_hex(x: int) -> string

pub fn int_from_hex(s: string) -> Result<int, string>

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

```

## `random`

One native function reading real OS entropy (`/dev/urandom`) -- Ziman's first source of genuine non-determinism.

```
pub native fn random_bytes(n: int) -> [byte]

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

## `uuid`

RFC 4122 v4 UUIDs, written in pure Ziman on top of `random` -- version/variant bits set with plain integer arithmetic, since Ziman has no bitwise operators.

```
pub struct Uuid {
    bytes: [byte]
}

pub fn uuid_new() -> Uuid

pub fn uuid_to_string(u: Uuid) -> string

pub fn uuid_from_string(s: string) -> Result<Uuid, string>

pub fn uuid_to_json(u: Uuid) -> Json

pub fn uuid_from_json(j: Json) -> Result<Uuid, string>

pub fn uuid_to_sql(u: Uuid) -> SqlValue

pub fn uuid_from_sql(v: SqlValue) -> Result<Uuid, DbError>

```
