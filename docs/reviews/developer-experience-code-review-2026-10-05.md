# Ziman developer-experience and code review (2026-10-05)

Reviewer background: backend engineer, 10+ years, Java/Spring, Go and Python. No prior exposure to Ziman.
Method: read the manuals first, wrote my own Ziman to check them, then reviewed the library and the
reference backend, ran both test suites, measured throughput and CPU, and probed hostile inputs against a
live server. Earlier reviews were read only after I had formed my own view.

Findings are tagged **[V]** where I reproduced them and **[O]** where they are judgement.

---

## 0. What I started from

| | |
|---|---|
| `ziman` | `3c0908f` (`coverage: report on stderr, so all three tools follow one rule`) |
| `health-record` (both backends) | `bd664c7` |
| `make test` (ziman fixtures) | **845 passed, 0 failed** — matches the stated baseline |
| `src/test/other/run-all.sh` (SQLite) | **19 suites, 958 checks, 0 failed** |
| `src/test/other/run-all.sh --postgres` | **17 of 19 suites FAILED** — the server cannot start (see B1) |
| Machine | 16 cores, 93 GB RAM, Fedora 43, CPython 3.14.7 |

Both repositories were clean before and after my work; the only untracked file in `ziman` is the
language-design review that ran ahead of me. I started a throwaway `postgres:17` container for a
repro and removed it; the pre-existing `health-record-db` container was left untouched.

---

## 1. Executive summary

1. The language is pleasant to learn and the diagnostics are the best I have seen in a project this size;
   I wrote working non-trivial Ziman on the first attempt from the manuals alone. [V]
2. The backend is genuinely well-layered and security-conscious: no SQL injection reachable, cross-tenant
   isolation correct on every verb, request smuggling closed, role checks enforced server-side. [V]
3. **The PostgreSQL backend is completely broken.** A fresh database cannot be migrated at all — 17 of 19
   test suites fail on `--postgres`. The README claims "Both pass." [V] **P0.**
4. **The collector, not the interpreter, is the server.** 92.9% of server CPU is garbage collection, and
   throughput *falls* as clients are added (peak ~1,500 rps at 4 clients, 763 rps at 64). [V] **P0.**
5. Three numeric defects reach clients: a JSON integer is silently saturated through `int64` into a `float`,
   every subnormal `float` turns a request into a 500, and `float` JSON output diverges from Jackson in
   three ways. [V] **P0/P1.**
6. 64 unauthenticated registrations move RSS from 66 MB to 1.09 GB permanently, with no rate limiting
   anywhere; registration also leaks which emails have accounts, undoing the care taken on login. [V] **P1.**
7. `routes.zm` is 2,328 lines for 132 endpoints with 105 near-identical encoder blocks — the one place the
   layering did not pay off, and the codebase's biggest maintenance liability. [V]
8. The application has **zero** Ziman unit tests, and that is partly the language's fault: a test file
   cannot live outside the source directory because `use` has no `..` and no search path. [V] **P1.**
9. Everything the previous developer-experience review called P0 is fixed and I re-verified all four. The
   loop-speed gap to CPython closed from 28× to 2.3×. [V]
10. **Verdict: a competent team could build this backend in Ziman, and did. They should not run it in
    production yet** — not for the language's ergonomics, which are fine, but for the runtime's throughput,
    its memory behaviour under load, and a Postgres path that does not start.

---

## 2. What changed since the last reviews

`docs/reviews/developer-experience-code-review.md` (2026-09-26) is the baseline for this report.

**Fixed, re-verified [V]:**

| Prior P0 / finding | Now |
|---|---|
| `Instant` with 1–6 fractional digits (`toISOString()`'s `.000Z`) → 400 | All of `.000Z`, `.1Z`, `.123456Z`, `.123456789Z`, bare `Z` → 201 |
| A date near year 9999 persists a row that can never be read or deleted | `startDate: "9999-12-31"` → create 201, read 200, list 200, delete 204 |
| Audit-log forgery by newline injection | Fixed **in the right layer**: `lib/core/log.zm:127` `single_line` rewrites `\n`/`\r` as two characters, so no application can reintroduce it |
| No CORS, preflight → 405 | `OPTIONS` with `Origin: http://localhost:5173` → 204 with `Access-Control-Allow-Origin` |
| Recursion ~1,450 frames segfaults | Clean runtime error, `--stack-mb` tunable |
| Any runtime error kills the whole server | Per-connection `on_error` in `server.serve` |
| Zero SQL indexes | 20 indexes in `V024__add_indexes.sql`, each `(patient_id, <sort column>)` |
| 158 hand-written `Err(e) -> ServiceError.Storage(e)` blocks | **3**, thanks to `#[from]` |
| 591 explicit generic call sites | **335** |
| "A simple loop is about 28× slower than CPython" | **2.3×** on trial division (0.45 s vs 0.20 s) |
| ~500–650 rps for an authenticated read, flat | 801 rps at 1 client, 1,511 at 4 — better, but now *degrades* past 8 (see §7) |

**Still holds, re-verified [V]:**

* A function-valued struct field cannot be called as `x.f(a)`, **and the diagnostic is actively wrong**:
  `'a' is not an imported module ... e.g. render(a, ...)`. The suggested fix does not work; the real fix is
  `val render = a.render`. `service/crud.zm` does this nine times (`:49`, `:71`, `:88`, `:97`, `:154`,
  `:159`, `:163`, `:164`, `:174`). Since `Crud<E, Req, Res>`, `server.Endpoint`, `server.Decoder`,
  `server.Encoder` and `server.Api` are *all* records of function values, this papercut lands squarely on
  the two main extension mechanisms in the codebase.
* A dead spawned thread leaves `receive` blocked forever. Independent corroboration: the
  language-design review that ran before me left a hung `ziman h3_lambda_sb.zm` process behind, still
  running when I arrived.
* The error-type zoo got **worse**, not better: `Result<_, string>` occurrences in `lib/` went from 242
  to **281**.

**Regressed since the last review [V]:**

* PostgreSQL went from working to not starting (B1). `V028` (2026-09-27-ish, commit `701b17f`) and `V029`
  (`885ea9c`) both create `TEXT` foreign keys to columns that `postgres/V025__native_uuid_keys.sql`
  (commit `b061b1e`) had already converted to `uuid`.

**Where I disagree with an earlier review [O]:**

* The 2026-09-26 review says `--lint` effectively covers nothing. That is no longer true and I was wrong to
  assume it too: `--lint` *does* follow project `use` edges. Linting `src/main/ziman/serve.zm` type-checks
  and lints all 152 application files and reports nothing — the application really is lint-clean. I
  verified this with a deliberately dirty helper module imported from a clean entry file; both findings
  surfaced with the helper's own file name.
* The language-design review's §4.7 presents the collector problem as a microbenchmark result. From the
  application end it is worse than a microbenchmark suggests, because it manifests as *negative scaling of
  a real endpoint* (§7), which is the shape an operator will actually meet.

---

## 3. Learnability and ergonomics

### 3.1 Rating the manuals: **A−**

`docs/SYNTAX.md` (2,372 lines) is the best language reference I have read for a project this young. Two
things make it work. First, it gives a reason at every restriction — "no positional construction (`Point(0,
0)`) — with two same-typed fields that's a silent swapped-argument bug" is a sentence that teaches rather
than legislates. Second, the "Semantic rules enforced before a program runs" and "Other limits" sections
collect, in one place, things most languages never write down at all.

The per-module manual under `docs/lib/` is generated from source and exercised by `*_example` fixtures, so
it cannot drift. I checked for dead public API across the whole library and found **none**: every `pub fn`
in `lib/` is referenced by a fixture or a doc example.

The grade is not an A for four reasons, all [V]:

1. **SYNTAX.md and prelude.md disagree about what exists.** `option_unwrap_or`, `result_to_option` and
   `option_to_result` appear in `docs/lib/prelude.md` and **zero** times in `SYNTAX.md`. I wrote my own
   `unwrap_or_zero` helper because the language reference's `Option`/`Result`/`Builtins` sections do not
   mention them. Conversely `map_get_or` is in SYNTAX.md with no `prelude.md` entry. There is no single
   page that lists everything available with no `use`.
2. **The primary `Iter<T>`/`fs.lines` example at SYNTAX.md:371 does not compile** — it uses postfix `?`
   twice. (Confirming the language-design review; verified independently.)
3. **`SYNTAX.md` says "Calendar year is assumed non-negative throughout (no BCE)".** The implementation
   supports negative years consistently: `localdate_of(-1, 1, 1)` is `Ok`, `localdate_to_string` gives
   `-0001-01-01`, `localdate_to_epoch_day` gives `-719893`, and `localdate_is_leap_year(-4)` is `true` —
   all matching `java.time`. The doc is wrong, not the code; the API surface is better than advertised.
4. **Builder signatures are named but not given.** SYNTAX.md lists `mapb_new`/`_set`/`_get`/… without
   types. I guessed `mapb_get` returned a `V` and had to go to `prelude.md` to learn it returns
   `Option<V>`.

### 3.2 What I fought

Writing a realistic 65-line program (group rows, count, sort by a derived key, round-trip JSON) worked on
the **first run**, which is a genuine signal. The friction I hit, in order of how much it cost me:

* **No `mapb_get_or`.** `Map` has `map_get_or`; `MapBuilder` does not, so the standard
  "count into a builder" loop needs an inline `switch` per lookup:
  ```
  val prev = switch mapb_get(counts, m.status) { Some(n) { n } None { 0 } }
  ```
  A one-line asymmetry between the immutable collection and its builder, repeated at every use. [V]
* **No descending sort by a secondary key.** `sort_by_keys_desc` flips *everything*, so "count descending,
  name ascending" means negating the count into a derived key struct — which quietly breaks at `int_min`.
  `sort_with` is the honest answer but then you hand-write the comparator.
* **A global `val` is not reachable as `mod.NAME`, and the error message hides why.**
  `number.float_min_positive` reports `module 'number' has no function 'float_min_positive'`. It *does*
  have it — as a global `val`, which only a per-name import can reach. SYNTAX.md documents the rule; the
  diagnostic contradicts it. [V]
* **The explicit-type-argument tax** is real but has halved since the last review (591 → 335 sites). What
  remains is mostly `collections.map<A, B>` / `sort_by_keys<T, K>`, which I did not mind.

### 3.3 Diagnostics: mostly outstanding, four weak spots

I wrote the nine mistakes a Java/Go developer makes on day one. Five produce messages better than Rust's:

```
'+' doesn't join strings (found string and string) -- use concat(a, b), or format("{}{}", a, b)
'i' is an element of 'a' here ('for i in a' binds each element, not its position), so 'a[i]' indexes
    with a value; to loop over positions write 'for i in 0..len(a)', or use 'for i, item in a' for both
