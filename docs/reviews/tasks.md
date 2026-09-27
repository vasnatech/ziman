# Tasks from the 2026-09-26 reviews

The two reviews merged and de-duplicated: [language-design-review.md](language-design-review.md) (**L**) and [developer-experience-code-review.md](developer-experience-code-review.md) (**D**). Items refer to the reports' own numbering (`L P0-1` is the language review's recommendation P0-1, `D B4` the developer review's bug B4, `D item 9` its action-list item 9, `D S1` its security issue S1).

Effort: **S** = a day or less, **M** = a few days, **L** = a week or more. Tick a box when the task is done; note the commit beside it.

Suggested order: A and B first (B1, B2, B4 and B5 are each about a day or less, and the web client depends on some of them), then C1, C2 and D1, which remove the most everyday friction, then E.

## A. Ziman runtime and safety (P0)

- [x] **A1** Stack-depth guard: a clean "stack depth exceeded" error instead of a segfault, from a per-thread limit read at run time (`src/stack.c`); the main interpreter thread and each `spawn` run on stacks sized from usable memory, thread count and `--stack-mb`/`--thread-stack-mb`/`--threads` (or the `ZIMAN_*` variables); parser, checker and deep values are guarded too. (L P0-1, D B4; S-M)
- [x] **A2** Fatal-error boundary per thread and per request: a runtime error in a spawned thread ends that thread (logged to stderr, its defers run) and the process stays up; `spawn f(x) on_error handler` chooses another response per spawn (`os.fatal` keeps the old exit); `server.serve` answers 500 and releases the connection. (L P0-4, D B5; M)
- [x] **A3** Builders stay in their thread (chosen over locking): `#[thread_bound]` marks the type; compile errors for a `Channel` element, a `spawn` argument and a module-level `val`; a runtime owner check on every builder operation; docs corrected. `Iter<T>` is thread-bound too. (L P0-2; S)
- [x] **A4** Numeric holes: range-check int literals (including the negated minimum), saturate `float as int`, convert `float as bigint` exactly, reject `%` on floats in the checker. (L P0-3, D B6; S)
- [x] **A5** Fix the prelude namespace leak: a user struct named `T`, `K`, `V`, `R` or `U` fails to compile and the error names no file. (L P0-5; S-M)
- [x] **A6** Make `arr = push(arr, x)` amortised O(1) (200,000 pushes: 8.5 s, against 0.23 s with `ArrayBuilder`). (L P0-6, D; M). `push` shares spare capacity through a tip check (`ArrayStore`, CAS on `used`); 200,000 pushes: 0.2 s. `--lint` flags the still-quadratic `x = concat(x, ..)` and `x = concat_arrays(x, ..)` in a loop.

## B. Backend bugs and security (P0)

- [x] **B1** `Instant` parsing: accept 0-9 fractional digits (`.000Z` from the web frontend returns 400 today); add fixtures for 0, 3, 6 and 9 digits; improve the message. (D B1; S). `localtime/localdatetime/instant_from_string` take 1 to 9 fraction digits (`.5`, `.000`, `.123456`); 0 or over 9 digits gets a clear message; fixture `time_instant_fraction_digits`, backend checks in `resources_test.py`.
- [x] **B2** Date range: a year near 9999 stores `10000-..` that cannot be read back, which permanently breaks that user's list and DELETE. Make `localdate_to_string` and `_from_string` symmetric and validate the range. (D B2; S). Chose symmetry: a year outside 0-9999 is written with a sign and read back (`+10000-03-07`, `-0044-03-15`, 4 to 9 digits) by every date, date-time and instant format; fixture `time_year_range_roundtrip`, backend checks in `resources_test.py`.
- [ ] **B3** Audit-log forgery through a newline in the login email: escape control characters in `log.write`, and stop rebuilding the audit trail from free text (use a table or JSON lines). (D S1; M)
- [ ] **B4** Remove the public default JWT secret: fail startup, or refuse the known constant. (D S2; S)
- [ ] **B5** CORS: answer preflight requests and add `Access-Control-Allow-*` headers, configurable like Spring's `app.cors.allowed-origins`. (D B3; S)
- [ ] **B6** Password-change endpoint, and revoke tokens on reset (`PASSWORD_RESET` can never be cleared today). (D B10; S-M)

## C. Ziman language ergonomics (P1)

