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
}
```

How often a job runs. Two forms, because these are the two questions that have come up: "every
so often" and "once a day at a fixed time".

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
