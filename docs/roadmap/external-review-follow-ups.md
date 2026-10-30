# External review follow-ups (2026-09-23)

Part of the [roadmap](README.md) -- see its preamble for what an entry's status means and how it goes stale.


An outside compiler/PL-expert review (first exposure to the language; it
read the docs, checked them against the source, ran ~25 probes and
load-tested the sample backend) produced the items below. Full report with
reproductions: docs/reviews/pl-expert-review-2026-09-23.md. Its verdict in
one line: the design is coherent and unusually well documented, but
measured against the "AI-first" goal Ziman fixed the cheap, visible error
classes (implicit coercion, overloading) and left the ones that actually
dominate machine-written bugs, and the runtime cannot yet host a
long-running process. Each item is meant to be picked up on its own, in
roughly this order (cheapest and highest-value first); tick them off here
as they land.

**A. Silent-wrong-answer holes (fix first -- small, and they contradict the
language's own principles)**

- [x] **R1. `Result<T, E>` auto-wrap turns errors into successes when
  `T == E`.** *Done 2026-09-23.* `return "path must not be empty"` from a
  `Result<string, string>` function compiles to `Ok(...)` -- no
  diagnostic -- and that is the stdlib's most common Result shape
  (`io.read_file` included). Fix: refuse the bare-value auto-wrap when
  `T == E` and require `ok(...)`/`err(...)`; audit the stdlib for existing
  victims. Also worth a diagnostic: `Option<Option<T>>` quietly wraps a
  `None` into `Some(None)`. Also correct docs/syntax/'s claim that this
  mirrors Rust's `From` (Rust has no such implicit conversion; this is
  closer to a C++ converting constructor).
- [x] **R2. Integer overflow is silent, and is undefined behaviour in C.** *Done 2026-09-23: traps with a runtime error; `-fwrapv` and `-O2` added.*
  `9223372036854775807 + 1` wraps quietly, while `bigint as int` and
  `int as byte` saturate "so nothing silently wraps" -- the language
  contradicts itself. Immediate floor: add `-fwrapv` to the Makefile (and
  `-O2`, which is currently absent and worth 2-3x). Then decide the
  language rule (trap, saturate, or documented wrap) and apply it to `+`,
  `-`, `*`.
- [x] **R3. `for x in arr` vs `for i in arr` is one syntax with two
  meanings.** *Done 2026-09-23 as a narrow compile error (`for i in a { a[i] }`), keeping one keyword; the `in`/`of` split was considered and deferred.* Iterating an array binds the element; iterating a range binds
  the index; the only guard is a naming convention in docs/syntax/. A model
  that writes Python's/Go's index loop gets silently wrong sums, not an
  error. Fix: a distinct form for index iteration (or a compile error when
  the binder of an array loop is used to index the same array).
- [x] **R4. No exhaustiveness checking for enums.** *Done 2026-09-23 as `switch` (one arm per variant, no default arm; enums and `Result`); full `match` stays deferred.* Adding a variant
  compiles cleanly everywhere and crashes at runtime in the consumers
  (the sample backend's `service_error_response` has exactly this
  shape). Full `match` stays deferred, but a `switch`-style statement over
  an enum that requires one arm per variant (desugaring to the existing
  `is_`/`unwrap_`) closes the gap with machinery that already exists. This
  is the highest-value static check for machine-generated code.

**B. Diagnostics (the error message is the interface an agent iterates
against)**

- [x] **R5. Special-case the mistakes models make most.** *Done 2026-09-23 for method calls, string/array `+` and `match`; the derive-error case is R6.* `p.to_string()`
  says "no module imported under the name 'p'" -- it should say Ziman has
  no methods and name the function; `"a" + "b"` says the operands must be
  "the same numeric type" and suggests a cast that cannot exist -- it
  should say `concat(a, b)`; `match` says "expected an expression" -- it
  should say there is no `match` and point at `Enum.is_X`/`unwrap_X`. A few
  lines each.
