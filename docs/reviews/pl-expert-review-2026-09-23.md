# Ziman — outside expert review

**Date:** 2026-09-23
**Reviewer:** external PL/compiler audit, first exposure to the language
**Scope:** `README.md`, `docs/{SYNTAX,DESIGN,ROADMAP,CORELIB}.md`, `src/*.c|h`, `lib/core/*.zm`, `lib/db/*.zm`, `tests/fixtures/`, and `health-record-backend-ziman` as a real-usage sample
**Method:** read the docs, then checked them against the source, then ran the interpreter against ~25 hand-written probes and load-tested the showcase backend. Every claim below that says "measured" or "verified" was executed, not inferred.

---

## 1. Executive summary

Ziman is a better piece of work than its category ("solo hobby language, AI-paired") normally produces. The core is coherent, the type checker is real (377 fixture tests pass, 0 fail), the value model is elegant, and `docs/DESIGN.md` is a genuinely excellent decision log — it names rejected alternatives and the reasoning behind each cut, which most production languages don't bother to write down. Several individual decisions (block-scoped `defer`, parser-enforced casing, compile-checked `format` literals, named arguments reusing struct-literal syntax) are good enough that I'd recommend them to other designers without qualification.

Against its actual stated goal — *"optimized for LLM coding agents to write correct code on the first try, while remaining fully human-readable"* — the verdict is more mixed, and the pattern of the misses is consistent enough to be diagnostic. **Ziman rigorously eliminates the error classes that are cheap to eliminate and highly visible (implicit numeric coercion, operator overloading, indentation sensitivity, positional struct construction), and leaves standing the error classes that are expensive to eliminate and that actually dominate LLM-generated bugs.** Specifically:

- **There is no exhaustiveness checking anywhere.** No `match`; enum consumption is an `if Shape.is_Circle(s) / unwrap_Circle(s)` chain. Adding a variant to an enum produces **no compile error at any call site** — it produces a runtime crash (verified). "Did you handle every case?" is the single highest-value static check for machine-generated code, and it is the one Ziman doesn't have, while it does have a compile error for mixing `int` and `float`.
- **The `Result<T, E>` bare-value auto-wrap silently turns errors into successes when `T == E`.** `Result<string, string>` occurs 11 times in the bundled stdlib including `io.read_file`, the flagship example. `return "path must not be empty"` from such a function compiles to `Ok("path must not be empty")` with no diagnostic (verified). SYNTAX.md:736 documents the rule; that does not help a generator that writes the idiomatic thing.
- **`for x in arr` silently produces wrong answers** when the author meant an index loop — a habit every model has from Python and Go. Verified: a 10-line program prints `2` where the author meant `4`, with no compile error and no runtime error.

And one finding outranks all of those. **The memory model makes the project's own north-star goal unreachable today, by about four orders of magnitude.** Nothing in the interpreter ever frees a heap value (`src/value.h:94`, stated plainly). I started `health-record-backend-ziman`'s HTTP server and sent it roughly 1,500 ordinary requests. **RSS reached 31 GB.** A trivial `GET /health` leaks ~87 kB, permanently, forever. `docs/ROADMAP.md:308` discloses the issue honestly and analyzes it intelligently — but it undersells the severity ("a real problem" rather than "the north-star application cannot run"), and, more importantly, **its proposed fix rests on a claim that is false**: ROADMAP.md:345 asserts the value graph "can never contain a cycle," which would make pure refcounting sufficient. It can (verified, §4.2).

Net: as a language design document, Ziman is strong and worth continuing. As a runtime, it is a prototype that cannot host a long-running process, and the gap between those two facts is the main thing to fix. The AI-first framing is, in a few specific places, doing work it hasn't earned — it is used to justify restrictions whose real motivation is interpreter simplicity (§9).

---

## 2. Type system

### 2.1 What's solid

The core is sound, and it's sound for a defensible structural reason rather than by luck: there is no subtyping, no variance, no inference on type arguments, no downcasting, and no reflection. Generics are runtime-type-erased with no monomorphization, which in most languages is where the holes appear; here the erasure is safe precisely *because* every type argument is written out at every use site. I tried to break it and couldn't:

```
struct Box<T> { value: T }
fn get<T>(b: Box<T>) -> T { return b.value }
val b = Box<int> { value: 5 }
val s: string = get<string>(b)
// ziman: type error at 5:33: argument 1 of 'get' expects Box<string>, found Box<int>
```

Bound enforcement at instantiation also works and produces a good message:

```
hashmap_new<Uuid, string>()
// ziman: type error: type argument uuid::Uuid does not satisfy bound 'Hash'
//        required by type parameter 'K' (it must support 'hash')
```

Nominal structs/enums, mandatory named field construction, and `with` for functional update are all the right calls. Value semantics for arrays with pointer-sharing under an immutability invariant (`src/value.h:10-20`) is a genuinely nice piece of engineering: the semantics are "always copies," the implementation almost never does, and the invariant that makes it safe is written down where it belongs.

### 2.2 The auto-wrap is a contradiction the docs half-admit

