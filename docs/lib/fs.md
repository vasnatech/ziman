# `fs`

The filesystem: files, directories and paths.

Every function that can fail returns a `Result` carrying an `FsError`, so a missing directory, a
permission problem or a full disk is something a program handles, not a crash -- and the cases worth
branching on are variants rather than substrings of the OS's message, which is what they used to be.
`fs_error_message(e)` is the message to show a human, and keeps the OS's own wording inside it. An action with no value
to report returns `Ok(unit)` (or `Ok(bytes)` for the ones that write), the same shape `net`
uses. The path predicates (`exists`, `is_dir`, `is_file`) return a plain bool: false covers
"does not exist" and "cannot be checked" alike.

Text is UTF-8 in a `string`, which cannot hold a NUL byte; anything else -- images, archives,
files that might contain NUL -- is read and written as `[byte]`.

`use fs` -- or `use fs.Name` to bring one item in bare.

## Example

```
use fs

// Every call that can fail returns a Result carrying the operating system's message.
fn main() {
    val dir = "/tmp/ziman_manual_fs_example"
    unwrap(fs.make_dirs(fs.path_join(dir, "notes")))
    val path = fs.path_join(dir, "notes", "todo.txt")

    unwrap(fs.write_text(path, "buy milk\n"))
    unwrap(fs.append_text(path, "call Ada\n"))
    print(trim(unwrap(fs.read_text(path))))
    print(fs.file_name(path))
    print(fs.extension(path))
    print(unwrap(fs.list_dir(fs.parent_dir(path))))

    // Read a big file one line at a time instead of all at once.
    val lines = unwrap(fs.lines(path))
    var count = 0
    for line in lines {
        if contains(unwrap(line), "milk") {
            count = count + 1
        }
    }
    print(count)

    // Walk a directory tree, and read a file in blocks.
    for file in unwrap(fs.walk(dir)) {
        print(unwrap(file))
    }
    var bytes = 0
    for chunk in unwrap(fs.chunks(path, 4)) {
        bytes = bytes + len(unwrap(chunk))
    }
    print(bytes)

    print(is_ok(fs.read_text(fs.path_join(dir, "missing.txt"))))

    unwrap(fs.remove(path))
    unwrap(fs.remove(fs.parent_dir(path)))
    unwrap(fs.remove(dir))
    print(fs.exists(dir))
}
```

Output:

```
buy milk
call Ada
todo.txt
.txt
[todo.txt]
1
/tmp/ziman_manual_fs_example/notes/todo.txt
18
false
false
```

## Errors

### `FsError`

```
pub enum FsError {
    NotFound(string)
    Denied(string)
    IsDirectory(string)
    NotDirectory(string)
    Exists(string)
    Other(string)
}
```

What went wrong, in the shape `net` uses for `NetError`. The three a program actually branches on --
"it is not there", "I am not allowed", "that is a directory" -- were reachable only by matching
substrings of `strerror`, which is neither stable across platforms nor meant to be parsed. `Exists`
covers the create-a-thing-that-is-already-there case; anything else is `Other`, carrying the OS's
message unchanged.

### `fs_error_message`

```
pub fn fs_error_message(e: FsError) -> string
```

The message to show a human. Every variant already carries the OS's own wording, so this adds nothing
but passes it through uniformly -- the counterpart of `net_error_message`.

## Text files

### `read_text`

```
pub fn read_text(path: string) -> Result<string, FsError>
```

### `lines`

```
pub fn lines(path: string) -> Result<Iter<Result<string, string>>, FsError>
```

### `chunks`

```
pub fn chunks(path: string, size: int) -> Result<Iter<Result<[byte], string>>, FsError>
```

### `walk`

```
pub fn walk(dir: string) -> Result<Iter<Result<string, string>>, FsError>
```

### `write_text`

```
pub fn write_text(path: string, text: string) -> Result<int, FsError>
```

### `append_text`

```
pub fn append_text(path: string, text: string) -> Result<int, FsError>
```

### `write_text_atomic`

```
pub fn write_text_atomic(path: string, text: string) -> Result<int, FsError>
```

## Binary files

### `read_bytes`

```
pub fn read_bytes(path: string) -> Result<[byte], FsError>
```

### `write_bytes`

```
pub fn write_bytes(path: string, data: [byte]) -> Result<int, FsError>
```

### `append_bytes`

```
pub fn append_bytes(path: string, data: [byte]) -> Result<int, FsError>
```

## Inspecting

### `exists`

```
pub native fn exists(path: string) -> bool
```

True if something (a file, directory, ...) exists at `path`.

### `is_dir`

```
pub native fn is_dir(path: string) -> bool
```

True if `path` is a directory.

### `is_file`

```
pub native fn is_file(path: string) -> bool
```

True if `path` is a regular file.

### `size`

```
pub fn size(path: string) -> Result<int, FsError>
```

### `modified_seconds`

```
pub fn modified_seconds(path: string) -> Result<int, FsError>
```

### `list_dir`

```
pub fn list_dir(path: string) -> Result<[string], FsError>
```

### `resolve`

```
pub fn resolve(name: string) -> Option<string>
```

`name` (a file or a directory), found by searching `ZIMAN_RESOURCE_PATH` -- a colon-separated list of
directories, checked in order, the first one that has `name` winning -- the same idea as `PATH`, or a
Java classpath. With `ZIMAN_RESOURCE_PATH` unset, `name` is returned exactly as given, unchecked, so a
program that never sets it behaves exactly as if this function did not exist: whatever it does next
with the path (`read_text`, `exists`, `list_dir`, ...) fails or succeeds precisely as it always did.
With it set and no listed directory having `name`, `None`. `properties.load` and `migrate.run` both
resolve their own file/directory argument through this, so `application.properties` and a migrations
directory follow the same convention as anything else that calls this directly.

## Changing

### `remove`

```
pub fn remove(path: string) -> Result<unit, FsError>
```

### `make_dir`

```
pub fn make_dir(path: string) -> Result<unit, FsError>
```

### `make_dirs`

```
pub fn make_dirs(path: string) -> Result<unit, FsError>
```

### `rename`

```
pub fn rename(from: string, to: string) -> Result<unit, FsError>
```

### `copy_file`

```
pub fn copy_file(from: string, to: string) -> Result<int, FsError>
```

## Paths

### `path_join`

```
pub fn path_join(pieces: variadic string) -> string
```

Joins path pieces with single slashes: `path_join("a/", "/b", "c.txt")` is "a/b/c.txt". A
piece that starts with "/" after the first one is treated as relative, not as a new root.

### `file_name`

```
pub fn file_name(path: string) -> string
```

The last segment of a path: `file_name("a/b/c.txt")` is "c.txt". A trailing slash is ignored.

### `parent_dir`

```
pub fn parent_dir(path: string) -> string
```

Everything before the last segment: `parent_dir("a/b/c.txt")` is "a/b"; "." for a bare name,
"/" for a top-level absolute path.

### `extension`

```
pub fn extension(path: string) -> string
```

The extension of the last segment including the dot, or "" if it has none:
`extension("a/b.tar.gz")` is ".gz"; `extension(".bashrc")` and `extension("a/b")` are "".
