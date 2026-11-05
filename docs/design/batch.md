# A batch library, and the chunking it does not need

Status: **built 2026-11-04**, as `lib/core/batch.zm`, with the North Star's medical-code import as its
first caller. The measurements in §2 refuted the reason this was proposed -- chunked commits -- so
what shipped is a quarter of Spring Batch and a different quarter than expected. §11 records what
building it changed.

Asked for because the North Star backend cannot answer three of Spring's endpoints
(`POST /api/admin/medical-codes/import/{system}`, `GET .../import/status/{id}`, and the export
beside them, which is now built): Spring launches a Spring Batch job and hands back an execution id
to poll. This port has no concept of a job that outlives the request that started it. The question
was whether a Spring-Batch-shaped library belongs in `lib/`, and the answer is that a quarter of one
does.

## 1. What Spring Batch is, in four parts

| part | what it does | what Ziman has |
| --- | --- | --- |
| `Job`/`Step`/launcher | names a unit of work, runs it | `schedule.Job` + `run_until`, `spawn` with `on_error` |
| `ItemReader` | pulls items one at a time | `Iter<T>`, and `io`'s sources: `xml.events`, `compress.zip_entries`, `fs.chunks`, `datasource.rows` |
| `ItemProcessor`/`ItemWriter` | transforms, writes a chunk | an ordinary function and `datasource.with_transaction` |
| `JobRepository` | records runs, status, progress; makes restart possible | **nothing** |

Three of the four are already here, and the third is just "a function this language could already
write". **The missing part is the one that is actually missing**, and it is the one Spring Batch
users interact with most: a record of what ran, how it went, and where it got to.

## 2. The measurements, and the premise they killed

The proposal was that a batch library's value is chunked commits -- that a long import must commit
every N items so memory stays bounded, which is how Spring Batch is usually explained and what its
`commit-interval` is for. The North Star's Apple Health import writes every record in one
transaction and costs about 95 KB of resident memory per record with superlinear time, so chunking
looked like the fix.

**It is not.** Three measurements, each on 20,000 rows into the real schema through the real
repository (`eql` translation, entity mapping, the duplicate-check query), on SQLite:

| commit strategy | time | peak resident |
| --- | --- | --- |
| autocommit, one statement at a time | 5,334 ms | 67 MB |
| one transaction per 1,000 rows | 5,065 ms | 68 MB |
| one transaction for all 20,000 | **4,990 ms** | **68 MB** |

Transaction size changes nothing, and the single large transaction is marginally the fastest.
Memory is flat in all three. A separate probe without the repository -- raw `datasource.execute` --
agrees: 20,000 inserts in one transaction, 401 ms; in chunks of 1,000, 398 ms.

**So where is the import's 95 KB per record?** Bounded by elimination, which is the useful part of
having measured:

| layer | 20,000 records | what it costs |
| --- | --- | --- |
| parse only (unmapped type, nothing written) | 615 MB, 5.6 s | about 22 KB per element, linear |
| repository only (insert + duplicate check) | 68 MB, 5.0 s | flat, linear |
| the real endpoint | 2,054 MB, 54.6 s | about 95 KB per record, superlinear |

The parse and the writes together account for about 683 MB and 11 seconds of a 2,054 MB,
54.6-second import. **The remaining 1.4 GB and 44 seconds are in `service/apple_health.zm`'s own
per-record path** -- date parsing, the identifier table, building the entity -- and that is a defect
to chase in that file, not an architecture to fix with a library. A batch library would not have
helped it, and shipping one *as* the fix would have been the wrong answer confidently delivered.

**One confound worth recording**, because it nearly produced a wrong design. The same repository
probe against `memdb` instead of SQLite reported 40 KB per row and 70 ms per duplicate check --
5,000 rows took 350 seconds. `memdb` has no usable index for that lookup, so every check is a scan
of a growing table. Had that been the only engine measured, this document would have argued for
chunking on evidence that was really about the test engine.

## 3. What this language can express, probed rather than assumed

Four questions decide the API's shape. Each was answered with a throwaway program.

1. **A generic step holding function values works.** `struct Step<I, S>` with
   `read: fn(S) -> Result<Option<I>, string>`, `write: fn([I]) -> Result<int, string>` and
   `mark: fn(S, I) -> S`, handed to `fn run<I, S>(step: Step<I, S>, ..)`, runs. Two type parameters,
   function-typed fields, a generic runner: all fine.
