# Design journal, part 2 (entries dated 2026-09-01 to 2026-10-10)

Part of the [design journal](README.md) -- see its "How to read this journal" for what a dated entry means.

## Formatter: wrapping, comment alignment, sorted `use`, stdin (open item 9)

**Extends "The formatter (`ziman --fmt`, R17)" above**, which is the entry to read
first for the design and is the one that lists these four as missing.

The formatter (see "The formatter" above) gained the four things it lacked, all in
the same text-level design, still self-checked for token equality.

**Line wrapping.** The formatter used to keep every line break where the author put
it. Now a line wider than the width (`--width=N`, default 120, 0 = never; comments
are not counted) is broken after a comma of its *outermost* bracket group, chosen
greedily as the last comma that keeps the first part within the width, and the
process repeats on the continuation until every line fits or has no legal break.
"Legal" is what the parser already accepts a newline after: a comma inside a call,
an array literal or a struct literal. It skips commas inside a generic list
(`Result<int, string>`: tracked as unmatched `<`, which also covers a comparison),
inside `fn(A, B) -> R` types, and in `#[...]` attributes, where a newline would not
parse; a line with no legal break is left long. The wrapper works by setting the
next item's "newline before" flag and re-emitting, so indentation of the
continuation comes from the existing bracket rule, and it never joins lines, which
keeps the formatter idempotent (checked over the library and backend). Reformatting
the library and the sample backend with it changed 33 files and every test still
passes, which is the check that the breaks are all places the parser accepts.

**Trailing comments** on consecutive code lines are aligned to one column (two
spaces after the longest line of the run); a blank line or a line without a trailing
comment starts a new run.

**`use` lines**: a run of consecutive `use` lines (no blank line, no comment line
between) is sorted by text, so `use http` precedes `use http.Request`. Order has no
meaning in Ziman (imports resolve after all are read), so this is safe; the
self-check compares against the sorted sequence.

**stdin**: `ziman --fmt -` reads stdin and always writes stdout, for an editor's
"format buffer". Still not done: joining lines that would now fit, wrapping at
binary operators or `&&`/`||`, wrapping long `use` or `if` conditions, and an LSP
"format document" (item 10).


## `--fmt --canonical`: settling brace placement, which plain `--fmt` will not (2026-10-07, F12)

**Extends "The formatter (`ziman --fmt`, R17)" and "Formatter: wrapping, comment alignment, sorted
`use`, stdin" above.**

The formatter keeps every line break the author wrote. That is what makes it safe to run on anything
lexable, and it is also why it does not settle style: a file in Allman braces and a file in K&R braces
both pass `--fmt --check`, which is most of what people actually want from gofmt. `--canonical` is the
opt-in that settles the part of it a scanner can decide on its own.

**What it does.** A `{` that starts a line joins the line before it, with one space. An `else` on its
own line after the `}` that closed its `if` joins that line. Nothing else moves, and without the flag
nothing moves at all.

**Why every line-leading `{`, and not only a construct's.** A first version was narrower, out of a
worry about a bare block statement:

```ziman ignore: the point is that this is not valid Ziman, so it cannot be formatted either
print(x)
{
    val scoped = 1
}
```

Pulling that brace up would produce `print(x) {`, a different program. It turns out Ziman has no bare
block statement at all -- a `{` cannot begin one, and the parser says so ("expected an expression,
found '{'") -- so there is no case where moving a line-leading brace up changes meaning, and the rule
covers a `switch` arm's pattern and a struct literal as well. Finding that out is what made the rule
simpler rather than more complicated, which is the opposite of how these usually go.

The one brace left alone is one that follows a trailing comment, which would be commented out if it
moved.

**Verified**: the canonicalised output still runs; running it twice changes nothing (it only ever
clears a newline, and a brace already in place has none to clear); `format_source`'s existing
token-equality self-check covers the rest; and `--fmt --canonical --check` over all of `lib/` and the
documentation examples reports no change, so the library was already in this style and the flag is a
no-op on it.

**A side effect worth recording.** Adding a call between `sort_use_runs` and its caller lost GCC the
proof that `items->n` is not negative, and it warned that a `memcpy` bound might be 18 quintillion
bytes. The bound is now guarded explicitly. The warning was about pre-existing code that only became
unprovable because something new was called next to it.


## `--coverage-lcov=F`: coverage something other than a person can read (2026-10-07, F13)

**Extends the `--coverage` entry above.** The text report is for a human and cannot gate anything:
to fail a build on a coverage drop, or to draw an annotated source view, a machine needs the numbers.
`--coverage-lcov=F` writes them in lcov's `.info` format -- `TN:`, `SF:`, one `DA:<line>,<count>` per
executable line, `LF:`/`LH:`, `end_of_record` -- which `genhtml`, Codecov and Coveralls all read.

**Counts, not a flag.** `LineHit` held a boolean "did it run"; it now holds how many times, summed
over the statements on the line and over every module accumulated. Nothing extra is counted at run
time to get that: `Stmt.exec_count` already had it and the text report was throwing it away.

**It merges, rather than telling the reader to.** An existing `F` is read first and this run's counts
are added to it, so several invocations into one path accumulate -- which a project needs when its
tests are several runs rather than one. The alternative was to write a fresh file each time and say
that concatenating two `.info` files merges them, because lcov sums repeated records for a file. That
is true as far as I know, and it is also a claim about a tool that is not installed on the machine
this was written on, so it would have been unverifiable here. Merging in `coverage.c` is a claim about
code in this repository, and `make test-coverage` checks it.

The cost is that counts only grow, so a stale `F` flatters the result. That is why the path is always
named explicitly and never defaulted, and why the help text says to delete it to start fresh.

**Verified** two ways. `make test-coverage` builds a Module by hand -- two statements, one run and one
not -- and checks the records, the `LF`/`LH` totals, and that a second write keeps what the file
already had while gaining what it did not. And by hand across processes, which is the case the unit
test cannot stage: one program with two branches, run twice with different arguments into one file,
took `LH` from 4 to 6 of `LF:6`, with the lines common to both runs summing rather than being
overwritten.


## A request knows where it came from, and a server can rate-limit on it (2026-10-07, B10)

Rate-limiting an unauthenticated endpoint needs to tell one caller from another, and nothing in the
library could. Worth being precise about why, because the task names a comment as the position to argue
with and that comment is about something else: it says the connection *timeouts* cannot tell one client
from another, which is true and unrelated. The actual gap was lower down -- `net` had `getsockname`, for
a listener's port, and no `getpeername` at all, so a peer address was not merely withheld from handlers
but unreachable from Ziman.

**`net.peer_address(socket)`** answers it, and **`Request.peer`** carries it to handlers, filled in by
`read_request` at both places it builds a Request and left `None` on `write_error`'s stub, which has no
connection behind it.

**A field rather than a synthetic header**, which is the security-relevant choice. Injecting something
like `X-Ziman-Peer` into the parsed headers would have changed no API and broken no literal -- and a
client could send that exact name, so it would be safe only as long as the parser stripped it. Relying on
that is the subtlety a peer address exists to avoid. Where a value comes from should be visible in its
type. The cost was 13 Request literals; two of them contain `if ... { } else { }`, so brace-matched
editing stopped at the inner block and put `peer: None` outside the literal, which the suite caught.

**`X-Forwarded-For` is never read by default**, and `server.forwarded_for(req, trusted_hops)` is how an
application opts in. `trusted_hops` counts proxies *you own*, and the function takes the entry that many
places from the **right**, because each proxy appends what it saw and only the rightmost entries were
written by machines you control. `0` returns the socket address and ignores the header entirely. Getting
the number too high is the dangerous direction: it starts trusting entries the client wrote. There is no
default and no auto-detection, because a library cannot know what is in front of a server and guessing
would mean every deployment that is *not* behind a proxy accepts a header its clients control.

**`server.rate_allow`** is a fixed-window counter in C, because an application cannot write one: Ziman has
no mutable state two threads can share -- a `MapBuilder` is `#[thread_bound]` on purpose and a lambda
cannot mutate what it captures -- so a thread-per-connection server cannot count anything across requests
without a native handle. `rate_allow_at` takes the clock the way `jwt.verify_at` does, so a window can be
shown expiring without waiting for one.

Three properties stated rather than left to be derived. It is a fixed window, so a caller can spend its
allowance at the end of one window and again at the start of the next: the honest worst case is twice the
limit over a window's width. At most 4096 keys are tracked, and on a full table an expired entry is reused
first, then the oldest window replaced -- so a flood can push a legitimate caller's count out and hand them
a fresh allowance, which is a bounded loss of accuracy chosen over refusing everyone (a denial of service)
or allowing everyone (no protection). And a limit of zero means "not configured", not "refuse everything".

**What the default had to be, measured rather than chosen.** The reference backend throttles `/api/auth/*`
per address, enforced in `routes.route` because registration's cost begins before it knows who is asking.
At 20 a minute the project's own suite failed three ways -- the lockout test makes eleven wrong passwords
on purpose, the MFA test more, and log rotation needs a run of registrations -- which is a fair model of
several real users behind one NAT. At 100 a minute all twenty suites pass, and a flood of thousands a
minute is still refused. A default that breaks legitimate use is worse than a high one.


## A per-client connection cap, which a request throttle cannot be (2026-10-07)

B10 gave the server a per-address request throttle. This is the half it cannot cover, and the reason the
two are separate mechanisms rather than one: a client that opens connections and **sends nothing** is
counted by no rate limit, because there is no request to count, and it can still fill every
`max_connections` slot. `header_timeout_ms` frees such a slot within seconds, which bounds how long the
flood holds one, not how many it holds at once.

`ServeOptions.max_connections_per_client`, 0 = off and that is the default: behind a reverse proxy every
connection carries the proxy's address, so a cap would cap the proxy. The address is necessarily the
socket's -- at accept time there is no request, so there is no `X-Forwarded-For` to consult even if it
were trustworthy -- which is the same reason the default is off.

**Why a second counter rather than `rate_allow`.** A rate limiter counts events in a window and never
counts down. A connection cap goes up on accept and down on close, so what it holds is live state, not a
decaying measurement. That difference also changes what to do when the table is full: the rate limiter
replaces the oldest window, while the cap **admits the connection and does not track it**. Refusing would
deny service because a table is busy, and evicting someone else's live count would silently raise their
cap; tracking fewer clients than there are is the only failure here that loses nothing already promised.

**Decided in the accept loop**, before a slot is taken and before a thread is spawned, because a cap is
only worth having if refusing costs less than admitting -- and that is only true if the refusal happens
before both. A refusal closes the socket without writing anything: answering would mean writing to a
flooding client from the accept loop, where one slow read stalls every other connection's accept.

**The release is the part that can go wrong quietly.** It is a `defer` in the connection thread, so it
runs on every path out including a handler that failed; a release that is ever skipped leaks the cap
downwards until the address can open nothing at all. `connection_key` returns `""` when the cap is off and
both natives ignore an empty key, so the `defer` needs no condition -- a `defer` that has to ask whether
to run is how a release gets skipped on the path nobody tested. The fixture checks the count returns to
zero after every connection closes, which is what would catch a leak.


## Grouped imports, declined (2026-10-07, C17)

Whether `use mod.{A, B, C}` should exist. SYNTAX.md asserted "one name per import, no `{A, B, C}` list"
without saying why, which is what put the question back on the table: a rule with no recorded reason is
indistinguishable from an oversight.

**The complaint is legitimate.** The reference backend has 1,275 `use` lines, and
`service/medication_adherence.zm` imports fifteen names from `time` alone. The cost is not typing —
emitting fifteen lines instead of one is nothing — it is *positional*: adding one name to an alphabetised
run means finding the right slot. That friction is real enough that an earlier session wrote a script to
maintain the ordering rather than do it by hand.

**Three things outweigh it.**

`--fmt` already sorts runs of consecutive `use` lines. The positional problem was solved before it was
raised; the right response to it is to run the formatter, not to change the language. An argument that
disappears once you use an existing tool is not an argument for new syntax.

One name per line is independently valid. A mistake costs one line and the error points at it, while a
malformed group — a missing brace, a stray comma — breaks the whole import and reports at the construct
rather than at the mistake. This matters more for generated code than for typed code: one line at a time
is the shape that goes wrong least.

One fact per line stays greppable. `grep -rn "use service.is_email"` answers "who imports this?" exactly;
with groups it becomes a multi-line pattern. For anything working in a repository larger than it can hold
at once — a person with grep, or a model with a tool — a self-contained searchable line is worth more than
brevity.

**And the principle decides it.** "One way to do a thing. No syntactic synonyms" is this language's second
principle, and a group is a second way to import three names. The places Ziman is actually hard to write
correctly are the places with an unstated option, not the verbose ones: whether `defer` is block- or
function-scoped (block, and the gap cost a silent bug in the metrics work), whether `\u` is a string escape
(it is not), whether a `pub val` is reachable as `module.name` (it is not). None of those is a verbosity
problem, and none would be helped by a shorter import.

**What would change this.** A reader finding fifteen `use time.X` lines worse to read than they are to
generate is a legitimate reason to revisit — the argument above is mostly about writing and searching, and
reading is the one side of it that a human owns. Nothing measured here speaks to that.


## The language server

`ziman --lsp` (src/lsp.c) reuses the compiler front end rather than a second
implementation. Three small hooks make that possible without `exit(1)`: an error
sink (`ziman_set_error_sink`) that collects diagnostics instead of printing them
and unwinds to an outermost recovery point; loader source overrides
(`loader_set_overrides`) so unsaved editor buffers are what gets parsed; and
`typecheck_skip_main_check` so a library file is not reported for lacking `main`.
Each edit re-analyses the whole program (entry = nearest `main.zm`, else the file)
and republishes per-file diagnostics -- up to a size: a document over 4 MB is
stored but not analysed, and gets one diagnostic saying so (2026-10-09, A27).
There was no limit at all before, so a client could hand the server an
arbitrarily large buffer on `didOpen` and pay a full parse and typecheck of it on
every keystroke after. The limit is applied in one place that both `didOpen` and
`didChange` go through, so it cannot hold for one and not the other, and 4 MB is
about 45x the largest file here. Measured over the real protocol: a 4.3 MB
document costs 74 MB of peak memory in 0.09 s, where a *smaller* 2.1 MB one that
is analysed costs 207 MB in 0.43 s. Skipping silently would have been worse than
refusing, because a file with no diagnostics looks like a file with no errors; declarations carry `source_file`, which is
what go-to-definition, hover and symbols read. Positions convert between Ziman's
byte columns and LSP's UTF-16 units. Hover and definition resolve by identifier
(plus a `module.` qualifier), not by scope, so a local shadowing a global name
resolves to the global.


## Map and Set

`Map<K, V>` and `Set<T>` replaced `collections.HashMap`/`HashSet`. As pure-Ziman
library types over immutable arrays every write copied the whole bucket array
(O(bucket_count)), and with no traits there was no way to give a TreeMap or any other
map the same API without duplicating every function. Making them built-in fixes both.

- **Persistent, not mutable.** The storage (src/mapobj.c) is a 32-way hash trie with
  bitmap-compressed nodes. `set`/`insert`/`remove` copy only the nodes on the path to the
  entry and share the rest, so the collections keep value semantics exactly like arrays
  (`val b = set(a, k, v)` leaves `a` alone) with near-O(1) writes. A mutable handle, like
  `StringBuilder`, was rejected: it would have made `Map` the one data type whose copies
  alias.
- **Insertion order.** Each entry stores the sequence number it was first inserted with;
  iteration collects and sorts by it (O(n log n), next to an O(n) loop body). Chosen over
  hash order so `print`, `for` and the key/value arrays are deterministic and unsurprising.
- **Surface.** Types are the reserved words `Map`/`Set` (like `Channel`, so
  a user `struct Map` is now an error). Literals are `Map<K, V> { k: v }` and `Set<T> { a }`
  with the type arguments written out -- consistent with "explicit over implicit", and it
  makes the empty literal need no inference. They parse into calls to the internal builtins
  `$map_lit`/`$set_lit`, so the loader and generic machinery needed no new node kinds.
  `len`, `contains`, `set`, `insert`, `remove` were extended by argument type; `map_get`,
  `map_keys` (returns a `Set<K>`), `map_values` and `set_items` are builtins rather than prelude generics
  because a generic native would need `map_get<string, int>(...)` (Ziman has no inference).
  `for k, v in m` reuses the existing two-binder `for`.
- **`for` walks a snapshot** (a sorted entry array taken when the loop starts), so the body
  may rebind the collection without a hazard to define.
- **Keys** must satisfy the `Hash` rule, checked wherever the type is written.
- **Equality and hash are order-independent**: `==` is same size plus every key of one
  found in the other with an equal value; the hash sums a mixed hash of each entry, which
  is commutative, so equal maps hash equal whatever order they were built in. That makes a
  map or set a legal key, struct field or set element.
- **Json:** `derive(Json)` maps a `Map<string|int, V>` to a JSON object (an int key as its
  decimal text, decoded with `parse_int`) and a `Set<T>` to an array. `Row`/`Properties`
  don't support them. **Not offered yet:** set algebra and other helpers (union,
  intersection, filter, ... -- see docs/ROADMAP.md), and a sorted `TreeMap`.


