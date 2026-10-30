# Dates, JSON, serialization and ordering

Part of the [v0 syntax reference](README.md).

## Date and time

The `time` core module (`use time.Name`) provides seven types, modeled on `java.time` but
scoped down. Every type is `{ ...: int }`-shaped (`Instant`/`Duration` are `bigint`-free
`{ seconds: int, nanos: int }`), never a packed number, and has a fixed ISO-8601-shaped
`format(value) -> string` / `parse*(s) -> Result<Value, string>` pair, each the other's exact
inverse. `LocalDate`, `LocalTime`, `LocalDateTime` and `Instant` also take an explicit pattern:
`format(value, fmt: DateTimeFormat) -> string` / `parse*(s, fmt) -> Result<Value, string>`; the
ISO pair is implemented in terms of these, via one of five `pub val iso_*_format` presets.
`Duration`/`ZoneOffset`/`ZoneId` only get the ISO pair -- their shapes (`"PT...S"`, `"+HH:MM"`, a
bare zone name) aren't a calendar breakdown.

**The names carry no type prefix.** `format` is one overload set across all seven types (plus the
`DateTimeFormat` and `Locale` forms), and so are `add`/`subtract`/`negate`, `add_days`/`_months`/
`_years`/`_hours`/`_minutes`/`_seconds`/`_nanos` and `diff`, resolved on the argument's type. Where
two types would need the same signature the name says which, one per type: `parse_localdate`,
`parse_time`, `parse_datetime`, `parse_duration`, `parse_offset`, `parse_zone` and
`parse_instant`. Constructors keep their prefix, since a type is all that distinguishes them
(`localdate_of`, `localtime_of`, `localdatetime_of`, `duration_of_seconds`, `instant_of`,
`zoneoffset_of_seconds`, `zoneid_of`), and so does `<type>_compare`, which `Ord` looks up by name.

**`DateTimeFormat`** is compiled once (`datetime_format_of(pattern: string) ->
DateTimeFormat` scans it into an array of field/literal parts) and reused across many
`format`/`parse*` calls. The pattern language is small: `yyyy` (4-digit year),
`MM`/`dd`/`HH`/`mm`/`ss` (2-digit, `HH` is 0-23), `SSSSSSSSS` (9-digit nanosecond), `MMM`/`MMMM`
(abbreviated and full month name) and `EEE`/`EEEE` (abbreviated and full day name). The `M` and
`E` run lengths are matched longest-first, so `MMMM` is one token rather than `MM` twice.
**The names come from the operating system's locale data**, so one pattern renders in any
language -- `EEEE, dd MMMM yyyy` is `Friday, 23 October 2026` under `en_US` and
`Cuma, 23 Ekim 2026` under `tr_TR`. A compiled `DateTimeFormat` carries no locale; the locale
goes to the call that renders or reads (`format(d, fmt, l)`), and the form without it
uses `locale.default()`. Parsing folds case under the same locale, and a parsed day name is
checked against the date rather than stepped over. Anything else is a literal, copied through
verbatim -- no escaping; an unrecognized run length of a token letter (three `S`s) is just
folded into the surrounding literal rather than erroring. **No name token has a fixed rendered
width** (`ja_JP` abbreviates January as `1月`, `ar_SA` as `يناير`), and `DateTimeFormat.width`
is -1 for any pattern holding one.

```ziman
use time.LocalDate
use time.add_months
use time.format
use time.localdate_of

fn main() {
    val d = unwrap(localdate_of(2024, 1, 31))
    print(format(add_months(d, 1))) // 2024-02-29
}
```

- **`LocalDate { year, month, day: int }`** — a calendar date, no time, no zone.
  `localdate_of(y, m, d) -> Result<LocalDate, string>` validates `month` (1-12) and `day`
  against that month's real length (leap years correct). `add_days`/`_months`/
  `add_years` (month/year forms clamp the day to the target month, like `java.time`'s
  `plusMonths` -- `2024-01-31` plus a month is `2024-02-29`), `diff_days`,
  `localdate_compare`, `to_epoch_day`/`localdate_from_epoch_day` (days since 1970-01-01,
  negative before it), `is_leap_year(year: int)`, `format` (`"YYYY-MM-DD"`)/`parse_localdate`,
  and `format`/`parse_localdate` with a `DateTimeFormat`.
