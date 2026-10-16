# `schedule`

Periodic background work inside a long-running process.

The gap this fills: `server.serve` owns the accept loop and never returns, so a program that
also has to do something *on a clock* -- a nightly scan, a cache refresh, an expiry sweep -- had
nowhere to put it. The pieces were all here (`spawn`, `Channel`, `clock.sleep_millis`), and the
loop that combines them is the part everyone would write slightly differently and one of them
would get wrong. So it lives here once.

The shape is one scheduler thread holding every job's own next-due time, spawning each due job
as it falls due. `spawn` it beside the server and the two run independently:


```
val stop: Channel<unit> = channel_new()
spawn schedule.run_until(jobs, stop, fn(name: string, message: string) {
    log.error(logger, concat("job ", name, " failed: ", message))
})
server.serve(listener, handler, options)
```

**Why a due job is `spawn`ed rather than called.** A runtime error ends the thread it happens in
(docs/DESIGN.md), so a job that aborts -- an `unwrap` on a `None`, a database that went away --
would take the scheduler with it and silently stop every *other* job for the life of the
process. Spawned, it ends only itself and arrives at `on_error` instead. The cost of that choice,
stated plainly: nothing waits for a job to finish, so a job that takes longer than its own
interval will overlap with its next run. `EveryMillis` therefore means "start one every N
milliseconds", not "leave N milliseconds between them" -- keep a job shorter than its interval,
or have it hold its own lock if overlapping would be wrong.

**Three forms, and when to use which.** `every_millis` for "every so often", `daily_at_utc` for
the common daily case, and `cron` for a pattern neither can express -- a weekday-only sweep, the
first of the month -- or one carried over from another system. The overlap between the last two
is real and deliberate: `daily_at_utc(8, 0)` and `cron("0 0 8 * * *")` are the same schedule, but
the first is checked by the compiler for arity and type and by a `Result` for range, while the
second is a string that parses or does not. Prefer the constructor when it fits.

**UTC only, deliberately.** `daily_at_utc` is arithmetic on the epoch second (`% 86400`), which
is why this module needs no calendar and does not import `time`. A local-time daily schedule
would need `time`/`zone` *and* an answer for the two days a year when 02:30 happens twice or not
at all -- a real design question, not an oversight, and one worth settling only when something
asks for it. Every `@Scheduled` job in the reference backend is already `zone = "UTC"`.

`use schedule` -- or `use schedule.Name` to bring one item in bare.

### `Schedule`

```
pub enum Schedule {
    // Start a run every N milliseconds, the first one immediately.
    EveryMillis(int)
    // Run once a day, at this many seconds past 00:00:00 UTC.
    DailyAtUtc(int)
    // Run whenever the UTC wall clock matches a cron pattern (`cron`).
    Cron(CronSpec)
}
```

How often a job runs. Two forms, because these are the two questions that have come up: "every
so often" and "once a day at a fixed time".

### `CronSpec`

```
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
```

A parsed cron expression, as one membership table per field: `seconds[s]` is true when second `s`
matches. Tables rather than a re-parse per tick, because the loop asks "does now match" several
times a second and the answer is then six array reads.

`dom_restricted`/`dow_restricted` record whether the day-of-month and day-of-week fields were
anything other than `*`, which `cron` uses to refuse an expression that restricts both -- see
there for why.

### `every_millis`

```
pub fn every_millis(ms: int) -> Result<Schedule, string>
```

`every_millis(60000)` -- every minute. Refuses a non-positive interval, which would otherwise be
a spin loop that looks like a scheduler.

### `daily_at_utc`

```
pub fn daily_at_utc(hour: int, minute: int) -> Result<Schedule, string>
```

`daily_at_utc(8, 0)` -- 08:00 UTC every day.

### `cron`

```
pub fn cron(expr: string) -> Result<Schedule, string>
```

Parses a cron expression into a `Schedule`.

**Six fields, not five: `second minute hour day-of-month month day-of-week`.** This is the form
Spring and Quartz use, and it is the form every expression in the reference backend is written
in. A five-field expression -- the Unix `crontab` form, which has no seconds -- is **refused by
field count rather than guessed at**, because the two dialects collide silently: `0 8 * * *` is
"08:00 daily" to crontab and "every hour at 8 minutes past" here, which is a schedule that runs
twenty-four times too often and never errors. Refusing is the same answer `..=` gets from the
lexer, and for the same reason: a message beats a plausible misreading.

Each field takes `*`, a number, `a-b`, `*/n`, `a-b/n`, and a comma-separated list of any of
those. `?` is accepted as a synonym for `*`. Day-of-week is 0-6 with 0 as Sunday, and 7 is also
Sunday so a copied expression keeps working; the names `SUN`-`SAT` and `JAN`-`DEC` are accepted
case-insensitively.

**What is refused, with a message rather than silently:** `L` (last), `W` (nearest weekday) and
`#` (nth weekday) are not implemented; and an expression that restricts *both* day-of-month and
day-of-week is refused, because the two established answers disagree -- Vixie cron fires when
**either** matches, Quartz requires a `?` in one and forbids the pair -- so an expression meaning
one thing elsewhere would quietly mean the other here. Write `*` or `?` in whichever one does not
apply.

### `Job`

```
pub struct Job {
    name: string
    schedule: Schedule
    run: fn() -> Result<unit, string>
}
```

One scheduled job. `name` is what `on_error` is told, so make it the name you would want to read
in a log line at four in the morning.

### `job`

```
pub fn job(name: string, schedule: Schedule, run: fn() -> Result<unit, string>) -> Job
```

### `run_until`

```
pub fn run_until(jobs: [Job], stop: Channel<unit>, on_error: fn(string, string))
```

Runs `jobs` until `stop` delivers anything, then returns. Intended to be `spawn`ed; a server that
wants the scheduler for the whole life of the process passes a channel it never sends on, and
process exit tears the thread down.

`on_error(name, message)` is called for a job that returns an `Err` *and* for one that aborts, so
it is the one place a failure can be seen. It runs on the scheduler's own thread for an `Err` and
on the job's thread for an abort, so it must be safe to call from either -- which is why it takes
plain strings rather than anything thread-bound.

### `cron_matches_at`

```
pub fn cron_matches_at(spec: CronSpec, epoch_second: int) -> bool
```

Whether the UTC wall clock at `epoch_second` matches `spec` -- what the loop asks on every look,
and public because it is the only way to check a schedule without waiting for it. A pure function
of its two arguments, so a test can ask about next February from today.

Day-of-month and day-of-week are both consulted, but `cron` has already refused an expression
that restricts both, so at most one of them is doing any work here and the two cron dialects
cannot disagree about this.
