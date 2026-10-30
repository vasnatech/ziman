# North star: write (a slice of) the health-record backend in Ziman

Part of the [roadmap](README.md) -- see its preamble for what an entry's status means and how it goes stale.


The first concrete target beyond language-design exercises: get Ziman
capable enough to build a real backend service, using the existing
`health-record` backend as the reference for what "capable enough" means.

**Scope, decided:** a representative core slice — a CRUD API, a database,
basic auth, a couple of real endpoints — not full parity with everything
the real backend does (every third-party integration, FHIR, MFA, the
terminology DB, etc.). Expand later once the slice actually works; a
smaller, finishable target is more useful right now than an exhaustive
one that never lands. The architecture to get there:

- ~~**A module system is the load-bearing prerequisite.**~~ Implemented —
  see the full write-up under "Larger, needs its own design pass" below
  (file = module, slash-separated `use` paths, always-qualified calls,
  `pub` for exports, `native fn` for the C boundary). Everything below
  (files, networking, a database client) can now actually be organized as
  a module instead of bolted directly onto the interpreter one builtin at
  a time the way arrays'/structs' builtins were.
- ~~**Only a minimal, fixed set of "native modules" touch C.**~~ The
  registration mechanism is implemented (`src/native.c`), and a first,
  minimal real module exists to prove it: `io` (`read_file`/`write_file`
  — see the module design write-up below for the C-side design, inspired
  by JNI's `RegisterNatives`). A native module still presents ordinary
  typed Ziman function signatures to its callers, who shouldn't need to
  know or care whether a given module is native or pure Ziman. Still
  missing: the actual syscalls (raw sockets, process/env access) beyond
  basic file I/O. The "no bundled-stdlib search path" gap this used to
  note is now closed — see "Bundled core modules" further below: `io.zm`
  is one of the two modules embedded in the `ziman` binary, so no
  project needs its own copy of it anymore.
  `io.read_file` now returns `Result<string, string>` (a missing file is
  recoverable, not fatal — see "Recoverable error handling" below);
  `write_file` is not yet migrated and still fails fatally on a write
  error.
  *Update 2026-10-16: **there is no `io` module any more**, so everything this
  item says about one describes a state that no longer exists. `fs` replaced it:
  `read_text`/`read_bytes`/`write_text`/`write_bytes`, plus
  `write_text_atomic`, each returning `Result<_, FsError>` -- so the
  "`write_file` is not yet migrated and still fails fatally" gap closed by the
  function being replaced rather than migrated, and `lib/core/` now holds 29
  modules rather than the two this item describes as embedded. Kept for the
  reasoning about the native floor, which still holds and is what the entry is
  for; read `fs` for the shape that shipped.*
  Today's built-ins (`print`/`concat`/`str`/`format`/`len`/`push`/
  `insert`/`set`) are conceptually the same idea already, but stay
  special-cased and unqualified for now rather than being migrated into
  the module scheme — see the module design write-up for why that's a
  deliberate non-decision, not an oversight.
