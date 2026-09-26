# `zone`

The IANA time zone database, read from the operating system's compiled zone files
(`/usr/share/zoneinfo/America/New_York` and so on, in the TZif format), for `time`'s named
zones. Most programs use it through `time.zoneid_of`; this module is the reader.

A zone's rules are a list of past transitions (each: from this UTC second, use that offset, DST
flag and abbreviation) plus, for the years after the last listed transition, the POSIX rule
string at the end of the file (`EST5EDT,M3.2.0,M11.1.0`: the second Sunday of March, the first
of November). The database directory is `$TZDIR` when that is set, else the first of
`/usr/share/zoneinfo`, `/usr/lib/zoneinfo` and `/usr/share/lib/zoneinfo` that exists. A system
with no zone files (a bare container, Windows) gets an Err from `load_rules`, never a wrong
answer; the zone `UTC` needs no files.

Leap-second tables in the files are skipped: times here are Unix time, which has none.

`use zone` -- or `use zone.Name` to bring one item in bare.

### `ZoneType`

```
pub struct ZoneType {
    utoff: int
    dst: bool
    name: string
}
```

One kind of local time in a zone: its offset from UTC in seconds (east positive), whether it is
daylight saving time, and its abbreviation (`EST`, `EDT`, `+03`).

### `RuleDate`

```
pub struct RuleDate {
    kind: int
    month: int
    week: int
    weekday: int
    day: int
    seconds: int
}
```

A day in the year on which a rule fires, as the POSIX string writes it. `kind` is 0 for
month/week/weekday (`M3.2.0`), 1 for a day of the year ignoring leap days (`J60`), 2 for a
zero-based day counting them (`59`); `seconds` is the local time of day (2 hours by default).

### `PosixRule`

```
pub struct PosixRule {
    standard: ZoneType
    daylight: ZoneType
    has_dst: bool
    start: RuleDate
    end: RuleDate
}
```

The daylight-saving rule of the years after a zone's last listed transition. A zone with no
daylight saving has `has_dst` false and only the standard type.

### `ZoneRules`

```
pub struct ZoneRules {
    transitions: [int]
    type_of: [int]
    utoffs: [int]
    dsts: [bool]
    names: [string]
    has_rule: bool
    rule: PosixRule
}
```

A zone's rules: `transitions[i]` is a UTC second from which local time type `type_of[i]` applies,
and type `k` has offset `utoffs[k]`, daylight flag `dsts[k]` and abbreviation `names[k]`.
Before the first transition type 0 applies; after the last, `rule` when `has_rule`. (Parallel
arrays of scalars and a plain rule field, rather than a list of ZoneType and an Option, so that
two zones compare with `==`.)

## The POSIX rule string

### `parse_posix_rule`

```
pub fn parse_posix_rule(text: string) -> Result<PosixRule, string>
```

The rule string at the end of a zone file: `std offset [dst [offset] [,start[/time],end[/time]]]`.
POSIX writes the offset as time WEST of UTC, so `EST5` is UTC-5.

## Reading a zone file

### `no_rule`

```
pub val no_rule = PosixRule
```

The placeholder rule of a zone with none (`has_rule` is false).

### `parse_rules`

```
pub fn parse_rules(data: [byte]) -> Result<ZoneRules, string>
```

The rules in the bytes of a zone file, or an Err when they are not a TZif file.

### `load_rules`

```
pub fn load_rules(name: string) -> Result<ZoneRules, string>
```

The rules of the zone called `name` (`Europe/Paris`), read from the system's zone files. An Err
says whether the name is unknown or the database is missing.

## Looking a time up

### `type_at`

```
pub fn type_at(rules: ZoneRules, t: int) -> ZoneType
```

The local time type in force at the UTC second `t` (seconds since 1970-01-01T00:00:00Z).
