# `os`

The process and its environment: environment variables, command-line arguments,
the working directory, ending or shutting down the program, and what can be asked of the machine
it runs on (CPUs, memory, disks, load, uptime).

Reading the working directory and the machine figures can fail, so they return a `Result`; the
rest are plain values or actions. The machine figures come from `/proc` and `statvfs` (Linux;
on another system a figure that cannot be read is an `Err("not supported here: ...")`, never a
made-up number). Sizes are in bytes.

`use os` -- or `use os.Name` to bring one item in bare.

## Example

```
use os

fn main() {
    print(Option.is_None(os.get_env("ZIMAN_SURELY_NOT_SET")))
    print(os.pid() > 0)
    // The working directory can be gone by the time it is asked for, so it answers a Result.
    if Ok(cwd) = os.cwd() {
        print(byte_len(cwd) > 0)
    }
    print(len(os.args()))
    print(os.shutdown_requested())
}
```

Output:

```
true
true
true
0
false
```

## Environment

### `get_env`

```
pub fn get_env(name: string) -> Option<string>
```

The value of the environment variable `name`, or None if it isn't set. A variable that is
set but empty is Some(""), which is not the same as unset. (The `properties` module uses this
to let `SERVER_PORT` override `server.port`.)
**`getenv` until 2026-10-24.** It sat beside `set_env`, so one of the two was wrong: `getenv`
is C's spelling and `set_env` is not C's (`setenv` is), which left the pair agreeing with
neither convention. Both are snake_case now.

### `set_env`

```
pub native fn set_env(name: string, value: string) -> bool
```

Sets an environment variable for this process and any it starts. False if the name or value
is not valid for the environment (an empty name, or a name containing `=`).

## Process

### `args`

```
pub native fn args() -> [string]
```

The program's own command-line arguments: everything after `--` in `ziman prog.zm -- a b`,
so `["a", "b"]`. Empty when there is no `--`.

### `cwd`

```
pub fn cwd() -> Result<string, string>
```

The process's working directory. An Err carries the operating system's message.

### `pid`

```
pub native fn pid() -> int
```

The process id.

### `collect_garbage`

```
pub native fn collect_garbage()
```

Runs the garbage collector now, and with it the finalizers of sockets and database connections
that nothing references any more (which close them). Ordinarily collections run on their own;
call this to reclaim descriptors promptly, or in a test. It does nothing in the non-collecting
build.

### `collector_enabled`

```
pub native fn collector_enabled() -> bool
```

True in the normal build, false in `ziman-nogc` (which never frees anything).

### `open_descriptors`

```
pub native fn open_descriptors() -> int
```

How many file descriptors the process has open (counted from /proc/self/fd), or -1 where that
cannot be counted. For diagnostics and tests.

### `exit`

```
pub native fn exit(code: int)
```

Leaves the process with status `code` (0 = success), flushing stdout and stderr first. This
is how a program reports a fatal setup error with a non-zero status instead of crashing on an
`unwrap`. It ends the whole process, not one thread.

### `fatal`

```
pub fn fatal(message: string)
```

A handler for `spawn f(x) on_error os.fatal`: writes the message of the runtime error that ended the thread to stderr and ends the whole
program with status 1, as an error in the main thread does. (Without an `on_error` clause only the failed thread ends.)

### `write_stderr`

```
pub native fn write_stderr(text: string)
```

Writes `text` to stderr immediately, with no newline added and no buffering -- for messages
that must not mix into the program's normal output.

## Graceful shutdown

### `install_shutdown_handler`

```
pub native fn install_shutdown_handler()
```

Turns on the behavior described above. Call it once at startup.

### `shutdown_requested`

```
pub native fn shutdown_requested() -> bool
```

True once SIGINT or SIGTERM has arrived (after `install_shutdown_handler`).

## The machine

### `cpu_count`

```
pub native fn cpu_count() -> int
```

How many logical CPUs are online (at least 1).

### `hostname`

```
pub native fn hostname() -> string
```

The machine's name.

### `name`

```
pub native fn name() -> string
```

The operating system: `linux`, `macos`, ... (lower case).

### `arch`

```
pub native fn arch() -> string
```

The processor architecture: `x86_64`, `aarch64`, ...

### `load_average`

```
pub native fn load_average() -> Result<[float], string>
```

The load averages over the last 1, 5 and 15 minutes: how many tasks were running or waiting for a CPU, on average.

### `uptime_seconds`

```
pub native fn uptime_seconds() -> Result<int, string>
```

How long the machine has been up, in whole seconds.

## Memory

### `memory_total`

```
pub native fn memory_total() -> Result<int, string>
```

The machine's memory, in bytes.

### `memory_available`

```
pub native fn memory_available() -> Result<int, string>
```

The memory that could be given to a program now without swapping, in bytes (Linux's `MemAvailable`: free memory plus what can be
reclaimed from caches).

### `process_memory`

```
pub native fn process_memory() -> Result<int, string>
```

This process's resident memory, in bytes.

## Disks

### `disk_total`

```
pub native fn disk_total(path: string) -> Result<int, string>
```

The size of the filesystem that `path` is on, in bytes. An Err when `path` does not exist.

### `disk_free`

```
pub native fn disk_free(path: string) -> Result<int, string>
```

What an ordinary user can still write to the filesystem `path` is on, in bytes.

### `disk_used`

```
pub native fn disk_used(path: string) -> Result<int, string>
```

What is in use on the filesystem `path` is on, in bytes (the total less what is free to anyone).

## CPU

### `CpuTimes`

```
pub struct CpuTimes {
    busy: int
    total: int
}
```

How much CPU time all CPUs have spent, in ticks since boot: `busy` (working) out of `total` (working and idle). One reading means little; the
difference of two is what `cpu_usage_between` turns into a rate.

### `cpu_times`

```
pub fn cpu_times() -> Result<CpuTimes, string>
```

A reading of the CPU counters, to be compared with a later one by `cpu_usage_between`.

### `cpu_usage`

```
pub fn cpu_usage(before: CpuTimes, after: CpuTimes) -> float
```

The fraction of the CPU (0.0 to 1.0, over all CPUs together) that was busy between two readings `before` and `after`. 0.0 when no time passed
between them.
**Two readings rather than a sample**, told apart by the arguments -- `cpu_usage_between` until
2026-10-24, where `_between` restated what two `CpuTimes` already say.

The two members answer different types, and for the reason `csv`'s `format` does: this one is
arithmetic over readings the caller already has and cannot fail, where the sampling form below
has to read the counters and can. A `Result` nobody could ever check would be worse than the
asymmetry.

```
pub fn cpu_usage(sample_millis: int) -> Result<float, string>
```

The fraction of the CPU that was busy over the next `sample_millis` milliseconds: it reads the counters, waits, reads them again. It blocks the
caller that long; take two `cpu_times` readings yourself to sample without waiting.

### `process_cpu_seconds`

```
pub native fn process_cpu_seconds() -> Result<float, string>
```

The CPU time this process has used so far, in seconds, all its threads together.

## This process

### `process_uptime_seconds`

```
pub native fn process_uptime_seconds() -> Result<int, string>
```

How long this process has been running, in whole seconds.

### `thread_count`

```
pub native fn thread_count() -> int
```

How many threads this process has (-1 where that cannot be read).
