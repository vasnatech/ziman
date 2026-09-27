# Ziman: independent language-design review

Reviewed at commit `766d26c` (`make test`: 660 passed, 0 failed; `make test-lsp`: 0 failures). Method: read `docs/SYNTAX.md` in full, the design principles and the "AI-first" section of `docs/DESIGN.md`, the "External review follow-ups" part of `docs/ROADMAP.md`, the public signatures of the bundled library, the sources of the collections, json, net, datasource, migrate and server modules, and the real application written in Ziman (`health-record-backend-ziman`, about 12,400 lines). I then ran about 90 small programs (scratch files under `/tmp/ziman_review_lang/`; nothing in the repo was modified except this file). I did not read the earlier `docs/reviews/pl-expert-review-2026-09-23.md`, so overlap with it is coincidence or confirmation.

Convention: **[V]** means I ran it or read the cited line; **[O]** is opinion.

---

## 1. Executive summary

1. Ziman is a coherent, unusually disciplined small language: value semantics, no coercion, exhaustive `switch`, `Result`/`Option` with `?`, named arguments, block-scoped `defer`, derives that emit readable code. It has shipped a 12k-line backend and a very broad standard library. [V]
2. The language core is better than its runtime. The biggest problems are not design taste; they are memory- and process-safety holes and silent wrong answers in numerics.
3. **Recursion deeper than about 1,300 frames segfaults with no message** on the default 8 MB stack, on the main thread and on spawned threads. [V]
4. **"Values are immutable" is false for the builders.** `StringBuilder`/`ArrayBuilder`/`MapBuilder` are aliasable mutable handles that type-check as ordinary values, and sharing one across `spawn`ed threads corrupted the heap and aborted the process in my test. [V]
5. **The idiomatic way to grow an array (`arr = push(arr, x)`) is O(n) per call.** 200,000 pushes took 8.5 s versus 0.23 s with `ArrayBuilder`. [V]
6. Numeric silent-wrong-answer holes remain, contradicting the language's own "never silently wrap" rule: out-of-range int literals saturate, `float as int` and `float as bigint` overflow to `INT64_MIN`, and `%` on floats passes the type checker and dies with an "internal error" at runtime. [V]
7. Several restrictions exist only because of the tree-walking interpreter (`?` not nestable, no `s.f(x)` for a function-typed field, no recursion in lambdas) and they hit exactly the everyday idioms (`ok(parse_int(s)? + 1)`). The design docs admit the `?` one. [V]
8. The standard library is the strongest part in breadth (Postgres/MySQL/TLS/HTTP/JWT/migrations in Ziman itself) and its weakest in consistency (five error types, internals exported, no math, no test facility, no channel close/select).
9. Documentation is excellent in intent but has drifted: I found eleven contradictions between sections or between docs and behaviour. [V]
10. **Verdict:** a promising, principled language whose front end (types, diagnostics, formatter, LSP) is good and whose back end (interpreter, runtime safety, performance) is not yet fit for unattended production. Fix the P0 list below before adding any more library surface.

---

## 2. Language design coherence

### 2.1 What is principled and worth keeping [O, with [V] on behaviour]

* **Enforced casing** (`val` and functions lowercase, types and variants uppercase). It removes the struct-literal-in-`if` ambiguity entirely and makes qualified names parse without lookahead. The price is real (`val MAX = 3` is a syntax error, so SCREAMING constants are impossible; appendix k1) but the trade is honest and documented.
* **Exhaustive `switch` with no default arm** (`SYNTAX.md` "switch"). The error text is excellent: "missing Blue (add an arm for each variant; there is no default arm on purpose ...)" (e8). This is the single highest-value static check for a backend language and is done right.
* **`if`/`switch` as values** with "arms may leave" (`return`, `?`, `continue`) is a well-thought-out compromise, and the type-directed auto-wrap through arms is consistent with the rest.
* **Block-scoped `defer` with arguments evaluated at registration** is better than Go's function-scoped one, and the doc admits its one cost (results discarded).
* **Immutable capture only** plus non-shadowable globals: coherent and gives a real concurrency argument.
* **Named arguments reuse struct-literal syntax; optional `Option<T>` parameters are omissible only in named calls.** Small, consistent.
* **Derive output is real source** (`--emit-derived`), with positions mapped back to the declaration.

### 2.2 Special cases and cliffs that will hurt as programs grow

**(a) `?` is restricted to "whole initializer of a val/var, or its own statement".** [V] `return ok(parse_int(s)? + 1)`, `str(g()?)`, `f(a()?, b()?)` are all syntax errors (e2, k4). Meanwhile `val x = if n > 0 { parse_int("5")? } else { 0 }` is accepted (k5), so the rule is not even "statement level": it is "not inside an operand". `DESIGN.md` states the motive plainly: "the tree-walking interpreter would otherwise have to unwind out of an arbitrary expression tree ... that is luck, not design". I agree with that self-assessment. **Opinion:** this is the most frequently felt restriction for anyone coming from Rust/Kotlin/Swift. A parser-level hoist (`f(g()?)` becomes `val t1 = g()?; f(t1)`, preserving left-to-right order and short-circuiting rules for `&&`/`||`/`if` arms) would remove it without touching the evaluator. The same hoisting would let `?` work inside lambda expression bodies.

**(b) `?` requires the identical error type; there is no conversion.** The real backend has 76 copies of the same arm `Err(failure) { ServiceError.Storage(failure) }` (I counted with grep in `health-record-backend-ziman`). The language answer ("no traits, so no `From`") is coherent but leaves the most repeated boilerplate in the largest program written in the language. Options that need no traits: `expr?` with a mapping function (`expr ? ServiceError.Storage`), or a convention that an enum variant with a single payload of type `E1` is an implicit conversion target for `?` (a closed, static, checkable rule). Either would delete most of that boilerplate.

**(c) Option/Result construction asymmetry.** [V] Patterns use bare names (`if Some(x) = o`, `switch ... { Some(x) {..} None {..} }`), but construction cannot: `return None` gives "use of undefined variable 'None'" (f5) and `Some(1)` gives "call to undefined function 'Some'" (f9). You must write `Option<int>.None` and rely on bare-value auto-wrap for `Some`. The real backend contains 134 explicit `Option<T>.Some/None` constructions. The docs justify qualified construction for user enums (fair), but `Option`/`Result` are already privileged (`unwrap`, `is_Some` bare shorthands exist), so leaving `None` bare-constructible when the expected type is known is the obvious missing step. The auto-wrap for `Some` and the missing one for `None` is a visible inconsistency.

**(d) Auto-wrap is the most surprising feature and it is mostly well contained.** The `T == E` ambiguity is now a compile error with a good message (e1), `Option<Option<T>>` likewise (l3). I could not construct a silent-wrong-answer with it. Two remaining concerns [O]: (i) the wrap is an implicit conversion in an otherwise no-implicit-conversion language, so the principle "what a line does is derivable from that line" is weakened (`return problem` in a `Result<_, ServiceError>` function silently means `Err(problem)`; see `service/allergy.zm` in the real backend); (ii) it is defined by the *expected type*, which is why `ok(...)`/`err(...)`/`channel_new()`/`[]` all need "expected type available" rules that are enumerated in three different places of the docs.