The language's first principle is "no hidden control flow… no implicit conversions" (DESIGN.md:16). It then implements implicit conversion for `Option<T>` and `Result<T, E>` in six syntactic positions. The docs call this "a deliberate, narrow exception to 'no implicit coercion,' modeled on Rust's own `impl<T> From<T> for Option<T>`" (SYNTAX.md:695-747).

The Rust analogy doesn't hold. Rust has *no* such implicit conversion — `From` is explicit; you write `.into()` or `Some(x)`. What Ziman built is closer to C++'s single-argument converting constructor, which is widely regarded as a mistake and which C++ added `explicit` to opt out of. That's not fatal on its own — I'd accept the ergonomics trade — but the language should say what it actually did rather than claim a precedent that doesn't exist.

The narrow-and-unambiguous defense also fails in two concrete cases.

**Case 1 — `T == E` silently picks `Ok`.** This is the important one, because it is not a corner case; it is the most common `Result` shape in Ziman's own standard library.

```
fn load(path: string) -> Result<string, string> {
    if len(path) == 0 {
        return "path must not be empty"      // INTENDED as an error
    }
    return concat("contents of ", path)
}
// prints: OK: path must not be empty
```

No warning, no error, exit 0. `grep -c 'Result<string, string>' lib/` returns 11, including `io.read_file`, `http.percent_decode`, `properties.get_string`, `json.json_require_string_field`. The failure is silent, it is in the most-written direction (error paths), and the correct incantation (`return err(...)`) is the *less* idiomatic-looking one. For a language whose thesis is first-try correctness from a generator, this is close to a worst case: the sugar is most dangerous exactly where it's most likely to be used.

The fix is cheap and doesn't require giving up the sugar: **reject the bare-value auto-wrap when `T == E`** and require `ok(...)`/`err(...)` there. The rule becomes "auto-wrap applies when it is unambiguous," which is what the docs already claim it is.

**Case 2 — `Option<Option<T>>` silently wraps `None` into `Some(None)`.**

```
val n: Option<int> = Option<int>.None
val x: Option<Option<int>> = n
print(Option.is_Some(x))     // true  -- n became Some(None)
```

Deterministic, defensible, and exactly the distinction people get wrong. Worth at least a diagnostic.

To the language's credit, auto-wrap is **not transitive** — `val x: Option<Option<int>> = 5` and `return 5` from `-> Result<Option<int>, string>` are both correctly rejected. That restraint is right.

### 2.3 Expressiveness limits

**No trait/interface system at all, and only two hardcoded bounds (`Eq`, `Hash`).** The consequences are larger than the docs suggest:

- **`Hash` is scalar-only**, so a `HashMap` key can only be one of the seven scalars. You cannot index anything by `Uuid` (verified), by an enum (verified), or by any struct. For a language whose north star is a REST backend over UUID-keyed entities, "you cannot build an in-memory map from entity id to entity" is a serious hole. The backend dodges it only because it pushes everything to SQL.
- **There is no `Ord`/`Cmp` bound, and no function values.** Therefore **it is impossible to write a sort** — not "there isn't one in the stdlib" (there isn't; `grep -rn 'fn sort' lib/` is empty), but impossible to write generically at all. No comparator can be passed; no bound can express "comparable." Every program that needs ordered output must either hand-write a monomorphic sort per element type per sort key, or push `ORDER BY` to the database. That is a real expressiveness cliff, and the nine new `_compare(a, b) -> int` functions the README mentions across `byte`/`uuid`/`time` are the symptom: the language noticed it needed ordering nine times and worked around it nine times rather than adding the bound once.

Adding an `Ord` bound is small (`Eq` and `Hash` already prove the mechanism) and would immediately unlock a generic `sort` given a `_compare`-style convention or a third bound. This is the highest expressiveness-per-effort item available.

**No pattern matching, no tuples, one payload per variant.** The single-payload rule forces a named struct for every multi-field variant. That's a defensible taste call (it also forces good naming), but combined with no `match` it makes enums verbose to both produce and consume.

### 2.4 Silent integer overflow

`src/interpreter.c:439`:

```c
if (l.type.kind == TY_INT) return value_int(l.as.i + r.as.i);
```

No overflow check. Verified: `9223372036854775807 + 1` prints `-9223372036854775808`, exit 0.

This directly contradicts the language's own repeatedly-stated principle. `bigint as int` saturates rather than wraps, and `int as byte` saturates rather than wraps, both justified in the docs as "clamp, don't silently wrap." Plain `+` then silently wraps. Pick one.

Worse, signed overflow is **undefined behavior in C**, and the Makefile has no `-fwrapv` (`Makefile:2`: `-std=c11 -Wall -Wextra -Wpedantic -g -MMD -MP -pthread`). Today it wraps because the build is unoptimized; at `-O2` the compiler is entitled to assume it can't happen. Add `-fwrapv` immediately as a correctness floor, then decide whether the language semantics should be trap, saturate, or documented wrap.

**Also in `Makefile:2`: there is no `-O` flag at all.** The tree-walking interpreter is built at `-O0`. That is a free 2-3x on every benchmark.

---

## 3. Error handling model

### 3.1 The good parts

`Result`/`Option` as the only error channel, with `unwrap` as an explicit opt-in to fatality, is the right model and is implemented consistently. `defer` (§6 of SYNTAX.md) is **better than Go's**: block-scoped rather than function-scoped, so a `defer` in a loop body runs per iteration instead of piling up — Go's version of this is a well-known trap and Ziman fixed it deliberately (DESIGN.md:2190). Arguments evaluated at registration, LIFO, result discarded: all correct choices, and the one genuinely lossy behavior (a deferred `Result` is dropped) is called out explicitly in the docs as a real cost rather than glossed.

The `?` operator's diagnostic is the best error message in the language:

```
ziman: type error at 2:24: '?' can only be used as a val/var's entire initializer
       or as its own statement, not nested inside another expression
```

That tells you the rule, not just that you broke it. More messages should look like this.

### 3.2 Non-nestable `?` — the friction is real but the cut is correct

`return parse_int(a)? + parse_int(b)?` is rejected. An LLM will write that, get the message above, and fix it in one turn. That's an acceptable cost, and the stated justification (avoiding non-local unwinding out of an arbitrary expression tree in a tree-walking interpreter) is honest and correct.

My only objection is framing, not substance: this is presented in §9-adjacent terms as serving clarity, but it is an implementation-driven cut, and DESIGN.md is refreshingly willing to say so. Keep it. If anything, the restriction genuinely does improve readability — one fallible step per line is a good style rule that many languages would benefit from enforcing.

### 3.3 The missing piece: no exhaustiveness, anywhere

This is the largest single design gap relative to the stated goal.

```
enum Shape { Circle(float) Square(float) Triangle(float) }

fn area(s: Shape) -> float {
    if Shape.is_Circle(s) { return 3.14 * Shape.unwrap_Circle(s) }
    return Shape.unwrap_Square(s)
}

// compiles clean; at runtime:
// ziman: runtime error at 5:12: called 'Shape.unwrap_Square' on a 'Shape.Triangle' value
```

Adding a variant to an enum is, in every language with sum types, the canonical example of a change the compiler should catch everywhere. Here it is caught nowhere. This is not hypothetical in the sample codebase — `health-record-backend-ziman/patient/controller.zm:16-20` has exactly this shape:

```
fn service_error_response(e: service.ServiceError) -> common_response.ApiResponse {
    if service.ServiceError.is_Validation(e) { ... }
    return common_response.db_error_response(service.ServiceError.unwrap_Storage(e))
}
```

A third `ServiceError` variant compiles fine and crashes the request thread.

Consider the AI-first argument here carefully, because it cuts hard the other way. An LLM refactoring an enum is *more* likely than a human to add a variant and miss a consumer, because it has no project-wide working memory and is often editing one file at a time. Exhaustiveness checking is the single feature that most directly converts that failure mode into a compile error. A language that skips `match` but keeps a compile error for `1 + 1.0` has, on its own terms, prioritized backwards.

A full `match` with binding patterns is a big design pass (rightly deferred in ROADMAP). But **exhaustiveness does not require `match`.** A much cheaper intermediate exists: a `switch`-like statement over an enum that requires one arm per variant, desugaring to the existing `is_`/`unwrap_` calls. It reuses everything already built and closes the gap.

---

## 4. Memory model

### 4.1 Measured, not inferred

`src/value.h:94` states it: *"nothing in this interpreter ever frees an ArrayObj/StructObj/EnumObj/ResultObj."* Confirmed by inspection — `grep 'free(' src/value.c src/interpreter.c src/env.c` returns only `Env` teardown, `value_display` scratch buffers, and spawn-arg arrays. No `Value` payload, no string buffer, no `BigNum`, is ever released.

Measurements on this machine:

| Program | Result |
|---|---|
| 2M-iteration `int`-only loop (baseline) | 4.4 MB RSS — scalars are inline, no leak |
| 2M iterations building one small struct + 3-element array + one `concat` | **1.32 GB RSS** (~660 B/iteration) |
| 200k iterations of `concat("x", str(i))` | 16.6 MB RSS |

Then the real test — `health-record-backend-ziman`'s server, in-memory SQLite, ~1,500 requests (500 `GET /health`, 500 `POST /patients`, 500 `GET /patients`):

```
RSS: 31,101,500 kB          # ~31 GB
Threads: 1
```

Incremental rate on a trivial request, measured directly:

```
RSS after 50 more GET /health: +4,384 kB  =>  ~87 kB leaked per trivial request
```

The 31 GB is dominated by the 500 list requests: each serializes a growing patient array through `#[derive(Json)]` → `Json` enum nodes → `JField` structs → per-node intermediate strings from `json_to_string`'s recursive `sb_to_string` calls, all permanently retained. That is the O(N²) accumulation ROADMAP.md:320 predicted, realized.

**Assessment:** this is disclosed, and the disclosure is intellectually honest and well-written — it deserves credit, and it's far better than the usual "performance is future work." But the ROADMAP's framing ("fine for a short-lived script, a real problem for the North Star") understates it. The correct statement is: **the north-star application, which exists and runs, exhausts a 32 GB machine in about 1,500 HTTP requests.** Ziman today is a CLI-and-test-fixture language. Every backend-facing feature built on top of that floor — the connection pool, keep-alive, the thread-per-connection server, TLS, SCRAM — is built on a runtime that cannot stay up.

I'd also flag a second-order risk: the leak is currently *load-bearing*. `src/value.h:88-97` correctly observes that cross-thread `Value` sharing needs no synchronization *because* nothing is ever freed. Fixing memory therefore also means designing cross-thread ownership. These are one project, not two.

### 4.2 The proposed fix rests on a false premise — verified

ROADMAP.md:343-356 argues that pure reference counting suffices, with no cycle collector, because:

> **the value graph can never contain a cycle** … nothing is ever mutated after being built, so nothing can ever be wired into a cycle

**This is false.** `Channel<T>` is mutable in place — `src/value.h:86` says so explicitly ("this one is genuinely mutable in place"). A struct can hold a channel of its own type, and sending it into that channel closes the loop:

```
struct Node {
    label: string
    peers: Channel<Node>
}

fn main() {
    val c: Channel<Node> = channel_new()
    val n = Node { label: "self-referential", peers: c }
    send(c, n)                      // ChannelObj -> StructObj -> ChannelObj
    val got = receive(c)
    print(got.label)
    send(got.peers, got)
    print(receive(got.peers).label)
}
// self-referential
// self-referential
// exit 0
```

`ChannelObj` → queued `Value` → `StructObj` → same `ChannelObj`. Under pure refcounting neither object ever reaches zero. The one mutable type in the language is exactly the one that reintroduces cycles, and the analysis missed it because it reasoned from "values are immutable" without excepting the type the same header file flags as the exception.

This doesn't sink refcounting — a queued value could be treated as a weak edge, or channels could be excluded from the refcounted graph and leaked deliberately (there are few, they're long-lived) — but the plan needs that paragraph rewritten before anyone implements against it.