## JSON on Map

`Json.JObject` was `[JField]`, an array of `{key, value}`. It is now `Map<string, Json>`:
lookup goes from a scan to near-O(1), a duplicate key gets a defined meaning (last wins,
`parse_json_strict` rejects), building an object one key at a time no longer copies an
array per key, and the insertion-order iteration of `Map` keeps document order so output
is stable. `JField` is gone; derived code builds a `Map` literal and decodes through
`map_get`. A `JInt(int)` variant was added so integers round-trip exactly (`JNumber` is a
float, which loses precision above 2^53); `derive(Json)` writes `int` fields as `JInt`
and reads them with `int_from_json` (which also accepts a whole-valued `JNumber`).
Strings gained `\uXXXX`, `\b`, `\f` in both directions. Still true: `Json` has no `==`
(arrays only compare scalar elements); a recursive-type equality would also need a
cycle guard in the type checker's structural walk.


## Serialization formats

`ndjson`, `csv`, `toml` and `yaml` are converters between text and the `Json` tree (which
is the format-neutral data model; see "JSON on Map"), not new derives -- so a type's one
`#[derive(Json)]` serves every format and adding a format never touches `derive.c`. They
are explicit (`person_from_json(toml.parse_toml(text)?)`) rather than generated
`person_from_toml`, which would multiply derive output per format.

Choices worth recording. *Ambiguity is resolved toward safe:* a CSV or YAML number with a
leading zero (`007`, a zip) stays a string; the writers quote exactly the strings that would
re-read as another type, so text round-trips. *Unsupported syntax is an error, never a
misreading:* YAML anchors/aliases/tags/merge keys/multiple documents/tab indentation, TOML
`inf`/`nan`. *What the `Json` model cannot say is flattened or dropped, documented:* TOML/CSV
`null` is omitted/empty, dates are strings, CSV nests one level as dotted columns. YAML block
structure is parsed by first lifting each `- ` onto a line of its own, which turns "a sequence
item that is a mapping" into ordinary nested indentation -- the parser never needs a
special case for it. Speed is the interpreter's: these are for configuration and
export-sized data.


## MapBuilder and SetBuilder

**Superseded in part** by "The builders become mutable collections" (2026-10-13): the two types
are renamed `MutableMap`/`MutableSet` and gain a full read/update surface. The storage design
below -- the private trie, snapshot-not-freeze, and the `shared` flag -- is unchanged by that.

A persistent `Map`/`Set` copies the trie nodes on the path to every changed entry, which is
cheap per call but adds up when a loop builds a whole map: measured in the non-GC build,
`collections.group_by` and `distinct` on 1,000 items produced 4.3 MB and 1.2 MB of garbage.
`MapBuilder<K, V>` and `SetBuilder<T>` are the mutable counterparts (like `ArrayBuilder` and
`StringBuilder`): a handle to a private trie that `mapb_set`/`setb_add` change in place
(`mapobj_set_mut`; growing a node is a `realloc`, which frees the old block). Two decisions:
`to_map`/`to_set` *snapshot* (an O(n) copy of the nodes, leaves shared) rather than freeze, so a
builder stays usable like `ab_to_array`, and a `shared` flag makes an overwrite after a snapshot
replace the leaf instead of updating it, so a snapshot never changes. Removal shrinks a node
without collapsing single-leaf children (still correct; lookups depend only on the hash path).
The persistent `Map` stays the default for ordinary code and for anything that keeps old
versions; builders are for loops that only add. Used in `collections.distinct`/`group_by`, the
`json`, `csv` and `yaml` object builders, and `#[derive(Json)]`'s map and set fields.


## Native handles and finalizers

A `Socket` or a SQLite `Connection` is an integer in Ziman code. It used to be an index into a
global table, which kept every socket and connection ever opened reachable forever: forgetting
`net.close` or `datasource.close` leaked the descriptor or the database for good (measured: 300 unclosed
connects exhausted `ulimit -n 300`; each unclosed in-memory database held ~88 KB). The integer is
now the native object's *address*, and a `HandleSet` (src/handles.c, a hash set in memory the
collector does not scan; **since 2026-10-09 it is 64 shards under their own locks rather than one
lock, which took a socket operation from 158.4 ns to 18.6 ns at sixteen threads -- A34, and read
that entry for why the same change is +0.3% end to end**) says which addresses are open, so a closed or made-up handle is still an
error and not a crash, while being in the set does not keep the object alive. When nothing holds the
Socket/Connection value any more the collector finalizes it (closing the descriptor, freeing the SSL
object, closing the database), and out of descriptors (`EMFILE`) `net` collects at once and retries.
`os.collect_garbage()` runs a collection and the finalizers on demand. The non-collecting build has
no finalizers: it leaks by design.

`pg` and `mysql` connections own a thread blocked on a request channel. Their `Connection` holds a
`Guard` (prelude): a native whose finalizer sends the connection's "close" request when the
Connection value is unreachable, so that thread closes the socket and exits instead of waiting
forever. Still true: close promptly -- a collection may be far off, and a file database stays locked
until it runs.


## Stack depth (2026-09)

Deep recursion used to run off the end of the C stack and crash with no message (about 1,450 calls on the default 8 MB). The interpreter is tree-walking and
recurses in C, so the fix is not to count Ziman calls but to guard the C stack: `src/stack.c` reads each thread's real bounds once (`pthread_getattr_np`), keeps
the low limit plus a margin in a thread-local, and `ZIMAN_STACK_CHECK()` (one compare against the address of a local) runs at every call, statement, expression
and parse/check step. Measured on the prototype: within noise on call-, loop- and mixed-heavy programs, and no cost per spawn. A Ziman call costs 3.5-11 KB of C stack
depending on its shape, which is why a frame count would have been the wrong limit.

The stack sizes are decided at run time (`stack.h`): the main interpreter thread runs on a created thread whose stack, and the stack of each `spawn`, come from an eighth of
the memory the process may use, split by the threads it plans for, with options to override. The 768 MB cap is the garbage collector's: Boehm aborted once a thread's stack
was near 1 GB. Deeper causes are not investigated (a `GC_allocobj` failure with a huge reserved stack); the cap keeps every accepted configuration on the working side.
Not done: a fatal error in a spawned thread still ends the whole process (task A2).


## A runtime error ends the thread it happened in (2026-09)

`ziman_error` used to exit the process, so one bad request in a thread-per-connection server was an outage. A spawned thread now arms a `ThreadBoundary` (error.h) before it
runs: a *runtime* error on that thread is not printed and does not exit, it is stored as text and the thread `longjmp`s back to its entry, where `thread_failed` (interpreter.c)
calls the optional `on_error` handler -- a `fn(string)` value, chosen per spawn: `spawn f(x) on_error handler` -- or, with none, writes the error to stderr; then runs the
deferred calls the thread had pending. The jump skips the block exits that would have run them, so scopes are put on a thread-local list at their first `defer` and removed when
their calls have run normally. The handler runs before the deferred calls, while the thread's resources are still open (the server writes the 500 to a socket its own `defer`
has not closed yet). Errors of other phases, and errors on the main thread, are unchanged. `os.fatal` is the handler that keeps the old behaviour for a thread.
Not done: nothing catches a *native* crash (a segfault); and an error while a native holds a lock would leave it held (none does).


## `push` is amortised O(1) (2026-09)

This is the *immutable* array's append and it keeps its name. Its mutable counterpart is
`mutable_array_push` -- see "The builders become mutable collections" (2026-10-13).

An `ArrayObj` built by `push` carries an `ArrayStore { capacity, used }` shared by every array that views the same buffer. `push` on an array of length n whose store has room
and whose `used == n` (it is the newest version, the "tip") claims slot n with a compare-and-swap on `used`, writes the element there and returns a new `ArrayObj` of length n+1
over the same buffer; a losing thread, or a `push` on an older version, copies into a fresh buffer of twice the size instead. Slots below an array's length are never written
again, so arrays stay immutable values and threads need no lock. Literals, slices and builder results have no store: their first `push` copies. Loops that rebuild a value with
`concat` stays quadratic; `--lint` flags it, and reads the type to say which builder to collect into.


## `try` hoisting (2026-09)