this Result<int, string> is discarded, silently dropping a possible error -- bind it ('val _ = ...'
    to discard on purpose), or use 'try' to propagate the error
```

Four do not, and these are the four *most likely* day-one mistakes [V]:

| Mistake | Message | What it should say |
|---|---|---|
| `a[0] = 9` | `left-hand side of assignment must be a variable name` | `… Ziman has no indexed assignment; write a = set(a, 0, 9)` |
| `p.x = 5` | same generic message | `… write p = with p { x: 5 }` |
| `k++` | `expected an expression, found '+'` | `… Ziman has no ++; write k = k + 1` |
| `Option<int> + int` | `… found Option<int> and int (use 'as' to cast explicitly)` | **wrong advice** — `as` cannot cast an `Option`; it should say `unwrap_Some` / `option_unwrap_or` |

Only one syntax error is reported per run (no recovery), so fixing eight mistakes meant eight compile
cycles. For a language betting on fast iteration this is worth fixing. [O]

### 3.4 Tooling

* **`--locals` is excellent.** On a division-by-zero it printed, per frame, every variable with its value
  (`o: Order = Order { id: 7, total: 100.0, tags: [] }`). That is the information a debugger breakpoint
  gives, with no setup. It is also *only* post-mortem and *only* on a fatal error — you cannot inspect
  state at an arbitrary point, or when a function returns `Err`. There is no stepping, no breakpoints, no
  conditional inspection. [V]
* **`--profile`** is function-level self time with an honest caveat about sample inflation. It immediately
  found the GC problem in the real server, which is the best thing you can say about a profiler. There is
  no call graph and no line attribution, so "which of `routes.zm`'s 2,328 lines" is unanswerable. [V]
* **`--coverage`** works on a long-running server (I SIGTERM'd it after driving requests and got per-file
  line coverage with a `total:` line and the unexecuted line numbers). Three gaps: no machine-readable
  output, so it cannot gate CI; no way to merge runs, and the suite starts 19 separate servers; and it
  counts statements, so `routes.zm`'s 2,328 lines appear as 67 — the route table's correctness is almost
  invisible to it. [V]
* **Discoverability** of all three is fine: they are in `--help` with one-line explanations.
* **`--lint`** has two rules and found a genuine quadratic string build **in the library itself**
  (`lib/time/time.zm:280`, `literal_buf = concat(literal_buf, …)` in `datetime_format_of`). Deliberate —
  a format is compiled once — but it means the project ships a file its own linter flags. Linting a library
  module directly also prints the findings *and then* `no 'main' function found` with exit 1, so you cannot
  lint a non-entry module cleanly. [V]
* **`--fmt`** is idempotent on all 37 library files and on 138 of 152 application files; 14 application
  files would be rewritten (over-long lines the formatter would wrap). `--fmt --check` is evidently not a
  CI gate for the backend. [V]
* **Missing entirely:** a debugger, package management (there is no manifest and no third-party ecosystem
  at all), and any build/dependency caching. For this backend none of that bit, because the library covers
  everything it needs — but that is a property of this application, not of the toolchain.

---

## 4. Code quality of the Ziman application

### 4.1 Layering and separation of concerns [O]: good, with one bad file

The layering genuinely holds. A controller is four lines and knows nothing about JSON or status codes:

```
// controller/allergy.zm:19
pub fn handle_get(ctx: Ctx, principal: Principal, id: Uuid) -> Result<model.AllergyIntoleranceResponse, ApiError> {
    return to_response(ctx, service.get_allergy(ctx, principal.patient_id, id))
}
```

Two decisions carry most of the weight and both are right:

* **`Ctx` as an explicit first parameter** (`common/common.zm:18`). The comment states the real reason — a
  function value carries no state, so there is nothing to inject into — and turns the constraint into a
  virtue: one struct means a new dependency is one field, not a parameter on every function. `ctx with {
  conn: tx }` for a transaction is better than Spring's thread-local `@Transactional` because the scope is
  visible at the call site.
* **One place that maps failures to statuses** (`controller/controller.zm:47`). `from_service_error` is 20
  lines of `switch` and every controller in the codebase goes through it. The comments explain *why* a
  constraint violation gets a generic message (its text can name a table) and why it is not logged (it is
  the caller's doing). This is better reasoned than most Spring `@ControllerAdvice` I have reviewed.

**`routes.zm` is the exception and it is serious.** 2,328 lines, 132 endpoints, 105 `server.encoder<...>`
calls, 107 `json_to_bytes(...)` lambdas. Every endpoint is ~17 lines of which ~12 are mechanical:

```
server.Endpoint<Ctx> {
    method: http.HttpMethod.Get,
    pattern: "/api/users/me",
    handler: server.secure_handler(
        who: authenticated,
        encoders: [
            server.encoder<user_model.UserResponse>(
                application_json,
                fn(value) { json_to_bytes(user_model.user_response_to_json(value)) }
            )
        ],
        run: users.handle_get_me,
        api: api
    )
},
```

What I would extract, in order [O]:

1. **`json_encoder<O>(to_json)`**, the mirror of the `json_decoder<I>` that already exists at
   `routes.zm:84`. That alone removes ~105 four-line blocks — roughly 400 lines — and the risk that one
   endpoint's encoder list drifts from the others'.
2. **`list_encoder<O>(to_json)`**, for the `collections.map` + `Json.JArray` blocks the list endpoints
   repeat.
3. **`crud_endpoints<E, Req, Res>(prefix, crud, controller_fns)`** returning the five entries. The README
   names this as "D item 16's third point, not started" — it is the single highest-leverage remaining
   refactor in the application.

Per-resource LOC against Spring, measured on `allergy` (the CRUD proof of concept): Ziman 296 lines across
five files plus ~90 in `routes.zm` = ~386; Spring 296 across eight files. So **after** the CRUD extraction
Ziman is ~30% heavier, and essentially all of the excess is `routes.zm`. That is a good result and it
argues for finishing the job rather than for abandoning the approach.

### 4.2 `service/crud.zm`: the right idea, three rough edges

Generating five operations from one `Crud` record is exactly right for 16 near-identical resources, and
building the SQL from `table`/`columns` removes the "two 17-placeholder strings that can drift" problem the
header comment describes. Three things I would change [O]:

* **`crud_save` and `crud_update` each do two round trips** (write, then `SELECT * … WHERE id`) to return
  the stored row. On Postgres a single `INSERT … RETURNING *` / `UPDATE … RETURNING *` halves it; SQLite
  has supported `RETURNING` since 3.35. On a pool each round trip is also a separate borrow.
* **`crud_update`'s `WHERE` has no `patient_id`** (`service/crud.zm:101`:
  `"UPDATE {} SET {} WHERE id = $1"`). It is safe today because `crud_update_one` verifies ownership first
  and `id` is the primary key, but the tenant predicate belongs in the statement, not in the caller's
  discipline — it is one `AND patient_id = $2` away from being defence in depth.
* **`crud_delete_one` returns `Result<bool, ServiceError>` and the `bool` is always `true`.** Dead return
  value; `Result<unit, ServiceError>` says what it means.

### 4.3 Validation and error handling [O]: the best part of the application

`service/service.zm`'s `accumulate` / `validation_result` pattern collects *every* broken field instead of
stopping at the first, and the messages are Bean Validation's own wording (`must not be blank`, `must be a
well-formed email address`). The enum word lists match the Spring enums **exactly** — I diffed all six
allergy enums character by character. `#[from]` on `Validation` and `Storage` removed 155 of 158
hand-written conversion blocks.

