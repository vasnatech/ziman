# `batch`

A job repository: a record of what ran, how it went and where it got to, so work that
outlives the request that started it can be polled, resumed and told apart from work that died.

**This is a quarter of Spring Batch, and deliberately the quarter nobody talks about.** Of its
four parts -- a job and launcher, an item reader, a chunk writer, and a `JobRepository` -- this
language already had the first three: `schedule` and `spawn` launch, `Iter` reads (`xml.events`,
`compress.zip_entries`, `fs.chunks`, `datasource.rows` all answer one), and a chunk writer is an
ordinary function over `datasource.with_transaction`. What was missing was the bookkeeping, which
is what a status endpoint reads and what a restart needs.

**There is no chunk pipeline here, and that is measured rather than a simplification.**
The case for one is that a long import must commit every N items to bound memory -- Spring
Batch's `commit-interval`. On 20,000 rows into a real schema through a real repository, on
SQLite: autocommit 5,334 ms and 67 MB, one transaction per 1,000 rows 5,065 ms and 68 MB, one
transaction for all 20,000 **4,990 ms and 68 MB**. Transaction size changes neither the clock nor
the memory, and the single large transaction is marginally the fastest. A job here writes however
it likes. See docs/design/batch.md §2, which also records the import whose per-record cost was
mistaken for this one.

What a caller writes:


```
val job = batch.job("icd10", fn(resume: string, note: batch.Note) -> Result<int, string> {
    var written = 0
    for item in try open_catalogue(resume) {         // opened here: see `Job.run`
        val code = try item
        written = written + try store_one(conn, code)
        try note(conn, written, code.code)           // the marker, in the same transaction
    }
    return written
})
val run = try batch.launch(pool, "medical_code_import", job, "")
// ... later, from a status endpoint
val now = try batch.status(conn, "medical_code_import", run.id)
```

**The table is this module's, the migration is yours.** `batch.schema(table)` answers the DDL for
a migration of the application's own, because `lib/` does not own anybody's schema -- but the
columns are fixed, so every application that adds that migration gets the same job repository
rather than inventing one.

`use batch` -- or `use batch.Name` to bring one item in bare.

### `status_running`

```
pub val status_running = "RUNNING"
```

A run's states. `RUNNING` is the only one a worker leaves behind when its process dies, which
is what `stale` is for.

### `status_succeeded`

```
pub val status_succeeded = "SUCCEEDED"
```

### `status_failed`

```
pub val status_failed = "FAILED"
```

### `Run`

```
pub struct Run {
    id: Uuid
    job: string
    status: string
    started_at: Instant
    finished_at: Option<Instant>
    // How many items the job reported writing. Its own count, in its own units.
    items: int
    resume: string
    // Why it failed, when it did. Never set on a run that succeeded.
    message: Option<string>
}
```

One run of a named job: what a status endpoint answers, and what a resumption reads.

`resume` is **the job's own opaque position** and this module never interprets it: a page number,
a URL, a byte offset and a code are all the same string here. That is Singer's `STATE` message
reduced to its one load-bearing property -- the runner persists what the reader says and hands it
back next time, and knows nothing about it.

### `Note`

```
pub struct Note {
    write: fn(datasource.Connection, int, string) -> Result<unit, string>
}
```

What a job calls when it reaches a point it could resume from: how many items it has written so
far, and the token to start from next time.

**It takes the connection on purpose, and it is the one rule this module enforces.** The marker
has to be written in whatever transaction the job is already in, or a crash between the work and
the marker leaves a run that cannot say what happened -- the work committed and the marker lost,
or the marker committed and the work rolled back. Passing `tx` inside
`datasource.with_transaction` makes the two atomic; passing the plain connection outside one is
correct too, for a job that does not use a transaction at all. What is never correct is a marker
written on a *different* connection from the work, which is why this is not a closure over one.
(Flink calls this a checkpoint; mainframe batch called it a checkpoint record forty years
earlier.)

### `Job`

```
pub struct Job {
    name: string
    run: fn(string, Note) -> Result<int, string>
    done: Option<fn() -> Result<bool, string>>
}
```

A job: a name, the work, and optionally a question about whether there is any.