**(e) Explicit type arguments everywhere, including where the compiler already knows them.** [V] `option_unwrap_or(o, 0)` is rejected with "requires explicit type arguments, e.g. 'option_unwrap_or<...>(...)'" (v1), even though `o` is an `Option<int>`. The checker has already unified the argument types to produce the bound checks, so the mandatory `<int>` is pure ceremony, not a design necessity. `collections.map<int, string>(xs, fn(n) { str(n) })` is the canonical shape. Justified by "explicit over implicit" (DESIGN) but I consider it the biggest readability tax in library-heavy code, and it is the exact opposite of the "cheap for an LLM to generate" goal: models will omit the arguments and get the error every time. **Recommendation:** infer type arguments from the argument types (unification, no HM needed), keep explicit arguments for the cases where no argument determines them (`channel_new`, `ab_new`), and have the diagnostic print the inferred suggestion.

**(f) Generic functions are second-class in odd places.** Function values of generic functions need explicit type args (`collections.sort<int>` is accepted, h3), native generics cannot be values, builtins (`len`, `print`, `min`...) cannot be values (h3), and a function-typed struct field cannot be called directly: `s.f(1)` gives the misleading "Ziman has no methods -- if 's' is a value, call a function on it instead" (k6) although `f` is a field. Each is individually documented; together they make higher-order code feel patchy.

**(g) The type system has two tiers of "magic".** Builtins (`len`, `contains`, `set`, `remove`, `insert`, `compare`, `abs`, `min`, `max`, `hash`, `format`, `map_get`, ...) are polymorphic in ways user code cannot express (`contains` is overloaded over string/array/set/map; `abs<T>` over four numeric types). Meanwhile the docs justify `decimal_round` as a separate name because "Ziman has no argument-based overloading anywhere (every builtin name maps to exactly one signature)" (`SYNTAX.md` "Numbers"), which is false for at least `contains`, `len`, `set`, `remove`, `insert`, `abs`, `min`, `max`. **[O]** The honest description is "the compiler knows a fixed set of overloaded intrinsics, users get none of that power". That is a defensible v0 position, but it is the seed of the eventual "we need traits" conversation (see §5).

**(h) Bounds are hard-coded `Eq`/`Hash`/`Ord`.** Coherent and small, and `#[derive(Ord)]` is the only route to `Ord` on structs (plus a naming convention `<snake_type>_compare` that the compiler looks up by name). That naming-convention dispatch (also `<type>_to_json`, `_from_row`, `_to_sql`) is an implicit, stringly-typed protocol; it works but it is exactly the kind of "action at a distance" Principle 1 says to avoid. It is also the concrete reason for the `T`/`U` prelude collision found in §3.

**(i) Inconsistent equality.** [V] `==` works on a struct and on an enum (l2), and `Map`/`Set` keys may be structs, but `==` and `contains` on an *array* of structs or enums is a type error (f1, l2: "requires an array of a comparable (scalar) type, found [Shape]"). The stated reason ("those element types don't have a well-defined equality of their own yet") has been obsolete since `Eq` became structural. `Json` also has no `==`, which makes it awkward to test code that builds JSON (js).

**(j) No first-class ranges, no labelled break, no local functions, no recursion in lambdas.** [V] `fn inner()` inside a function is a syntax error (r5); a lambda cannot refer to itself (r4, which also shows a poor cascading error, "this lambda does not return a value on every path", caused by the first error). Acceptable for a v0; recursion in a helper needs a top-level function that threads captured state explicitly.

**(k) Constants must be lowercase; `format` has no way to print `{` and requires a literal template.** [V] k1, o1. The latter blocks i18n-style templates and any generic "message catalog" code; an explicit `{{`/`}}` escape and a runtime `format_dynamic(template: string, args: [string])` would fix both.

### 2.3 Value semantics are advertised more strongly than delivered

See §3.1. The language says "a value has no notion of mutability, only a binding does". The `Channel` handle and the three builder families break that on purpose; `==` on `sb == sb` is `true` and `val b = a` aliases (m1). That is a reasonable engineering choice, but it is a second, hidden type category ("handle types") that the docs describe as "not a data aggregate" only for `Channel`.

---

## 3. Semantics and safety

### 3.1 Findings that are real defects [V]

**S1. Stack overflow is an uncontrolled segfault.** [V]

```
fn depth(n: int) -> int { if n == 0 { return 0 } return 1 + depth(n - 1) }
fn main() { print(depth(N)) }
```
`N = 1200` prints; `N = 1500` and above die with SIGSEGV, no output, no Ziman diagnostic (exit 139). A `spawn`ed thread does the same at 5,000 (t.zm). With `ulimit -s unlimited` depth 100,000 works, so the limit is purely the C stack (about 5-6 KB of C stack per Ziman call). Nothing in `src/` calls `setrlimit`, sets a pthread stack size or counts depth (`grep` for rlimit/stacksize is empty). For a language whose docs advertise `parse_json` nesting caps (512) and recursive derives on tree types, a user-triggerable crash from ordinary recursion (a tree walk, a naive recursive descent, a JSON structure nested 1,500 deep through a recursive `from_json`) is a denial-of-service primitive in a server. Fix: run the interpreter on a thread with a big explicit stack and a depth counter that raises "stack depth limit exceeded" as a normal runtime error. The docs never mention the limit.

**S2. Mutable handles break value semantics and are memory-unsafe across threads.** [V]

* `val a = ab_new<int>(); ab_append<int>(a, 1, 2); val b = a; ab_append<int>(b, 3)` makes `a` contain `[1, 2, 3]` (m1). A `StringBuilder` passed to a function is mutated in place; `with` copies the reference, not the buffer.
* Four threads appending to one `StringBuilder` produced "Duplicate large block deallocation" and `abort` (exit 134) (m2). The type checker accepted it. The docs say "One thread at a time, like the other builders" but nothing enforces it, and the Concurrency section still claims "Sharing a `Value` across threads needs no locking of its own" because "nothing mutates one after construction" (`SYNTAX.md` 2942-2948). That sentence has been false since the builders were added, and false in a way that can corrupt the collector's heap.
* Recommended fix: either take a mutex inside every builder operation (cheap, uncontended), or make builders a distinct non-sendable kind (the checker rejects them as `spawn` arguments, channel payloads, struct fields that cross `spawn`). The lock is 30 lines and removes the crash.