Two gaps:

* **Email validation is `contains(email, "@")`** (`service/auth.zm:39`). `"@"` alone is accepted on
  `PUT /api/patients/me` (200). Spring's `@Email` rejects it. [V]
* **A broken enum word answers a different body shape from Spring.** Ziman gives
  `400 {"errors": {"type": "must be one of ALLERGY, INTOLERANCE"}}`; Spring's Jackson failure becomes
  `HttpMessageNotReadableException` → `400 {"message": "<Jackson's text>"}`. Ziman's is *better*, but a
  client that reads `message` gets nothing. [V]

### 4.4 Security [V]: strong fundamentals, three real gaps

What I could not break, and tried to:

* **SQL injection.** Seven payloads (`'; DROP TABLE medications; --`, `' UNION SELECT …`, `x'||'y`, …) in
  body fields, query parameters and the login email: no injection, tables intact. The
  "bound parameters only, nothing spliced" design holds end to end. `repository/paging.zm` builds
  `AND type IN ($2, $3, …)` dynamically and still binds every value (`:14`–`:39`); only the table name and
  the `ORDER BY` are interpolated, and both are code constants.
* **Cross-tenant isolation.** B's token on A's medication id: GET 404, PUT 404, DELETE 404, and A's record
  untouched afterwards.
* **Role checks.** Eight admin endpoints as a plain user: all 403; without a token: 401. The role is read
  from the database on every request (`service/security.zm:21`), so a demotion applies immediately — better
  than the usual "it's in the token" shortcut.
* **Request smuggling.** Duplicate `Content-Length` → 400; `Content-Length` + `Transfer-Encoding` → 400;
  no `Host` on HTTP/1.1 → 400; two `Host` → 400; CR inside a header value → 400; `Host : x` → 400; NUL in
  the target → 400; absolute-form target → 400; negative `Content-Length` → 400. This is a better-hardened
  HTTP/1.1 parser than several production frameworks I have audited.
* **Log injection.** Closed at the library layer, which is the right place.
* **Password handling.** Argon2id with `password_needs_rehash` upgrading bcrypt hashes at login;
  constant-time comparison; a dummy verify on every failure path so timing says nothing; secrets masked in
  the startup report (`app.jwt.secret=****`); `JWT_SECRET`/`MFA_SECRET_KEY` required with no default, the
  old public placeholder explicitly refused, and the two forced to differ (`config.zm:131`–`:168`).

The gaps are in §6.

### 4.5 Data access [V]

* **Indexes:** 20, each `(patient_id, <the column the list sorts by>)` — exactly right. Two misses:
  `V029`'s `medication_dose_logs` and `medication_refill_logs` got **no index at all**, on either
  `patient_id` or `medication_id`, while every other table in the schema has one.
* **N+1:** `service/lab_panel.zm:56`–`:64` runs one query for the panels then one per panel for its
  results. Honest assessment: **Spring does exactly the same** (`LabService.findAllPanels` is
  `.stream().map(this::withResults)`), so it is a faithful port rather than a divergence — but it is a
  defect in both, and on Ziman each of those queries is also a separate pool borrow.
* **Ordering:** better than Spring. Spring's `findAllByPatientId` has no `ORDER BY` at all;
  `crud_find_all_by_patient_id` uses `ORDER BY created_at, id`. And `repository/lab_result.zm:115` puts
  NULL `effective_date_time` last with an explicit `CASE`, which makes SQLite agree with Postgres — Spring's
  `…OrderByEffectiveDateTimeAsc` would disagree between the two engines. Careful work.
* **Nullable handling:** `Option<U>` → NULL-or-value through `#[derive(Row)]`, consistently.
* **Transactions:** `service.atomically` (`service/service.zm:78`) acquires an exclusive connection,
  joins rather than nests when already inside one, and `discard`s instead of `release`ing a connection
  that died. That is a more careful pool story than most hand-rolled Java code.
