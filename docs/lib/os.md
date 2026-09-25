# `os`

The process and its environment: environment variables, command-line arguments,
the working directory, and ending or shutting down the program.

Nothing here can fail in a way a program needs to handle except reading the working
directory, which returns a `Result`; the rest are plain values or actions.

`use os` -- or `use os.Name` to bring one item in bare.

## Environment

### `getenv`

```
pub fn getenv(name: string) -> Option<string>
```

The value of the environment variable `name`, or None if it isn't set. A variable that is
set but empty is Some(""), which is not the same as unset. (The `properties` module uses this
to let `SERVER_PORT` override `server.port`.)

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
