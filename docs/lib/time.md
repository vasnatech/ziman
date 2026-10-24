# `time`

Dates, times, durations and instants, modeled on java.time and written in ordinary
Ziman (the only native code is `clock.now_raw`, which reads the system clock).

Seven types, all plain structs with `==`/`!=` and a `_compare` function that makes them `Ord`
(so `compare` and `array_sort` work on them):


```
LocalDate       a calendar date: year, month, day
LocalTime       a time of day, to the nanosecond
LocalDateTime   a date and a time of day, with no zone
Duration        a length of time: seconds and nanoseconds
Instant         a point on the timeline: seconds and nanoseconds since 1970-01-01T00:00:00Z
ZoneOffset      a fixed offset from UTC, such as `+05:30`
ZoneId          an IANA time zone such as `Europe/Paris`, with its daylight saving rules
```

Build a value with its `<type>_of` function, which checks the fields and returns a `Result`; add
to it with `add_*`; compare with `<type>_compare`. Every type reads and writes its ISO-8601 form
with `format` and its own `parse*` (`2026-09-24`, `13:05:09.5`, `PT1.5S`, `+05:30`). The calendar
types and `Instant` also take a `DateTimeFormat`, a pattern compiled once by
`datetime_format_of` and reused: `yyyy MM dd HH mm ss` and `SSSSSSSSS` (nanoseconds), plus
`MMM`/`MMMM` and `EEE`/`EEEE` for the month and day as names, taken from the host's locale data,
anything else copied through as literal text. A compiled pattern carries no locale; the locale
goes to the call that renders (`format(d, fmt, l)`), or is `locale.default()`. Every type has
`_to_json` / `_from_json` and `_to_sql` / `_from_sql`, so `#[derive(Json)]` and `#[derive(Row)]`
handle a field of these types (an ISO string, and a TEXT column).

**A name here carries its type only where the arguments cannot say it.** `format`, `add`,
`subtract`, `negate`, `diff` and the `add_*` family are one overload set each, resolved on the
argument; `<type>_of` and `<type>_compare` keep the prefix because they differ only in return
type or are looked up by name (`Ord`, `#[derive]`); and the parse family needs one name per type
-- `parse_localdate`, `parse_time`, `parse_datetime`, `parse_duration`, `parse_offset`,
`parse_zone`, `parse_instant` -- because `parse(s, fmt)` is a single signature four of them want.

