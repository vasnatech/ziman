# Ziman developer-experience and code review (2026-10-14)

Reviewer background: backend engineer, 10+ years, Java/Spring, Go and Python. No prior exposure to
Ziman. Method: read the manuals first and checked them against the interpreter with scratch programs,
then reviewed the library written in Ziman and the reference backend, ran both test suites on both
engines, measured throughput and CPU, and probed hostile inputs against a live server. Earlier reviews
were read only after I had formed my own view, and every prior finding quoted below was re-run rather
than believed.

Findings are tagged **[V]** where I reproduced them and **[O]** where they are judgement. Every scratch
file lived under `/tmp/ziman_review_dev/`; nothing in either repository was modified except this file.

---

## 0. What I started from

| | |
|---|---|
| `ziman` | `80c8cb1` (*Fix api-check, which F122 left asking for a deleted module*) |
| `ziman` binary under test | `./ziman --version` → `d4d3b3d`; `make -q ziman` says up to date |
| `health-record` (both backends) | `4acd6e6` (*Move to the bare array helpers*) |
| `make test` (ziman fixtures) | **1006 passed, 0 failed** (27 s wall) |
| `src/test/other/run-all.sh --both`, SQLite | **49 suites, 2268 checks, 0 failed** (21 Python + 28 Ziman) |
| `src/test/other/run-all.sh --both`, Postgres | **20 suites, 1008 checks, 0 failed** |
| `--fmt --check` gate inside that script | passed (no `formatting: FAILED`) |
| Machine | 16 cores, 93 GB RAM, Fedora 43 (Linux 7.2.9), CPython 3.14.7, `ab` 2.4 |

Both repositories were clean before and after. The suite started and removed its own throwaway
`postgres:17` container; the pre-existing `health-record-db` container was left untouched, as were the
two long-exited containers beside it. `ziman`'s HEAD moved while I worked (`4deddb6`, `67675dd` landed
the C review and its task list); all my measurements are against the tree as of `80c8cb1`.

A small note on build identity [V]: `--version` reports `d4d3b3d` while HEAD is three commits later.
Those three commits touch only documentation and `tools/`, so `make` correctly had nothing to do — but
it means `--version` identifies *a build*, not *a tree*, and a reader comparing a bug report against a
commit cannot tell which. Stamping the version from `git describe --dirty` at link time, or printing
both the build commit and a warning when the tree is newer, would remove the ambiguity.

---

## 1. Executive summary

1. The manuals are still the best thing about the project, and the diagnostics have gone from
   "outstanding with four weak spots" to outstanding with one. I wrote working non-trivial Ziman from
   the documentation alone, first try. [V]
2. **Everything the 2026-10-05 review called P0 is fixed**, and I verified all of it: Postgres migrates
   and passes 1008 checks, the two numeric-corruption bugs are gone, server throughput no longer
   degrades with concurrency, and the function-valued-field papercut that landed on both extension
   mechanisms is gone. [V]
3. The backend remains genuinely well-layered and the most security-conscious code of its kind I have
   reviewed: no reachable SQL injection, cross-tenant isolation on every verb including UPDATE, request
   smuggling closed, log injection closed, and nine hardening measures the Spring original does not
   have. [V]
4. **Two P0 library defects reach any real deployment**: a driver thread's runtime error hangs its
   caller for ever, and `datasource.with_transaction` permanently leaks a pooled connection on a runtime
   error, killing the server's database access one handler at a time. Both re-verified independently. [V]
5. **Six list endpoints silently changed their sort order** because `Crud` cannot express an ordering;
   six indexes built for the old orderings are now dead weight. This is the template abstraction leaking. [V]
6. `parse_json` accepts unescaped control bytes including NUL, manufacturing a `string` the language says
   cannot exist; the backend then stores it and SQLite truncates it. Silent data loss from a request
   body. [V]
7. **`mutable_array_push` is superlinear for reference element types** — 4M strings in 16.4 s / 904 MB,
   against 0.81 s / 685 MB for the value-semantics `push` the manual steers you away from. The documented
   guidance is exactly inverted. [V]
8. Neither codebase has a single `#[test]` function. The language ships a unit-test runner, parametrized
   cases and an assertion module, and 39,000 lines of Ziman use none of it; all testing is end-to-end. [V]
9. CPU-bound Ziman is **3–4× slower than CPython 3.14** across three benchmarks, and 10–20× on the array
   pathology. Where the work is native (`json`, `crypto`) it is at parity. [V]
10. **Verdict: a competent team can build and now also *operate* this backend in Ziman** — the runtime
    objections of the last review are answered. Adopt it for a service this library covers, after the two
    P0 library fixes; the remaining risks are ecosystem, bus factor and the absence of a debugger, not
    correctness or throughput. [O]

---

## 2. What changed since the last review

`docs/reviews/developer-experience-code-review-2026-10-05.md` is the baseline. I re-ran every
reproduction in it.

**Fixed, re-verified [V]:**

| Prior finding | Now |
|---|---|
| **P0** Postgres cannot be migrated; 17 of 19 suites fail | `--both` → `postgres: 0 failed`, 20 suites, 1008 checks |
| **P0** Throughput *falls* with clients (1,511 at c=4 → 763 at c=64) | Rises to a plateau: 1,689 at c=16, 1,559 at c=64 (§8.1) |
| **P0** 92.9% of server CPU in garbage collection | Not reproduced; the scaling curve it caused is gone |
| **B2** `float_from_json` saturates a big JSON integer through `int64` | `1.2345678901234568e+29` — correct |
| **B3** `parse_float` rejects every subnormal → 500 | `parse_float("5e-324")` → `Ok(5e-324)`; `str`/`parse` agree |
| **B4** `float` JSON output diverges from Jackson three ways | `1.0`→`1.0`, `100.0`→`100.0`, `-0.0`→`-0.0`, kind preserved as `Float(1.0)` |
| **B5** Control characters other than CR/LF reach the log | ESC/BEL written as `\x1b`/`\x07` |
| **B6** Protocol faults answer a different body shape, no HSTS | 413/431/408 now carry the app's `{timestamp,status,message}` and HSTS, via `ServeOptions.fault` → `routes.protocol_fault` |
| **B7** `HEAD` on a `GET` route answers 405 | `HEAD /health` → 200, `Content-Length: 15`, no body |
| **B8** `medication_dose_logs`/`refill_logs` have no indexes | `V030__index_medication_logs.sql` |
| **B9** A dead spawned thread hangs its peers for ever | Deadlock detector fires with a message naming the cause |
| A function-valued struct field cannot be called as `x.f(a)` | `h.run(21)` works; the diagnostic no longer misleads |
| `a[0] = 9`, `p.x = 5`, `k++`, `Option<int> + int` diagnostics | All four now name the exact fix; the `Option` one is excellent |
| Only one syntax error reported per run | Two reported from one file |
| No `mapb_get_or` | `mutable_map_get_or` exists |
| `--coverage` has no machine-readable output; `--fmt --check` is not a CI gate | `--coverage-lcov=F` merging across runs; `--fmt --check` is the first thing `run-all.sh` does |
| The application has **zero** Ziman tests; a test file cannot live outside the source directory | 28 Ziman suites under `src/test/ziman`, found via `ZIMAN_MODULE_PATH` |
| `routes.zm` is 2,328 lines with 105 near-identical encoder blocks | 1,888 lines; `json_encoder`/`list_encoder`/`crud_endpoints` extracted |
| 591 → 335 explicit generic call sites | Inference now covers `array_sort([3,1,2])` and `array_map` with an annotated lambda |
| Registration burst: RSS 66 MB → 1.09 GB, permanent | 94 MB → 244 MB and plateaus over three rounds (§8.1) |
| No rate limiting; account enumeration through registration | Per-address `/api/auth/*` throttle, account and MFA lockout, Argon2id concurrency cap; registration answers the same either way with the hash computed in both branches |

That is an unusually complete response to a review. Nothing in the prior P0/P1 list is outstanding.

**Still holds, re-verified [V]:**

* **A module-level `val` is unreachable as `mod.NAME`, and the diagnostic still contradicts the manual.**
  `number.float_min_positive` → `module 'number' has no function 'float_min_positive'`. It has it, as a
  global `val`. The message should say so and name `use number.float_min_positive`. This is now the only
  actively wrong diagnostic I found.
* **`--lint` cannot be run over a library.** `ziman --lint libmod.zm` prints the findings, then
  `no 'main' function found`, exit 1. There is no entry file for `lib/`, which is why — as §5 shows —
  nothing lints the standard library and it carries findings its own linter would print.
