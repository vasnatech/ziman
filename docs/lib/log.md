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

A logger with a `file` can rotate it (`max_bytes`, `keep_files`): a file that has reached `max_bytes` is renamed `<file>.1`, the older `<file>.1` becoming `<file>.2`
and so on up to `<file>.<keep_files>`, and the oldest is deleted, before the next line is written to a fresh `<file>`. So a log never grows past about
`max_bytes` (plus one line) in the file that is written, nor past `keep_files` more of them. Without `max_bytes` a log is never rotated (the operator's `logrotate`
does it, or nothing does).

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

    // Reading the file back can fail, so the arm that succeeds names the text it got.
    switch fs.read_text(path) {
        Ok(written) { print(trim(written)) }
        Err(e) {
            printf("could not read {}: {}\n", path, fs.fs_error_message(e))
        }
    }
    if Err(e) = fs.remove(path) {
        printf("could not remove {}: {}\n", path, fs.fs_error_message(e))
    }

    print(log.enabled(logger, Level.Debug))
    if Ok(level) = log.parse_level("warn") {
        print(level == Level.Warn)
    }
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
    max_bytes: int
    keep_files: int
    // One JSON object per line instead of the text format, for a log a machine reads. See `write`
    // for the field names and why the escaping goes through `json` rather than `single_line`.
    json: bool
    // Carried on the logger rather than passed to every call, so a request's handler sets it once
    // (`with_request_id`) and every line it logs afterwards carries it -- the same shape as
    // `prefix`, and the reason this is a field and not a parameter. Empty means no id, and the
    // field is then left out of the object rather than written as null, which is friendlier to
    // whatever indexes the log.
    request_id: string
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
    console: Option<bool>,
    max_bytes: Option<int>,
    keep_files: Option<int>,
    json: Option<bool>
) -> Logger
```

A logger for messages at `level` and above. Without `file` it writes to
stderr; with one it appends to that file (a failed write falls back to
stderr, so a log line is never silently lost). `timestamps: false` leaves the
time off, for output that is compared in tests. With `console: true` a logger that has a `file` also
writes every line to stderr, as a server does that logs to a file for the record and to the console for whoever
is watching. `max_bytes` (0 or left out: never) rotates the file when it reaches that size, keeping `keep_files` old ones (default 5; 0 keeps none).

### `with_prefix`

```
pub fn with_prefix(l: Logger, prefix: string) -> Logger
```

The same logger with `prefix` in front of every message -- one per component.

Written with `with` rather than by naming all nine fields: the field-by-field version silently
dropped whatever was added to `Logger` later, which is exactly how `json` and `request_id` would
have been lost here when they arrived.

### `with_request_id`

```
pub fn with_request_id(l: Logger, id: string) -> Logger
```

The same logger stamping `id` on every line it writes -- set once where a request begins, so the
handler's own logging needs no extra argument. An empty `id` clears it again.

### `rank`

```
pub fn rank(level: Level) -> int
```

0 for Debug up to 3 for Error: the order of severity.

### `format`

```
pub fn format(level: Level) -> string
```

The level's name: "DEBUG", "INFO", "WARN" or "ERROR".

**Unpadded, as of 2026-10-24.** It used to answer "INFO " -- padded to five characters so the
text format's columns line up -- which made a presentation detail part of the value every other
caller got. `json_line` had to `trim` it back off before putting it in a log index, and anyone
writing their own format inherited a stray space they had to know about. The padding lives in
the text formatter now, where the columns are.

### `parse_level`

```
pub fn parse_level(text: string) -> Result<Level, string>
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