- [x] **R6. `#[derive(...)]` errors point into an invisible file.** *Done 2026-09-23: positions map to the declaration and the message names the derive; no per-field pinpointing.*
  Generated code has no source mapping, so any error reports `1:5` of
  nothing. Stamp every spliced token with the originating declaration's
  line/column and prefix the message with `in derive(Json) for 'Point':`.
- [x] **R7. Auto-import what a derive itself generated.** *Done 2026-09-23 (Json, Row, Properties); field types' own conversions stay explicit but their missing-function error now says what to do.* One
  `#[derive(Json)]` currently needs five hand-written `use json.*` lines
  the generator already knows it needs, and forgetting one produces the
  unhelpful error above. Inject them, or at least say which `use` is
  missing.
- [x] **R8. `--check` and `--json` diagnostics.** *Done 2026-09-23: `--check`, `--json`, `--emit-derived`, `--version`, `--help`.* The CLI takes exactly one
  argument. A type-check-only mode is a few lines (typecheck is already a
  separate phase); machine-readable diagnostics are docs/design/journal/ principle #5
  and are still unbuilt, even though they are the item most aligned with
  the AI-first goal. (`--emit-derived` to print generated code would help
  R6 too.)

**C. Runtime and memory**

- [x] **R9. Rewrite the memory-reclamation plan before implementing
  anything.** *Superseded 2026-09-23: neither plan was built. A tracing
  collector (Boehm) cut the load test from ~31 GB to ~0.47 GB, and on
  2026-09-24 became the default build (vendored, `make nogc` for the old one),
  with pause times measured; sockets/SQLite handles are still closed by hand.
  See docs/design/journal/'s "Memory reclamation" entries.* Measured: the sample backend's server reached ~31 GB RSS
  after ~1,500 requests (about 87 kB leaked per trivial request), so it is
  not merely "a real problem" -- the north-star application cannot stay up.
  And both proposed fixes have a design error: (a) the refcounting entry
  claims the value graph can never contain a cycle, but `Channel<T>` is
  mutable in place, so a struct holding a channel of its own type, sent into
  that channel, is a real cycle (reproduced) -- channels need weak edges or
  to be excluded from counting; (b) the per-thread arena entry does not fit
  a thread-per-*connection* server, where one thread serves thousands of
  keep-alive requests and would free nothing until disconnect -- the region
  has to be per request, which needs an escape rule for the buffered
  session and for anything sent over a channel. Also note the leak is
  currently what makes cross-thread `Value` sharing lock-free, so this is
  one project with the ownership design, not two. See the corrections
  added inline to the two entries below.
- [x] **R10. Server hygiene in the sample backend.** *Done 2026-09-23: `os.exit`/`write_stderr`/shutdown handler, `server.max-connections`, graceful drain; bounded channels landed 2026-09-24: `channel_new(capacity)`, blocking `send`, `try_send`/`try_receive`/`channel_len`.* Cap concurrency
  (`server.zm` spawns an unbounded thread per connection -- a DoS
  primitive on its own), and fix the config path that prints a friendly
  error and then crashes on `unwrap` anyway. Add a native `exit(code)`;
  there is also no signal handling, so no graceful shutdown.
  Backpressure is absent too: `send` never blocks and queues are
  unbounded.
- [x] **R23. A packed `[byte]` representation.** *Done 2026-09-24. A `[byte]`
  is now one real byte per element (`ArrayObj.bytes`) instead of a 16-byte
  `Value`; `tests/bench/bytes_bench.zm` (a 1 MB buffer: build, sum, sha256,
  concat, slice, compare, convert) went from 297 MB / 0.33 s to 66 MB / 0.23 s.
  Views (`slice`) work on both representations. See docs/design/journal/ "Packed byte
  arrays".* (Original problem: every element of a `[byte]` was a full
  interpreter `Value`, so a 1 KB buffer cost ~16 KB and every `net.read`,
  `bytes_concat` and `crypto` call copied and boxed accordingly.)