* **The error-type zoo keeps growing**: `Result<_, string>` in `lib/` went 242 → 281 → **307**.
* **No `#[test]` anywhere.** The prior review scored this against the application; it is equally true of
  the library (§5.4).

**Regressed or newly broken since the last review [V]:**

* **Six list endpoints lost their domain ordering** when they moved to `crud.zm` (§4.5).
* **`tests/bench/server_load.sh` is dead and silently lies.** It launches `server.zm`, which no longer
  exists (`src/main/ziman/serve.zm` does), hits `/patients` endpoints that were never in this backend,
  prints empty RSS and `p50=0.0001s` for requests that all failed — and **exits 0**. `README.md:208`
  still advertises it. This is precisely the failure mode `run-all.sh`'s own header warns about: "a check
  that exists but that nothing invokes protects nothing".
* **`docs/TUTORIAL.md:247` still sends a newcomer to `collections.map`/`filter`/`fold`**, deleted on
  2026-10-14. `use collections` is now a module error. `docs/lib/examples/collections_example.out` is an
  orphan `.out` with no `.zm`. `docs/ROADMAP.md` names the module in forward-looking text (not only in its
  historical entries). SYNTAX.md and README.md are clean.

**Where I disagree with an earlier review [O]:**

* The 2026-10-05 review's headline that "the collector, not the interpreter, is the server" no longer
  describes this build, and I would not carry it forward. The scaling curve is now the ordinary shape of a
  thread-per-connection server on a slow interpreter: linear to saturation, flat after. The remaining
  memory story is a permanent step on first load, not unbounded growth.
* It rated the manuals **A−** partly for the per-module manual being drift-proof because generated. It is
  generated, but generation propagates mistakes as faithfully as it propagates truth: a misplaced comment
  in `prelude.zm` has published `array_join`'s signature under `array_frequencies`'s prose, and
  `array_frequencies` ships with no documentation at all (§5.3). Generated is not the same as checked.
* It called the explicit-type-argument tax "real but halved… which I did not mind". I mind one corner of
  it: inference works for a *fully annotated* lambda and fails for the short form, so
  `array_map([1,2], fn(x: int) -> int { x * 2 })` compiles while `array_map([1,2], fn(x) { x * 3 })`
  demands `<int,int>`. The terser the code, the more ceremony it needs — backwards.

---

## 3. Learnability and ergonomics

### 3.1 Rating the manuals: **A**