`try expr` is the prefix spelling of `expr?` and, unlike it, may be nested. It is a parser rewrite, so the type checker and the interpreter keep the statement-level `?` only: `parse_block` runs `hoist_stmt` on
each statement it parses, which replaces every prefix `try` in the positions that run once and unconditionally (val/var initializer, assignment, return value, `if` condition, `switch` subject, `for` range or
iterable) by a hidden `val $tN = inner?` placed in front. So that the order of effects stays the written one, everything the statement evaluates before a `try` and that is not pure (any call, an index, a
division) is hoisted into a temporary as well; `hoist_expr` carries a `later` flag for a subtree that some later part of the statement's `try` has to wait for. Literals, identifiers, lambdas and arithmetic on
them stay put. A `try` in a position the hoister does not handle (`else if` and `while` conditions, the right of `&&`/`||`, `defer`/`spawn` arguments, an `if` or `switch` used as a value) is left in the tree and
refused by the type checker, so no conditional evaluation is ever changed. A lambda body is its own function and hoists on its own. Named arguments are hoisted in the order written, though `check_call`
later reorders them into parameter order. The formatter and the language server work on tokens and are not affected. The postfix `?` deliberately stays statement-level: the eye finds `try` at the left of the
line, and there is one spelling of each early exit per position, not two ways to nest.


## `try` replaces the postfix `?` (2026-09)

The postfix `?` is gone; `try expr` is the only spelling (the earlier entries above describe `?` as it was designed). `?` is now a syntax error that says to write `try`. Reason: a trailing `?` is easy to
miss when reading, above all once nesting made it possible in the middle of an expression; a prefix keyword marks the early exit at the left of the line. The rewrite of the existing code (`lib/`, the tests, the
backend and the doc examples) was mechanical: `val x = f()?` became `val x = try f()` and a bare `f()?` became `try f()`. The AST node (`EXPR_TRY_UNWRAP`), the type check and the interpreter are unchanged, and
the generated code of `#[derive]` uses `try` too.


## `#[from]`: error conversion in `try` (2026-09)

`try` on a `Result<_, P>` inside a function returning `Result<_, E>` with `P != E` used to be a type error, which left the backend with about 160 hand-written `Err(failure) { ServiceError.Storage(failure) }`
blocks. A variant of an enum can now be marked `#[from]` (parsed in `parse_enum_decl`, stored as `EnumVariant.is_from`): `check_try_unwrap` looks for a `#[from]` variant of the function's error enum whose
payload type is `P`, records its name on the node (`try_unwrap.convert_variant`) and `eval_try_unwrap` wraps the error in it when it propagates. It is a declaration on the enum, not an inference at the call site: a
second `#[from]` for the same payload type, one without a payload and one on a generic enum are refused, and the error message of an unconverted `try` names the option. Only `try` converts: `return err(x)`,
the auto-wrap positions and `switch` arms are unchanged, and there is no chaining (`A` into `B` into `C`). `Result<_, string>` stays explicit on purpose: marking a `string` variant would turn every string
error inside a service into that variant. A `map_err` in the prelude, and enum constructors usable as function values, are left for later.


## Generic call inference, part 1: plain arguments (2026-09)

A generic function call may now omit its type arguments when they can be worked out from the checked types of its own arguments (`identity(5)`, not `identity<int>(5)`); explicit type arguments are unchanged and still work. `check_call` (`typecheck.c`) does this in a new `infer_call_type_args`, called only when `type_args->count == 0 && fn->type_params.count > 0`: for
each fixed parameter whose *declared* type mentions one of the function's own type parameters (`type_mentions_any`), and whose argument is a form `check_expr` can type without an expected type already in hand (`!expr_needs_expected_type` -- excludes an empty array literal, `ok`/`err`/`channel_new`, an unannotated lambda, and an `if`/`switch` used as a value), the argument is checked and its type is unified against the declared parameter type (`unify_type`, a plain structural walk that binds a bare type parameter and recurses through array/result/channel/set/map/func/struct/enum shapes, erroring only on an actual conflict between two arguments). The
variadic tail is unified too: a lone trailing argument whose own type is an array is read as the spread form (unify against its element type); otherwise, or with several trailing arguments, each is unified as an individual element value -- the same shape-driven test `call_function` itself uses once the element type is known, just without that reference (a variadic parameter whose own element type is itself an array, passed as one spread argument, is the one case this
can misjudge, same class of ambiguity call_function's own comment on `variadic_tail_is_array_spread` calls out). A type parameter nothing bound is today's "requires explicit type arguments" error, now naming which one. Bounds are checked on the inferred type arguments exactly as on explicit ones. `find_function`, `substitute_type`, `check_value_against_param_type`'s "already resolved, reuse it" cache (`arg->resolved_type`) mean a "peeked" argument is not re-type-checked by
the ordinary argument loop that runs afterwards -- it just reads the cached type back. A struct/enum's own construction and `spawn` of a generic function stay always-explicit; unaffected on purpose (see docs/ROADMAP.md's C3 entry for what "part 2" -- inferring through an unannotated lambda argument -- would still need).


## Generic call inference, part 3: bare `Some`/`None` (2026-10)

`Some(x)` and `None` take their type from the context, which was already true in the five places that offer one -- an annotated val, a struct literal field, a `with` override, a returned value, a call argument. Three places had the context and did not pass it inward, so the explicit spelling was the one that worked:

* **Inside `ok`/`err`.** `check_ok_err_call` knew its own payload type (`*expected.elem`) and still checked its argument with a bare `check_expr`, so `return ok(Some(x))` from a `Result<Option<T>, E>` function was refused while `ok(Option<T>.Some(x))` was fine. It now checks the argument *expecting* that payload type, through the expression slot so a bare `None` can be rewritten in place as it is everywhere else.
* **Inside another `Some`.** `check_bare_option_call` had the same shape, and the error message told the reader to write `Option<Option<int>>.Some(Option<int>.Some(4))`. Same fix, same one line.
* **As an argument to a generic function.** `expr_needs_expected_type` -- the predicate `infer_call_type_args` uses to decide which arguments it can check before the type parameters are bound -- listed `ok`, `err` and `channel_new` but not `Some` or bare `None`. So a generic call checked them eagerly and failed, and `assert_eq(Some("es"), opt)` was rejected although the *other* argument said which Option it was. Both are now on that list and get checked afterwards, against the bound type.

The last of those leaves a case the predicate alone cannot fix: `same(Some("es"), None)`, where every argument needs a context and nothing binds `T`. But a bare `Some(x)` **does** say which Option it is whenever its payload types on its own -- `Some("es")` can only be an `Option<string>` -- so `infer_call_type_args` now reads that payload and unifies against `Option<payload>` (`bare_some_payload_type`, deliberately not applied to `Some(None)` or `Some(ok(1))`, where the payload needs a context too). `None` can never participate: there is nothing inside it to read, which is the whole reason it needs the context in the first place.

**Two more contexts, found by sweeping the sample backend a second time.** *Assignment*: a `var`'s declared type is the context its value needs, and the declaration already used it (`var found: Option<string> = None`, and `val a: Option<int> = 5` through the bare-value auto-wrap) while the assignment one line below offered nothing, so `found = Some(x)` was refused. It now goes through `check_expr_expecting`, which subsumes the lambda case the assignment path used to handle by hand. *Equality*: the two sides of `==`/`!=` are the same type, so each is the other's context -- whichever side types on its own goes first, the other is checked against it, and `record.status != Some("ACTIVE")` stands. Only the equality operators: the arithmetic and ordering ones take no Option, so offering one there would produce a stranger message for the same mistake.

**What is still refused, and why it should be.** `val a = None` and `print(Some(5))` -- no annotation, and a parameter that accepts anything, so nothing says an Option is wanted here at all. The fix for the first is to annotate it, which is the better spelling anyway: `var x: Option<T> = None` puts the type in type position, where `var x = Option<T>.None` puts it in the value. `only(None)` where `only<T>(a: T)` is the only argument: the type parameter has nothing to bind from, reported as "cannot infer type argument 'T'". Both messages now name a generic argument as one of the contexts that counts, since it is one as of this change.

**One level further in: the `Result` bare-value auto-wrap.** A `Result<T, E>` already accepts a bare `T` and rewrites it as `ok(T)`, but only once that value had a type of its own -- so a repository's `return Option<Row>.Some(row)` worked where `return Some(row)` did not, in a function declared `Result<Option<Row>, DbError>`. That is the shape of every `find_by_*` in the sample backend. `check_value_against_param_type` now offers the Ok payload type to a value that needs a context (`expr_needs_expected_type`) before giving up, and wraps the result. Deliberately **not** when `T` and `E` are the same type: that is the one case the wrap below it already refuses to guess at, and guessing earlier would be worse rather than better. `ok`/`err` are not retried against the payload either -- those match the `Result` itself one branch above, and a bare `ok(x)` reaching the auto-wrap would mean `ok(ok(x))`, which nobody writes by accident.

**The variadic tail, which was assumed to be a different problem entirely.** The sites that still
needed the qualified spelling after all of the above were all `mutable_array_push`, so the cause
looked like "a builtin's arguments are hand-checked". It is not: a plain function with a
`variadic Option<int>` parameter refused `Some(1)` while the *same function with a fixed parameter*
accepted it, and `mutable_array_push(ab: MutableArray<T>, pieces: variadic T)` is declared in the
prelude like any other. A fixed parameter's argument goes through `check_value_against_param_type`;
the variadic tail called a bare `check_expr`, so it offered none of what that function gives -- not
a bare `Some`/`None`, not `ok`/`err`/`channel_new`, not an unannotated lambda, not the bare-value
auto-wrap (`f(4)` where the element is an `Option<int>` was refused). With several trailing
arguments there is no ambiguity at all -- each is an element -- so each now goes through the same
path a fixed parameter's does.

A *lone* trailing argument is the exception, and stays one. It may be a single element or an array
to spread, and which it is is read **from its own type**, so the argument has to be typed before
the question can be answered. Three forms answer it by what they are -- `Some(x)`, `None`,
`ok(x)`, `err(e)` and `channel_new()` are never arrays -- and those are now accepted. An `if`/`switch`
whose arms carry no type of their own is not answerable: typing it is what fails, and offering the
element type instead would take the spread reading away from `f(if c { arrayA } else { arrayB })`,
which works today. That one keeps the qualified spelling, and now says so in its own error rather
than reporting "cannot use a void value as an argument".

