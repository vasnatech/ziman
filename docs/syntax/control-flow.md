# Result, defer and concurrency

Part of the [v0 syntax reference](README.md).

## Result

```ziman
fn safe_div(a: int, b: int) -> Result<int, string> {
    if b == 0 {
        return err("division by zero")
    }
    return ok(a / b)
}

fn main() {
    val r = safe_div(10, 0)
    if is_ok(r) {
        print(unwrap(r))
    } else {
        print(concat("failed: ", unwrap_err(r)))
    }
}
```

- **`Result<T, E>` is a builtin, not a declared enum** -- manipulated through `ok`, `err`,
  `is_ok`, `unwrap`, `unwrap_err` (see "Builtins") or `switch` (arms `Ok(v)`/`Err(e)`, a
  statement not an expression). You can't declare your own `Result`-shaped type, only an
  `enum` (see "Enums").
- **`ok(x)`/`err(e)` can't fully infer their own type** -- each determines only half of
  `Result<T, E>`. Legal in exactly two places that supply the missing half: a `val`/`var` with
  an explicit `Result<T, E>` annotation, or a `return` inside a function declared `-> Result<T,
  E>` (the common shape). Elsewhere (a bare statement, a call argument) it's a compile error --
  the same restriction empty array literals have, for the same reason (nothing to infer the
  missing type from); `[]` gets inferred in a couple more positions (struct fields, `with`
  overrides) since it only needed one type, not two to disambiguate.
- **`unwrap`/`unwrap_err` are the explicit, opt-in escape hatch to a fatal error** -- `unwrap(r)`
  returns the success value or raises a runtime error on `Err` (mirror for `unwrap_err`);
  crashing on failure becomes the caller's explicit choice, not something forced unconditionally.
- **`==`/`!=` compares variant then payload** -- `Ok`/`Err` never equal each other; same
  variant is equal iff payloads are. Legal only when both `T` and `E` are comparable
  (recursively, same rule as structs/arrays), since the variant isn't known statically.
- **`T`/`E` can be any Ziman type**, including another `Result`, an array, or a struct --
  letting application code layer a richer error type over a simpler native one, converting at
  the boundary rather than forcing one universal error shape.
- Printing shows `Ok(...)`/`Err(...)`, recursively displaying the payload like `print` always does.

## Defer

`defer f(args)` runs a call when the enclosing *block* exits, by any path:
falling off the end, `return`, a `try` that propagates an error, `break`, or
`continue`. It is how a resource is released without destructors.

```ziman
use datasource
use db.DbError
use net

fn read_reply(sock: net.Socket) -> string { return "0" }   // stands for the caller's own reading

fn talk(port: int) -> Result<int, string> {
    val sock = unwrap(net.connect("127.0.0.1", port))
    defer net.close(sock)              // closed on every path out of talk
    val reply = try parse_int(read_reply(sock))   // an early return still closes it
    return reply
}

fn transfer(conn: datasource.Connection) -> Result<int, DbError> {
    try datasource.begin(conn)
    defer datasource.rollback(conn)            // undone unless we reach the commit
    try datasource.execute(conn, "...")
    try datasource.commit(conn)
    return 0
}
```

- **Block-scoped** -- runs when its own block ends (an `if`/loop body, not just the function),
  so one inside a loop body runs every iteration (unlike Go, where defers pile up to the return).
- **Last in, first out** across a block's defers.
- **Arguments are evaluated when `defer` runs, not when it fires**: `var n = 1; defer show(n);
  n = 2` shows `1` -- only the call itself is postponed.
- **The return value is computed first**: `return x * 2` evaluates `x * 2`, runs the defers,
  then returns -- a deferred call can't change what's returned; `try` propagates the same way.
- **A single call only** -- not a block, not `f()?`; put cleanup logic in a helper function
  instead. The argument-expression-reading builtins (`format`, `ok`, `err`, `channel_new`)
  can't be deferred.
