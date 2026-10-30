# Design journal, part 9 (entries dated 2026-10-26 to 2026-10-29)

Part of the [design journal](README.md) -- see its "How to read this journal" for what a dated entry means.

## `eql` adopted in one repository, and the collection parameter that had to come first (2026-10-26)

`repository/health_event.zm` in the sample backend now reads through `eql` -- seven queries, each translated once in a module-level `val` -- and writes in SQL. That is increment 4 of the plan, and it is deliberately **one** repository of eighteen: the entity query language earns the other seventeen by surviving this one, which is the one that motivated it.

**Adoption could not start until `eql` had collection parameters.** `find_page` and `count` are the functions the project was for, and both filter by a *list* of types. What they did before is the thing worth remembering, because it is what a hand-numbered query looks like when it grows:

```ziman
// repository/paging.zm, still there for the seventeen that have not moved
val marks = mutable_array_new<string>()
for i in 0..len(wanted) {
    push(marks, format("${}", i + 2))           // + 2: $1 is the patient
}
return format(" AND type IN ({})", join(separator: ", ", pieces: snapshot(marks)))
```

and the values were assembled by a *second* function -- `params_of`, whose comment is literally *"the patient's id, then the types, in the order `type_condition` numbers them"*: an ordering invariant held by a sentence. A shift of one there reads the wrong column and answers rows; nothing fails. So the increment began in `eql` rather than in the backend:

* **`eql.bind_list(name, values)`** is a binding of its own kind, not an array in the same slot as a scalar. The two cannot be told apart by looking: `:ids` bound to one value and `:ids` bound to a list of one value are different queries, and only the caller knows which it meant.
* **`Translated` keeps the query and the registry**, because a collection's SQL depends on how many values are bound -- `IN :types` is three placeholders for three values and one for one. `lists` names the collection parameters the query has, and `prepare` uses it to decide: no collections, send `sql` as it stands; a collection, translate again now that the count is known. A static query still translates once, which is the measurement that shaped this layer in the first place.
* **An empty list is refused.** `x IN ()` is not SQL, and every spelling that *is* has the wrong answer in one direction: `IN (NULL)` is unknown rather than false, `NOT IN (NULL)` is unknown rather than true. A caller with an empty filter knows what it means by one -- here, "match nothing", which it answers without a round trip -- so the decision is made at the call site. The old hand-built form sent ` AND 1 = 0` to the database to learn what the caller already knew.
* **A collection bound with `bind` is refused by name**: *the query names the collection :types after IN, so it needs eql.bind_list("types", ..) rather than eql.bind*. This is the mistake the previous point makes available, so the message names the function to call.

**The fixture made the mistake the API is designed around, which is the best evidence for it.** `tests/lang/eql_against_sqlite.zm` takes its bindings from strings and read "has a comma in it" as "is a collection" -- so `names=Ada` bound one value and `NOT IN :names` was refused, correctly, by the rule above. The shorthand is now an explicit `*names=Ada`, because a one-element collection is exactly what cannot be inferred from a value. The cross-check is at **22 of 22 agreeing** across memdb and real SQLite, five of them collection cases, and it prints the *prepared* SQL rather than the translated SQL, so the expansion a reviewer needs to see is on the page.

What adoption bought, stated as the three ways it was possible to be wrong here:

1. **A field name is checked when the query is translated**, and translation happens when the module loads -- so `e.efective_start` fails at startup, not on the request that first reads that column.
2. **Nothing is numbered by hand.** `LIMIT :limit OFFSET :offset` is bound like any other parameter; the type filter is one `bind_list`.
3. **`SELECT e` is the entity's own column list**, aliased to the bare names `health_event_from_row` reads by, rather than a `SELECT *` whose column order belongs to the engine.

Verified by the application suite rather than by unit tests, since this is a behaviour claim about a running server: `./tests/run_northstar.sh` is green at **61 suites, 2603 checks**, and the health-event suite exercises `?type=`, `?category=sleep` (which is a multi-type `IN` list), paging, and `totalElements` -- the count query and the page query, which are the two that take the collection.

The writes stay SQL, and that is a choice rather than a gap: an `INSERT` naming its fifteen columns says what it stores, and `eql` translates reads.

Left for later, in the order it should happen: the other seventeen repositories; a shared registry in `common.zm` once more than one entity has a `meta`; and tuple results, so `SELECT p, v FROM ..` can answer both entities instead of the first.


## memdb keeps its uniqueness index instead of rebuilding it, and a one-row write stops being a pass over the table (2026-10-26)

A `PRIMARY KEY` or `UNIQUE` index now lives on the `Table` as a value and is maintained by the three functions that may give a table new rows. Inserting rows one statement at a time into a keyed table, before and after:

| rows | before | after |
|---|---|---|
| 500 | 376 ms | 62 ms |
| 1,000 | 1,519 ms | 120 ms |
| 2,000 | 8,090 ms | 274 ms |
| 4,000 | 48,324 ms | 522 ms |
| 8,000 | *not measured; ~5 minutes extrapolated* | 1,038 ms |

Quadratic and then worse — the growth factor rises from 4x to 6x per doubling as the collector takes over — against about 2x per doubling after. **93x at 4,000 rows**, and the shape is now linear.

**The question this started from was the wrong one, and that is the useful part.** It was asked as "would mutable collections make memdb faster", which is the natural suspicion: a `Database` is an immutable value, a write publishes a new one, so surely the copying is the cost. Measured, that suspicion is empty. Appending to an immutable array, including through a `with` rebuild of the struct that holds it, is amortised O(1) and indistinguishable from a `MutableArray`:

| appends | `a = push(a, x)` | through `with t { rows: .. }` | `MutableArray` |
|---|---|---|---|
| 2,000 | 0 ms | 1 ms | 0 ms |
| 4,000 | 1 ms | 1 ms | 1 ms |
| 8,000 | 2 ms | 2 ms | 1 ms |

`array_push` takes the next slot of its buffer in place when the array it is given is the tip of that buffer (src/value.c), and a `Map` is a HAMT whose `set` copies the path from the root and shares the rest (src/mapobj.c). Immutability here costs a pointer, not a pass. Two further measurements said the same thing from the other side: the same 4,000 single-row inserts into a table with **no constraint** took 262 ms, and the same 4,000 rows in **one** statement took 157 ms. Same data, same immutable collections, no quadratic — so the cost was neither the data structure nor the number of rows but something the *keyed* path did per statement.

That something was the index. `unique_rule` had two strategies and both were O(rows) per statement:

* a statement that wrote **more than four rows** built a bucket map over the whole table — the optimisation that took a 400-row `UPDATE` from 1.4 seconds to 20 ms, by replacing 160,000 key comparisons with 800;
* a statement that wrote **four or fewer** scanned the table once per written row instead, on the reasoning that building a map for a single row could not pay for itself.

Each was right about the other and they were wrong together: the cost they were dividing between them was the *building*, and the division made the per-statement O(rows) unavoidable either way. A kept index does not build. Both branches are gone and the dual path with them.

**Keeping it in the table's value is what makes it correct, and that is the argument for immutability rather than against it.** `ROLLBACK` republishes an older `Database`; the index that comes back with it is the one that matches those rows, with nothing to invalidate and no cache to tell. A failed statement is the same story: `run_insert` extends the index before `enforce` runs, and when `enforce` refuses, the table carrying the extension is simply never published. A mutable index would need both of those written by hand, and the second is the kind of thing that is wrong for a year.

**The hazard it introduces is specific, so it is held down by construction.** A stale index does not fail — it accepts a duplicate without a word. Five sites write rows and one rewrites them all, which is exactly the count that gets a sixth added with no index update, so `rows_appended` (O(added)), `rows_changed` (O(touched)) and `rows_rebuilt` (O(rows), where a `DELETE` shifts every later position and the statement is O(rows) regardless) are now the **only** functions that may hand a table different rows, and the field's comment says so. A `with t { rows: .. }` written by hand still compiles, which is the residual risk and why the fixture below exists.

`tests/lang/memdb_kept_index.zm` runs 40 write statements on `memdb:` **and on real SQLite**, comparing the outcome and the whole contents of every table after each one — because the engine cannot check this against itself: the code that decides and the code that records are the same code. The cases are the ones where positions move or keys change hands: a key freed by an `UPDATE` and reused, a key taken by one and refused, a `DELETE` of the *first* row, a `ROLLBACK` over a `DELETE FROM`, an `ON DELETE CASCADE` that moves positions in a table the statement never named, NULL keys (in no bucket, since such a key conflicts with nothing), and writes that were refused and must leave no trace. 40 of 40 agree.

Three cases in it compare `ok` without a row count, for `BEGIN`/`COMMIT`/`ROLLBACK`. That is not a concession: none of the three affects a row, `memdb` says 0, and SQLite answers the count of the statement *before* it, because `sqlite3_changes` is not reset by a transaction keyword. The contents are still compared after each.

Two things fell out of the A/B, both measured interleaved because one bench process's allocations shape another's collector pauses:

* **A table with no key must pay nothing.** The first version built an empty index list per statement, which cost 22% of a write to an unkeyed table (4,000 inserts, 330 ms against 400) — all of it allocation for a list that stays empty. Both maintaining functions now return early.
* **A statement that fills an empty table rebuilds rather than maintains.** Maintaining row by row made a 4,000-row single `INSERT` 17% slower (217 ms against 182). This is not a threshold — the "small writes this way, large ones that way" split is what the replaced code got wrong — but an exact condition: when the table was empty, every row is new, so a rebuild and a maintenance produce the same index and the rebuild may use a mutable map in one pass. With it, the bulk shape matches the old engine exactly (190-198 ms either way). A large insert into a table that *already has rows* is left maintained, because there the two are not the same work.

`tests/stdlib/memdb_test.zm`'s seeding guard was rewritten rather than retuned. It allowed 32x for 4x the rows and said in its comment that seeding is "quadratic in principle" because a `Database` is an immutable value — a claim this entry's second table disproves. It now asserts 4x the rows costs under 8x the time, and the new assertion was checked against the old binary, where it fails at 2,606 ms against a 944 ms bound. A test that cannot fail on the code it was written for proves nothing.