* **Read-modify-write:** most `update_*` services do SELECT-then-UPDATE with no transaction and no version
  column, so a partial PUT can overwrite a concurrent change. I measured it (15 of 30 trials lose an
  explicit `active: false` to a concurrent PUT that omits `active`) but I do **not** count it as a defect:
  Spring's `AllergyIntoleranceService.update` is not `@Transactional` either, and last-writer-wins is the
  documented semantics of a merging PUT. It is a shared structural risk, not a Ziman bug. [O]

### 4.6 Configuration and migrations [V]

Configuration is the strongest non-obvious part of the application. `config.zm` validates fourteen values
with messages that name the key (`app.jwt.secret: must be the base64 of at least 32 bytes`), `check_unknown`
refuses a typo'd key rather than silently defaulting, and `startup_report` prints every effective value with
its source and masks secrets. `ZIMAN_RESOURCE_PATH` as a classpath analogue lets the Maven-shaped layout work
without the application knowing about it. I would add two checks it lacks: `server.idle-timeout-ms` and
`server.port` are unvalidated.

Migrations run in one transaction with a checksum and refuse an edited file. I verified the all-or-nothing
property the hard way: when B1 fails, **zero** tables exist afterwards. Good design, wrong content (B1).

### 4.7 Testing [V]

19 Python suites, 3,881 lines, 958 checks, driving a real server over HTTP against a fresh database each.
I mapped all 75 distinct route patterns in `routes.zm` against the suites: **every one is exercised**
(`resources_test.py` drives twelve resources from one table, which my first naive grep missed). The tests
check things curl cannot: the JWT with an independent HMAC, TOTP with an independent implementation,
that the stored MFA secret is not the plaintext, the bcrypt upgrade, the lockouts, the replay guard.

The gap is categorical: **there are zero `#[test]` functions in the application.** Every assertion goes
through HTTP, so nothing unit-tests `service/audit.zm`'s `parse_event`/`tail_of`, the pregnancy
gestational-age arithmetic, the mappers, or `service/crud.zm`'s SQL builders. I proved this is a
self-inflicted-but-excusable gap:

* `ziman test` works on application modules **today**, with no setup — I dropped a three-test file beside
  the services and got `3 passed, 0 failed` in one command.
* But a test file **cannot** live in `src/test/ziman/`: `use service/service` resolves relative to the entry
  file only, `use ../..` is a syntax error (`expected a module path segment, found '..'`), and
  `ZIMAN_RESOURCE_PATH` governs `fs.resolve`, not module resolution. So the only way to unit-test is to put
  `*_test.zm` next to production code — which is exactly what the Maven-mirroring layout the README is
  proud of forbids.

The README's claim that `src/test/ziman/` is "reserved for Ziman-language tests … once ziman's own
`ziman test` exists" is stale; it exists.

### 4.8 Consistency with the Spring API [V]

Real behavioural divergences, beyond §4.3's two:

| | Spring | Ziman |
|---|---|---|
| 404 message | `Medication not found: <uuid>` | `medication not found: id <uuid>` (case, and an extra `id `) |
| `HEAD` on a GET route | 200, headers only (Tomcat synthesises it) | **405 Method Not Allowed** |
| `"measurementValue": "70.5"` (a string) | accepted — Jackson coerces a String to `Double` | **400 `expected a number`** |
| `measurementValue: 1.0` in a response | `1.0` | `1` |
| `measurementValue: -0.0` | `-0.0` | `-0` |
| `measurementValue: 1e21` | `1.0E21` | `1e+21` |
| 413 / 431 / 408 error body | `{"timestamp","status","message"}` | `{"status","error"}`, and **no CORS or HSTS headers** |
| `?type=` (empty) | treated as absent | 400 `must be one of …` |

Several of these are one-line fixes; the 413 one is architectural (B6).

Also stale in the application README, all [V]: the Scope section still says "**no real HTTP** … handlers are
plain functions standing in for one" (there is a full HTTP server); "Not done: TLS …, **authentication**, and
any rate limiting" (authentication is done); "Both pass" for the two test modes (Postgres does not); and the
login-lockout paragraph says an unknown email is throttled "real or not" — in the code
(`service/auth.zm:87`–`:91`) the unknown-email path returns without recording anything. The last one is
harmless (both answers are identical 401s with matching timing) but the sentence is wrong.

---

## 5. Code quality of the library written in Ziman

16,424 lines across 37 modules. Overall [O]: this reads like library code written by someone who has
maintained libraries. Header comments explain the design, not the mechanics; limits are stated; every
public function has a doc example that is executed as a fixture.

**What is good:**

* `lib/http/server.zm`'s typed-endpoint layer is the reason the application's controllers are four lines.
  Nine builders crossed over `public_`/`secure_`, with 415/406/400/401/403 all answered before the handler
  runs, is a genuinely good API shape — and it is implemented in ordinary Ziman on top of `net`.
* `lib/db/datasource.zm`'s pool: `refuse_on_pool` turns "a transaction on a pool silently does the wrong
  thing" into a compile-time-adjacent error with the fix in the message; `rows` on a pool wraps the
  iterator so `iter_close` releases the borrow; `settle`/`discard` replace a dead connection rather than
  returning it. The 30-second idle health check is the detail most hand-rolled pools omit.
* `lib/core/log.zm`'s `single_line` fixes a class of bug (log forging) in the one place it can be fixed.

**Where the language forced awkward code [V]:**

* **Every function-valued field needs a rebinding line.** `lib/http/server.zm:1393` (`val h = e.handler`)
  and nine places in the application. The library's own comment in `service/crud.zm:26`–`:29` documents the
  workaround as a convention, which is the right response to a language limitation — but it is 10+ lines of
  pure ceremony across the two codebases, and the diagnostic that teaches it is wrong.
* **`concat`-in-a-loop** at `lib/time/time.zm:280`, flagged by the project's own `--lint`. Defensible
  (compiled once per pattern) but it should be a `StringBuilder` or carry a `// lint:` style note.

**Duplication and inconsistency [V]:**

* **Two 80-line URL parsers.** `db.parse_url(url, prefixes, default_port) -> Result<DbUrl, DbError>`
  (`lib/db/db.zm:219`, 82 lines) and `client.parse_url(url) -> Result<Url, string>`
  (`lib/http/client.zm:103`, 80 lines) both parse scheme/userinfo/host/port/path with different result and
  error types. A `url` module with one parser and two adapters removes ~80 lines and a divergence risk.
* **Two `percent_decode`s** with different signatures *and* different error types:
  `db.percent_decode(s) -> Result<string, DbError>` vs
  `http.percent_decode(s, plus_is_space) -> Result<string, string>`.
* **`Map` vs `MapBuilder` asymmetry**: `map_get_or` exists, `mapb_get_or` does not.
* **281 `Result<_, string>`** against five structured error enums. This is the library's biggest
  consistency problem and it grew since the last review.

**API-clarity smell [O]:** `datasource.query_one` is documented as "the first row, or None" but
implemented as `query` (every row) then `rows[0]`. Every call in the application is on a primary or unique
key so nothing is hurt today, but the name invites `LIMIT 1` semantics it does not have.

**Missing tests:** none that I could find at the API level — no unreferenced `pub fn`. What is untested is
*adversarial* numeric input: B2, B3 and B4 below are all in native code paths that the fixtures exercise
only with well-behaved values.

---

## 6. Bugs

### B1 — PostgreSQL cannot be migrated from scratch. **Severity: High.** [V]

**Reproduction**