- **`LocalTime { hour, minute, second, nano: int }`** — a time of day, no date, no zone.
  `localtime_of(...)` validates ranges (`nano` is 0-999999999). `add_hours`/
  `add_minutes`/`add_seconds`/`add_nanos` wrap at 24h and discard day overflow (use
  `LocalDateTime` when that needs tracking), `diff_nanos`, `localtime_compare`,
  `format` (`"HH:MM:SS"`, or with `.nnnnnnnnn` when `nano != 0`)/`parse_time`
  (auto-detects which shape), `format`/`parse_time` with a `DateTimeFormat`.
- **`LocalDateTime { date: LocalDate, time: LocalTime }`** — the two combined, still no
  zone. `localdatetime_of(date, time)`, `add_hours`/`add_minutes`/`add_seconds`/`add_nanos`
  (these DO carry into the date on overflow, unlike `LocalTime`'s own), `add_days`/
  `add_months`/`add_years` (delegate to `LocalDate`, time untouched), `localdatetime_compare`,
  `diff(a, b) -> Duration` (elapsed wall-clock time on one fixed calendar, not a real
  cross-zone computation), `format` (`"YYYY-MM-DDTHH:MM:SS"`)/`parse_datetime`, and
  `format`/`parse_datetime` with a `DateTimeFormat`.
- **`Duration { seconds, nanos: int }`** — a length of time, not a point. `nanos` stays in
  `[0, 999999999]`; `seconds` alone carries the sign (`-0.5`s is `{-1, 500000000}`, not
  `{0, -500000000}`) -- which is also why plain `seconds` comparison is a correct total
  ordering, `nanos` only breaking ties. `duration_of_seconds(s, n)` normalizes rather than
  rejects, `duration_zero()`, `add`/`subtract`/`negate`, `duration_compare`, `format`
  (`"PT{seconds}[.{nanos}]S"`; a negative duration prints its true magnitude with a leading
  `-`, not a misleading field-by-field negative) and its exact inverse `parse_duration`.
- **`Instant { seconds, nanos: int }`** — a point on the UTC timeline, same shape and
  invariant as `Duration` (not a packed `bigint` nanosecond count). `instant_now()` reads
  the real system clock (`clock` module's `clock_gettime`, Ziman's only non-determinism
  besides `random`); `localdatetime_now(offset)`/`localdate_now(offset)`/
  `localtime_now(offset)` read the current wall-clock time on a clock in `offset`.
  `instant_of(s, n)` (normalizing), `add`/`subtract` (with a `Duration`), `diff(a, b) ->
  Duration`, `instant_compare`, `to_localdatetime(i, offset: ZoneOffset) -> LocalDateTime` /
  `to_instant(dt, offset)` (the only place `ZoneOffset` does something -- shifting a UTC instant
  to/from wall-clock time), `format` (UTC ISO-8601 via
  `zoneoffset_utc()`) and its inverse `parse_instant` (accepts `"Z"` or a real offset
  suffix), and `format(i, offset, fmt)`/`parse_instant(s, fmt)` (offset handled separately
  from the pattern).
- **`ZoneOffset { total_seconds: int }`** — a fixed UTC offset (`+05:30`), not a named zone.
  `zoneoffset_utc()`, `zoneoffset_of_seconds(s) -> Result<ZoneOffset, string>` (validated to
  `±18:00`), `zoneoffset_of_hours_minutes(h, m)` (rejects mismatched signs -- `(-5, 30)` is
  invalid, must be `(-5, -30)` or `(-5, 0)`), `zoneoffset_compare`, `format` (`"Z"` for UTC,
  `"+HH:MM"`/`"-HH:MM"` otherwise)/`parse_offset`.
- **`ZoneId`** — an IANA time zone (`"Europe/Paris"`) with its rules, read from the OS zone
  files (`$TZDIR`, else `/usr/share/zoneinfo`); `zoneid_of(name)` is an `Err` for an unknown
  name or no zone database, `zoneid_utc()`/`"UTC"` never need the files.
  `zoneid_offset_at(z, instant)` is the offset in force then, with `zoneid_is_dst_at`/
  `zoneid_abbreviation_at` (`EST`/`EDT`). `to_localdatetime(i, z: ZoneId)`/
  `to_instant(dt, z: ZoneId)` -- the same two names as the `ZoneOffset` forms, told apart by the
  second argument -- apply daylight saving: an ambiguous wall-clock time
  (clocks going back) is the earlier instant, a nonexistent one (clocks going forward) is
  moved forward by the gap's length; a zone file's trailing rule string covers dates past
  its last listed transition. `format`/`parse_zone` are the bare zone name; two
  `ZoneId`s of the same zone compare equal.
- **No `<`/`>` on any of these seven types** -- use `compare`/`Ord` instead: each has a
  `<type>_compare(a, b) -> int` function, which is what makes it `Ord` (works in `sort`,
  as an `Ord`-bounded type argument). `==`/`!=` come free from structural equality on the
  plain `int`/struct fields. Calendar year is assumed non-negative throughout (no BCE).

`clock.sleep_millis(ms)` (`use clock`) blocks the calling thread only, for at least `ms`
milliseconds (a non-positive value returns at once) -- for retry backoff and tests needing
overlapping work.

## JSON

The `json` core module (`use json.Name`) is a parser/serializer, pure Ziman.

```ziman
use json
use json.Json
use json.parse

fn main() {
    val j = unwrap(parse("{\"name\": \"Ada\", \"age\": 210}"))
    print(json.format(j))                    // {"name":"Ada","age":210}
    val name = json.get(j, "name")            // Option<Json>, not Result -- unwrap_Some, not unwrap
    print(Json.unwrap_JString(Option.unwrap_Some(name)))  // Ada
}
```

- **`Json`** is a recursive enum: `JNull`, `JBool(bool)`, `JNumber(Number)`, `JString(string)`,
  `JArray([Json])`, `JObject(Map<string, Json>)`.
- **Objects are Maps** — near-O(1) lookup, insertion-order iteration (a repeated key keeps its
  first position with the last value; `parse_strict` errors on one instead).
  `parse`/`_bytes_strict` are the same over `[byte]`, no string made first. Build one
  with `Json.JObject(Map<string, Json> { "k": v })`.
- **Numbers.** `JNumber` holds a `json.Number` (`enum Number { Int(int) Float(float)
  Bigint(bigint) Decimal(decimal) }`) recording how it was written. A 64-bit-fitting integer
  parses to `Int`; a larger one to `Bigint` with exact digits (never a float that's lost low
  digits); a fraction/exponent to `Float`. `parse_exact`/`_bytes_exact` read a
  fraction/exponent as `Decimal` instead, keeping every digit and the scale exactly;
  `decimal_to_json`/`json.format` round-trip it, and a `Bigint` writes as its digits. Readers
  decide losslessness: `int_from_json` (any in-range whole number), `bigint_from_json` (any
  whole number), `decimal_from_json`/`float_from_json` (any number, rounding a bigint/decimal
  like a cast would); `#[derive(Json)]` may have `bigint`/`decimal` fields. `number_of(j)` is
  the `Number` of a `JNumber`, or `None`; `switch` on it (no nested patterns) to branch on kind.
  Grammar is JSON's (`01`, `+1`, `.5`, `5.`, a dangling exponent are errors); an integer or exact
  decimal caps at 1000 digits (exponent at 100000).
- **`parse(s) -> Result<Json, string>`** — native, errors give the byte position, nesting
  past 512 levels errors. All JSON escapes read (`\" \\ \/ \b \f \n \r \t`, `\uXXXX` with
  surrogate pairs, decoded to UTF-8); `\u0000` errors (a string can't hold a 0 byte).
- **`json.format(j) -> string`** — native, compact, shortest-round-trip floats, NaN/infinity
  as `null`, control chars as `\b \f \u00XX`. `json.format(j, indent)` indents.
- **Lookups**: `json.get(j, key) -> Option<Json>`, `get_path(j, ["items", "0",
  "name"]) -> Option<Json>` (array segment = decimal index), `require`/
  `_string_field`/`_int_field`/`_float_field`/`_bool_field` (all `Result`, message names the field).
- **Required fields**: `require(j, key) -> Result<Json, string>` and the typed readers
  `require_string`/`require_int`/`require_bigint`/`require_decimal`/`require_float`/`require_bool`,
  each an Err naming the field when it is missing or the wrong shape. Use `require_decimal` for
  money: `require_float` answers the nearest binary float and shortest-round-trip display hides
  it (`19.99 + 19.99` prints `39.98` either way; `0.1 + 0.2` is `0.30000000000000004` through
  floats and `0.3` through decimals).
- **Map helpers**: `json_to_map<V>(j, decode)`/`json_from_map<V>(m, encode)` convert an object
  to/from `Map<string, V>`; `to_string_map(j)` is the `Map<string, string>` case.
- **No `==`/`!=` on `Json`** — array equality needs a scalar element type, and `Json` isn't one.
- **A general parsing engine, not a schema-specific request parser** — decoding into one
  known struct is application code built on `json.parse` + `json.get` + validation.

## Media types (`mime`)

`mime` names a kind of content: `mime.parse("text/html; charset=utf-8")` reads a media type (a `MediaType`
with `type`, `subtype` and `params`, all names lower-cased, an Err for a malformed one), `mime.format`
writes it back (quoting a parameter value that is not a token), `mime.essence` is `type/subtype` alone,
`mime.charset` and `mime.param` read parameters, and `mime.with_param` sets one. `mime.matches(pattern,
candidate)` compares with wildcards and ignores parameters (`*/*`, `text/*`); `mime.is_json` is true for
`application/json` and any `+json` type. `mime.of_extension("csv")` and `mime.of_path(path)` give the type of a
file, or None for an extension it does not know. Well-known types are functions (`mime.application_json()`,
`mime.text_csv()`, ...). The module has no HTTP in it: the `Accept` header and its weights are `http`'s.

## Serialization formats (`ndjson`, `csv`, `toml`, `yaml`)

Each format is a pure-Ziman module that converts text to and from a `Json` tree, so
`#[derive(Json)]` types decode from (and encode to) every format for free:

```ziman
use json.Json
use toml

#[derive(Json)]
struct Server { host: string  port: int  tags: [string]  limits: Map<string, int> }

fn main() {
    val tree = unwrap(toml.parse("host = \"a\"\nport = 80\ntags = [\"x\"]\n[limits]\nrps = 10\n"))
    val server = unwrap(server_from_json(tree))          // the same call for yaml.parse, csv rows, ...
    print(unwrap(toml.format(server_to_json(server))))
}
```

| Module | Reads | Writes |
| --- | --- | --- |
| `ndjson` | `ndjson.parse(text) -> Result<[Json], string>` (one value per line, blank lines skipped); `records(path)` reads a file lazily as an `Iter` of `Result<Json, string>` for a `for` loop, one record in memory at a time | `ndjson.format([Json])` |
| `csv` | `parse` / `parse_typed` -> a `Json` array of objects keyed by the header row; `parse_rows` -> `[[string]]`; `parse_columns` / `parse_columns_typed` for a headerless file; each also takes a delimiter as a second argument (`parse(text, "\t")`); `records(path)` (and `records_typed`) reads a file lazily as an `Iter` of `Result<Json, string>` for a `for` loop, a record at a time, quoted newlines included | `format(Json)`, `format([[string]])` |
| `toml` | `toml.parse(text) -> Result<Json, string>` | `toml.format(Json) -> Result<string, string>` |
| `yaml` | `yaml.parse(text) -> Result<Json, string>` | `yaml.format(Json) -> string` |

- **Errors** are `Err(string)` with a `line N:` prefix (CSV: `row N`).
- **CSV**: RFC 4180 quoting; LF/CRLF/lone-CR endings; blank lines skipped; a leading UTF-8 BOM
  ignored. `parse` keeps every cell a string; `parse_typed` reads an unquoted cell as a
  number (`42`, `-1.5`, not `007`), boolean, or null (empty) -- a quoted cell stays a string.
  `parse_columns`/`_typed` take column names for a headerless file. **The delimiter is a second
  argument, not a second name**: `parse(text, "\t")`, `format(rows, ";")` and so on, one
  character and not a quote or a newline (invalid is `Err`). They are two declarations under one
  name rather than one with an `Option<string>`, because Ziman omits an optional parameter only
  in an all-named call -- which would make the common `parse(text)` into `parse(text: text)`.
  `typed` goes the other way and stays a *name*, a bare `parse(text, true)` saying nothing.
  Errors name the line a row started on (including an unclosed quote's own start). The writer
  unions the rows' keys as columns (first-seen order), flattens an object one level into `a.b`
  columns, rejects deeper nesting, and quotes a string that would otherwise read back typed
  (`parse_typed(format(x))` round-trips); lines end in `\n`.
- **TOML 1.0**: tables, arrays of tables, dotted/inline tables, all string forms, hex/octal/
  binary/`_` integers, floats, booleans, comments. Dates/times stay `JString` text; `inf`/`nan`
  rejected; a duplicate key/table errors. The writer omits `JNull` fields (no TOML null),
  writing scalars, then sub-tables, then `[[arrays]]`.
- **YAML** is a strict subset of 1.2: block maps/sequences, flow `[..]`/`{..}`, plain/quoted
  scalars, `|`/`>` block scalars, comments, a leading `---`; core-schema typing (`null`/`~`,
  `true`/`false`, numbers; `yes`/`no` stay strings, `007` stays a string). Anchors, aliases,
  tags, `<<` merge keys, `? ` keys, multiple documents and tab indentation are errors, not
  misreadings. The writer quotes exactly what would re-read as another type.
- Ordinary Ziman on the tree-walking interpreter -- fine for config/export-sized data, slow for
  megabytes.

## Ordering (`Ord`, `compare`, sorting)

```ziman ignore: `people` and `patients` stand for the reader's own arrays

#[derive(Ord)]
struct Person { last: string  first: string }

val sorted = sort<Person>(people)                     // by last, then first
val by_age = sort_by<Patient, int>(patients, fn(p: Patient) -> int { return p.age })
```

- **`< > <= >=`** work on `int`/`float`/`bigint`/`decimal`/`byte`/`string` (byte-by-byte then
  length for strings) and stay non-overloadable; a struct/enum uses `compare(a, b)`.
- **`compare<T: Ord>(a, b) -> int`**: negative/zero/positive -- only the sign is meaningful, not
  the magnitude (a hand-written `_compare` may return `a.score - b.score`; nothing relies on
  exactly `-1`/`0`/`1`). `Ord` is satisfied by the six scalars, by a struct/enum with
  `<snake_case_type>_compare(a, b) -> int` (`Person` looks for `person_compare`; the spelling
  without inserted underscores, `localdate_compare`, is also accepted, used by `time`), by an
  `Ord`-bounded type parameter, and recursively by **arrays** (lexicographic), **`Option<T>`**
  (`None` before every `Some`) and **`Result<T, E>`** (every `Ok` before every `Err`) whose
  element/payload is `Ord` -- so `[Option<Result<int, string>>]` sorts. `bool` and channels have
  no order; `float`'s order is total (NaN sorts after everything, equals itself).
- **`#[derive(Ord)]`** writes `<type>_compare`: a struct compares fields in declaration order
  (first difference decides); an enum by variant position then payload. Every field/payload must
  itself be `Ord`; a fieldless struct can't derive it. Generic: `#[derive(Ord)] struct Pair<A,
  B>` writes `pair_compare<A: Ord, B: Ord>`, so `Pair<int, string>` is `Ord` but `Pair<int,
  bool>` is rejected.
- **`sort<T: Ord>(items) -> [T]`** is a stable merge sort (new array). Sort by
  something other than the type's own order with **`sort_by<T, K: Ord>(items, key: fn(T) -> K)
  -> [T]`**, which calls `key` **once per element** where the comparator form recomputes it per
  comparison, or by a comparator function via **`sort_with<T>(items, cmp: fn(T, T) -> int)`**.
  Descending: `sort_desc`/`sort_by_desc` -- also stable, not the same as
  reversing ascending (which flips ties). `inverse(cmp)` turns a comparator round for `sort_with`,
  but cannot reverse a key function, which is why `sort_by_desc` is its own name. `sort`,
  `sort_desc`, `sort_with`, `sort_by` and `sort_by_desc` each take an array **or** a
  `MutableArray`, answering a new array for the first and sorting the buffer in place for the
  second. A
  packed `[int]`/`[float]`/`[byte]` or string array sorts natively (~10x faster); anything else
  calls `compare` in a merge sort. Also `reverse`, and for sorted sequences `is_sorted`,
  `lower_bound`/`upper_bound` (insertion points) and `binary_search` (`Option<int>`) -- those four
  take either world too, and the "already sorted" precondition is the caller's in both.
  `array_order_of(keys, descending)` is the permutation underneath, generic over the key type and
  the thing `sort_by` feeds to `permute`; the `sort_indices`/`_desc` wrappers over it were deleted
  in F169, never having had a caller.
  `Uuid` and every `time` type are `Ord`.
- **`error_message(e) -> string`** dispatches like `compare` but for a different suffix: an enum
  `e: NetError` looks for `net_error_message`, and so on for any enum with its own
  `<snake_case_type>_message(e) -> string` (`NetError`/`DbError`/`JwtError`/`ClientError` already
  have one). No bound to name and no array/Option/Result recursion -- one concrete enum in, one
  string out.