What this does **not** fix, measured and left alone: a one-row `SELECT ... WHERE id = ?` is still a scan, because `CREATE INDEX` is recorded nowhere and a kept uniqueness index is not consulted by query planning. That is a larger piece of work — it changes what `run_select` does, not what a write maintains — and the next candidate after it is the parser, which the profiler puts at 21% of an unkeyed write (`sql::tokenize`) and which is re-run for every identical statement.


## `eql` guarded on real PostgreSQL and MySQL, and `sql.zm` kept out of the drivers (2026-10-26)

Two questions, asked together: can `sql.zm` and `eql.zm` be used in the concrete drivers? They have opposite answers, and both are now measured rather than argued.

**`eql` works on every driver, and that is now guarded rather than asserted.** `tests/integration/pg/pg_eql.zm` and `tests/integration/mysql/mysql_eql.zm` run the same entity queries against real PostgreSQL 17 and real MySQL 8, under `make test-pg` and `make test-mysql`. They print the same lines by construction, so **the two `.out` files differ in exactly one line**:

```
1c1
< dialect: postgres
---
> dialect: mysql
```

Every translated SQL string, every row and every refusal message is byte-identical across the two engines. Anything that appears in that diff later is a dialect leak in the translator, which makes the pair a standing check and not just two passing tests. Together with `memdb` and the SQLite cross-check, `eql` is now exercised on four engines.

Two mechanisms do that work, and naming them is the point, because neither is luck:

* **`$N` is the common denominator.** PostgreSQL and SQLite take it as written, `memdb` reads it directly, and MySQL's driver rewrites it to `?`. MySQL is therefore the engine that actually tests the claim — it is the only one of the four that does not accept `$N`, so the SQL the fixture prints is what `eql` produced and not what reached the server.
* **`SELECT p` expands to aliased columns** — `p.id AS id, p.last_name AS last_name, ..` — so the names a `from_row` reads belong to the translator and not to the engine. That is what lets one `patient_from_row` serve all four, and what a `SELECT *` would not have given.

The portability that is *not* eql's to give is worth stating in the same breath: it extends exactly as far as the SQL in the query text does. `LIMIT`/`OFFSET`, `GROUP BY`/`HAVING`, `LIKE` and `IN` travel; a projected boolean or `||` concatenation would not, and `memdb-ansi:` is still the cheap way to find that out before a container does.

**`sql.zm` in a driver: measured and declined.** No driver uses it, and there is exactly one place that is tempting — `mysql.zm`'s `rewrite_placeholders`, sixty lines of hand-written lexing (single, double and backtick quoting, backslash escapes, doubled quotes, `#`, `--` and `/* */` comments) whose entire job is to find `$N` *outside* strings and comments. That is `sql.tokenize`'s job, and the duplication is real. But the substitution is a regression, on ordinary MySQL:

```
SELECT 'a\'b', $1               -> REFUSED: a quoted string that is never closed
SELECT 1 # a comment with $1    -> REFUSED: unexpected character '#' in SQL
```

`sql.zm` is a SQLite-and-standard-shaped dialect, and the places it departs from MySQL **are** the lexing. Worse than the refusal is the near miss it implies: a `$1` inside a `#` comment would be rewritten and the parameters renumbered, which is a silently different query rather than an error. So a driver's lexer belongs to its server. This is where `sql` is syntax, `memdb` is semantics reaches its edge: the syntax a driver has to read is the server's, not the engine's.

One case in both fixtures is a mistake the first draft made, kept deliberately because the diagnostic is the feature: reading a projection (`SELECT p.last_name`) with `eql.list<Patient>` answers `no such column in result row: id`. An entity reader asked for a row that is not that entity, and it says which column was missing.

A recording note, since it cost ten minutes: **never pipe a golden-file capture through `head`.** `./ziman fixture.zm | tee fixture.out | head -12` truncated the `.out` at twelve lines, because `head` exiting closed the pipe and SIGPIPE killed the writer mid-file. The fixture was correct and the recording was not, and the resulting diff looked exactly like the fixture dying partway through its run.


## A statement is parsed once per connection, in all four engines (2026-10-26)

Asked for as "parsed and cached once in the driver and connection; parameters can be different in the second call but the SQL template is going to be the same", together with a second half -- format the statement into a full executable SQL string with the parameters injected -- which **cancels the first** and was dropped after that was put to the user.

The two are mutually exclusive, and the reason is in the request's own terms rather than in security: if the values are formatted into the SQL text then every call produces a *different* string, so there is no template that repeats and nothing for either the driver or the server to cache. Keeping the values out of the text is what makes a template a template; that is parameter binding. The rest of the case against injecting them is the ordinary one -- it would make SQL injection the normal path, where `mysql.zm`'s header currently promises values "never spliced into the SQL", and it would turn every value into a per-dialect literal problem (`'` doubling against backslash escapes, bytes as `'\xDE'::bytea` against `x'DE'` against `X'DE'`, booleans as `TRUE` against `1`, floats with no literal form in Postgres without a cast).

What landed, engine by engine, each measured interleaved against the previous binary.

**`memdb`: 2.3x on a keyed write, 3.2x on an unkeyed one.** `run_one` called `sql.parse(sql)` on every statement; a connection now keeps what it has parsed, by text. 4,000 single-row inserts: keyed 503-573 ms to 219-235, unkeyed 365-411 to 123-125. More than the 21% the profiler attributed to `sql::tokenize`, because the whole parse goes -- tokens, tree, and the collector pressure from both. **Nothing needs invalidating**: the parse is a pure function of the text, a `Statement` is an immutable value, and no parameter is part of it, which is exactly why one template serves every later call.

**`eql`: 227 microseconds to 7, a 32x cut.** `prepare` re-translated a query with a collection parameter per call, because `IN :types` is as many placeholders as there are values. The fix is **a value rather than a cache**, and that was forced rather than chosen: `eql` sits above the connection with nowhere to keep per-connection state, module-level mutable state is refused, and Ziman has no mutex -- a shared mutable cache would be unreachable *and* unsound. So `translate` works out the first eight sizes up front and keeps them in the immutable `Translated`, which every thread may read and nothing can invalidate.

**`pg`: about 6x where planning dominates, and nothing measurable where it does not.** The driver now `Parse`s under a name on first use and `Bind`s to the name afterwards. Measured on a six-join query over tiny tables, chosen so the planner works and the executor does not: 1,930-2,507 microseconds to 280-437. On a single-table lookup: 215-257 to 112-255, which is **within the noise and is reported as no result** -- the first benchmark attempted said 38% faster and then said slower on the next round, which is what a benchmark looks like when the effect is smaller than the variance. It saves no round trip, because the Parse/Bind/Describe/Execute/Sync batch was always one write and one read; what it saves is the server's parsing and planning, and the statement's bytes on the wire.

**`mysql`: about 1.8x, and the discovery of something much larger.** Here a call really did cost two round trips -- prepare, parameter and column metadata, execute, close -- and is now one. But the first measurement said **42,700 microseconds a call against 332**, which is not a round trip, and 40 milliseconds is the signature of Nagle meeting a delayed ACK. **`TCP_NODELAY` is set on no socket anywhere in this runtime.** The old path ended every parameterised call with an unreplied `COM_STMT_CLOSE`, so the next call's small write waited for an ACK that the server was holding back. With `TCP_NODELAY` patched in as a scratch experiment, the four cells read:

| mysql, one-row lookup | Nagle on (as shipped) | TCP_NODELAY |
|---|---|---|
| no statement cache | 42,700 us | 450-547 us |
| statement cached | 272-345 us | 262-292 us |

So caching is worth 1.8x and *masks* an 85x stall rather than removing it: a statement past the cap is still prepared and closed per call, and still stalls. The scratch patch was reverted rather than folded in here -- it is one line, it changes every socket in the runtime including the HTTP server, client and TLS paths, and it deserves its own increment and its own suites. **It is now the most valuable single change available in this repository**, which is a strange thing for a statement cache to have produced, and the reason to write it down here.

Three design points are shared by both protocol drivers, and the first is why this was cheap at all:

* **The cache lives in `serve`'s locals.** Each of these drivers already gives one connection to one thread driving the socket in lock-step, precisely so that there is no mutex; a per-connection cache therefore needs no lock, and **a reconnect runs a fresh `serve` whose cache is empty**. The hazard worth naming -- a cache outliving its connection and binding to handles the new session never issued -- cannot happen rather than has to be remembered.
* **A name is recorded only when the whole call succeeded**, and closed otherwise. A `Parse` that failed left nothing to name; a `Parse` that succeeded before a failing `Execute` left a statement the driver would otherwise never mention again. Postgres makes this safe without knowing which happened: closing a name that does not exist is explicitly not an error.
* **Past the cap a statement goes the old way** rather than evicting one. Eviction means choosing a victim for a workload that has already outgrown the cache, where falling back costs exactly what every call cost before. On MySQL the cap does second duty: `max_prepared_stmt_count` is 16,382 across *all* connections, so 64 per connection is how a hundred-connection pool stays clear of it.

Where they differ is invalidation, and MySQL needs less care for a reason that is the server's: a binary-protocol statement is **re-prepared by MySQL** when the metadata under it changes, so a `DROP`/`CREATE` beneath a cached statement is handled before the driver could see it, and SQL-level `DEALLOCATE PREPARE` is a different namespace entirely. Postgres has no such protection, and nothing in the protocol reports it: `DEALLOCATE ALL` and `DISCARD ALL` remove the session's statements and the next `Bind` simply fails. Rather than recognise that failure afterwards and try to recover -- which would mean re-executing a statement whose side effects are unknown -- the driver reads the first keyword before sending and clears the cache. Those are the only two statements that can do it, so the check is complete, and it is on the no-parameter path too, which is never prepared but can still issue them. The keyword is folded with the **invariant** locale, because with a Turkish default `discard` upper-cases to `DİSCARD` and silently stops matching.

**Both drivers are watched by the server's own bookkeeping**, which is what makes any of this more than an account of intent. `tests/integration/pg/pg_prepared.zm` reads `pg_prepared_statements` and `tests/integration/mysql/mysql_prepared.zm` reads `Prepared_stmt_count`: one template called three times holds one statement, a second template holds two, `DEALLOCATE ALL` and `DISCARD ALL` empty the catalogue and the next call still answers, a failed prepare and a prepare-then-failed-execute each leave nothing recorded, and seventy distinct templates against a cap of sixty-four all answer with the count resting at exactly sixty-four. Reading the catalogue is safe inside the tests because a statement with no parameters goes through the simple or text protocol and is never prepared, so the counting query never appears in its own count.