```sh
docker run -d --name t -e POSTGRES_USER=health -e POSTGRES_PASSWORD=test -e POSTGRES_DB=health \
  -p 127.0.0.1:18479:5432 postgres:17
cd health-record-backend-ziman
ZIMAN_RESOURCE_PATH="$PWD/src/main/resources" SERVER_PORT=18471 PROFILE_ACTIVE="" \
DATASOURCE_URL="postgres://health:test@127.0.0.1:18479/health" \
JWT_SECRET="$(openssl rand -base64 32)" MFA_SECRET_KEY="$(openssl rand -base64 32)" \
ziman src/main/ziman/serve.zm
# cannot migrate the database: database error: foreign key constraint
#   "user_preferences_user_id_fkey" cannot be implemented
```

`src/test/other/run-all.sh --postgres`: **17 of 19 suites fail** with that message (the two that pass,
`lifecycle_test.py` and `migration_timestamps_test.py`, do not need a migrated schema). No tables are
created at all, because `migrate.run` is one transaction.

**Root cause.** `postgres/V025__native_uuid_keys.sql` converts `users.id`, `patients.id` and every
`patient_id` to the native `uuid` type. `V028__create_user_preferences.sql:8` then declares
`user_id TEXT NOT NULL UNIQUE REFERENCES users(id)`, and Postgres refuses a `text` → `uuid` foreign key:

```
ERROR:  foreign key constraint "p_user_id_fkey" cannot be implemented
DETAIL: Key columns "user_id" and "id" are of incompatible types: text and uuid.
```

`V029__extend_medications_and_add_logs.sql:18`–`:30` has the identical defect twice
(`medication_id TEXT … REFERENCES medications(id)`, `patient_id TEXT … REFERENCES patients(id)`); V028
simply fails first. SQLite does not type-check foreign keys, which is why the default test mode is green.

**Fix.** Add `migrations/postgres/V028…` and `migrations/postgres/V029…` using `uuid` for those columns
(the mechanism already exists and is already used for V025), and add `--postgres` to whatever runs the
suite — this regression survived two commits and a release of the README because nothing runs it.
**Effort: ~1 hour, plus a CI job.**

### B2 — `float_from_json` saturates a large JSON integer through `int64`. **Severity: High.** [V]

```
POST /api/me/body-measurements {"type":"BODY_WEIGHT","measurementValue":123456789012345678901234567890,
                                "effectiveDateTime":"2026-01-01T00:00:00Z"}
→ 201, "measurementValue": 9.223372036854776e+18
```

Library-level:

```
{"v": 123456789012345678901234567890} -> float 9223372036854776000.0
```

The correct `float` is `1.2345678901234568e29`, which a double holds exactly fine. SYNTAX.md promises
"`float_from_json` (any number, rounding a bigint/decimal like a cast would)" and documents
`bigint as float` as saturating to *float's* limits. The implementation evidently goes
`bigint as int` (saturating at `INT64_MAX`) and then to `float`. For a numeric API this is **silent data
corruption**, and on a medical measurement it is the worst kind. **Fix:** convert `Number.Bigint` to
`float` directly. **Effort: small.**

### B3 — `parse_float` rejects every subnormal, turning a client value into a 500. **Severity: Medium.** [V]

```
POST /api/me/body-measurements {"measurementValue": 1e-320, ...}  → 500 "An unexpected error occurred"
server log: ERROR database error: bad float from driver: 9.9998886718268301e-321
```

```
parse_float("1e-320")                   = Err cannot parse '1e-320' as float
parse_float("9.9998886718268301e-321")  = Err
parse_float("5e-324")                   = Err          // this is number.float_min_positive
parse_float(str(float_min_positive))    = Err          // str/parse do NOT agree
```

**Root cause:** `src/prelude.c:96` treats `errno == ERANGE` as a parse failure. glibc's `strtod` sets
`ERANGE` on gradual underflow, i.e. for every subnormal result, even though it returns the correct value.
So `str()` produces text that `parse_float` cannot read back — which breaks SYNTAX.md's "All four agree"
and its "shortest decimal text that reads back exactly", and makes `number.float_min_positive` (documented
as "smallest positive nonzero, denormalized") unusable. The SQLite driver's text→float path hits it, which
is the 500.

**Fix:** on `ERANGE`, accept when the result is finite and non-zero (a subnormal) and reject only true
overflow (`±HUGE_VAL`) or underflow to zero from non-zero input. **Effort: small.**

### B4 — `float` JSON output diverges from Jackson three ways, and loses `Number`'s kind. **Severity: Medium.** [V]

```
                  print/str        json_to_string
1.0               1.0              1              (Jackson: 1.0)
100.0             100.0            100            (Jackson: 100.0)
-0.0              -0.0             -0             (Jackson: -0.0)
1e21              1e+21            1e+21          (Jackson: 1.0E21)

json_to_string(float_to_json(1.0))      = {"v":1}
re-parsed                               → Number.Int(1)      // was Float(1.0)
```

`src/native_json.c:519`'s `put_float` starts at 15 significant digits; `src/value.c:618`'s
`float_shortest_text` finds the genuinely shortest form and always emits a `.` or `e`. So the *same* double
prints two different ways depending on which function you call, and a `Json` tree is **not** idempotent
through text at the `Number` level — which is the one thing the `Number` enum exists to preserve.
**Fix:** have `put_float` reuse `float_shortest_text`'s search and keep its "always a `.` or `e`" rule for
non-exponent output. **Effort: small.** Note this is an API-compatibility change for existing clients.

### B5 — Control characters other than CR/LF reach the log file. **Severity: Low.** [V]

```python
POST /api/auth/login {"email":"a\u001b[2Jb@x.com\u0007", "password":"q"}
→ 401; the log file now contains the raw ESC (0o33) bytes
```

`lib/core/log.zm:127`'s `single_line` strips only `\n` and `\r`. An operator who `cat`s or `tail`s the log
gets terminal-escape injection (screen clearing, cursor control). `/api/admin/logs` is safe because JSON
escapes it. **Fix:** escape every byte below 0x20 in `single_line`. **Effort: trivial.**

### B6 — Protocol-level rejections answer a different body shape and skip CORS and HSTS. **Severity: Medium.** [V]

```
413  {"status":413,"error":"request body too large"}        — no timestamp, key is "error" not "message"
431  {"status":431,"error":"request headers too large"}
408  {"status":408,"error":"request timed out"}
```

and the response carries neither `Access-Control-Allow-Origin` (even with an allowed `Origin`) nor
`Strict-Transport-Security`, because `server.serve` calls `error_response(e)` (`lib/http/server.zm:626`)
directly and the application's `routes.route` — which adds both — is never reached. A browser SPA on an
allowed origin that uploads an oversized body therefore sees an opaque network error instead of a readable
413. **Fix:** give `ServeOptions` a `fault: fn(int, string) -> Response` hook so a protocol-level rejection
goes through the application's own renderer, as `dispatch` already does. **Effort: small, library-side.**

### B7 — `HEAD` on a `GET` route answers 405. **Severity: Low–Medium.** [V]

```
HEAD /health HTTP/1.1  →  405 Method Not Allowed, Allow: GET
```

`server.dispatch` (`lib/http/server.zm`) matches `e.method == req.method` exactly with no HEAD→GET
fallback, so every route table built from the typed-endpoint API is HEAD-less. The library already does the
hard half — `write_response` suppresses a HEAD body and keeps the `Content-Length` — so this is one
condition in `dispatch`. RFC 9110 says a server should support HEAD wherever GET is supported, and
Tomcat does it for free. **Effort: small.**

