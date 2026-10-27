# Ziman: independent language-design review (2026-10-05)

Reviewed at `ziman` commit **3c0908f**; the reference application `health-record-backend-ziman` at **bd664c7**.
Starting test state, run once before anything else: **`make test`: 845 passed, 0 failed** (exit 0); **`make test-lsp`:
0 failures**. The prebuilt `./ziman` was already current (`make -q` reported nothing to do); I did not run `make gc`,
`make nogc` or `make clean`, and I did not run the backend's own test suites or bind any port.

Method: read `docs/SYNTAX.md` in full, `docs/DESIGN.md`'s principles and non-goals plus about twenty of its decision
entries, `docs/ROADMAP.md`'s open items, `docs/MEMORY.md`, the `pub` surface of every bundled module and the sources of
`json`, `fs`, `net`, `log`, `collections`, `datasource`, `db`, `server` and `client`, and the reference backend
(14,058 lines of Ziman across 152 files). I then ran about 70 scratch programs under `/tmp/ziman_review_lang/`.
Nothing in either repository was modified except this file. I read `docs/reviews/*` only *after* forming a view on
each topic; §3 says which of their findings I re-ran and what the result was.

Convention: **[V]** = I ran it or read the cited line. **[O]** = opinion.

---

## 1. Executive summary

1. Ziman is the most disciplined small language I have reviewed in a while: value semantics, zero implicit coercion
   (two narrow, well-guarded exceptions), exhaustive `switch`, must-use `Result`, block-scoped `defer`, enforced
   identifier casing, and derives that emit readable source. [V]
2. Almost everything the two earlier reviews found has been fixed and I re-verified it: 44 of the 45 tracked tasks are
   closed, and the numeric, stack-safety and builder-aliasing holes are genuinely gone. [V]
3. The remaining single largest risk is unchanged and correctly named by the project itself: **under concurrency the
   collector makes allocation-heavy work slower in aggregate than one thread** — 8 threads doing 8× the work took
   10.6× longer per thread, while pure compute scaled 4.7×. [V]
4. **New defect: invalid UTF-8 escapes into JSON output.** A byte-level `substring` that splits a code point flows
   through `#[derive(Json)]` into a document Python's `json` rejects and Ziman's own `parse_json` accepts. [V]
5. **New defect: `NaN` silently breaks `Map`/`Set`.** `compare(nan, nan) == 0` but `nan == nan` is false, so a set
   accumulates duplicate `nan`s it reports not containing. [V]
6. **New defect: a stack-depth error costs 32.5 s, 1.26 GB and 97,390 lines of stderr** — `build_trace` is quadratic
   and unbounded, and Boehm's warnings are never suppressed. [V]
7. The actor story is half-built: a dead spawned thread leaves every peer blocked on `receive` forever, with no join,
   no supervision and no deadlock detection. `server.serve` works around it per connection; nothing else can. [V]
8. The library's breadth (36 modules, TLS, three SQL drivers, JWT, migrations, regex) is its best and riskiest
   feature, and its weakest axis is still error types: 242 `Result<_, string>` against five structured enums. [V]
9. Diagnostics are a genuine competitive advantage — four or five messages here teach better than Rust's. Four weak
   ones are listed in §6. [V]
10. **Verdict: the front end and the library are production-shaped; the runtime is not yet.** Ship no new modules
    until §8's P0 list is closed.

---

## 2. What changed since the last review

The previous two reviews are tracked as `docs/reviews/tasks.md`, which reports 44 of 45 items done. I did not trust
the ticks; I re-ran a representative sample.

**Fixed, re-verified [V]:**

| Prior finding | Now |
|---|---|
| S1 stack overflow is an uncontrolled segfault | A clean runtime error; `--stack-mb` / `ZIMAN_STACK_MB` tune it |
| S2 builders alias and corrupt the heap across threads | `#[thread_bound]`: compile error as a `spawn` argument, a channel element or a module-level `val`, plus a runtime owner check; transitive through struct fields and array/channel element types |
| S3 a runtime error in any thread kills the process | Ends that thread only, defers run, `spawn f(x) on_error h` chooses the policy |
| S4 out-of-range int literals saturate | Syntax error naming the `n` suffix |
| S5 `float as int` / `as bigint` overflow to `INT64_MIN` | Saturates (`9223372036854775807`) and converts exactly |
| S6 `%` on floats reaches the interpreter as an internal error | Type error with a teaching message |
| S7 float display is lossy `%g` | Shortest round-trip (`98765.4321`, `10.0`, `1234567.891`) |
| S8 no math library | `math` module exists |
| S10 a bare `Result` statement is silently discarded | Must-use `Result`, with `val _ =` as the opt-out |
| `push` is quadratic | Amortised O(1): 300,000 pushes in 0.06 s |
| 2.2(a) `?` not nestable | `try` with parser hoisting: `ok(try parse_int(s) + 1)` and `g(try a(), try b())` both work |
| 2.2(b) no error conversion | `#[from]`, verified one-step and chained through two enums |
| 2.2(c) `return None` fails | Bare `None`/`Some` work in return position (but *not* in a call argument — see §4, L4) |
| 2.2(e) mandatory type arguments | Inferred from plain arguments; `option_unwrap_or(o, 0)` works |
| 2.2(i) `==` rejected on arrays of structs/enums | Works, including `Json == Json` |
| `parse_int` leniency | `parse_int(" 5")` and `parse_int("+5")` are both `Err` |
| Internals exported `pub` | `RunRequest`, `run_request`, `PoolState`, `IdleConnection`, `idle_put/take` are now `internal` |
| No channel close / timed receive / select | `channel_close`, `receive_option`, `receive_timeout`, `select2` |
| No unified error message | `error_message(e)` builtin (`src/typecheck.c:1650`) |

**Still holds [V]:**

* A function-typed struct field cannot be called directly, **and the diagnostic is actively misleading** (§4, L2).
  This is the prior review's 2.2(f), untouched, and it is the single most-repeated papercut in the reference backend.
* The error-type zoo (§5, D1): 242 `Result<_, string>` vs `DbError` 100, `ClientError` 25, `NetError` 16,
  `HttpError` 10, `JwtError` 7.
* `db.percent_decode` (`lib/db/db.zm:180`) and `http.percent_decode` (`lib/http/http.zm:188`) are still two functions
  with different signatures *and* different error types.
* No nested functions, with a poor message (`expected '(', found identifier`).
* The explicit-type-argument tax, now narrowed to the lambda cliff (§4, L3) — which is where it bites most.

**Where I disagree with an earlier review [O]:**

* `pl-expert-review` §2.2 calls the `Option`/`Result` auto-wrap "a contradiction the docs half-admit". I spent
  real effort trying to turn it into a silent wrong answer and could not: the `T == E` case, the
  `Option<Option<T>>` case and the array-element case are all compile errors with specific messages. It is a
  narrow, well-guarded sugar and I would keep it. The honest cost is not unsoundness, it is that the *reach* of
  expected-type propagation is now a list you have to memorise (§4, L7).
* `language-design-review` §2.2(g) says the "no overloading anywhere" claim is false because `len`/`contains`/`abs`
  are overloaded. That is correct as stated, but I would not change the design — a closed set of compiler-known
  intrinsics is a defensible and common choice (Go does exactly this with `len`/`cap`/`append`). I would change the
  *sentence* in `SYNTAX.md`, not the language.
* `language-design-review` §2.3 frames handle types as "value semantics advertised more strongly than delivered".
  With `#[thread_bound]` landed and the checker transitive through struct fields, I think the current position —
  handles exist, they are a named category, and the checker refuses them wherever they could be shared — is
  principled rather than a gap. The remaining hole is the depth-12 cutoff (§4, S4), not the design.

---

## 3. Language design coherence

### 3.1 What is principled, and worth defending against future pressure [O, with [V] on behaviour]

**Enforced identifier casing.** Because a value name can never be uppercase, a struct literal parses unambiguously
in every expression position, including an `if` condition — the thing Rust needs a restricted-position rule for.
A one-line constraint that buys real grammar simplicity. Keep it.

