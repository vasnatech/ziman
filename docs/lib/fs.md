# `fs`

The filesystem: files, directories and paths.

Every function that can fail returns a `Result` carrying the operating system's own message
("cannot write 'x/y': No such file or directory"), so a missing directory, a permission
problem or a full disk is something a program handles, not a crash. An action with no value
to report returns `Ok(0)` (or `Ok(bytes)` for the ones that write), the same shape `net`
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

## Text files

### `read_text`

```
pub native fn read_text(path: string) -> Result<string, string>
```

The whole file as a string. An Err if it cannot be read or contains a NUL byte.

### `lines`

```
pub native fn lines(path: string) -> Result<Iter<Result<string, string>>, string>
```

The lines of a file, read lazily as a `for` loop asks for them, so a file of any size uses memory
for one line: `val lines = fs.lines(path)?` then `for line in lines { val text = line? ... }`. Each item is a
`Result<string, string>`: the line without its `\n` or `\r\n` (a final line with no newline counts,
an empty file has no lines), or an Err for a line with a NUL byte (a string cannot hold one) or a read
error, after which the iteration ends. The Err on the call itself is the file failing to open.
Leaving the loop early closes the file.

### `chunks`

```
pub native fn chunks(path: string, size: int) -> Result<Iter<Result<[byte], string>>, string>
```

The bytes of a file in blocks of `size` (1 to 1073741824), read lazily as a `for` loop asks for them, so
a file of any size uses memory for one block: for copying, hashing or uploading a large file. Each item
is a `Result<[byte], string>`: a block of exactly `size` bytes, except the last, which may be shorter (an
empty file has none); a read error is a last Err item. The Err on the call is the file failing to open
or a `size` out of range. Leaving the loop early closes the file.

### `walk`

```
pub native fn walk(dir: string) -> Result<Iter<Result<string, string>>, string>
```

The files under a directory, found lazily and depth first, each directory's entries in sorted order:
`val files = fs.walk(dir)?` then `for file in files { val path = file? ... }`. Each item is a
`Result<string, string>`: a path made of `dir` and the names below it, for anything that is not a
directory (a file, a link, a device); a subdirectory that cannot be read is an Err item and the walk
carries on. Directories themselves are not listed, and a symbolic link to a directory is a path, not
followed, so a cycle cannot loop. The Err on the call is `dir` failing to open. Leaving the loop early
releases it.

### `write_text`

```
pub native fn write_text(path: string, text: string) -> Result<int, string>
```

Replaces the file's contents with `text`, creating the file. `Ok(bytes written)`. Not
atomic: a crash part-way leaves a partly written file (use `write_text_atomic` for that).

### `append_text`

```
pub native fn append_text(path: string, text: string) -> Result<int, string>
```

Adds `text` to the end of the file, creating it. `Ok(bytes written)`.

### `write_text_atomic`

```
pub native fn write_text_atomic(path: string, text: string) -> Result<int, string>
```

Replaces the file's contents so that a reader, or a crash, sees either the old contents or
the new ones, never a mixture: the text goes to a temporary file next to `path`, is flushed
to disk, and is renamed over it. `Ok(bytes written)`.

## Binary files

### `read_bytes`

```
pub native fn read_bytes(path: string) -> Result<[byte], string>
```

The whole file as bytes (any content, including NUL).

### `write_bytes`

```
pub native fn write_bytes(path: string, data: [byte]) -> Result<int, string>
```

Replaces the file's contents with `data`, creating the file. `Ok(bytes written)`.

### `append_bytes`

```
pub native fn append_bytes(path: string, data: [byte]) -> Result<int, string>
```

Adds `data` to the end of the file, creating it. `Ok(bytes written)`.

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
pub native fn size(path: string) -> Result<int, string>
```

The size of the file in bytes.

### `modified_seconds`

```
pub native fn modified_seconds(path: string) -> Result<int, string>
```

The time the file was last modified, in seconds since the Unix epoch.

### `list_dir`

```
pub native fn list_dir(path: string) -> Result<[string], string>
```

The names in a directory, sorted (no "." or ".."). Names only, not paths: join them with
`path_join`.

## Changing

### `remove`

```
pub native fn remove(path: string) -> Result<int, string>
```

Deletes a file or an *empty* directory.

### `make_dir`

```
pub native fn make_dir(path: string) -> Result<int, string>
```

Creates one directory; its parent must exist and it must not already exist.

### `make_dirs`

```
pub native fn make_dirs(path: string) -> Result<int, string>
```

Creates a directory and any missing parents (like `mkdir -p`); `Ok(0)` if it already
exists as a directory.

### `rename`

```
pub native fn rename(from: string, to: string) -> Result<int, string>
```

Moves or renames `from` to `to`, replacing `to` if it is a file.

### `copy_file`

```
pub native fn copy_file(from: string, to: string) -> Result<int, string>
```

Copies a file's contents (not its permissions) to `to`, replacing it. `Ok(bytes copied)`.

## Paths

### `path_join`

```
pub fn path_join(pieces: string...) -> string
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