### B8 — `medication_dose_logs` and `medication_refill_logs` have no indexes. **Severity: Low.** [V]

Every other table in the schema got a `(patient_id, <sort column>)` index in `V024`; `V029`'s two new
tables got none, on either `patient_id` or `medication_id`, while
`repository/medication_dose_log.zm` queries by both. **Fix:** one migration. **Effort: trivial.**

### B9 — A runtime error in a spawned thread hangs its peers forever. **Severity: Medium (language).** [V]

```
fn worker(out: Channel<int>) { val a = [1, 2]  print(a[5])  send(out, 1) }
fn main() { val out: Channel<int> = channel_new()  spawn worker(out)  print(receive(out)) }
→ runtime error printed, "the thread started by 'spawn worker' at 9:5 ended; the program goes on",
  then hangs until killed (exit 124 under timeout 5)
```

`spawn worker(out) on_error os.fatal` fixes it (exit 1). My developer-facing addition to this known
finding: the previous reviewer's own scratch process was still hung when I started, 12 hours later, which
is what the default does in practice. The `Channel<T>` design — one symmetric type, no sender/receiver
split — is what makes "no sender left" undetectable, so this is a consequence of a deliberate simplification
rather than an oversight. **Fix options, in order of cost:** make `--lint` flag a `spawn` without
`on_error` whose channel the parent `receive`s; or default `on_error` to `os.fatal`; or add a
`channel_close`-on-thread-death link. **Effort: small for the lint rule.**

### B10 — smaller items [V]

* `service/auth.zm:130` binds `failure` and never uses it (`if Err(failure) = saved { log.warn(…, "…") }`).
* `config.zm:97` binds `val checked` from `check_unknown` and never uses it.
* `routes.zm`'s `with_cors` adds `Vary: Origin` only on the allowed-origin path, so a response produced for
  a disallowed or absent `Origin` is cacheable without it.
* A bare-LF request line is held for `header-timeout-ms` and answered 408 rather than 400.
* `login` promotes `NEEDS_LOGIN` → `ACTIVE` (`service/auth.zm:135`) *before* the MFA second factor is
  verified.
* 14 of 152 application files are not `--fmt`-clean.

---

## 7. Security issues (listed separately)

### S1 — Account enumeration through registration. **Severity: Medium.** [V]

```
POST /api/auth/register {"email":"ada@example.com", …}  → 400 {"errors":{"email":"Email already in use"}}
POST /api/auth/register {"email":"nobody-new@…",   …}  → 200
```

The login path was hardened with real care — unknown email and wrong password both answer the same generic
401, a dummy Argon2id verify keeps the timing identical, and the provider is deliberately not named — and
then registration gives the same information away directly, unauthenticated and unthrottled. Spring does
the same, so it is not a divergence, but it makes the login hardening decorative. **Fix:** the clean answer
is email verification (answer 200 either way and send a mail); the cheap answer is to rate-limit
`/api/auth/*` per source address.

### S2 — No rate limiting anywhere, and the register path is memory-expensive. **Severity: Medium (availability).** [V]

```
30 sequential registrations: 30 × 200 in 0.8 s
64 concurrent registrations: 64 × 200 in 0.31 s
server RSS: 66 MB fresh → 1,088 MB after the burst → 1,166 MB after a second burst, then flat
```

RSS **never returns**: still 1,166 MB 30 seconds later and after 2,000 further GETs. Argon2id at
`m=19456 KiB` × 64 concurrent connections is ~1.2 GB of transient allocation that glibc does not give back.
It plateaus, so it is arena retention rather than an unbounded leak — but a single unauthenticated burst
permanently raises the footprint 17×, which in a container with a 512 MB limit is an OOM kill. The README
states "Not done: … any rate limiting", so this is known; the memory consequence is what makes it urgent
rather than cosmetic. **Fix:** bound concurrent password hashing with a channel semaphore (4–8 permits),
and/or rate-limit `/api/auth/*`. A semaphore is ~15 lines given `Channel<T>`.

### S3 — Tokens are not bound to an issuer or audience. **Severity: Low–Medium.** [O]

`service/security.zm:40` and `service/token.zm:53` use `jwt.verify(token, key)`. The library offers
`jwt.verify_claims(token:, key:, issuer:, audience:)`, and SYNTAX.md explains that this is "what stops a
token for one service being accepted by another sharing the key". Spring omits `iss`/`aud` too, so there is
no divergence — but if this key is ever shared with another service, or reused between environments, any of
them will accept the others' tokens. **Fix:** issue and require `iss`/`aud`; switch to `verify_claims`.

### S4 — Terminal-escape injection into the operator's log. **Severity: Low.** [V] (= B5)

### S5 — Observations that are *not* problems, recorded so they are not re-litigated [V]

No SQL injection (7 payloads × 3 surfaces). No cross-tenant read, write or delete. Role checks
server-side and live. Request smuggling closed on nine vectors. Password hashing, rehashing and
constant-time comparison correct. Secrets required, placeholder refused, the two keys forced to differ,
masked in the startup report. Log-line forgery closed in the library. Constraint-violation text never
reaches the client. 403-before-body for `PASSWORD_RESET` accounts. Token version invalidation on password
change. MFA secret encrypted at rest with a separate key, TOTP replay guarded, lockouts on both login and
MFA. This is a better security posture than most Spring backends I have reviewed at this stage.

---

## 8. Productivity and maintainability verdict

### 8.1 Measurements

**Request throughput and scaling** (same 16-core box, `ab`, server on in-memory-file SQLite, pool 4):

| clients | `GET /api/me/medications` (authenticated) | `GET /health` | 404 (no DB, no auth) | `POST /api/me/medications` |
|---|---|---|---|---|
| 1 | 801 rps | 1,559 | 1,381 | 439 |
| 4 | **1,511** | — | — | **694** |
| 8 | 1,409 | **2,628** | **3,042** | 657 |
| 16 | 939 | — | — | 571 |
| 32 | 897 | 1,894 | 1,981 | 473 |
| 64 | 763 | — | — | — |

Every column has the same shape: a peak at 4–8 clients, then decline. At 64 clients the authenticated read
is **slower than at 1 client**. Lowering the thread bound *raises* throughput:

| `server.max-connections` | rps at 8 clients |
|---|---|
| 4 | 2,685 |
| 8 | 2,468 |
| 64 | 2,286 |

`--profile` on the live server under 4,000 requests says why:

```
profile: 3489 samples at 1000Hz, over 7.27s of CPU time
   92.9     3243  (garbage collection)
    0.8       28  (not in Ziman code: startup, typecheck)
    0.7       25  server::read_request
```

Single-threaded, the same interpreter spends only 12% in GC on an allocation-heavy JSON loop, so this is
specifically the cost of collecting across many connection threads. **This is the single finding that
decides whether Ziman can host a real backend**, and it confirms the language-design review's §4.7 from
the application end. For calibration, `0.69 ms` of server CPU per `/health` request, and Python's stdlib
`http.server` — a byword for slow — does 2,770 rps at 1 client and 3,503 at 8 on the same box, i.e. **the
same order as Ziman, and with better scaling.** Spring Boot on this hardware would be 10–20k rps; Go 100k+.

**CPU-bound micro-benchmark against Python** (CPython 3.14.7):

| benchmark | Ziman | Python | ratio |
|---|---|---|---|
| primes below 300,000 by trial division | 0.44 / 0.46 s | 0.19 / 0.20 s | **2.3× slower** |
| 200k `format` + MapBuilder counts, then 200k `sb_append` | 0.18 s | 0.05 s | **3.6× slower** |
| 60k `parse_json` + `json_to_string` round trips | 0.15 s | 0.16 s | **parity** |