`run` is given the previous run's `resume` token (`""` the first time) and answers how many items
it wrote. **It opens whatever it reads**, which is not a style preference: an `Iter` belongs to
the thread that made it and cannot cross a `spawn`, so a job that runs in the background cannot
be handed a reader. Opening it inside `run` is also exactly what resuming means, which is the
happy half of that constraint.

`done` is Luigi's idea: a task is complete if its *output* is, so the question "is there anything
to do" is asked of the world rather than of this table. A `Some` answering true means `launch`
records a `SUCCEEDED` run of zero items without starting a worker. It is better than deciding
from a previous run's status, because a record of progress can disagree with the progress and
observable state cannot.

### `job`

```
pub fn job(name: string, run: fn(string, Note) -> Result<int, string>) -> Job
```

A job with no completion check, which is most of them.

### `job_unless`

```
pub fn job_unless(
    name: string,
    run: fn(string, Note) -> Result<int, string>,
    done: fn() -> Result<bool, string>
) -> Job
```

A job that first asks whether there is anything to do.

### `schema`

```
pub fn schema(table: string) -> string
```

The table every function here reads and writes, for a migration of the application's own.

`{{uuid}}` is `migrate`'s own token, so the same string works on SQLite, Postgres and MySQL. The
two instants are `TEXT` rather than a token because there is no timestamp token to use: an
`Instant` crosses `datasource` as the ISO text `time.to_sql` writes, on every engine, which is
what `time.to_instant` reads back. (The first version of this answered `{{timestamp}}` and the
migration failed to load -- "unknown token" -- which is the right error and found it at once.)

**The columns are fixed deliberately.** An application that adds this migration gets the same job
repository as every other one, which is the whole point of the table living in a library rather
than in each application's head.

## Reading the record

### `status`

```
pub fn status(conn: datasource.Connection, table: string, id: Uuid) -> Result<Option<Run>, string>
```

One run by id, or None when no such run was recorded. What a status endpoint answers.

### `recent`

```
pub fn recent(
    conn: datasource.Connection,
    table: string,
    job_name: string,
    limit: int
) -> Result<[Run], string>
```

The most recent runs of one job, newest first: what an administrator's "has this ever worked"
question actually asks.

### `stale`

```
pub fn stale(
    conn: datasource.Connection,
    table: string,
    older_than_seconds: int
) -> Result<[Run], string>
```

A run left `RUNNING` by a process that is no longer running, which nothing will ever finish.

**This is a guess, and the shape of the guess is the honest part.** A worker that dies takes its
row's future with it, so something has to decide that a run which started long enough ago and
never finished is not coming back. `older_than_seconds` is that decision, and it belongs to the
caller because only the caller knows how long its longest job legitimately takes. A heartbeat
column would be a fact rather than a guess; it is not here because it is more machinery than one
caller has yet justified (docs/design/batch.md §9.3).

## Running one

### `run_now`

```
pub fn run_now(
    conn: datasource.Connection,
    table: string,
    j: Job,
    resume: string
) -> Result<Run, string>
```

Runs `j` to completion **on this thread**, recording the run as it goes, and answers the finished
record. A failing job is a `SUCCEEDED`/`FAILED` row and an `Ok` here: the job failed, this
function did not. An `Err` means the *bookkeeping* failed, which is the one thing a caller cannot
be told by a row.

**It exists so that a batch job can be tested without a thread.** A library whose first version
could only run work in the background would be a library nothing could assert about -- and it is
also what `schedule` wants, a scheduled job being
`schedule.job(name, daily, fn() { batch.run_now(..) })`.

### `launch`

```
pub fn launch(
    pool: datasource.Connection,
    table: string,
    j: Job,
    resume: string
) -> Result<Run, string>
```

Starts `j` on a thread of its own and answers the run as it stands at that moment -- `RUNNING`,
or `SUCCEEDED` with nothing done when `Job.done` said there was nothing to do. An endpoint
returns that record and the caller polls `status`.

**The worker takes a connection of its own and gives it back.** `pool` is the application's
pooled handle; the request that called this returns its own connection the moment it answers, so
a job still running on it would be writing through a connection somebody else has. `acquire`
inside the worker is the whole fix, and the `defer` beside it is what makes the pool survive a
job that fails.

**A failing job is not a failing `launch`.** Once the row exists, every outcome is recorded in
it; the `Err` here is for the bookkeeping failing before the worker starts, which is the only
failure a caller can still be told about directly.