- **The result is discarded, including a `Result`** -- a deferred `net.close`/`rollback` has
  nowhere to report failure. `defer` is the one exception to must-use `Result` (see "Must-use
  `Result`"), since there's nowhere left to propagate to by the time it runs -- deliberate, and
  what lets `defer datasource.rollback(conn)` stay harmless after a successful commit.
- **A fatal runtime error skips defers** -- `unwrap` on `Err`, an out-of-bounds index, exit the
  process instead of unwinding, so nothing deferred runs (the OS closes sockets, a DB server
  rolls back a dropped connection's transaction, but nothing else deferred happens).
- **Threads**: a `spawn`ed function gets its defers like any other.

## Concurrency

`Channel<T>` and `spawn` — a third hardcoded parametric type (after
`[T]`/`Result<T, E>`) plus one new statement, giving Ziman real OS-thread
concurrency built on a single-writer / actor pattern: one thread owns a
piece of mutable state and is the only thing that ever touches it; every
other thread reaches it only by sending a message and waiting for a
reply.

```ziman
enum PatientMessage {
    Create(string)
    GetById(int)
}

fn run_patient_actor(inbox: Channel<PatientMessage>, replies: Channel<string>) {
    var next_id = 1
    while true {
        val msg = receive(inbox)
        if PatientMessage.is_Create(msg) {
            val name = PatientMessage.unwrap_Create(msg)
            send(replies, format("created #{}: {}", next_id, name))
            next_id = next_id + 1
        } else {
            send(replies, "lookup not shown here")
        }
    }
}

fn main() {
    val inbox: Channel<PatientMessage> = channel_new()
    val replies: Channel<string> = channel_new()
    spawn run_patient_actor(inbox, replies)

    send(inbox, PatientMessage.Create("Ada"))
    print(receive(replies))   // created #1: Ada
}
```

- **Builders and iterators stay in their thread.** `MutableString`, `MutableArray`,
  `MutableMap`, `MutableSet` and `Iter` are mutable, so each belongs to the thread that made
  it: not a `Channel` element, `spawn` argument, or module-level `val` (compile errors), and
  any other cross-thread use is a runtime error. See "Attributes and derive" (`#[thread_bound]`).
- **`Channel<T>` is a thread-safe queue of one element type**, unified rather than split into
  sender/receiver types -- any value supports both `send`/`receive`. **Unbounded**
  (`channel_new()`: `send` never blocks) or **bounded** (`channel_new(8)`: `send` blocks while
  full -- backpressure). `receive(chan) -> T` blocks until available; `try_send`/`try_receive`
  never block (`false`/`None` when full/empty); `channel_len(chan) -> int`. A bounded `send`
  with nobody receiving waits forever; there is no deadlock detection.
- **Closing, timed receives, two-channel waits.** `channel_close(chan)` stops new sends (closing
  twice is harmless); already-queued values still deliver. `send`/`try_send` on a closed channel
  is a runtime error, and a `send` blocked on a full channel is woken by the close and fails the
  same way. Plain `receive` on closed-and-empty is a runtime error too; `receive_option(chan) ->
  Option<T>` blocks like `receive` but returns `None` once closed and drained (no sentinel
  needed). `receive_timeout(chan, millis) -> Option<T>` gives `None` on timeout (indistinguishable
  from closed-and-empty). `select2(a, b) -> Selected<A, B>` (`First(x)`/`Second(y)`, possibly
  different element types) returns whichever is ready first, `a` on a tie; polls both with a 1 ms
  pause (up to ~1ms latency), same no-deadlock-detection property.
- **`channel_new()` can't infer its own type** -- same context-inference mechanism as
  `ok`/`err`/`[]`, legal in the same four positions (an annotated `val`/`var`, a struct field, a
  `with` override, a function's declared return type).
- **Not comparable**: `==`/`!=` on a `Channel` is a type error (a stateful handle, not data).
- **`spawn name(args)` starts `name` on a new OS thread**, `args` evaluated on the calling
  thread first. `name` must be a declared function (never a function value -- `spawn mod.name`
  for an imported one, subject to the same `pub` rule as a qualified call; a generic spawn
  writes its type arguments, `spawn worker<int>(args)`). **The spawned function must return
  `void`** -- there's no join/thread-handle, so its result is only observable through a
  `Channel` it's explicitly given, and `spawn` is a statement, not an expression.
- **A runtime error in a spawned thread ends that thread, not the program**: written to stderr,
  the thread's `defer`red calls run (innermost first), everything else continues (a main-thread
  error still ends the program). `spawn worker(x) on_error handler` (`handler: fn(string)`) runs
  first, while resources are still open, then the defers; `on_error os.fatal` makes that thread's
  failure end the whole program like a main-thread one. `server.serve` uses this per connection:
  a crashing handler gets a 500, its slot and socket release, and the server continues. Nothing
  but the defers runs -- a lock a native held at the moment of the error stays held (none of the
  library's natives hold one across a possible error). If that thread was the only one that could
  have sent on a channel someone is blocked reading, the reader does not wait for ever: when every
  live thread is blocked on a channel, nothing can ever send, and that is reported as a runtime
  error (`deadlock: every thread is blocked on a channel ...`) on each blocked thread. The failed
  thread's own error is printed first, so the two together say what happened and why the program
  stopped. A thread that is computing, sleeping or in a syscall counts as live, so this fires only
  when every thread really is waiting on a channel.
- **`thread` module**: `thread.id()` (0 for main, then 1, 2, ... in start order), `thread.name()`
  (`main` or `<function>-<id>`; `set_name` renames), `is_main()`, `stack_remaining()` (bytes
  before "stack depth exceeded"). No join or thread handle. `thread.ThreadLocal<T>` is a
  per-thread slot (like Java's `ThreadLocal`): make the key once as a module-level `val` via
  `thread.local<T>(init: fn() -> T)`, then `thread.get(key)`/`set(key, v)`/`has(key)`/
  `reset(key)` (forgets it, so the next `get` re-runs `init`) per thread. A slot holds an
  immutable value (`set` to update; keep a `MutableMap`/`MutableArray` in it for a large mutable
  cache -- a builder must stay on its own thread, which a thread-local slot guarantees; marked
  `#[thread_key]`, it's the one type allowed to sit at module level). Slots drop when the thread
  ends -- right for per-request scratch space (`server.serve`'s one-thread-per-connection), not
  for a cache that must outlive connections (an actor thread owning it over a channel is that tool).
- **Sharing a `Value` across threads needs no locking of its own.** An `ArrayObj`/`StructObj`/
  `EnumObj`/`ResultObj` is immutable once built, so crossing a channel is just a pointer copy --
  no data race, and no use-after-free since the collector only reclaims what's unreachable
  everywhere. The one thing that would break this, a mutable handle, is refused wherever it could
  cross a thread boundary (`#[thread_bound]`) instead of being allowed to race.