The third row is the interesting one: where the work is in native C (`json`, `regex`, `crypto`), Ziman
matches CPython; where it is in interpreted Ziman, it is 2–4× behind. That is a *good* result for a
tree-walking interpreter — the last review measured 28× — and it means the problem is not the interpreter.

### 8.2 How fast could a team build and maintain this?

**Building: faster than I expected, slower than Spring.** [O] The evidence is on disk: 33 resources, 132
endpoints, auth, MFA, admin, audit, broadcasts, paging and migrations, in 14,058 lines, lint-clean,
`--check`-clean, with 958 passing integration checks. Per resource, after the `Crud` extraction, Ziman is
~30% more code than Spring, and essentially all of the excess is `routes.zm`. There is no annotation magic
to learn, no classpath, no reflection, no Hibernate session semantics — the things a new team member
usually spends a month on in Spring simply do not exist. Against that: no package ecosystem, so anything
the bundled library does not cover you write yourself; and no debugger, so you debug with `print`,
`--locals` and `--profile`.

I would estimate **1.3–1.8× Spring's effort** for a backend this library covers, and **3×+** for one it
does not (file upload, OAuth, message queues, S3, OpenAPI generation — none of which exist).

**Maintaining: better than Spring in the small, worse in the large.** [O] Reading an unfamiliar Ziman
function is easier than reading an unfamiliar Spring one, because there is no action at a distance:
`distance_squared(a, b)` cannot mutate `a`, `from_service_error` is the only status mapper, and there is no
`@Transactional` on a proxy you have to remember. Exhaustive `switch` means adding a `ServiceError` variant
breaks every consumer at compile time, which is the single property I most want in a maintained codebase.

What will hurt at scale: `routes.zm` growing linearly at 17 lines per endpoint; no way to unit-test without
putting tests in the production directory; `--coverage` with no CI gate; and the fact that the only
polymorphism available — a record of function values — is also the thing the language makes awkward to call.

### 8.3 Top adoption risks [O]

1. **The collector.** Negative scaling and a 17× permanent RSS jump from one unauthenticated burst. Nothing
   in the language or library layer changes this.
2. **One engine actually works.** A Postgres path that cannot start, undetected across two commits, says
   the second backend is not really in the test loop. Any production deployment uses Postgres.
3. **Bus factor and ecosystem.** One implementation, one author, no package manager, no third-party
   libraries. Every dependency is a feature request.
4. **Numeric edge cases in the native layer.** B2 and B3 are the kind of bug that does not show up in
   tests written by the implementer, and B2 corrupts data silently.
5. **No debugger.** Survivable for a team that writes tests; painful for incident response.

What I would *not* worry about: the language's learnability, the diagnostics, the layering, the HTTP
parser's security, or the SQL safety. Those are solved.

---

## 9. Prioritised action list

### P0

| | Item | Rationale | Effort |
|---|---|---|---|
| 1 | **Fix the Postgres migrations (B1) and add `--postgres` to CI** | The production engine cannot start; it regressed silently | 1 h + 1 CI job |
| 2 | **Fix `float_from_json`'s bigint path (B2)** | Silent data corruption on a medical measurement | S |
| 3 | **Fix `parse_float` on subnormals (B3)** | A legitimate client value becomes a 500; breaks the documented `str`/`parse` agreement | S |
| 4 | **Collector work: per-thread heaps, or stop allocating per iteration** | 92.9% of server CPU; throughput falls as clients are added | L |
| 5 | **Bound concurrent password hashing; rate-limit `/api/auth/*` (S2)** | One unauthenticated burst raises RSS 17× permanently | S |

### P1

| | Item | Rationale | Effort |
|---|---|---|---|
| 6 | `ServeOptions.fault` hook so 413/431/408 use the application's error shape and get its headers (B6) | A browser client cannot read the body of a protocol-level rejection | S |
| 7 | Make module resolution reach a sibling test directory (a `..` segment, or a module search path) | Unblocks `src/test/ziman/`; today the application has **zero** unit tests | M |
| 8 | Extract `json_encoder<O>`, `list_encoder<O>` and `crud_endpoints` from `routes.zm` | Removes ~600 of 2,328 lines and the drift risk across 105 copies | M |
| 9 | Align `float` JSON output with `print`/Jackson (B4) | API divergence in three forms; `Number` kind lost on a round trip | S |
| 10 | Fix the function-valued-field diagnostic | It gives advice that does not work, on the codebase's main extension mechanism | S |
| 11 | HEAD→GET fallback in `server.dispatch` (B7) | RFC conformance; free for every Ziman server | S |
| 12 | Escape all control bytes in `log.single_line` (B5/S4) | Terminal injection into the operator's log | Trivial |
| 13 | Index `medication_dose_logs` / `medication_refill_logs` (B8) | The only two tables in the schema without one | Trivial |
| 14 | Add `option_unwrap_or`/`result_to_option`/`option_to_result` to SYNTAX.md; fix the `fs.lines` example; fix the BCE claim | Three measurable "I wrote it myself because the reference didn't say" cases | S |
| 15 | Update the backend README (Scope, "Both pass", "Not done: authentication", the lockout sentence) | Four statements are now false; a reader learns the wrong system | S |
| 16 | `--lint` rule: a `spawn` without `on_error` whose channel the parent `receive`s (B9) | Turns a silent forever-hang into a warning | S |

### P2

| | Item | Rationale | Effort |
|---|---|---|---|
| 17 | Machine-readable `--coverage` output (lcov or JSON) and a way to merge runs | Without it coverage cannot gate anything | M |
| 18 | Teach the diagnostics to name the fix for `a[i] = x`, `p.f = x`, `k++`, and `Option + int` | The four most likely day-one mistakes get the four weakest messages | S |
| 19 | One `url` module; one `percent_decode` | ~160 lines of duplicated parsing across two modules, two error types | M |
| 20 | `mapb_get_or` (and audit `Map`/`MapBuilder` for other asymmetries) | Removes an inline `switch` from every counting loop | Trivial |
| 21 | `INSERT … RETURNING *` / `UPDATE … RETURNING *` in `crud_save`/`crud_update` | Halves the round trips and pool borrows on every write | S |
| 22 | `AND patient_id = $2` in `crud_update`'s `WHERE`; `crud_delete_one -> Result<unit, _>` | Defence in depth; a dead return value | Trivial |
| 23 | `@Email`-grade email validation; align the 404 message with Spring; accept a numeric string where Spring does | Three small API divergences | S |
| 24 | Error-type consolidation in the library (281 `Result<_, string>`) | The one axis that got worse since the last review | L |
| 25 | Validate `server.idle-timeout-ms` and `server.port`; `--fmt --check` in CI; remove the two unused bindings | Housekeeping | Trivial |
| 26 | Clarify `datasource.query_one`'s doc (it runs the whole query) | The name promises `LIMIT 1` | Trivial |
| 27 | Reduce syntax-error batching to more than one error per run | Eight mistakes cost eight compile cycles | M |

---

## Appendix: experiments and commands

All scratch files were written under `/tmp/ziman_review_dev/`; nothing was written inside either
repository except this report. Servers were run on ports 18470–18476 and all were stopped; my throwaway
`postgres:17` container (`zrev-pg`) was removed; the pre-existing `health-record-db` container was left
running untouched.

### A. Baselines