- **Everything above the native floor is real Ziman code.** A minimal
  `io` exists now (`read_file`/`write_file`, straight `fopen`/`fread`/
  `fwrite` underneath — no buffering story, no append mode, no directory
  listing). Still ahead: the rest of `io`/`fs` on native file-descriptor
  primitives, `net` (raw sockets) and then `http` (protocol parsing,
  serving, client) on native socket primitives, and a database module
  either as pure Ziman speaking a DB's wire protocol directly over `net`,
  or as a native module wrapping an existing C client library (e.g.
  libpq/sqlite) if hand-rolling the wire protocol turns out not to be
  worth it.
  *Update 2026-10-09: that whole "still ahead" list has landed, and this
  paragraph had gone on describing a state months out of date. `fs` has
  append (`append_text`/`append_bytes`), directory listing (`list_dir`,
  `walk`), atomic writes, byte and line/chunk access, and the path and
  metadata functions. `net` exists with TLS on both ends, and `http`,
  `server` and `client` are pure Ziman on top of it -- the six native
  functions in `lib/http/server.zm` are policy primitives (arena bounds,
  the rate limiter, the per-client connection cap), not protocol parsing.*
  *The open design question in this entry got answered **both ways, split
  by engine**, which is the part worth keeping: `pg` and `mysql` are pure
  Ziman speaking the wire protocol over `net` (`pg` has exactly one native
  function, `float_to_text`, a number-formatting helper), while `sqlite`
  is the native route, nine native functions wrapping the bundled C
  library. So "if hand-rolling the wire protocol turns out not to be worth
  it" was not a single verdict: it was worth it for the two networked
  engines and not for the embedded one, where there is no wire protocol to
  hand-roll in the first place. Both `pg` and `mysql` carry
  `#![experimental]` (see docs/design/journal/'s F4 entry) -- shipped, not yet
  hardened.* ~~`json` (encode/decode) is flagged separately below since
  it hits a real language gap, not just missing library code.~~
  Implemented — see the `json` core module entry further below; the
  suspected language gap (a self-referential `Json` type) turned out
  not to be one.

Building any of this will surface language gaps that don't show up in
toy programs — worth calling out now rather than discovering them one at
a time mid-implementation:

- ~~**No map/dictionary type.**~~ `Map<K, V>` and `Set<T>` are built-in
  types now, on a 32-way bitmap-compressed persistent hash trie
  (O(log32 n), value semantics like every other Ziman value, so `set` and
  `remove` return a new map and share the untouched structure) -- see the
  "[Resolved: `Map`/`Set` are now built-in types...]" entry below for how
  that was decided, and docs/syntax/ "Maps and sets" for the operations.
  Keys must support `==` and hashing, which is the `Hash` rule rather than
  a special case for maps.
  The "one collection type" question this entry posed did get answered,
  and not the way the entry assumed: there is no `{K: V}` type syntax.
  A map is written `Map<string, int> { "apple": 4 }`, with the type
  arguments always spelled out, because `{...}` alone could not say what
  an empty literal meant and the sequence brackets stay scoped to
  sequences -- the same reasoning that gave `Result<T, E>` angle brackets.
  Still unfinished underneath, and recorded in docs/design/journal/ rather than
  here: the trie is not arena-routed, so a map crossing a thread is
  allocated the way everything was before that work.
- ~~**No optional/nullable type.**~~ `Option<T>` exists now (built on
  generics, not a hardcoded parametric type like `Result<T, E>` after
  all — see the "Generics" and "Collections module" entries below for
  how it was originally built, and the "Prelude" entry in docs/design/journal/
  for why it later moved out of `collections` into the prelude, needing
  no `use` anywhere). Still genuinely needed for a
  *direct* self-referential struct with no escape (`struct Node { next:
  Node }` is still unbuildable as-is; `struct Node { next: Option<Node>
  }` would be) — but a struct/enum that recurses *through an array*
  instead (`enum Json { ... JArray([Json]) ... }`, see the `json` core
  module entry below) never needed `Option<T>` for this at all, since
  an empty array (`[]`) is already a natural finite base case. The two
  looked like the same problem from here; they weren't.
- ~~**No recoverable error handling.**~~ `Result<T, E>` exists now — see
  "Recoverable error handling" under "Near-term, likely next" below for
  the full design and what's still not covered by it (see also: it
  turned out *not* to force the generics question the way this entry
  originally guessed — see that entry's update).
- ~~**Almost no string manipulation.**~~ `concat`/`str`/`format` plus now
  `len` (also accepts `string`), `substring`, `split`, `trim`,
  `to_upper`/`to_lower`, `parse_int`/`parse_float`, and `contains`
  (string-in-string, and array membership for scalar `[T]`) — see
  docs/syntax/ "Builtins". ~~Still missing: `replace`, and a general
  `index_of`/`find` (deferred — see the `index_of` entry below on why it
  needs `Option<T>` or a Result-shaped decision first).~~ *Update 2026-10-16:
  both shipped and this clause had gone stale. `replace(s, old, new)`,
  `index_of(s, needle, from)` and `last_index_of(s, needle)` are builtins
  (docs/syntax/ "Builtins"), and the Result-shaped decision the deferral was
  waiting on was settled by returning `-1` rather than an `Option<int>`.* Numeric builtins
  now also exist: `abs`/`min`/`max`/`floor`/`ceil`/`round`. Array
  `remove(arr, i)` exists, and `==`/`!=` on arrays is now allowed when
  the element type is scalar (structs, Results, and nested arrays still
  aren't comparable — see docs/syntax/ "Arrays"/"Structs"). This closes
  out the "complete the built-in function set" pass.
- ~~**No concurrency story.**~~ Implemented: real OS threads via `spawn`
  and a thread-safe `Channel<T>` (docs/syntax/ "Concurrency"), built on
  a single-writer/actor pattern rather than shared memory plus locks —
  one thread owns a piece of mutable state and is the only thing that
  ever touches it; everyone else reaches it by sending a message and
  waiting for a reply on a `Channel`. `Channel<T>` is a third hardcoded
  parametric type (after `[T]`/`Result<T, E>`), but native by necessity
  this time, not by choice — see docs/design/journal/'s new entry for why that
  doesn't reopen the generics question the way `Option<T>`/`Map<K, V>`
  did. Surfaced by (and solves) the exact problem found writing
  `health-record-backend-ziman`'s layered example: pure functional
  state-threading works within one request's own call chain, but doesn't
  answer how *multiple concurrent* requests reach one shared,
  consistently-updated store. Still open, not yet decided:
  - ~~**Unbounded queue.** `send` never blocks in v0 — no backpressure/
    capacity limit, a real production concern (a runaway producer could
    exhaust memory) but not a correctness one. Revisit if a real program
    needs bounded backpressure.~~ *Update 2026-10-16: answered on 2026-09-24
    and this item never caught up -- R10's own entry records it. A channel is
    unbounded (`channel_new()`: `send` never blocks) or **bounded**
    (`channel_new(8)`: `send` blocks while full, which is the backpressure
    asked for here), with `try_send`/`try_receive`/`channel_len` alongside
    and `channel_bounded.zm` pinning it.*
  - **No thread-handle/join mechanism.** A spawned function must return
    `void`; its result, if any, can only come back through a `Channel`
    it's explicitly given. Revisit only if a concrete need for "wait for
    this thread to finish" shows up.
- **Generics: the "just hardcode it" strategy stopped scaling, and this
  entry's earlier prediction has flipped.** `Result<T, E>` shipped
  *without* general generics, using the same "hardcode one more
  parametric type into the compiler" trick arrays already used — a
  deliberate, cheaper alternative that solved that one problem well.
  But `Option<T>` and `Map<K, V>` would each mean repeating that same
  one-off C special-casing forever, in C, never in Ziman itself — the
  opposite of what this project is *for*. User-declared `enum` (tagged
  unions, docs/syntax/ "Enums") shipped as half the fix: it's concrete/
  monomorphic today (`enum Shape { Circle(float) Point }`, not
  `enum Option<T> { Some(T) None }`), but combined with real type
  parameters it would let `Option<T>` — and anything Result/Option-shaped
  a user wants — be written as ordinary Ziman, not another C builtin.
  Generics themselves were the still-missing other half, tracked as its
  own design pass below (parser syntax for `<T>` on `struct`/`enum`/`fn`
  declarations, and a monomorphization-or-erasure runtime strategy).
  *Update 2026-10-16: **done, and this paragraph had gone stale** -- it still
  read "in progress, staged struct → enum → function; generic structs are
  done", which is the state of some weeks ago. All five stages shipped
  2026-10-10 (F39): generic structs, bounded type parameters, generic enums,
  cross-module generics and generic functions. `Option<T>` is now ordinary
  Ziman built on them rather than another C builtin, which is what this entry
  wanted. The one residue is stated in that entry: a generic function body can
  do nothing with an abstract `T` beyond `Eq`/`Hash`/`Ord`, and extending that
  was the `Row` bound, withdrawn 2026-10-09 (C18) on finding it cannot work the
  way `compare` does.*