**S3. A fatal runtime error in any thread kills the whole process; there is no isolation.** [V] i1: an out-of-bounds index in a `spawn`ed function terminates the program (exit 1), which the docs describe as "fatal-by-default" (DESIGN, Result section). For a thread-per-connection HTTP server (`server.serve`) that means one bad request that hits `unwrap` on an `Err`, an index error, division by zero or integer overflow takes down every other connection. Erlang's answer (isolate, let a supervisor restart) and Go's (`recover`) both exist for this reason. I did not construct a multi-connection test, but `serve` has no catch mechanism available to it: the language has none. At minimum, `spawn` and `server.serve` need a "run this and turn a runtime error into a value/500" boundary. **[O]** This, more than performance, is what keeps the language from unattended production use.

**S4. Out-of-range integer literals silently saturate.** [V] s1: `print(99999999999999999999999)` prints `9223372036854775807`; `-9223372036854775808` evaluates to `-9223372036854775807` because the literal `9223372036854775808` saturates first. Cause: `strtoll` unchecked at `src/parser.c:367`. The docs explicitly present the `n` suffix as the fix for "would silently overflow as an int today" (`SYNTAX.md` 276-281), so the current behaviour contradicts the stated design. Fix: check `errno`/range, and special-case the negation of the minimum value.

**S5. `float as int` and `float as bigint` are undefined for out-of-range values.** [V] i4/s3: `(1e27 as int)` (as `1000000000000.0 * 1000000000000000.0`) yields `-9223372036854775808`, i.e. a large positive float becomes `INT64_MIN`; `... as bigint` gives the same wrong value (only `as decimal` is right). Source: `(int64_t)v.as.f` at `src/interpreter.c:1018` and `:1038` (C undefined behaviour; x86 happens to produce the "indefinite" value). The docs promise saturation for the bigint-to-int direction and truncation for float-to-int, but say nothing about overflow here. Fix: saturate like `bigint as int`, and convert `float as bigint` through the exact float mantissa/exponent, not through `int64_t`.

**S6. `%` on floats passes the type checker and fails at runtime.** [V] g3: `print(7.5 % 2.0)` gives `runtime error at 9:15: internal error: '%' on float should have been rejected by the type checker` (`src/interpreter.c:672`). The docs' table says `%` is int-only and the type checker evidently does not enforce it for floats. Trivial fix, but "internal error" in a shipped release is a bad look and the type-check-before-run guarantee ("a program that violates one of these never executes any code", `SYNTAX.md` "Semantic rules") is broken.

**S7. `float` text output is lossy and inconsistent with JSON.** [V] s4/h1: `print(98765.4321)` prints `98765.4`; `str(2.5 * 4.0)` is `10`; `print(1234567.891)` is `1.23457e+06` (six significant digits, C `%g`). JSON output uses shortest-round-trip. So the same float has two textual forms in one program. For a language aimed at record-keeping backends (this is a health-record app), silently truncating a value to six digits in a log line or `format` call is a trap. The only exact-text route is `decimal` or `number.format_*`. Fix: `str(float)` = shortest round-trip (`0.1`, `10.0`); add `{:.2}`-style specifiers to `format`.

**S8. There is no math library at all.** [V] `grep -r sqrt lib src` finds nothing; there is no `sqrt`, `pow`, `exp`, `log`, `sin`, `floor` beyond the three rounding functions, and no float exponent literal (`1e3` does not lex). Non-issue for CRUD, but it is a surprising hole for "a language for backends" (percentile calculations, growth-chart z-scores are exactly what the health-record app needs) and forces users to write series expansions.

**S9. Strings are bytes with no validity guarantee.** [V] `len("héllo")` is 6; `substring(s, 0, 2)` returns `h` plus half of `é` (invalid UTF-8) and `bytes_to_string` accepts it, `Ok("h\xc3")` (e5, i3). `to_upper("straße")` is ASCII-only; `to_upper("é")` is unchanged. The prelude comments document byte semantics (`prelude.zm:7-9`), so this is a documented model, but a `string` that may be invalid UTF-8 flows into JSON encoding and HTTP responses. **[O]** Either validate at the boundaries (`substring` splitting a code point could be a runtime error, or provide `substring_chars`), or provide a code-point iteration primitive. Today there is no way to iterate characters or to truncate text safely.

**S10. Silent discard of a `Result`.** [V] g1: a bare statement `risky()` whose `Result` is `Err` compiles, runs, and continues; `--lint` reports nothing (only "a `var` never reassigned"). The docs say `defer` is "the one place Ziman silently drops a Result"; that is not true, any expression statement does. Given the language's thesis ("errors are data") a `#[must_use]`-style rule (an expression statement of type `Result` needs `_ =` or `?`) would be the cheapest high-value check to add to the checker, or at least to `--lint`.

### 3.2 Performance traps that the docs only half-warn about [V]

* **`arr = push(arr, x)` is quadratic.** 100,000 pushes: 2.1 s; 200,000: about 8.5 s; the same with `ArrayBuilder` 0.23 s (q1, q4-q6). The docs teach `push` as *the* way to grow an array (SYNTAX "Arrays") and mention builders only later. A fully persistent fix that keeps value semantics is the Go trick: give the array object capacity; `push` appends in place when the array's length equals the storage's used length and capacity remains (older values keep their shorter length and never see the new elements). That makes the idiomatic loop amortised O(1) with no aliasing hazard, and it removes the need to expose a mutable builder for the common case.
* Strings: `s = concat(s, "ab")` 100,000 times took 0.6 s; same shape (quadratic memcpy) but much cheaper constant.
* `substring`/`slice` are O(1) views that pin the whole parent (documented). With a conservative GC, a 30-byte substring of a 10 MB request body keeps 10 MB alive: worth a sentence.
* Raw interpreter speed: a 3M-iteration integer loop took 1.3 s and `fib(27)` 0.47 s (h4, h5), roughly 4-5x slower than CPython on the same code [O, from memory of typical CPython numbers]. Identifier lookup is by `strcmp` down a linked list of scopes (`src/env.c:54-69`), not by resolved slot; that alone probably accounts for a large share.

### 3.3 Concurrency model

`spawn` starts an OS thread on a *named* function, no join, no return value; `Channel<T>` is unbounded or bounded with `try_send`/`try_receive`. Sound for immutable data. Gaps that will hurt a real service [V, from the docs and by absence in the API]:

* No way to **close** a channel, so "producer is done" needs a sentinel message; no **timed receive**; no **select**; deadlocks are silent (i2: a `receive` on a channel nobody sends to just hangs; the docs say "no deadlock detection").
* No thread handle/join, no `spawn` of a lambda/function value, no error propagation from a worker (see S3).
* Reply-channel-per-request is the only way to build shared state, which is heavy (allocation per call) but coherent with the actor pitch. The pitch omits supervision, and supervision is exactly what Elixir adds to make actors practical.

### 3.4 Other semantic notes

