# `process`

Starting another program, waiting for it, signalling it.

Ziman had no way to do this until F54, which made a whole class of program -- a test harness, a
build script, a deploy tool -- impossible to write in it. It was found missing by the reference
backend's own test migration: 19 of its 20 suites start the server as a child process.

Output goes to a **file**, not a pipe. `start` takes optional paths for the child's stdout and
stderr; given none, the child inherits this process's. There is no pipe-reading API on purpose --
a pipe nobody drains fills up and blocks the child, so a server logging to one deadlocks after a
few kilobytes, and "read it after waiting" is itself the deadlock. A file has no such limit, and
is still there to read with `fs.read_text` after the child is gone, which is when a test wants it.

`use process` -- or `use process.Name` to bring one item in bare.

## Example

```
use fs
use process

// Starting, waiting for and signalling a child process. A `Process` is a handle to something
// outside this program, so every operation on it answers a Result -- the child may already have
// gone -- and its output goes to files rather than to a pipe, which is what keeps a slow reader
// from deadlocking a chatty child.
fn demo(out_path: string) -> Result<unit, string> {
    // Arguments are a list, not a line to be re-parsed, so a space or a quote in one is just a
    // character and there is no shell to escape for.
    val hello = try process.start(
        "/bin/echo",
        ["a b", "c'd"],
        None,
        Some(out_path),
        None,
        None
    )
    print(try process.wait(hello))
    // Reading the file answers an fs.FsError, not this function's string, so it converts here.
    switch fs.read_text(out_path) {
        Ok(text) { print(trim(text)) }
        Err(e) { return err(fs.fs_error_message(e)) }
    }

    // The environment is given explicitly, so a child sees what it was handed.
    val printenv = try process.start(
        "/usr/bin/env",
        [],
        Some([process.env("ZIMAN_EXAMPLE", "set-by-the-parent")]),
        Some(out_path),
        None,
        None
    )
    try process.wait(printenv)
    val lines = switch fs.lines(out_path) {
        Ok(it) { it }
        Err(e) { return err(fs.fs_error_message(e)) }
    }
    for line in lines {
        if Ok(text) = line && starts_with(text, "ZIMAN_EXAMPLE=") {
            print(text)
        }
    }

    // A failing child is not an error of this call: the exit status is the answer.
    val failing = try process.start("/bin/false", [], None, None, None, None)
    print(try process.wait(failing))

    // Waiting with a timeout answers None while the child is still running, so a supervisor can
    // decide to signal it rather than block for ever.
    val sleeper = try process.start("/bin/sleep", ["5"], None, None, None, None)
    print(Option.is_None(try process.wait_for(sleeper, 50)))
    try process.terminate(sleeper)
    print(is_ok(process.wait(sleeper)))
    return unit
}

fn main() {
    val out_path = "/tmp/ziman_manual_process_example.out"
    if Err(e) = demo(out_path) {
        printf("process example failed: {}\n", e)
    }
    if Err(e) = fs.remove(out_path) {
        printf("could not remove {}: {}\n", out_path, fs.fs_error_message(e))
    }
}
```

Output:

```
0
a b c'd
ZIMAN_EXAMPLE=set-by-the-parent
1
true
true
```

### `Process`

```
pub struct Process {
    handle: int
}
```

A running (or finished) child. Hold it, wait for it, signal it; the handle stops being valid once
`wait` or `release` has reaped the child, and using it after that is a runtime error rather than a
quietly wrong answer.

### `EnvVar`

```
pub struct EnvVar {
    name: string
    value: string
}
```

One `NAME=value` setting for a child's environment. A pair rather than two parallel arrays so a
caller cannot get them out of step, and a struct rather than a map because order is preserved and
a repeated name is a caller's bug worth seeing rather than one silently collapsed.

### `env`

```
pub fn env(name: string, value: string) -> EnvVar
```

### `start`

```
pub fn start(
    program: string,
    args: [string],
    vars: Option<[EnvVar]> = None,
    out_path: Option<string> = None,
    err_path: Option<string> = None,
    cwd: Option<string> = None
) -> Result<Process, string>
```

Starts `program` with `args`, which are the arguments *after* the program name -- argv[0] is the
program itself, the way a shell and exec already do it, so the name is never repeated.

`vars` are applied over this process's own environment: a name already present is replaced, not
duplicated, and an empty value is kept, since `PROFILE_ACTIVE=""` is a setting and not a request
to unset. `out_path`/`err_path` redirect the child's output to those files, truncating them;
`None` leaves the child sharing this process's. `cwd` is the directory to run in.

The program is looked up on PATH when it has no '/' in it, as a shell would.

### `wait`

```
pub fn wait(p: Process) -> Result<int, string>
```

Blocks until the child ends, then answers its exit code -- or `128 + signal` for one a signal
ended, the convention a shell reports, so `terminate` then `wait` reads as 143. Reaps the child,
so it may be called only once: the status is gone afterwards, and a second call is a runtime
error rather than an invented -1.

### `terminate`

```
pub fn terminate(p: Process) -> Result<unit, string>
```

Asks the child to stop (SIGTERM). Returns without error if it has already ended -- a child that
beat you to it is not a failure. Follow it with `wait` to collect the status.

### `kill`

```
pub fn kill(p: Process) -> Result<unit, string>
```

Stops the child outright (SIGKILL), for one that ignored `terminate`.

### `interrupt`

```
pub fn interrupt(p: Process) -> Result<unit, string>
```

Interrupts the child (SIGINT) -- what a terminal sends on ctrl-C, and a different thing from
`terminate` to a program that installs a shutdown handler. Three named signals rather than a raw
number: these are what a caller wants, and a number would be less readable and less portable.

### `wait_for`

```
pub fn wait_for(p: Process, millis: int) -> Result<Option<int>, string>
```

`wait`, but giving up after `millis`: `Some(code)` if the child ended in time, `None` if it is
still running. Only a child that ended is reaped, so after a `None` a caller can escalate --
`kill` then `wait` -- rather than being stuck.

Prefer this to `wait` wherever a hang is possible. `wait` on a child that never exits blocks for
good, which turns "the server did not shut down" from a failing test into one that never
finishes.

### `release`

```
pub fn release(p: Process)
```

Kills, reaps and forgets the child, ignoring every failure. For a cleanup path that must not
leave a zombie and has nothing useful to do if it cannot -- a test whose assertions already
failed, say, where reporting a second problem would bury the first.

### `pid`

```
pub fn pid(p: Process) -> int
```

### `run`

```
pub fn run(
    program: string,
    args: [string],
    vars: Option<[EnvVar]> = None,
    out_path: Option<string> = None,
    err_path: Option<string> = None,
    cwd: Option<string> = None
) -> Result<int, string>
```

Starts a program and waits for it, answering its exit code. For the common one-shot case where
there is nothing to do while it runs.

### `output`

```
pub fn output(
    program: string,
    args: [string],
    vars: Option<[EnvVar]> = None,
    cwd: Option<string> = None
) -> Result<string, string>
```

`run`, with the child's output captured to a temporary file and returned as a string. The file is
removed before returning, so a caller gets the text and no cleanup. Both streams go to the same
file, in the order the child wrote them, which is what a reader of a transcript wants.
