# `io`

> **Experimental** -- see docs/design/journal/'s "Experimental modules" entry. Generated from this
> module's own `#![experimental]` attribute, so this page cannot claim otherwise than the source does.

Byte streams: a pull source of `[byte]` chunks, one error for every layer over it, and
the layers themselves.

**A byte source is an `Iter<Result<[byte], IoError>>`** -- a convention, not a type of its own, so
that `for chunk in source` and `iter_close` keep meaning what they mean everywhere else. A chunk
is never empty, `None` is a clean end, and an `Err` is terminal: a consumer that sees one stops
and closes. See docs/design/byte-streams.md for why this is a convention and not a `struct
Source`, and for everything here that is deliberately absent.

**The shape existed three times before this module did** -- `fs.chunks`, `fs.lines` and
`client.stream` -- with two different error types for the same kind of failure and nothing that
composed. This is the name, the one error, and the combinators.

**Reading a whole value is still the normal thing to do.** `drain` exists to go back to `[byte]`
in one call, and most of this library should keep taking one: a SHA-256 of a token and a base64 of
a key are whole-value operations, and a stream buys nothing there. What a stream is for is the
case where **the size is the caller's rather than ours**: an uploaded file, an archive entry, a
response being read from someone else's server.

`use io` -- or `use io.Name` to bring one item in bare.

## Example

```
use io
use io.IoError

// A byte source is an `Iter<Result<[byte], IoError>>`: pull one chunk at a time, so a stream of any
// size costs one chunk of memory. `of_bytes` makes one from bytes already in hand, which is how a
// whole-value caller reaches the layers.
fn demo() -> Result<unit, string> {
    // `drain` goes back to a whole `[byte]`, with a ceiling (negative for none). It is what hands
    // the bytes to anything that still takes them whole -- a parser, a hash, a codec.
    val whole = try map_err(io.drain(io.of_bytes(to_bytes("hello")), -1), io.io_error_message)
    print(try bytes_to_string(whole))

    // `limited` is the ceiling, as a layer. **Put it on the side that grows**: over a decompressor,
    // not under it, since a bomb is small going in.
    val capped = io.drain(io.limited(io.of_bytes(to_bytes("hello")), 3), -1)
    print(switch capped {
        Ok(_) { "under the ceiling" }
        Err(e) { io.io_error_message(e) }
    })

    // `text` decodes UTF-8 and holds back a character split across two chunks, which is the one job
    // Java needs a whole `Reader` hierarchy for. `lines` reads it a line at a time, `\r\n` or `\n`.
    for line in io.lines(io.of_bytes(to_bytes("one\r\ntwo\nthree"))) {
        print(try map_err(line, io.io_error_message))
    }
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("io example failed: {}\n", e)
    }
}
```

Output:

```
hello
the stream is too large: more than 3 bytes
one
two
three
```

### `IoError`

```
pub enum IoError {
    Timeout
    Closed(string)
    TooLarge(string)
    Malformed(string)
    Failed(string)
}
```

Why a stream stopped early. **Five variants, each named after the answer a server gives**, which
is the one thing a caller actually has to decide from an I/O failure:

| | | |
|---|---|---|
| `Timeout` | the source stopped producing within its deadline | 408 |
| `Closed` | the other end went away mid-stream | nothing to answer; drop |
| `TooLarge` | a `limited` ceiling was passed | 413 |
| `Malformed` | the bytes are not what this layer reads | 400 -- the caller's data |
| `Failed` | a disk, a socket, an allocation | 500, logged -- ours |

**`Closed` and `Failed` are different on purpose**, though both mean "it stopped": a client that
hung up is nothing to answer and nothing to log, and a disk that failed is both.

### `io_error_message`

```
pub fn io_error_message(e: IoError) -> string
```

The message to show a human, the counterpart of `fs_error_message` and `net_error_message`.

## Starting one

### `of_bytes`