That MySQL test earned its place on its first run, by failing: **`defer` runs when its enclosing *block* exits, not the function's.** A `defer close_statement(..)` written inside `if !keeping { .. }` closed the statement the moment that `if` ended, before the execute that needed it, and MySQL answered `Unknown prepared statement handler (68)` -- at the cap, which is the only place the unkept path runs. It is a function-level `defer` over a flag now, and the reason is written where the code is.

One behaviour change to state plainly rather than bury: **Postgres moves a prepared statement to a generic plan after about five executions.** Much of the 6x above is that, and it cuts the other way for a query whose best plan depends on the parameter value -- a selective value and an unselective one now share one plan. It is inherent in reusing a prepared statement rather than in how this was built, and `plan_cache_mode = force_custom_plan` is the server-side escape hatch.

`sqlite` is the one engine left: its statements are prepared in `src/native_sqlite.c`, so caching them is C work rather than Ziman, and it is the cheapest of the four to begin with, having no socket and no server.


## The statement cache reaches SQLite, and finds that Nagle was never turned off (2026-10-26)

The last two parts of "parsed and cached once in the driver and connection". One is the smallest result of the five and one is the largest thing in this repository, and they are in the same entry because the second was found by disbelieving the first.

### SQLite: the cheapest engine to cache and the least to gain

`sqlite3_prepare_v2` runs in process, so there is no round trip to save -- the saving is its parse and code generation, and nothing else. Interleaved A/B, 4,000 calls each:

| | base | cached |
|---|---|---|
| insert | 8-9 us | 7 us |
| one-row lookup | 11-12 us | 9 us |
| `GROUP BY` over 4,000 rows | 367-368 us | 361-371 us |

About 15 to 20% on a cheap statement, nothing where the cost is in the execution. **Small, and recorded as small.** It does reproduce across rounds, which is the difference between this and the Postgres single-row lookup that was reported as no result at all.

So the work here was not the speed, it was that the speed costs no correctness, and SQLite's hazards are not the socket drivers':

* **`DbConn` carries a mutex.** This connection is shared between threads rather than owned by one -- the opposite of the pg and mysql drivers, whose caches are safe because a single thread owns the socket -- so the cache lives under the lock that every site which prepares already takes.
* **`sqlite3_close` answers `SQLITE_BUSY` while any statement is live.** Both close paths, the explicit one and the collector's finalizer, therefore drain the cache first. Without it `close` silently stops closing: the handle leaks, a file database stays locked, and nothing says so.
* **A cached statement is reset after use as well as before.** An unreset statement holds its read lock and a cached one lives as long as the connection, so skipping the second reset would leave a reader open for ever and block every writer. The return value of `sqlite3_reset` is the *previous* run's error code, already reported, and is deliberately ignored.
* **`prepare_v2` is what makes a schema change safe.** SQLite re-prepares a v2 statement itself when the schema under it changes, so an `ALTER` between two uses of one template needs no handling here. A v1 statement would answer `SQLITE_SCHEMA` and have to be redone by hand.
* Only the first statement of a script is a candidate, and only when nothing follows it, so the script path is untouched -- bound parameters are still refused alongside a multi-statement script.

**The test for all of that was worthless in its first version, and that is the part worth keeping.** It opened and closed two thousand file-backed connections, each caching a statement, on the reasoning that a leaked descriptor per connection could not survive two thousand cycles. It passed **with the draining deliberately removed**, because `ulimit -n` on this machine is 1,048,576. It now reads `/proc/self/fd` and compares the count either side, and was checked both ways: `descriptors did not grow: false` without the drain, `true` with it. That is the second time in one day -- after the memdb seeding guard -- that a test had to be run against the broken code before it could be believed. **Verifying a guard is a separate step from writing it**, and skipping it produces a test that reports success for the wrong reason.

### Nagle: never turned off, and worth more than the feature that found it

`TCP_NODELAY` was set on **no socket anywhere in this runtime**.

Nagle holds a small write back until the previous one is acknowledged; the peer's delayed ACK holds that acknowledgement for up to 40 milliseconds. Together they are a 40ms stall on any protocol that writes a small request, reads a reply, and writes again -- which is every protocol here.

It was found by refusing to accept a good number. The MySQL statement cache appeared to make a parameterised query **129x** faster, and saving one round trip of two cannot do that. 42.7 milliseconds a call is not a protocol cost, and 40 of them is a number with a known cause. The old path ended each call with an unreplied `COM_STMT_CLOSE`, so the next call's small write waited for an ACK the server was holding back. With `TCP_NODELAY` patched in as a scratch experiment, the four cells read:

| mysql, one-row lookup | Nagle (as shipped) | TCP_NODELAY |
|---|---|---|
| no statement cache | 42,700 us | 450-547 us |
| statement cached | 272-345 us | 262-292 us |

and, measured again per statement rather than per configuration:

| | Nagle | TCP_NODELAY |
|---|---|---|
| kept (cached) statement | 200-300 us | 275 us |
| unkept statement | 42,575-42,825 us | 475-900 us |

So **a kept statement was never affected** -- its path is a clean write-then-read with no trailing unreplied write -- and an unkept one is 50 to 90 times faster. The statement cache did not fix the stall; it hid it for the statements it keeps and left it for every other one, which is worse than either fixing it or leaving it alone, because it makes the pathology harder to find. **An optimisation that routes around a defect conceals it**, and that is the general lesson here: the 129x was not a result to report, it was a symptom to chase.

The fix is three sites and one helper:

* `set_nodelay(fd)` **ignores failure on purpose**: a Unix-domain socket answers `ENOPROTOOPT` and has no Nagle to turn off, so attempting it everywhere and ignoring the answer is both correct and the shortest way to say so.
* Outbound connections, at the one success point in `open_socket_timeout`, guarded on `!listener` because the option belongs to a connected socket.
* Accepted sockets, immediately after `accept(2)` -- there rather than on the listener, because an accepted socket does not reliably inherit it, and before both returns so the TLS path gets it too.

**Latency over throughput is the right default** for a runtime whose sockets carry database protocols and HTTP. A caller wanting the other trade-off writes one large buffer instead of several small ones, which is what the HTTP server already does with a response. The verification that mattered was breadth rather than depth: `make check`, whose North Star phase starts 49 real HTTP servers through the accept path, plus `test-pg`, `test-mysql` and `test-tls` -- the TLS handshake being the one place this could plausibly have interacted badly, and the reason that suite was run at all.

One number to keep in proportion: across the five engines the statement cache was worth 2.3x, 32x, 6x, 1.8x and 15%. One line of `setsockopt` was worth 50 to 90x. The feature was asked for and is worth having; the defect it uncovered was not asked about by anyone, and had been there the whole time.


## The table name moves into the annotation, and an entity becomes a struct again (2026-10-26)

`#[derive(Entity("vital_signs"))]`. The user's suggestion, and it was better than the objection already written against it.

`docs/SYNTAX.md` said the table is not derived because *"no pluralisation rule survives real schemas"* -- `FamilyHistory` in `family_history`, `UserNotificationPreference` in `user_notification_prefs` -- and that is still true and still the reason nothing guesses. But it is an argument about **deriving** the name, and it had been read as an argument about **where the author writes it**. Written inside the attribute it is still written explicitly, just once instead of three times.

Three times is what it was. `entity/vital_sign.zm` before:

```ziman
#[derive(Row, Entity)]
pub struct VitalSign { .. }

pub val table = "vital_signs"
pub val meta = eql.entity("VitalSign", table, vital_sign_columns)
```

The table twice, and the struct's own name a third time inside the call. All three are mechanical given the table, so the table is the only one worth writing, and the file is now the struct and its annotation and nothing else.

**What made the change bigger than it looked**: `pub val table` is not an `eql` artifact. It predates that work and the *services* import it -- `use entity/allergy.table` and twenty more -- so emitting it from the derive is not an addition, it is a replacement, and every one of the **21** entities that derives `Entity` also hand-wrote it. A 1:1 mapping, so the conversion was uniform: **119 lines deleted, 21 added**. Had the derive emitted only the metamodel, the table name would have appeared *twice* and the change would have been a loss.

Three decisions, each one a fork that could have gone the other way:

* **The argument is optional.** 18 of those 21 entities derive `Entity` for the column list alone and have no use for a metamodel, so requiring a table would have forced one on them. `#[deprecated]` is the precedent for an attribute whose argument is decoration rather than its whole content -- against `#[when_unmatched]`, whose bare form is a syntax error because there the message *is* the content.
* **The entity's name is the struct's, not the table's.** A query writes `FROM Patient p`, naming the type the reader has in hand. Deriving it from the table would make `FROM patients p` the spelling, which is SQL leaking back through the one layer whose job is to hide it.
* **The argument must be a string.** `Entity(patients)` is refused, because a bare word could plausibly name a type, and the message names the form that works.

The grammar change is one optional `("...")` per derive target. `DeriveList` and both declaration nodes carry a `derive_args` array parallel to `derives` -- one array rather than a struct per target, since exactly one target reads an argument, and a parallel array is the shape the rest of `ast.h` already uses for this. The generated `eql.entity(..)` call needs the module, and a file that wrote only the attribute has no reason to have imported it, so the derive calls `ensure_module_import` -- which is what the standard-type conversions already do for `time` and `uuid`.

**The effect on work not yet done is the part worth keeping.** 24 repositories still read raw SQL, and adopting `eql` in one used to mean editing its entity first: add the target, add the table, add the metamodel. Every entity now has a `<name>_meta` waiting, so that step is gone -- which is a thing to notice about this kind of change. It removed 98 lines, and the reason to do it was the 24 edits it will not need.

Guarded by two fixtures, both forms in one file so they cannot drift apart, and one for the refusal. `make check` green at 1208 fixtures with North Star at 62 suites / 2646 checks; in the application, the in-process repository suite at 43 checks and the behavioural suite with 0 failures.


## Code generation moves out of the parser, and what sat between the old position and the new one (2026-10-26)

`generate_derived_functions(module)` ran at the end of parsing **one file**, from the first derive until today. It now runs in `load_module`, after a module's dependencies are loaded and before its named imports are resolved.