2. **A lambda can capture a value, live in a struct field, and cross a `spawn`.** So a job can carry
   a writer that closed over its settings or its connection to a worker thread. (Captures are by
   value and a `var` cannot be captured, which is a limit this design does not need.)
3. **An `Iter` cannot cross a `spawn`**, and the compiler says so:
   *"it belongs to the thread that made it, as a builder or an iterator does"*. **This is the
   constraint that shapes everything.** A job that runs on its own thread cannot be *handed* a
   reader; it must be handed something that opens one -- `open: fn(S) -> Result<Iter<I>, string>` --
   and the `Iter` comes into being on the worker. Which turns out to be what resumption wants
   anyway: reopening from a saved position is the same operation as opening.
4. **A registry must be erased, and `schedule` already shows how.** `Job<I, S>` cannot sit in a list
   beside `Job<OtherItem, OtherState>`, so a launcher that takes a job *name* out of a request
   cannot hold generic jobs. `schedule.job(name, schedule, run: fn() -> Result<unit, string>)`
   solves this by erasing to a nullary function; a `Map<string, Job>` of
   `{ name, run: fn() -> Result<int, string> }` holding jobs over two different item types was
   probed and works. **Two layers: generic machinery inside a closure, erased jobs in the
   registry.**

And one more, measured end to end: a lambda inside a generic function capturing values of the
generic parameters' types, passed to `datasource.with_transaction`, writing rows and a marker in
one transaction -- 7 items in chunks of 3, 7 rows, marker at 7. The hard shape is available.

## 4. The design

**`lib/core/batch.zm`, and it is small.** What a caller gets:

```ziman
// A run of a named job: what a status endpoint reads, and what makes a restart possible.
pub struct Run {
    id: Uuid
    job: string
    status: string          // PENDING | RUNNING | SUCCEEDED | FAILED
    started_at: Instant
    finished_at: Option<Instant>
    items: int              // how many items the job reported writing
    resume: string          // the job's own opaque position, "" before it starts
    message: Option<string> // why it failed, when it did
}

// A job, erased so a registry can hold several: given its last resume token, do the work and
// report how far it got. Called on a worker thread, so it opens whatever it reads.
pub struct Job {
    name: string
    run: fn(string, fn(int, string) -> Result<unit, string>) -> Result<int, string>
}
```

The second argument is the progress callback: `report(items_so_far, resume_token)`. A job calls it
when it reaches a point it could resume from, and the library persists it. That is Singer's `STATE`
message as a function call, and it is the whole of the resumption story -- **the library never
interprets the token**, so a page number, a URL, a byte offset and a code are all the same to it.

Four functions:

* **`batch.launch(store, job, resume)`** -- records a `Run` as `RUNNING`, spawns the worker,
  answers the `Run`. The caller's endpoint returns that immediately.
* **`batch.status(store, id)`** -- reads the row. A status endpoint is this and a serializer.
* **`batch.run_now(store, job, resume)`** -- the same work on the calling thread, for a test or a
  CLI, because a background job nothing can run synchronously is a background job nothing can test.
* **`batch.sweep(store, older_than)`** -- marks a `RUNNING` row whose process died as `FAILED`. A
  crashed worker leaves a row that nothing will ever finish; this is how the lie gets corrected.

**`store` is an interface this language does not have**, so it is a struct of four functions --
`insert`, `update`, `get`, `list_stale` -- supplied by the application against its own table. The
library brings no schema: `lib/` does not own anyone's migrations, and the North Star's
`medical_code_import` table is its own business.

## 5. What is deliberately not in it

* **No chunk/processor/writer pipeline.** Section 2: chunked commits buy nothing measurable here.
  A job writes however it likes, including one transaction for everything.
* **No skip or retry policies.** Spring Batch's `skipLimit`/`retryLimit` exist because its steps are
  declarative and cannot catch their own errors. A Ziman job is a function; `try` and a `switch` are
  already the vocabulary, and a policy object would be a second way to write an `if`.
* **No partitioning, no remote steps, no DAG.** One process, one thread per run. Airflow, Dagster
  and Prefect solve a different problem -- orchestrating work across systems -- and nothing here
  has that problem.
* **No job *instance* identity.** Spring Batch decides whether two launches are "the same run" from
  the job parameters, which is why its own controller in this very port catches
  `JobInstanceAlreadyCompleteException` and defeats the rule with
  `.addLong("timestamp", currentTimeMillis())`. **Every `launch` here is a new run.** Whether a run
  *should* happen is the caller's question, and section 6 gives it a better tool than identity
  rules.