**`format`'s template must be a literal.** Placeholder count is checked at compile time against the argument list,
killing the entire printf arity bug class with no type machinery. `format_dynamic` exists for the genuine
message-catalog case and returns a `Result`. This is the cleanest solution to that problem I have seen.

**Must-use `Result` with `val _ =` as the only opt-out, and `defer` as the one documented exception.** Verified: a
bare `risky(-1)` statement is a compile error; `val _ = risky(-1)` and `try risky(-1)` both pass. Combined with
exhaustive `switch`, this is the highest-value pair of static checks in the language. [V]

**Block-scoped `defer` with arguments evaluated at registration.** Strictly better than Go's function-scoped version,
and the one lossy case (a discarded `Result`) is documented as a cost rather than hidden.

**`try` written in front, with parser hoisting.** `try` reads worse than `?` to anyone coming from Rust, but the
hoisting makes it *more* capable than the old `?`: `ok(try parse_int(s) + 1)`, `g(try a(), try b())` and a `try`
inside a lambda body all work, with left-to-right evaluation preserved. The refusals (`else if`/`while` condition,
right of `&&`/`||`, `defer`/`spawn` argument, inside an `if`/`switch` used as a value) are a coherent set with one
rule behind them — "runs exactly once, unconditionally". [V]

**`#[from]` instead of a `From` trait.** A closed, per-variant, one-step conversion with "at most one `#[from]` per
payload type" enforced. It deletes the boilerplate the earlier review counted 76 copies of, without opening the
coherence can of worms. Verified working one-step and chained through two enums. [V]

**`unit` as a soft keyword.** `unit` is a type where a type is expected and an ordinary name elsewhere, so a JSON
field called `unit` is fine. It is `==`-comparable, hashable, usable as a `Map` key and prints as `unit`. Verified
all of it. A small, complete design. [V]

**No wildcard imports, non-shadowable globals, derive output as readable source, insertion-ordered `Map`/`Set`.**
All four are the right call and all four are rarer than they should be.

### 3.2 Problems, ordered by how much they will hurt as programs grow

**L1 — Flat module names do not scale, and the reference backend proves it. [V]**

A module's name is its path's last segment, so `controller/patient`, `service/patient`, `model/patient`,
`mapper/patient`, `entity/patient` and `repository/patient` all want to be called `patient`. The only tool is a
hand-written alias. Measured in the backend:

* 1,261 `use` lines in 14,058 lines of code — **about 9% of the program is import boilerplate**.
* 351 of them carry a forced `as` alias.
* `routes.zm` alone has 69 `use` lines, 52 of them aliased, including 59 project-wide instances of the invented
  `_model` suffix convention (`use model/patient as patient_model`) that exists purely to dodge the collision.

This is not a style complaint; it is a naming scheme that makes every layered program invent the same workaround.
Two cheap directions, neither needing a module system rewrite: (a) let a `use` bind a *multi-segment* reference, so
`use model/patient` is reachable as `model.patient.PatientResponse` and never collides; or (b) make the default local
name the last *two* segments when the last one is already taken, and say so in the error. **P1, M.**

**L2 — A function-typed struct field cannot be called, and the error message sends you the wrong way. [V]**

```
struct Ops { run: fn(int) -> int }
fn main() {
    val o = Ops { run: fn(n: int) -> int { n * 2 } }
    print(o.run(21))
}
```
```
ziman: module error at 4:11: 'o.run(...)': 'o' is not an imported module, and Ziman has no methods
  -- if 'o' is a value, call a function on it instead, e.g. run(o, ...) (if 'o' should be a module, add 'use o')
```

The advice is wrong for this program: `run` is a field holding a function, and `run(o, ...)` does not exist. The
workaround is `val run = o.run` then `run(21)`, and the compiler already knows `o`'s type and that it has a
function-typed field spelled `run`. Evidence that this matters: `health-record-backend-ziman/src/main/ziman/service/crud.zm`
carries a 4-line comment explaining the workaround (lines 25-28) and applies it at every call site: 11 extractions
of a function-typed field in the backend, **all of them in `crud.zm`** — the one file where an abstraction over
behaviour lives. The library does the same at `lib/http/server.zm:1006` (`val encode = w.encode`), `:1086`
(`val fault = api.fault`), `:1122` (`val decode = chosen.decode`) and `:1393` (`val h = e.handler`).

Two independent fixes, both worth doing. First, the message: when the receiver's type has a function-typed field of
that name, say so and name the workaround. Second, the language: let `a.f(args)` mean "call the function value in
field `f`" when `a` is a local of a struct type with such a field. The grammar already distinguishes
`qualified_call` from field access by lookahead, and a module qualifier is always lowercase and declared by a `use`,
so the resolution is unambiguous at check time. **Message: P1, S. Language: P1, M.**

**L3 — A fully annotated lambda does not contribute to type-argument inference, and the docs say it should. [V]**

```
fn apply2<T, U>(x: T, f: fn(T) -> U) -> U { return f(x) }
fn main() { print(apply2(3, fn(n: int) -> string { str(n) })) }
```
```
ziman: type error: cannot infer type argument 'U' of 'apply2' from its arguments; write it explicitly
```

But the same call with a *named* function works:

```
fn tostr(n: int) -> string { return str(n) }
fn main() { print(apply2(3, tostr)) }        // prints 3
```

`SYNTAX.md` (Generics) says only "an **unannotated** lambda" contributes nothing. This lambda is fully annotated and
has a complete, known type. The cause is `src/typecheck.c:1251`: `if (e->kind == EXPR_LAMBDA) return 1;` — the
`expr_needs_expected_type` predicate rejects every lambda regardless of annotation. The user-visible consequence is
that the library's single most common call shape needs its type arguments spelled out:
`collections.map<int, string>(xs, fn(n: int) -> string { str(n) })`, while `collections.filter(xs, lambda)` infers
fine because it only has `T`. The fix is to return 0 when every parameter has a declared type and `->` is written.
**P1, S** — this is the highest ergonomic return per line of C in the whole report.

**L4 — Bare `Some(x)`/`None` do not reach a call argument, but bare-value auto-wrap does. [V]**

```
fn f(o: Option<int>) -> string { return str(option_unwrap_or<int>(o, -1)) }
f(4)                        // works -- auto-wraps to Some(4)
f(Option<int>.Some(4))      // works
f(Some(4))                  // type error: cannot infer the type of 'Some(...)' here
f(None)                     // type error: cannot infer the type of 'None' here
```

So the *implicit* spelling is accepted in an argument and the *explicit* one is rejected. `SYNTAX.md` says bare
`Some`/`None` are legal "wherever an `Option<T>` is already expected (the same four positions `ok`/`err`/
`channel_new` get)", and the Option-omission section lists a call argument as a position where a bare `T` wraps —
so the two sugars have different reach and the docs do not name the difference. A function parameter's declared type
is exactly as known as a struct field's. Add the argument position to the `Some`/`None` list. **P1, S.**

**L5 — A `try` in a lambda whose return type is not a `Result`/`Option` gets the wrong diagnostic. [V]**

```
val f = fn(s: string) -> int { try parse_int(s) }
```
```
ziman: type error: 'try' cannot be used here: it must run exactly once and unconditionally (not in an 'else if'
  or 'while' condition, on the right of '&&' or '||', in a 'defer' or 'spawn' argument, or inside an 'if' or
  'switch' used as a value); put it in its own 'val' first
```

None of the five listed reasons applies. The real reason is "this lambda returns `int`". The equivalent top-level
function gets the right message:

```
fn f(s: string) -> int { try parse_int(s) }
ziman: type error: 'try' on a Result<T, E> value requires the enclosing function to return Result<..., string>, found int
```

The cause is reachability: the lambda's bare-final-expression path never reaches `check_try_unwrap`, so it falls
into the generic catch-all at `src/typecheck.c:2783-2785`. Route the lambda body through the same check. **P2, S.**