**The specification that forced it was one line.** Designing a `#![repository(VitalSign)]` that generates the ten CRUD functions a repository writes by hand, the requirement settled as: the annotation names the entity and nothing else, and the generated `delete(conn, id: Uuid)` takes its parameter's name and type from the entity's `#[pk]` field. That cannot be met from the parser. The parameter name and type are compile-time text, the field is in another module, and at the end of parsing `repository/vital_sign.zm` the entity module may not have been read yet. The alternatives were to repeat the fact in the annotation, or to assume `id: Uuid` -- right for 27 of 27 entities in the application today, and the dangerous kind of wrong, because a wrong parameter *name* with the right type compiles and reads perfectly well.

**`load_module` already had the position**, which is why this is three edits rather than a redesign: it parses, loads every dependency to completion, then resolves named imports. Generation belongs between the second and the third -- dependencies are loaded, so a generator may read an imported `StructDecl`; imports are unresolved, so a generator may still add the ones its output needs.

Four callers ask for generation now, because `parse` no longer does it: `load_module`, the entry module, and `--api <file>` -- which lists derived functions, so dropping it would have quietly changed that output. The formatter and the LSP's prelude read do not, because they want the text.

**Both failures were in the gap between the old position and the new one, and neither was the risk that was predicted.** The roadmap entry said the thing to verify was whether any of the seven derive targets depends on running before anything else is known. None does. What does depend on it is the loader, twice:

* **A segfault.** `#[derive(Entity("t"))]` calls `ensure_module_import` to add `use eql`. Appended after the loop that loads uses had already run, so nothing loaded `eql`, and resolving the generated `eql.entity(..)` call dereferenced a module that was never there. The seam needs both halves: generate after dependencies are loaded, *then load what generation itself imported*.
* **Two fixtures, and a lesson about which tests are load-bearing.** The loader assigns every declaration's `source_file` before the uses loop, so the newly generated declarations missed it -- and `source_file` is what later phases report an error *in*. `derive(Ord)` on an unordered field went from *"type error at 2:1 in tests/lang/ord_derive_field_not_ordered.zm (in derive(Ord) for 'Bad')"* to the same sentence with no file. Exactly two fixtures caught it, because the field is invisible unless a derive's **output** fails to check, and almost nothing has a deliberately broken derive. Without those two this would have shipped as "every error inside generated code names no file", to be found by whoever next debugged one.

**The general lesson is about moving a phase at all.** The hazard was not the phase and not its new neighbours -- it was everything that had been running *between* the old position and the new one, each step of which had quietly assumed the generated declarations were already there. Neither failure is visible by reading `generate_derived_functions`, and neither would have been found by reasoning about derives. A suite of 1,208 fixtures found both in two runs.

What this unblocks, in order: `#[pk]` on a field, so the metamodel carries the key columns rather than assuming the first one; `eql`'s runtime CRUD half, callable without any annotation; and then `#![repository(..)]`, whose generated signatures are now derivable from the entity. The remaining four decisions of the roadmap entry are untouched -- whether a generator may write into a module it is not declared in, what happens on a name collision, whether generated declarations are visible in the source, and whether the name contract can be checked rather than conventional.


## `eql` writes one row, and the column list stops being written twice (2026-10-26)

`eql` could read. Its first two writes -- `insert_one` and `find_by_key` -- take an `Entity` rather
than a translated query, because **a single-row write has nothing to translate**: the table, the
columns and the key are already in the metamodel that `#[derive(Entity("t"))]` writes, so the
statement is derivable and the query language would only be a way of restating it.

What this removes from a repository is a *second* ordering convention. The hand-written insert named
ten columns in one place and filled them from `vital_sign_to_params` in another, and the two had to
agree in order -- checked by nothing, and wrong in the worst available way: swap two adjacent
columns of the same type and every value is valid, the row saves, the API answers 200, and only the
contents are wrong. `insert_one` builds the statement from `entity.columns`, which the derive writes
from the same struct `to_params` walks. The two agreements become one fact.

**`writable` holds the two refusals none of them may forget**, which is why it exists before there
is a second caller: an entity with no key cannot have one of its rows addressed, so a missing `#[pk]`
is an error at the call rather than an update with no `WHERE`; and a value count that disagrees with
the column count is refused rather than sent, which is what a renamed field without a regenerated
`to_params` looks like.

**`find_by_key` refuses a second row rather than taking the first.** This is the one deliberate
strictness. An entity whose `#[pk]` does not actually identify a row -- one column of a compound key,
a key marked on a non-unique column -- otherwise returns plausible data forever, picked by whatever
order the engine happened to use. The refusal names the key columns, so the message says what is
wrong with the entity rather than what went wrong with the query.

Together they turn the sample backend's `save` into one line, and the read-back moves from id *and*
patient to the key alone: equivalent, because the row was just inserted with that id, and one query
fewer to keep right.

Eight of the ten runtime writes are still to come. The order is set by what the repository actually
hand-writes, not by what looks symmetric.


## An update takes a scope column, and why the read-back does not go through the key (2026-10-26)

`eql.update_one` replaces every non-key column of one row from one array of values in column order,
which removes the second half of the `SET`-list problem `insert_one` removed the first half of: nine
assignments in one place, nine values from `to_params` in another, agreeing by a convention nothing
checked.

**It takes a `scope`: columns the stored row must match as well as the key.** This exists because
the repository it replaces needed it. `repository/vital_sign.zm`'s `WHERE` matched `id AND
patient_id`, so an entity carrying a different owner moves no row -- the defence added earlier the
same day, after running the key-only version and watching four rows become three and a stranger's
none become one. A key-only `update_one` would have quietly undone it. The scope's values come out
of the same `values` array by column position, so naming one adds no second argument to keep
aligned, and `[]` is the plain keyed shape the generated repository will use.

**The decision worth recording is the read-back.** The obvious implementation reads the row back by
its key, since the key is what identifies it. With a scope that is wrong in the one direction that
matters: an entity carrying a different owner matches no row, so the `UPDATE` correctly changes
nothing -- and then a read-back by the key alone finds the row anyway and hands back *the other
owner's data*. The update refused and its subject leaked, in one call. Going through the same
predicate answers `NotFound`. Verified rather than argued: swapping the read-back to the key makes
`a_scope_column_refuses_an_update_that_would_move_the_row` fail with
`expected Err, got Ok(Visit { id: 1, patient_id: 1, .. })`.

**The other obvious implementation is to count the rows the `UPDATE` affected**, and it is ruled out
by an engine divergence rather than by taste: MySQL reports rows *changed* rather than rows matched
unless `CLIENT_FOUND_ROWS` is set, and `lib/db/mysql.zm` does not set it. Storing the values a row
already has would then be indistinguishable from matching nothing, so an idempotent update -- a
retried request, a form submitted twice -- would answer `NotFound`. The predicate read-back has no
such regime, which is why it does both jobs. `an_update_that_changes_nothing_still_finds_its_row`
exists to hold that, and reads like a tautology only until this paragraph.

Two refusals are new, both for mistakes about the entity rather than about the data: an entity whose
every column belongs to its key has nothing an update could set, so it refuses rather than sending
`SET` with no assignments; and a scope column that is already part of the key says nothing, so it is
a caller error rather than a redundancy to absorb. `find_by_key` gained the no-key refusal it was
missing -- it built `WHERE` out of an empty key list, which is not a statement.

Seven of the ten runtime writes remain.


## `#![repository(Entity)]`, and the rule that a hand-written function wins (2026-10-26)

The seven remaining runtime writes landed first -- `delete_one`, `delete_where`, `delete_all`,
`find_maybe`, `find_all`, `exists`, `count_all` -- and then the attribute that calls them.
`#![repository(VitalSign)]` generates nine functions whose body the entity decides, and the sample
backend's `repository/vital_sign.zm` now carries it.

**The attribute names the entity and nothing else**, which was the specification that moved
generation out of the parser weeks of work ago: `delete(conn, id: Uuid)` takes its parameter's name
and type from the entity's `#[pk]` field, and the entity is in another file. The lookup lives in
`loader.c` and the emitting in `derive.c`, split exactly there because at the generation point it is
the loader that knows which files are loaded -- `find_loaded` is the only way from a `use` path to
the module it named -- while `derive.c` owns `emit_to_sql`, so a key's value reaches SQL by the same
route its column does.

**What is not generated is as decided as what is.** A repository's domain queries stay hand-written:
"this patient's vital signs, newest first, page 3" is a fact about the application, not about the
entity, and a generator inferring it from a function name would be a naming convention pretending to
be a feature.

**A name the module defines itself wins, and nothing is emitted for it.** This was the one open
decision, and it was settled by what it would have cost here:
`repository/vital_sign.zm` scopes its `update` and `delete` by the patient as well as the id, which
is what makes another patient's row *not found* rather than moved or deleted. A generated keyed
`update` replacing it would have removed that silently, in a diff that reads like an improvement.
Generating only what the file does not write makes the attribute addable to 27 repositories already
written, none of them losing a guarantee. The cost is that the file does not say which half is
which; `--emit-derived` prints the generated functions and names each one it skipped, and `--api`
names the attribute, since reading one file in isolation cannot reach the entity in another.

**The generated reads are not owner-scoped, and that is a property of the entity, not an oversight.**
`VitalSign`'s key is its id alone, so the generated `find(conn, id)` answers any patient's row. Every
hand-written access in that file is patient-scoped, so the attribute adds seven entry points that
are not -- additive, unused until called, and a thing a request path must not reach for. The
alternative is a `#[scope]` field attribute beside `#[pk]`, which would put the owner into every
generated signature; it is not built, and is the shape to reach for if an unscoped read is ever
called by mistake.

**Three places leaked the loader's internal name.** A declaration read out of an already-loaded
module carries its self-qualified form -- `entity/vital_sign::VitalSign` -- and `::` is not source
syntax, while the generated text *is* source. It appeared as `id: uuid::Uuid` in a signature, as
`uuid::uuid_to_sql(id)` from `emit_to_sql` falling past the standard-type table, and as the entity
name in the splice label. One helper, `bare_name_of`, handles all three; it strips only `::`,
because a dotted `uuid.Uuid` is how a program writes a qualified type itself. Worth recording
because nothing about reading a `StructDecl` suggests its name is not the name in the source: the
field is the same field the Row derive reads, and the difference is only *when*.


## A `{ .. }` block is an expression, and the crash that was waiting under module initialisation (2026-10-26)

The question was whether ziman should have top-level `{ .. }` blocks run once at load, interleaved
with the global vals. The answer was no, and the reasons are worth keeping because they are about
what the language already is rather than about taste:

**The semantics already existed.** `val _ = install()` runs before `main`, once per module, in
dependency order -- deepest first, then file order. Measured with three modules, where a shared
dependency ran once: `dep loaded / mid loaded / entry loaded / main`.

**What a static initialiser is classically *for* is already refused.** Java's `static { MAP.put(..) }`
exists to fill static mutable state, and a module-level `MutableArray` is rejected by name here
("every thread would share it"). So the feature would have arrived with its principal use case ruled
out, leaving only external effects -- the ones least wanted on import, since importing a module to
call one function from it would fire them.

**And nobody had wanted it**: `val _ =` appears zero times across `lib/` and the sample backend --
27 repositories, a db layer, an HTTP server, a graphics library.

**What the question did expose was a different gap**, and the one worth filling: a `val` needing
several statements had to invent a function to hold them. So `{` now starts a **block expression**,
whose last expression is its value.

**It cost 40 lines of parser and nothing else**, which is the part worth recording. `{` was free as
an expression starter because an uppercase identifier is what makes `Name { .. }` a struct literal
(`starts_upper`), so a bare brace was unambiguous in every position, condition included. And
everything behind it already existed for `if`/`switch`-as-a-value: `yieldify` *already* handled
`STMT_BLOCK`, because an arm of a value `if` is one, so turning a block's trailing expression into a
`STMT_YIELD` was already written. The new form reuses `EXPR_VALUE_BLOCK` whole -- the type checker
and the interpreter learned nothing new.

**The shape that motivated it is the one that needs the block, not a function.** A module-level val
built with a loop:

```ziman
val patterns = {
    val out = mutable_array_new<string>()
    for name in names { push(out, concat("^", name, "$")) }
    snapshot(out)
}
```

A `MutableArray` cannot be a module-level val's type, but a builder *inside* the block is not the
val's type -- `snapshot(out)` is. The handle never escapes the scope it was made in, which is
exactly the rule the module-level refusal exists to keep.

### The crash, which was not mine and was two bugs

The first module-level block segfaulted. So did `val x = if true { 1 } else { 2 }` -- on the
committed build, with no block expression involved. A value block at module level had **never**
worked:

`exec_stmts_in_scope` saves the current frame's Env before replacing it, as
`ziman_frame_env_at(ziman_frame_count() - 1)`. During module initialisation nothing has been called,
so the count is 0, the index is -1, and `ziman_set_frame_env` had a `t_frame_count > 0` guard while
the *reader* had none. `t_frames[-1]`.

**Guarding the read would have turned a crash into something worse.** `ziman_set_frame_env` is what
makes a *nested* scope reachable from a root walk at all -- `gc_mark_env_chain` only walks an Env
upward through its parents, and `gc_interp_roots.c` reaches a block's scope only through the frame
it was recorded in. With no frame that call is also a no-op, so the builder in the example above
would not have been a root, and a collection during initialisation could have taken it. A segfault
on every run would have become a corruption on some runs.

So module initialisation now gets a frame of its own, at both entry points (`interpret` and
`interpret_tests`), and the accessor refuses an out-of-range index. The fixture allocates 20,000
strings inside a module-level block for the second half, because the first half is caught by any
block at all and the second by nothing smaller.

**The general lesson is the one the frame-env comment in `exec_stmts_in_scope` already recorded
once**, about the same mechanism: a GC root that is reached only through a side table is invisible
in the code that depends on it. The first incident was a value in a `for` body; this one is a value
in a global initialiser. Both read as ordinary code, and in both the missing root was a no-op guard
two files away.


## `eql.bind` converts its own value, and the thirteen-against-three asymmetry (2026-10-26)

`eql.bind` took a `SqlValue`, so every binding list in a repository carried its conversions:

```ziman
[eql.bind("id", uuid_to_sql(id)), eql.bind("patient_id", uuid_to_sql(patient_id))]
```

The names and the values are what a reader of that line is checking, and `uuid_to_sql` is in the way
of both. It is also not a decision: `uuid_to_sql` is the one conversion a `Uuid` has, so writing it
out was a step that could be forgotten rather than a choice being made. `bind` now has an overload
per bindable type and the line is `[eql.bind("id", id), eql.bind("patient_id", patient_id)]`.

**The `SqlValue` overload stays, and is the point of keeping it.** A value whose SQL representation
*is* a decision says so by constructing the `SqlValue`: a decimal as text at a chosen scale, or an
`Option` as `Null`. The overloads deliberately do not cover `Option<T>` for exactly that reason --
"absent means NULL" is a choice a repository should write down.

**Nothing new was needed to make it resolve.** Full overloading shipped in F167-F202, `to_sql` was
already an overload set in `lib/time/time.zm` for the seven time types, and `uuid_to_sql` was
already there to call. The cost was `use time` and `use uuid.uuid_to_sql` in `lib/db/eql.zm`, which
is a dependency from the query layer onto two domain-scalar modules -- allowed, and already taken by
`lib/db/migrate.zm`, and not a cycle: `time` and `uuid` both depend on `db`, while `eql` is a
different module in that folder.

**`bind` covers thirteen types and `bind_list` three, and the asymmetry is the recorded decision.**
A collection parameter is an `IN` list, and what a query filters a set of rows by is identifiers,
numbers or text -- so `[string]`, `[int]` and `[Uuid]`. `IN` over a list of durations or zone
offsets is not a shape any query in either repository has, and the `[SqlValue]` overload takes one
if it ever is. Mirroring `bind` exactly would have been ten more one-line functions for query shapes
nobody writes, which is the kind of symmetry that costs API surface and buys nothing.

What it removed from the sample backend: **25 conversions** at the binding sites (18 `uuid_to_sql`,
6 `SqlValue.Int`, 1 `SqlValue.Text`) and **three copies of a ten-line `type_values` helper** whose
whole job was turning `Option<[string]>` into `Option<[SqlValue]>`. With `bind_list` taking the
`[string]`, what is left at the call site is the part that was always a real decision -- that an
empty filter answers nothing without asking the database.


## `DELETE` joins the entity query language, and `UPDATE` does not (2026-10-26)

`eql` translated `SELECT` and refused everything else with *"an entity query starts with SELECT --
writes stay SQL"*. `DELETE FROM VitalSign v WHERE v.patient_id = :patient_id` now translates to
`DELETE FROM vital_signs WHERE patient_id = $1`, and `eql.execute` runs it for a row count.

**The line is drawn by what each statement needs, not by how much work it is.** A bulk delete is a
`WHERE` over one entity and nothing else -- the same translation a `SELECT`'s `WHERE` already gets --
and the reason to write it as an entity query is the reason a select is: a misspelt `v.patinet_id`
fails when the module loads rather than on the request that would have run it. A bulk update
additionally needs a `SET` list, which is the half with no entity to check it against; and a one-row
update is already `eql.update_one`, which builds its statement from the entity's own columns and so
cannot disagree with them. An `UPDATE .. SET ..` entity query would add nothing but a second way to
get a column list wrong.

**The alias is dropped from the SQL, where a `SELECT` keeps it, and that was measured rather than
assumed.** `DELETE FROM t AS x WHERE x.a = 1` is accepted by SQLite and *refused by memdb*
("unexpected 'AS' after the statement"), so the bare form is the only one all four engines take. The
alias is still required in the entity query -- it is what makes `v.patient_id` a field the
translator can check -- it simply does not survive into SQL, where a single-table delete has nothing
to qualify against. This is the one translation decision a `DELETE` has that a `SELECT` does not,
which is why the SQLite cross-check gained a delete phase: five deletes, each on a freshly seeded
pair, comparing both the count reported *and* the ids each table is left holding, because a count
can agree while the wrong rows are gone.

**Three refusals, each about what the statement would otherwise do.** One entity, because a
multi-table delete is `USING` in PostgreSQL, a table list in MySQL and a subquery in SQLite -- there
is no single SQL to produce. A `WHERE` required, because without one the statement empties the
table, and `delete_where` already refuses an empty predicate for that reason while `delete_all` is
the spelling that means it; a filter must not be able to collapse into "every row". And
`DELETE v FROM ..` refused, because the row list between `DELETE` and `FROM` is one engine's
spelling rather than SQL's.

**The read and write paths refuse each other's statements by name**, and that guard is not about
returning a wrong answer: handing a `DELETE` to `eql.list` would have *run* it, removed the rows, and
handed back the empty result as though nothing had been found. `removes_rows` answers the one
question a `switch` over `Statement`'s thirteen variants would have cost thirteen arms.

### The fourth place the loader's `::` leaked, and why it waited (2026-10-26)

The entry above on `#![repository(Entity)]` says three places leaked `entity/vital_sign::VitalSign`
into generated source. There were four. `import_field_converters` compares a field's type name
against `STANDARD_TYPES` after stripping a `.`, and never a `::` -- so for a `#[pk] id: Uuid` read
out of a loaded module it matched nothing, and the generator emitted `uuid_to_sql(id)` while
importing no `uuid_to_sql`.

**It waited because the one repository using the annotation imported that converter by hand**, for
its own raw SQL. `#![repository(VitalSign)]` worked from the day it landed, on an import written for
something else. The gap appeared the moment the raw SQL went away: converting the file's two deletes
to `eql.execute` left `use uuid.uuid_to_sql` unused, removing it broke the module, and the error
named the attribute.

Two things worth keeping from that. The **Row and Json derives can never hit this** -- they read a
struct in their own module, whose field types are not self-qualified yet -- so the bug belonged to
exactly one caller and no existing fixture could have caught it. And the fixture that does catch it
had to be **two files**: a repository and an entity in its own module, with the repository importing
no converter at all. The single-file `repository_annotation` fixture passes either way, because its
key is an `int` and an `int` needs no converter. A cross-module generator needs a cross-module test,
and the first one written was not.


## Reading a module val before its initialiser has run is an error now, not a crash (2026-10-26)

```ziman
val greeting = decorated("one")

fn decorated(text: string) -> string {
    return concat(prefix, text)
}

val prefix = "p:"
```

This segfaulted. `EXPR_IDENT` ended `return b->value` with no check, and `env_lookup` answers NULL
for a name that resolves but has no value *yet* -- which is exactly a module-level val reached,
through a function, by an initialiser that runs earlier. Module vals are initialised in file order
(`init_global_vals`), so the order is a real constraint of eager initialisation; what it must not be
is a crash with no name in it.

