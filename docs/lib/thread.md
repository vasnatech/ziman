# `thread`

What a thread can ask about itself, and values that are different on every thread.

Every thread has an `id` (the main thread is 0, a `spawn`ed one gets the next number) and a `name`
(`main`, or `<function>-<id>` for a spawned thread, which `set_name` can change). Nothing here
starts, stops or waits for a thread -- `spawn` starts one and a `Channel` is how it talks -- so this
module is only what a running thread can see of itself.

`ThreadLocal<T>` is a slot that holds a separate `T` on every thread, like Java's `ThreadLocal`.
Make the key once, as a module-level `val`, giving the function that makes a thread's first value;
each thread then reads and replaces its own with `get` and `set`:


```
use thread

val request_count = thread.local<int>(fn() -> int { return 0 })

fn handle() {
    thread.set(request_count, thread.get(request_count) + 1)
}
```

A value is immutable, so "editing" the slot is `set` with a new value (`push` on an array is
amortised O(1), so a slot holding an array is cheap to grow). For a large mutable cache, keep a
`MutableMap` or `MutableArray` in the slot: a builder must stay on the thread that made it, which a
thread's own slot guarantees, and `ThreadLocal` is the one type that may hold one at module level
(the key itself is only a number and a function, shared by every thread).

A thread's slots are dropped when the thread ends. `server.serve` runs one thread per connection,
so a thread-local lives as long as a connection, not for the whole run: good for per-request
context and scratch space, not for a cache that must outlive a connection (an actor thread owning
the cache, reached over a channel, is the tool for that).

`use thread` -- or `use thread.Name` to bring one item in bare.

## Example

```
use os
use thread

// A key for a value that is separate on every thread; each thread's first `get` runs the function.
val request_count = thread.local<int>(fn() -> int { return 0 })

fn worker(done: Channel<string>) {
    thread.set(request_count, thread.get(request_count) + 1)
    send(done, format("{} counted {}", thread.name(), thread.get(request_count)))
}

fn main() {
    print(thread.name())
    print(thread.is_main())

    thread.set(request_count, 10)
    val done: Channel<string> = channel_new()
    // `on_error os.fatal` because the code below waits on this channel: a thread that fails
    // without a handler leaves whoever is waiting waiting for ever, which `--lint` reports and
    // which an example shown as exemplary should not demonstrate.
    spawn worker(done) on_error os.fatal
    print(receive(done))

    // The worker's count started from 0 and never touched this thread's.
    print(thread.get(request_count))
}
```

Output:

```
main
true
worker-1 counted 1
10
```

### `id`

```
pub native fn id() -> int
```

The calling thread's number: 0 for the main thread, then 1, 2, ... in the order threads started.

### `name`

```
pub native fn name() -> string
```

The calling thread's name: `main`, or `<function>-<id>` for a spawned thread (`worker-3`).

### `set_name`

```
pub native fn set_name(name: string)
```

Renames the calling thread.

### `is_main`

```
pub native fn is_main() -> bool
```

True on the main thread (the one running `main`).

### `stack_remaining`

```
pub native fn stack_remaining() -> int
```

Bytes of stack the calling thread has left before the "stack depth exceeded" error.

### `ThreadLocal`

```
pub struct ThreadLocal<T> {
    id: int
    init: fn() -> T
}
```

A key for a value that is separate on every thread. `init` makes a thread's first value, the first
time that thread calls `get` on the key.

### `local`

```
pub fn local<T>(init: fn() -> T) -> ThreadLocal<T>
```

A fresh key for a thread-local slot, with `init` as the function that makes a thread's first value.

### `get`

```
pub fn get<T>(key: ThreadLocal<T>) -> T
```

The calling thread's value for `key`, made with the key's `init` on the first call.

### `set`

```
pub fn set<T>(key: ThreadLocal<T>, value: T)
```

Replaces the calling thread's value for `key`.

### `has`

```
pub fn has<T>(key: ThreadLocal<T>) -> bool
```

True if the calling thread has a value for `key` yet (it has once `get` or `set` has run).

### `reset`

```
pub fn reset<T>(key: ThreadLocal<T>)
```

Forgets the calling thread's value, so the next `get` makes a fresh one with `init`.