**What this is not.** Inference still flows one way, from an expected type into an expression. There is no unification variable for an un-annotated `None`, and `Some` is not special-cased to a user enum that happens to declare a `Some` variant -- bare `Some`/`None` mean `Option` and nothing else (a user enum's own variants are always qualified; `Mine("y")` bare is "call to undefined function"). The source comment claiming `Some(x)`'s argument "doesn't say this is an Option rather than something else" was wrong on that point and is corrected.


## Recursive `Eq`/`Hash` (2026-09)

`==`/`contains` on arrays, and `Hash`, used to require the element (or field/payload) type to be a scalar or bottom out in one within `depth <= 64` recursive steps (`type_supports_equality`/`type_supports_hashing` in typecheck.c); an array of a comparable struct/enum, and a struct/enum that refers back to its own type -- `Json`'s own `JArray([Json])`/`JObject(Map<string, Json>)`, or a user's own `enum Tree<T: Eq> { Leaf(T) Node([Tree<T>]) }` -- were both rejected, the second one because the depth counter always ran out (the type's own shape recurses forever, even though any real *value* of it is finite). Both functions now check a small "currently being validated" stack of `Type` values (`g_eq_check_stack`/`g_hash_check_stack`, compared with the existing `type_equals`, which already accounts for type arguments so `Tree<int>` and `Tree<string>` are tracked separately) before recursing into a struct's fields or an enum's payloads: a type already on the stack is assumed comparable/hashable rather than re-entered. This is sound because the recursion is over the *type's declared shape*, not over an actual value's structure -- once every payload not of the same type has already checked out, the recursive occurrence needs no further verification. `depth` remains as a backstop against a non-cyclic but very deep chain, unrelated to this. `struct Node { next: Node }` (a genuinely un-constructible direct self-reference, a separate open gap -- see docs/ROADMAP.md) now also type-checks as comparable; harmless, since no value of that shape can exist to exercise it.


## Must-use `Result` and a real `_` discard (2026-09)

An expression statement (`STMT_EXPR` in `check_stmt`) whose checked type is `TY_RESULT` is now a type error, closing the gap the language review called out (S10): any bare statement, not only `defer`, used to drop a `Result` silently, `Err` included. `defer` is unaffected -- it is checked by `check_defer`, a separate code path from ordinary `check_stmt`, and stays deliberately exempt (see its own DESIGN.md entry). Making this an error needed a real way to discard a `Result` on purpose, so `_` became a genuine discard rather than an ordinary
variable name: `env_define` (`src/env.c`, shared by the type checker and the interpreter) now special-cases `name == "_"` at its very top, returning success without ever inserting a binding. This is the *only* place `_` is special -- no parser or `check_*` code needed its own case, since every binding form (`val`/`var`, a function parameter, a loop binder, a `switch`/`if` payload binder) already funnels through `env_define`. A `_` therefore never conflicts with an earlier `_` in the same scope, and reading it back is the ordinary "use of undefined variable" every other never-declared name already gets, at zero extra cost (no new AST node, no new check). The type-supported audit before landing this: I extended the checker experimentally to flag every discarded `Result` and ran it over the whole standard library and test suite, finding 17 real sites (six inside `lib/db/pg.zm`, `lib/db/mysql.zm`, `lib/http/server.zm` and `lib/http/client.zm` -- all genuine best-effort cleanup on an already-failing connection -- and the rest test-fixture setup/teardown, e.g. `fs.remove(path)` before a test creates its own file); each got `val _ = ...`. `try expr` already satisfies the rule by propagating instead of discarding.


## No cascades, sorted, always-named, source excerpt (2026-09, C7 part 1)

Four of the language review's diagnostics complaints (§5.2), fixed as one pass over `src/error.c` and two specific cascades in `typecheck.c`; a runtime error's stack trace (the fifth) is not part of this pass -- see docs/reviews/tasks.md's C7 entry.

- **Always the file name.** Text-mode output used to omit the file when it was the *entry* file (`ziman_set_error_file`'s old `in_other_file`/`show_file` gating), so two errors at the same position, one in the entry file and one in a `use`d module, printed differently depending on which file happened to be given on the command line. Now every error names its file whenever one is known (`show_file = g_error_file != NULL`); a bundled core-library module still has none, on purpose (see `ziman_set_error_file`'s own comment) -- it has no real file of its own, and typechecking it a second time inside a user's own program is not something the review's complaint was about. The now-dead `g_entry_file` global and `ziman_set_entry_file` were removed.
- **Sorted by position.** `ziman_error` used to print each error the moment it was raised, so a program whose functions are checked in one order (declaration order, generally matching source order) could still show its errors out of that order once more than one check fires on the same statement (`STMT_SWITCH`/`STMT_VALUE_BLOCK` both raising on one `switch`, e5's report). Every "syntax"/"type" error (a "runtime" error is always a one-shot, still printed immediately, unaffected) is instead collected into a small buffer (`CollectedError g_collected[MAX_ERRORS]`) as `ziman_error` raises it, still jumping to the next recovery point exactly as before; `ziman_flush_errors` (called once the errors from a whole checking pass are all in -- either a checking loop finished normally and called `ziman_finish_errors`, or `ziman_error` itself had nowhere further to jump) sorts them by `(file, line, col)` and only then prints them, in both text and `--json` mode.
- **Source excerpt and caret (text mode only).** `ziman_register_source(file, text)` is called once per real project file as `loader.c` reads it (`load_module_graph` for the entry file, `load_module` for each `use`d one); `ziman_flush_errors` looks the file back up and prints its reported line with a caret under the column, gutter-aligned the way rustc/clang do. A single caret, not a span: most call sites only ever had a start position to give `ziman_error`, and teaching every one of them an end column too is a separate, much larger change. A file `ziman_register_source` was never called for (a bundled core module) just prints without an excerpt, same as before. The table is a small fixed array (`MAX_REGISTERED_SOURCES`, 512): registering the same file twice (the language server re-analyzing after every edit) just adds another entry rather than replacing one, harmless because the LSP's own diagnostics path (`g_sink != NULL`) never reaches `ziman_flush_errors` or looks anything up here at all -- it streams each diagnostic to its sink immediately, unaffected by any of this.
- **Two cascades closed**, of the several the review found (this is a sampling, not an exhaustive sweep -- docs/reviews/tasks.md's C7 entry says so explicitly):
  - A `val`/`var` whose own initializer failed to check gets a poisoned placeholder binding (`poison_declaration`, pre-existing) so a later plain reference to it aborts quietly instead of raising a second, unrelated error. A *call* through such a name (`f()` after `val f = fn() { ... a bad capture ... }`) went through a different, earlier path in `check_call` (`e->as.call.local_name`) that checked the binding's type without ever checking whether it was poisoned first, so it fell through to `find_function` and reported "call to undefined function 'f'" -- true-sounding and nonsensical, since `f` is right there. Fixed by checking `fb->poisoned` before that path's own `TY_FUNC` check.
  - A function whose body contains a `switch` statement found non-exhaustive got a second, independent-sounding error, "function does not return a value on every path": true (the switch, left exactly as written since it never reached the point of being rewritten into an if/else chain, really is a path that reaches the end of the function without returning) but redundant, since the real problem is the switch. `check_function` now skips that check (`stmt_always_returns`) when checking the body's own statements already raised an error (an `errors_before_body`/`ziman_error_count()` comparison around the statement loop), which is also correct in the more general "this function's body had some other, unrelated failure" case.


## `fs.resolve` and `ZIMAN_RESOURCE_PATH`: a classpath, not a directory-naming convention (2026-09)

Grew out of restructuring `health-record-backend-ziman` onto a Maven-style layout (`src/main/ziman`, `src/main/resources`, `src/test/ziman`, `src/test/resources`). The first attempt made the application's own code path-aware -- `properties.load("src/main/resources/application")` -- which the project's owner rejected outright: Java code never knows `application.properties` came from `src/main/resources` rather than the classpath root, and Ziman code calling `properties.load("application")` should have exactly that same ignorance. A second attempt (launch the process with `src/main/resources` as its working directory) covered the plain-app case but broke down once tests needed *two* roots at once -- `src/test/resources` overriding `src/main/resources` by name -- since one process has exactly one cwd. That ruled out any solution built purely on directory conventions or launch tricks; what Java actually has here is a real search mechanism (the classpath), so Ziman needed one too, not a name for the problem.

`ZIMAN_RESOURCE_PATH` is that mechanism: a colon-separated list of directories, checked in order, first match wins -- `PATH` and the classpath's own shape, reused rather than invented. `fs.resolve(name) -> Option<string>` (`lib/core/fs.zm`) is the one new primitive: with the variable unset it returns `Some(name)` unchanged, so any program that never sets it is provably unaffected -- no behavior to regress, no flag to migrate. With it set, `None` on a hard miss rather than silently falling back to the bare name, so a missing resource fails where it's misconfigured instead of turning into a confusing downstream file-not-found. `properties.load` (`lib/core/properties.zm`) and `migrate.run` (`lib/db/migrate.zm`) both resolve their own file/directory argument through `fs.resolve` before touching the filesystem, rather than each growing its own search logic -- the same reason `read_text`/`exists`/`list_dir` etc. are the shared primitives everything else builds on. This is deliberately the entire feature: no separate "classpath" type, no caching, no directory-merge view -- one function, one env var, two call sites updated to use it, because that's all a classpath-shaped lookup actually requires once it exists as a real search rather than a naming convention. `health-record-backend-ziman` sets `ZIMAN_RESOURCE_PATH=src/main/resources` to launch the app and `ZIMAN_RESOURCE_PATH=src/test/resources:src/main/resources` to run its tests, and neither `config.zm` nor `application.properties` needed to change at all.


## `#[test]`, `ziman test`, and the `test` module (2026-09, D1)

The language review's D1 (a JUnit-style test framework): closes the roadmap's long-standing "project layout... and `ziman test` -- not started" item's test-runner half (the project-root-marker half is still open; see ROADMAP.md).

The assertions (`test.assert`, `assert_eq`, `assert_lt`, `assert_ok`, ...) needed **no new interpreter primitive at all** -- `lib/core/test.zm` is ordinary Ziman built entirely on mechanisms that already existed for other reasons: `unwrap` on a manufactured `Err` is already "the explicit, opt-in escape hatch to a fatal error" every program has (see "Result"'s own DESIGN.md-adjacent docs); the per-thread `ThreadBoundary` `spawn` already uses to keep one thread's runtime error from ending the process is exactly what isolates one failing test from the rest of a `ziman test` run; and recursive structural `Eq` (see "Recursive `Eq`/`Hash`" above) is what lets `assert_eq<T: Eq>` use a plain `!=` on almost anything. The one real discovery this surfaced: `str(x)` is deliberately narrower than it looks from the interpreter side (`value_display` genuinely is fully generic -- structs, enums, arrays, all of it) -- the type checker restricts `str`'s argument to a fixed scalar allowlist (int/float/bool/bigint/decimal/byte), checked before a generic function's own type parameters are resolved, so a bare `T` is rejected even when every type that could instantiate it would be fine. `format`'s `{}` placeholder has no such restriction (same display code, checked per call site instead of per declaration), so every failure message in `test.zm` uses `format("{}", x)` rather than `str(x)`.

`#[test]` needed the one real parser/typechecker addition: attributes before a top-level declaration previously only ever preceded `struct`/`enum` (`derive`, `thread_bound`); `test` is now a third recognized bare item, this time requiring a following `fn` instead (rejected before `struct`/`enum`/`native fn`/`val`, mirroring `thread_bound`'s existing rejection before `enum`). `#[test(cases: NAME)]`'s one argument breaks the existing `IDENT(IDENT, IDENT, ...)` attribute-argument shape (`derive(Json, Hash)`'s own grammar) -- it is `cases: IDENT` instead, parsed as a special case inside `parse_attributes` before falling through to the generic comma-of-idents path. `NAME` is resolved with a deliberately narrow rule: `env_lookup` on `g_global_env` (which, for the entry file `ziman test` always treats each argument as, only ever holds that file's own *unqualified* globals -- see `check_global_vals`'s own comment) or `find_function`, so cross-module `NAME`s are not just unsupported but structurally impossible to hit by accident, the same reasoning that gives a global val no `mod.NAME` dotted form at all.

The runner itself (`interpret_tests`, `src/interpreter.c`) is new: it walks a module's own functions for `is_test`, calls each inside a fresh `ThreadBoundary` (or once per array element, for `#[test(cases: ...)]`, the array itself either an already-evaluated global val or a zero-parameter function called once to build it -- also boundary-wrapped, so a broken case-builder is one reported failure rather than a crash), and a `Result` that comes back `Err` with no runtime error raised is treated the same as one. Deliberately **not** included: elapsed time in the PASS/FAIL lines, despite matching this project's other test runners' own style (`tests/run_tests.sh`, `.curly/run-scenarios.sh`) -- this project's own `tests/lang` harness checks stdout byte-for-byte, and a duration would make every fixture for this feature flaky. (The harness gained one small, backward-compatible extension for this feature's fixtures anyway: an optional `name.exit` file overrides a `name.out` fixture's expected exit code, default 0 -- needed because `ziman test`'s own successful, deterministic PASS/FAIL report can still exit 1.) `ziman test file.zm [file2.zm ...]` is a new subcommand (dispatched before the usual flag loop), not a `--flag`, deliberately mirroring `go test`/`cargo test` rather than this project's own otherwise all-flags CLI -- because the review phrased the feature that way and there is real precedent for a language's own test invocation being its odd one out. **Corrected 2026-10-29 (F227):** that dispatch tested `argv[1] == "test"` alone, so an option written before the subcommand -- `ziman --gc-stress test f.zm` -- read `test` as a second source file. The subcommand is now whichever argument is the first that is not an option, with the options before it handed to the mode along with the file names; `--api` and `--` stop that scan, since each already takes what follows it.

Known limitation, not fixed here: a `test.assert*` failure's reported position (`runtime error at L:C`) is always somewhere inside `test.zm`'s own `fail` function, not the caller's `test.assert_eq(...)` call site -- Ziman's runtime errors carry no call stack yet (tracked separately, see C7's own DESIGN.md entry, "a runtime error's stack trace... is not part of this pass"). The failure *message* (which values, what mismatched) is always correct and is what actually identifies the problem; only the position is uninformative for this one family of fatal errors, until stack frames exist.


## Shortest-round-trip float display, and a `math` module (2026-09, D2)

Language review D2 bundled five separable pieces (`str(float)`, `format` precision/escapes, float exponent literals, `format_dynamic` and a `math` module). This section covers the first and the `math` module; the rest are under "`format` precision and escapes, `format_dynamic`, and exponent literals" at the end of it.

**`str`/`print`/`format` were lossy where `json` already wasn't.** `value_display`'s `TY_FLOAT` case was a bare `snprintf(buf, "%g", ...)`: six significant digits, scientific notation past that, no attempt at round-tripping -- `print(1234567.891)` gave `1.23457e+06`, silently dropping digits a health-record backend's own log line or `format` call would want kept. `native_json.c`'s `put_float` already had the fix, just nowhere else: a precision search (`%.*e` at 0..16, first that reads back via `strtod` as the exact same double) has been JSON's own shortest-round-trip algorithm all along -- discovered by grepping for `isnan`/`isinf` while chasing this, not by reading the review's own citation table closely enough the first time. `number.float_to_text` (`native_number.c`) turned out to have an even more complete version of the *same* algorithm (also picking plain-vs-scientific notation, at a 1e-7/1e21 threshold) -- meaning the codebase had two independent, subtly different implementations of "shortest float text" already, which is exactly the "two textual forms in one program" bug the review's S7 complained about, just one level further down than S7's own examples showed. Fixed by extracting `float_to_text`'s algorithm into one shared `float_shortest_text(double) -> char*` in `value.c` (a lower-level file both `value_display` and `native_number.c` can call into), with `native_float_to_text` now a two-line wrapper around it -- `float_to_text`'s own documented contract (`10.0` displays as `"10"`, no forced decimal point) is preserved exactly, since it's still the same code.

`value_display` adds one thing on top that `float_to_text` deliberately doesn't: if the shared algorithm's result has no `.` or `e` in it (a whole-number magnitude), `value_display` appends `.0`. This is the fix for the review's other cited example, "`1.0` prints like an int" (S7's own words) -- `float_to_text` is meant for exact numeric round-tripping (a protocol value, a stored figure), where matching an int's own text for a whole number is fine and arguably correct; `str`/`print`/`format` are meant for a human or a log line to read, where a `float` that's indistinguishable from an `int` is the actual bug. Same underlying digits, two different "is this whole-number case special" answers layered on top, at the one point they actually diverge.

Twelve existing fixtures needed regenerating (`.out` files whose expected stdout was the *old*, lossy behavior) -- `bigint_casts`, `enum_basic`, `int_literal_limits`, `json_map_objects`, and others, all a one-line `42` -> `42.0`-shaped diff, checked by hand against each fixture's own source before regenerating to confirm every diff was this fix and nothing else (`string_views` briefly looked like a real regression -- `Option.unwrap_Some` on `None` -- until re-running with its `.env` sidecar actually sourced, the same way `tests/run_tests.sh` does, showed the only real diff was the float line after all).

**A `math` module** (S8): `sqrt`, `pow`, `exp`, `log` (natural), `sin`/`cos`/`tan`, `is_nan`, `pi`/`e`, and float-preserving `floor`/`ceil`/`round` (the bare builtins of the same name convert to `int`) -- one-line wrappers over libm (`lib/core/math.zm` + `src/native_math.c`, registered the same `"math::sqrt"`-keyed way every other native module is). A domain error (`sqrt(-1.0)`, `log(0.0)`) is NaN, matching C/Java/JS, not a `Result` -- Ziman's `compare<T: Ord>` already gives NaN a total order, so it was already an ordinary, well-supported float value before this module existed, not a new kind of failure to wrap. `is_nan` needed no native code at all (`x != x`, the IEEE-754 definition, directly). One thing this surfaced that had nothing to do with `math` itself: Ziman's `float / float` by zero is a *runtime error*, not an IEEE-754 infinity (checked the same way `int` division by zero is) -- an early draft of this module's own comments claimed `1.0 / 0.0` was a source of NaN/infinity in Ziman, which is simply false; fixed before landing, but worth recording since it's an easy assumption to import from any other language's floats.

**`decimal_pow`/`decimal_floor`/`decimal_ceil`** (added on request, not part of the review's own D2 line): unlike `math`'s transcendental functions, a non-negative integer power and a directional rounding are *not* irrational, so these are exact -- pure `decimal`/`BigNum` arithmetic, no `float` involved at all, and registered as bare builtins (`decimal_pow`, `decimal_floor`, `decimal_ceil` in `lib/prelude.zm` + `src/prelude.c`) alongside the existing `decimal_round`, not inside `math` -- `math` is specifically the *floating-point* module; `decimal`'s own arithmetic helpers were already bare prelude builtins before this, and these follow that precedent instead of inventing a second home for decimal math. `decimal_pow(base, exponent)` computes by squaring (`O(log exponent)` multiplications, reusing `decimal_mul` directly rather than touching `BigNum` by hand) and rejects a negative `exponent` with a runtime error explaining the composition that does work -- `numerator / decimal_pow(base, -exponent)` -- since a negative power needs a division, and `decimal_div` already has its own well-defined target-scale rule (`max` of its two operands' scales) that `decimal_pow` has no business overriding. `decimal_floor`/`decimal_ceil` are `decimal_round`'s two directional cousins: same "scaling up is exact, scaling down needs `BigNum` division" shape, just correcting the truncated-toward-zero quotient toward the more negative (floor) or more positive (ceil) neighbor instead of rounding to nearest.


### `format` precision and escapes, `format_dynamic`, and exponent literals

**One template scanner.** `format`'s placeholder handling lived in two places that had to agree: `count_placeholders` in typecheck.c (which counted `{}` at compile time) and `call_format` in interpreter.c (which filled them). Both now use `fmt_next` in `src/fmtstr.c`, which splits a template into pieces -- a run of literal text, a `{{` or `}}` (a literal one-character piece), a `{}` placeholder or a `{:.N}` placeholder with its precision -- and reports a malformed one with a message. `format_dynamic` uses the same scanner, so the three cannot disagree about what a template means.

**Only `{:.N}`, not the whole C/Rust mini-language.** The one thing money-style output needs that `decimal`/`number.format_*` cannot do inline is a fixed number of decimal places, so that is all that was added: no width, alignment, fill, sign or radix. Each of those is a place for the template to grow another grammar, and `number.format_*` already covers padding and grouping. The precision must be digits followed by `}` (`{:.}` and `{:.x}` are errors) and at most 100, so a template cannot ask for a megabyte of zeros. It is a compile error on anything but a `float` or a `decimal` -- silently ignoring `{:.2}` on an `int` would hide a mistake.

**Rounding.** A decimal goes through `decimal_round` (half away from zero, exact); a float goes through `snprintf("%.*f")`, so it rounds the *binary* value: `{:.2}` of `1.005` prints `1.00`, and `{:.1}` of `2.25` prints `2.2` (an exact tie, rounded to even by the C library). That is what every C-family language does with a float; a program that needs decimal rounding of a price should hold a `decimal`, which is the reason the type exists. A tiny negative value can print `-0.0`, again as C does.

**`{{` and `}}`.** Doubled braces, as in Rust and C#. A lone `}` is an error (Rust's rule) rather than a literal, because a stray one is far more often a typo than intent, and the error says how to write a literal.

**`format_dynamic(template, args)`.** `format` requires a string-literal template so its placeholder count can be checked against its arguments before the program runs. A message catalog looks the template up at run time, so this is a separate function: `[string]` arguments (the template is the only thing that is dynamic, and a catalog's arguments are already text) and a `Result<string, string>`, since a wrong template is data, not a bug in the program. A count mismatch, a malformed template and a `{:.N}` (nothing to round in a string) are each an `Err`. Indexed placeholders (`{0}`) for reordering are the obvious next step and are noted in ROADMAP.md.

*(2026-10-22: deleted. The message catalog this was built for never arrived — F44 had already found no catalogue in this library or the backend, `time.zm` with no locale support at all, and `{0}`-style placeholders in 0 of 30 cold benchmark attempts — and the function had no caller anywhere but its own fixture. It is the one deletion in this file's history that removed a capability rather than a redundant spelling: `format` cannot take a computed template and nothing else can either, so a runtime template is now scanned by hand. Kept in `renamed_to_syntax` so the name answers with that rather than with "undefined". `fmt_next` stays, shared by `format` and `printf`, which is two agreeing instead of three.)*

**Exponent literals.** `lex_number` reads `e`/`E`, an optional sign and digits after the mantissa, but only when a digit follows, so `1e` and `1else` lex exactly as before. An exponent makes the literal a `float` (`1e3` is `1000.0`, and the parser's `strtod` already understood the text). `1e3d` is an error naming the reason instead of lexing as a float followed by the name `d`: a decimal literal has its own digits (`15000d`), and an exponent on one would need a scale rule the language does not have.

## Closing channels, timed receive and `select2` (2026-09, D3)

Language review D3 (P1-10, §3.3): the concurrency model had no way to say "the producer is done" (a sentinel value in the stream was the only option), no timed receive and no select. `Channel<T>` is a hardcoded parametric type whose operations are hardcoded builtins (`send`, `receive`, ... are special-cased by name in `typecheck.c`'s `check_call` and dispatched in `interpreter.c`, not declared functions), so each addition is a branch in both places plus the three name lists (the no-named-args list, the cannot-redefine list, the language server's completions).

**Close.** A `closed` flag on `ChannelObj`; `channel_close` sets it and *broadcasts* both condition variables so every blocked sender and receiver re-checks. The semantics follow Go's, adapted to no panics: values already queued are still delivered (close only stops new ones), `send`/`try_send` on a closed channel is a runtime error, and closing twice is harmless (Go panics; there is nothing to gain from replicating that). `channel_send` and `channel_receive` stopped returning `void`/`Value` and now return a status the interpreter turns into an error at the call site -- the same place `channel_new`'s capacity check already raises, since `value.c`'s pure functions have no source position to report. `channel_try_send` grew a three-way status (sent / full / closed, `channel_try_send_status`), because `try_send`'s `false` legitimately means "full" and must stay distinct from the closed error. Plain `receive` on a closed, empty channel is also an error rather than a silent hang, which turns one case of the review's "deadlocks are silent" complaint into a debuggable failure. Code that expects a close uses the new `receive_option`, which blocks the same way but returns `None` once closed and drained -- both `receive` and `receive_option` run the same `channel_receive` C function, differing only in what the interpreter does with its "closed and empty" result, and `receive`'s signature and behavior for a channel that is never closed are unchanged, so nothing existing breaks.

**Timed receive.** `receive_timeout(chan, millis) -> Option<T>` is `channel_receive` with `pthread_cond_timedwait` against an absolute `CLOCK_REALTIME` deadline, looping over spurious wakeups. A timeout and a closed-and-empty channel both give `None`; they are not distinguished, the same blur a `select` with a timeout case has in practice, and one fewer thing to name.

**`select2`, and why polling.** Scoped to exactly two channels (the review's own phrasing), not a general N-way `select`. The channels can have different element types, so the result is a two-variant enum over two independent type parameters, `Selected<A, B> { First(A) Second(B) }`, declared in the prelude like `Option` (generic enums already give it `is_First`/`unwrap_First` and so on for free). It is named `Selected`, not `Either`: the first version used `Either`, and `derive_json_generic`/`derive_ord_generic` -- existing fixtures that declare their own `Either<L, R>` -- immediately failed with "enum 'Either' is already defined". Any name the prelude adds is bare in every program and can collide with a user's declaration, and `Either` is exactly the sort of name people write themselves; a specific name costs nothing. The implementation is the real tradeoff: Ziman's channels are userspace mutex + condvar with no file descriptor or scheduler hook to block on "either of two", so `channel_select2` polls both with `channel_try_receive` and sleeps 1 ms between rounds. That is correct and simple, but it is not a true zero-latency select -- it can add up to the poll interval before noticing a value -- and, like `receive`, it waits forever if both channels stay empty (now including both being closed). A real select would need each channel to carry a list of external waiters that `send` also signals, a much more invasive change to `ChannelObj` that nothing yet needs; it stays a possible follow-up if the polling latency ever matters.

### Moving the rounding functions and `decimal_pow` into `math` (2026-09, D2 follow-up)

At the user's request `floor`/`ceil`/`round` (to `int`), `decimal_round` and the `decimal_floor`/`decimal_ceil`/`decimal_pow` added a day earlier moved out of the prelude into `math`, so all of Ziman's numeric functions beyond the operators live in one place and the prelude keeps to strings, parsing and builders. This supersedes the D2 entry above, which described the decimal functions as bare prelude builtins alongside `decimal_round`. Consequences worth recording: (1) it is a breaking change -- bare `floor(x)` no longer resolves, a program needs `use math` and `math.floor(x)` -- but the blast radius was four fixtures, one docs example and no backend code, so it was cheap; (2) the float-preserving roundings I had already put in `math` under the names `floor`/`ceil`/`round` collided with the int-returning ones that now joined them, so they were renamed `floor_float`/`ceil_float`/`round_float` and the int-returning ones took the plain names, matching what the bare builtins were called; (3) the natives moved from `src/prelude.c` (registered bare) to `src/native_math.c` (`"math::floor"` and so on), and a type error now names the function as `'math::floor'`.


## The `thread` module and `ThreadLocal<T>` (2026-09)

Asked for as "a thread module with utility functions, and an editable cache per thread". Ziman had no thread identity at all -- only `__thread` variables for the stack limit and the error boundary -- so `src/native_thread.c` keeps a small record per thread (id, name, slots). A spawned thread gets its record in `spawn_thread_entry` (`ziman_thread_begin`, named `<function>-<id>`) and drops it at the end (`ziman_thread_end`); the main thread's is made the first time it is asked for. The records hang off a global list, a garbage-collector root, rather than only off a `__thread` pointer, so the values in a thread's slots stay reachable however the collector treats thread-local storage; a thread's own slots are touched by that thread only, so only the list needs the lock.

**The cache is `ThreadLocal<T>`, not a string-keyed bag.** Ziman has no `Any`, so `put("key", anything)` cannot be typed; a key that carries `T` can. `ThreadLocal<T>` is an ordinary generic struct in `thread.zm` -- a number naming the slot and the `fn() -> T` that makes a thread's first value -- with `get`/`set`/`has`/`reset` written in Ziman over five small untyped natives (`slot_get<T>` and friends), so there is no new type kind and no interpreter dispatch. "Editable" means `set` with a new value, since every value is immutable; `push` being amortised O(1) keeps an array slot cheap, and a `MapBuilder` in the slot is the efficient mutable cache.

Two things needed a change outside the module. (1) A module-level `val` may not hold a `#[thread_bound]` type, and `ThreadLocal<MapBuilder<..>>` mentions one in its type argument, so a new struct attribute `#[thread_key]` says "this type holds nothing that belongs to a thread": `type_holds_thread_bound` stops at it. It is a struct-only attribute, rejected on an enum, and a function, like `#[thread_bound]`. (2) The interpreter overwrote the result type of every generic native call with the call's `resolved_type` (to tag things like an empty `ArrayBuilder<T>`), but generic bodies run unspecialised, so inside `get<T>` that type is still the placeholder `T` and the first `thread.get` died with "a runtime value should never have an unsubstituted type variable". A native whose return type is a bare `T` returns a Value it was handed earlier, which already carries its true type, so the retag is now skipped when `resolved_type` is a bare type variable.

The limit that matters: a thread-local dies with its thread, and `server.serve` uses a thread per connection, so this is per-connection state (request id, current user, scratch space), not a cache across requests. That needs a different mechanism (an actor thread owning the map behind a channel, which already works, or a lock-guarded shared handle, which would be a separate feature). Slots are found by linear search: a thread has a handful of keys, and a real cache lives in a builder in one slot.


## HTTP server hardening (2026-09, D4)

Language review D4 (developer review B7, B8 and action item 13): what a proxy in front of the server might read differently from it, response headers written without a look, and a client that takes forever to send a request.

**Smuggling.** `read_request` used the first `Content-Length` (`header_value`), so `5` then `N` truncated the body at 5 and treated the rest as the next request; `+N` parsed; HTTP/1.1 needed no `Host`; a bare LF inside a header line was kept in the value. Now: differing `Content-Length` values are a 400 (identical repeats are allowed, RFC 7230 lets a recipient merge them), the value must not start with `+` or `-` and must parse (so `5, 5`, `0x5`, empty and overflow are refused too -- `parse_int` alone accepts `+5`, which is D7's separate complaint), a second `Transfer-Encoding` or `Host` is a 400, HTTP/1.1 without `Host` is a 400 (1.0 is exempt), and a header name must be a token and a header value or request line free of control characters (a tab is allowed). Since header lines are already split on CRLF, "bare LF or CR" needs no separate scan: the leftover `\n` or `\r` in a line is a control character, which `is_field_value` refuses. The check is two natives, `http.is_token` and `http.is_field_value` (`src/native_http.c`), not Ziman loops: a per-character loop over every header of every request would cost more than the request's own handling, and the D5 item is about 25 microseconds per endpoint. They are `pub`, so an application can validate a value before it builds a header. Ziman strings have no `\u` escape, which is why the fixture builds a control character with `bytes_to_string([1b])`.

**Response headers.** `with_header` returns a `Response`, and changing it to a `Result` would break every caller; a `Response` is also a public struct anyone can build directly. So the check is where every response passes, `write_response`: a header whose name is not a token or whose value has a control character is refused, the response is replaced by a 500, the connection closed, and stderr gets a fixed line that does not include the offending text (it would be a log-injection route of its own). Failing closed, not stripping the characters, so an attack is visible rather than silently defanged.

**Slowloris.** `net.set_timeout` bounds each read, so a client sending a byte just inside it, again and again, held a connection thread forever. `Session` gained `timeout_ms` and `open_session_with_timeout(sock, ms)`: the whole request, head and body, must arrive within `ms` of its first byte. The first read of a request (an idle keep-alive wait) is still bounded by the socket's own timeout, and the clock starts when data arrives, or at once if a pipelined request is already buffered; every later read lowers the socket's timeout to what is left (`read_within`), and once the request is complete the timeout is put back (`restore_timeout`) so the response write and the wait for the next request get the full limit again. I deliberately did not add a `header_timeout_ms` to `ServeOptions`: it would break every existing `ServeOptions { ... }` literal (the backend, six fixtures, the docs examples), so `serve` reuses `idle_timeout_ms` for both jobs -- one number for how long the server waits on a client -- and a separate knob stays a possible follow-up. The body loop replaced `net.read_exact` (which restarts its timeout on every read and so has the same hole) with reads through `read_within`. The clock is a new `clock.monotonic_millis()` (`CLOCK_MONOTONIC`); the wall clock `now_raw` can jump and is documented as not for measuring elapsed time.

Verified with `http_server_hardening`, and by mutation: removing the `restore_timeout` call made its last check fail (408 where 200 is expected), which it did not until the head was sent in three pieces -- with two, the timeout lowered on the second read was still the full limit, since the second read is issued right after the first byte arrived. `http_server` itself needed `Host` on 17 of its requests, since it deliberately sent HTTP/1.1 requests without one.


## Compiled route tables (2026-09, D5)

Developer review item 14: `server.dispatch` scanned the whole endpoint list per request and re-split every pattern string on every attempt. Measured on the health-record backend's 117 endpoints before the change: `/health` 0.45 ms, an unknown path 3.9 ms, a wrong-method request 3.8 ms, `GET /api/me/allergies` 2.2 ms, `GET /api/patients/me` 2.5 ms. The source of the miss cost is two passes, not one: the scan itself (every endpoint of the request's method ran `path_params`, which trims a slash and runs `split` on the pattern *and* on the path), then, to decide between 404 and 405, `describe` built a `"GET /path"` string for every endpoint with `format` and `not_found_or_method_not_allowed` split each string again and ran `path_params` against all 117 patterns whatever their method.

The fix is `server.routes(endpoints) -> Routes<C>`, compiled once (the backend's table is already a module-level `val`, so it is built at start-up), and `dispatch` taking that. A `Routes` holds the endpoints, each pattern split into a `Pattern` (segment text, or the name for a `{param}`, plus which is which), and two maps of endpoint indexes in table order: by method and segment count (`"GET 3"`) and by segment count alone. A request splits its path once, looks up the `"METHOD count"` list and tests only those endpoints -- with `shape_matches`, which allocates nothing, and `capture`, which decodes only on a hit. A miss walks the by-count list and collects the methods of the paths that match, with no description strings. `path_params` itself became `capture(compile_pattern(pattern), segments_of(path))`, so a hand-written router (`route`, `path_params`) and `dispatch` share one implementation of the matching rules and cannot drift apart.

**Why buckets by method and length, not the review's "method and first segment".** Nearly every backend route starts with `/api`, so a first-segment index narrows almost nothing there; segment count does, and it keeps the semantics that matter: the first endpoint in table order still wins, so `/items/{id}` listed before `/items/new` still answers for `new`. A trie that prefers literals would have been faster still but silently changes that rule. The method is also re-checked with `==` after the bucket lookup, because two `HttpMethod` values can print the same (`Other("GET")` and `Get`) and the old code compared the values.

**Verified against the old code, not just the new tests.** `http_dispatch_routes` (17 requests: the root, trailing slashes, order, percent-decoding, empty segments, bad encoding, 404 vs 405 with `Allow` lists in table order, `HEAD`) was run through the previous commit's build with the old `dispatch` on an array, and the two outputs are byte-identical. After the change, on a quiet machine: `/health` 0.4 ms, unknown path 0.63 ms, wrong method 0.7 ms, `GET /api/me/allergies` 1.9 ms, `GET /api/patients/me` 2.3 ms. The matched deep routes gained only about 0.3 ms because the rest is the handler (authentication, the database), not routing; the miss path, the one that was mostly routing, dropped by a factor of six.

The signature change is breaking: `dispatch`'s first parameter is now a `Routes<C>`, so the backend (three call sites, one new `val table = server.routes<Ctx>(endpoints)`) and two fixtures were updated. The other `Endpoint` builders (D6) are untouched.


## Endpoint builders: one core, a shared `Api`, an optional `status` (2026-09, D6)

Language review P1-11 asked to collapse the eight (there are nine: five `secure_*`, four `public_*`) endpoint builders into one function taking optional named parameters, `path:`, `body:`, `who:`. That cannot be written in Ziman: `Option<T>` lets a *value* be omitted, but `run`'s own parameter list -- `(ctx, who)`, `(ctx, who, path)`, `(ctx, who, path, input)`, ... -- and the type parameters behind it differ with which parts an endpoint has, and an omitted `who` leaves its type parameter `U` with nothing to infer from (there are no default type parameters). The alternative, one fixed `run(ctx, who, path, query, input)` with placeholders for absent parts, would make the common plain `GET` handler, most of the backend, longer than it is now. So the API keeps its nine typed builders and what changed is around them.

**One private core.** The five `secure_*` bodies repeated the same steps (identify, media type, negotiate, decode, answer), 30 lines each. `endpoint_core` does them once, in the one order every variant already used: identify the caller (401/403/500), check the body's media type (415) if there is a body, negotiate the answer (406), read the path input (400), read the query input (400), decode the body (400), run. No variant has both a query and a body, and the path always came before the body, so one order reproduces all five. The absent parts are `unit`: `no_path`/`no_query` produce `unit` from nothing, `decoders: None` means no body is read and `blank: Some(unit)` is what `run` gets instead (with a body, `decoders` is `Some` and `blank` `None` -- a `blank: I` could not be written for a real `I`), and `run` is the builder's own `run` wrapped to ignore them. Each of the nine public builders is now 15-20 lines that build that wrapper; the `public_*` ones still delegate to the `secure_*` with `anyone`, one shared `who`. Adding an axis (headers, cookies) is one core parameter and one adapter per builder, not a doubling.

**`Api<E>`.** `failure:` and `fault:` were the same two functions on every one of the backend's 118 calls. `server.Api<E> { failure, fault }` is written once (`val api = server.Api<ApiError> { failure: failure, fault: fault }`) and passed as `api:`, which also brings `E` in through one argument instead of two. `dispatch`'s own `fault` parameter is separate and unchanged: it needs no `E`.

**`status` is optional.** `status: Option<int>`, 200 when the named call omits it (`option_unwrap_or<int>(status, 200)` in each adapter); a bare `status: 201` auto-wraps as for any `Option` parameter. 79 of the backend's 118 calls said `status: 200`. A positional call cannot omit it (omission is only for named calls), which is why the parameter's position among the others did not change.

**The explicit type arguments were already unnecessary.** Every backend call spelled out `<Ctx, Principal, Uuid, MedicationRequest, MedicationResponse, ApiError>`. Before changing anything I stripped that list from all 118 sites in a copy and it type-checked, since inference from the arguments (C3, part 1) covers it; the real change strips them for good.

Net effect on the backend's `routes.zm`: 433 lines removed and 240 added (about 190 fewer), each endpoint with two fewer lines for `failure`/`fault`, none for a default `status`, and no type-argument list. Behaviour is unchanged: `http_endpoints` (nine endpoints across every builder, the media-type, negotiation, path, body and denial cases, with the 200 default in eight and an explicit `status: 200` in one) and `http_query_endpoint` pass with their expected output untouched, as do the backend's suites and curly scenarios, and a request through a matched route costs the same to within the noise of the benchmark (1.9 ms for `GET /api/me/allergies` before and after) -- the extra wrapper closure per request is not measurable next to authentication and the database. **Breaking:** every builder call changes (`failure`/`fault` become `api`, `status` becomes optional), so the backend, two fixtures and the docs were updated; there is no compatibility shim.


## JSON numbers: `JNumber(Number)`, bigint and decimal, and a strict `parse_int` (2026-09, D7)

Developer review B14 named three things: `parse_json("01")` was accepted, `parse_int(" 5")` and `"+5"` were accepted against the docs, and JSON integers past 64 bits became floats.

**Leading zeros.** JSON's grammar is `0` or a digit 1-9 and more digits; `parse_number` scanned digits and let `strtoll` read `01` as 1. It is now an error at the number's position. The rest of the grammar (`+1`, `.5`, `5.`, a dangling exponent) was already refused.

**`parse_int` and `parse_float`.** Both handed the text to `strtoll`/`strtod`, which skip leading whitespace, take a `+`, and (`strtod`) read hex floats, `infinity` and `nan(...)`. `parse_bigint` and `parse_decimal` have their own parsers and already refused `+5` and blanks, so the four disagreed. Now all four agree: an optional `-` and digits, for a float also a point and an exponent, plus exactly `nan`, `inf` and `-inf` (the spellings `str` prints, which existing fixtures parse). The docs already said junk is an Err; `prelude.zm`'s own comment said "surrounding blanks allowed" and "an optional leading `+`", which was wrong on both counts and is corrected. The rest of the library was unaffected: the TOML and YAML readers strip a `+` themselves before calling `parse_int`, and the backend's suites pass unchanged.

**Big integers, following Jackson.** An integer literal too big for `int` used to fall through to `strtod` and become a `JNumber(float)`, which silently changes its value past 2^53 (and documented, deliberately, in SYNTAX.md). Jackson's model is the useful one: type a number by its shape (`int`, then `long`, then `BigInteger`), never coerce an integer literal to a double, and let the *target type* decide whether losing precision is acceptable (a `long` field errors out of range, a `BigInteger` is exact, a `double` field converts). Ziman has `bigint` and `decimal`, so the same works: the number is a `Number`, and each reader states what it accepts. I considered rejecting a lossy integer outright, or accepting one only if it is what a double prints (a JS producer's `123456789012345680000`); an exact representation makes both unnecessary, since `float_from_json` converts it on request and nothing is decided by the parser.

**`Number`, one variant, not four.** First sketched as two more `Json` variants next to `JInt` and `JNumber`; the user proposed `JNumber(Number)` with `enum Number { Int Float Bigint Decimal }`, which is one numeric node with a type, as Jackson has, and is the cleaner model: a future numeric kind touches `Number`, not `Json`, and a converter written as an exhaustive `switch` on `Number` is checked for every kind, where the old `is_JInt`/`is_JNumber` chains silently ignored a kind they did not know (the `csv`, `toml` and `yaml` writers were exactly that). The cost is that Ziman has no nested patterns (`if JNumber(Int(n)) = v` is a syntax error), so the one-line `if JInt(n) = found` in `jwt.zm` and the backend's `token.zm` became either a call to a reader (`if Ok(n) = int_from_json(found)`, which also accepts a whole float) or a `switch` over `json_number(v)`. The migration touched `json`, `ndjson` (its native takes a `Number` sample too), `csv`, `toml`, `yaml`, `jwt`, `derive.c` (a derived `int` field now writes `int_to_json(x)`, and `bigint` fields are supported for the first time), `native_json.c`, six fixtures, the backend's three files and the docs. `Number` lives in `json`, so a user's own `Number` is `Number` in their module and cannot collide (unlike the prelude name that made `Either` a problem for D3).

**`Decimal`, and `parse_json_exact`.** `decimal_to_json` wrote the nearest float, so a decimal with more than about 15 significant digits lost the rest and `19.90` was written `19.9`. It builds `Number.Decimal` now, written at its own scale (a negative scale, the zeros it stands for, is written out). The default parse still gives `Float` for a fraction or exponent, because `csv`, `toml`, `yaml` and `jwt` and user code read those; `parse_json_exact` is Jackson's `USE_BIG_DECIMAL_FOR_FLOATS`, off by default: a fraction or exponent is a `Decimal` with every digit and the scale kept (`1.50` stays `1.50`, `1.5e3` is 1500, `0.5e2` is 50). A number is bounded, because an exact `1e999999999` would be memory the sender chooses: at most 1000 digits for a bigint or exact decimal (turning n digits into a bigint takes time quadratic in n; Jackson's `StreamReadConstraints` default is also 1000) and an exponent of at most 100000 in size.

Verified with `json_number_kinds` (every kind from the default and the exact parse, the grammar errors, the readers on each kind, the limits, writing), `parse_number_strictness` (the whole matrix across all four parsers), and `derive_json_bigint_decimal`; the existing JSON fixtures needed only their expected output regenerated where a `Json` value is printed (`JNumber(Int(3))` for `JInt(3)`) and `1e+20` for the 20-digit integer that is now `99999999999999999999`.


## JWT: a required `exp`, `iss` and `aud` (2026-09, D8)

Developer review S9: a token without a valid numeric `exp` never expired (`seconds_claim` returned "no claim" for a missing `exp` *and* for one that was not a number, and `verify` skipped the check), and `iss`/`aud` were not checked at all. Both are the kind of gap that stays invisible until two services share a key: a token issued for one is then accepted by the other, and a token nobody thought to expire is valid forever.

**`exp` is required by default.** The RFC makes `exp` optional, and most libraries do not require it; but a library whose own docs say "a token without `exp` never expires" has a foot-gun, and here every token the library issues (`claims`) has one, so requiring it costs the callers who use the library as designed nothing (the backend's suites pass unchanged). `verify`, `verify_leeway` and `verify_at` now refuse a token with no `exp` (`MissingClaim("exp")`); a token meant to be open-ended has to say so with `verify_claims(require_exp: false)`. This is a behaviour change to the existing functions, made deliberately rather than as a new opt-in flag nobody would set. `seconds_claim` now returns a `Result`: an `exp` or `nbf` that is present and not a number is `Malformed("the 'exp' claim is not a number")`, whatever `require_exp` says, so `"exp": "never"` cannot pass as open-ended.

**`iss` and `aud` are opt-in, because only the service knows what it expects.** `verify_claims(token:, key:, ..., issuer:, audience:)` takes them as named `Option` parameters (Ziman's omission rule, so `verify_claims(token: t, key: k, audience: "web")` says only what it means); there is no default to guess. A given issuer must equal the token's `iss` exactly; a given audience must equal the token's `aud` when that is a string or be one of its strings when it is an array (RFC 7519 allows either), and non-string array elements are ignored. A token that lacks the claim entirely is `MissingClaim("iss")`/`MissingClaim("aud")`, distinct from `WrongIssuer`/`WrongAudience` (the claim is there and names someone else), so a log line says which. The signature is checked first, then `exp`/`nbf`, then `iss`, then `aud`. Adding three variants to `JwtError` cannot break an exhaustive `switch` in this repo (the backend matches `Err(_)`); `jwt_error_message` covers them. New readers `jwt.issuer(claims)` and `jwt.audience(claims)` (always a list) mirror `subject`.

The existing positional functions are thin over one private `check` with all six inputs, so there is one implementation of the rules. `jwt_claims` covers each rule (a missing `exp` with and without `require_exp`, an `exp`/`nbf` that is a string, leeway, `iss` present/absent/wrong/not a string, `aud` as a string and as an array with a non-string in it, and the signature failing before any claim is looked at). The backend does not yet set `iss`/`aud` on the tokens it issues or verify them: it inherits the required `exp` and nothing else, and adding an issuer and audience to `token.zm` and `security.zm` is a backend change left for later.


## Two collector bugs a concurrency test found (2026-09)

Eight simultaneous requests that each ran a transaction (the backend's insurance rule, E4) made the sample backend hang on about one run in fifteen and, over a few hundred rounds, crash with a segmentation fault. Both were latent in the runtime; the workload only made them likely.

**A thread waiting for itself in `handleset_add`.** The set of live sockets, connections and iterators grows by allocating a bigger table, and it did that with its lock held. Under the collector an allocation can start a collection, and the collector runs finalizers on the thread that allocated: `net_finalizer` takes its dead socket out of the set, which takes the same, non-recursive lock. The thread held the lock and waited for it; every other thread that then touched a socket queued behind it, and the server answered nothing. `handleset_add` now allocates the new table with the lock released and re-checks that the set is as it was before installing it (a lost race just discards the table and looks again). `tests/c/handles_test.c` (`make test-handles`) wraps `malloc`/`calloc` so that an allocation does what the collector does -- run a finalizer that removes a handle -- and fails, by alarm, if the lock is held across it. A stress fixture over sockets could not reproduce the hang reliably; a collection has to start inside that one allocation, with a dead socket waiting to be finalized.

**The list of scopes with a pending `defer`.** Each thread keeps the scopes that have deferred calls pending in a `__thread` array, so a runtime error that ends the thread can still run them (`t_defer_envs`). The array was allocated with the collector's `realloc` and referenced only from that thread-local pointer, and the collector does not reliably scan thread-local storage (`native_thread.c` already says as much of its own records). A collection freed the array under a running thread; the next `defer` or block exit wrote into memory that had become something else, and in the crash under gdb that was an `Env` whose `count` field now held half a pointer. It is now allocated uncollectable -- a root the collector scans, which also keeps the tracked scopes alive -- and given back when the thread ends. `tests/lang/spawn_defer_gc_stress.zm` runs eight threads that `defer` while allocating heavily; the unfixed interpreter dumped core on all three tries, the fixed one prints its result in under two seconds.

What made these hard to see is that the non-collecting build never shows either, so `make test-nogc` cannot catch this class; the fixture and the unit test are the only guard, and a new thread-local pointer to collected memory should be allocated the way the defer list now is.


## Slow clients, a hardening header and no banner (2026-09, E7)

**A second, shorter timeout for the head.** `serve` used one number, `idle_timeout_ms`, for how long a client may take over anything, so 64 sockets that said nothing (or a byte per 29 seconds) each held a slot for the whole 30 s, and an attacker who reconnected as they closed kept every slot full. The earlier slowloris fix (above) deliberately reused that one number to avoid breaking every `ServeOptions { ... }` literal; this adds the second knob anyway, and updates the literals (the backend, the fixtures, the docs examples), because the number that suits a keep-alive wait or a large upload is far longer than the one that suits a request head. `ServeOptions.header_timeout_ms` bounds a new connection's first byte and, once a request has begun, the time to the blank line that ends its head; the wait between requests of a keep-alive connection, and a body, keep `idle_timeout_ms`. It is done in `read_request` with two more `Session` fields: `head_timeout_ms`, and `fresh` (no request read on this connection yet), which makes the socket's own timeout `head_timeout_ms` for the first read and puts it back to `idle_timeout_ms` once the first request is in. The head loop reads within the smaller of the two limits; the body reads keep using the whole-request one, measured from the same first byte. `open_session_with_timeouts` is the new constructor; `open_session_with_timeout` keeps its signature and its meaning.

**What it does not do.** It cannot tell one client from another, so an attacker who opens enough connections still occupies the slots, only for seconds instead of half a minute. The standard answer, a cap per client address, needs the peer address of a socket (`net` has none) and a counter shared by every connection thread (a native handle, as `StringBuilder` is), and behind a reverse proxy every connection has the proxy's address, so it would have to default to off. It is noted in the roadmap rather than done.

**`X-Content-Type-Options: nosniff` on every response, no `Server` header, no HSTS in the library.** `write_response` adds `Date` when the handler did not set it; it now adds `X-Content-Type-Options` the same way (a handler that sets its own is not overridden or repeated) and no longer adds `Server: ziman`, which tells an attacker what to look for and had no use. `Strict-Transport-Security` is not the library's to send: it does not know whether TLS ends in the process (`net.listen_tls`) or at a proxy, and HSTS over plain HTTP is ignored by browsers anyway. The sample backend adds it in its router from `server.hsts-max-age-seconds` (one year; 0 turns it off), on every answer that goes through the router.


## Log rotation, fixed-width timestamps in SQL, and a loader bug in generic type arguments (2026-09, E11)

**`log` rotation.** A server's log file grew for ever and the sample backend's admin page read all of it into memory per request. `log.logger` takes `max_bytes` and `keep_files` (both `Option`, so every existing named call is untouched; without `max_bytes` nothing changes). Rotation is in `write` itself, in Ziman: `fs.size` before each append, and once the file has reached the limit the files are shifted (`.N` deleted, `.n` renamed to `.n+1`, the file to `.1`). Two threads that both see a full file must not both rotate it, so the shift takes a lock -- a `Channel<int>` holding one token, made once as a module-level `val` -- and the size is checked again once it is held. An append that was already under way lands in the renamed file, not in a lost one; `tests/lang/log_rotation.zm` has four threads write 1000 lines through 500 rotations and counts them all. A line costs one `stat`. The alternative, a native that appends and rotates under a C mutex, would avoid the stat when rotation is off, but the check is skipped then already (`max_bytes` 0 never calls it).

**`instant_to_sql` writes the nine-digit fraction always.** `instant_to_string` leaves the fraction off for a whole second, which is fine for display and JSON, but the value goes into a TEXT column and is sorted as text, where `Z` sorts after `.`: `...:09Z` came after `...:09.5Z`, so `ORDER BY effective_start DESC` mis-ordered a whole-second event against a fractional one of the same second (the review's B9). Only the storage form changes -- `instant_from_sql` and `instant_from_string` read both -- so every value the same width sorts in time order. A database that already holds short values needs them rewritten for `ORDER BY` to be right across the two; the sample backend's V026 does that with one `UPDATE` per timestamp column. Making `instant_to_string` itself fixed-width was rejected: `2026-09-24T13:05:09.000000000Z` is not what anyone wants to read in a JSON response, and it would have changed every printed timestamp.

**A generic enum imported by name lost the aliases in its type arguments.** `Listing<model.Thing>` -- `Listing` imported with `use model/paging.Listing`, `model` an alias -- is reclassified from a struct reference to an enum by the loader's first pass; the second pass, which resolves dotted names (`model.Thing`) to their module, returned early for anything already an enum and never looked at its type arguments. The alias was then looked up later in whichever module used the type, which does not have it: `unknown struct or enum type 'model.Thing'`, reported at a line of an unrelated file. `resolve_type_refs` now resolves the type arguments of an enum too (`tests/lang/multifile_generic_enum_aliased_type_arg`). Found while adding `Listing<T>` for the backend's paged lists.


## The traits/interfaces stance (2026-09-28, F1)

The language-design review (P2-2) named a real inconsistency: `Eq`, `Hash` and `Ord` are compiler-known generic bounds -- a type parameter can require one and use it inside a generic body -- but `_to_json`/`_from_json` and `#[derive(Row)]`'s `_from_row`/`_to_params` are the identical shape of thing, a fixed-signature function found by string-matching the concrete type's own name, with no bound form at all: a generic function cannot say `<T: Json>` or `<T: Row>` today, only a concrete type gets the lookup. That is the "two tiers of magic" from §2.2(g), and left alone it is the seed of "Ziman needs traits" the moment a generic function over either protocol is wanted.

**Decision: still no general trait/interface system.** The general form needs coherence rules (what happens when two modules each declare a bound for the same type), orphan rules, default method bodies, and eventually dynamic dispatch for a heterogeneous `[dyn Ord]`-shaped collection -- exactly the "behavior depends on where in a hierarchy an override lives, not on anything visible at the call site" complexity principle #1 already rejected classes for (see "Structs are plain..." above). And there is still no concrete program that needs it: `health-record-backend-ziman` (about 13,000 lines) has never once written a generic function over an *unbounded* `T`'s JSON-ness or row-ness -- every real use (`service/crud.zm`'s `Crud<E, Req, Res>`, built for E9) is "this one concrete type has this one function," already expressible with a plain top-level function and a derive. The deferral rule from day one -- "revisit only once a concrete program needs it and free functions plus nominal typing genuinely can't express it" -- has not been tripped for a *general* mechanism; it has only been tripped for the narrower "let `Eq`/`Hash`/`Ord`'s bound *mechanism* itself cover a fourth case."

**What changes: the bound list grows by one, `Row`.** `Eq`/`Hash`/`Ord` were never three unrelated special cases; each already means "the type has a function of this exact signature," resolved by the type's own derived or hand-written name (`<snake_type>_compare` for `Ord`, and so on) -- a function-typed bound, just not named that and not opened past three instances. `Row` is added to the same closed list on the strength of a concrete pain point this session's E9 work hit directly: `Crud<E, Req, Res>` cannot express "`E` already has a `#[derive(Row)]` pair," so every migrated resource's service file plumbs `from_row`/`to_params` through the `Crud` literal by hand as two function-value fields -- boilerplate, and nothing stops a copy-paste mistake wiring one resource's `from_row` to another's `to_params`, since both are just function values of matching shape. `E: Row` closes both: `crud_find_by_id_and_patient_id<E: Row, ...>` would resolve `E`'s row functions the same way `compare<T: Ord>` already resolves `T`'s, and the two fields disappear from every `Crud` literal. Implementation is its own pass (ROADMAP.md, "Larger, needs its own design pass") -- this entry is the decision, not the patch. **That pass was done on 2026-10-09 (docs/reviews/tasks.md C18) and it does not support this paragraph: see "The `Row` bound, and why `from_row` cannot work the way `compare` does" below. The sentence above that `E: Row` "would resolve `E`'s row functions the same way `compare<T: Ord>` already resolves `T`'s" is true of `to_params` and false of `from_row`, for a structural reason this entry did not see. The decision to add `Row` to the closed list is withdrawn until something listed there changes.**

**What does not change: `Json` stays outside the bound list.** No generic function anywhere in the library or the reference backend needs `<T: Json>` -- `#[derive(Json)]` on a generic struct already resolves each type parameter's converter individually at the derive site (R14), which is the only real need seen so far. Adding a bound nobody calls would be exactly the "add more convention-dispatched protocols" this task exists to stop doing casually. Revisit if a concrete generic-over-Json function shows up.

**`?`'s missing error conversion (P1-2) is unrelated and already solved a different way** -- `#[from]` on an enum variant (C2, done), a closed per-variant attribute, not a bound. It does not need this mechanism, and this decision does not touch it.

The set stays closed and compiler-known (`Eq`, `Hash`, `Ord`, and now `Row`) -- a user still cannot declare their own bound. That is the traits stance: narrow, purpose-built extensions to one existing mechanism when a concrete program hits a wall, never a general, user-extensible protocol system.


## Experimental modules: `pg`, `mysql`, `toml`, `yaml` (2026-09-28, F4)

The language-design review's risk 3 (§6) named a real tension: one project owns a tree-walking interpreter, a type checker, an LSP, a linear-time regex engine, TLS/SCRAM clients, three database drivers, four data formats, JWT, OTP and a migration tool, and every one of them is surface the core language's own changes must keep green. P2-9's fix is policy, not code: freeze the modules that carry the most of that risk as "experimental" until the core is hardened, rather than implicitly promising the same stability for all of them.

**Why these four, and not the rest of the library.** Two different reasons land on the same four modules. `pg` and `mysql` each reimplement a real wire protocol end to end in Ziman -- authentication, TLS negotiation, the extended/binary protocol, error mapping -- by far the largest, most externally-specified surface in the library; each module's own "what it does not" paragraph is long enough (MD5/Kerberos/COPY/LISTEN-NOTIFY for `pg`; the RSA exchange, the compressed protocol, LOCAL INFILE, multiple result sets for `mysql`) that neither is a small, closed problem the way `uuid` or `csv` are. `toml` and `yaml` are format converters of the same shape as `json`, but unlike `json` -- which the entire reference backend speaks for every request and response, so every edge case is exercised constantly -- neither is used anywhere in `health-record-backend-ziman`: each has its own test suite, but no real program has ever depended on either one in a load-bearing way. `sqlite` stays out of "experimental" on purpose: it is the reference backend's actual, constantly-exercised database, has by far the fewest open gaps of the three engines, and flagging it would leave the North Star application with nothing to run on.

**What "experimental" means, concretely.** Not "broken" or "do not use" -- all four pass their own test suites and are already documented as honestly as any other module, each with its own "what it does not support" list. It means: a protocol-conformance bug is more likely to still be hiding in these four than elsewhere in the library, simply because they have had less real usage to find one, and the project makes no special effort to avoid a breaking change to one of them ahead of hardening the core -- the same posture the language itself already takes pre-1.0. Someone picking Postgres for production, or reading a config file with `toml`, should know that going in.

**What graduates a module out of experimental.** The same test this project already applies to language decisions (see "The traits/interfaces stance" above): sustained real use by a concrete program, not a calendar date or a feature checklist. If the reference backend, or another real Ziman program, comes to depend on `toml`/`yaml`/`mysql` the way it already depends on `json`/`sqlite`/`pg`, that module's flag comes off then.

**Where the label lives.** Each of the four modules' own header comment (so `docs/lib/toml.md`, `docs/lib/yaml.md`, `docs/lib/pg.md` and `docs/lib/mysql.md`, generated from it, carry the label too), README.md's module table, and `docs/CORELIB.md`'s per-module description (`tools/gen_corelib_reference.sh`'s `describe()`, which had no entry for `pg`/`mysql` at all before this -- added along with the label).


## Rationale consolidated from SYNTAX.md (2026-09-28, F5)

The language-design review's P1-9 suggested keeping SYNTAX.md as a purely normative reference (rules only) and moving rationale into this file. Read start to finish, SYNTAX.md turned out not to have a handful of separable essays sitting in an otherwise rule-only document -- nearly every bullet welds a short "why" directly onto the rule it explains, at a granularity too fine to extract without fragmenting the reference or losing something a reader needs right there. The few passages that *were* genuinely self-contained digressions -- readable as an aside, not needed to understand the rule itself -- are collected here, each replaced in SYNTAX.md by a one-line pointer to this entry. A full rewrite of the file down to "rules only" was considered and set aside: the size/risk of touching nearly every bullet outweighed the benefit given DESIGN.md already covers most of this ground from when each feature was built.

**Why `a..b` (in a `for` header) has no first-class `Range` type.** Deliberate, the same "hardcode the minimum, not general machinery" instinct behind `Result<T, E>`: a real `Range` type is a bigger design than it looks (does it own its own equality? can it be empty as a first-class value? does `for` need to special-case it or does it become a third iterable kind alongside arrays?) that nothing has asked for yet.

**Why enum construction is always qualified (`Shape.Circle(5.0)`, never bare).** Rust/Swift-style languages let `Option`/`Result`'s variants be written bare only because they're special-cased into every file's prelude; an ordinary user enum in those languages requires qualification too. Since two unrelated enums can easily share a variant name, bare construction would make that silently ambiguous (or force an implicit "first declaration wins" rule) -- exactly the kind of implicit behavior Ziman avoids elsewhere.

**Why `Option<T>`'s bare-value auto-wrap is closer to a C++ converting constructor than to Rust.** Rust makes you write `Some(x)` or `.into()` explicitly; Ziman's auto-wrap is a deliberate, narrow exception to "no implicit coercion" specifically because wrapping a single, statically-known type is never lossy, unlike numeric coercion (which stays banned). `Result<T, E>`'s own bare-value auto-wrap is the same exception for the same reason.


## A unified `error_message` builtin (2026-09-28, F3/P2-4)

The language-design review named a real inconsistency (§4.2): "Five different failure shapes for the same job" -- `net` returns `NetError`, `datasource` `DbError`, `client` `ClientError`, `jwt` `JwtError` -- "each has its own `*_message` function with a different name (`net_error_message`, `db_error_message`, ...). There is no shared 'error with a message' notion... a tiny convention (every error type has a function of the form `<type>_message`, or a builtin `error_message(e)` intrinsic over enums that have one) would help." That is exactly what this adds.

**Decision: the builtin, not a bound.** `error_message(e)` dispatches to `<snake_case_type>_message(e: Type) -> string` by the enum's own declared name -- the identical naming-convention lookup `compare`'s `Ord` dispatch already uses for `<snake_case_type>_compare` (`find_compare_function` in the type checker), just a different suffix and no bound to satisfy first, since `error_message` is never called from inside a generic declaration's own body the way `compare<T: Ord>` is -- it only ever takes one already-concrete enum value. `find_message_function` mirrors `find_compare_function` deliberately narrowed to enums (an error type is never a struct in this codebase, so there was no reason to carry the struct case, or `Ord`'s generic-type-parameter handling, across).

**What changed to make the four bundled error types agree.** `net_error_message`, `db_error_message` and `jwt_error_message` already fit the convention. `client`'s own function was bare `error_message` (renamed from `client_error_message` for R16's "called qualified: a short verb, no module prefix" reason) -- which would now collide with the new builtin, a reserved name like `compare`/`hash`. Renamed back to `client_error_message`, and every internal caller (the client test fixtures' `client.error_message(...)`) moved to the plain builtin, which now reaches all four uniformly with no module qualifier at all: `error_message(e)` for `e: NetError | DbError | JwtError | ClientError`, from any module, no `use` beyond whatever already brought the error type itself into scope. `HttpError` stays outside this convention on purpose -- it flows into `error_response(e) -> Response`, not a plain string message, a different job entirely; forcing a `_message` function onto it that nothing would call would be exactly the "add convention-dispatched protocols" the traits-stance entry above says not to do casually.

**A user's own error enum needs no special declaration to join in** -- a plain `fn <snake_case_type>_message(e: Type) -> string` in the same module is all `error_message` looks for, the same "just write the function, the naming convention is the whole interface" shape `Ord`/`Hash` already have. Verified: `tests/lang/error_message_dispatch` (all four bundled types, one call site each), `error_message_unsupported_type_rejected` (a type with no `_message` function, naming what it's missing), `error_message_redefine_builtin_rejected` (the same "builtin function and cannot be redefined" protection `ok`/`compare`/`hash` already have).