**`init_param_defaults`' own comment had already written this down as the unaddressed half of the
same problem**, from the day the two eager passes were ordered against each other: *"A default
expression could, in principle, also reference an actual global val -- the reverse dependency,
unaddressed either way, and not yet hit by anything real in this codebase."* It is hit now, by the
ordinary shape of a repository that wants a one-argument `query(..)` helper over a registry declared
above it. The fix that would remove the constraint rather than report it is the one that comment
names -- evaluating globals lazily on first reference -- and it is still not built.

The message says what to do, and the stack trace now ends at `<module initialisation>`, the frame
added earlier the same day for the value-block crash. Two bugs in one morning in the same
mechanism, and the second one's diagnostic is legible because of the first one's fix.

**What found it was a three-line helper.** Turning
`repository.translated(reg, "SELECT ..")` into `query("SELECT ..")` across three repositories means
a module-level `fn` closing over a module-level `val`, and the first thing worth checking about that
shape is whether the order matters. It does, it was undiagnosable, and now it is one line of
message.


## `Registry` becomes a map keyed on the folded name (2026-10-26)

`Registry` was `{ entities: [Entity] }` and `entity_named` walked it, calling `sql.fold_name` on
**both** sides of every comparison. It is now `Map<string, Entity>` keyed on the folded name, so
folding happens once per entity at construction instead of once per entity per probe.

**This is a data-structure correction, not a measured win, and the entry says so because the
measurement does not support one.** Every registry read happens during module load, before `main`:
`bindings_of` resolves each `FROM`/`JOIN` entity, the rewriter resolves it again to write the table,
and `translate` re-runs the whole pass once per pre-computed collection size (`eager_sizes = 8`, plus
the base). So the worst case is about 18 probes per query, against a registry holding one entity in
every repository that exists today. Nothing reads a registry per request -- `prepare` and `execute`
work from the translated SQL string -- and that is the property worth protecting, not the scan.

What it does buy is that a *shared* registry stops being a different performance question from a
per-module one, which is the shape the sample backend keeps asking about.

**Two properties the list had and a map does not, both kept deliberately.**

The keys are folded, and a map has no order, so the refusal would have read
*"registered: vitalsign, bodymeasurement"* -- neither what the caller wrote nor the order they wrote
it in. `Registry` therefore carries `names: [string]` beside the map, for the message alone. That is
a field whose only reader is an error path, which is worth the line: the message names what you
asked for *and* what is available, and both halves are what makes a mistyped entity name a
five-second fix.

And a repeated name resolved to the *first* registered under a scan, where `set` into a map would
have made it the last. `registry` answers a `Registry` rather than a `Result`, so it cannot refuse
the duplicate -- changing that would put an `unwrap` at every call site for a mistake nothing has
made -- so it keeps first-wins with a `contains` check. Silently changing which of two entities a
query resolves to is the one outcome worse than either.


## The derived table val takes a prefix, and two entities fit in one module (2026-10-27)

`#[derive(Entity("t"))]` emitted `pub val table = "t"` under that **bare** name while its three
siblings were all prefixed -- `<snake>_columns`, `<snake>_meta`, `<snake>_from_row`. It is
`<snake>_table` now.

**The bare name made two entities in one module impossible.** Both emitted `table`, and the
collision was reported as *"'table' is already declared in this scope"* pointing at the **struct** --
because neither declaration appears in the source a reader is looking at. `entity/broadcast.zm`
holds `Broadcast` and `Dismissal` and could have neither, so neither reached the registry and
`repository/broadcast.zm` could not take the annotation. That is now the fixture
`derive_entity_two_in_one_module`, built in that module's own shape.

What it cost outside the one `cb_appendf`: ten service files importing `entity/X.table`, in two
forms. Nine aliased it to the name the val now has, so those lost a redundant `as`; `allergy` keeps
one, because its struct is `AllergyIntolerance` and the val is `allergy_intolerance_table`. **The
aliased form is what the first sweep missed** -- a regex for `use entity/X.table` does not match
`use entity/X.table as allergy_table`, and `--check serve.zm` named the file immediately.

**`Dismissal` is the schema's only compound key**, and converting it put the two-column path into
production for the first time: `dismiss` goes through `eql.insert_one`, which reads the row back
through `dismissal_meta.keys` -- both columns -- so a two-column `find_by_key` now runs against a
real database rather than only in a test.

**And `broadcast.set_active` is the clearest justification yet for `eql` having no `UPDATE`.** It
sets a single column. `eql.update_one` writes every non-key column from a whole entity, so using it
there would mean reading the broadcast first and overwriting whatever a concurrent request had
changed -- a worse statement than the one it replaced. A partial update has no entity-shaped
equivalent. That reasoning was written down when `DELETE` landed on general grounds; this is the
first place it is load-bearing, and the comment now lives in the function.


## A parameter named after a clause keyword is a parameter (2026-10-27)

`WHERE d >= :from AND d <= :to` -- the obvious names for a date range -- was refused with
*"no registered entity is called 'AND'"*, which names nothing the caller wrote.

`bindings_of` scans the token list for `FROM` and `JOIN` to find each entity and its alias, and it
walks tokens one at a time. The rewriter consumes `:name` as a pair, so it never sees the name
alone; `bindings_of` does, so `from` read as a clause and it went looking for an entity after it,
found `AND`, and reported that. One extra condition fixes it: a name preceded by `:` is a parameter.

**It took the sample backend's only date-range read to find.** `repository/encounter.zm` asks for
every patient's encounters with a follow-up in a window, and `from`/`to` are what those parameters
are called -- in the Spring backend too, whose `findByFollowUpDateBetween` this mirrors. Nothing
else in 26 repositories had a parameter named after a clause word, and the error message was far
enough from the cause that reading it would not have suggested the fix.

Tested for `:from`, `:to`, `:order`, `:join` and `:where`, which is the set a parameter might
plausibly be called.


## A name after `AS` is a label, not an alias (2026-10-27)

`SELECT count(*) AS n FROM Notification n` -- `n` for both the count's column name and the entity --
expanded the *label* into the entity's column list:

```
SELECT count(*) AS n.id AS id, n.patient_id AS patient_id, .. FROM notifications n
```

and then failed with `unexpected '.' after the statement`, about a `.` the caller never wrote.

The bare-alias expansion is what makes `SELECT v FROM VitalSign v` mean the entity's columns, and it
fires on any name in the select list that a `FROM` bound. A label is such a name. One condition
fixes it: not immediately after `AS`.

**The collision was avoidable and the fix is still right.** Every other repository in the sample
backend writes `AS n` with an entity alias of `v`, `m`, `p` and so on, so 24 conversions passed
before one chose `n` for a `Notification`. But a label is not an alias in any query, so the
translator should not read one as the other -- the same shape as the `:from` bug a few hours
earlier, where a name was read in a position it could not occupy.

**Where it surfaced is worth recording.** `--check serve.zm` passed it, because translation runs at
module *initialisation* and not at type-check time, so the only thing that could catch it was
running the application. It then failed at load with the SQL in the message, which is the property
the module-level `val` exists for: not on the request that would have read that column. Of roughly
twenty-five repository conversions this was the only mistake the type checker could not see.


## What adopting `eql` across 26 repositories taught, and what it did not remove (2026-10-27)

The entity query language went from one repository to all of them: **26 of 26 convertible
repositories**, 30 backend commits and 18 ziman commits, 1,356 insertions against 1,449 deletions
across `repository/`. The individual conversions are in those commits. What is here is what the
exercise found, because almost none of it was visible from the first repository.

### The real reason `eql` has no `UPDATE`

When `DELETE` landed the argument was that a bulk update needs a `SET` list, which is the half with
no entity to check it against. True, and far weaker than what adoption showed: **thirteen partial
updates across six repositories**, against a handful of full-row ones.

`user` alone has seven -- password hash, name, role and status, provider, reset, change, login
attempt -- each setting a named subset, several baking a literal into the `SET`. `eql.update_one`
writes every non-key column from a whole entity, so using it there would mean a password reset
rewriting `role` and `status` from whatever the caller happened to be holding.

Three shapes have no entity-shaped form at all:

* `notification.mark_all_read` -- two columns, many rows, and a predicate on the very column being
  written (`status <> $2`). Not one row, not every column, and the `WHERE` depends on the old value.
* `patient.lock` -- `UPDATE patients SET id = id WHERE id = $1`, a write that writes nothing,
  because that is what makes SQLite take its write lock and Postgres the row lock where a `SELECT`
  takes neither and `FOR UPDATE` is not SQLite's.
* `broadcast.set_active` -- one column of five.

So the rule is not "an update is hard to translate". It is that **an entity-shaped update can only
express a full-row write, and in a real application those are the minority.**

### Four defects only adoption could find

Each needed a real query. None would have come from more unit tests on the first repository.

* **`:from` read as a `FROM` clause.** `bindings_of` scans tokens for `FROM`/`JOIN` without
  consuming `:name` pairs the way the rewriter does, so a date range's obvious parameter names broke
  it -- with the error *"no registered entity is called 'AND'"*. It took the one date-range read in
  26 repositories.
* **A label after `AS` expanded as an alias.** `SELECT count(*) AS n FROM Notification n` produced
  `SELECT count(*) AS n.id AS id, ..`. 24 conversions passed first, because every other repository
  writes `AS n` with an entity alias of `v`, `m` or `p`.
* **`import_field_converters` stripped `.` but not `::`**, so `#![repository(E)]` over an entity
  with a `Uuid` key emitted `uuid_to_sql(id)` and imported nothing. Invisible while the one
  repository using the annotation imported that converter by hand for its own raw SQL.
* **A module-level `val` read before its initialiser segfaulted.** Found by wanting a three-line
  `query(..)` helper over a registry -- `init_param_defaults`' own comment had predicted the
  dependency and recorded it as unaddressed.

### Three places had already solved the column-list problem

`patient.save`/`update` built their statements from the entity's own `columns` at runtime,
`assessment.update` did the same through `crud.set_clause`, and `service/crud.zm` is that idea as a
whole module. `patient.zm`'s comment dates the bug they were avoiding: *the profile grew from 8
columns to 24 in one commit, and a hand-numbered INSERT would have gone on compiling while passing
24 parameters to 8 holes.*