**L6 — Two documented asymmetries that read as accidents. [O]**

* A **lambda**'s final bare expression is its result; a **named function**'s is not. So
  `fn f(s: string) -> Result<int, string> { ok(try parse_int(s) + 1) }` is a type error while the identical body
  inside `fn(s: string) -> Result<int, string> { ... }` works. [V] Both are documented, but the same body text
  meaning different things in the two forms is a cliff a reader will fall off exactly once per week.
* Expected-type propagation reaches a `val`'s declared type, a struct field, a `with` override, an argument and a
  return — but not an array literal's elements: `val xs: [Option<int>] = [1, 2]` is a type error and you must
  write `[Option<int>.Some(1), ...]`. [V] Consistent with the docs' enumeration; inconsistent with the mental model
  the enumeration is trying to express.

**L7 — No nested functions, with a bad message. [V]** `fn inner() { }` inside a function gives
`expected '(', found identifier`, which does not mention that nested functions do not exist. For a language whose
diagnostics are otherwise this good, a parser-level special case is worth the ten lines. The *restriction* itself is
fine for v0 — a lambda plus the `val NAME = fn ... -> T` self-call rule covers the common case. **P2, S.**

**L8 — The no-traits stance is well argued, and the library's own shape is its invoice. [O]**

`DESIGN.md`'s "traits/interfaces stance" entry (2026-09-28) is the best-reasoned section in the docs: the general
form needs coherence and orphan rules, no concrete program has needed it, and the narrow fix (adding `Row` to the
closed `Eq`/`Hash`/`Ord` bound list) addresses the one real pain point. I agree with the decision. I would only
record what it costs, visibly, in two places:

* `lib/http/server.zm:1140-1340` has **nine** handler constructors — `secure_handler`, `secure_path_handler`,
  `secure_query_handler`, `secure_body_handler`, `secure_path_body_handler` and four `public_*` counterparts — each
  a ~20-line adapter onto one `endpoint_core<C, U, P, Q, I, O, E>`, with `unit` used as a type-level "this slot is
  absent" marker. That is the cross product {secure, public} × {∅, path, query, body, path+body} written out by
  hand because the type system cannot abstract over which slots are present. [V]
* `crud.zm`'s `Crud<E, Req, Res>` is a trait hand-rolled as a struct of six function values, and its own comment
  notes that nothing stops a copy-paste mistake wiring one resource's `from_row` to another's `to_params`, since
  both are just function values of matching shape. [V] The `Row` bound closes exactly this; it is worth prioritising
  over any new module.

---

## 4. Semantics and safety

### 4.1 What holds up

Integer overflow traps on `+ - * /`, unary `-` and `abs(MIN)`; `int as byte` and `bigint as int` saturate;
`float as int` saturates and NaN is 0; an out-of-range literal is a compile error. `%` is a compile error on
floats. Equality and hashing are structural, so `a == b` implies `hash(a) == hash(b)` for free, and the recursive
`Eq` rule correctly rejects a struct reachable to a `Channel` (`struct Node { ch: Channel<int>  next: [Node] }`
is not comparable, in either field order). Integer `/` and `%` truncate toward zero consistently
(`-7 / 2 == -3`, `-7 % 2 == -1`, `7 % -2 == 1`). Maps and sets iterate in insertion order, which makes tests and
logs deterministic. The type checker resisted every soundness probe I wrote: generic type-argument substitution,
`Box<A>` → `Box<B>` field assignment, auto-wrap when `T == E`, derive over a function-typed field. [V]

The collector reclaims. 4 million `Map` entries churned through 2,000 calls, every one garbage, held RSS flat at
41 MB. The "memory is never reclaimed" item in `ROADMAP.md:728` is correctly annotated as superseded. [V]

### 4.2 S1 — `NaN` silently breaks `Map` and `Set`. [V]

```
use math
fn main() {
    val nan = math.sqrt(-1.0)
    print(compare(nan, nan))       // 0
    print(nan == nan)              // false
    var s = Set<float> {}
    s = insert(s, nan)
    s = insert(s, nan)
    print(len(s))                  // 2
    print(contains(s, nan))        // false
    var m = Map<float, int> {}
    m = set(m, nan, 1)
    m = set(m, nan, 2)
    print(len(m))                  // 2
    print(map_get(m, nan))         // None
    print(m)                       // {nan: 1, nan: 2}
}
```

A `Set<float>` that contains two copies of a value it reports not containing; a `Map<float, _>` whose `len` lies
about its distinct keys, that grows without bound on repeated `set` of the "same" key, and whose entries are
unreachable by `map_get`. Printing it shows two identically-spelled keys.

The pieces are each documented: `SYNTAX.md`'s `hash` entry warns "`NaN` is never found (doesn't equal itself)", and
the Ordering section states "`float`'s order is total (NaN sorts after everything, **equals itself**)". So the docs
deliberately let `compare` and `==` disagree. The consequence for collection keys is not documented and is worse
than either neighbour: Rust refuses the problem by not giving `f64` `Eq`/`Hash` at all, so a float cannot be a
`HashMap` key; Java normalises through `doubleToLongBits`, so `HashMap<Double, ?>` finds `NaN`. Ziman accepts the key
and then misbehaves.

