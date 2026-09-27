# `log`

Levelled logging to stderr or to a file.

Ziman has no mutable globals, so there is no hidden "the logger": you build a
`Logger` once (from configuration, say) and pass it where it is needed.


```
val logger = log.logger(level: Level.Info)
log.info(logger, "listening on port 8080")
log.debug(logger, "not printed: below the Info threshold")
```

A line looks like `2026-09-24T12:00:00.123Z INFO  listening on port 8080`.
Messages below the logger's level are dropped without being written.

`use log` -- or `use log.Name` to bring one item in bare.

## Example

```
use fs
use log
use log.Level

// A Logger is a value you build once and pass where it is needed. Records below its level are dropped.
fn main() {
    val path = "/tmp/ziman_manual_log_example.log"
    val logger = log.logger(level: Level.Info, file: path, timestamps: false, prefix: "app: ")
    log.debug(logger, "not written")
    log.info(logger, "started")
    log.error(log.with_prefix(logger, "db: "), "connection lost")
    print(trim(unwrap(fs.read_text(path))))
    unwrap(fs.remove(path))
    print(log.enabled(logger, Level.Debug))
    print(unwrap(log.level_parse("warn")) == Level.Warn)
}
```

Output:

```
INFO  app: started
ERROR db: connection lost
false
true
```

### `Level`

```
pub enum Level {
    Debug
    Info
    Warn
    Error
}
```

A message's severity, lowest first. A logger drops messages below its own level.

### `Logger`

```
pub struct Logger {
    level: Level
    file: Option<string>
    timestamps: bool
    prefix: string
    console: bool
}
```

A logger's settings; build one with `logger`, and use `with_prefix` for per-component copies.

### `logger`

```
pub fn logger(
    level: Level,
    file: Option<string>,
    timestamps: Option<bool>,
    prefix: Option<string>,
    console: Option<bool>
) -> Logger
```

A logger for messages at `level` and above. Without `file` it writes to
stderr; with one it appends to that file (a failed write falls back to
stderr, so a log line is never silently lost). `timestamps: false` leaves the
time off, for output that is compared in tests. With `console: true` a logger that has a `file` also
writes every line to stderr, as a server does that logs to a file for the record and to the console for whoever
is watching.

### `with_prefix`

```
pub fn with_prefix(l: Logger, prefix: string) -> Logger
```

The same logger with `prefix` in front of every message -- one per component.

### `level_rank`

```
pub fn level_rank(level: Level) -> int
```

0 for Debug up to 3 for Error: the order of severity.

### `level_name`

```
pub fn level_name(level: Level) -> string
```

The level as it appears in a log line, padded to five characters ("INFO ").

### `level_parse`

```
pub fn level_parse(text: string) -> Result<Level, string>
```

"debug", "info", "warn"/"warning" or "error" (any case), for a level read
from configuration.

### `enabled`

```
pub fn enabled(l: Logger, level: Level) -> bool
```

True if a message at `level` would be written by `l`.

### `write`

```
pub fn write(l: Logger, level: Level, message: string)
```

Writes `message` at `level` if the logger's level allows it: a line of the form
`<timestamp> <LEVEL> <prefix><message>`. Line breaks in `message` are written as `\n` and `\r`, so
one call is always one line. To stderr, or appended to the logger's file (falling back
to stderr if the file cannot be written, and also written to stderr when the logger is a `console` one).

### `debug`

```
pub fn debug(l: Logger, message: string) { write(l, Level.Debug, message) }
```

`write` at each level: detail useful while developing.

### `info`

```
pub fn info(l: Logger, message: string) { write(l, Level.Info, message) }
```

Normal events worth recording.

### `warn`

```
pub fn warn(l: Logger, message: string) { write(l, Level.Warn, message) }
```

Something unexpected that the program handled.

### `error`

```
pub fn error(l: Logger, message: string) { write(l, Level.Error, message) }
```

A failure the program could not handle.