**Messages in here are `prelude.format`, not `format`.** This module's own `format` has taken the
bare name, so the interpolating builtin is reached through the `prelude.` qualifier (see
docs/SYNTAX.md's "Modules"). A plain `concat` is left where there is no message to template --
`pad_left`'s per-digit append, the pattern scanner's `literal_buf`, and the two places that join
already-computed strings with no literal text between them.

Named zones (`zoneid_of("Europe/Paris")`) come from the system's IANA zone files (see `zone`), so
they need the operating system's tzdata package; `zoneid_offset_at` gives the offset at an
instant and `instant_to_localdatetime_in` / `localdatetime_to_instant_in` convert with daylight
saving applied.

Limits: years are 0 or later (no BCE). Month and day names (`MMM`, `MMMM`, `EEE`, `EEEE`) come
from the host's locale data as of 2026-10-23, so they are as good as the operating system's and
no better -- see lib/core/locale.zm for what that does and does not answer. What is still absent
is era, quarter and week-of-year fields, a localised *ordering* of the fields themselves (this
renders the pattern it is given, so a caller writes `dd MMMM yyyy` or `MMMM dd, yyyy` as the
language wants), and anything 12-hour (`hh`/`a`).

`use time` -- or `use time.Name` to bring one item in bare.

## Example

```
use time

// Dates, times, durations and instants. Constructors check their fields and return a Result.
fn main() {
    val d = unwrap(time.localdate_of(2026, 1, 31))
    print(time.format(d))
    print(time.format(time.add_months(d, 1)))  // clamped to the month's length
    print(is_ok(time.localdate_of(2026, 2, 30)))
    print(time.diff_days(unwrap(time.localdate_of(2026, 12, 25)), d))

    val dt = time.localdatetime_of(d, unwrap(time.localtime_of(23, 30, 0, 0)))
    print(time.format(time.add_minutes(dt, 45)))  // carries into the next day

    // An instant is a point on the timeline; an offset says how a clock reads it.
    val at = time.localdatetime_to_instant(dt, unwrap(time.zoneoffset_of_hours_minutes(5, 30)))
    print(time.format(at))
    print(time.format(time.add(at, time.duration_of_seconds(90, 0))))

    // Any type reads back its own text form; a pattern is compiled once and reused.
    print(time.format(unwrap(time.parse_duration("PT1.5S"))))
    val fmt = time.datetime_format_of("dd/MM/yyyy HH:mm")
    print(time.format(dt, fmt))
}
```

Output:

```
2026-01-31
2026-02-28
false
328
2026-02-01T00:15:00
2026-01-31T18:00:00Z
2026-01-31T18:01:30Z
PT1.500000000S
31/01/2026 23:30
```

### `is_leap_year`

```
pub fn is_leap_year(year: int) -> bool
```

True when `year` is a leap year in the Gregorian calendar.

It takes a *year*, not a `LocalDate` -- which the old name `localdate_is_leap_year` obscured, and
dropping the prefix made obvious: the public function was a one-line wrapper over this private
one with the identical signature, so the rename turned it into a duplicate. One definition now.

### `DateTimeFormat`

```
pub struct DateTimeFormat {
    parts: [DateTimePart]
    width: int
}
```

A pattern, compiled once (by datetime_format_of) into a form
format_fields/parse_fields can walk directly, without ever re-
scanning the original pattern text -- the whole point of this type
existing separately from a bare pattern string, since a
`DateTimeFormat` is meant to be built once (see the `iso_*_format`
global vals below, or a project's own, compiled once wherever it's
declared) and then reused for many format/parse calls. `width` is
the pattern's total rendered/parsed length, precomputed at compile
time from `parts` (never recomputed per call) -- see the pattern-
language comment above for why every part's width is already fixed
and known. `parts`/`width` are implementation details, not part of
the intended public API, same as `HashMap`'s `buckets`/`count`.

### `datetime_format_of`

```
pub fn datetime_format_of(pattern: string) -> DateTimeFormat
```

Scans `pattern` exactly once, building the compiled parts array
(and its total width) that format_fields/parse_fields below then
work from -- the one place the pattern text itself is ever matched
character by character.
`parts` is built via MutableArray<DateTimePart> rather than `push` in
a loop -- an arbitrarily long/verbose pattern used to mean one
leaked, ever-growing array per recognized token (see docs/DESIGN.md's
"MutableArray" entry). `literal_buf`, in contrast, is left as plain
`concat` on purpose: it only ever accumulates the handful of literal
characters *between* two tokens in a pattern (a "-", a ":", at most a
few characters in any realistic pattern), reset back to "" the
instant a token flushes it -- a fixed, small bound with nothing to
gain from a builder, the same reasoning `pad_left` above and
`int_to_hex`'s zero-padding loop are exempted from converting too.

### `iso_date_format`

```
pub val iso_date_format = datetime_format_of("yyyy-MM-dd")
```

The fixed shapes every existing `_to_string` produces, now also the
default `DateTimeFormat`s their matching `_parse`/`_format` pair
uses -- one engine, not two independently-maintained implementations
of "what does an ISO date look like." Compiled exactly once, at
program start (global vals are evaluated eagerly before `main` runs
-- see docs/DESIGN.md's "Global val" entry), and reused for the rest
of the process's lifetime; `pub` so application code can also pass
them straight to `_format`/`_parse_pattern` instead of compiling the
pattern again itself.

### `iso_time_format`

```
pub val iso_time_format = datetime_format_of("HH:mm:ss")
```

### `iso_time_nanos_format`

```
pub val iso_time_nanos_format = datetime_format_of("HH:mm:ss.SSSSSSSSS")
```

### `iso_datetime_format`

```
pub val iso_datetime_format = datetime_format_of("yyyy-MM-ddTHH:mm:ss")
```

### `iso_datetime_nanos_format`

```
pub val iso_datetime_nanos_format = datetime_format_of("yyyy-MM-ddTHH:mm:ss.SSSSSSSSS")
```

### `LocalDate`

```
pub struct LocalDate {
    year: int
    month: int
    day: int
}
```

A calendar date with no time or zone: a proleptic Gregorian `year`, `month` (1-12) and `day`.
Build one with `localdate_of`, which checks the fields.

The year is **signed**, and negative years are a supported case rather than an accident: 44 BC is
`-0044`, `add_days` walks back past year 1 into it (the fixture
`time_year_range_roundtrip` pins `-0044-03-15` round-tripping and an 800,000-day subtraction
landing on `-2190-09-05`), `format_year` writes a sign outside 0-9999, and
`parse` reads a signed year of 4 to 9 digits. This comment used to say "0 or
later", which is what led the 2026-10-14 review to report accepting BCE years as a bug (D19):
the code was right and the sentence describing it was not.

### `localdate_of`

```
pub fn localdate_of(year: int, month: int, day: int) -> Result<LocalDate, string>
```

The date `year`-`month`-`day`, or an Err naming the field that is out of range (the day is checked
against that month's length, leap years included).

### `to_epoch_day`

```
pub fn to_epoch_day(d: LocalDate) -> int
```

The number of days from 1970-01-01 to `d`; negative before it.

### `localdate_from_epoch_day`

```
pub fn localdate_from_epoch_day(epoch_day: int) -> LocalDate
```

The date `epoch_day` days after 1970-01-01 (the inverse of `to_epoch_day`).

### `add_days`

```
pub fn add_days(d: LocalDate, days: int) -> LocalDate
```

`d` moved by `days` (negative to go back), across month and year ends.

### `add_months`

```
pub fn add_months(d: LocalDate, months: int) -> LocalDate
```

Clamps the day to the target month's own length, same as
java.time.LocalDate.plusMonths -- Jan 31 + 1 month is Feb 28 (or 29),
never "Mar 3".

### `add_years`

```
pub fn add_years(d: LocalDate, years: int) -> LocalDate
```

`d` moved by `years` years; 29 February in a non-leap target year becomes the 28th, as in
`add_months`.

### `diff_days`

```
pub fn diff_days(a: LocalDate, b: LocalDate) -> int
```

The whole days from `b` to `a`: positive when `a` is the later date.

### `localdate_compare`

```
pub fn localdate_compare(a: LocalDate, b: LocalDate) -> int
```

Negative, zero or positive as `a` is before, the same as, or after `b`. This makes `LocalDate`
`Ord`.

### `format`

```
pub fn format(d: LocalDate, fmt: DateTimeFormat) -> string
```

`d` rendered with a compiled pattern from `datetime_format_of`. Fields the date lacks (hour,
minute, second, nanosecond) print as zeros.

```
pub fn format(d: LocalDate, fmt: DateTimeFormat, l: Locale) -> string
```

The same, with the names drawn from `l`.//
**The compiled pattern carries no locale and the call does.** A `DateTimeFormat` is locale-free on
purpose -- one pattern renders in any language -- so a form taking a `Locale` sits beside the one
that uses `locale.default()`. Only a pattern with a name token (`MMM`, `MMMM`, `EEE`, `EEEE`)
reads the locale at all; `yyyy-MM-dd` renders identically under every one.

### `parse_localdate`

```
pub fn parse_localdate(s: string, fmt: DateTimeFormat) -> Result<LocalDate, string>
```

Reads `s` with a compiled pattern; an Err says which part did not match, or that the resulting
date does not exist (31 February).

```
pub fn parse_localdate(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalDate, string>
```

The same, reading any name token under `l`.

```
pub fn parse_localdate(s: string) -> Result<LocalDate, string>
```

Reads an ISO date, `2026-09-24`; an Err otherwise. The inverse of `format`.

### `format`

```
pub fn format(d: LocalDate) -> string
```

`d` as an ISO date, `2026-09-24`.

### `LocalTime`

```
pub struct LocalTime {
    hour: int
    minute: int
    second: int
    nano: int
}
```

A time of day with no date or zone: `hour` (0-23), `minute`, `second` and `nano` (nanosecond
within the second). Build one with `localtime_of`, which checks the fields.

### `localtime_of`

```
pub fn localtime_of(hour: int, minute: int, second: int, nano: int) -> Result<LocalTime, string>
```

The time `hour`:`minute`:`second`.`nano`, or an Err naming the field that is out of range.

### `add_nanos`

```
pub fn add_nanos(t: LocalTime, nanos: int) -> LocalTime
```

`t` moved by `nanos` nanoseconds (negative to go back). Wraps at midnight and drops the day carry,
like java.time.LocalTime.plusNanos; use `add_nanos` to keep it.

### `add_seconds`

```
pub fn add_seconds(t: LocalTime, seconds: int) -> LocalTime
```

`t` moved by `seconds`, wrapping at midnight (see `add_nanos`).

### `add_minutes`

```
pub fn add_minutes(t: LocalTime, minutes: int) -> LocalTime
```

`t` moved by `minutes`, wrapping at midnight.

### `add_hours`

```
pub fn add_hours(t: LocalTime, hours: int) -> LocalTime
```

`t` moved by `hours`, wrapping at midnight.

### `diff_nanos`

```
pub fn diff_nanos(a: LocalTime, b: LocalTime) -> int
```

The nanoseconds from `b` to `a` within one day: positive when `a` is later. Both are read as
times of the same day, so the result is in -86399999999999 to 86399999999999.

### `localtime_compare`

```
pub fn localtime_compare(a: LocalTime, b: LocalTime) -> int
```

Negative, zero or positive as `a` is earlier than, the same as, or later than `b`. This makes
`LocalTime` `Ord`.

### `format`

```
pub fn format(t: LocalTime, fmt: DateTimeFormat) -> string
```

`t` rendered with a compiled pattern from `datetime_format_of`. Year, month and day print as
zeros.

```
pub fn format(t: LocalTime, fmt: DateTimeFormat, l: Locale) -> string
```

The same under `l`. A time has no month or weekday, so a name token in the pattern renders from
the zeros the date fields hold -- which is what it did before a locale existed too.

### `parse_time`

```
pub fn parse_time(s: string, fmt: DateTimeFormat) -> Result<LocalTime, string>
```

Reads `s` with a compiled pattern; an Err says which part did not match or which field is out of
range.

```
pub fn parse_time(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalTime, string>
```

The same, reading any name token under `l`.

```
pub fn parse_time(s: string) -> Result<LocalTime, string>
```

`to_string`'s own output only includes the fractional part when
`nano != 0` (see format below), so parsing has to
detect which of the two fixed shapes it's looking at the same way --
checking for "." is safe here since neither shape ever contains one
otherwise.

### `format`

```
pub fn format(t: LocalTime) -> string
```

`t` as ISO text, `13:05:09`, with `.nnnnnnnnn` after the seconds only when the nanoseconds are not
zero.

### `Duration`

```
pub struct Duration {
    seconds: int
    nanos: int
}
```

A length of time as `seconds` and `nanos` (0 to 999999999). `seconds` carries the sign, so -0.5 s
is `{seconds: -1, nanos: 500000000}`. Build one with `duration_of_seconds`, which keeps that form.

### `duration_of_seconds`

```
pub fn duration_of_seconds(seconds: int, nanos: int) -> Duration
```

The duration `seconds` + `nanos`, with `nanos` allowed outside 0-999999999 and carried into
`seconds` (`duration_of_seconds(0, -1)` is -1 ns).

### `duration_zero`

```
pub fn duration_zero() -> Duration
```

The empty duration.

### `add`

```
pub fn add(a: Duration, b: Duration) -> Duration
```

The sum of two durations.

### `negate`

```
pub fn negate(d: Duration) -> Duration
```

`d` with its sign flipped.

### `subtract`

```
pub fn subtract(a: Duration, b: Duration) -> Duration
```

`a` minus `b`.

### `duration_compare`

```
pub fn duration_compare(a: Duration, b: Duration) -> int
```

seconds/nanos direct comparison is sound (not just convenient)
specifically because of the normalize invariant: nanos is always
non-negative, so seconds alone is the correct primary ordering key
and nanos only ever needs to break a tie.

### `format`

```
pub fn format(d: Duration) -> string
```

ISO-8601-shaped (PT<seconds>[.<nanos>]S), not full ISO-8601 duration
support (no PnYnMnDTnHnMnS breakdown) -- nothing needs interop with
an external system that would require exact compliance. Negative
durations are shown by their true magnitude with a leading '-'
(`{seconds: -1, nanos: 500000000}`, i.e. -0.5s, prints "PT-0.5...S",
not the misleading "PT-1.5...S" a naive field-by-field print would
give), the same care java.time.Duration.toString() takes. No
pattern-based `duration_format` -- a total elapsed-seconds count
isn't a calendar field breakdown the way year/month/day/hour/
minute/second are, so the pattern language above doesn't fit it;
`parse_duration` below is this function's exact inverse instead.

### `parse_duration`

```
pub fn parse_duration(s: string) -> Result<Duration, string>
```

The exact inverse of format, including its negative-
duration magnitude transform: a negative-with-fraction string stores
`mag_seconds = -true_seconds - 1` and `mag_nanos = 1e9 - true_nanos`,
so parsing has to invert that arithmetic rather than just negating
the parsed seconds -- verified against format's own
worked example (-0.5s prints "PT-0.500000000S") before trusting it.

### `LocalDateTime`

```
pub struct LocalDateTime {
    date: LocalDate
    time: LocalTime
}
```

A date and a time of day with no zone: the wall-clock reading on a calendar. Pair it with a
`ZoneOffset` (`localdatetime_to_instant`) to place it on the timeline.

### `localdatetime_of`

```
pub fn localdatetime_of(date: LocalDate, time: LocalTime) -> LocalDateTime
```

The date-time made of `date` and `time`.

### `add_nanos`

```
pub fn add_nanos(dt: LocalDateTime, nanos: int) -> LocalDateTime
```

`dt` moved by `nanos` nanoseconds (negative to go back), carrying into the date.

### `add_seconds`

```
pub fn add_seconds(dt: LocalDateTime, seconds: int) -> LocalDateTime
```

`dt` moved by `seconds`, carrying into the date.

### `add_minutes`

```
pub fn add_minutes(dt: LocalDateTime, minutes: int) -> LocalDateTime
```

`dt` moved by `minutes`, carrying into the date.

### `add_hours`

```
pub fn add_hours(dt: LocalDateTime, hours: int) -> LocalDateTime
```

`dt` moved by `hours`, carrying into the date.

### `add_days`

```
pub fn add_days(dt: LocalDateTime, days: int) -> LocalDateTime
```

`dt` moved by `days`; the time of day is unchanged.

### `add_months`

```
pub fn add_months(dt: LocalDateTime, months: int) -> LocalDateTime
```

`dt` moved by `months`; the day is clamped to the target month as in `add_months`.

### `add_years`

```
pub fn add_years(dt: LocalDateTime, years: int) -> LocalDateTime
```

`dt` moved by `years`; 29 February becomes the 28th in a non-leap target year.

### `localdatetime_compare`

```
pub fn localdatetime_compare(a: LocalDateTime, b: LocalDateTime) -> int
```

Negative, zero or positive as `a` is before, the same as, or after `b`. This makes `LocalDateTime`
`Ord`.

### `diff`

```
pub fn diff(a: LocalDateTime, b: LocalDateTime) -> Duration
```

Treats both sides as wall-clock time with no zone (exactly what
LocalDateTime means) -- well-defined without needing a ZoneOffset,
since it's just "elapsed calendar time assuming a fixed calendar",
not a real elapsed-wall-time-across-zones computation.

### `format`

```
pub fn format(dt: LocalDateTime, fmt: DateTimeFormat) -> string
```

`dt` rendered with a compiled pattern from `datetime_format_of`.

```
pub fn format(dt: LocalDateTime, fmt: DateTimeFormat, l: Locale) -> string
```

The same, with the names drawn from `l`.

### `parse_datetime`

```
pub fn parse_datetime(s: string, fmt: DateTimeFormat) -> Result<LocalDateTime, string>
```

Reads `s` with a compiled pattern; an Err says which part did not match or which field is out of
range.

```
pub fn parse_datetime(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalDateTime, string>
```

The same, reading any name token under `l`.

```
pub fn parse_datetime(s: string) -> Result<LocalDateTime, string>
```

Reads an ISO date-time, `2026-09-24T13:05:09` with an optional fraction of 1 to 9 digits; an Err
otherwise.

### `format`

```
pub fn format(dt: LocalDateTime) -> string
```

`dt` as ISO text, `2026-09-24T13:05:09`, with the fraction only when the nanoseconds are not zero.

### `ZoneOffset`

```
pub struct ZoneOffset {
    total_seconds: int
}
```

A fixed offset from UTC in seconds, east positive, at most 18 hours either way. Build one with
`zoneoffset_of_seconds` or `zoneoffset_of_hours_minutes`.

### `zoneoffset_utc`

```
pub fn zoneoffset_utc() -> ZoneOffset
```

The zero offset, written `Z`.

### `zoneoffset_of_seconds`

```
pub fn zoneoffset_of_seconds(total_seconds: int) -> Result<ZoneOffset, string>
```

The offset of `total_seconds` from UTC, or an Err beyond 18 hours (64800 seconds) either way.

### `zoneoffset_of_hours_minutes`

```
pub fn zoneoffset_of_hours_minutes(hours: int, minutes: int) -> Result<ZoneOffset, string>
```

The offset of `hours` and `minutes` from UTC, both negative for a west offset (`-5, -30`). An Err
when they disagree in sign or the total exceeds 18 hours.

### `zoneoffset_compare`

```
pub fn zoneoffset_compare(a: ZoneOffset, b: ZoneOffset) -> int
```

Negative, zero or positive as `a` lies west of, at the same place as, or east of `b`. This makes
`ZoneOffset` `Ord`.

### `format`

```
pub fn format(z: ZoneOffset) -> string
```

`z` as `Z` for UTC, otherwise `+HH:MM` or `-HH:MM`.

### `parse_offset`

```
pub fn parse_offset(s: string) -> Result<ZoneOffset, string>
```

The exact inverse of format: "Z" for UTC, otherwise
exactly "+HH:MM"/"-HH:MM" (6 characters, no seconds component --
format never emits one either).

### `ZoneId`

```
pub struct ZoneId {
    name: string
    rules: ZoneRules
}
```

A time zone by its IANA name (`UTC`, `Europe/Paris`, `America/New_York`), carrying the rules that
say which offset from UTC applies at each instant, daylight saving included. Get one with
`zoneid_of`; `zoneid_utc()` needs no zone files. Its `name` is what is written to JSON and SQL.

### `zoneid_utc`

```
pub fn zoneid_utc() -> ZoneId
```

The `UTC` zone.

### `zoneid_of`

```
pub fn zoneid_of(name: string) -> Result<ZoneId, string>
```

The zone called `name`, read from the system's IANA time zone files (see `zone`), or an Err
when the name is unknown or the system has no zone database. `UTC` and `Z` always work.

### `zoneid_offset_at`

```
pub fn zoneid_offset_at(z: ZoneId, i: Instant) -> ZoneOffset
```

The offset from UTC in force in `z` at the instant `i`: `-05:00` in New York in January,
`-04:00` in July.

### `zoneid_is_dst_at`

```
pub fn zoneid_is_dst_at(z: ZoneId, i: Instant) -> bool
```

True when `z` is on daylight saving time at the instant `i`.

### `zoneid_abbreviation_at`

```
pub fn zoneid_abbreviation_at(z: ZoneId, i: Instant) -> string
```

The abbreviation `z` uses at the instant `i`: `EST`, `EDT`, `CET`, or `+03` where the zone has
no letters.

### `format`

```
pub fn format(z: ZoneId) -> string
```

The name of `z`.

### `parse_zone`

```
pub fn parse_zone(s: string) -> Result<ZoneId, string>
```

The zone called `s`; the same as `zoneid_of`, named for the `_from_string` convention.

### `Instant`

```
pub struct Instant {
    seconds: int
    nanos: int
}
```

A point on the timeline, as `seconds` since 1970-01-01T00:00:00Z and `nanos` (0 to 999999999);
negative instants are before the epoch. Build one with `instant_of` or `instant_now`.

### `instant_of`

```
pub fn instant_of(seconds: int, nanos: int) -> Instant
```

The instant `seconds` + `nanos` after the epoch, with `nanos` allowed outside 0-999999999 and
carried into `seconds`.

### `instant_now`

```
pub fn instant_now() -> Instant
```

The current time from the system clock. It is the wall clock and can jump when the system time is
adjusted, so it is not for timing an interval.

### `add`

```
pub fn add(i: Instant, d: Duration) -> Instant
```

`i` moved later by `d` (earlier when `d` is negative).

### `subtract`

```
pub fn subtract(i: Instant, d: Duration) -> Instant
```

`i` moved earlier by `d`.

### `diff`

```
pub fn diff(a: Instant, b: Instant) -> Duration
```

The time from `b` to `a`: positive when `a` is the later instant.

### `instant_compare`

```
pub fn instant_compare(a: Instant, b: Instant) -> int
```

Negative, zero or positive as `a` is before, the same as, or after `b`. This makes `Instant`
`Ord`.

### `instant_to_localdatetime`

```
pub fn instant_to_localdatetime(i: Instant, offset: ZoneOffset) -> LocalDateTime
```

The wall-clock date and time of `i` under `offset`.

### `localdatetime_to_instant`

```
pub fn localdatetime_to_instant(dt: LocalDateTime, offset: ZoneOffset) -> Instant
```

The instant at which a clock in `offset` reads `dt`.

### `localdatetime_now`

```
pub fn localdatetime_now(offset: ZoneOffset) -> LocalDateTime
```

The current date and time on a clock in `offset`: `instant_to_localdatetime(instant_now(), offset)`.

### `localdate_now`

```
pub fn localdate_now(offset: ZoneOffset) -> LocalDate
```

Today's date on a clock in `offset`.

### `localtime_now`

```
pub fn localtime_now(offset: ZoneOffset) -> LocalTime
```

The current time of day on a clock in `offset`.

### `instant_to_localdatetime_in`

```
pub fn instant_to_localdatetime_in(i: Instant, z: ZoneId) -> LocalDateTime
```

The wall-clock date and time in zone `z` at the instant `i`, daylight saving applied.

### `localdatetime_to_instant_in`

```
pub fn localdatetime_to_instant_in(dt: LocalDateTime, z: ZoneId) -> Instant
```

The instant at which a clock in zone `z` reads `dt`. Two readings need a rule. When the clocks
go back, `dt` happens twice and the earlier instant is returned. When the clocks go forward,
`dt` never happens (02:30 on the night the hour 02:00 to 03:00 is skipped) and the result is
`dt` moved forward by the length of the gap (03:30), as `java.time` does.

### `format`

```
pub fn format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat) -> string
```

Formats `i` as wall-clock time under `offset` (via
instant_to_localdatetime), using `fmt` for the calendar part, then
always appends the offset's own string form -- the same shape
format below already uses, just with `fmt` and `offset`
as explicit parameters instead of hardcoding
iso_datetime_format/zoneoffset_utc().

```
pub fn format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat, l: Locale) -> string
```

The same, with the names drawn from `l`. The zone-offset suffix is digits and signs, so it reads
the same under every locale.

### `parse_instant`

```
pub fn parse_instant(s: string, fmt: DateTimeFormat) -> Result<Instant, string>
```

`fmt` covers only the calendar part; the zone-offset suffix is
always parsed separately (via parse_offset) and used to convert
the parsed wall-clock time into an absolute Instant -- not something
a caller chooses per call, since the string's own suffix already
says what it is.

**Where the calendar part ends comes from the parse, not from a
width.** This sliced `s` at `fmt.width` first, which worked while
every token rendered exactly as many characters as it occupied in
the pattern. `MMMM` does not ("May" is three, "September" nine), so
the calendar part is parsed from the start and whatever it did not
consume is the offset. That reads the same for a fixed-width pattern
and works for a variable one, and it is why nothing here needs
`fmt.width` -- which is -1 for such a pattern anyway.

```
pub fn parse_instant(s: string, fmt: DateTimeFormat, l: Locale) -> Result<Instant, string>
```

The same, reading any name token under `l`.

```
pub fn parse_instant(s: string) -> Result<Instant, string>
```

Accepts either offset shape parse_offset does ("Z" or a real
"+HH:MM"/"-HH:MM"), not just the "Z" format always
produces -- a real external Instant/OffsetDateTime-shaped string
(from an actual API, not just round-tripping this module's own
output) may carry a genuine offset, and there's no extra cost to
accepting it.

### `format`

```
pub fn format(i: Instant) -> string
```

`i` in UTC as `2026-09-24T13:05:09Z`, with a `.nnnnnnnnn` fraction when the nanoseconds are not
zero.

### `local_date_to_json`

```
pub fn local_date_to_json(d: LocalDate) -> Json
```

Thin wrappers over each type's own _to_string/_from_string pair,
named to match #[derive(Json)]'s own <type>_to_json/<type>_from_json
naming convention (src/derive.c's pascal_to_snake -- "local_date",
not this file's own older "localdate" shortcut) -- so a struct field
of one of these types resolves here automatically. See
docs/DESIGN.md's "Attributes and derive" entry.

### `local_date_from_json`

```
pub fn local_date_from_json(j: Json) -> Result<LocalDate, string>
```

A `LocalDate` from a JSON string in its ISO form; an Err for any other JSON value or a malformed string.

### `local_time_to_json`

```
pub fn local_time_to_json(t: LocalTime) -> Json
```

`LocalTime` as a JSON string in its ISO form.

### `local_time_from_json`

```
pub fn local_time_from_json(j: Json) -> Result<LocalTime, string>
```

A `LocalTime` from a JSON string in its ISO form; an Err for any other JSON value or a malformed string.

### `duration_to_json`

```
pub fn duration_to_json(d: Duration) -> Json
```

`Duration` as a JSON string in its ISO form.

### `duration_from_json`

```
pub fn duration_from_json(j: Json) -> Result<Duration, string>
```

A `Duration` from a JSON string in its ISO form; an Err for any other JSON value or a malformed string.

### `local_date_time_to_json`

```
pub fn local_date_time_to_json(dt: LocalDateTime) -> Json
```

`LocalDateTime` as a JSON string in its ISO form.

### `local_date_time_from_json`

```
pub fn local_date_time_from_json(j: Json) -> Result<LocalDateTime, string>
```

A `LocalDateTime` from a JSON string in its ISO form; an Err for any other JSON value or a malformed string.

### `zone_offset_to_json`

```
pub fn zone_offset_to_json(z: ZoneOffset) -> Json
```

`ZoneOffset` as a JSON string in its ISO form.

### `zone_offset_from_json`

```
pub fn zone_offset_from_json(j: Json) -> Result<ZoneOffset, string>
```

A `ZoneOffset` from a JSON string in its ISO form; an Err for any other JSON value or a malformed string.

### `zone_id_to_json`

```
pub fn zone_id_to_json(z: ZoneId) -> Json
```

`ZoneId` as a JSON string in its ISO form.

### `zone_id_from_json`

```
pub fn zone_id_from_json(j: Json) -> Result<ZoneId, string>
```

A `ZoneId` from a JSON string in its ISO form; an Err for any other JSON value or a malformed string.

### `instant_to_json`

```
pub fn instant_to_json(i: Instant) -> Json
```

`Instant` as a JSON string in its ISO form.

### `instant_from_json`

```
pub fn instant_from_json(j: Json) -> Result<Instant, string>
```

A `Instant` from a JSON string in its ISO form; an Err for any other JSON value or a malformed string.

### `local_date_to_sql`

```
pub fn local_date_to_sql(v: LocalDate) -> SqlValue
```

`LocalDate` as a SQL text value in its ISO form.

### `local_date_from_sql`

```
pub fn local_date_from_sql(v: SqlValue) -> Result<LocalDate, DbError>
```

A `LocalDate` from a SQL text column in its ISO form; a DbError for NULL, a non-text value or a malformed string.

### `local_time_to_sql`

```
pub fn local_time_to_sql(v: LocalTime) -> SqlValue
```

`LocalTime` as a SQL text value in its ISO form.

### `local_time_from_sql`

```
pub fn local_time_from_sql(v: SqlValue) -> Result<LocalTime, DbError>
```

A `LocalTime` from a SQL text column in its ISO form; a DbError for NULL, a non-text value or a malformed string.

### `duration_to_sql`

```
pub fn duration_to_sql(v: Duration) -> SqlValue
```

`Duration` as a SQL text value in its ISO form.

### `duration_from_sql`

```
pub fn duration_from_sql(v: SqlValue) -> Result<Duration, DbError>
```

A `Duration` from a SQL text column in its ISO form; a DbError for NULL, a non-text value or a malformed string.

### `local_date_time_to_sql`

```
pub fn local_date_time_to_sql(v: LocalDateTime) -> SqlValue
```

`LocalDateTime` as a SQL text value in its ISO form.

### `local_date_time_from_sql`

```
pub fn local_date_time_from_sql(v: SqlValue) -> Result<LocalDateTime, DbError>
```

A `LocalDateTime` from a SQL text column in its ISO form; a DbError for NULL, a non-text value or a malformed string.

### `zone_offset_to_sql`

```
pub fn zone_offset_to_sql(v: ZoneOffset) -> SqlValue
```

`ZoneOffset` as a SQL text value in its ISO form.

### `zone_offset_from_sql`

```
pub fn zone_offset_from_sql(v: SqlValue) -> Result<ZoneOffset, DbError>
```

A `ZoneOffset` from a SQL text column in its ISO form; a DbError for NULL, a non-text value or a malformed string.

### `zone_id_to_sql`

```
pub fn zone_id_to_sql(v: ZoneId) -> SqlValue
```

`ZoneId` as a SQL text value in its ISO form.

### `zone_id_from_sql`

```
pub fn zone_id_from_sql(v: SqlValue) -> Result<ZoneId, DbError>
```

A `ZoneId` from a SQL text column in its ISO form; a DbError for NULL, a non-text value or a malformed string.

### `instant_to_sql`

```
pub fn instant_to_sql(v: Instant) -> SqlValue
```

`Instant` as a SQL text value in its ISO form, always with the nine-digit fraction (`2026-09-24T13:05:09.000000000Z`), which `format` leaves off for a
whole second. A column of text is sorted as text, and `Z` sorts after `.`, so with the short form a whole second would sort after a fraction of the same
second (`...:09Z` after `...:09.5Z`); every value the same width sorts in time order. `instant_from_sql` (and `parse_instant`) read either form, so a
column that already holds short values still reads back; `ORDER BY` on one that mixes the two is only right once the old values are rewritten.

### `instant_from_sql`

```
pub fn instant_from_sql(v: SqlValue) -> Result<Instant, DbError>
```

A `Instant` from a SQL text column in its ISO form; a DbError for NULL, a non-text value or a malformed string.