That is the defect `insert_one` exists for, discovered independently three times before the library
had an answer. It is the strongest evidence the feature was worth building -- and the conversion of
`patient` is the one place `eql` replaced a *correct* hand-rolled solution rather than a risky one.

### "A hand-written function wins" became an override, not a grandfather clause

The rule was chosen so that a scoped `update` could not be silently replaced by a keyed one.
Adoption turned it into a deliberate mechanism: `file_attachment.insert` and `shared_link.insert`
are **named for the generated function on purpose**, because `INSERT .. RETURNING *` is one
statement where `eql.insert_one` is two. Writing a function under the generated name is now how a
repository keeps a better statement than the generator can produce.

### Four repositories declined the annotation, each for its own stated reason

`#![repository(E)]` is not free, and refusing it is a design choice:

* `encounter` and `recipe_ingredient` -- their rows are never addressed by id on their own, which is
  all nine generated functions do.
* `patient` and `user` -- a generated `delete_all(conn)` over *patients* or *users* is every account
  in the system in one call.
* `audit_event` -- the generated nine include `update`, `delete` and `delete_all`, which is exactly
  what an audit table forbids.

That is the unscoped-surface concern becoming concrete. It is also the argument for `#[scope]`:
eleven repositories keep a hand-written `delete` only to carry `patient_id`, three of them needing
two extra columns.

### Which check is load-bearing, measured rather than assumed

* `--check main.zm` **does not reach the services.** It is the CLI entry; controllers and services
  hang off `serve.zm`. Four caller mistakes passed `main.zm` and failed `serve.zm` at once.
* Only the running suite could catch the `AS`-label bug, because translation happens at module
  *initialisation*, not at type-check time. Of 26 conversions that was the single mistake no static
  check could see -- and it failed at load with the SQL in the message, which is the property the
  module-level `val` exists for.
* A textual sweep that inserts the pattern it then rewrites will rewrite its own output: `query`
  became `return query(text)` in three files, and `--check` passed it, a self-call being perfectly
  well typed.

### What `eql` did not remove

`stats.zm` and `service/crud.zm` build SQL from **runtime** table and column names, so no entity
query can express them; ten entities are served through `crud.zm` and have no repository at all.
And `recipe_ingredient.save` now pays for a read-back it discards -- accepted, because the
alternative keeps a twenty-column `INSERT` whose silent failure mode is worse than a keyed `SELECT`
inside a transaction that was already writing the whole list.


## A repository function's name is its query (2026-10-28)

`#[repository]` on a bodyless `pub fn` reads the function's own name as its predicate and writes
the body:

```ziman ignore: a signature with no body, which is the whole of what the author writes
#[repository]
pub fn find_by_id_and_patient_id(conn: datasource.Connection, id: Uuid, patient_id: Uuid)
    -> Result<VitalSign, DbError>
```

The full design, written before any of it was built and unchanged by building it, is
[docs/design/derived-queries.md](../derived-queries.md); its §10 holds the nine questions the
implementation settled. What belongs here is the three decisions that are about the *language*
rather than about the feature.

### Binding by name is the reason to build this rather than copy it

Spring Data binds a derived query's parameters positionally against the predicate's terms. So
`findByIdAndPatientId(patientId, id)` -- two `UUID`s the wrong way round -- compiles, boots, and
answers another patient's rows for as long as the application lives. There is no test that is
obviously missing, because the signature is satisfied.

Here the parameters after the connection **must be named for their columns, in that order**, and
the type is checked against the entity's field as well. The transposition is this instead:

```
parameter 1 is named 'owner' and the name's term 1 is 'id' -- a derived query binds its
parameters by name, not by position, so two transposed arguments are this error rather than
the wrong rows
```

It is the same defect `insert_one` and `#[pk]` exist to remove -- a list that agrees with another
list by convention and is checked by nothing -- caught at the one place the two lists are both
visible. 91 of the sample backend's 144 repository functions are derivable, and six name shapes
cover 69 of them, so this is not a rare shape being hardened.

### Two readings is a refusal, not a precedence rule

The tokeniser's dictionary is the entity's own column list, and the obvious implementation takes
the longest column that is a prefix of the remaining text at each position. That is what makes
`find_by_patient_id_and_type` read `patient_id` then `type` rather than `patient` then
`id_and_type` -- and it is also how a resolver surprises people, because a column named
`name_and_type` beside columns `name` and `type` makes `find_by_name_and_type` genuinely two
queries and longest-match silently picks one.

So the implementation **enumerates every parse** rather than committing at each step, and a name
with two of them is refused showing both. It costs nothing measurable (the search is over a
handful of terms against a few dozen columns, once per function at load) and it removes a class of
bug that cannot be found by reading the call site. No column in the sample backend's 36 entities
contains `_and_`, `_in`, `_not`, `_between`, `_asc` or `_desc`, so the refusal cannot fire there
today; the point is what happens to the first schema where it can.

### A bodyless `fn` carrying attribute X is filled by X's generator

The marker is `#[repository]` and not a `derived` keyword, and the reason generalises past this
feature. A bare `derived` never answers *derived from what?*, and it would be the first bodyless
form in the language that does not say what fills it -- `native fn` names the runtime, `builtin fn`
names the type checker. An attribute names the mechanism, and reads against the module attribute as
the inner/outer split already works: `#![repository(VitalSign)]` says *this module is a repository
over VitalSign*, `#[repository]` says *this function is one of its queries*.

That is general by construction without a registry, an owner, a what-if-nobody rule or a
what-if-two rule, which is what docs/ROADMAP.md's code-generation entry asked for and what a
generic `derived` hole would have needed. One consumer exists; a second costs no new grammar.

The cost in the compiler was one line outside the generator. `fn_has_body` used to ask
`!is_native && !is_builtin`, which is a list of the kinds that are bodyless *forever*; a derived
query is bodyless between parsing and generation and has a body afterwards, so it now asks whether
the body is there. The same question, asked about the property rather than about the kinds -- which
is the fix that comment already recommended the first time a third bodyless kind arrived.

### Adoption found the one thing the design could not: the generator writes the error message

Adopted across the sample backend the same day: **30 functions in 19 repositories**, 121
`repository.query` vals down to 97. The first attempt converted more than that and was reverted,
which is the finding.

Two of §5's seven verb/return pairs raise `DbError.NotFound`, so the generator has to write its
text, and it writes `"no Medication matching find_by_id_and_patient_id"` -- the entity's name and
the function's, which is all it has. The application writes `"Medication not found: <uuid>"`, and
that difference is three separate problems: the id is gone (`repository.not_found`'s own comment:
*"a 404 in a log with no id cannot be matched to a request"*), the struct's name is not the
entity's label, and two of the backend's suites assert the exact wording in a check named *"a 404
is worded exactly as Spring words it"*.

**The second of those is the one that matters, because the application had already decided it.**
From the same comment, written long before any of this existed: *"`label` is the entity as a
person says it ("Vital sign", "Medication refill log"), not as the struct spells it … That is why
it is an argument rather than derived from the entity."* A generator reading a `StructDecl` cannot
produce a label that is, by the application's own reasoning, not a fact about the struct.

So `find -> Result<E, DbError>` and `delete -> Result<unit, DbError>` are unused in the backend,
and what converted is the message-free half: `delete -> Result<int>`, `count`, `exists`,
`find -> Result<Option<E>>`, `find -> Result<[E]>`. That is expensive --
`find_by_id_and_patient_id` (12 instances) and the scoped `delete` (15) are two of the six repeated
shapes the feature was counted on.

**The fix is one optional argument on the module attribute**, `#![repository(Medication, label:
"Medication")]`, where a label is a fact about the module's entity rather than about any one
query. Built the same day, in about forty lines: the parser takes `label:`, and the generator
writes `DbError.NotFound(concat("Medication not found: ", uuid.format(id)))` -- the text the
application had written by hand in 31 places. The alternative, a message per function, is the
per-function configuration both the module attribute and the derived-query design refused, and it
would write the text once per query instead of once per entity.

The id is in the message when the predicate has a plain-equality term on the entity's single
`#[pk]` column and that column renders as text (`uuid.format`, `str`, or a string as itself).
Deliberately **not** `format`, which prints a struct as its fields -- `uuid::Uuid { bytes: [17,
17, ..] }` in a 404 body is worse than the label alone, which is what a type with no rendering
gets.

The general lesson is the one the `eql` adoption already recorded in a different shape: a
generator can write a query from a declaration, and it cannot write anything the declaration does
not contain. Prose aimed at a person is the obvious example and was not obvious in advance.


## The second generator kind: a module generator (2026-10-29)

`#[derive(X)]` has one shape -- `generate(d: meta.Decl) -> Result<string, string>`, called once
per declaration -- and all five bundled generators are written to it. `#![repository(Entity)]` is
not that shape, and the attempt to make it one is what produced this entry.

**Why a `Decl` cannot express it.** A repository is generated from three things: the entity, which
is declared in *another file*; the entity's `use` path; and the set of functions the current
module already defines, because a hand-written `find_by_id` must not be replaced by a generated
one. The draft design put `repository_*` fields on a `Module` reachable from `Decl` and it did not
merely read badly -- **it did not work**: in the North Star, `#![repository(VitalSign)]` lives in
`repository/vital_sign.zm` and `#[derive(Entity("vital_signs"))]` in `entity/vital_sign.zm`, so
`Decl.module` for the entity is the *entity's* module, which carries no repository attributes at
all. Every one of those fields would have been `None` at every real site.

So there are now two kinds, and **the entry point a module exports is which kind it is**:

```ziman
pub fn generate(d: meta.Decl) -> Result<string, string>                      // one declaration
pub fn generate_module(t: meta.Target) -> Result<string, string>             // one module
pub fn generate_query(t: meta.Target, f: meta.Function) -> Result<string, string>  // one function
```

The alternative was a second argument to `#![generator(X)]` naming the kind. It was rejected
because the entry point has to exist either way, so the attribute would be a second source of
truth that can disagree with it -- and a disagreement surfaces at the first *use*, in another
file, which is the one thing the attribute exists to prevent (user-generators.md §4). Exporting
both kinds is refused where it is written.