* `int` overflow now traps (documented and verified, e3) and `MIN / -1` is covered. Good, and consistent with saturating casts elsewhere. Float division by zero (`1.0 / 0.0`) is a runtime error rather than IEEE infinity (g4), so `NaN` and infinity cannot arise from division; the docs' ordering rules for NaN are nearly unreachable. Consistent, just worth stating.
* Equality/hash/order are structural and coherent: `hash` derives from the same shape as `==`, `Ord` is opt-in via `_compare`, maps and sets compare order-insensitively but iterate in insertion order (deterministic, a real plus for tests and logs).
* **`parse_int` is lenient where the docs say strict.** [V] r3: `parse_int(" 5")` is `Ok(5)` and `parse_int("+5")` is `Ok(5)`, while `SYNTAX.md` says `Err` for "leading/trailing junk". (`0x10` and `1_000` are rejected; `parse_float("1e3")` is `Ok(1000)` even though `1e3` is not a valid float *literal*.) Leading whitespace acceptance is the classic `strtoll` behaviour leaking through; for input validation in a backend it should be strict.

---

## 4. Standard library design

### 4.1 Shape and layering [O]

The layering (prelude, `lib/core`, `lib/db`, `lib/http`, `lib/time`) is sensible and the library is written in Ziman on top of a thin native floor, which is the right dog-fooding decision. Quality of individual modules is high (`migrate.zm` is Flyway-shaped with checksums, advisory locks and a well-argued "no down migrations"; `regex` is linear-time by construction; `jwt` refuses `alg: none`; `password_hash` is Argon2id with bcrypt verify for migration). Breadth is far beyond what a v0 needs; that is both the strength and a risk (§6).

### 4.2 Consistency problems [V from signatures]

* **Five different failure shapes for the same job.** `fs`, `regex`, `json`, `mime`, `properties` return `Result<_, string>`; `net` returns `NetError`; `datasource` returns `DbError`; `client` returns `ClientError`; `server.read_request` returns `HttpError`; `jwt` returns `JwtError`. Each has its own `*_message` function with a different name (`net_error_message`, `db_error_message`, `error_message`, `jwt_error_message`). There is no shared "error with a message" notion, which is what forces the 76 `ServiceError.Storage(failure)` arms noted in §2.2(b). A tiny convention (every error type has a function of the form `<type>_message`, or a builtin `error_message(e)` intrinsic over enums that have one) would help.
* **Success payloads that carry no information.** `fs.write_text -> Result<int, string>` (bytes written) but `fs.remove`, `make_dir`, `rename -> Result<unit, string>`; `net.write -> Result<int,...>`. Callers almost always `?` and ignore the `int`. Pick `unit` unless the value is genuinely useful.
* **Fatal vs recoverable argument errors are mixed.** `collections.sort_by_keys` returns an `Err` for mismatched lengths (a programmer bug) while `crypto` says "a wrong argument is a runtime error" and `server.param` says asking for an unknown name "is a fatal error -- it is a bug in your route function". Three policies for the same class of mistake. **Opinion:** programmer errors should all be fatal (or all `Result`); pick one and say it in DESIGN.
* **Naming rules conflict with their own text.** The documented rule 1 says qualified names are "a short verb, no module prefix (`net.net_connect` would stutter)", yet `json.parse_json`, `json.json_to_string`, `json.json_object_get`, `json.json_to_json`, `uuid.uuid_new`, `time.localdate_*`, `datasource.datasource_...` do stutter. The explanation (derive and bare-import friendliness) is legitimate, but the rule as written is not the rule as practised. Also the split of array operations between prelude builtins (`push`, `set`, `remove`, `contains`, `len`) and `collections` (`first`, `last`, `reverse`, `map`) is arbitrary from a user's viewpoint; likewise `map_get` (prelude) vs `set`/`remove` (overloaded builtins) for maps.
* **Internals exported as `pub`.** `net.idle_put/idle_take/idle_clear`, `db.RunRequest/Page/OpenRequest/FetchRequest/EndRequest/Request/run_request`, `datasource.PoolState/IdleConnection`, and `db.percent_decode` alongside `http.percent_decode` (different signatures) are implementation details of the drivers. Everything `pub` is public API; a `pub(pkg)` level, or simply moving these to `_internal` modules, is needed before a stability promise.
* **Two float-to-text conversions** (S7), and `number.format_*` "truncates, never rounds" by explicit design, which is defensible but surprising for money-like output (`decimal_round` first).

### 4.3 What a real backend language still lacks [V by absence]

* **Testing.** There is no `assert`, no test runner, no in-language unit test convention. The 660-test suite is external (shell + `.zm`/`.out` fixtures). The real application is tested by black-box Python HTTP tests (`health-record-backend-ziman/tests/*.py`). A Ziman program cannot test its own functions without printing and diffing. `ziman test` with `assert`, `assert_eq`, and expected-failure support is the first tool I would add for adoption.
* **Structured logging** (key-value fields, JSON output), correlation ids; `log.write` returns nothing and the logger must be threaded explicitly through every function (there are no mutable globals). Reasonable, but see "context passing" below.
* **Math** (S8), **bitwise operators** (`bytes_xor` exists "since Ziman has no bitwise operators"; CRCs, bit flags, hashes and packed protocols are painful), **time zone arithmetic** exists but only via system tzdata.
* **Config/secret handling** exists (`properties`), **metrics** appear only in the application, not as a module, **rate limiting/caching primitives** would each need an actor.
* **HTTP server:** no TLS at the `server` layer in the docs (see §7 mismatch; `net.listen_tls` exists though), chunked request bodies refused with 501, no multipart, no compression, no HTTP/2, no WebSocket/SSE. The typed endpoint builders are a functional but heavy design: eight builder functions (`public_`/`secure_` x `handler`/`path`/`body`/`path_body`) each with 4-6 type parameters; the combinatorial explosion is a symptom of having no default/optional generic parameters or middleware composition. **[O]** A single `Endpoint` builder taking optional named parameters (`path:`, `body:`, `who:`), which the language already supports for `Option<T>` parameters, would collapse the eight functions into one.
* **Iteration protocol.** `Iter<T>` is native and lazy but `collections.map/filter/fold` work only on arrays; there is no lazy `map`/`filter`/`take` over `Iter`, so streaming pipelines (`fs.lines` to filter to write) are written as loops.
* **Async I/O**: none; thread per connection. Acceptable at this scale; note the real ceiling is thread count, and each Ziman thread also carries the 8 MB stack that makes S1 bite.

### 4.4 Over-engineered or duplicated [O]

* Three "builder" families, three `Iter` sources, `MapBuilder`/`SetBuilder` are mostly workarounds for the persistent-collection cost model; a capacity-sharing `push` (§3.2) and an in-place-when-unique `set` would let most of them be deleted from the user-facing surface.
* Formats: `csv`, `toml` (1.0 with dotted keys), `yaml` (strict subset), `ndjson` are each 400-1000 lines of Ziman running on a tree-walker that the docs themselves call "slow for megabytes". Each is a maintenance liability for a one-person project unless a real user exists. `pg`, `mysql` and TLS-with-SCRAM in Ziman are impressive but the same argument applies.
* `derive(Json)` fixes `snake_case -> camelCase` and the enum tagging shape ("Fixed, not configurable yet"), and rejects `bigint`, `byte`, `Result`, generic user types, and (for `Row`) enums and arrays. That is the right minimal start; the missing rename attribute will be needed on the first external API integration.
* `json` stores integers above 64 bits as `float` (`12345678901234567890` becomes `1.2345678901234567e+19`, js). Documented, and JS-compatible, but a `JBigint` (or keeping the literal text) would avoid silent precision loss for ids.