**D. Language expressiveness**

- [x] **R11. `Ord` bound and a generic `sort`.** *Done 2026-09-23: `Ord` bound, `compare`, `<` on strings, `#[derive(Ord)]`, `collections.sort`/`sort_by_keys`/`binary_search`.* Sorting is not just
  missing, it is inexpressible: no comparator (no function values) and no
  "comparable" bound. The nine hand-written `_compare` functions across
  `byte`/`uuid`/`time` are the symptom. `Eq` and `Hash` already prove the
  bound mechanism.
- [x] **R12. Widen `Hash`.** *Done 2026-09-23: structural and automatic (any `==`-comparable type with hashable leaves); `Uuid`, enums, structs, `Option`, `Result` are usable keys.* Map keys are limited to the seven scalars, so
  nothing can be indexed by `Uuid`, an enum, or a struct -- a real hole for
  an entity-id-keyed backend. Derive `Hash`/`Eq` for structs and enums
  whose fields are hashable.
- [x] **R13. Revisit "no closures".** *(Follow-up 2026-09-24: expression-bodied lambdas and parameter/return type inference from the expected function type, see docs/syntax/ "Function values".)* *Stage 1 done 2026-09-23: function types and references to named functions (no capture); `collections.map`/`filter`/`fold`/`any`/`all`/`find`/`for_each`/`sort_with`. `datasource.with_transaction` and `http.serve` (with an explicit context value) followed on 2026-09-24, and `spawn` accepts explicit type arguments. Stage 2 done 2026-09-24: lambdas `fn(x: int) -> int { ... }` capturing immutable locals by value (a `var` cannot be captured); see docs/design/journal/ "Closures".* The stated benefit ("libraries can't
  take opaque callback holes") is already delivered by no overloading, no
  methods and no inheritance; the cost is that `http` cannot be a
  framework, every app hand-writes a linear route chain (near-copy blocks,
  the riskiest shape for copy-paste mistakes at ~40 entities), and no
  generic `map`/`filter`/`sort`/`with_transaction` can exist. Decide
  explicitly, and record the decision either way.
- [x] **R14. Derives on generic types.** `#[derive(Ord)]` and `#[derive(Json)]`
  now work on generic structs and enums. Ord generates a generic
  `name_compare<A: Ord, ...>`. Json takes one converter *function value* per
  type parameter (`page_to_json<T>(v, t_to_json)`, `page_from_json<T>(j,
  t_from_json)`), with `int_/float_/bool_/string_ to/from_json` in `json`
  for the scalar arguments. A field like `Page<Point>` gets its converters
  filled in automatically; array/Option type arguments are rejected with a
  message. `Row` and `Properties` still reject generics.

- [x] **R21. `if` and `switch` as expressions.** *Done 2026-09-24: first as the whole right-hand side of `val`/`var`, an assignment or `return` (arms may leave), then anywhere an expression can appear -- an argument, an operand, an element -- provided no arm can leave. See docs/syntax/ "`if` and `switch` as values". Not done: an arm that leaves inside an expression.* (Not from the review --
  surfaced while implementing R4.) Both are statements today, so getting a
  value out of a branch means declaring a `var` first and assigning in each
  arm, or returning from every arm:
  ```
  var status = 500
  switch e {
      NotFound(m) { status = 404 }
      Constraint(m) { status = 409 }
      Connection(m) { status = 500 }
      Sql(m) { status = 500 }
  }
  ```
  Wanted: `val status = switch e { NotFound(m) { 404 } ... }` and
  `val label = if n > 0 { "positive" } else { "not" }`, with each arm's
  final expression as its value. Design questions to settle before
  building: the value of a block (a trailing expression, or a keyword such
  as `yield`); every arm must have the same type (and an `if` used as a
  value needs an `else`); how `return`/`break`/`?` inside an arm behave
  (they leave the enclosing function/loop, not the expression); whether it
  can appear in any expression position or only as a `val`/`var`
  initializer, a `return` value or an argument (compare the deliberately
  narrow rules for `?`); and how it interacts with the Option/Result
  auto-wrap. `switch` already type-checks by rewriting into an `if` chain,
  so the two should be designed together, and doing `if` first makes the
  `switch` case mostly free. Benefit: it removes the mutable-`var`-then-
  assign pattern, which is the shape most likely to leave a path
  unassigned.

**E. Standard library gaps for a backend language**

- [x] **R15. Missing pieces** (HTTP client, logging, filesystem and regex are all done -- regex is a native, linear-time engine without backreferences; see docs/syntax/ and docs/design/journal/ "Regular expressions"): an HTTP *client* (a Ziman service cannot
  call another); logging (levels, destination); filesystem beyond
  `read_file`/`write_file` (append, stat, delete, list; `write_file` is
  still fatal on failure); regex; sorting (R11); domain-typed map keys
  (R12). *Update 2026-10-08: "linear-time" is a promise about the
  subject string, not about the pattern. `regex.compile("()" * 6000)`
  then `is_match` segfaults at every stack size up to the 768 MB
  ceiling (A16) -- the thread-list construction recurses once per
  instruction carrying a VLA frame, and allocates `n_inst x ncap`
  beside it, so the engine is linear in what it scans and unbounded in
  what it compiles. The existing nesting-depth limit does not catch it
  because the pattern is wide, not nested. No backreferences still
  means no exponential backtracking; it does not mean a hostile pattern
  is safe.*
- [x] **R16. Naming convention.** *Done: the rule is written down (docs/syntax/, "Naming conventions for module functions"). Surveying every bundled module found it was already followed -- every name imported bare is noun-prefixed, every qualified-use name is a short verb -- so the only rename was `httpclient.client_error_message` -> `error_message`.* `collections` uses type-prefixed names
  meant for bare import (`hashmap_get`); `http` uses bare names meant for
  qualified use (`header`, `response`), which then collide with
  application modules. Pick one convention per usage mode and document it.
- [x] **R22. More serialization formats, and maybe a `Serde` name.** *Done: `ndjson`, `csv`,
  `toml` and a strict `yaml` subset, each a pure-Ziman module converting text to and from a `Json`
  tree, so every `#[derive(Json)]` type works with all of them (docs/syntax/ "Serialization
  formats"). `Json` gained `JInt` and Map-backed objects first (docs/design/journal/ "JSON on Map").
  No `Serde` alias: `derive(Json)` is the data-model derive and a second derive name would only
  duplicate it -- revisit if that reads badly.*
  *XML followed as `xml`, and the "attribute/text convention" this line waited for turned out to
  be the wrong thing to look for: every such convention (`@attr` keys, text under `#text`) loses
  either the order between text and elements or the difference between an attribute and a child,
  so `xml` has a tree of its own -- `Document`/`Element`/`Node` -- and is the one format module
  that does not go through `Json`. It reads and writes; it has no DTD at all, which is what rules
  out entity-expansion and external-entity attacks, and `XmlLimits` caps size, depth and attribute
  count.*
  *Markdown followed as `markdown`, and it is the one format module whose reader cannot report
  what it does not understand: Markdown has no syntax errors, so an unsupported construct is a
  paragraph rather than a failure. `markdown.parse` does what Markdown prescribes and
  `markdown.parse_strict` returns an Err naming the line, which is the only way a caller learns
  it was handed something that would be misread. The writer is the part that earns its keep --
  escaping chosen per position, so nothing a caller puts in a string can change the structure of
  the output.* Still open: MessagePack/CBOR (binary; bytes have no `Json` form), YAML
  anchors/aliases, TOML dates as a real type, CSV nesting beyond one level.

**F. Tooling and documentation**

- [x] **R17. A formatter (or at least a lint) that makes `val` and `var`
  visually distinct** -- docs/design/journal/ already calls the one-character
  difference a silent-mutability typo hazard. *Lint done: `ziman --lint`
  flags a `var` that is never reassigned, in user code only (bundled modules
  and derived code are skipped); running it over the bundled library found
  three. `ziman --fmt` is the formatter (a text formatter: it keeps comments and line breaks, see docs/design/journal/ "The formatter"); `make fmt-check` keeps the library, backend and extra tests formatted.*
  *Update 2026-10-08: the formatter buffers a quadratic result in
  memory -- 32 KB of nested blocks formats to 256 MB of output at 9.7
  GB RSS, and the same code reached through the language server turns
  a 24 KB document into a 144 MB reply (A18). The quadratic output is
  inherent, since every line is indented by its depth; holding all of
  it before writing a byte is not. `make fmt-check` never sees this
  because real code is not nested 8,000 deep -- an editor and a fuzzer
  both are.*
  *Update 2026-10-09: bounded, and the 2026-10-08 note above was half
  wrong about why. Capping nesting depth (A18) did not remove the
  blowup, because the cost is O(lines x depth) and only depth was
  capped -- a document of ordinary depth and very many lines passed the
  check and still grew without limit. The output itself is now capped at
  16 MB as it is appended (A27), which is ~180x the largest file here, and
  the language server will not analyse a document over 4 MB. At depth 190,
  inside the nesting limit: 60,000 lines used to produce 45 MB of output
  at 235 MB RSS and is now refused at 70 MB. Through the language server,
  that same document's `textDocument/formatting` reply is now a 4,682-byte
  refusal. (The 24 KB / 144 MB case in the note above was already refused
  by A18's depth limit; what A27 adds is the ordinary-depth document, which
  that limit cannot see.) Streaming the output, which the note above implied
  was the fix, was not needed and was not built.*
- [x] **R18. Split README.md into sections.** *Done: README.md is now ~900 words (a taste, the language at a glance, a module table, tooling); the old wall of prose lives in docs/FEATURE-NOTES.md under ten headings.* It is ~3,000 words in
  essentially one paragraph, and it is the document most likely to be
  pasted into a model's context as "the language".
- [x] **R19. Fix documentation drift.** *Done: the three named items, plus a sweep that also fixed the "Eight core modules" paragraph (it claimed `Uuid` had no `Hash`), "no `match`"/"no Ord bound" statements in docs/syntax/, the time/collections/http comments, and stale "no function values" claims (annotated in docs/design/journal/, which is a dated record, rather than rewritten).* docs/syntax/'s `Result` section still
  says "no `enum`, no `match`"; docs/design/journal/ still lists multi-file modules
  under "deliberately deferred"; the `index_of` entry in docs/syntax/'s
  builtins list is corrupted (starts mid-signature) so the function cannot
  be discovered from it.
- [x] **R20. Reconcile the "AI-first" rationale with the real motives.** *Done: docs/design/journal/ "What the AI-first goal does and does not justify" states the real motive per restriction and the training-distribution cost of each divergence; the weakest diagnostics were fixed (`;`, `if`/`fn` in expression position, `.length`/`.size` on a value).*
  Non-nestable `?` is an interpreter constraint (it happens to be good
  style -- say so plainly); "no closures" is justified by a benefit other
  choices already give (R13). Also acknowledge the training-distribution
  cost of deliberate divergences (`Enum.unwrap_X(e)` for `match`,
  `arr = push(arr, x)` for `arr.push(x)`, `concat` for `+`): a model's
  priors are wrong on the most common operations, which makes R5's
  diagnostics matter more.

Things the review said to keep and copy (do not "fix"): block-scoped
`defer`, parser-enforced identifier casing, compile-checked `format`
literals, named arguments reusing struct-literal syntax, no wildcard
imports, non-shadowable globals, readable generated derive source, the
decision-log-with-rejected-alternatives habit, and generating CORELIB.md
from source.
