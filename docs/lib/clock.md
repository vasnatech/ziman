# `clock`

The system clock and sleeping, the two things Ziman cannot do in pure code.
Both are native. `time.zm` builds `Instant` on top of `now_raw`, the same split as
`random`/`uuid`: a plain native function with one fixed signature here, the wrapping
in Ziman there.

`use clock` -- or `use clock.Name` to bring one item in bare.

## Example

```
use clock

fn main() {
    val before = clock.now_raw()
    clock.sleep_millis(20)
    val after = clock.now_raw()
    print(after[0] > 1700000000)
    print(after[0] * 1000 + after[1] / 1000000 - (before[0] * 1000 + before[1] / 1000000) >= 20)
    print(len(clock.now_iso()))
    print(len(clock.now_http_date()))
}
```

Output:

```
true
true
24
29
```

### `now_raw`

```
pub native fn now_raw() -> [int]
```

The current wall-clock time as `[seconds, nanos]`: seconds since the Unix epoch and the
nanoseconds within that second (0 to 999999999). Both are read by a single
`clock_gettime(CLOCK_REALTIME)` call, so the clock cannot tick over between them. It
returns a two-element array because a native function cannot build a struct.
This is the wall clock: it can jump when the system time is adjusted, so do not use it
to measure elapsed time.

### `now_iso`

```
pub native fn now_iso() -> string
```

The current UTC time as ISO-8601 text with milliseconds: "2026-09-24T12:00:00.123Z". The
fast way to stamp a log line; use `time.instant_now()` when you need to compute with the time.

### `now_http_date`

```
pub native fn now_http_date() -> string
```

The current time as an HTTP date (RFC 7231), the form of a `Date` header:
"Thu, 24 Sep 2026 12:00:00 GMT".

### `monotonic_millis`

```
pub native fn monotonic_millis() -> int
```

Milliseconds since an arbitrary fixed start, and only ever forward: the clock to measure how long
something took (`val start = clock.monotonic_millis()` ... `clock.monotonic_millis() - start`). The
number means nothing by itself, and unlike `now_raw` it does not jump when the system time changes.

### `sleep_millis`

```
pub native fn sleep_millis(ms: int)
```

Blocks the calling thread, and only that thread, for at least `ms` milliseconds; other
threads keep running. A signal cannot cut the sleep short. A `ms` of zero or less
returns at once.