---

## 5. Implementation quality, tooling and documentation

### 5.1 Architecture [V]

* A classic tree-walking interpreter over the AST with a separate static type-check pass (`typecheck.c` 3,462 lines, `interpreter.c` 1,507, `parser.c` 2,073, `loader.c` 1,439, `derive.c` 1,103). Scopes are linked frames searched by `strcmp` (`env.c:54`), so lookup cost grows with nesting depth and name count. Values are 16-byte tagged structs; arrays of `int`/`float`/`bool`/`byte` are packed (a good decision). Memory is reclaimed by a vendored Boehm collector with `free` turned into a no-op via a force-included header (`Makefile`), i.e. conservative scanning of C stacks and registers, in a multi-threaded process.
* The standard library is embedded into the binary as C headers generated from the `.zm` files (`src/*_source.h`), so `use json` needs no install. Good for deployment (one binary); it does dynamically link sqlite3, OpenSSL, libcrypt and zlib, so "one binary" is not fully true.
* `derive` generates source text that is re-parsed (with source positions mapped back), which is the pragmatic route and is easy to debug (`--emit-derived`). The cost is that any mistake in generated code surfaces as an error in prelude/library code (see the collision below).
* **Prelude namespace leak. [V]** The prelude is merged bare into every program, including its helper types and *generic parameter names*. Consequently a user struct named `T`, `K`, `V`, `R` or `U` fails to compile with an error at a prelude line number and no file, e.g. `type error at 331:1: type parameter 'U' in function 'set_map' collides with an existing struct name` (n loop; also j3, t2), and a user struct named `Guard`, `MapEntry` or `Iter` reports `struct 'Guard' is already defined` at `1:1`. `struct U { ... }` (for "User") is a plausible first program. The right fix is lexical: type parameters must be scoped to their declaration and must never conflict with a struct in another module; prelude private helpers must not enter user scope.
* Scale: single-file `typecheck.c` with 3.4k lines is still manageable, but every new feature (auto-wrap positions, if/switch values, derive, lambdas, generics) has added special cases; the rules for "expected type available" are re-implemented per construct.

### 5.2 Diagnostics [V]

Overall good and above average for a hobby-scale language: messages are specific and often name the fix ("'+' doesn't join strings -- use concat(a, b), or format(...)", the `for i in a { a[i] }` trap, `match`, `.push`, `;`). `--json` diagnostics exist (`{"severity","phase","file","line","col","message"}`), `--check` exists, and errors carry a phase. Weaknesses:

* **No source excerpt and no caret**, and no end column. The text form is `ziman: type error at 2:18: ...` only. In text mode the *entry file's* name is omitted while module errors include it (`type error at 1:28 in mm/lib1.zm`), so an IDE-less user with several files cannot tell which file line 331 is in.
* **Cascading follow-on errors** after a lambda or declaration error: `r4` (self-referencing lambda) yields three errors, the second nonsensical; `e4` yields "call to undefined function 'f'" after the real problem. Errors also print out of source order (e8: line 3, then line 2).
* **Misleading messages in a few cases**: `s.f(1)` on a field of function type (k6, "Ziman has no methods"), `f(a: 1, 2)` fine, `p.x = 2` says "left-hand side must be a variable name" without naming `with` (r6/s2); the "requires explicit type arguments, e.g. `f<...>(...)`" message does not suggest the types it has already worked out (v1).
* Syntax error recovery: `val x = <newline> }` reports two syntax errors for one mistake (p1 and p2); acceptable.
* Runtime errors carry position but no stack trace; S1's segfault carries nothing.

### 5.3 Tooling [V]

* `ziman --fmt` works, wraps long arrays (o2), has `--check`/`--write`, and is enforced on the library via `make fmt-check`. It is a text formatter (keeps comments and line breaks), which is a pragmatic choice.
* `--lint` has exactly one rule (`var` never reassigned). This is the natural home for: unused `val`, ignored `Result` (S10), unreachable code after `return`, shadowing of a prelude name.
* LSP: diagnostics, formatting, symbols, go-to-definition, hover, completion; its own test target passes. A VS Code extension is in `editors/vscode`.
* No package manager or dependency story (stated non-goal), no debugger, no profiler, no REPL.
* CI: none visible in the repo (`Makefile` targets only). The 660 tests pass in 12 s, which is excellent.
* Test conventions: `.zm` + `.out` for run tests, `.zm` + `.err` for expected compile errors (314 error fixtures), separate scripts for pg/mysql/tls/shutdown. That is a good regression discipline; the absence of an in-language test facility (§4.3) is the gap.

### 5.4 Documentation quality and doc/behaviour mismatches [V]

Documentation is thorough, generated where it can be (`CORELIB.md` and `docs/lib/*.md` from source comments; the `make lib-manual-check` target), and honest about trade-offs (a decision log with rejected alternatives is rare and valuable). The problem is sprawl: `DESIGN.md` is 3,668 lines, `SYNTAX.md` 3,174, `FEATURE-NOTES.md` 308, and features are documented in the section that introduced them, with later sections contradicting earlier ones. Mismatches I found:

1. `SYNTAX.md` "Generics" (line ~1124): "`Hash` is scalar-only -- no struct or enum has a derived/structural hash". The Builtins section (~3113) says `hash` is structural and works for structs, enums, `Option`, `Result`, `Uuid`. Same file, opposite claims.
2. "Concurrency": "Nothing in the interpreter ever frees an ArrayObj/StructObj ... so a value that crosses a channel is just a pointer copy: no use-after-free ... no data race" (lines ~2942-2948). Boehm GC is now the default build and the builders are mutable (S2).
3. Grammar note at line ~172: "no expression-level `if`"; the `Enums` and `Result` sections still say "`switch` is a statement ... there is no `match` expression (ROADMAP R21 tracks expression forms)". Both are contradicted by "`if` and `switch` as values" later in the same file.
4. `SYNTAX.md` "Numbers" rationale for `decimal_round`: "Ziman has no argument-based overloading anywhere (every builtin name maps to exactly one signature)" versus the overloaded `contains`/`len`/`set`/`remove`/`abs`/`min`/`max` documented in the Builtins list.
5. Table of operators says `%` only for `int` (and the type checker fails to enforce it: S6); the table also omits `bigint`, `decimal` and `byte` entirely and still says `<` works on "int,int or float,float".
6. `SYNTAX.md` "Networking" says "Not yet: server-side TLS (`listen_tls`)"; `lib/core/net.zm:159` defines `pub fn listen_tls` and `net.zm:38` says "Server-side TLS is `listen_tls`". The HTTP section still says "Not yet: TLS (a plain `net.Socket` only)" while `server.zm:18` describes HTTPS via `listen_tls`.
7. HTTP section: "Bodies are `string`s, so a request carrying a 0 byte gets a 400" versus the same section later: "`http.Request.body` and `http.Response.body` are `[byte]`".
8. "There are 27 core modules" (`SYNTAX.md` ~1492): the tree has 32 module files (21 core, 6 db, 3 http, 2 time) and 34 pages in `docs/lib`.
9. `tools/gen_corelib_reference.sh:22` (so `CORELIB.md:270`) says the two known `json` gaps are "`\uXXXX` unicode escapes and `==`/`!=` on `Json`". The escape decoding works (`parse_json("\"caf\\u00e9\"")` returns `Ok(JString(café))`, js); only `==` is a gap.
10. `parse_int` is documented as rejecting leading/trailing junk but accepts leading whitespace and a `+` sign (r3).
11. `SYNTAX.md` says `arr == arr` requires scalar element type because non-scalars "don't have a well-defined equality of their own yet", but struct `==` is defined (§2.2(i)).