**`meta.Target` is named for what it is and carries only what is read.** Not `Module`: a general
name is where every later module attribute would accrete, and §6 had already taken the other road
for members (`#[pk]` is an entry in `attrs`, not a `pk: bool`). The design proposed six fields and
the port reads four -- `entity`, `label`, `crud`, `defined`. The entity's `use` path is read by
nothing, because generated text spells the bare name and the import that puts it in scope is the
compiler's own work; the list of bodyless functions is a parameter to `generate_query` instead.
That matters more than it sounds: a `Target` hands a generator a declaration from another module
and the current module's function names, which is a far wider grant than anything `meta` gave
before, and a grant is hard to take back once a generator depends on it.

**Two entry points rather than one returning a list of pieces.** A single call answering
everything would collapse every refusal onto the module attribute's position. Today a derived
query whose name cannot be read against the entity is refused *at its own line*, and three broken
queries in one file are three errors rather than the first one -- which is a property of the
per-function loop, so that loop stays in C and each call gets the function's own position.

**What stays in C, and why that is the division rather than a shortfall.** A generator answers a
string. The imports the generated bodies name (`eql`, `datasource`, `DbError`, the entity's own
`_meta`/`_from_row`), the position a refusal is reported at, and the splice into the module's AST
are all operations on the compiler's own data structures, which a function whose only output is
text cannot perform. The five declaration derives are split in exactly the same place --
`import_for_derive` is C beside every one of them. The code that *decides what the text says* is
what moved.

**The keyed CRUD moved first, then the derived queries -- both on the same day.** The two arrived
in one design and are two jobs: nine functions whose bodies follow from the entity (F228), and a
name grammar with a signature check and a SQL builder (F229). The measurement that each is a port
rather than a rewrite is the same one twice: the North Star's 23 repositories and 80 derived
queries generate **12,542 lines of byte-identical text** before and after, and all thirteen
refusal fixtures print byte-identical messages. The only difference anywhere is that a function
the module writes by hand now has its "written by hand" note where the function would have been,
rather than above the block, because the note is part of the text now instead of a `printf`
beside it.

**What that leaves in `src/derive.c`: 947 lines, down from 1,716, and no code generation at all.**
The growable buffer the file opened with existed because every generator was a string builder;
with the last of them in Ziman, the file finds declarations, ensures imports and splices. Two
things stayed that are worth naming because they look like leftovers and are not. The
type-parameter refusal on a derived query is a fact about the declaration's own shape, needs no
entity, and `meta.Function` deliberately carries no type parameters -- a field for the sake of one
check is what §12's guard is against. And the `<fn>_query` val-name collision is a fact about the
*module*, not the entity, so it is checked where the module is.

**One duplication was accepted on purpose.** `standard_flat_type_name` in the C and `flat_names`
in `derive_row.zm` are the same seven-entry table: the generator writes `to_sql(x)` for those
types and the C has to import exactly the name that text calls. They have to agree. It is
survivable because a disagreement is a *missing import*, which is an error at the deriving
declaration -- not a wrong answer at run time -- and the alternative was letting a generator
dictate the module's imports, a wider grant than text.

**A superset where the exact set is no longer known.** The C used to import a SQL converter for
each column a query actually bound, which it knew because it did the name reading. It no longer
does, so it imports one per entity field instead. That is a superset of what any query can bind,
every field of an entity is a column anyway, and an unused `use` costs a load and nothing else --
which is a better trade than keeping a name grammar in C for the sake of an import list.

**One trap worth recording, because it is the third time it has been paid.** The key parameter's
type must be written as the *source* spells it, module qualifier and all: an entity field written
`uuid.Uuid` generates `id: uuid.Uuid`, because that is what resolves where the text lands. The
first version of the ported generator reached for `meta.bare_name` and generated `id: Uuid` --
"unknown struct or enum type 'Uuid'". `bare_name` is for looking a name up in a table of bundled
types, never for writing one out; `src/generators.c`'s `source_name` carries the same warning
from the same mistake, made twice before this.


## A string literal written over several lines (2026-10-29)

A long constant -- a refusal message, a `format` string -- has to be one literal and does not fit
one line. Before this there were two ways to write one and both were wrong in a different way:

* **`concat` of pieces**, which is a call and an allocation at run time to produce something that
  was constant at compile time. It is also *refused* where a literal is required, and `format`'s
  first argument is exactly that: its placeholders are counted against its arguments at compile
  time, which a runtime concatenation cannot support.
* **One very long line**, which is what the alternative forced: `lib/db/derive_repository.zm`
  carried **11 lines over 120 columns, the longest 272**, every one of them a `format` literal
  that had nowhere to break.

So a `\` at the end of a line continues a string literal onto the next:

```ziman
return err(prelude.format(
    "#![repository({})] needs '{}' to mark at least one field #[pk] -- " \
        "a repository's find, update and delete each address one row, and " \
        "without a key there is nothing that addresses one.",
    entity,
    entity
))
```

The pieces are joined **in the lexer**, end to end, with nothing added between them, so what the
parser and the type checker see is a single `TOK_STRING_LIT` -- which is what makes the wrapped
`format` string above legal and keeps its placeholder count checked.

**Why the `\` rather than bare adjacency, which is what C and Python have.** Adjacency reads
better and costs nothing to specify: two literals in a row are one string. It was the first
choice, and it was wrong for one reason found by testing it against this language's own code
rather than by reasoning:

```ziman
val all_nine = ["insert", "update", "find", "find_or_error", "exists", "delete", "find_all",
                "count_all", "delete_all"]
```

Drop the comma after `"find_all"` and bare adjacency compiles it to a five-element array holding
`"find_allcount_all"` -- a generator that emits eight CRUD functions and then looks for one nobody
wrote. **Today that is a syntax error**, and a language that refuses an ambiguous derived-query
name rather than resolving it by precedence (see "A repository function's name is its query")
should not take a silent join here. The restriction that was considered first -- fold only across a
newline -- does not help: a forgotten comma in a *vertical* list is exactly a newline apart, and a
vertical list of strings is the commonest shape there is.

So the cost is one character, paid by the person who wants the fold, and
`tests/lang/string_continuation_missing_comma` pins the error that character buys. Four refusals
are pinned beside it: a `\` after something that is not a string, a `\` with no string after it,
two in a row, and one at the end of the file.

`ziman --fmt` indents a continuation one level past the line it continues, which is the treatment
a line ending in a binary operator already gets and for the same reason -- at the statement's own
level it reads as a new statement. The formatter's own scanner is separate from the lexer, so it
sees the pieces rather than the folded literal, and it never re-joins or re-breaks them: where a
sentence breaks is a judgement about reading, not about width.

## Four documents become four folders (2026-10-30)

`docs/DESIGN.md` had reached **10,449 lines and 1.2 MB** -- roughly 300k tokens, more than fits in
a context window, so neither a person nor a model could read it in one pass. That is not a
tidiness complaint: two documentation errors in this project came out of it, both the same shape. A
claim about what gated the expression work was made twice from a summary note rather than from the
section, which said the opposite; and a memory note was trusted over the file it described. A
document too big to re-read gets quoted from memory, and memory drifts.

| was | now | parts |
| --- | --- | --- |
| `docs/DESIGN.md`, 1.2 MB | `docs/design/journal/` | `principles`, `v0-decisions`, `part-01`..`part-09` |
| `docs/reviews/tasks.md`, 975 KB | `docs/reviews/tasks/` | one per section A-E, F in 7 ranges |
| `docs/ROADMAP.md`, 283 KB | `docs/roadmap/` | one per section |
| `docs/SYNTAX.md`, 250 KB | `docs/syntax/` | 14 chapters, contiguous runs of its sections |

**The split rule is each document's own organising principle, not a uniform one.** The task list is
numbered, so a task is found by its number and F is cut at entry boundaries. The roadmap and the
reference are read by topic, so they split by section, and the reference's chapters are *contiguous*
runs -- the splitter refuses to run if its chapter list is not the document's own order, so no
section can be quietly reordered or dropped.

**Two things only measuring caught.** The first cut used a line budget and produced a 431 KB part,
because a task entry is one very long line: 824 lines of F1-F148 is three times the bytes of 824
lines of anything else. The budget is now 120 KB, which is the unit that actually matters. And the
journal's parts were going to be named for the date each starts at, until the names came out as
"2026-10-10 to 2026-09-28" -- **the log is not in date order**, opening in mid-October and dropping
back to September, with three large steps backwards among its 174 dated headings. Numbered parts
claim no ordering the entries do not have, and the generated index carries the part for every
entry, so nothing navigates by guessing a filename.

**`docs/design/journal/` rather than `docs/design/` directly**, because `check_design_status.py`
globs `docs/design/*.md` and requires a `Status:` line in each -- a feature design says whether it
is built, which a dated entry cannot. One directory deeper and the nine design documents keep that
gate with no exemption list to maintain.

**What was deliberately not rewritten: the dated archives.** The first reference sweep replaced
every `SYNTAX.md` in the repository, which turned a 2026-09-28 heading into "Rationale consolidated
from docs/syntax/" -- naming a path that did not exist on that date. The rule at the top of this
journal is that an entry is true of its date, so the archives (this folder, the task list, and the
review reports, which were restored from `HEAD` after that sweep had already edited them) keep the
paths their authors wrote, and each archive's README says what those names map to now. Living
documents and code comments *were* updated, since a cross-reference there is for following rather
than a record of a day.

**Three tools read these paths, which is the part a move like this actually costs.**
`gen_design_index.py` indexes the folder, discovering parts rather than being told them, and each
link now carries a part as well as an anchor. `check_not_yet.py` opened one `ROADMAP.md` in three
separate functions; those now share a `roadmap_items()` iterator over every part, and it exits
rather than passing if the folder is empty -- a check that silently reads nothing passes for the
wrong reason, which is the same failure F176's empty shadow run was. `check_syntax_examples.py` and
`check_doc_modules.py` glob `docs/syntax/*.md`, so a chapter added later is covered without editing
a list.

Verified lossless against `HEAD`, line by line: 231 journal entries over 8,174 lines, all 37 of the
reference's sections, 155 roadmap list items and 4,428 task lines, with the only differences being
the link depths the move required. 1,347 fixtures, 420 library tests, the North Star's 2,670 checks,
42 reference examples still compiling, and every relative link in all 215 markdown files resolving.
The largest hand-written file is now 200 KB, down from 1.2 MB -- `v0-decisions.md`,
which is one section and splits no further without breaking it, as `roadmap/larger.md` at 152 KB
also does; every part that *was* cut came out at or under the 120 KB budget.