* **No scheduling.** `schedule` already does cron, and a scheduled batch job is
  `schedule.job(name, daily, fn() { batch.run_now(..) })` -- which is why `Job.run` answers a
  `Result` shaped like `schedule`'s.

## 6. Should this run at all: the idea taken from Luigi

Luigi defines a task as complete iff its *output* exists -- a `Target` -- and skips any task whose
target is already there. No run table is consulted, so nothing can disagree with reality. The same
idea fits here as an optional field rather than a mechanism:

```ziman
    // Whether there is anything to do, asked of the world rather than of the run table.
    done: Option<fn() -> Result<bool, string>>
```

For the medical-code import that is "this system already holds codes and the catalogue has not been
revised"; for an export, "the file is already there". It is better than Spring's job-instance
identity for the same job, because it is a question about observable state instead of about
bookkeeping -- and this project has been bitten twice this week by a *record* of progress
disagreeing with the progress itself, which is the failure mode a `Target` does not have.

## 7. The checkpoint rule, taken from Flink and from mainframe batch before it

**Never commit the work and the marker separately.** If a job writes rows and then reports
progress, a crash between the two leaves a run that cannot say what happened; if it reports first,
a crash loses work silently. So the contract is one sentence, and it belongs in the library's own
documentation rather than in each caller's head:

> `report` writes the marker in the transaction the job is already in, or the job must not use a
> transaction at all.

Which is enforceable here precisely because the store is the application's: its `update` runs on the
connection the job hands it, so a job that is inside `with_transaction` reports inside it too.

## 8. Prior art, and what each one actually contributed

| system | the idea taken | the idea left |
| --- | --- | --- |
| Spring Batch | a job repository: runs, status, restart | chunk pipeline, skip/retry policies, job-instance identity |
| JSR 352 | the chunk/batchlet distinction -- not every job is item-oriented | XML job definitions |
| Luigi | completion as observable state (§6) | the DAG and the scheduler |
| Singer / Embulk | the reader emits its own resume token; the runner persists it blindly (§4) | plugin discovery, a state *file* |
| Flink / mainframe checkpointing | work and marker in one commit (§7) | distributed snapshots, exactly-once |
| GoodJob / Hangfire | the table is the queue and the status page is a query over it | polling workers, a dashboard |
| dbt | full-refresh versus incremental as an explicit property of a load | models, templating |
| Airflow / Dagster / Prefect | nothing -- different problem | all of it |

## 9. Open questions

1. **Is one caller enough to build it?** This project's bar is two. The medical-code import wants it
   today. The Apple Health import would want it *after* its own per-record defect (§2) is fixed,
   since a 90,000-record import is a background job whatever its per-record cost. The FHIR bulk
   import is a third candidate. **Recommendation: build it with the medical-code import, and make
   the second adoption a condition of keeping it.**
2. **Does a worker get its own connection, and who releases it?** `datasource.acquire(conn)` gives a
   dedicated one and `release` gives it back; a job that outlives its request must not use the
   request's. The sketch above hides this inside `launch`, which means `launch` needs the pool
   handle -- fine, but it makes `batch` depend on `datasource`, which pushes it out of layer 0.
   Alternative: the job acquires and releases itself, and `batch` stays pure. **Leaning toward the
   second**, since `Job.run` is already a function the application wrote.
3. **What does `status` answer for a run the process never finished?** §4's `sweep` marks it
   `FAILED` after a timeout, which is a guess dressed as a fact. The alternative is a heartbeat
   column the worker touches, which is more honest and more machinery.
4. **Should `report` be allowed to say "and stop"?** A job that discovers the catalogue is unchanged
   mid-run has nothing to do. Returning early is already available, so probably not.

## 10. Staging

1. The `Run`/`Job`/store shapes and `run_now`, with a fixture. No threads: a batch library whose
   first version cannot be run synchronously cannot be tested.
2. `launch` and `status`, over the North Star's own `medical_code_import` table.
3. The three medical-code fetchers as one job each, which closes the two remaining Spring endpoints.
4. `sweep`, once question 3 is answered.
5. Reconsider the whole thing if no second caller has adopted it by then.

**And the thing this document exists to prevent**: building the chunk pipeline because Spring Batch
has one. Section 2 is four measurements saying it would be ceremony. The part worth having is the
part Spring users never mention, which is the part that remembers what happened.

## 11. What building it changed