Fix (pick one, both are small): canonicalise `NaN` in `hash` and in the `==` used for key comparison so that
collections agree with `compare` (Java's answer, and it preserves "`float` is `Hash`"), **or** make `float`/`decimal`
not satisfy the `Hash` bound as a key type (Rust's answer, more disruptive). I would take the first.
**P0, S.** Also: whichever is chosen, say in `SYNTAX.md` that `compare` and `==` deliberately disagree on `NaN`,
because today that fact is split across two bullets and one `DESIGN.md` entry.

### 4.3 S2 — Invalid UTF-8 escapes into JSON output. [V]

```
use fs
use json
#[derive(Json)]
struct Note { text: string }
fn main() {
    val n = Note { text: substring("héllo", 0, 2) }   // splits the two-byte 'é'
    val _ = fs.write_bytes("out.json", json.json_to_bytes(note_to_json(n)))
}
```
```
$ xxd out.json
00000000: 7b22 7465 7874 223a 2268 c322 7d         # {"text":"h<0xC3>"} -- a lone UTF-8 lead byte
$ python3 -c "import json; json.loads(open('out.json','rb').read().decode('utf-8'))"
UnicodeDecodeError: 'utf-8' codec can't decode byte 0xc3 in position 10: invalid continuation byte
```

And from inside Ziman, `is_ok(json.parse_json(s))` on the same text is **`true`** — so round-tripping through Ziman
hides a document no conformant consumer can read. For a language whose North Star is an HTTP/JSON backend, that is
the worst shape a bug can have: it passes every test written in the language and fails at the client.

`DESIGN.md`'s UTF-8 entry (2026-09-29) states the model as "validate at the one real boundary" — `bytes_to_string`,
which does correctly reject invalid input. The flaw is that **JSON and HTTP output are also boundaries** and neither
validates. `truncate_chars` exists precisely so that user code can avoid splitting a code point, but nothing forces
its use and `substring` is the function everybody reaches for.

Fix, cheapest first: validate in the JSON string encoder and either return a `Result` or substitute U+FFFD (the
latter matches what browsers and most JSON libraries do, and keeps `json_to_bytes` infallible). Consider the same in
`log.write` and the HTTP response writer. **P0, S-M.**

### 4.4 S3 — A stack-depth error costs 32.5 s, 1.26 GB and 97,390 lines of stderr. [V]

```
fn depth(n: int) -> int { if n == 0 { return 0 } return 1 + depth(n - 1) }
fn main() { print(depth(100000)) }
```
```
$ /usr/bin/time -f "%e s, %M KB" ./ziman e2_deeprec.zm 2>&1 >/dev/null | tail -2
32.54 s, 1262044 KB max rss
$ ./ziman e2_deeprec.zm 2>&1 >/dev/null | wc -l
97390
$ ./ziman e2_deeprec.zm 2>&1 >/dev/null | sort | uniq -c | sort -rn | head -2
  85631     at depth (e2_deeprec.zm:3:16)
   5906 	May lead to memory leak and poor performance
```

Two independent defects compound:

1. **`build_trace` is quadratic and unbounded** (`src/error.c:289-330`). It `malloc`s a buffer one frame larger and
   `memcpy`s the whole accumulated trace on every one of 85,631 frames. That is both the 32.5 s and the 1.26 GB, and
   it is also what triggers the second problem.
2. **Boehm's warnings are never suppressed.** `GC_set_warn_proc` is called nowhere in `src/`; `GC_INIT()` at
   `src/main.c:288` leaves the default, so 5,906 "Repeated allocation of very large block" warnings — artifacts of
   (1) — go straight to the user's stderr, interleaved with the trace.

The error message itself is excellent and arrives first. Everything after it is noise. Fixes: build the trace into a
growing buffer with doubling (or a `StringBuilder`-shaped accumulator), cap it at the Java-style "first 32 frames,
`... N more`, last 8 frames", and call `GC_set_warn_proc` with a silent or log-routed handler next to `GC_INIT()`.
**P1, S.** This one matters beyond aesthetics: a 32-second, 1.26 GB response to an input-triggered recursion is a
denial-of-service primitive in a server, which is exactly what the stack guard was added to remove.

### 4.5 S4 — The static thread-bound check silently stops at nesting depth 12. [V]

`type_holds_thread_bound` (`src/typecheck.c:55`) opens with `if (depth > 12) return 0;`. A `StringBuilder` wrapped
in 12 or more layers of struct therefore passes `spawn` unchallenged:

```
struct S0 { sb: StringBuilder }
struct S1 { inner: S0 }
... // S2 .. S14
fn worker(c: Channel<int>, v: S14) { sb_append(v.inner.inner....sb, "x")  send(c, 1) }
```

| nesting depth | `--check` |
|---|---|
| 10 | rejected |
| 11 | rejected |
| 12 | **accepted** |
| 14 | **accepted**, then a runtime owner error in the worker thread |

Not unsound — the runtime owner check still fires — but a static guarantee that silently degrades on deep types is
worse than one that is honestly partial, because nothing tells you which side of the line you are on. Replace the
cutoff with cycle detection over the set of struct/enum names already visited (the recursive-`Eq` code already needs
the same machinery), and reserve a depth limit for genuinely unbounded recursion with an error rather than a
silent `0`. **P2, S.**

### 4.6 S5 — A dead spawned thread hangs its peers forever, with no diagnostic. [V]

The thread-bound check deliberately does not look through a function value's captures (the code comment at
`src/typecheck.c:52-54` says so: "A function type does not count (what a closure captured is not in its type; the
run-time owner check covers that)"). That is defensible. What is not defensible is the failure mode:

```
fn worker(c: Channel<fn() -> string>, done: Channel<string>) {
    val f = receive(c)
    send(done, f())
}
fn main() {
    val c: Channel<fn() -> string> = channel_new()
    val done: Channel<string> = channel_new()
    spawn worker(c, done)
    val sb = sb_new()
    sb_append(sb, "owned by main")
    val f = fn() -> string { sb_to_string(sb) }
    send(c, f)
    print(receive(done))
}
```

`--check` passes. At run time the worker correctly dies with the owner error — and `main` then blocks on
`receive(done)` **forever**. I killed it at 120 s. Swap `receive` for `receive_timeout` and you see the sequence:
the owner error, "the thread started by 'spawn worker' at 10:5 ended; the program goes on", then `None`.

The owner check is doing its job; the *process* has no way to learn that the thread it is waiting for is gone.
`SYNTAX.md` is upfront ("there is no deadlock detection", "no join or thread handle"), and `server.serve` works
around it with `on_error` per connection. But this is the actor model's missing half: Elixir's pitch is
`spawn` + mailboxes **+ links and supervisors**, and the third part is what makes the first two usable. Concretely,
the cheapest thing that removes the silent hang: when a thread holding the only live reference to a channel's
receive side ends, close that channel, so a blocked `receive` fails rather than hangs; and add a documented pattern
(or an `on_error` that can `channel_close` a reply channel) for "the worker died, tell the waiter". **P1, M.**

Independently: give the static check a *second* hook for the lambda case that is actually checkable — a lambda whose
captured set contains a thread-bound value is not sendable, and the capture set is known at the point the lambda is
type-checked. That turns a runtime hang into a compile error for the common case. **P2, M.**

### 4.7 S6 — Allocation throughput falls as threads are added. [V]

This is the project's own open item F7, and I reproduce it from the other end — a microbenchmark rather than the
HTTP server — because the shape of the result is sharper than the `req/s` table.

```
struct Rec { id: int  name: string  tags: [string] }
fn work(id: int, done: Channel<int>) {
    var total = 0
    for r in 0..600000 {
        val rec = Rec { id: r, name: format("name-{}", r), tags: ["a", "b", "c"] }
        total = total + rec.id + len(rec.tags)
    }
    send(done, total)
}
```

| threads | allocation-heavy | per-thread throughput | pure compute (`n + i % 7`) | speedup |
|---|---|---|---|---|
| 1 | 0.28 s (97% cpu) | 2.14 M iter/s | 1.48 s (99% cpu) | 1.0× |
| 2 | 0.37 s (179% cpu) | 1.62 M iter/s | 1.53 s (193% cpu) | 1.9× |
| 4 | 0.98 s (253% cpu) | 0.61 M iter/s | 1.81 s (370% cpu) | 3.3× |
| 8 | 2.97 s (333% cpu) | 0.20 M iter/s | 2.53 s (646% cpu) | **4.7×** |

Each thread does the same fixed work, so the wall time *should* stay near 0.28 s. Instead 8 threads take 10.6× as
long as 1, and **total** throughput falls from 2.14 M to 1.62 M iter/s — adding cores makes the program slower in
aggregate. Re-run twice: 2.68 s and 2.76 s at 8 threads, 0.28-0.29 s at 1. The control row is the important one: the
interpreter itself parallelises fine at 4.7× on 16 cores, so this is purely the collector. `--profile` on a
200,000-entry map loop corroborates it directly: **81.2% of samples in `(garbage collection)`**, 18.8% in `main`.

`docs/MEMORY.md` diagnoses this correctly (stop-the-world pauses that get both longer and more costly as client
count rises) and `tasks.md`'s F7 entry records, with evidence, that the incremental `gc_heap` migration plan is a
dead end because each half is the other's prerequisite. I have nothing to add to the diagnosis; I would only
reinforce the prioritisation. **This is the one thing standing between Ziman and its stated North Star**, and no
amount of library surface changes that. **P0, L.** If a full per-thread heap is too large a bet, the intermediate
step with the best ratio is to stop allocating per iteration: `format` and small struct/array construction are the
hot allocators, and an escape-analysis-free version of the same win is a per-call reusable buffer for `format`'s
output and a free list for small `StructObj`s, both of which reduce collection *frequency* without touching the
collector's design.

### 4.8 S7 — Smaller semantic notes [V]

* **`float / 0.0` is a runtime error, not an IEEE infinity**, and `SYNTAX.md:165` says only "`float` — double-precision
  IEEE 754". The behaviour is deliberate and `DESIGN.md:3882` records it (including that an early draft of `math`'s
  own comments got it wrong), but the language reference never states it and the "Other limits" table omits it.
  `NaN` is reachable only through `math.sqrt(-1.0)` and `parse_float("nan")`. Add one clause to `SYNTAX.md:165`.
* **`set(arr, i, x)` in a loop is quadratic**, as documented: 20,000 elements 0.23 s, 100,000 elements 3.99 s
  (5× the data, 17× the time). The docs name `ArrayBuilder` as the fix in the right place. Fine as is.
* **`[byte]` prints as `[104, 195, 169]`**, indistinguishable from `[int]`. Minor, but a `b` suffix or a hex form
  would make a byte dump readable. [O]