`docs/SYNTAX.md` (2,680 lines) is the best language reference I have read for a project this young, and I
can defend that concretely: I learned the language from it and the per-module pages alone, and the first
non-trivial program I wrote — structs, `Option`, lambdas, `array_map`/`array_sort`, `if`-as-value,
`if Some(e) = x` — ran correctly on the first attempt. Three properties do the work. It gives a *reason*
at every restriction, so the rule is learnable rather than memorable ("No positional construction
(`Point(0, 0)`) — with two same-typed fields that's a silent swapped-argument bug"). It writes down what
most references never state at all: "Semantic rules enforced before a program runs", "Other limits" with
every numeric ceiling and where it is enforced, the exact cost model of `push` versus `insert`/`set`. And
it is honest about its gaps in the same breath as the feature ("Not yet: HTTP/2, multipart, compression,
trailers") — the most useful habit a manual can have for someone deciding whether to adopt.

`docs/TUTORIAL.md` is a good hour and correctly sequenced — except for the stale `collections` line in
its closing "where to go next" paragraph, the worst possible place for one.

Against the grade: SYNTAX.md is a reference pretending to be approachable. No index, no cross-reference
table, and extreme density — "Maps and sets" alone carries twelve behavioural rules in eleven bullets.
Twice in this review I found experienced developers in the reference backend hand-rolling something the
manual documents, because `contains` is documented inside a 60-item builtin list (§4.3). Splitting "the
20 things you need" from "the 400 rules that exist" would cost nothing.

### 3.2 What I fought

* **Unannotated lambdas cost type arguments; annotated ones don't** [V]. Described above. This is the
  single friction I hit most often.
* **`fn main()` cannot return a `Result`**, so an entry point that wants `try` must `unwrap` instead. The
  backend lives with it (`main.zm` is a ladder of `switch`/`unwrap`), and the error is clear
  (`'main' must take no parameters and return nothing`), but Rust's `fn main() -> Result<(), E>` exists
  because this is the one place where a crash *is* the right handling.
* **The `Option`/`Result` unwrap vocabulary is large and only partly symmetric** [O]. `unwrap`,
  `unwrap_Some`, `unwrap_err`, `Option.unwrap_Some`, `option_unwrap_or`, `is_Some`, `is_ok`, bare `Some(x)`
  in five positions but not elsewhere. I got `unwrap(instant_of(1000, 0))` wrong (that one returns a plain
  `Instant`, not a `Result`) and the compiler caught it cleanly — but *which* constructors validate is not
  guessable: `localdate_of` returns `Result`, `instant_of` does not, `duration_of_seconds` normalizes
  instead of rejecting. That is defensible per function and hard to hold in your head across a module.
* **No field or index assignment is the right call and costs nothing**, after about twenty minutes. I
  expected this to be the main friction and it was not; `with` reads better than a setter chain.

### 3.3 Diagnostics: the best in class

Nine day-one mistakes, nine messages that name the fix. Two worth quoting because they teach:

```
'i' is an element of 'a' here ('for i in a' binds each element, not its position), so 'a[i]' indexes
    with a value; to loop over positions write 'for i in 0..len(a)', or use 'for i, item in a' for both

arithmetic needs the value inside the Option<int>, not the Option itself (found Option<int> and int)
    -- unwrap it first: 'if Some(v) = o { v + 1 }', or option_unwrap_or(o, 0) for a default,
    or Option.unwrap_Some(o) when it cannot be None
```

The second was *wrong advice* six weeks ago ("use 'as' to cast explicitly", which cannot cast an
`Option`). It is now better than anything Rust or Go will tell you. One wrong diagnostic remains, the
global-`val` one in §2.

### 3.4 Tooling

`--locals`, `--profile`, `--coverage` (now with `--coverage-lcov=`), `--fmt`, `--lint`, `--api` and
`--lsp` are a real toolchain, and `--profile` earned its keep twice in this review: it told me in one run
that 88% of a `mutable_array_push` loop was collection, and it is how the prior review found the GC
problem. Three gaps, in order of how much they would cost a team:

1. **No debugger.** `--locals` is excellent post-mortem and *only* post-mortem, only on a fatal error.
   You cannot inspect state when a function returns `Err`, which is the common failure in this language.
2. **No package management, and no manifest** — `ziman.toml` exists expressly as a reserved name that
   nothing reads. Every dependency is a feature request against the standard library. For this backend
   that cost nothing, because the library covers it; that is a property of this application.
3. **`--coverage` cannot see a bundled module, and `--lint` cannot be pointed at a library.** Together
   these mean there is no coverage figure and no lint gate for 19,110 lines of library Ziman (§5.4).

---

## 4. Code quality of the Ziman application

20,181 lines across 209 files; `routes.zm` is 1,888 of them.

### 4.1 Layering [O]: very good, and the comments are a model

`routes.zm` → `controller/` → `service/` → `repository/` → `datasource`, with `mapper/` between entity
and DTO and `model/` holding only `#[derive(Json)]` shapes. The dependency direction is never violated,
`Ctx` carries what a stateless handler cannot capture, and the narrow end is correct: a repository takes a
`datasource.Connection` and nothing else (`common/common.zm:14`).

What lifts this above "well-layered" is the commenting. These record *why*, name the ticket, and quote
measurements: `service/crud.zm:131-137` explains that `AND patient_id = $2` on the UPDATE is defence in
depth and that the asymmetry with DELETE was the only reason to wonder which was right; `config.zm:78-88`
says what `max-concurrent-hashes` does *not* do and gives the RSS numbers that prove it
(`MALLOC_ARENA_MAX=1` → 101 MB, `=2` → 120 MB). I have reviewed paid codebases with a tenth of this. It
is also what is most at risk from the staleness in §4.2.

### 4.2 Fossils and drift in the comments [V]

The commenting is good enough that the errors in it matter.

* `routes.zm:148,150` says the five-endpoint template serves "thirteen" resources. It serves **fourteen**
  (`grep -c 'crud_endpoints<'` → 15, one being the declaration).
* `config.zm:121-137` builds one `[string]` from seven with a **six-level nested pyramid** of
  `concat_arrays`:
  ```ziman
  return concat_arrays<string>(
      concat_arrays<string>(server_config_keys("server"), datasource_config_keys("datasource")),
      concat_arrays<string>(jwt_config_keys("app.jwt"), concat_arrays<string>( ... )))
  ```
  `concat_arrays` is variadic; `concat_arrays<string>(a, b, c, d)` works, verified. This is a fossil from
  before it was, and it is thirteen lines where one would do.
* `service/crud.zm:27-31` documents that reading a function-valued field into a local before calling it
  "was once required… either spelling is fine, and new code can call the field directly" — correct and
  well-handled, but the nine occurrences stayed, so a reader still meets the workaround nine times before
  meeting the note that it is one.

None of these is a bug. All three are the same thing: comments that were true and are now the most
authoritative wrong statement in the file.

### 4.3 The template: right idea, two things to extract [O]

`service/crud.zm`'s `Crud<E, Req, Res>` is the correct abstraction and the SQL it builds from
`table`/`columns` genuinely removes the hand-numbered-placeholder drift it was filed for. Three
observations.

**The per-resource wrappers are pure ceremony.** Ten services declare a `Crud` value and then five
one-line functions each (`return crud.crud_create(allergy_crud, ctx, patient_id, req)`); each matching
controller adds five more (`return to_response(ctx, service.create_allergy(...))`). That is ~100
functions whose entire content is their own name. `routes.zm` could take the `Crud` value directly and
build the five handlers with one generic adapter, deleting all of them; the only obstacle is that the
`Crud` vals are private. Worth doing — ~100 functions is ~100 places for a copy-paste to go wrong, and
one already did (§4.5).

**The template widens the API surface silently.** `crud_endpoints` emits a fixed five, so
`GET /api/me/emergency-contacts/{id}` exists in Ziman and not in Spring — because the resource was
registered through the template, not because anyone decided to add it. Harmless here; the mechanism is not.

**Two hand-rolled builtins** [V], both exactly `contains(arr, x)`, which works on `[string]` (verified):
`service/audit.zm:10` `contains_string` and `service/dashboard_summary.zm:69` `wants`. Two authors wrote
the same loop rather than find the builtin — a discoverability finding, not a competence one (§3.1).

### 4.4 Validation, error handling and status mapping [V]: the best part

One mapper, `controller/controller.zm:48-70`, decides every status; `service/service.zm`'s `FieldError`
accumulators mean a broken request reports *every* field rather than the first; `is_email` is a 60-line
faithful reimplementation of Hibernate Validator's rules with the reason written down ("the point here is
to agree with Spring rather than to be stricter than it"). Messages match Bean Validation to the letter,
including `size must be between 8 and 2147483647`.

One rough edge [O]: `Constraint(_)` maps to 409 `"a record with that value already exists"`. A
foreign-key violation naming a row that does not exist takes the same branch and the same text, which is
then wrong. The comment above it knows the two kinds differ; the message does not.

### 4.5 Data access [V]

**[V] High — six list endpoints silently changed their sort order.** `service/crud.zm:112-118` hardcodes
one ordering for every resource routed through it:

```ziman
format("SELECT * FROM {} WHERE patient_id = $1 ORDER BY created_at, id", crud.table)
```

`struct Crud` has no ordering field, so the template *cannot* express one. For six of the ten resources
using it, Spring orders by something else — `emergency_contacts` by `priority_order` (a column whose
entire purpose is display order), `family_history` by relationship, and `encounters`, `medical_procedures`,
`dental_records`, `vision_records` each by their own date column descending. What makes this a regression
rather than a simplification: `V024__add_indexes.sql` still carries the six indexes built for those
orderings, and its own header asserts "One index per table on the owner and the column the list is sorted
by serves the filter and, for a plain `ORDER BY`, the order" — now false for exactly those six. Each list
sorts in the engine on an index that no longer matches. **Fix:** add `order_by: string` to `Crud`,
default `"created_at, id"`, set the six. **Effort: small.**

**[V] Medium — `dashboard_summary` fetches every row to show five.** Each card calls its resource's own
service (`get_medical_procedures(ctx, patient_id)` → `SELECT *` over the whole table), sorts the result
in memory through an interpreted comparator, takes 5, and reports `len(all)` as the total
(`service/dashboard_summary.zm:397-403` and ~18 more). The design reason is sound ("every card comes from
its own resource's service, never from SQL of its own") and the totals are genuinely needed — but
`COUNT(*)` plus `LIMIT 5` is the shape that wants. Around twenty sequential round trips per dashboard
request, each over a full table.

**[V] Low — `most_recent` orders Instants wrongly, and its comment says it cannot.**
`service/dashboard_summary.zm:100-104`: "Dates are compared as their ISO text, which orders exactly as the
dates do… so one helper serves a `LocalDate` field and an `Instant` one alike." True for `LocalDate`,
false for `Instant`, because `instant_to_string` omits the fraction when `nano == 0`:

```
a = 1970-01-01T00:16:40Z          b = 1970-01-01T00:16:40.999999999Z
instant_compare(a,b) = -999999999      (a is earlier)
compare(text(a), text(b)) = 1          ('Z' > '.')
```

Live at `:375`, where the `familyHistory` card keys on `instant_text(f.created_at)`. Two records in the
same second, one with zero nanos, order backwards. **Fix:** key on `instant_compare`, or make
`instant_to_string` fixed-width.

**[V] Medium — the audit trail is a parsed text log, and it is O(log size) per request.**
`service/audit.zm:33-60`'s `tail_of` streams the entire log file on every `/api/admin/logs` and
`/api/admin/audit` call — 10 MiB of I/O and line allocation per request at the configured
`logging.max-bytes`. Beyond cost: the audit feed for a *health records* application is derived by
grepping nine event names out of rotating text (`logging.keep-files=5`), so audit history is bounded at
~50 MB and then deleted, and a reworded log line silently empties a compliance view. Parity with Spring,
which does the same — but "both are wrong" is the finding. An `audit_events` table written in the act's
own transaction is the shape this wants. [O]

**Correct and worth recording [V]:** every read and write is `WHERE id = $1 AND patient_id = $2` with the
patient from the authenticated `Principal`, never from path or body; no user data is ever interpolated
into SQL (the only `format`-built SQL interpolates `crud.table`/`crud.columns`, module-level `val`s);
`service.atomically` is nesting-aware, `defer`s its rollback, and calls `datasource.discard` rather than
`release` when the failure was `DbError.Connection`; and hand-written repositories use
`ORDER BY (CASE WHEN col IS NULL THEN 1 ELSE 0 END), col DESC` because Postgres and SQLite disagree on
NULL placement — a portability win Spring's derived queries do not have. The N+1 queries in
`get_lab_panels` and `get_recipes` are faithfully inherited from Spring, which has no fetch join either;
the difference is the *remedy*, one line of JPA against a hand-written grouping query here.

### 4.6 Configuration and migrations [V]

Configuration is exemplary and I would copy it. No secret has a default (`app.jwt.secret=${JWT_SECRET}`,
`app.security.mfa-secret-key=${MFA_SECRET_KEY}`); an unknown key stops startup with a suggestion; the old
shipped placeholder secret is refused *by value* with an explanation; the MFA key must differ from the JWT
key; every bound is range-checked with a message naming the key. Three settings are *not* validated where
every neighbour is (`server.max-connections-per-client`, `app.security.max-auth-attempts-per-address`,
`auth-attempt-window-seconds`) — worth closing for consistency.

Migrations: 28 shared plus 9 engine-specific pairs, every version in exactly one place, checksummed, one
transaction, advisory-locked on Postgres. **But eight of the nine engine pairs differ only in `TEXT`
versus `uuid` on `id`/`patient_id`** (plus the comment saying so) — ~300 lines of DDL maintained twice for
one type substitution, and precisely the duplication that caused the V028/V029 failure `run-all.sh`'s
header documents so candidly. A type token in `migrate` (`${uuid_type}`) collapses eight files to none.
**[O] The clearest library feature request to come out of the application.**

### 4.7 Testing [V]

Real strengths: 49 suites and 2268 checks on SQLite, 20 and 1008 on Postgres, both engines by default with
the script explaining *why* ("running only one is what let the schema break"), `--fmt --check` as the
first gate, and 28 suites now written in Ziman against a `harness.zm`. Two gaps.

**[V] Zero `#[test]` functions in 39,000 lines of Ziman.** Not in the backend, not in `lib/` (only
`lib/core/test.zm`, testing its own assertions). Every test in both codebases starts a real HTTP server,
so `is_email`, `record_failure`/`is_locked`, `most_recent`, `placeholders`/`set_clause`, all 426 lines of
`growth_percentile` and 273 of `vaccine_forecast` are reachable only through a socket. This directly
causes two findings above: nothing asserts ordering per resource, so §4.5's six-endpoint regression passed
2268 checks, and nothing feeds `most_recent` two same-second instants.

**[V] The Ziman suites are sqlite-only and run *in addition to* the Python they translate.** The script is
explicit and right about why (nothing in Ziman talks to the throwaway container); the present cost is 28
files maintained alongside 21 asserting the same things, with byte-comparison the only thing keeping them
honest.

### 4.8 Consistency with the Spring API [V]

I audited 226 Spring method+path pairs against 171 Ziman ones. 58 Spring endpoints are absent and cluster
almost perfectly by feature — 37 device/EHR integrations, 6 sharing, 3 attachments (needs multipart,
which `server` does not have), 4 admin DB browser, plus terminology, food search and export. `README.md`
states the scope up front ("24 of its 61 controllers"), so this is scope, not defect — but it does not
*enumerate* what is missing, so a reader cannot tell "not ported" from "forgotten". A generated parity
table would be cheap.

Provable behavioural divergences, beyond §4.5's ordering:

| | Divergence | Severity |
|---|---|---|
| 1 | `register` accepts `timezone`/`language` and **discards them**: `model/auth.zm:6-12` declares them, nothing on the register path reads them, and no `user_preferences` row is written. Spring writes one with `nonBlank(…, "UTC")`/`"en"`. `GET /api/users/me/preferences` then returns defaults. | **Medium** |
| 2 | `DELETE /{id}` on a missing or foreign id → **404** (Ziman) vs **204** (Spring, derived `deleteByIdAndPatientId`, a no-op). Deliberate and better REST; it breaks idempotent-delete clients and the stated indistinguishability goal. | Medium |
| 3 | A constraint violation → **409** (Ziman) vs **500** (Spring has no `DataIntegrityViolationException` handler at all). Ziman is right. | Low |
| 4 | Error `timestamp` is a UTC instant (`…Z`); Spring's is a zoneless `LocalDateTime`. Documented. | Low |
| 5 | The PASSWORD_RESET 403 body is the full three-key shape; Spring's filter emits a bare `{"message":…}`. | Low |
| 6 | CORS allowed-methods includes `PATCH` in Ziman only. | Informational |

Finding 1 is the one to fix, and it is the shape of bug this language is supposed to prevent: two struct
fields that decode and are never read. `--lint` flags an unused `val`; it does not flag an unread field.

---

## 5. Code quality of the library written in Ziman

19,110 lines. `lib/http/server.zm` is 2,577 of them, `lib/time/time.zm` 1,496, `lib/http/client.zm` 1,426.

### 5.1 Readability [O]

`lib/db/migrate.zm` is the best file in the library — checksums over CRLF-normalised text, strict version
contiguity, dialect directories that cannot collide with the shared one. `server.zm`'s typed-endpoint
layer (nine builders crossed over "where the input comes from" × "who may call", with one
`Api<E> { failure, fault }` per application) is genuinely good API design, and `routes.zm` is the proof:
adding an endpoint is one table entry. `time.zm` funnels every `Duration`/`Instant` constructor through
one `normalize`, so the sign invariant lives in one place.

### 5.2 Where the language forced awkward code [V]

The library itself documents the clearest case, at `lib/core/yaml.zm:444-450`:

```ziman
// The `while true` above only ever exits via a `return`; this line is dead and exists
// purely because the typechecker still requires every path out of the function to
// produce a value. The same idiom recurs in flow_map below.
return err("unreachable")
```

Three of these (`yaml.zm:450`, `:490`, `server.zm:323`). A `never` type, or flow analysis that understands
`while true`, deletes them.

Second: **error-type conversion exists and is unused**, so `fn lift<T>(r: Result<T, NetError>) -> Result<T, HttpError>`
is hand-written four times — `server.zm:147`, and again in `client.zm`, `pg.zm`, `mysql.zm` — identical in
shape, differing only in the target enum. `#[from]` covers this, and the reference backend uses it to
collapse 158 blocks to 3; none of the four library sites does.

Third: **`switch` is a statement**, so a boolean decision needs a `var` that a lambda cannot capture.
`write_response` (`server.zm:704-740`) makes three separate passes over the headers setting
`refused`/`keep`/`has_date`/`has_nosniff` where one pass would do.

Fourth [O]: **variadic-as-array makes a call read wrong.** `server.zm:311` is
`mutable_array_push(body, slice<byte>(buf, 0, n))`, which appends *every byte of the slice* because a lone
trailing array argument is spread. It reads like a one-element push. `push_all` says what is meant.

### 5.3 Dead code, inconsistent APIs, and documentation the generator published wrong [V]

* **`array_join` in the prelude is quadratic and reimplements a native builtin.** `prelude.zm:1042-1050`
  is `out = concat(out, …)` in a loop. Measured on 40,000 ten-byte pieces: native `join` **1 ms**,
  `array_join` **2,292 ms**, identical output. Exactly quadratic (20k → 767 ms, 40k → 3,048 ms). The
  one-line fix is `return join(separator: sep, pieces: items)`. Worse, its doc comment justifies its
  existence with a false claim — that the builtin `join` "is a *path* join over strings and cannot take a
  list". `join(separator:, pieces: variadic string)` is a general join and an array works. So the prelude
  ships a 2,600×-slower reimplementation of a builtin, documented with a wrong reason, under the very name
  the discoverability work steered newcomers and LLMs toward.
* **A misplaced comment corrupts the generated manual.** `prelude.zm:1034-1035` carries
  `array_frequencies`'s documentation but sits above `fn array_join`. Because `docs/lib/*.md` is generated
  from these comments, `docs/lib/prelude.md:1360-1372` prints `array_join`'s signature under
  `array_frequencies`'s prose, and `array_frequencies` has **no documentation at all**. Collateral damage
  from the `collections` → prelude move.
* **A documented `ServeOptions` field that does not exist.** `server.zm:1014` and `:1151` both reference
  `ServeOptions.max_body_bytes`; there is no such field — the cap is the hard-coded
  `val max_body_bytes = 1048576` at `server.zm:144`. It propagates to `docs/lib/server.md:470`, while
  SYNTAX.md correctly says "1 MiB, fixed". Separately [O]: a *non-configurable* 1 MiB body cap means
  multipart uploads could never exceed 1 MiB even once multipart exists.
* **Two genuinely dead private functions**: `mysql.zm:282` `skip_lenenc_string` and `:346` `expect_ok`,
  referenced nowhere in `lib/`, `tests/` or `docs/`. The language has no unused-function check.
* **Duplication the module system cannot share**: `pad(n, width)` at `mysql.zm:694` duplicates
  `pad_left(n, width)` at `time.zm:125`. A private helper cannot be imported and there is no internal util
  module.

### 5.4 Missing tests, and a gate that does not exist [V]

Beyond "no `#[test]` anywhere" (§4.7), two structural gaps:

**`grep -n lint Makefile` returns nothing.** There is no `lint` target, while `fmt-check` *does* cover
`lib`. Copying library files to a scratch directory and linting them as ordinary modules finds real
findings in 15 of 38 files — four quadratic string builds (`yaml.zm:670,686,697,708`, `csv.zm:329`,
`time.zm:280`), both of §5.5's P0 `spawn` sites, three `val pool` switch arms that should be `Pool(_)`,
three discarded `Result`s bound to a name (defeating must-use), and `server.zm:2552,2575` `val slot` never
read. **The linter already prints the exact warning for the worst defect in the library, and nothing runs
it.** The obstacle is §2's standing finding: `--lint` on a non-entry module exits 1 with
`no 'main' function found`, so there is no clean way to lint a library at all.

**Bundled-module coverage is not measurable with shipped tooling.**
`./ziman --coverage docs/lib/examples/yaml_example.zm` reports `5/5 lines (100%)` and says nothing about
the ~577 lines of `yaml.zm` it just exercised. Copying the module into a project makes it visible
(31% for that example; 76% for a 15-case driver), so the capability exists and nothing in `make check`
uses it. There is no coverage number for 19,110 lines of library Ziman.

Thinnest fixture coverage by module: **`pg` 0, `mysql` 0** — the two largest drivers, 2,185 lines
combined, with `test-pg`/`test-mysql` deliberately out of `make check` because they need containers
(documented, and the reason is good). Then `zone`, `url`, `otp`, `sqlite` at 1 each.

### 5.5 Library defects

**[V] P0 — a driver's `serve` thread dying hangs its caller for ever.** `pg.zm:820` is
`spawn serve(sock, requests)` with no `on_error` (same at `mysql.zm:1295`), and `db.zm:348-352` does
`send(requests, …)` then an untimed `receive(reply_to)`. Any runtime error in `serve` — an `unwrap`, an
out-of-bounds index on a malformed server reply — ends that thread without replying, and the caller blocks
for ever. The deadlock detector cannot help in a server: it fires only when *every* live thread is blocked
on a channel, and a server always has others working. Reproduced and independently re-run
(`/tmp/ziman_review_dev/lib/t_hang2.zm`, killed by `timeout 10`, exit 124). **Fix:** `on_error` that drains
pending requests with `Err(DbError.Connection(m))`, and/or `receive_timeout` in `run_request`.

**[V] P0 — `with_transaction` permanently leaks a pooled connection on a runtime error.**
`datasource.zm:446-451` calls `settle(conn, tx, result)` as a plain statement, not a `defer`. A runtime
error inside `work` unwinds past it, so the connection never returns to the idle channel. Reproduced and
independently re-run (`t_pool.zm`, pool of 2, two failing transactions):

```
both transactions failed; now trying an ordinary statement
acquire_timeout: Err(Connection(timed out after 2000 ms waiting for a pooled connection))
```

Under `server.serve` each failing handler eats one connection until the server cannot reach the database
at all, while still answering 500s — a slow, silent death with no log line saying why. `execute`, `query`
and `query_one` on a pool have the same inline-`settle` shape. The fix is verified to work: a spawned
thread's `defer`s do run after `on_error`, so `defer settle(…)` releases correctly.

**[V] High — `parse_json` accepts unescaped control bytes, including NUL, and manufactures an impossible
`string`.** RFC 8259 forbids bytes below 0x20 unescaped in a string; Ziman accepts all of them, and for
NUL it produces a `string` containing a 0 byte — which SYNTAX.md says cannot exist ("`bytes_to_string` is
a `Result` … the one place UTF-8 is checked"; "`\u0000` errors (a string can't hold a 0 byte)"). The two
spellings disagree: `"\u0000"` is correctly refused, a raw `\x00` is not.

```
parse_json_bytes({"s":"a\x00b"})  → Ok,  len(s) = 3
after a SQLite round trip          → len = 1
print/format of it                 → "a"
```

End-to-end on the backend: `POST /api/me/allergies` with a raw NUL in `substance` returns **201** with
`"substance":"a"` — silent truncation of a client's data — where the escaped form correctly returns 400.
Jackson rejects unescaped control characters, so this is also a Spring-parity divergence on any field.
**Fix:** reject unescaped bytes < 0x20 in the string scanner, matching the `\u0000` path and the RFC.

**[V] Medium — deeply nested YAML aborts the process.** `yaml.zm`'s flow parser (`flow_seq:426`,
`flow_map:452`) has no depth limit and allocates a handle per level, unlike `json`'s documented 512-level
cap. 50,000 levels of `[` gives `Too many retries in GC_allocobj` / `Aborted (core dumped)`, exit 134,
with no Ziman error — contradicting SYNTAX.md's "a pathological input is an error, not a crash".
`toml.parse_toml` degrades correctly on equivalent input.

**[V] Medium — `ZoneOffset` does not range-check minutes, so the "exact inverse" claim is false.**
`time.zm:1038-1043` checks only that hours and minutes agree in sign, and `zoneoffset_from_string` checks
only shape and the ±18 h total. `"+05:99"` parses and silently renormalises to `+06:39`; so does
`instant_from_string("…T00:00:00+05:99")`, which means a timestamp field in a JSON request body accepts a
malformed offset and stores a different instant than the client wrote. **Fix:** reject minutes outside
−59..59.

**[V] Low — `localdate_of` accepts negative (BCE) years the docs say cannot exist.** `time.zm:518` and
SYNTAX.md both say year is non-negative; `localdate_of(-44, 3, 15)` → `Ok`, `"-0044-03-15"`, and it
round-trips through `to_epoch_day`/`from_epoch_day` with the leap rule correct for −4. The arithmetic is
*right*; the documented invariant is simply false, so callers can rely on neither. Related: date
arithmetic has no upper bound either — `localdate_add_days(+999999999-12-31, 1)` gives
`+1000000000-01-01` where java.time throws, and `+12626367463885277-04-14` is reachable. True int overflow
traps cleanly.

**[V] Low — URL-embedded Basic credentials are dropped on a same-host path redirect.**
`client.zm:763-778`'s `resolve_location` rebuilds the next URL from `host_header(base)`, which omits
`userinfo`, so `GET https://user:pass@host/a` answered `302 Location: /b` re-requests `/b`
unauthenticated and the caller gets an unexplainable 401. The cross-origin stripping just above is
correct; this is the same-origin case leaking the other way.

**[V] Low — `float` exponent form still diverges from Jackson.** `1e21` writes as `1e+21`; Jackson writes
`1.0E21`. The other three parts of the prior B4 are fixed.

**Checked and correct [V]:** constant-time comparison in `jwt.zm:237` and `otp.zm:130` (both via
`crypto.bytes_equal`); the regex engine is genuinely linear (`(a+)+b` against 30 `a`s returns at once);
`log`'s JSON mode escapes control characters through `json`, and its text mode escapes every byte below
0x20 — which is what closes log injection (§6); `properties.report` masks `password`/`token` keys *and*
URL credentials; `json` rejects `\u0000` and caps nesting at 512; `csv`, `toml`, `mime`, `properties` and
`number` all reject hostile input with precise line-numbered messages; `migrate`'s checksum and ordering
logic; `server.forwarded_for`'s right-to-left hop counting.

---

## 6. Security

Listed separately, as the brief asks. The headline is that the Ziman port is **meaningfully more secure
than the Spring original it copies**, and that is unusual enough to state plainly.

**Verified correct, and the reference backend does not have it:** per-account login and MFA-attempt
lockout (`service/service.zm`'s `AttemptWindow`); a per-address throttle on `/api/auth/*` enforced in
`routes.route` *before* any handler runs, because registration allocates a ~19 MiB Argon2id buffer before
it knows who is asking; a counting-semaphore cap on concurrent Argon2id (`common/common.zm:88-101`, with
a `defer` so a runtime error cannot leak a slot); Argon2id with transparent upgrade from Spring's bcrypt
hashes; a `ver` claim bound to `users.token_version`, so a password change revokes outstanding tokens
(Spring's survive until `exp`); the TOTP secret encrypted at rest under a key that must differ from the
JWT secret; the login provider-disclosure oracle closed with a dummy verify against a fixed hash; and
registration answering identically whether or not the email exists, with the hash computed in both
branches so the timing matches too.

**Verified correct, both backends:** no reachable SQL injection; cross-tenant isolation on every verb,
with the patient id taken only from the authenticated `Principal`; role checks read from the database
rather than the token, so a demotion takes effect at once; CORS from the library's own policy, with
`Vary: Origin` on every answer it touches including the 403 preflight refusal, and `Allow-Credentials`
suppressed for a wildcard origin.

**Request-level hardening, all probed live [V]:** two `Content-Length`, `TE`+`CL`, two `Host`, no `Host`
on HTTP/1.1, a control character in a header value, a NUL anywhere in the head, `HTTP/9.9`, a malformed
request line — every one a 400 with a message naming the problem; 2 MB body → 413 and 20 KB of headers →
431, **both now in the application's own JSON shape with HSTS**; a preflight from a disallowed origin →
403; `alg: none` → 401. No `Server` banner, `nosniff` on everything.

**Log injection is closed here and open in the reference** [V]. Both backends log
`AUTH_LOGIN_FAIL … email={}` with an unvalidated email, and both build the admin audit feed by parsing
that text. Ziman's `log` escapes control bytes, so `"a@b.c\r\nFAKE LOG LINE"` lands as one line with the
CR and LF literalised. Logback does not, so the same request forges an audit row in Spring.

**Open security issues:**

| | Issue | Severity |
|---|---|---|
| S1 | **Tokens carry no `iss`/`aud` and are verified with bare `jwt.verify`** (`service/security.zm:40`), although the library ships `verify_claims(issuer:, audience:)` precisely so a token for one service cannot be accepted by another sharing the key. Equivalent to Spring's weakness, and a cheap hardening the library is already equipped for. | Low–Medium [O] |
| S2 | **A raw NUL in any string field truncates silently** (§5.5), so a client can store a different value than it sent and than the API echoes back. On a medical record that is the bad kind of data loss. | High [V] |
| S3 | **`+05:99` is accepted as a zone offset and renormalised** (§5.5), so a timestamp is stored differently from what the client wrote. | Medium [V] |
| S4 | **The audit trail is parsed from a rotating text log** bounded at `max-bytes × keep-files` (~50 MB by default) and then deleted, with no tamper-evident record (§4.5). | Medium [O] |
| S5 | **An impersonation token is marked `impersonated: true` and nothing ever reads the claim** (`service/token.zm:80`; `grep` finds no consumer). The act is audited at issue and the lifetime is short, so this is defence-in-depth missing rather than a hole — but a 15-minute token that acts as the user and is indistinguishable from a real login once issued is worth being able to recognise server-side. | Low [V] |
| S6 | **A registration burst costs 150 MB of RSS permanently** (§8.1). Down from the prior review's 1.09 GB and now plateauing, so it is a capacity-planning fact rather than a denial of service — with the per-address throttle in front of it. | Low [V] |

**Not problems, recorded so they are not re-litigated [V]:** control characters other than NUL in a string
field are accepted and stored, which matches Jackson; `%2e%2e%2f` in a path decodes to `../` and is
answered 400 by `uuid_param` with the decoded text reflected into a JSON body, which is correctly escaped;
`forwarded_for` with `trusted_proxy_hops: 0` ignores `X-Forwarded-For` entirely, which is the right
default; the 413/431 rejections carry no CORS headers, which is unavoidable because no `Origin` has been
parsed yet, and the code says so.

---

## 7. Bugs, consolidated

Library defects are detailed in §5.5 with reproductions; application defects in §4.5 and §4.8.

| | Bug | Where | Severity |
|---|---|---|---|
| D1 | A driver's `serve` thread dying hangs its caller for ever | `pg.zm:820`, `mysql.zm:1295`, `db.zm:348` | **High** |
| D2 | `with_transaction` leaks a pooled connection on a runtime error | `datasource.zm:446` | **High** |
| D3 | `parse_json` accepts raw control bytes; NUL makes an impossible `string`, then truncates | `json`, reaching every backend field | **High** |
| D4 | Six list endpoints lost their domain ordering; six indexes now unused | `service/crud.zm:112` | **High** |
| D5 | `array_join` is quadratic and 2,600× slower than the builtin it reimplements | `prelude.zm:1042` | Medium |
| D6 | `mutable_array_push` is superlinear for reference element types | prelude / runtime | Medium |
| D7 | Deeply nested YAML aborts the process instead of erroring | `yaml.zm:426,452` | Medium |
| D8 | `ZoneOffset` minutes unchecked; `+05:99` silently renormalised | `time.zm:1038,1068` | Medium |
| D9 | `register` discards `timezone`/`language` | `service/auth.zm:74-85` | Medium |
| D10 | `tail_of` streams the whole log per admin request | `service/audit.zm:33` | Medium |
| D11 | `dashboard_summary` fetches every row to show five | `service/dashboard_summary.zm` | Medium |
| D12 | `most_recent` orders `Instant`s by text, which is wrong when `nano == 0` | `dashboard_summary.zm:104` | Low |
| D13 | `tests/bench/server_load.sh` is dead, reports fake numbers, exits 0 | `tests/bench/` | Low |
| D14 | `localdate_of` accepts BCE years and unbounded future years the docs exclude | `time.zm:529` | Low |
| D15 | Basic credentials dropped on a same-host path redirect | `client.zm:763` | Low |
| D16 | `1e21` writes as `1e+21`, Jackson `1.0E21` | `json` | Low |
| D17 | `docs/TUTORIAL.md:247` names the deleted `collections` module; orphan `collections_example.out` | docs | Low |
| D18 | Two `ServeOptions.max_body_bytes` references to a field that does not exist | `server.zm:1014,1151` | Low |
| D19 | `array_frequencies` ships undocumented; its prose is published under `array_join` | `prelude.zm:1034` | Low |
| D20 | Dead private functions `skip_lenenc_string`, `expect_ok` | `mysql.zm:282,346` | Low |
| D21 | `--lint` on a non-entry module exits 1; no `make lint`; no library coverage figure | tooling | Medium [O] |
| D22 | `number.float_min_positive` reports "module has no function" | diagnostics | Low |

---

## 8. Productivity and maintainability verdict

### 8.1 Measurements

**Request throughput and scaling** — same box, `ab`, in-memory SQLite, pool 4, `max-connections=64`,
3,000 requests per cell. Compare the prior review's row in §2:

| clients | `GET /health` | 404 (no DB, no auth) | `GET /api/me/allergies` (auth) | `GET /api/me/medications` (auth, 5 rows) |
|---|---|---|---|---|
| 1 | 628 | 625 | 472 | 171 |
| 4 | 1,881 | 1,987 | 1,262 | 481 |
| 8 | 2,561 | 3,045 | 1,627 | 676 |
| 16 | **2,808** | **3,097** | **1,689** | **710** |
| 32 | 2,846 | — | 1,629 | 710 |
| 64 | 2,655 | — | 1,559 | 674 |

Zero failed requests anywhere. p99 rises from 3 ms at c=1 to 81 ms at c=64 on `/health` and 166 ms on the
medications read. **The negative scaling is gone**: every column rises to a plateau at 16 clients and
holds, where six weeks ago each peaked at 4–8 and fell below its single-client figure by 64. That was the
finding the last review said "decides whether Ziman can host a real backend", and it is answered.

For calibration [O]: Spring Boot here would be 10–20k rps and Go 100k+, so Ziman sits an order of
magnitude below both, roughly where Python's stdlib `http.server` does — but with sane tail latency and no
collapse. For an internal service, an admin API, or anything behind a cache, that is enough.

**Memory.** Idle 94 MB RSS. Three rounds of 64 concurrent registrations: 94 → 201 → 233 → 244 MB, and it
does not come back on idle. Down from 66 MB → 1.09 GB, and now plateauing rather than stepping each
round — the difference between a capacity fact and a leak. 94 MB idle is still high for the work done [O].

**CPU-bound micro-benchmarks against CPython 3.14.7** (best of 2–3 runs, `/usr/bin/time`):

| benchmark | Ziman | Python | ratio |
|---|---|---|---|
| `fib(32)` — 7.05M calls | 0.57 s | 0.15 s | **3.8× slower** |
| Sieve of Eratosthenes to 2,000,000 | 0.85 s / 41 MB | 0.27 s / 25 MB | **3.1× slower**, 1.6× memory |
| 400k `MutableMap` get-or-default + set | 0.16 s / 14 MB | 0.04 s / 10 MB | **4.0× slower** |
| 400k `mutable_array_push` of a `string` | 0.29 s / **122 MB** | 0.03 s / 13 MB | **10× slower, 9× memory** |
| Interpreter start-up (`print(0)`) | 0.00 s | 0.01 s | Ziman faster |

So: **3–4× slower than CPython on ordinary interpreted work.** The prior review measured 2.3× on one
benchmark and read it as a good result for a tree-walking interpreter; across three benchmarks I get 3–4×,
which is still a respectable number and still means CPU-bound work belongs in a native module.

The fourth row is the one to act on. Scaling `mutable_array_push` of a `string`:

| n | `mutable_array_push` | immutable `push(arr, x)` |
|---|---|---|
| 500,000 | 0.38 s / 122 MB | 0.15 s / 101 MB |
| 1,000,000 | 1.26 s / 233 MB | 0.26 s / 185 MB |
| 2,000,000 | 4.64 s / 455 MB | 0.50 s / 351 MB |
| 4,000,000 | **16.43 s / 904 MB** | **0.81 s / 685 MB** |

The builder is superlinear — roughly 3.5× per doubling, about O(n^1.8) — while the value-semantics `push`
is clean O(n) and **20× faster at 4M**. `--profile` attributes 88.4% of the 2M case to garbage collection;
`ziman-nogc` still takes 3.64 s, so allocation shares the blame. A packed `[int]` builder is fine (4M in
0.69 s), and struct elements sit in between (1M: 1.47 s builder vs 1.01 s `push`), so the pathology is
specific to pushing an already-allocated reference value.

This matters because SYNTAX.md says the opposite: `MutableArray<T>` "is the right tool for
building/editing a large array piece by piece", and the reference backend follows that advice in
`crud_find_all_by_patient_id`, `tail_of`, `placeholders` and ~40 other places. For small results it is
irrelevant; for a list endpoint over a large table it is the difference between a request and a timeout.
Either fix the builder or change the guidance — right now a developer who reads the manual picks the
slower path.

### 8.2 How fast could a team build and maintain a real backend?

**Building: 1.3–1.8× Spring's effort** for a service this library covers, **3×+** for one it does not [O].
I will not move the prior review's estimate; the evidence on disk supports it. 171 endpoints, auth, MFA,
admin, audit, broadcasts, paging, migrations and a dashboard aggregator in 20,181 lines, lint-clean,
`--check`-clean, passing 3,276 integration checks on two engines. What a Spring team spends its first
month on — classpath, reflection, annotation processors, Hibernate session semantics, proxy-based
`@Transactional` — does not exist here; what replaces it is explicit (`atomically` is a function you read).
The 3×+ figure is the one to plan around, and §4.8 quantifies it: the 58 unported endpoints are 37 OAuth
integrations, 3 multipart uploads and an export path, every one of which is "write it yourself, with no
library and no prior art".

**Maintaining: better than Spring in the small, and the gap in the large has narrowed.** Nothing acts at
a distance — a function taking a struct cannot mutate the caller's copy, `from_service_error` is the only
status mapper, and exhaustive `switch` means adding a `ServiceError` variant breaks every consumer at
compile time, which is the single property I most want in a maintained codebase. `routes.zm` shrank
rather than grew. What will still hurt: ~100 one-line passthrough functions (§4.3); the schema written
twice for two engines (§4.6); no unit-test level, so every assertion costs an HTTP round trip and whole
modules are reachable only through a socket (§4.7); and no debugger when an incident is live.

### 8.3 Top adoption risks [O]

1. **The two P0 library defects (D1, D2).** Both are silent, both are in the database path, and D2
   degrades a server to total database failure one failing request at a time while still answering 500s.
   Neither is hard to fix. Nothing else on this list should be considered until they are.
2. **Bus factor and ecosystem.** One implementation, one author, no package manager, no third-party
   libraries, Linux only with no second compiler tried. Every dependency is a feature request, and this
   is now the largest risk by a distance — bigger than anything about the language.
3. **No unit-test level, used by nobody.** The runner exists and 39,000 lines ignore it. This is why the
   six-endpoint ordering regression survived 3,276 checks. It is cultural, not technical, and cultural is
   harder.
4. **Gates that exist and are not invoked.** `--lint` finds both P0s and every quadratic in the library,
   and no target runs it; `--coverage` cannot see a bundled module; `server_load.sh` reports fabricated
   numbers and exits 0. The project has written down this exact failure mode twice in its own comments and
   it keeps recurring.
5. **3–4× CPython on interpreted work, and one 20× pathology where the manual points you.** Fine for an
   I/O-bound API; it means any real computation must be designed around, and the guidance you would follow
   to do that is currently wrong.

What I would *not* worry about: the language's learnability, the diagnostics, the layering, the HTTP
parser's security, SQL safety, cross-tenant isolation, or — now — throughput scaling. Those are solved.

---

## 9. Prioritised action list

### P0

| | Item | Rationale | Effort |
|---|---|---|---|
| 1 | `on_error` on `pg.zm:820` / `mysql.zm:1295` `spawn serve`, draining pending requests with `Err(DbError.Connection(…))`; `receive_timeout` in `db.zm`'s `run_request` | D1: a hung request thread per driver failure, undetectable by the deadlock detector in a server | small |
| 2 | `defer settle(…)` in `with_transaction`, and the same in pooled `execute`/`query`/`query_one` | D2: total loss of database access, arriving silently | small |
| 3 | Reject unescaped bytes < 0x20 in `json`'s string scanner | D3: silent truncation of client data in every text field, and a `string` the language says cannot exist | small |
| 4 | `order_by: string` on `Crud`, default `"created_at, id"`, set for the six resources | D4: six endpoints changed behaviour; six indexes dead | small |
| 5 | A `make lint` target over `lib/**/*.zm`, gated in `check` — which needs `--lint` to accept a non-entry module | Finds D1, D5 and every quadratic today; the gate is the fix that prevents the next one | small |

### P1

| | Item | Rationale | Effort |
|---|---|---|---|
| 6 | `array_join` → `return join(separator: sep, pieces: items)`; delete the false claim about `join` | D5: 2,600× on 40k pieces, under the name newcomers are steered to | one line |
| 7 | Investigate `mutable_array_push`'s superlinearity for reference elements; until fixed, correct SYNTAX.md's guidance | D6: the manual currently recommends the 20×-slower path | medium / trivial |
| 8 | Minute range check in `ZoneOffset`; nesting cap in `yaml`'s `flow_node` | D8 stores a different instant than sent; D7 aborts the process | small |
| 9 | Write `user_preferences` at registration from `timezone`/`language` | D9: accepted input silently discarded; a client cannot tell | small |
| 10 | `jwt.verify_claims(issuer:, audience:)` in `service/security.zm`, issuer/audience in `token.zm` | S1: the library already offers it; stops key-sharing cross-acceptance | small |
| 11 | An `audit_events` table written in the act's own transaction, replacing log parsing | S4/D10: compliance record that rotation deletes, and O(log size) per admin request | medium |
| 12 | Fix `docs/TUTORIAL.md:247`, delete the orphan `collections_example.out`, sweep ROADMAP's forward-looking mentions | D17: the first doc a newcomer finishes ends on a module error | trivial |
| 13 | Delete or rewrite `tests/bench/server_load.sh`; remove the README reference | D13: a benchmark that fabricates numbers and exits 0 | trivial |
| 14 | `COUNT(*)` + `LIMIT` per dashboard card | D11: ~20 full-table reads per request | medium |
| 15 | Fix the `array_frequencies`/`array_join` comment swap; the two `ServeOptions.max_body_bytes` references | D19/D18: the generator published both as documentation | trivial |

### P2

| | Item | Rationale | Effort |
|---|---|---|---|
| 16 | `#[test]` units for `time`, `json`, `csv`, `toml`, `yaml`, `number`, `mime`, and for the backend's `is_email`, `AttemptWindow`, `most_recent`, `growth_percentile`, `vaccine_forecast` | D4 and D12 are both "no test could have caught this at the level it lives" | medium |
| 17 | A coverage figure for `lib/`, via `--coverage` seeing bundled modules | 19,110 lines with no number | medium |
| 18 | Collapse the eight `TEXT`/`uuid`-only migration pairs, with a type token in `migrate` | 300 duplicated lines; this duplication already caused one two-commit outage | medium |
| 19 | Build the five CRUD handlers from the `Crud` value in `routes.zm`, deleting ~100 passthrough functions | Each is a place for a copy-paste to go wrong; one already did | medium |
| 20 | `instant_compare` in `most_recent`; fixed-width `instant_to_string` | D12 | small |
| 21 | `#[from]` on `HttpError`/`ClientError`/`pg`/`mysql` error enums, deleting four hand-written `lift`s | §5.2; the backend already proved the pattern (158 → 3) | small |
| 22 | Diagnostic for a module-level `val` reached as `mod.NAME`; `--lint` rule for an unread struct field | D22, and the rule that would have caught D9 | small |
| 23 | Separate "the 20 things you need" from SYNTAX.md's 400 rules; `make experimental-check`-style guard for stale module names in docs | Two developers hand-rolled `contains`; one tutorial line names a deleted module | medium |
| 24 | Quadratic `concat` at `yaml.zm:670-708`, `csv.zm:329`, `time.zm:280`; linear body accumulation at `server.zm:487`; make the 1 MiB body cap a `ServeOptions` field; delete `mysql.zm:282,346` | §5.3/§5.4 | medium |
| 25 | Validate `max-connections-per-client`, `max-auth-attempts-per-address`, `auth-attempt-window-seconds`; stamp `--version` from the tree; enumerate the unported Spring endpoints in the README | Consistency with every neighbouring setting; build identity; scope legibility | small |

---

## Appendix: experiments and commands

Everything ran from `/tmp/ziman_review_dev/`. `$Z` is
`/home/mfg/workspace/vasna.tech/repos/ziman/ziman`; `$BK` is
`/home/mfg/workspace/vasna.tech/repos/health-record/health-record-backend-ziman`.

### A. Baselines

```
$ git -C ziman rev-parse --short HEAD                       → 80c8cb1  (clean)
$ git -C health-record rev-parse --short HEAD               → 4acd6e6  (clean)
$ ./ziman --version                                         → ziman d4d3b3d
$ make -q ziman; echo $?                                    → 0  (up to date)
$ make test                                                 → 1006 passed, 0 failed   (27.5 s)
$ $BK/src/test/other/run-all.sh --both
  == sqlite ==    49 suites, 2268 checks, sqlite: 0 failed
  == postgres ==  20 suites, 1008 checks, postgres: 0 failed
  (the script's own `time` reported `real 462m`, against `user 1m12s` — bogus; observed wall
   time was ~75 min. The suite created and removed its own `ziman-tests-$$` container;
   `health-record-db`, up 44 h, was not touched.)
```

### B. Reading the manuals, then checking them

```
$ $Z e1.zm            # structs, Option, lambdas, array_map/sort, if-as-value, if Some(e) = x
  → ran correctly first attempt
$ $Z e2.zm            # array_sort([3,1,2]) and array_map with an annotated lambda infer; the
                      # terser fn(x) { x * 3 } does not:
  type error: cannot infer type argument 'U' of 'array_map' from its arguments
$ $Z ca.zm            # concat_arrays<string>(a, b, c, d) → [a, b, c, d]   (config.zm's pyramid is a fossil)
$ $Z ff.zm            # H { run: twice }; h.run(21) → 42   (prior review's papercut, fixed)
$ $Z c.zm             # contains(["a","b"], "b") → true   (what audit.zm and dashboard_summary.zm hand-rolled)
```

Diagnostics probed (`--check`): string `+`, `int + float`, `p.x = 2`, `a[0] = 2`, missing `switch` arm,
lambda capturing a `var`, `try` in a `while` condition, `k++`, `Option<int> + int`, `main` returning a
`Result`, two errors in one file, `number.float_min_positive`, `--lint` on a non-entry module.
All excellent except the last two.

### C. Re-running the previous review's bugs

```
$ $Z verif.zm
  B2 float_from_json = 1.2345678901234568e+29          FIXED
  B3 parse_float(5e-324) = Ok(5e-324); str/parse agree  FIXED
  B4 str(1.0)=1.0 json=1.0 | 100.0→100.0 | kind Float(1.0)   FIXED
$ $Z v2.zm
  B4 -0.0: str=-0.0 json=-0.0                          FIXED
  B5 WARN before\x1b[2J\x07after                       FIXED
$ timeout 10 $Z v3.zm
  deadlock: every thread is blocked on a channel …      FIXED (was: hang)
```

### D. Hostile input against a live server (`probe/hostile.py`, `probe/proto.py`, port 19777/19779)

Application level — register, log in, then 30 probes. Notable:

```
onsetDate "1990-02-30"  → 400 onsetDate: invalid day: 30 (month 2 of 1990 has 28 days)
onsetDate "0000-01-01"  → 201   (matches java.time)
onsetDate "+10000-01-01"→ 201   (matches java.time)
substance "a\u0000b"    → 400 malformed JSON: \u0000 is not supported …
substance "a<raw NUL>b" → 201  "substance":"a"          ← D3, silent truncation
Content-Type text/plain → 415 | absent → 415
600-deep JSON           → 400 nesting is too deep at position 525
2 MB body               → 413 | 900 KB field → 201
Authorization alg:none  → 401 | Basic → 401 | malformed → 401
substance "x'; DROP TABLE allergy_intolerances; --" → 201, stored verbatim
GET /api/me/allergies/%2e%2e%2f → 400 invalid allergy id: ../
email "a@b.c\r\nFAKE LOG LINE" → one log line, CR/LF literalised
```

Protocol level — 15 raw socket probes, all answered as §6 lists. `HEAD /health` → 200 with
`Content-Length: 15` and no body (B7 fixed); 413/431 in the app's JSON shape with HSTS (B6 fixed).

### E. The NUL defect, isolated

```
$ $Z nul.zm    # {"s":"a<NUL>b"} via parse_json_bytes
  PARSED OK | {"s":"a\u0000b"} | s len = 3
$ $Z nul2.zm   # raw TAB / LF / CR / 0x01 inside a JSON string
  all four: OK      ← RFC 8259 forbids unescaped < 0x20; Jackson rejects them
$ $Z nul3.zm   # the same string through SQLite
  ziman string length: 3 | read back length: 1 | printed: [a]
```

### F. Time module

```
$ $Z t1.zm
  -0001-01-01 → OK (docs say year is non-negative)      D14
  +999999999-12-31 + 1 day → +1000000000-01-01          D14 (java.time throws)
  +10000-01-01T…Z → Err "invalid second in '+10000-01-01T00:00:'"  (LocalDate accepts, Instant does not)
  +19:00 offset → Err out of range | PT-1.5S → PT-1.500000000S
$ $Z t3.zm   # negative years round-trip through epoch_day; leap rule correct for -4
$ $Z t4.zm   # add_days(…, INT64_MAX) → clean "integer overflow in '+'" runtime error
$ $Z ord.zm
  instant_compare(a,b) = -999999999   but   compare(text(a), text(b)) = 1      D12
```

### G. Performance

```
$ /usr/bin/time $Z fib.zm        (fib(32))       0.57 s   |  python3 -I fib.py        0.15 s
$ /usr/bin/time $Z sieve.zm      (2e6)           0.85 s / 41 MB  |  python  0.27 s / 25 MB
$ /usr/bin/time $Z wc2.zm        (400k map ops)  0.16 s / 14 MB  |  python  0.04 s / 10 MB
$ $Z --profile wc2.zm            → 43.4% mutable_map_get_or, 20.8% mutable_map_get, 5.0% GC
$ /usr/bin/time $Z arr.zm        (400k pushes)   0.29 s / 122 MB |  python  0.03 s / 13 MB
$ for n in 5e5 1e6 2e6 4e6: mutable_array_push vs push   → the table in §8.1
$ $Z --profile sc.zm             (2M strings)    → 88.4% (garbage collection), 9.3% mutable_array_push
$ ziman-nogc sc.zm               (2M strings)    3.64 s / 506 MB   (vs 4.48 s / 455 MB with GC)
$ $Z sci.zm                      (4M packed int) 0.69 s / 95 MB    — no pathology
$ ./tests/bench/server_concurrency.sh ./ziman 18391 3000 "1 2 4 8 16 32 64"
  c=1 471.75 | c=2 806.95 | c=4 1262.22 | c=8 1626.64 | c=16 1689.47 | c=32 1629.03 | c=64 1559.47
$ bench/load.sh                  → the four-column table in §8.1, zero failed requests
$ bench/regburst.sh              → 94 → 201 → 233 → 244 MB over three 64-way registration bursts
$ ./tests/bench/server_load.sh ./ziman 3 18901
  awk: cannot open /proc/2322106/status | health p50=0.0001s | exit 0
  /tmp/server_load_18901.log: ziman: module error: cannot open 'server.zm'        D13
```

### H. Static measurements

```
$ find lib -name '*.zm' | xargs wc -l                      → 19,110 (server.zm 2,577; time.zm 1,496)
$ find $BK/src/main/ziman -name '*.zm' | xargs wc -l       → 20,181 across 209 files; routes.zm 1,888
$ grep -ro "Result<[^,]*, string>" lib --include=*.zm | wc -l   → 307   (was 281, was 242)
$ grep -rc '#\[test' lib $BK/src --include=*.zm | grep -v ':0'  → lib/core/test.zm only
$ grep -c 'crud_endpoints<' $BK/src/main/ziman/routes.zm   → 15 (14 uses; the comment says "thirteen")
$ grep -ln 'crud.Crud<' $BK/src/main/ziman/service/*.zm    → 10 resources, 5 wrappers each
$ ls $BK/src/main/resources/migrations                     → 28 shared + 9 engine pairs; 20 indexes in V024
$ for f in sqlite/*.sql; do diff $f postgres/$(basename $f); done
  → 8 of 9 differ only in TEXT vs uuid on id/patient_id (plus the comment saying so)
$ grep -rn collections docs/*.md                           → TUTORIAL.md:247 (stale), ROADMAP forward text
$ ls docs/lib/examples/collections_example.*               → .out only, no .zm
$ $Z --check /tmp/ziman_review_dev/coll.zm                 → cannot find module 'collections'
$ $Z bench_join2.zm   (40k pieces)                         → native join 1 ms, array_join 2,292 ms, identical
$ timeout 60 $Z t_pool.zm                                  → acquire_timeout: Err(Connection(timed out …))   D2
$ timeout 15 $Z t_hang2.zm                                 → exit 143, still hung                            D1
```

### I. Parity audit method

Spring endpoints extracted by grepping `@(Get|Post|Put|Patch|Delete|Request)Mapping` across 62
controllers (226 method+path pairs); Ziman from `routes.zm`'s 101 literal patterns plus 14 resources × 5
generated (171). Path parameters normalised before diffing. Twelve matching endpoints then compared
side by side on method, path, success and error statuses, error body keys, JSON field names, validation
message text, defaults, list ordering, pagination parameters and auth requirements. Ordering claims
checked against each Spring repository's derived method name and against
`V024__add_indexes.sql`. Artifacts in `/tmp/ziman_review_dev/parity/`.
