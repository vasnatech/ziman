# Ziman developer-experience and code review

Reviewer profile: senior backend engineer (Java/Spring, Go, Python), first contact with Ziman. Method: read SYNTAX.md, ROADMAP.md, the docs/lib pages and the DESIGN headings; read the library (`lib/**`), the health-record Ziman backend (`health-record-backend-ziman`) side by side with the Spring backend; ran the backend's test suite, the language fixture suite, and about 60 small experiments (scratch files under `/tmp/ziman_review_dev/`). Nothing in the repos was modified except this report. "Verified" means I ran it; "inferred" means I only read the code.

## 1. Executive summary

1. Ziman is a coherent, unusually well-documented small language. Diagnostics are excellent, `--check`/`--fmt`/`--lsp` exist, and both test suites are green (fixtures: 660 passed, 0 failed; backend: 12 Python suites, about 725 checks, all passing on SQLite).
2. The Ziman backend is a well-layered, security-conscious port of about 22 resources, with cross-tenant, forged-token and role tests. It is not yet a drop-in for the Spring backend.
3. Verified P0 defects: (a) any `Instant` with 1-6 fractional digits (including the web frontend's `toISOString()` output `.000Z`) is rejected with 400; (b) a valid date near year 9999 persists a row that can then never be read or deleted (500 forever); (c) an unauthenticated client can forge audit-log events through newline injection; (d) no CORS support, so a browser on another origin gets 405 on preflight.
4. Runtime robustness is the largest platform risk. Recursion deeper than about 1,450 frames segfaults with no message. Any runtime error in any request thread terminates the whole server process. Runtime errors carry no file name and no stack trace.
5. Performance is modest and does not scale across cores: roughly 500-650 req/s for an authenticated read on a 16-core box, flat from 1 to 8 client threads. A simple loop is about 28x slower than CPython. There are zero SQL indexes in the app's migrations.
6. Ergonomics friction is real but bounded: 591 explicit generic call sites, 158 hand-written `Err(e) -> ServiceError.Storage(e)` blocks (no `map_err`), 12,400 lines for what Spring does in far less because of per-resource copies of six layers.
7. Docs are a strong learning path (A-), with a few stale statements and missing cost notes (`push` is O(n)).
8. Verdict: fine for a research or internal tool with a proxy in front and a supervisor restarting it; not ready to replace Spring/Go for a production health-data service. Fix the P0 list first (each item is small).

## 2. Learnability and ergonomics

**How I learned it.** SYNTAX.md (3,174 lines) is a reference with rationale inline; the "why" paragraphs are unusually useful, and errors that mimic other languages give direct advice (for example `"a" + "b"` says "use concat(a, b)", `p.x = 2` explains there is no field assignment, `for i in xs { xs[i] }` explains that `i` is the element). I could write working programs after about an hour. The docs/lib pages, generated from source comments, are accurate where I spot-checked them.

**Docs gaps and errors found (verified unless noted):**
- SYNTAX.md ~1122-1126 still says "`Hash` is scalar-only ... a Map key is effectively restricted to int/float/bool/string/bigint/decimal", contradicting the Maps section and ROADMAP R12 (structural Hash is done). Stale.
- The array idiom `nums = push(nums, x)` is presented as the way to build arrays (SYNTAX ~398) with no cost note. It is O(n) per push: 50,000 pushes took 620 ms and 100,000 took 1,970 ms (quadratic). `ArrayBuilder` is only mentioned in passing.
- Float literals have no exponent form (`1e21` gives "expected ')', found identifier" at the identifier, a poor message). `str(float)` prints 6 significant digits (`str(0.1+0.2)` is `0.3`, `str(1.0/3.0)` is `0.333333`, `format("{}", x)` likewise) while `json_to_string` prints the shortest exact text. This is documented deep in the `number` section but is a footgun in the main docs.
- Strings are bytes: `len("héllo")` is 6, `substring` can cut a UTF-8 character in half, `char_at` returns half a character, `to_upper("straße")` is `STRAßE`. Documented in the prelude header, not prominent in SYNTAX.md. Spring-style `size(min=8)` on a password counts bytes here (verified by reading `len(req.password) < 8`).
- Naming rules (SYNTAX "Naming conventions") say qualified calls should be short verbs, but `csv.parse_csv`, `toml.parse_toml`, `yaml.parse_yaml`, `json.parse_json` stutter (verified: `csv.parse` does not exist). Inconsistent with `regex.compile`/`client.get`.
- `print(uuid)` shows `uuid::Uuid { bytes: [105, 128, ...] }`, `print(instant)` shows the internal struct. There is no display protocol, so debugging output of domain types is poor.
- Runtime-error location is `line:col` only (no file), see section 5.

**Language features I kept fighting:**
- `?` only as a whole `val` initializer or a bare statement. `str(a()?)` and `a()? + b()?` are rejected (verified). Every fallible step needs a temporary. The docs state honestly that this is an interpreter constraint.
- Same-`E` rule for `?` plus no `map_err`/`and_then`/`map` on `Result`/`Option` (verified: `map_err` undefined; only `option_unwrap_or` exists). The backend pays 158 times: `Err(failure) { ServiceError.Storage(failure) }` (76) and `return ServiceError.Storage(failure)` (82). Generics and lambdas already exist, so this is a ten-line library addition.
- Explicit generic type arguments everywhere: 591 call sites in the backend, many redundant (`to_response<model.AllergyIntoleranceResponse>(ctx, service.create_allergy(...))` where the argument's type determines it). `collections.sort([3,1,2])` is an error.
- No tuples: `[[string]]` stands in for pairs (`count_per_day`, `logins_by_day` return `[[string]]` and re-parse numbers with `parse_int(pair[1])`, `service/system.zm`, `service/audit.zm`).
- No bitwise operators: `otp.zm` hand-rolls shifts with `divisor` loops (`base32_encode`, `hotp`), and `counter_bytes` builds digits with `push` in a loop.
- No string switch, so enum words are validated with `val types = ["ALLERGY", ...]` and `not_one_of`; the domain enums are strings in the entities (documented as "derive Row has no enum support"), which loses compile-time exhaustiveness for statuses and roles (`user.role == "ADMIN"`, `user.status == "PASSWORD_RESET"` in several files).
- Auto-wrap is pleasant (`return problem` in a `Result` function) but inconsistently used: `broken_rule` functions return `Option<ServiceError>` with explicit `Option<ServiceError>.Some(...)` (56 `Option<..>.None` occurrences) even though a bare value would wrap.
- Immutability: fine for data, but every accumulation needs `ab_new/ab_append/ab_to_array` (129 lines in the backend).

**Rating of the manuals as a way to learn:** 8/10. Strong on rationale, examples, decisions and error text; weak on cost model, runtime limits (stack depth, process-fatal errors) and cross-references between stale sections. There is no tutorial path; the reference is long, so a "first hour" guide would help.

## 3. Code quality of the Ziman application

### 3.1 Layering, naming, duplication
Layers are clean: routes, controller (thin, takes `Principal`), service (rules), repository (SQL), mapper, model (DTO with `#[derive(Json)]`), entity (`#[derive(Row)]`), `common/Ctx`. Ownership is enforced structurally: every query is `WHERE id = $1 AND patient_id = $2` and `patient_id` comes from the principal, never the body (verified by tests and by cross-tenant 404 checks in `resources_test.py:937-943`). Explicit `ctx with { conn: tx }` for transactions is a good pattern.

Duplication is heavy. Line counts: controller 1,128, service 3,837, repository 1,896, mapper 1,550, model 945, entity 471, routes 2,172 (12,434 total). `controller/allergy.zm`, `service/allergy.zm` and `repository/allergy.zm` differ from their siblings only by names and word lists (a `diff` of `service/encounter.zm` against `service/medical_procedure.zm` after renaming shows only the validation lists differ). `routes.zm` is about 18 lines per endpoint, 117 endpoints, with the same encoder lambda repeated for every route (`fn(value) { json_to_bytes(x_to_json(value)) }`).
Acceptable? For a dogfooding exercise yes; for maintenance no: a column addition touches entity, model (two structs), mapper (two functions), repository (two SQL strings with 15-17 numbered placeholders), migration, and test table. What to extract, in order:
1. `storage_error<T>(Result<T,DbError>) -> Result<T,ServiceError>` (removes 158 blocks).
2. A generic `Crud<E, Req, Res>` record of function values (table name, columns, `from_row`, `to_params`, `validate`, `from_request`, `apply`, `to_response`) so list/get/create/update/delete for the 12 "generated" resources is written once. Ziman already has `Endpoint<C>` built the same way.
3. A `crud_endpoints<...>(prefix, ...)` helper producing the five route entries, and a `json_encoder<T>(to_json)` helper in `routes.zm`.
4. Generate SQL column lists from the derive (or an `INSERT` builder) so the 17-placeholder strings cannot drift from the entity.

### 3.2 Error handling and status mapping
`ServiceError` (Validation, Storage, BadRequest, Unauthorized, Internal) plus one `from_service_error` is a good design. Divergences and defects (verified):
- **Only one validation error is reported.** `PUT /api/patients/me` with a bad email and a future date returned only the email error; register with `name:""`, `email:"bad"`, `password:"x"` returned only `name`. Spring's Bean Validation returns all field errors in one `errors` object.
- **Type errors do not name the field**: `{"email":5}` gives 400 `expected a string` (README promises "naming the field"). A bad instant says `'...' is too short to match the given format`.
- **Raw database messages reach the client**: concurrent register of one email returned 409 `constraint violation: UNIQUE constraint failed: users.email` (table and column names). The README says details go to the log only; that holds for 500s but not for 409/404.
- **Delete of a missing record is 404** here; Spring's derived `deleteByIdAndPatientId` silently returns 204 (read, not run).
- Library-generated errors (431, malformed header) use `{"status":..,"error":..}`, a different shape from the app's Spring-shaped `{timestamp,status,message}`.
- Errors for unknown JSON fields: silently ignored (same as Spring's default), but here it means data loss: `POST /api/me/medications` with `prescribedBy` and `color` returns 201 and drops both (the Ziman `MedicationResponse` has 12 fields versus Spring's 30+; no `rxNormCode`, `color`, doses, refills, adherence). The README calls medications "deliberately simplified", yet allergies are complete; the most used resource is the least complete.

### 3.3 Validation
Email check is `contains(email, "@")`: `"a b@ c"` registered successfully (verified). Emails are case-sensitive: `al@x.com` and `AL@x.com` are two accounts (verified; Spring likely the same on Postgres, inferred). Password minimum counts bytes. No maximum lengths on any string field. No trimming.

### 3.4 Security (also listed separately in section 6)
Good: Argon2id with rehash-on-login and bcrypt compatibility; dummy hash on unknown user (timing); role and status read from the DB on every request (suspension is immediate, verified by `admin_test.py:106`); `alg:none`, wrong alg, wrong key, expired and `mfa_pending` tokens rejected (my own probes agree); only parameterized SQL (the only string-built SQL is `repository/stats.zm`, whose table names are constants); impersonation token cannot reach admin routes; startup validates secret length and unknown config keys.
Weak: see section 6.

### 3.5 Data access
- **No indexes at all** in migrations V001-V022 (verified with grep). Every `WHERE patient_id = $1` is a full scan on Postgres; `users.email` has a UNIQUE index only.
- **Schema is SQLite-shaped**: every id, date, timestamp and enum is `TEXT`, booleans are `INTEGER`. On Postgres nothing benefits from native `uuid`/`timestamptz`/`date`, and ordering is by text. Text ordering of timestamps is actually wrong here: `instant_to_string` omits the fraction when nanos is 0 (`...:47Z`) but otherwise prints 9 digits (`...:47.5Z`), and `'Z' > '.'`, so `ORDER BY effective_start DESC` mis-orders whole-second versus fractional values (inferred from the format; I could not create fractional events because of the parser bug in P0-1, but the emitter output is verified).
- **N+1**: `get_lab_panels` runs one results query per panel (`service/lab_panel.zm`, `get_lab_panels`); pregnancy records do the same for events.
- **Read-back round trips**: `save` is INSERT + SELECT, `update` is SELECT + UPDATE + SELECT, `delete` is SELECT + DELETE (the DELETE's row count is ignored, so the first SELECT exists only to create the 404). All non-atomic.
- **Transactions**: only three uses of `with_transaction` (register, delete user, test data). Multi-statement operations run outside one: `delete_lab_panel` (delete results, then panel) and `delete_pregnancy_record` (events, then record) can leave partial state; insurance "at most 2 ACTIVE" is check-then-insert with no constraint (race).
- **Unbounded lists** (no paging): `GET /api/me/health-events` returns every row; that table is designed to be fed by wearables.
- `row_get` is a linear scan by column name, so derived `from_row` is O(columns²) per row (minor).
- Admin `logs`/`audit` read the entire log file into memory per request and keep only the last N (`service/audit.zm:log_lines`); log rotation is left to the operator.

### 3.6 Configuration and migrations
Config is good: profile files, environment override, unknown-key rejection, startup report with secrets masked, `${VAR:default}`. Weakness: the default `app.jwt.secret` in `application.properties` is a public constant (`QUFB...` = 32 bytes of `A`); nothing refuses to start or warns when it is in use (verified that I could forge an admin request with it). The migration runner is solid (checksum, lock, contiguous versions, per-dialect directories), but the app uses a single shared dialect-neutral schema.

### 3.7 Testing
Strengths: table-driven `resources_test.py` (417 checks) covering 12 resources with create/list/get/update/validation/enum/filters/cross-user; forged-token matrix; admin RBAC; independent TOTP and HMAC implementations in the tests; real servers per suite; SQLite/Postgres switch via `dbshim`.
Gaps: no unit tests in Ziman itself (the language has no test framework or `assert`); nothing for concurrency or load; nothing for MFA brute force or replay; no test that a fractional-second `Instant` works (which is why P0-1 shipped); no year-boundary tests; Postgres run not exercised by me (did not run `--postgres`); no coverage tool; no CORS tests; medication and patient tests are folded into other suites.

### 3.8 Consistency with the Spring API (endpoint coverage)
Ziman has 117 method/pattern entries (65 patterns). Missing versus Spring (from the controller listing): `/api/users/me` profile, preferences, password change, convert-to-local, delete self; medication doses, refills and interaction-check (`GET /api/me/medications/interaction-check` is routed as `{id}` and answers 400 `invalid medication id`); emergency-summary; dashboard, export, sharing, integrations, notifications, nutrition, documents, assessments, terminology, admin database and medical-code import, config, well-known. Notably **there is no password-change endpoint**, yet admin reset-password puts the account in `PASSWORD_RESET` "so it must choose its own at next login": the user cannot ever do so (verified: no such route). Registration does not create user preferences (Spring does), MFA `timezone`/`language` are accepted and dropped.

## 4. Code quality of the library (Ziman-written)

Overall good: readable, commented per function, sensible use of `defer`, no dead code found in a skim (one TODO-free tree), strong fixture coverage (593 `.zm` fixtures; 316 golden `.out`, 314 `.err`), regexp is linear-time and stayed fast on `(a+)+$`, JSON nesting of 100,000 returned a clean error, sort is stable (verified), redirect handling drops `Authorization` cross-host (read).

Issues:
- **Hidden performance**: `server.dispatch` scans the whole endpoint table per request and re-splits each pattern string on every attempt. Measured on the app (117 endpoints): `/health` (early in the table) 0.75 ms, an unknown path 4.0 ms (about 25 microseconds per endpoint plus building the 405 description list). Precompiling patterns would remove it. `with_header` uses `push` (quadratic in headers, tiny). `row_get` linear. `bytes_concat(buf, chunk)` per read (bounded at 16 KiB, fine).
- **Awkward code forced by the language**: base32/HOTP shifts (`otp.zm`), `counter_bytes`, `[string]` pair returns, `option`-typed error returns.
- **Inconsistent API names**: section 2. `regex.replace/split` shadow prelude names if imported bare.
- **Print/debug ergonomics**: no display for `Uuid`/`Instant`.
- **JWT** (`lib/core/jwt.zm`): a token without `exp`, or with a non-numeric `exp` (`"exp":"x"`), is accepted forever (`seconds_claim` returns None and the check is skipped). No `require_exp` option. Not exploitable without the key, but it turns any signing mistake into an immortal token (read).
- **JSON**: `parse_json("01")` accepts leading zeros; integers above 2^63 silently become floats (`12345678901234567890` becomes `JNumber(1.23457e+19)`, precision lost); `[1e999]` parses to `inf`.
- **Time**: `localdate_to_string` can emit a 5-digit year that `localdate_from_string` refuses (root cause of P0-2). `instant_from_string` only accepts exactly 9 fractional digits (P0-1). Error text does not say "expected ISO-8601".
- **Missing tests** (fixture names inspected): no fixture for fractional-second Instants in 3/6-digit forms, year-10000 round trip, duplicate Content-Length, recursion depth, header-value CRLF.

## 5. Productivity and maintainability verdict

**Speed of building.** A CRUD resource costs about six new files, about 500 lines, copied from a template; with a codegen or the extractions above that could drop by 70 percent. For a Spring team the concepts map one to one (controller/service/repository/mapper/DTO), which is why the port was fast. Type-check of the entire 12k-line app takes 0.09 s (verified), formatter and LSP exist; startup UX (report of effective config with sources) is better than Spring's.

**Versus Spring/Go today:** roughly 2-3x slower to build features (boilerplate, no mapper generator, no validation annotations, no ORM, no OpenAPI), and materially slower to debug (below). Runtime is much lighter in memory (RSS 175 MB flat over 8,000 requests, verified stable, so the GC works) but far slower in CPU.

**Measured performance (verified, one machine, SQLite file, pool of 4):**
- Authenticated `GET /api/me/allergies`: 496 req/s with 1 client thread, 654 with 4, 506 with 8 (16 cores). Parallelism does not help (cause not investigated; suspects: GC allocation lock or the single connection-owner thread per DB connection).
- Micro: 3,000,000-iteration integer loop 3.9 s versus CPython 0.14 s (about 28x slower); 50,000-element `Map` build 75 ms; sort of 50,000 ints 27 ms.

**Missing tooling:** no debugger, no stack traces (runtime errors print `ziman: runtime error at 3:13: ...` with no file and no call chain; verified with a two-module program), no profiler, no coverage, no test runner or assertion library, no package manager or lockfile (modules are files, the bundled core is embedded), only one lint rule (`var` never reassigned), no OpenAPI or schema tooling, no metrics beyond hand-rolled `/proc` reads, no hot reload. LSP and formatter are present.

**Top adoption risks:**
1. Process-fatal runtime errors: any `unwrap`, index error, integer overflow (traps by design) or stack overflow in any handler thread ends the process (verified in a spawned thread: exit 1). One latent bug equals a full outage. Needs per-request isolation or a recoverable-panic boundary.
2. Stack limit around 1,450 Ziman frames on an 8 MB stack, segfault with no message (verified: 1,000 OK, 2,000 crash; about 5.8 KB of C stack per Ziman call).
3. Single-maintainer, single-implementation language and library (`lib` is 14.8k lines, C runtime 20k lines); no ecosystem, no security-advisory process, TLS server side missing (`listen_tls` not yet), no HTTP/2, no chunked-request gaps beyond the documented ones.
4. Performance ceiling (above) with no compile path.
5. Compatibility drift from a moving language (frequent syntax changes in the ROADMAP log).

## 6. Bugs and security issues

Format: ID, severity, reproduction, suggested fix. All verified unless marked (inferred).

### Defects

**B1 (High) Instants with a fractional part other than 9 digits are rejected.** `POST /api/me/health-events {"type":"STEPS","effectiveStart":"2026-01-01T08:00:00.000Z","value1":1}` returns 400 `'...' is too short to match the given format`. Same for `.5Z`, `.500Z`, `.123456Z`, and `08:00Z`. The web frontend's `toInstant` (`src/utils/dateFormatters.ts:95-107`) returns `new Date(..).toISOString()`, which always ends in `.000Z` (read), so every Instant field the web app posts would fail. Fix: `instant_from_string` should accept 0-9 fractional digits (pad), and `+HH:MM` offsets already work; add fixtures for 0/3/6/9 digits; improve the message.

**B2 (High) Date near year 9999 poisons the record.** `POST /api/me/pregnancy-records {"status":"ACTIVE","startDate":"9999-06-01"}` returns 500; the row is inserted with `estimated_due_date = '10000-03-07'`; afterwards `GET` (list) and `DELETE /{id}` for that user both return 500 forever. Cause: `localdate_to_string` writes 5-digit years, `localdate_from_string` requires 4. Fix: reject years outside 1..9999 in `localdate_of`/`localdate_add_*` (or make both directions symmetric), validate date range in the service, and make delete not depend on parsing derived fields.

**B3 (High) No CORS.** `OPTIONS /api/auth/login` with `Origin` and `Access-Control-Request-Method` returns 405. Spring has `app.cors.allowed-origins`. A separate-origin browser (Vite dev server) cannot use the API. Fix: an option in `server.dispatch` or app middleware answering preflight and adding `Access-Control-Allow-*` headers, plus security headers.

**B4 (High) Recursion crash without diagnostic.** `fn f(n:int)->int{ if n==0 {return 0} return 1+f(n-1)}` crashes (SIGSEGV, exit 139, no output) at depth about 1,452 on the main thread. Fix: a depth counter with a clean runtime error (or run the interpreter on a big stack), and document the limit.

**B5 (High) Runtime error in any thread kills the process.** Spawned thread with `a[1]` on an empty array prints the error and exits 1 while main waits. In the server every request runs in such a thread. Fix: catch per-connection (turn into a 500 and log), or offer `recover`.

**B6 (Medium) Integer literal overflow is silent.** `print(9223372036854775808)` prints `9223372036854775807`; `print(-9223372036854775808)` prints `-9223372036854775807`. Fix: lexer error on out-of-range literal (special-case the negated minimum).

**B7 (Medium) Conflicting duplicate `Content-Length` accepted** (`lib/http/server.zm` around 341, `header_value` returns the first). Sent `Content-Length: 5` then `Content-Length: N` with a body of N bytes: the body was truncated at 5 and the rest treated as the next request. `Content-Length: +N` also accepted. Also no `Host` required for HTTP/1.1, and bare LF inside the head is tolerated. Behind a proxy this is a request-smuggling primitive. Fix: reject differing duplicates, digits only, require Host.

**B8 (Medium) Response header values are not validated** (`with_header` / `write_response` append raw `name`/`value`, `server.zm:412,549`). CRLF injection if an app passes user input into a header (inferred; not exploitable in this app).

**B9 (Medium) Timestamp text ordering** (see 3.5), because nanos=0 drops the fraction; fixed-width would sort correctly. (inferred impact, verified format)

**B10 (Medium) No password-change endpoint** although reset marks `PASSWORD_RESET` (see 3.8). Also existing tokens survive a password reset (tokens are stateless, verified by reading; `authenticate` never compares issue time with a password-change time).

**B11 (Low) `interaction-check` is captured by `/{id}`** and returns 400 instead of 404/501.

**B12 (Low) Only the first validation error is returned; type errors omit the field name; raw constraint messages leak schema** (3.2).

**B13 (Low) Delete-then-check pattern and non-transactional cascades** (3.5).

**B14 (Low) `str(float)` lossy, JSON big integers become floats, `parse_json("01")` accepted, `parse_int(" 5")` and `"+5"` accepted** (verified).

**B15 (Low) Stale docs and comments**: `model/auth.zm` says `mfa_required` and `mfa_token` "nothing sets them yet" and `timezone`/`language` "not stored" (MFA is implemented); SYNTAX Hash paragraph (section 2).

### Security issues

**S1 (High) Unauthenticated audit-log forgery.** `POST /api/auth/login` with `email` = `"x\n2026-09-27T00:00:00.000Z INFO AUTH_LOGIN_SUCCESS email=ceo@victim.com"` writes a second log line; `GET /api/admin/audit` then lists a forged `AUTH_LOGIN_SUCCESS` for `ceo@victim.com`, and `logins_by_day` metrics count it. Causes: `log.write` does not escape newlines; the audit trail is reconstructed by parsing free text (`service/audit.zm:parse_event` matches an event name anywhere in the message). Fix: escape or quote user values in log lines (library level: replace `\r`/`\n`), write audit events to a structured store (table) or JSON lines, and do not parse them out of the general log.

**S2 (High if deployed as-is) Public default JWT secret.** `application.properties` ships a fixed key; I forged a valid admin request against a local server with it. Nothing warns. Fix: no default (fail startup), or refuse the known constant.

**S3 (Medium) No rate limiting or lockout**: 30 wrong logins in a row all answered 401 with no delay; MFA `verify`/`verify-backup` have no attempt counter (a 5-minute pending token allows repeated 6-digit guesses against 3 valid codes; feasibility depends on throughput of about 500 req/s, so at 5 minutes it is about 150k of 1M guesses, before retrying with a fresh password login); a TOTP code can be replayed within its window; `verify_backup` runs up to 8 Argon2 verifications per attempt (CPU amplification). Fix: per-account and per-IP throttle, store last used TOTP step, cap attempts on the pending token.

**S4 (Medium) TOTP secrets stored in plaintext** in `user_mfa.secret` (Argon2 would not apply; needs encryption at rest with a key from config). `disable` requires only a TOTP code, not the password (matches Spring, inferred).

**S5 (Medium) Slow-client DoS**: `max_connections` 64 with one thread each and a per-read idle timeout of 30 s means 64 slow sockets (a byte per 29 s) occupy the server (inferred from code; not run).

**S6 (Medium) Account enumeration remains** through the wrong-provider branch (401 `This account uses GOOGLE login`), and registration duplicate answers 400 (documented tradeoff). The README claims no enumeration; only the unknown-email/wrong-password case is covered.

**S7 (Low) Admin operations**: no guard against an administrator demoting, suspending or deleting themselves or the last administrator; `ADMIN_*` audit lines do not carry the acting admin's email (except impersonate); `ADMIN_USER_DELETE` is logged before the transaction commits, so the log can claim a delete that failed.

**S8 (Low) Missing hardening headers** (`X-Content-Type-Options`, HSTS), and a `Server: ziman` banner.

**S9 (Low) JWT**: tokens without a valid numeric `exp` never expire (library, section 4); no `iss`/`aud`.

## 7. Prioritised action list

**P0 (before anyone depends on the Ziman backend)**
1. B1 fractional-second Instants. Effort: 0.5 day (parser + fixtures). Rationale: breaks the real web client on day one.
2. B2 year-range symmetry and validation, plus test. 0.5 day. Permanent per-user damage from valid input.
3. S1 log escaping and structured audit store. 1-2 days. Trivial unauthenticated forgery of the audit trail.
4. S2 remove the default JWT secret. 1 hour.
5. B5 + B4: per-request panic boundary and stack-depth guard. 3-5 days (interpreter work). One bad request equals an outage.
6. B3 CORS (or document a mandatory same-origin proxy). 0.5 day.
7. Password-change endpoint (B10) and revoke-on-reset (add a `password_changed_at` claim check). 1 day.

**P1 (next iteration)**
8. Add indexes on every `patient_id` and the sort columns; use native Postgres types in a per-dialect migration set. 1-2 days.
9. `storage_error` helper plus `Result.map_err`/`and_then`/`map` in prelude; drop 158 blocks. 1 day.
10. Multi-error validation (accumulate a list of `FieldError`s), field names in decode errors, hide constraint text. 1-2 days.
11. Transactions for cascades and the two-active-insurance rule (unique partial index where the engine allows). 1 day.
12. Rate limiting, MFA attempt cap and TOTP replay guard, TOTP secret encryption (S3, S4). 2-3 days.
13. HTTP hardening: B7 (duplicate/plus Content-Length, Host), B8 header validation, total header-read deadline for slowloris. 1-2 days.
14. Compile route patterns once and dispatch by method/first segment (about 3 ms saved per unmatched request, 25 microseconds per endpoint). 1 day.
15. Runtime errors with file name and a stack of frames; a tiny `test` module (`assert_eq`, a runner). 3-5 days.
16. Extract `Crud<E,Req,Res>` and `crud_endpoints` to cut the six-layer duplication. 3-5 days.
17. Fill the Spring gaps that clients need: `/api/users/me*`, medication fields (`color`, codes), doses, refills. 1-2 weeks.

**P2 (later)**
18. Lexer error for out-of-range integer literals (B6); float exponent literals; `str(float)` to shortest text (or document loudly).
19. JWT `require_exp`; JSON leading-zero and big-integer handling.
20. Nested `?` support (or `try` blocks), inference of generic type arguments from arguments, tuples, bit operators.
21. Structured pagination for `health-events` and similar high-volume lists; log rotation and bounded log reads.
22. Display protocol for `Uuid`/`Instant` in `print`; naming clean-up (`csv.parse`).
23. Docs: fix the stale Hash paragraph and `model/auth.zm` comments, add cost notes (`push`), runtime limits, and a first-hour tutorial.
24. Profiler, debugger, coverage, package/version story.

## Appendix: experiments and commands

Repo state: `git status` before and after showed only the pre-existing untracked `docs/reviews/language-design-review.md` (not mine) in the ziman repo and nothing changed in health-record. I started three local servers (ports 18099, 18098, 18097, SQLite files and log files under `/tmp/ziman_review_dev/`, config via environment variables) and stopped all of them; final `ps` shows no `ziman serve` process (only the user's editor `ziman --lsp`). No containers were started; `--postgres` was not run.

1. `tests/run-all.sh` in the backend: PASS admin (54), allergy (31), audit (17), auth (47), broadcast (22), immunization (26), lab (30), lifecycle (15), mfa (33), resources (417), rules (36), testdata (24); "sqlite: 0 failed".
2. `tests/run_tests.sh` in ziman: `660 passed, 0 failed`.
3. Server smoke: `curl localhost:18099/health` gives `{"status":"ok"}`. Register/login worked; response included `mfaRequired:false`.
4. Duplicate register `al@x.com` then `AL@x.com`: both 200. `email:"a b@ c"`: 200. `name:"" email:"bad" password:"x"`: only `errors.name`. `{"email":5}`: `expected a string`. Not JSON: `malformed JSON: invalid value 'nope' at position 0`. No Content-Type: 415.
5. Log forgery: login with newline in email, then `GET /api/admin/audit` (after promoting my scratch user in my own `/tmp` SQLite DB) listed `AUTH_LOGIN_SUCCESS ... ceo@victim.com`.
6. JWT: token HS256 signed with 32 bytes `A` accepted on `/api/admin/users` (200). `alg:none` and HS512 with the wrong key: 401.
7. 30 consecutive wrong logins: 401 x30, no throttling.
8. Concurrent register of one email (6 and 4 parallel): one 200, others 409 `constraint violation: UNIQUE constraint failed: users.email`.
9. Health-event instants: `...:00Z` OK (201); `.5Z`, `.500Z`, `.000Z`, `.123456Z`, `T08:00Z`: 400 `is too short to match the given format`; `.123456789Z` and `+02:00` OK (201); no zone: 400 `invalid zone offset ''`.
10. Medication with unknown fields `prescribedBy`, `color`: 201, fields absent from the response. `expectedIntervalHours:0`: 400 `must be greater than 0`; `9223372036854775808` and `1.5`: 400 `expected an integer`; `9223372036854775807`: 201.
11. Pregnancy record `startDate` `9999-12-31`, `9999-12-30`, `9999-06-01`: 500 each; DB rows have `estimated_due_date` `10000-10-06` etc.; subsequent list and DELETE: 500. Log: `invalid LocalDate value: '10000-10-06' does not match expected literal '-' at position 4`. `2026-01-01`: 201 with due date `2026-10-08`.
12. `OPTIONS /api/auth/login` with CORS headers: 405 with `Allow: POST`.
13. Raw sockets: duplicate Content-Length (5 then N): first value used, `malformed JSON: unterminated string`; `Content-Length: +N`: accepted; bare-LF header: 200; `%00` path: 404; `/api/me/allergies/%2e%2e`: 401; header name with space: 400 `{"status":400,"error":"malformed header name"}`; no Host: 200; 20 KB URL: 431.
14. Latency (300 sequential keep-alive requests): `/health` 0.75 ms; `/nope` 4.0 ms; `/api/admin/test-data/presets` 3.4 ms; `/api/patients/me` 2.5 ms; `/api/me/allergies` 2.2 ms. Throughput 1/4/8 client threads: 496/654/506 req/s. RSS 175,440 kB before and after 2 x 4,000 requests.
15. Language: `str(0.1+0.2)` 0.3, `str(1.0/3.0)` 0.333333, `json_to_string` 0.30000000000000004; `len("héllo")` 6; `substring("héllo",0,2)` invalid UTF-8; `to_upper("straße")` `STRAßE`; `split("", ",")` `[]`; `parse_int(" 5")` `Ok(5)`; `1.0/0.0` runtime error "division by zero"; `parse_float("nan")` `Ok(nan)`; `print(9223372036854775808)` `9223372036854775807`; `x+1` at INT_MAX: runtime error (good).
16. Recursion: depths 1000 OK, 2000 to 9000 exit 139 (segfault), bisected threshold about 1452; inside a spawned thread depth 900 OK; with `ulimit -s unlimited` depth 100,000 OK.
17. Thread panic (`a[1]` in a spawned function): message printed, process exit 1. Two-module runtime error prints `ziman: runtime error at 3:13: index 3 out of bounds (array length 0)` with no file or frames.
18. Diagnostics probes (all clear, accurate messages): `Result<int,int>` bare value, `str(a()?)`, `a()? + b()?`, `xs[i]` in `for i in xs`, missing switch arm, `collections.sort` without type args, missing struct field, mixed `if` arm types, lambda capturing a `var`, `p.x = 2`, `a[0] = 5`, `1e21` literal (poor message).
19. Performance: `push` x50,000 620 ms, x100,000 1,970 ms; concat x20,000 52 ms; map+sort 50,000 27 ms; Map set x50,000 75 ms; 3M-iteration loop 3,928 ms versus CPython 141 ms.
20. Library: sort_with stable; UUID parse accepts upper case, rejects unhyphenated, braces, trailing space; CSV ragged and unterminated rows give clear errors; regex `(a+)+$` on a hostile string returns quickly; JSON: 100,000 nested `[` returns an error (no crash), `😀` decoded, lone surrogate and `\u0000` rejected, `01` accepted, `12345678901234567890` becomes a float, duplicate keys last wins.
21. Time formats: `instant_to_string` emits `...47Z` for nanos 0, `.860000000Z` otherwise; year 10000 prints as `10000-01-01T00:00:00Z`; `localdate_from_string("2026-2-3")` errors.
22. Tooling: `ziman --check serve.zm` 0.09 s for the whole app; `--fmt` is a no-op on `routes.zm` and `service/auth.zm` (formatted); `--lint serve.zm` clean.
23. Spring comparison: `AllergyIntoleranceController`/`Service`/`Request`, `AuthService`, `RegisterRequest` (`@NotBlank @Email`, `@Size(min=8)`), `MedicationRequest`/`Response`, controller route listing (277 mappings) versus `grep -o 'pattern: ...' routes.zm` (65 patterns, 117 methods).