- [ ] **C1** Nested `?` by parser-level hoisting into temporaries (`ok(parse_int(s)? + 1)`), preserving evaluation order and short-circuit rules. (L P1-1, D; M)
- [ ] **C2** Error conversion in `?` without traits, for example `expr? as ServiceError.Storage`, or a `map_err`; removes the 76 or 158 hand-written wrapping blocks in the backend. (L P1-2, D item 9; M)
- [ ] **C3** Infer generic type arguments from arguments (591 explicit generic calls in the backend). (L P1-3; M-L)
- [ ] **C4** Bare `Some(x)` and `None` where the expected type is known. (L P1-4; S-M)
- [ ] **C5** `==` and `contains` on arrays of any `Eq` element type, and `==` on `Json`. (L P1-5; S)
- [ ] **C6** Must-use `Result` statements (type error or lint error). (L P1-8; S)
- [ ] **C7** Better errors: source excerpt and caret, always the file name, stack frames, sorted by position, no cascades. (L P1-12, D item 15; M)

## D. Ziman library (P1)

- [ ] **D1** `ziman test` with `assert` and `assert_eq`. (L P1-6, D item 15; M)
- [ ] **D2** `str(float)` as shortest round-trip; `format` precision and `{{` escapes; a small `math` module; float exponent literals. (L P1-7, D; M)
- [ ] **D3** Channel additions: `channel_close`, `receive_timeout` and a two-channel `select`. (L P1-10; M)
- [ ] **D4** HTTP server hardening: reject duplicate or differing `Content-Length` and `+N` (request smuggling), require `Host`, validate response header values (CRLF injection), add a total header-read deadline. (D B7, B8, item 13; M)
- [ ] **D5** Compile route patterns once and dispatch by method and first segment (25 microseconds per endpoint today). (D item 14; S)
- [ ] **D6** Consolidate the eight `public_`/`secure_` endpoint builders into one with optional named parameters. (L P1-11; M)
- [ ] **D7** Parsing strictness: `parse_json("01")`, `parse_int(" 5")` and `"+5"` are accepted against the docs, and JSON big integers become floats. (D B14, L; S)
- [ ] **D8** JWT: `require_exp` (a token without `exp` never expires), plus `iss` and `aud`. (D S9; S)

## E. Backend hardening and features (P1)

- [ ] **E1** Indexes on every `patient_id` and the sort columns; native Postgres types through a per-dialect migration set. (D item 8; S-M)
- [ ] **E2** `storage_error` helper and reduced error boilerplate (with C2). (D item 9; S)
- [ ] **E3** Multi-error validation (a list of field errors), field names in decode errors, hide raw constraint text from clients. (D item 10, B12; M)
- [ ] **E4** Transactions for the cascades and for the two-active-insurance rule; delete without a check-then-delete race. (D item 11, B13; S-M)
- [ ] **E5** Rate limiting and lockout on login, an MFA attempt cap, a TOTP replay guard, and encryption of TOTP secrets at rest. (D item 12, S3, S4; M)
- [ ] **E6** Admin safety: guard against demoting, suspending or deleting yourself or the last admin; put the acting admin's email in `ADMIN_*` lines; log a delete only after it commits. (D S7; S)
- [ ] **E7** Slow-client protection (64 slow sockets can occupy the server), security headers (HSTS, `X-Content-Type-Options`), and drop the `Server: ziman` banner. (D S5, S8; S-M)
- [ ] **E8** Close the account-enumeration gap on wrong-provider login and correct the README claim. (D S6; S)
- [ ] **E9** Extract a shared CRUD helper to cut the six-layer copy per resource. (D item 16; M-L)
- [ ] **E10** Fill the Spring gaps clients need: `/api/users/me*`, medication `color`, codes, doses and refills. (D item 17; L)
- [ ] **E11** Small ones: `interaction-check` captured by `/{id}`, timestamp text ordering when nanos is 0, pagination and log rotation for high-volume lists. (D B9, B11, item 21; S-M)

## F. Longer term and documentation (P2)

- [ ] **F1** Decide the traits stance (a function-typed bound would unify `_compare`, `_to_json` and `_from_row`). (L P2-2; L)
- [ ] **F2** Interpreter speed: resolve identifiers to slots or compile to closures (a loop is about 28 times slower than CPython; 500-650 requests per second that does not scale with clients). (L P2-1, D; L)
- [ ] **F3** UTF-8 correctness, unified error conventions, visibility for library internals, bit operators, local and recursive lambdas. (L P2-3 to P2-8; M each)
- [ ] **F4** Mark pg, mysql, toml and yaml as experimental until the core is hardened (policy only). (L P2-9; S)
- [ ] **F5** Doc sweep: 11 mismatches in SYNTAX and the library docs, split normative from rationale, stale `model/auth.zm` comments, cost notes, a first-hour tutorial. (L P1-9, D B15, item 23; M)
- [ ] **F6** An LLM-generation benchmark to measure the "AI-first" claims; profiler, debugger and coverage tooling. (L P2-7, D item 24; M-L)