Shipped as `lib/db/batch.zm` -- not `lib/core/` -- because it owns a table, as `migrate` owns
migrations, and `gen_lib_manual.py`'s groups made that call before I did.

**Moved to `lib/core/batch.zm` on 2026-11-04, on the user's call, and the original reasoning above
is the thing that was wrong.** Owning a table is where this module *stores* something, not what it
is about: `lib/db` is for talking to a database, and a reader meets `batch` by having work that has
to survive a crash rather than by reaching for a connection. It keeps its own group in the manual
("Jobs") rather than a line under Databases, because what it composes with is `schedule` -- that
decides *when* something runs, this records *what happened*, and neither contains the other. The
module's `use` name was always flat, so nothing that imports it changed.

Five things the design got wrong or left out.

1. **`Note` had to take the connection, and that is now the one rule the module enforces.** §7 said
   the marker must be written in the job's own transaction; §4's sketch had the library build the
   `report` closure, which would have closed over a *different* connection and broken exactly the
   rule §7 states. `Note.write(conn, items, resume)` is how a job passes its `tx` in.
2. **`finish_run` must not touch `resume`.** The first version passed the launch's input token
   through on completion, which erased the marker of every successful run -- so a "resumption" was
   a restart. The fixture caught it by reading the row back, which is also why it reads the row back.
3. **`schema` cannot use a timestamp token, because there is no such token.** `migrate`'s only type
   token is `{{uuid}}`; an `Instant` crosses `datasource` as ISO text on every engine. The first
   version answered `{{timestamp}}` and the migration refused to load, naming the unknown token --
   the right error, immediately.
4. **`Option<fn() -> Result<bool, string>>` is expressible after all**, so `Job.done` is the field
   §6 wanted. The roadmap's note about function types as type arguments is narrower than it reads:
   it is about `Channel`, and an `Option` of a function works in a field and across a `spawn`.
5. **The store-as-four-closures in §4 is gone.** The library owns the table's columns and answers
   its DDL for a migration of the application's own, which is fewer moving parts for a caller and
   makes every application that adds the migration get the same job repository. `lib/` still does
   not run anybody's migrations.

**The first caller, and what it proves.** `service/medical_code_import.zm` fetches ICD-10-CM from
the NLM's clinical-tables API -- paginated, no credential -- and writes each page with the offset as
its marker, in one transaction per page. A refused offset is retried from an earlier one with the
duplicates dropped, which is a real quirk of that service the Spring client documents. 19 checks in
`medical_code_import_test.py` against a stub catalogue in the test file, including that a failed run
resumes rather than re-fetching and that the migration is still the DDL the library answers.

**ICD-10 and ICD-11 are refused by name**, their readers being a ClaML XML in a zip and a zip of
linearisation files. The endpoint exists and works; what it cannot fetch it says it cannot fetch,
which is the §9.1 bar met once rather than twice -- the second adoption is still owed.

**And it is not `service/apple_health.zm`, which this document named as the candidate for weeks.**
Ruled out 2026-11-04, on a mechanical test rather than a judgement: *can the job obtain its input
again?* Everything this library is for depends on yes. The medical-code import resumes because its
source is a URL it can re-fetch. An Apple Health import's source is an **upload**, and the server
deletes a spilled body at the end of the request -- `lib/http/server/serve.zm` says so where it does
it, and draws the same conclusion for its own reasons: *"which is also why a handler must not keep
the source."* So a `batch` run there would write a resume marker that nothing could ever use, and
would break the endpoint's contract besides, since its response carries `imported: N` today.

**The per-record cost, which this section used to make the blocker, was never the obstacle.** It
turned out to be the collector (ziman F267), and it is fixed -- the import is flat in the record
count now. The obstacle was the input, and nobody had checked it.

Two candidates survive that test, and both are worth stating so the next reader starts from the
test rather than from a name. **`service/integration_sync.zm`** fits properly: a provider's API can
be asked again, so a marker naming the window or page reached is meaningful. Against it: it already
records status on the `user_integration` row, so `batch` would partly duplicate that, and the change
touches seven providers. **`service/testdata.zm`** is re-derivable from its preset and far smaller,
but it is scaffolding, so it would exercise less of the library than its first caller already does.

**The honest third option is to wait.** This bar exists to test the design against a real second
use; choosing a caller *because* the bar exists inverts it, and a library shaped by that pressure is
the thing the bar was written to prevent. Better one real caller and an open §9.1 than two callers
and a design that was never challenged.