* **`substring`/`slice` are O(1) views that pin the whole parent buffer.** Documented. With a conservative
  collector, a 30-byte substring of a 1 MiB request body keeps the megabyte alive for as long as the substring
  lives — worth one explicit sentence in `SYNTAX.md`'s `substring` entry rather than only the existing "it keeps
  the whole parent buffer alive, which matters once memory is reclaimed". [O]

---

## 5. Standard library design

### 5.1 Shape and layering [O]

The layering is right: `prelude.zm` merges bare (the types and string/array builtins every program needs),
`lib/core` holds self-contained modules, `lib/db` and `lib/http` the two stacks, `lib/time` the date/time types,
with `docs/lib/dependencies.md` recording the graph. Writing the library *in Ziman* over a thin native floor is the
single best decision in the project: it dogfoods the language continuously, and it is why the language's gaps are
visible in the library's shape rather than hidden behind C.

Individual module quality is high. `migrate.zm` is Flyway-shaped with checksums and advisory locks and a
well-argued no-down-migrations stance; `regex` is linear-time by construction; `jwt` requires `exp` and refuses
`alg: none`; `client` has a real `CookieJar` with domain/path/Secure/Max-Age handling; `collections` is a coherent
30-function set with consistent argument order (collection first, function last) and consistent bounds.

### 5.2 D1 — The error-type zoo is the library's biggest consistency problem. [V]

Counted over every `-> Result<_, E>` in `lib/`:

| error type | occurrences |
|---|---|
| `string` | 242 |
| `DbError` | 100 |
| `ClientError` | 25 |
| `NetError` | 16 |
| `HttpError` | 10 |
| `JwtError` | 7 |

`string` is the dominant error type, and `fs` — the module a backend touches on every config load, resource
resolution and upload — is entirely string-errored:

```
Err(cannot read '/nonexistent/x': No such file or directory)
Err(cannot read '/etc/shadow': Permission denied)
Err(cannot read '/etc': Is a directory)
```

The three cases a caller actually wants to distinguish (missing → 404 or create-it, denied → 500 and alert,
is-a-directory → 400) are available only by substring-matching `strerror` text, which is locale-dependent and
not part of any contract. The same applies to `json`, `regex`, `properties` and `mime`.

The `error_message(e)` builtin solved the *rendering* half of this nicely (one convention,
`<snake_type>_message`, dispatched by the compiler). The *matching* half is still open. The smallest useful step is
not a universal error type: it is a structured enum for the two or three modules where a caller branches on the
cause — `FsError { NotFound(string) Denied(string) IsDirectory(string) NotDirectory(string) Other(string) }`
with a `fs_error_message`, exactly the shape `NetError` already has. `json`'s errors are genuinely "a message for a
human" and can stay strings. **P1, M.**

### 5.3 D2 — Duplication and asymmetry [V]

* **`percent_decode` twice, differently.** `lib/db/db.zm:180` is `(s: string) -> Result<string, DbError>`;
  `lib/http/http.zm:188` is `(s: string, plus_is_space: bool) -> Result<string, string>`. Same algorithm, different
  arity, different error type. One should call the other. **P2, S.**
* **Cookies exist on the client and not on the server.** `client` has a full `CookieJar`; `server.zm` contains the
  string "cookie" zero times. There is no `Set-Cookie` helper and no `Cookie:` header parser. The reference backend
  does not notice because it uses bearer tokens, but a browser-facing backend needs both on day one. Together with
  no CORS (tracked in `ROADMAP.md:1762`, not started), no multipart (documented as unsupported,
  `lib/http/server.zm:17`) and no metrics module (designed in `ROADMAP.md:1741`, not started), that is four items a
  first real browser-facing deployment will hit in its first week. **P1, M each.**
* **`derive(Json)` decode stops at the first bad field.** `user_from_json` on `{"id": 1}` returns
  `Err(missing field 'name')` and says nothing about the other four. [V] That is why
  `health-record-backend-ziman/src/main/ziman/service/service.zm` has its own `FieldError`/`accumulate`/
  `validation_result` machinery: the derived decoder cannot produce the per-field error object a 400 response needs.
  An `#[derive(Json)]` variant that accumulates (`Result<T, [string]>`, or a `json_decode_all` companion) would move
  the most repeated piece of the reference backend into the library. **P2, M.**

### 5.4 What is over-engineered, and what I would not touch [O]

`server.zm`'s nine handler constructors (§3.2, L8) are the one place I would call the library
over-engineered — though the author chose the least-bad option available (one `endpoint_core`, nine thin adapters,
documented), and the right fix is the `Row` bound plus better abstraction in the language, not fewer constructors.
Three `open_session*` variants exist, but one is already `#[deprecated]`, so that is a migration in progress rather
than a wart.

What I would *not* trim: the breadth. `toml`, `yaml`, `csv`, `ndjson`, `compress`, `otp`, `bits`, `zone` look like
scope creep from outside, but `#![experimental]` plus `make experimental-check` is a real, enforced answer to
"which of these do you promise to keep green", and it is a better answer than most languages give. The risk is
maintenance load on one person, not design incoherence.

---

## 6. Implementation quality, tooling and documentation

### 6.1 Diagnostics: the best thing in the project [V]

Four messages I would hold up as models for other language implementers:

```
for i in xs { print(xs[i]) }
  -> 'i' is an element of 'xs' here ('for i in xs' binds each element, not its position), so 'xs[i]'
     indexes with a value; to loop over positions write 'for i in 0..len(xs)', or use 'for i, item in xs' for both

val f = fn() -> int { n }        // n is a var
  -> a lambda cannot capture 'n' because it is a 'var' -- copy it into a 'val' first and use that
     (captures are by value, so the lambda and the enclosing function never share mutable state)

switch e { A {...} B {...} }     // enum E { A B C }
  -> this 'switch' over E is not exhaustive: missing C (add an arm for each variant; there is no default
     arm on purpose, so a new variant is a compile error here)

ok(f()? + 1)
  -> there is no postfix '?': write 'try <expression>' in front of the Option or Result instead
```

Each names the mistake, the rule behind it, and the fix. Multi-error reporting works (three independent type errors
in one file, each with a source excerpt and a caret, then `ziman: 3 errors`), errors are sorted and never cascade,
and `--json` exists for machine consumption. This is the project's clearest competitive advantage and it should be
treated as a first-class feature, not a nicety.

**Four weak messages**, in priority order: `o.field(args)` sending you to `run(o, ...)` (§3.2, L2); `try` in a
non-`Result` lambda listing five irrelevant reasons (§3.2, L5); a nested `fn` producing `expected '(', found
identifier` (§3.2, L7); and the two cases where the docs state a deliberate rule the message does not teach —
a trailing comma (`expected a field name, found '}'`, with no hint that the newline is tolerated and the comma is
not) and `Option<Option<T>>` (the generic "cannot initialize" message, with no mention of the documented ambiguity
or the `Option<Option<T>>.Some(...)` fix). **All five: P1-P2, S.**

### 6.2 Tooling [V]

* **`--locals` is excellent** and I have not seen its equal in a language this small. Each frame's variables with
  their values, under the frame, in the trace. For a tree-walking interpreter this is nearly free and it is the
  single best debugging affordance here.