### 4.3 The arena alternative also doesn't fit as described

ROADMAP.md:370+ prefers per-thread arenas freed "when the thread's work is done," citing Erlang/BEAM and nginx pools. The instinct is right — request-scoped regions are the correct shape for this workload — but the granularity in the proposal doesn't match the server that motivates it. `server.zm:19-41` is thread-**per-connection**, not per-request:

```
fn serve_connection(conn, sock, idle_timeout_ms, log_requests) {
    ...
    while open {
        val next = http.read_request(session)   // many requests, one thread
        ...
    }
}
```

With keep-alive and a 30s idle timeout, one thread can serve thousands of requests. A per-*thread* arena frees nothing until the client disconnects. What's needed is a per-**request** region, which is harder, because `session` deliberately carries buffered bytes across the loop iteration (a correct design for pipelining) and so must outlive each region. Values escaping over a `Channel` are the same problem in general form.

The honest summary: neither proposed fix is ready, and the arena one needs an escape analysis or a copy-on-send rule before it works.

---

## 5. Concurrency model

Real OS threads, an unbounded thread-safe `Channel<T>`, `spawn` on a statically-resolved function name, no shared mutable state, no global `var`. As a model this is coherent and the reasoning behind it (DESIGN.md's single-writer/actor entry) is sound. Forbidding a global `var` specifically because it would reintroduce exactly the shared state the model exists to avoid is good discipline.

Three real costs:

**1. No closures means the library/application boundary inverts.** DESIGN.md:2355 owns this honestly: *"A framework's central move is `serve(listener, handler)` … and Ziman can't express that."* So every application writes its own accept loop and its own `route`. The consequence is visible in `health-record-backend-ziman/routes.zm`: 8 routes cost ~55 lines of hand-written if-chain, each block a near-copy of the last:

```
val m_patient = http.route(req, "GET", "/patients/{id}")
if Option.is_Some(m_patient) {
    val text = http.param(Option.unwrap_Some(m_patient), "id")
    val id = uuid_from_string(text)
    if !is_ok(id) { return invalid_id("patient", text) }
    return respond(patients.handle_get(conn, unwrap(id)))
}
```

The production health-record backend this is modeled on has ~40 entity types. Linearly extrapolated that's a 1,500-line `routes.zm` of copy-pasted blocks differing by one identifier each — which is precisely the highest-risk shape for LLM copy-paste errors (`m_patient` vs `m_patient_meds` in an otherwise identical block). The no-closures decision is defensible in the abstract; its interaction with the AI-first goal is not obviously favorable and isn't discussed anywhere.

**2. No thread cap.** `server.zm:56-59` is `while true { accept; spawn serve_connection(...) }` with no pool and no limit. At ~8 MB of stack per thread, a few thousand concurrent connections is fatal independent of the leak. Thread-per-connection with no bound is a denial-of-service primitive.

**3. No `join`, no thread handles, no backpressure.** `send` never blocks and the queue is unbounded, so a slow consumer turns into unbounded memory growth — which, given §4, is indistinguishable from the background leak. `spawn` requiring `void` is the right call given no handles exist.

Also missing and load-bearing for a server: **no `exit()` and no signal handling** (verified: `exit` is an undefined function; no signal support in `src/native_os.c`). There is no way to terminate with a nonzero status or shut down gracefully. This produces a visible wart in the showcase app's own startup path (`server.zm:44-47`):

```
val loaded = config.load()
if !is_ok(loaded) {
    print(concat("configuration error: ", unwrap_err(loaded)))
}
val cfg = unwrap(loaded)     // ...and then crash anyway
```

It prints a friendly message, falls through, and dies on `unwrap` — because there is no way to say "stop here." A one-line native `exit(code: int)` fixes this.

---

## 6. Modules, imports, and `#[derive(...)]`

### 6.1 Modules

Path-as-identity with no manifest, `pub` for export, cycles as a compile error, project-relative resolution with a bundled-core fallback: all correct and pleasantly boring. Two choices stand out as genuinely good:

- **No wildcard imports.** `use mod.Name` names exactly what it binds, so grepping a file's `use` lines tells you the origin of every bare name. This is straightforwardly better for both humans and machines than `import *`.
- **A global `val` cannot be shadowed by a local** (a compile error, not silent shadowing). Small, cheap, eliminates a real bug class.

One wart: the two usage styles (`use http` → `http.response`, vs `use http.response` → bare `response`) collide with the stdlib's inconsistent naming (§7), and the sample backend already works around it: `use common/response as common_response`.

### 6.2 `#[derive(...)]`: right idea, incomplete plumbing

**Generating real Ziman source and re-parsing it is the correct call for this project, and I'd defend it against the "that's a hack" reflex.** `src/derive.c:15-19` gives the reason: the generated code can be printed and read. For a solo project with no macro-hygiene infrastructure, text generation into the existing lexer/parser is dramatically cheaper than an AST-transform API and produces output a human can debug. Hygiene — normally the fatal objection to textual macros — is largely a non-issue here because Ziman has no closures, no shadowing of globals, and enforced casing. This is a good pragmatic decision.

The plumbing around it is where it falls down, and all three problems are fixable:

**(a) Generated code has no source mapping.** Errors report positions inside an invisible file. Verified — the same error regardless of where the declaration sits:

```
// a struct declared on lines 10-14, with #[derive(Json)] on line 10:
ziman: type error at 1:5: unknown struct or enum type 'Json'
```

Line 1, column 5, of nothing the user can open. `parse_and_splice` (`src/derive.c:622`) never rewrites token positions to the originating declaration. For an AI-first language — where the compile error *is* the interface the agent iterates against — a wrong line number pointing into invisible text is a serious defect. Fix: stamp every spliced token with the `StructDecl`'s own line/col, and prefix the message with `in derive(Json) for 'Point':`.

**(b) The required boilerplate is unreasonable and undiscoverable.** One `#[derive(Json)]` requires the user to hand-write five imports:

```
use json.Json
use json.JField
use json.json_to_string
use json.json_require_field
use json.json_object_get
```

SYNTAX.md:1535 states the rule ("Nothing is auto-imported") as though it were a principle, but it isn't one — it's an implementation consequence. The attribute knows exactly what it generated and therefore exactly what it needs. A model will forget these, and the error it gets is (a) above: a bogus line number and a message that never mentions `use`. Auto-injecting the imports the generator itself emitted violates nothing (the names are still traceable — to the `#[derive]` line).

**(c) Name-convention dispatch leaks into the stdlib.** Because per-field conversion dispatches on `<type>_to_json`/`<type>_from_json` by name, `lib/core/json.zm:455-461` has to define identity functions so that a plain `Json`-typed field works:

```
pub fn json_to_json(j: Json) -> Json { ... }
pub fn json_from_json(j: Json) -> Result<Json, string> { ... }
```

That's the tell of a stringly-typed macro. It works, and it's a small price, but it's the seam to watch as more derives land.

Also: derives reject generic types entirely (`derive(Json) does not support a generic struct yet`). The sample backend already hit this and worked around it by typing a field as raw `Json` (`common/response.zm:30-40`), losing the type.

---

## 7. Standard library

For a solo project this is a surprising amount of real, working code: a hand-rolled Postgres wire-protocol client with SCRAM-SHA-256 (with server-signature verification), TLS via OpenSSL with libpq-compatible `sslmode`, a bounded HTTP/1.1 server with correct keep-alive and pipelining, a connection pool, `BigInteger`/`BigDecimal`-shaped numerics tested against RFC vectors, and a `java.time`-shaped date library using Hinnant's days-from-civil. `make test-pg` runs against a real server. That is not toy work, and the decision to keep everything above a thin native floor in pure Ziman is the right one — it dogfoods the language hard, and it clearly *did* surface real gaps (`StringBuilder`, `ArrayBuilder`, variadics, named args all trace back to stdlib pain).

**Naming is inconsistent between two live conventions.** `collections` uses type-prefixed names built for bare import (`hashmap_get`, `hashset_add`, `bytes_concat`); `http` uses bare names built for qualified use (`header`, `response`, `not_found`, `route`). Both are fine alone; together they mean a reader can't predict which style a module uses, and `use http.response` binds a bare `response` that collides with an application module of the obvious name. Pick one convention per intended usage mode and document it.

**Conspicuously missing for the stated backend goal:**

| Missing | Why it matters |
|---|---|
| **Sorting** | Not merely absent — *inexpressible* (no `Ord` bound, no comparators). Any ordered output must come from SQL. |
| **HTTP client** | `http` is server-only (parse request / write response). A Ziman service cannot call another service. For a backend language this is a first-order gap. |
| **`exit()` / signals** | No clean termination, no graceful shutdown, no nonzero exit status (§5). |
| **Logging** | The server does `print(format(...))`. No levels, no structured output, no destination control. |
| **Regex** | Absent. All the string work is hand-rolled `index_of`/`char_at` loops. |
| **Filesystem** | `lib/core/io.zm` is two functions: `read_file`, `write_file`. No append, stat, delete, or directory listing — so no log rotation, no migration directory. `write_file` is still fatal on failure (ROADMAP owns this). |
| **Map keyed by a domain type** | Only the 7 scalars (§2.3). |

The `properties` module (typed config with env overrides, `${VAR:default}` interpolation, unknown-key rejection at startup) is a nice piece of work and better than what many real frameworks ship. The startup report the sample backend prints — every key with its provenance (`application.properties:7` vs `env SERVER_PORT`) — is a detail I'd steal.

---

## 8. Tooling and ecosystem

What actually exists, verified:

- `editors/vscode/` — a TextMate grammar for syntax highlighting. Not published.
- `tools/gen_corelib_reference.sh` — generates `docs/CORELIB.md` from `lib/**/*.zm`. Good practice; the output is accurate.
- `tests/` — 759 fixture files, `run_tests.sh` (98 lines), plus `run_pg_tests.sh`, `run_mysql_tests.sh`, `run_tls_tests.sh`, and C unit tests for bignum/decimal. **377 pass, 0 fail.**
- A Makefile.

What doesn't exist: LSP, formatter, debugger, REPL, package manager, and — notably — **any CLI surface at all**. `src/main.c:16-19`:

```c
if (argc != 2) {
    fprintf(stderr, "usage: ziman <file.zm>\n");
    return 1;
}
```

No `--version`, no `--check` (typecheck without running), no `--emit-derived`, and no `--json` diagnostics *despite DESIGN.md principle #5 promising exactly that* ("Errors are data, not just prose… structured (machine-parseable)… lets an LLM consume a compile error programmatically without scraping a string"). That principle is listed as core and remains unimplemented; it is also, of all the unimplemented items, the one most directly aligned with the AI-first thesis and among the cheapest to build.

At this project's size most of this absence is fine — an LSP is not a good use of solo time. Three exceptions:

1. **`--json` diagnostics.** Promised by the design, cheap (the errors already carry line/col and a stable shape via `src/error.c`), and the single highest-leverage AI-first feature not yet built.
2. **`--check`.** A typecheck-only mode is ~5 lines given `typecheck(module)` is already a separate phase, and it's what an agent loop wants to call between edits.
3. **A formatter.** DESIGN.md itself flags `val`/`var` as a one-character typo that silently flips mutability and says "a future formatter/linter should visually distinguish them." Worth more than an LSP here.

### Documentation quality

`DESIGN.md` is the best artifact in the repository — a real decision log with rejected alternatives. `SYNTAX.md` is thorough and mostly precise. But:

- **`README.md` is 3,009 words in what is essentially one unbroken paragraph.** For a language whose entire thesis is that a reader (human or model) should be able to absorb the rules and get it right first try, the front door is close to unreadable. It's also the document most likely to be pasted into a context window as "here is the language." Splitting it into sections with headings is a couple of hours and is probably the highest value-per-minute change available anywhere in the project.
- **Doc drift.** `SYNTAX.md:1992` still says *"No sum types, no `enum`, no `match`"* in the `Result` section — enums shipped. `DESIGN.md:2423` still lists *"Modules / imports beyond a single file"* under "Deliberately deferred" — modules shipped.
- **`SYNTAX.md:2278` is corrupted**: the `index_of` builtin entry begins mid-signature with a stray leading comma —
  ```
  , needle: string, from: int) -> int` — the byte index
  ```
  The function name and opening are gone. A reader learning the string API from this file cannot discover `index_of` exists.

---

## 9. The "AI-first" premise

**Is optimizing a language for LLM-generated correctness a coherent goal?** Yes, but only partly for the reasons Ziman assumes.

The framing rests on an implicit model: LLM errors come from ambiguity and from syntactic forms with multiple meanings, so removing ambiguity removes errors. That's true at the margin and it's why the no-coercion and no-overloading calls are fine. But it under-weights two larger effects:

**(a) Training distribution dominates local clarity.** A model writes Ziman well in proportion to how much Ziman-shaped code it has seen. A novel language starts at zero, and every deliberate divergence from mainstream syntax — even a *clearer* one — costs generation accuracy until enough corpus exists. Ziman pays this cost in several places without acknowledging it: `Shape.unwrap_Circle(s)` instead of `match`, `arr = push(arr, x)` instead of `arr.push(x)`, `concat(a, b)` instead of `a + b`. Each is individually defensible; collectively they mean a model's priors are wrong in the most common operations in the language. My probes confirm this isn't theoretical — the most likely model habits (`p.method()`, `"a" + "b"`, `match`) all failed, and two of the three produced *misleading* messages (below).

**(b) The errors that matter are cross-file and semantic, not local and syntactic.** Models rarely emit `1 + 1.0`. They routinely miss a case after a type changes, get a boundary condition wrong, or mis-paste one identifier in a duplicated block. Of those three, only the first is addressable by language design — and it is addressable, by exhaustiveness checking, which Ziman doesn't have (§3.3). This is the central misallocation: **Ziman spent its correctness budget on the errors that are easy to catch rather than the ones that are common.**

The strongest thing I can say for the premise is that it produced good *discipline*. "Would an LLM get this right?" is a real design forcing function, and it plausibly explains why Ziman is small, orthogonal, and unusually well documented. That's a genuine methodological win independent of whether each specific choice pays off.

### Where the goal was used to justify a limitation

Three places, stated plainly:

**1. "No nested `?`" is an interpreter constraint wearing a clarity costume.** DESIGN.md is honest about this internally ("avoids needing the interpreter to unwind out of an arbitrary expression tree"), but the README presents it as a readability feature. It happens to also be good style, which is lucky, not designed. Keep the rule; drop the post-hoc rationalization.

**2. "No closures keeps libraries from taking opaque callback-shaped holes."** This is the weakest argument in the design. The actual consequence isn't that libraries got more transparent — it's that `http` can't be a framework, every app hand-writes its accept loop and a linear route chain, and no generic `sort`, `map`, `filter`, or `with_transaction` can exist. The stated benefit (call sites are obvious) is largely already delivered by "no operator overloading, no methods, no inheritance." Closures would cost little of that and buy a great deal.

**3. `for x in arr` — one syntax, two meanings, resolved by the operand's type.** This is the clearest self-contradiction in the language. Principle #1 forbids behavior that isn't derivable from the line plus its declared types; principle #2 forbids two ways to do a thing. Here, one form means "the element" or "the index" depending on `expr`'s type, and SYNTAX.md's own mitigation is *a naming convention*:

> **Naming convention, not enforced by the grammar:** name a range-loop's single `int` binder `i`/`j`/`k` … this convention is what lets a reader (human or AI) tell which case they're in at a glance

A language that enforces identifier casing in the *parser* to disambiguate struct literals, and then falls back to "please name it `i`" to disambiguate loop semantics, has an inconsistency. And it produces silent wrong answers — verified:

```
fn main() {
    val a = [2, 2, 0]
    var sum = 0
    for i in a {
        sum = sum + a[i]     // author meant a[0]+a[1]+a[2] = 4
    }
    print(sum)               // prints 2 -- no compile error, no runtime error
}
```

An LLM carrying Python's `for i in range(len(a))` or Go's `for i := range a` writes this. Sometimes it crashes with an out-of-bounds; sometimes, as here, it silently computes garbage. Requiring a distinct keyword or form for index iteration would cost one token and close it.

### Diagnostic quality is the AI-first surface, and it's uneven

If the thesis is "a model iterates against the compiler," then error messages *are* the product. Measured on the mistakes a model actually makes:

| Input | Message | Verdict |
|---|---|---|
| `val x: float = 1` | `cannot initialize 'x' of declared type float with a value of type int` | Good |
| `Box { value: 5 }` | `'Box' is generic and requires explicit type arguments, e.g. 'Box<...> { ... }'` | Excellent — shows the fix |
| `foo()?` nested | `'?' can only be used as a val/var's entire initializer or as its own statement…` | Excellent |
| `hashmap_new<Uuid, string>()` | `…does not satisfy bound 'Hash' required by type parameter 'K'` | Good |
| `p.to_string()` | `module error: no module imported under the name 'p'` | **Bad** — the #1 model habit, and the message sends you to fix an import instead of telling you Ziman has no methods |
| `"a" + "b"` | `arithmetic operator requires two operands of the same numeric type, found string and string (use 'as' to cast explicitly)` | **Bad** — self-contradictory (they *are* the same type), and suggests a cast that cannot exist. Should say `use concat(a, b)` |
| `match e { }` | `syntax error: expected an expression, found '{'` | **Bad** — never mentions that `match` doesn't exist |
| `#[derive(Json)]` w/o imports | `type error at 1:5: unknown struct or enum type 'Json'` | **Bad** — invisible file, wrong line, doesn't say `use json.Json` |

Four good, four bad, and the bad ones are precisely the highest-frequency model mistakes. Each fix is a special-cased message of a few lines. This is the cheapest available improvement to the stated goal in the entire project.

---

## 10. Top 10 recommendations

Ranked by impact × feasibility for a solo, AI-paired project.

1. **Special-case the four bad diagnostics above** (`p.method()` → "Ziman has no methods; call `to_string(p)`"; `string + string` → "use `concat(a, b)`"; `match` → "Ziman has no `match`; use `Enum.is_X`/`Enum.unwrap_X`"; derive errors → real line numbers + "add `use json.Json`"). *A day's work, and it directly improves the exact loop the language exists to optimize.*
2. **Reject the `Result<T, E>` bare-value auto-wrap when `T == E`.** *Closes a silent error-becomes-success hole in the stdlib's most common Result shape; a few lines in the typechecker; breaks nothing that wasn't already a latent bug.*
3. **Add `-fwrapv` and `-O2` to `Makefile:2`.** *One line. Removes live undefined behavior and buys 2-3x interpreter speed.*
4. **Add an exhaustive `switch`-over-enum statement** (one arm per variant, compile error if any is missing), desugaring to the existing `is_`/`unwrap_`. *Not full `match` — no binding patterns — but it closes the single largest correctness gap relative to the AI-first goal using machinery that already exists.*
5. **Split `README.md` into sections.** *3,009 words in one paragraph is the front door and the most-pasted context. Hours of work, disproportionate return.*
6. **Add `exit(code: int)`, a `--check` flag, and `--json` diagnostics.** *All small; `--json` is a stated core principle (DESIGN.md #5) that's still unbuilt and is the most AI-first-aligned item on the list; `exit` fixes a visible wart in the showcase app.*
7. **Rewrite ROADMAP.md:343-356 before implementing any memory fix, and re-scope the arena plan to per-request.** *The cycle-freedom premise is false (§4.2, reproducible), and the per-thread arena granularity doesn't match the thread-per-connection server (§4.3). Cheap to fix on paper, expensive to discover after implementing.*
8. **Add an `Ord` bound and a generic `sort`.** *`Eq`/`Hash` already prove the mechanism. Unblocks the nine ad-hoc `_compare` functions and removes a hard expressiveness cliff.*
9. **Auto-inject the imports that `#[derive(...)]` itself generated, and stamp generated tokens with the originating declaration's line/col.** *Removes the worst boilerplate-plus-bad-error combination in the language; the generator already knows both facts.*
10. **Cap `spawn` in the sample server with a bounded worker pool, and fix `server.zm:44-47`'s print-then-crash config path.** *Documentation-by-example matters; the showcase app is what a model will imitate.*

Deliberately **not** on this list: a GC or refcounting implementation. It's the biggest real problem, but it's a months-long cross-cutting change that needs the §4.2/§4.3 design errors resolved first — and items 1-10 are each a day or less. Get the design right on paper (item 7) and keep shipping the cheap wins.

---

## 11. Things done right that other language designers should copy

- **Block-scoped `defer`.** Go's function-scoped version is a documented trap (defers accumulating in a loop); Ziman fixed it deliberately and correctly, kept Go's better rule (arguments evaluated at registration), and documented the one lossy case (a dropped `Result`) as a real cost rather than hiding it. **This is strictly better than the language it borrowed from.**
- **Parser-enforced identifier casing.** Because a value name can never start uppercase, a struct literal is unambiguous in *every* expression position — including inside an `if`/`while` condition, where Rust needs a special restricted-position rule. A tiny constraint that buys real grammar simplicity. Copy this.
- **`format`'s first argument must be a string literal**, so the placeholder count is checked against the argument list at compile time. Eliminates an entire bug class (printf arity/type mismatches) with no runtime cost and no type-level machinery. Copy this.
- **The immutable-value + shared-pointer model.** Semantics say "always copies," implementation shares pointers, and the invariant that makes it safe is documented where it's relied upon (`src/value.h:10-20`). Clean, fast, and it makes cross-thread sharing free.
- **Named arguments reusing struct-literal syntax**, all-or-nothing per call, labels matching declared parameter names with no separate external label. One syntax for "name a value," no Swift-style dual-label complexity.
- **`Option<T>` parameter omission with defaults evaluated once at program start.** Good ergonomics with a crisp rule, and the restriction to `Option`-typed parameters keeps it from becoming general default arguments.
- **No wildcard imports.** Every bare name's origin is greppable in the file's own `use` lines. Straightforwardly better than `import *`.
- **A global `val` cannot be shadowed by a local** — compile error, not silent shadowing. Cheap; removes a real bug class.
- **Generating derive output as readable source text rather than opaque AST.** For a project this size it's the right engineering trade, and the debuggability argument in `src/derive.c:15-19` is correct. (Just map the source locations back.)
- **`docs/DESIGN.md` as a decision log with rejected alternatives.** This is the single most transferable practice here. Most languages document what they did; very few document what they considered and why they didn't. It makes the project reviewable by a stranger in an afternoon — which is exactly what happened.
- **`docs/CORELIB.md` generated from source** via `make corelib-reference` rather than hand-maintained. It's accurate, which hand-written API docs never stay.
- **A ROADMAP that names the project's own worst flaw in detail.** ROADMAP.md:308 is a better description of the memory problem than most postmortems. The analysis has an error (§4.2), but the honesty is real and rare, and it's why this audit could go straight to measuring rather than discovering.

---

## Appendix: reproduction notes

All probes were run against `ziman` built from `main` at `7b625f6`, on Linux 7.2.5 x86_64, using the prebuilt binary at repo root. The load test used `health-record-backend-ziman/server.zm` with `SERVER_PORT=18099 SERVER_LOG_REQUESTS=false` and the default in-memory SQLite datasource. No files in either repository were modified. `make test` was run once (377 passed, 0 failed) and writes only to `src/*.o` and `/tmp`.