```
pub fn of_bytes(data: [byte]) -> Iter<Result<[byte], IoError>>
```

Bytes already in hand, as a source: one chunk and then the end.

**This is how a whole-value caller reaches the layers.** `compress`'s ceiling is `limited` now
rather than a parameter of its own (docs/design/byte-streams.md §6.1), so decompressing a `[byte]`
under a cap reads `drain(limited(inflating(of_bytes(data)), max), max)` -- and every fixture for
this module starts here.

### `mapped`

```
pub fn mapped<T, E, F>(
    source: Iter<Result<T, E>>,
    as_error: fn(E) -> F
) -> Iter<Result<T, F>>
```

A source whose items fail some other way, as a source that fails this way.

**This is the combinator the library was missing**, and `fs` is the proof: its chunks are produced
in C, whose error channel is a `string`, and the conversion had nowhere to happen -- `fs`'s own
`lift` converts the error of *opening* the file, because that one has already happened, where the
chunks' errors do not exist until the iterator produces them. So the `string` leaked to every
caller for want of this function.

**Generic in all three**, which it became while the retrofit was being written and is the honest
shape: this is "the error of every item, mapped", and nothing about it is specific to `IoError`.
`fs.lines` needs `string -> IoError` over `string` items, `fs.chunks` needs it over `[byte]` ones,
and `csv` needs `IoError -> string` because its own error vocabulary is text -- one function
serves all three, where an `IoError`-shaped one would have served the first two and left the third
writing its own.

## The ceiling

### `limited`

```
pub fn limited(source: Iter<Result<[byte], IoError>>, max: int) -> Iter<Result<[byte], IoError>>
```

The same source, refusing to produce more than `max` bytes in total: the chunk that would pass the
ceiling is answered `TooLarge` instead, and the stream ends there. A negative `max` is no ceiling,
which is `compress`'s own convention for the parameter this replaces.

**Put this on the side that grows.** A decompressor's output is what a bomb inflates, so
`limited(inflating(source), max)` is a guard and `inflating(limited(source, max))` is not -- it
caps the compressed bytes, which were never the problem. docs/design/byte-streams.md §6.1 has the
two lines side by side, and `compress`'s own documentation leads with the right one.

## Ending one

### `drain`

```
pub fn drain(source: Iter<Result<[byte], IoError>>, max: int) -> Result<[byte], IoError>
```

Every byte of the source, in one `[byte]`, with `max` as the ceiling (negative for none).

**The way back to the whole-value world, and the reason this module can arrive without rewriting
anything**: every existing codec, parser and `crypto` function takes a `[byte]`, and this is what
hands them one. It is also where a whole-value reader's ceiling lives now that `compress` does not
carry its own.

The source is closed, including when the ceiling or a chunk ends it early.

## Bytes as text

### `text`

```
pub fn text(source: Iter<Result<[byte], IoError>>) -> Iter<Result<string, IoError>>
```

The source decoded as UTF-8 text, in chunks that do not have to line up with its bytes.

**This is the one thing Java needs a whole parallel `Reader` hierarchy for**, and it is a function:
a multi-byte sequence split across two reads has to be held back until the rest of it arrives.
Decoding each chunk on its own would fail on a 4 KiB read that ends in the middle of a `ö`, which
is a bug that only appears on the files that happen to split there.

Bytes that are not UTF-8 at all are `Malformed`, and so is a sequence still unfinished at the end
of the stream -- a truncated document should be reported, not silently shortened.

### `lines`

```
pub fn lines(source: Iter<Result<[byte], IoError>>) -> Iter<Result<string, IoError>>
```

The source as text, a line at a time, with the line ending removed -- `fs.lines` over anything
rather than only over a file, which is what a CSV read off a socket or out of an archive entry
needs.

A `\r\n` and a bare `\n` both end a line; a last line with no ending is still a line. An empty
stream has no lines, and a stream that is just `"\n"` has one empty one.