* **`--profile` is honest.** It explains why its sample total does not match CPU time (kernel timer-signal merging
  with the collector's marker threads) and tells you to compare percentages. It also immediately surfaced the
  81.2%-in-GC result of §4.7.
* **`--fmt` is idempotent and token-verified** (the output is rescanned and must carry the same code tokens and
  comments, or nothing is written). It deliberately never moves a line break, which is documented at
  `DESIGN.md:3247`. The cost is that it does not end style arguments the way gofmt does: a half-split struct stays
  half-split (`struct Point { x: int` / `    y: int }`), so two correctly-formatted files can disagree about
  everything that matters to a reader. [O] I would not change the engineering choice — a text formatter that works
  on files the compiler rejects is worth a lot — but I would add a `--fmt --canonical` mode that normalises brace
  placement for the subset of constructs the lexer can identify unambiguously.
* **`--lint` is very narrow**: two rules (an unreassigned `var`, and `x = concat(x, ..)` in a loop). The second one
  fired correctly in my test and is genuinely valuable. An unused `val` is not flagged. Candidates with the same
  value-per-line as the `concat` rule: unused `val`/parameter, `set(arr, i, ..)` in a loop (the other documented
  quadratic), and `unwrap(...)` outside a test.
* **`--coverage`** works and reports per-file percentages. Test conventions (`#[test]`, `ziman test`, the `test`
  module, 845 fixtures with `.out`/`.err` expectations, the `*_example` fixtures that execute the library manual's
  own examples) are a real harness, not a shell script.
* **LSP**: `make test-lsp` reports 0 failures across diagnostics, formatting, symbols, go-to-definition, hover and
  completion, including pattern-bound names and switch-arm binders.

### 6.3 Architecture [V]

27,854 lines of C across 44 files, with `typecheck.c` at 4,339 the largest and `parser.c` at 2,428 next — a sane
distribution for a checker-heavy design. The library's Ziman source is embedded as `*_source.h` headers so the
binary is self-contained. Value representation is documented where it is relied upon (`src/value.h`), which is how
the "semantics say copy, implementation shares pointers" invariant stays safe. Slot-resolved variable lookup and a
per-call `Env` pool replaced the earlier `strcmp`-down-a-list scheme, and it shows: a 3-million-iteration arithmetic
loop runs in 0.14 s (~21 M ops/s).

Per-call overhead is the remaining hot spot: `fib(30)` (1.66 M calls) takes 0.28 s, about 170 ns per call. For
comparison on the same machine, CPython 3.14 does it in 0.052 s and Node in 0.05 s. [V] So Ziman's arithmetic is
fast and its calls are ~5× slower than CPython's. For a tree-walker with a static checker that is respectable, but
it means the positioning ("a typed language for backends") has to rest on types, diagnostics and the library, not on
speed.

### 6.4 Documentation [V]

`SYNTAX.md` (2,372 lines) is a genuinely good language reference — it explains *why* at every restriction, and the
"Semantic rules enforced before a program runs" and "Other limits" sections are the kind of thing most languages
never write down. `docs/lib/` is generated from source and verified by the `*_example` fixtures, so it cannot drift.
`DESIGN.md` as a decision log with rejected alternatives is the single most transferable practice in this
repository.

Four doc/behaviour mismatches found by experiment:

1. **`SYNTAX.md:371-374` is a complete code example that does not compile.** It is the primary demonstration of
   `Iter<T>` and `fs.lines`, and it uses postfix `?` twice: `val lines = fs.lines("app.log")?` and
   `val text = line?`. Run verbatim: `syntax error: there is no postfix '?'`. Replacing both with `try` makes it
   print `Ok(1)`. **P1, S.**
2. **`SYNTAX.md:1022`** claims an `if`/`switch` arm's value "may be `foo()?`". It may not. (`:2074`'s negative
   mention of `f()?` is harmless but also stale.)
3. **`SYNTAX.md:165`** describes `float` as "double-precision IEEE 754" without noting that `/ 0.0` traps (§4.8).
4. **`SYNTAX.md`'s Generics section** says only "an unannotated lambda" contributes nothing to type-argument
   inference; a fully annotated one also contributes nothing (§3.2, L3). Either the doc or the code should move —
   I recommend the code.

One structural comment [O]: `DESIGN.md` is 5,046 lines of chronological log, which makes it an outstanding decision
record and a poor reference. Its formatter entry (`:3247`) lists stdin as "not done", superseded 365 lines later at
`:3612`; a reader who stops at the first hit gets the wrong answer. A short, dated "current state" header on each
superseded entry, or a generated index that marks entries as superseded, would make the 5,000 lines usable without
losing the history.

---

## 7. Comparison and positioning

**Against Go**, which it most resembles in temperament (value semantics, explicitness, a small closed feature set,
`defer`, no inheritance): Ziman is better on exhaustive `switch`, block-scoped `defer`, must-use `Result`, named
arguments, no nil, and diagnostics. It is behind on everything runtime: OS threads instead of a scheduler, a
stop-the-world collector that anti-scales, no race detector, no profiler beyond sampling, no modules/versioning. The
language comparison favours Ziman; the implementation comparison does not, and for a backend the implementation is
what you deploy.