```sh
git -C ziman rev-parse --short HEAD                      # 3c0908f
git -C health-record rev-parse --short HEAD              # bd664c7
cd ziman && make test                                    # 845 passed, 0 failed
cd health-record-backend-ziman && src/test/other/run-all.sh
#   19 suites PASS (admin 61, allergy 31, audit 22, auth 56, broadcast 22, cors 11,
#   headers 18, immunization 26, lab 30, lifecycle 15, logrotation 7,
#   medication_details 27, mfa 38, migration_timestamps 6, password 19,
#   resources 483, rules 41, testdata 24, user_profile 21) = 958 checks; sqlite: 0 failed
src/test/other/run-all.sh --postgres                     # postgres: 17 failed
```

### B. Reading the manuals, then checking them

* `docs/SYNTAX.md` (2,372 lines), `docs/DESIGN.md` (first 130 lines of principles + targeted sections),
  `docs/lib/README.md` and the per-module pages, `docs/ROADMAP.md` headings, `./ziman --help`.
* `learn1.zm` — 65 lines: `#[derive(Json)]`, `#[derive(Ord)]`, `MapBuilder`, `collections.map`/`fold`/
  `sort_by_keys`, lambdas, `Option`. **Ran correctly on the first attempt.**
* `learn2.zm` / nine single-mistake files — day-one error-message quality (§3.3).
* `iterex.zm` — SYNTAX.md:371 verbatim → `syntax error: there is no postfix '?'`.
* `ff.zm` — function-valued struct field → the misleading "not an imported module" message.
* `d1.zm` — `localdate_from_string("-0001-01-01")`, `localdate_of(-1,1,1)`, `localdate_is_leap_year(-4)`:
  all consistent, contradicting SYNTAX.md's no-BCE claim.
* `f1.zm`–`f4.zm` — `float_from_json`, `float_to_json`, `parse_float`, `json_number` kinds (B2, B3, B4).
* `loc.zm` — `--locals` on a division by zero.
* `hang.zm` / `hang2.zm` — `spawn` + `receive` deadlock, and `on_error os.fatal` (B9).
* `lintcheck/` — proved `--lint` follows project `use` edges (correcting my own and an earlier assumption).
* `zt/t.zm` — proved a test file outside `src/main/ziman` **cannot** import the application's modules
  (`use ../..` → `expected a module path segment, found '..'`; `ZIMAN_RESOURCE_PATH` has no effect).
* A three-test `#[test]` file placed beside `service/service.zm`: `ziman test` → `3 passed, 0 failed`
  (file deleted; `git status` clean).

### C. Hostile input against a live server (`probe.py`, `t1.py`–`t5.py`)

* 14 `dateOfBirth` values (year 0, year −1, 99999, Feb 30, an instant where a date is wanted, `null`, …).
* 14 paging/query-parameter values (`page=-1`, `size=0`, `size=201`, `page=abc`, `size=1.5`,
  `page=9223372036854775807`, `page=&size=`, duplicate `page`, `size=+5`).
* 7 UUID path parameters (`../../etc/passwd`, `%2e%2e%2f`, 32 hex digits, trailing `x`).
* 13 `measurementValue` values including `1e-320` (→ 500), a 30-digit integer (→ saturated),
  a numeric string (→ 400 where Spring accepts).
* 12 timestamp forms (`.000Z`, `.1Z`, `.123456Z`, `.123456789Z`, `+05:30`, zoneless, hour 24, Feb 30,
  year 99999, year −1).
* Media type and body faults: wrong `Content-Type` (415), no `Content-Type` (415),
  `Accept: application/xml` (406), 1.1 MB body (413), 17 KB header (431), `{not json` (400),
  empty body (400), `null` (400), `[1,2]` (400), 600-deep nesting (400 at depth 513).
* 21 SQL-injection payloads across body fields, query parameters and the login email: no injection.
* 15 raw-socket HTTP vectors (duplicate `Content-Length`, `CL`+`TE`, no `Host`, two `Host`, bare LF,
  CR in a header value, pipelining, absolute-form target, 32-char method, `HTTP/9.9`, negative `CL`,
  `Host : x`, NUL in the target, HTTP/1.0): all closed; `HEAD` → 405 (B7).
* Cross-tenant: B's token on A's medication id — GET/PUT/DELETE all 404, A's record intact.
* Enumeration and throttling: register with a known vs unknown email; 30 sequential and 64 concurrent
  registrations with RSS sampled before, after and 30 s later.
* Log injection: `\n` in an email (escaped correctly by the library) and ESC/BEL (passed through, B5).

### D. Performance

```sh
# throughput, per column in §8.1
ab -q -n 600 -c $C -H "Authorization: Bearer $TOK" http://127.0.0.1:18470/api/me/medications
ab -q -n 1000 -c $C http://127.0.0.1:18470/health
ab -q -n 2000 -c $C http://127.0.0.1:18470/api/zzz
ab -q -n 400  -c $C -T application/json -p post.json -H "Authorization: Bearer $TOK" \
   http://127.0.0.1:18470/api/me/medications

# CPU per request, from /proc/<pid>/stat fields 14+15 around 3000 serial /health requests
# => 206 ticks / 3000 = 0.69 ms CPU per request

# Python stdlib http.server on the same box: 2770 rps at c=1, 3503 at c=8

# thread-bound sensitivity
SERVER_MAX_CONNECTIONS=4|8|64 … ; ab -q -n 2000 -c 8 /health   # 2685 / 2468 / 2286 rps

# server profile under 4000 requests, SIGTERM to flush
ziman --profile src/main/ziman/serve.zm   # 92.9% (garbage collection)

# micro-benchmarks
ziman bench.zm   / python3 bench.py    # primes: 0.44-0.46 s vs 0.19-0.20 s
ziman bench2.zm  / python3 bench2.py   # map+string: 0.18 s vs 0.05 s
ziman alloc.zm   / python3 alloc.py    # json round trip: 0.15 s vs 0.16 s
ziman --profile alloc.zm               # 12.0% GC single-threaded, 51% in native json

# memory
fresh server RSS 66 MB → 1,088 MB after 64 concurrent registers → 1,166 MB after a second
burst → unchanged 30 s later and after 2,000 further GETs
```

### E. Static measurements

```sh
find src/main/ziman -name '*.zm' | wc -l                    # 152 files, 14,058 lines
grep -c 'server.Endpoint<Ctx> {' src/main/ziman/routes.zm   # 132 endpoints in 2,328 lines
grep -c 'server.encoder<'        src/main/ziman/routes.zm   # 105
grep -rc '#\[test'  --include='*.zm' src/main/ziman         # 0
grep -rc 'ServiceError.Storage(' --include='*.zm' .         # 3   (was 158)
grep -roE '[a-z_]+(\.[a-z_]+)?<[^>]+>\(' --include='*.zm' . | wc -l   # 335 (was 591)
grep -roc 'Result<[^,]*, string>' ziman/lib | awk -F: '{s+=$2} END{print s}'  # 281 (was 242)
grep -c 'CREATE INDEX' src/main/resources/migrations/V024__add_indexes.sql    # 20
# every pub fn in lib/ is referenced by a fixture or a doc example: 0 dead public API
# --fmt: 0 of 37 library files, 14 of 152 application files would change
# --lint src/main/ziman/serve.zm: no findings, exit 0 (all 152 files checked)
# --lint lib/time/time.zm: one real quadratic-concat finding at :280
# 75 distinct route patterns in routes.zm, all exercised by the Python suites
```