Suggestion: keep `SYNTAX.md` as the normative reference (<1,500 lines, rules only), and move rationale to `DESIGN.md` entries that are dated and never edited. Today rationale and rule are interleaved, so every change edits both and one goes stale.

---

## 6. Comparison and positioning

Ziman sits between **Gleam** (typed, immutable, no methods, `Result`, explicit and small; but Gleam has type inference, a `use`-callback sugar for early return, pipelines and the BEAM's isolation and supervision), **Go** (simple, batteries-included backends, goroutines; Go has interfaces, `recover`, fast native code, a test tool), **Rust** (enums, `?`, exhaustive match, derives; but traits, `From`, and nested `?`), **Kotlin/Spring** (the health-record backend is deliberately a Spring layering: entity, mapper, repository, service, controller), and **Elixir** (actors, no shared memory; but supervision, per-process heaps).

**Best ideas, worth keeping [O]:**

1. Exhaustive `switch` with no default (and its diagnostic).
2. Enforced casing and "one way to do it"; the formatter output is canonical.
3. Named arguments that reuse struct-literal syntax, plus omissible `Option` parameters.
4. Block-scoped `defer`.
5. Compile-time-checked `format` templates (extend, do not relax; add `{{`).
6. Immutable-capture lambdas and the non-shadowable-global rule.
7. Derives that generate readable, inspectable source, and diagnostics designed for machine consumption (`--json`).
8. A batteries-included stdlib written in the language itself with generated docs.
9. The decision log with rejected alternatives; rare and it made this review far easier.
10. Deterministic insertion-ordered maps and sets.

**Biggest risks [O]:**

1. **Runtime robustness** (S1-S3): the process dies on ordinary recursion, on any fatal error in any thread, and on misuse of a type that looks like a plain value.
2. **Implementation-driven restrictions** posing as design (`?` nesting, no `x.f(y)`, no recursive lambdas). `DESIGN.md` is honest about this; the risk is that "the interpreter can't" gets frozen into the language spec.
3. **Scope vs. bus factor.** A tree-walker, a type checker, an LSP, a linear-time regex engine, TLS/SCRAM clients, three database drivers, four data formats, JWT, OTP and a migration tool are all owned by one project. Every feature increases the surface a change to the core must keep green.
4. **"Cheap for an LLM to generate" is asserted, not measured.** The docs admit the training-distribution cost of every divergence (`arr = push(arr, x)`, `Enum.unwrap_X`, explicit type args). No experiment in the repo measures generation accuracy; a fixed 50-task benchmark with pass rates before and after each restriction would turn a slogan into a design instrument.
5. **Performance ceiling.** A slot-resolved bytecode VM or closure-compiled AST would probably give 3-10x; without it, JSON/CSV/YAML parsers written in Ziman are slow by construction, and the docs already say so.
6. **Traits question is deferred, not answered.** Naming-convention dispatch (`_compare`, `_to_json`, `_from_row`, `_to_sql`), hard-coded bounds, magic overloaded builtins and per-type error-message functions are all one missing feature (interfaces/traits or at least "function-typed bound") showing up in different places. It is fine to defer; it is not fine to keep adding convention-based dispatch, since each addition raises the eventual migration cost.

---

## 7. Prioritised recommendations

Effort: S = a day or less, M = a few days, L = a week or more.

### P0 (correctness and safety; do before any new features)

| # | Change | Why | Effort |
|---|--------|-----|--------|
| P0-1 | Run the interpreter (and every `spawn` thread) on an explicit large stack and enforce a depth counter that raises a normal "stack depth limit exceeded" runtime error; document the limit. | S1: silent segfault at about 1,300 frames; a denial-of-service in a server. | S-M |
| P0-2 | Make builders thread-safe (a mutex per builder) or forbid them as `spawn`/`Channel`/struct payloads; correct the Concurrency prose. | S2: heap corruption reachable from safe code. | S |
| P0-3 | Fix numeric holes: range-check integer literals (and the `-9223372036854775808` literal), saturate `float as int`, convert `float as bigint` exactly, reject `%` on float in the checker. | S4-S6: silent wrong answers contradicting the stated rule. | S |
| P0-4 | Add a fatal-error boundary for spawned threads and `server.serve` (turn a runtime error in a handler into a 500 and a log line; keep the process alive). Requires a `setjmp`-style unwind to the thread boundary, which is much easier than a general `catch`. | S3: one bad request kills all connections. | M |
| P0-5 | Fix the prelude namespace leak: type parameter names are scoped per declaration, prelude-private structs are not visible to user code, and errors always name the file. | §5.1: `struct U`/`T`/`K`/`V`/`R` cannot be declared; misleading errors. | S-M |
| P0-6 | Make `arr = push(arr, x)` amortised O(1) (capacity-sharing append on the tail). | §3.2: 8.5 s vs 0.23 s; the idiom taught first is the slow one. | M |

### P1 (ergonomics and consistency that shape how the language is used)

| # | Change | Why | Effort |
|---|--------|-----|--------|
| P1-1 | Lift the `?` nesting restriction by parser-level hoisting into temporaries (preserving evaluation order and short-circuit rules). | §2.2(a): `ok(parse(s)? + 1)`, `f(a()?)`. | M |
| P1-2 | Error-type conversion in `?` without traits: `expr? as ServiceError.Storage`, or an implicit single-payload-variant rule. | §2.2(b): 76 identical arms in the reference app. | M |
| P1-3 | Infer generic type arguments from arguments (keep explicit form for uninferable ones like `channel_new`, `ab_new`). | §2.2(e). | M-L |
| P1-4 | Allow bare `None` and `Some(x)` construction where the expected type is known. | §2.2(c). | S-M |
| P1-5 | Allow `==`/`contains` on arrays of any `Eq` element type; add `==` on `Json`. | §2.2(i). | S |
| P1-6 | `ziman test` and `assert`/`assert_eq` (test files, expected-failure). | §4.3: no in-language testing. | M |
| P1-7 | `str(float)` = shortest round-trip; `format` gets `{:.N}`, `{{`/`}}` escapes; a runtime-template `format_dynamic`. Add a small `math` module (`sqrt`, `pow`, `exp`, `log`, trig, `floor` returning float, `is_nan`) and float exponent literals. | S7, S8, §2.2(k). | M |
| P1-8 | Must-use `Result` statements (type error, or at least `--lint` error). | S10. | S |
| P1-9 | Doc sweep: fix the 11 mismatches above, split `SYNTAX.md` (normative) from rationale, add a "CI check" that greps docs for the "not yet" list against `pub` signatures. | §5.4. | M |
| P1-10 | Channel additions: `channel_close`, `receive_timeout`, and a two-channel `select`. | §3.3. | M |
| P1-11 | Consolidate the eight `public_`/`secure_` endpoint builders into one with optional named parameters. | §4.3. | M |
| P1-12 | Give errors a source excerpt/caret, always print the file name, sort by position, and suppress cascades after a failed declaration. | §5.2. | M |

### P2 (longer-term)

| # | Change | Why | Effort |
|---|--------|-----|--------|
| P2-1 | Resolve identifiers to slots at check time (or compile to closures/bytecode) to remove `strcmp` lookups and speed up the interpreter substantially. | §3.2, §5.1. | L |
| P2-2 | Decide and document the traits/interfaces stance before adding more convention-dispatched protocols; even a minimal "function-typed bound" (`T: Ord via cmp`) would unify `_compare`, `_to_json`, `_from_row`. | §2.2(g)(h), §6. | L |
| P2-3 | UTF-8 correctness: validate strings at input boundaries or offer `chars`/`substring_chars`; keep byte functions explicit. | S9. | M |
| P2-4 | Unify error conventions: a common `<type>_message` intrinsic, `unit` for no-information successes, one policy for programmer errors. | §4.2. | M |
| P2-5 | Visibility for library internals (`pub(package)`/`_internal` modules) and move the driver protocol structs out of the public surface. | §4.2. | S-M |
| P2-6 | Bitwise operators (or a `bits` module) and a bigint-aware JSON number. | §4.3, §4.4. | S-M |
| P2-7 | An LLM-generation benchmark (fixed task set) to measure the AI-first claims per restriction. | §6 risk 4. | M |
| P2-8 | Local named functions and recursive lambdas (a `let rec`-style name binding for lambdas). | §2.2(j). | M |
| P2-9 | Reconsider the size of the shipped library relative to core maturity; freeze pg/mysql/toml/yaml as "experimental" until the core is hardened. | §6 risk 3. | S (policy) |

**If I owned it, I would spend the next two weeks on P0-1 through P0-6 and P1-1, P1-6 and P1-9, and add nothing else.** After that the language would be defensible as "small, sound, and pleasant" rather than "small, broad, and occasionally crashes".

---

## Appendix: experiments (input, output, why it matters)

All under `/tmp/ziman_review_lang/`; `X.zm` is run with `ziman X.zm` unless noted. Output abbreviated.

| ID | Input (essence) | Output | Why it matters |
|----|-----------------|--------|----------------|
| e1 | `fn f(x:int)->Result<string,string>{ return "hi" }` | `a bare string is ambiguous where Result<string, string> is expected ... write ok(...) or err(...)` | The `T == E` auto-wrap hole is closed, with a good message. |
| e2 | `print(str(g()?))` | `'?' can only be used as a val/var's entire initializer or as its own statement` | `?` restriction (§2.2a). |
| e3 | `9223372036854775807 + 1` | `runtime error ... integer overflow in '+'` | Overflow traps as documented. |
| e4 | lambda capturing a `var` | error naming the fix; then a spurious `call to undefined function 'f'` | Good primary message, cascade noise. |
| e5 | `substring("héllo", 0, 2)`, `char_at(s,1)`, `len(s)` | `6`, `h\xc3`, `HéLLO`, `\xc3` | Bytes model; invalid UTF-8 produced (S9). |
| e6 | `"x" + "y"` | `'+' doesn't join strings ... use concat(a, b), or format(...)` | Excellent diagnostic for a common habit. |
| e7 | wrong type + undefined fn | two clear errors | Multi-error reporting works. |
| e8 | non-exhaustive `switch` | `missing Blue (add an arm ...; there is no default arm on purpose ...)`, then a second, redundant "does not return a value" error, printed out of order | Great core check; minor duplicate/ordering. |
| f1 | `[P{..}] == [P{..}]` | `array '=='/'!=' requires ... a comparable (scalar) type, found [P]` | Equality inconsistency (§2.2i). |
| f2, d.zm loop | `depth(N)` for N = 1000, 1200, 1500 ... 100000 | 1000/1200 print; 1500+ exit 139 (SIGSEGV), no message; also fails on a `spawn`ed thread at 5000 (t.zm); works only with `ulimit -s unlimited` | S1. |
| f3 | 20,000 `push` and `concat` in a loop | completes | Sanity for small n (see q1). |
| f4 | `id(5)` for `fn id<T>` | `'id' is generic and requires explicit type arguments` | Type-argument ceremony (§2.2e). |
| f5 | `return None` in `Option<int>` fn | `use of undefined variable 'None'` | Construction asymmetry (§2.2c). |
| f6 | `?` inside `Result<int,int>` fn on `Result<int,string>` | `requires the enclosing function to return Result<..., string>` | No error conversion (§2.2b). |
| f7-f9 | string `switch`, `&`, `Some(1)` | syntax errors ("expected a variant name", "did you mean '&&'?", "call to undefined function 'Some'") | No string switch, no bitwise ops, no bare `Some(...)` constructor. |
| g1 | `risky()` as a statement, `Err` result | prints `continued`; `--lint` says nothing | Silent `Result` discard (S10). |
| g2 | `collections.map(xs, fn(n){n})` | `'collections::map' is generic and requires explicit type arguments` | §2.2e again. |
| g3 | `7.5 % 2.0` | `runtime error ... internal error: '%' on float should have been rejected by the type checker` | S6; `src/interpreter.c:672`. Also `-7 / 2 = -3`, `-7 % 3 = -1` (C semantics). |
| g4 | `0.0 / 0.0`, `1.0/0.0` | `division by zero` | Float division traps; no NaN/inf from arithmetic on literals. |
| g5 | shadowing in nested block and a lambda param | works (inner `x` = 2; lambda `x` = 3); same-scope redeclaration is an error (r2) | Shadowing rules: nested only. |
| g6, g7 | `unwrap_A` on `B`; `a[5]` | fatal runtime errors with position | Fatal-by-default runtime errors. |
| g8, g9 | `m["a"]`, `switch` on a string | `cannot index into Map<string,int>`; `expected a variant name, found string literal` | No index on maps, no switch on values. |
| h1, s4 | `print(1234567.891)`, `print(98765.4321)`, `str(2.5*4.0)`, `print(1.0)` | `1.23457e+06`, `98765.4`, `10`, `1` | S7: six-digit float output; `1.0` prints like an int. |
| h2 | struct arrays with `with`, `hash`, channel print | works; `print(channel)` shows `Channel` | Structural hash works; fine. |
| h3 | `collections.sort<int>` as a value; `val g = len` | first accepted, second: `'len' is a builtin, not a function value` | §2.2f/g. |
| h4, h5 | 3M-iteration loop; `fib(27)` | 1.3 s; 0.47 s | §3.2 performance. |
| i1 | out-of-bounds index inside a `spawn`ed function | whole process exits 1 | S3. |
| i2 | `receive` on a channel nobody sends to | hangs (killed by `timeout`) | No deadlock detection; also stdout lost when killed (buffered). |
| i3 | `bytes_to_string(slice<byte>(b,0,2))` on `"héllo"` | `Ok(h\xc3)` | S9. |
| i4, s3 | `1e27` (as a float product) `as int`; `as bigint`; negative | `-9223372036854775808` for both int and bigint; decimal correct | S5; `src/interpreter.c:1018,1038`. `300 as byte` saturates to 255 as documented. |
| j1, j2 | recursive `struct Node { next: Option<Node> }`; mutually recursive struct/enum | accepted | Recursive types via Option/enum work. |
| j3, t2 | user `struct U {..}` with `#[derive(Json)]` | `type error at 331:1: type parameter 'U' in function 'set_map' collides with an existing struct name` | Prelude namespace leak (§5.1). |
| n (loop) | `struct T/K/V/R/U/Guard/MapEntry/Iter` | T, K, V, R, U: collision errors at prelude line numbers; Guard, MapEntry, Iter: `already defined`; A, Uuid, Row: fine | Same. |
| j4 | `s[0]` on a string | `cannot index into string (not an array)` | Strings not indexable; use `char_at`. |
| j5 | `f(a: 1, 2)` | `a call's arguments must be either all positional or all named` | Documented rule, good message. |
| k1 | `val MAX = 3` | `a global value name must not start with an uppercase letter` | Constants cannot be SCREAMING_CASE. |
| k2 | `val a = u()` where `u` returns `unit`; `val b = noop()` where `noop` returns nothing | only the second is an error | `unit` vs no return type as documented. |
| k3 | pattern-`if` used as a value | `an 'if' that binds a pattern ... is a statement` | Documented restriction; `switch` is the workaround. |
| k4 | `ok(parse_int(s)? + 1)` | nested-`?` error | §2.2a. |
| k5 | `val x = if n > 0 { parse_int("5")? } else { 0 }` | `Ok(5)` | `?` allowed inside if-arm values but not in operands: inconsistent. |
| k6 | `s.f(1)` where `f` is a function-typed field | `Ziman has no methods ...` | Misleading message (§2.2f, §5.2). |
| k7 | Map insert/iterate/`map_get_or`, `Set` | works, insertion-ordered | Maps/sets behave as documented. |
| l1 | `sort_by_keys` + `fold` with lambdas | works, stable sort | Library higher-order code fine (with explicit type args). |
| l2 | `switch` as a value in `return`; `contains([Shape], Shape)` | switch works; `contains` errors `[Shape]` not scalar | §2.2i. |
| l3 | `Option<Option<int>>` from bare `Option<int>` | compile error explaining `Some(None)` hazard | Auto-wrap corner covered. |
| m1 | `val b = a` for `ArrayBuilder`; `sb` passed to a function; `with w { sb: sb }` | `xxx`, `[1, 2, 3]`, `sb == sb` is `true` | S2: aliasing handles. |
| m2 | four `spawn`ed threads appending to one `StringBuilder` | `Duplicate large block deallocation ...`, core dump (exit 134) | S2: memory-unsafe from safe code. |
| m3 | aliasing a `Channel` | works as a shared handle | Intended. |
| o1 | `format("{{}} {}", 1)`, non-literal template | `malformed placeholder ... stray '{'`; `'format' requires its first argument to be a string literal` | No brace escape; no runtime template (§2.2k). |
| o2 | `--fmt` on badly spaced code | canonical output, wraps at width 120 | Formatter works. |
| e7/e8 `--json --check` | JSON object per error | `{"severity":"error","phase":"type","file":"e7.zm","line":2,"col":18,...}` | Machine-readable diagnostics exist; no end column, no fix suggestion field. |
| o3 | `var unused = 1` with `--lint` | `warning: 'var unused' is never reassigned; declare it with 'val'` | The only lint rule. |
| p1-p3, mm | assorted syntax and type errors, error in a `use`d module | precise positions; module errors include the file, entry-file errors do not; cascades are suppressed after some errors | §5.2. |
| q1-q7 | 100k/200k `push`; `ArrayBuilder` equivalents; `sort`; `map` | push loop 2.1 s (100k) and ~8.5 s (200k); builder 0.23 s; native `sort<int>` of 200k about 0.04 s over the build; `map` 200k 0.16 s | §3.2 quadratic append. |
| r1, s1 | `-9223372036854775808`; `99999999999999999999999` | `-9223372036854775807`; `9223372036854775807` | S4; `src/parser.c:367`. |
| r2 | `val x = 1; val x = 2` in one scope | `'x' is already declared in this scope` | Same-scope redeclaration is rejected. |
| r3 | `1 < 2 < 3`; `parse_int(" 5")`, `"+5"`, `"0x10"`, `"1_000"`; `parse_float("1e3")` | chained comparison: clear error; parse results `Ok(5)`, `Ok(5)`, `Err`, `Err`, `Ok(1000)` | Good message; `parse_int` is lenient contrary to the docs. |
| r4, r5, r6, s2 | recursive lambda; nested `fn`; `arr[0] = 5`; `p.x = 2` | cascading errors; `expected '(', found identifier`; `left-hand side of assignment must be a variable name` | §2.2j and §5.2. |
| u1-u3 | Result/Option auto-wrap combinations, variadic edge cases | errors as documented (`Option<string>` vs `string` `==` is rejected) | Auto-wrap is well contained. |
| v1, v2 | `option_unwrap_or(o, 0)` vs `option_unwrap_or<int>(o, 0)`, `unwrap(o)`, `is_Some(o)` | first is an error, others work | §2.2e. |
| js | `parse_json("\"caf\\u00e9\"")`; `==` on `Json`; large integers | `Ok(JString(café))`; `enum 'json::Json' cannot be compared`; `12345678901234567890` re-serialises as `1.2345678901234567e+19`; `1.0` re-serialises as `1` | Doc mismatch #10; `Json` has no `==`; number fidelity. |
| t1-t3 | `derive(Json)` with `bigint`; `derive(Row)` with `[string]` | `derive(Json) does not support field type 'bigint' yet`, `derive(Row) does not support field type '[string]'` | Derive limits produce clear messages. |
| make test / test-lsp | run the shipped suites | 660 passed, 0 failed; LSP 0 failures | Baseline: the regression suite is healthy. |