**Against Kotlin/Java**, which the library deliberately echoes (`time` is java.time-shaped, `derive(Row)` is
JPA-lite, `migrate` is Flyway-shaped, `decimal` is `BigDecimal`'s shape with `1.0d == 1.00d` fixed): Ziman is a
much smaller surface with a much better error-handling story and no JIT. The honest pitch here is "the 5% of Spring
Boot you actually used, in 36 modules you can read in a weekend" — and the reference backend demonstrates that
pitch convincingly.

**Against Rust**, from which it takes `Result`/`Option`, derives, exhaustiveness and `#[from]` without lifetimes or
borrowing: the divergence that matters is traits. Ziman's closed `Eq`/`Hash`/`Ord`(+`Row`) bound list is the
principled minimum, and §3.2's L8 is what it costs. I think the stance is right for now and will be relitigated the
first time someone writes a second `Crud`-shaped abstraction.

**Against Elixir**, whose actor pitch Ziman explicitly adopts: Ziman has `spawn` and mailboxes and does not have
links, monitors or supervisors. §4.6 is what that absence looks like in practice. Supervision is not a nice-to-have
on top of actors; it is the half that makes the model survive contact with production.

**Against Python**, which is what a small team actually chooses instead: Ziman gives static types, exhaustiveness,
must-use errors and a single self-contained binary, at ~5× CPython's call cost and with a collector that does not
yet scale. The static-typing argument is strong; the performance argument is not available and should not be made.

**Best ideas worth keeping and worth copying elsewhere:** enforced identifier casing buying grammar simplicity;
`format`'s literal-template compile-time check; block-scoped `defer`; must-use `Result` with a real `_`; `#[from]`
as a closed alternative to a `From` trait; derive output as readable source with positions mapped back;
`#[thread_bound]` as a type-level "this handle stays here"; `--locals`; `DESIGN.md` as a decision log; the generated
library manual verified by executable examples; and the teaching style of the diagnostics.

**Biggest risks, ordered:** (1) the collector under concurrency — it invalidates the North Star and nothing else
compensates; (2) no supervision, so a worker's death is a silent hang; (3) breadth-to-depth ratio — 36 modules and a
language under one maintainer, where every core change must keep all of it green (`#![experimental]` is the right
mitigation and should be used more aggressively); (4) flat module names, which make every layered program invent the
same alias convention; (5) the no-traits ceiling, which is fine today and will not be fine at 50,000 lines.

**If I owned it, the first thing I would do** is freeze the library surface entirely for one cycle and spend it on
§8's P0 list — the collector design decision, the two correctness defects, and the trace blowup — because every new
module makes the collector decision more expensive and none of them makes it easier.

---

## 8. Prioritised recommendations

### P0 — correctness and the North Star

| # | Recommendation | Rationale | Effort |
|---|---|---|---|
| P0-1 | **Decide the collector question** (`tasks.md` F7, `MEMORY.md`): one heap per thread, or something else, but decide before more surface lands. Interim cheap win: cut allocation *frequency* in the two hottest allocators (`format`'s output buffer, small `StructObj`) rather than migrating more objects into `gc_heap`, which F7 shows measures slower. | §4.7: 8 threads doing 8× the work take 10.6× longer per thread while pure compute scales 4.7×; `--profile` shows 81.2% in GC. This is the one defect that invalidates the stated goal. | **L** |
| P0-2 | **Stop invalid UTF-8 at the JSON and HTTP output boundaries** — validate or substitute U+FFFD in the JSON string encoder. | §4.3: a `substring` that splits a code point yields JSON that Python rejects and Ziman's own parser accepts, so it passes every in-language test and fails at the client. | **S-M** |
| P0-3 | **Make `NaN` behave in `Map`/`Set`** — canonicalise it in `hash` and in the equality collections use, so `compare` and key lookup agree. Document that `==` and `compare` deliberately disagree on `NaN`. | §4.2: a `Set<float>` holds two copies of a value it says it does not contain; `Map<float, _>` grows without bound on repeated `set` of the same key and `map_get` cannot find it. | **S** |

### P1 — ergonomics, consistency and operability

| # | Recommendation | Rationale | Effort |
|---|---|---|---|
| P1-1 | **Bound and de-quadratic `build_trace`** (`src/error.c:289`) and **call `GC_set_warn_proc`** (next to `src/main.c:288`). | §4.4: a stack-depth error costs 32.5 s, 1.26 GB and 97,390 stderr lines — an input-triggered DoS in a server. | **S** |
| P1-2 | **Let a fully annotated lambda contribute to type-argument inference** (`src/typecheck.c:1251`: return 0 when every parameter is typed and `->` is written). | §3.2 L3: `collections.map` needs its type arguments spelled out while `filter` does not; the docs already promise the fix. Best ergonomic return per line of C in this report. | **S** |
| P1-3 | **Fix the `o.field(args)` diagnostic**, then **allow the call**: `a.f(args)` means "call the function value in field `f`" when `a` is a local of a struct type with such a field. | §3.2 L2: the current message sends you to `run(o, ...)`, which does not exist; the workaround appears 11× in the backend (all in `crud.zm`, with a 4-line comment explaining it) and 4× in `lib/http/server.zm`. | **S** then **M** |
| P1-4 | **Accept bare `Some(x)`/`None` in a call argument.** | §3.2 L4: `f(4)` works and `f(Some(4))` does not, so the explicit spelling is the rejected one. | **S** |
| P1-5 | **Fix the four weak diagnostics** (§6.1): `try` in a non-`Result` lambda, nested `fn`, trailing comma, `Option<Option<T>>`. | Diagnostics are the project's competitive advantage; these four are the places where the compiler knows the answer and gives a generic message. | **S** |
| P1-6 | **Fix `SYNTAX.md:371-374`** (a primary example that does not compile), `:1022` and `:165`. Consider a mechanical check that extracts fenced `zm` blocks from `SYNTAX.md` and at least `--check`s them, the way `docs/lib/` examples are already fixtures. | §6.4: the one non-compiling example is the main demonstration of `Iter`/`fs.lines`, and the generated lib docs show the project already knows how to prevent this class of drift. | **S** (fixes) / **M** (check) |
| P1-7 | **Remove the silent hang when a worker dies**: close a channel whose only remaining peer has ended, so a blocked `receive` fails with a diagnostic instead of hanging; document the supervision pattern that `server.serve`'s `on_error` already demonstrates. | §4.6: a dead spawned thread leaves `main` blocked forever with no output. The actor pitch needs its third leg. | **M** |
| P1-8 | **Give `fs` a structured error enum** (`NotFound`/`Denied`/`IsDirectory`/`NotDirectory`/`Other` with `fs_error_message`), matching `NetError`'s shape. Leave `json`/`regex` on strings. | §5.2: 242 `Result<_, string>` means the three cases a backend branches on are reachable only by substring-matching `strerror`. | **M** |
| P1-9 | **Server-side cookies**: a `Cookie:` parser and a `Set-Cookie` builder, plus CORS as a `dispatch` wrapper. | §5.3: the client has a full `CookieJar` and the server has nothing; both are week-one needs for anything browser-facing. | **M** each |
| P1-10 | **Multi-segment `use` references** (`model.patient.X`) or last-two-segment defaulting on collision. | §3.2 L1: 1,261 `use` lines (9% of the reference backend), 351 forced aliases, and an invented `_model` suffix convention. | **M** |

### P2 — worth doing, not urgent

| # | Recommendation | Rationale | Effort |
|---|---|---|---|
| P2-1 | **Land the `Row` bound** already decided in `DESIGN.md`'s traits entry. | Deletes two function-value fields from every `Crud` literal and the copy-paste hazard `crud.zm`'s own comment names. | **M** |
| P2-2 | **Replace `type_holds_thread_bound`'s `depth > 12` cutoff** (`src/typecheck.c:56`) with visited-set cycle detection; make genuine unbounded recursion an error, not a silent `0`. | §4.5: a `StringBuilder` 12 structs deep passes `spawn` statically. | **S** |
| P2-3 | **Check thread-bound captures at lambda construction.** | §4.6: turns the common lambda-capture case from a runtime hang into a compile error; the capture set is already known where the lambda is checked. | **M** |
| P2-4 | **An accumulating JSON decoder** (`Result<T, [string]>` or a `json_decode_all` companion). | §5.3: the reference backend hand-rolls `FieldError`/`accumulate`/`validation_result` because the derived decoder stops at the first bad field. | **M** |
| P2-5 | **Broaden `--lint`**: unused `val`/parameter, `set(arr, i, ..)` in a loop, `unwrap` outside a test. | §6.2: two rules today; the `concat`-in-a-loop rule shows the shape works. | **S** |
| P2-6 | **De-duplicate `percent_decode`**; make `db`'s call `http`'s. | §5.3: two functions, two arities, two error types, one algorithm. | **S** |
| P2-7 | **A `metrics` module** per the design already in `ROADMAP.md:1741`, with `server` recording its own. | Nothing in §4.7 is diagnosable in production without it. | **M** |
| P2-8 | **Mark superseded `DESIGN.md` entries** (a dated "superseded by" line, or a generated index). | §6.4: the formatter entry at `:3247` contradicts `:3612` and a reader who stops at the first hit gets the wrong answer. | **S** |
| P2-9 | **`--fmt --canonical`** that normalises brace placement where the lexer can decide unambiguously. | §6.2: the formatter is idempotent and safe but does not settle style, which is most of why gofmt is loved. | **M** |

---

## Appendix: experiments

All scratch files under `/tmp/ziman_review_lang/`. `Z=./ziman` at 3c0908f. Nothing in either repository was modified
except this file. Timings on a 16-core Linux 7.2.7 box; every surprising result was re-run at least once.

**A1 — Baseline.** `make test` → `845 passed, 0 failed`, exit 0. `make test-lsp` → `0 failure(s)`.
*Why it matters:* matches the stated expected baseline, so every later result is attributable to the program under
test rather than a dirty tree.

**A2 — `NaN` in collections** (`n3.zm`, `n4.zm`). Input as quoted in §4.2. Output: `compare(nan, nan)` → `0`,
`nan == nan` → `false`, `len(Set)` after two `insert(nan)` → `2`, `contains(s, nan)` → `false`,
`len(Map)` after two `set(m, nan, _)` → `2`, `map_get(m, nan)` → `None`, `print(m)` → `{nan: 1, nan: 2}`.
Also `collections.sort<float>([nan, 1.0, nan, 2.0])` → `[1.0, 2.0, nan, nan]` (correct per the documented total
order), `contains([nan], nan)` → `false`, `hash(nan)` → `9221120237041090560`.
*Why it matters:* a `Set`/`Map` whose `len`, `contains` and `get` disagree with each other for a value the type
system accepts as a key.

**A3 — Invalid UTF-8 through JSON** (`u5.zm`, `u6.zm`). Input as quoted in §4.3.
Output: `json_to_bytes` → `[123, 34, 116, 101, 120, 116, 34, 58, 34, 104, 195, 34, 125]`;
`xxd` → `7b22 7465 7874 223a 2268 c322 7d`; Python `json.loads` → `UnicodeDecodeError ... byte 0xc3`;
Ziman `is_ok(parse_json(s))` → `true`.
*Why it matters:* the failure is invisible from inside Ziman and visible to every client.

**A4 — Stack-depth error cost** (`e2_deeprec.zm`, `depth(100000)`).
Output: correct error message first, then `97390` lines of stderr containing `85631` trace frames and `5906` Boehm
warnings; `/usr/bin/time` → `32.54 s, 1262044 KB max rss`.
*Why it matters:* quantifies `src/error.c:289`'s quadratic trace builder and the missing `GC_set_warn_proc`, and
shows the guard that was added to prevent a crash now has its own denial-of-service cost.

**A5 — Thread-bound depth cutoff** (`sd_10.zm` … `sd_14.zm`, generated).
Output: `--check` rejects at nesting depth 10 and 11, **accepts** at 12, 13 and 14; running the depth-14 case gives
the runtime owner error in the worker thread, then `None` from `receive_timeout`.
*Why it matters:* locates `src/typecheck.c:56` exactly and shows the static guarantee silently degrading.

**A6 — Worker death hangs its peer** (`h3_lambda_sb.zm`, `h7.zm`). `h3` as quoted in §4.6: `--check` exit 0, run
killed at 120 s with no output. `h7` (same program with prints and `receive_timeout`): `main: before send` /
`worker: before receive` / `main: after send` / `worker: after receive` / owner error / `the thread started by
'spawn worker' at 10:5 ended; the program goes on` / `None`.
*Why it matters:* separates the two issues — the owner check works; the absence of supervision is what turns it
into a silent hang. Also confirms the thread-bound check *is* transitive through struct fields (`h1`, `h2`) and
array/channel element types (`h6`), so the lambda path is the only hole.

**A7 — Thread scaling** (`c2_alloc.zm` allocation-heavy, `c3_cpu.zm` pure compute). Full table in §4.7.
Allocation: 1→8 threads is 0.28 s → 2.97 s for 8× the work (re-runs 2.68 s, 2.76 s at 8; 0.28-0.29 s at 1);
compute: 1.48 s → 2.53 s, a 4.7× speedup at 646% CPU. `--profile p3_map.zm` → `81.2% (garbage collection)`,
`18.8% main`.
*Why it matters:* isolates the collector from the interpreter and shows total throughput *falling* as cores are
added, which is a sharper statement of F7 than the `req/s` curve.

**A8 — Inference cliff** (`l2_inf.zm`, `l4.zm`, `l5.zm`, `l6.zm`, `l3_inf.zm`).
`collections.map(xs, fn(x: int) -> int { x * 2 })` → cannot infer `U`; `apply2(3, fn(n: int) -> string { str(n) })`
→ cannot infer `U`; `apply2(3, tostr)` → `3`; `pick(1, 2)` → `1`; `collections.filter(xs, lambda)` → `[2, 3]`.
*Why it matters:* shows the cliff is specifically "lambda", not "function-typed parameter", and contradicts the
documented rule.

**A9 — Bare `Some`/`None` reach** (`z2_bare_val_arg.zm`, `z2_bare_some_arg.zm`).
`f(4)` → works; `f(Option<int>.Some(4))` → works; `f(Some(4))` → type error; `f(None)` → type error.
*Why it matters:* the explicit spelling is the rejected one, and the docs do not list the exclusion.

**A10 — `try` diagnostics** (`y_*.zm`, `z_*.zm`, `z3.zm`, `z4.zm`, `z5.zm`).
Correctly refused with the positional message: `while` condition, right of `&&`, `else if` condition. Correctly
accepted: `ok(try parse_int(s) + 1)`, `g(try a(), try b())`, `try` inside a `-> Result<...>` lambda (both as a
`return` and as a bare final expression), `try` on an `Option` in an `Option`-returning lambda.
Wrongly diagnosed: `fn(s: string) -> int { try parse_int(s) }` gets the positional message, while the equivalent
top-level function gets the correct "requires the enclosing function to return Result<..., string>, found int".
Also found: `fn f(...) -> Result<...> { ok(...) }` (bare final expression in a *named* function) is a type error
while the same body in a lambda works.
*Why it matters:* locates the wrong-message path at `src/typecheck.c:2783` and documents the lambda/function
asymmetry.

**A11 — Re-verifying the prior reviews' numeric findings** (`v_*.zm`). `print(99999999999999999999999)` → syntax
error naming the `n` suffix; `print(-9223372036854775808)` → `-9223372036854775808`;
`(1e12 * 1e15) as int` → `9223372036854775807`, `as bigint` → `1000000000000000013287555072`;
`7.5 % 2.0` → type error; `print(98765.4321)` → `98765.4321`, `str(2.5 * 4.0)` → `10.0`,
`print(1234567.891)` → `1234567.891`; `parse_int(" 5")` and `parse_int("+5")` → `Err`.
*Why it matters:* all of S4-S7 and the `parse_int` leniency are genuinely fixed, not just ticked.

**A12 — Re-verifying the prior reviews' ergonomics findings** (`v_try_*.zm`, `v_bare_none.zm`, `v_unwrap_or.zm`,
`v_arr_struct_eq.zm`, `v_json_eq.zm`, `v_local_fn.zm`, `fv1.zm`, `fv2.zm`).
Fixed: `try` nesting/hoisting, bare `None`/`Some` in a return, `option_unwrap_or(o, 0)` without type arguments,
`==` and `contains` on arrays of structs and enums, `Json == Json`. Still failing: a nested `fn` (poor message), and
`o.run(21)` on a function-typed field (misleading message); `val run = o.run` then `run(21)` → `42`.
*Why it matters:* separates what is closed from what the tasks list ticked without closing.

**A13 — Doc examples** (`d1_doc_iter.zm`, `d1b_fixed.zm`). `SYNTAX.md:371-374` verbatim → `syntax error: there is
no postfix '?'`. The same program with `try` in both places → `Ok(1)`.
*Why it matters:* a non-compiling example in the language reference's primary `Iter` demonstration.

**A14 — Performance shape** (`p1`-`p7`, `p6_set_big.zm`, `fib.zm`/`fib.py`/`fib.js`).
300,000 `push` → 0.06 s (amortised O(1) confirmed); `set` in a loop → 0.23 s at 20k, 3.99 s at 100k (quadratic,
documented); 200,000 `Map` inserts → 0.96 s, 219 MB; 3 M arithmetic iterations → 0.14 s; 4 M garbage `Map` entries
churned → RSS flat at 41 MB (the collector reclaims). `fib(30)`: ziman 0.28 s, CPython 3.14 0.052 s, Node 0.05 s.
*Why it matters:* establishes that arithmetic is fast, calls cost ~170 ns, the GC does reclaim, and the two
documented quadratics behave as documented.

**A15 — Soundness probes** (`a_*.zm`, `b_*.zm`, `s2_receq.zm`, `s3_receq2.zm`, `w_*.zm`, `u_*.zm`).
All correctly rejected or correctly handled: generic substitution (`unbox<string>(Box<int>)`), `Box<A>` → `Box<B>`
field assignment, auto-wrap when `T == E`, `Option<Option<T>>`, derive over a function-typed field, `==` on a struct
reachable to a `Channel` (both field orders), `[Channel<int>] == [Channel<int>]`, `str` on a string, `min` of an
empty array (runtime), a trailing comma, `#[from]` on two variants with the same payload type. Correct behaviour
confirmed: `#[from]` one-step and chained, `for` over a snapshot, `Map<int, V>` to JSON as `{"1": "a"}`,
multi-line comma lists, `unit` as a map key, `-7 / 2 == -3` and `7 % -2 == 1`, `to_upper("straße")` → `STRAßE`
(ASCII-only, documented), `chars` on a half-code-point → runtime error naming the byte offset,
`bytes_to_string([104b, 195b])` → `Err`.
*Why it matters:* I found no type-soundness hole. The two correctness defects (A2, A3) are both in the
*library/runtime* contract, not the type system.

**Not run, and recorded as such:** any comparison between the `ziman` and `ziman-nogc` builds (the operator asked
that `make gc`/`make nogc`/`make clean` not be run, since the binary is shared with a concurrent review), the
reference backend's own test suites, and anything binding a port. §4.7's scaling result is therefore from a
microbenchmark on the default collector build only; `tasks.md`'s F7 entry has the back-to-back two-build numbers.
