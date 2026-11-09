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

Bytes already in hand, as a source, in chunks of `chunk_bytes`.

**This is how a whole-value caller reaches the layers.** `compress`'s ceiling is `limited` now
rather than a parameter of its own (docs/design/byte-streams.md §6.1), so decompressing a `[byte]`
under a cap reads `drain(limited(inflating(of_bytes(data)), max), max)` -- and every fixture for
this module starts here.

**It chunks rather than answering once**, for the reason `chunk_bytes` gives: a reader over this
should cost what a reader costs, not what holding the whole value costs. Anything shorter than
`chunk_bytes` is still exactly one chunk, so nothing about a small value changed.

### `stdin`

```
pub fn stdin() -> Iter<Result<[byte], IoError>>
```

**The process's standard input, as a source** -- so that `io.text`, `io.lines`, `io.cursor`,
`limited` and every parser built over them read a pipe exactly as they read a file.

This is the piece that was missing for a program at the end of a pipeline. Until it existed the
way to read standard input was `fs.read_text("/dev/stdin")`, which is three separate lies: it is
Linux-only, it reads the whole input into memory before the first byte can be looked at, and it
fails outright on a pipe on some systems rather than streaming it. `curl .. | prog` is the normal
shape of a command-line tool and it should cost one chunk of memory, not the size of the input.

**Three things about it are not like `fs.chunks`,** all for the same reason -- a file descriptor
the process was handed is not a file this function opened:

* **It cannot fail to start.** There is no `Result` on the call, because there is nothing to
open: descriptor 0 either reads or its first chunk is an `Err`. A redirection that went wrong is
the caller's shell's problem and arrives here as a read failure.
* **Two calls are two views of one stream, not two streams.** Each has its own end-of-input flag,
but the bytes are consumed from the same descriptor, so reading both interleaves them. Call it
once and pass the source along.
* **Closing it does not close descriptor 0.** `iter_close` ends *this* source -- a later
`io.stdin()` still reads, and a child process still inherits a working standard input. Closing the
process's own input out from under itself is not a thing a `for` loop leaving early should be able
to do.

Chunks are `chunk_bytes` at most and usually smaller: a pipe hands over what has been written so
far, and a terminal a line at a time. **The never-empty rule is kept here** -- the native says end
of input with an empty answer, because C cannot build an `Option`, and this turns that into the
`None` the convention promises. Once it has ended, by input or by error, it stays ended.

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

## A range of one

### `taking`

```
pub fn taking(source: Iter<Result<[byte], IoError>>, n: int) -> Iter<Result<[byte], IoError>>
```

The first `n` bytes of the source and no more: the chunk the count lands inside is split, and the
stream ends there. A negative `n` is the whole source.

**This is not `limited`, and the difference is the error.** `limited` refuses a source that is
larger than it should be -- the chunk that would pass the ceiling becomes a `TooLarge` -- because
its caller is guarding against a bomb. This one *cuts a source to size* for a caller that knows
how long the piece it wants is: a part of a multipart body, a range of a file. Nothing has gone
wrong when the source holds more, so nothing is reported.

### `skipping`

```
pub fn skipping(source: Iter<Result<[byte], IoError>>, n: int) -> Iter<Result<[byte], IoError>>
```

The same source with its first `n` bytes dropped: the chunk the count lands inside is split and
its remainder is the first chunk out. A negative or zero `n` changes nothing.

**It reads what it skips**, because a source is a pull sequence and has no seek (§10): skipping
40 MB means reading 40 MB and discarding it. That is why `taking(skipping(s, from), length)` is a
cheap way to read a range near the *start* of something and an expensive way to read one near its
end -- and why a format that can arrange to want the first range should.

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

## A cursor for a parser

### `Cursor`

```
pub struct Cursor {
    source: Iter<Result<string, IoError>>
    // The window, as characters, and the absolute position of its first one.
    window: Mutable<[string]>
    base: Mutable<int>
    // Whether the source is finished, and the first failure it reported -- kept rather than thrown
    // so that the helpers a scanner calls can stay the plain `bool` and `string` answers they were
    // over an array. A scanner checks `failure` once, at the end; a read error cannot be mistaken
    // for a document that simply stopped, because `parse` asks.
    ended: Mutable<bool>
    failure: Mutable<Option<IoError>>
    // Newlines dropped with the window, so `line` stays right as it advances.
    lines_before: Mutable<int>
    // **How `line` stays cheap.** Newlines are counted once, forward, as positions are asked
    // about: `counted_to` is how far that has reached and `lines_counted` is the count before it.
    // A scanner asks about increasing positions -- each element's opening line, in order -- so the
    // counting is linear over the document rather than linear *per question*. Scanning the window
    // on every call put 82% of a profile in this function and made a parse quadratic.
    counted_to: Mutable<int>
    lines_counted: Mutable<int>
    // The position before which nothing will be asked for again. **`release` only moves this**;
    // the characters are dropped later, by `compact`, which is what keeps releasing cheap -- see
    // `release`.
    live: Mutable<int>
}
```

A window over a text source, addressed by **absolute character position**, so that a parser can
look ahead, take a span it has already passed, and ask what line a position is on -- over a
stream it never holds whole.

**This is what `buffered` should have been.** The design document proposed a chunk combinator and
justified it with "a parser asks for a few bytes at a time"; a consumer of an `Iter` takes whole
chunks and cannot ask for four characters, so the thing a parser actually needs is this: random
access over a *bounded window*, and a way to say what it will not look at again.

**Characters, not bytes**, because that is what a text parser indexes: `xml`'s scanner decoded its
whole document to a `[string]` of characters for a measured reason -- reaching position `p` with
`char_at` walks the string, so threading a character position over a document was quadratic in the
document, and 40 KB of CJK XML took 2.2 s. The window keeps that property over a stream.

**`release` is what makes it bounded, and the bound is `max(the largest chunk, what has not been
released)`.** Nothing is dropped until a caller says a position will not be asked for again, so a
parser that never releases holds the whole document -- correct, and no better than reading it
whole. A scanner releases as it finishes each token.

**A chunk is the floor, and that is not a detail.** The cursor cannot hold less than the piece its
source handed it: `of_bytes` of a whole document is one chunk, so the window is the document --
which is honest, since it already was a `[byte]`. Over a source that delivers pieces, the window
is a piece: `xml_events` reads a 26,309-character document in 512-byte chunks and the window
never passes 510, with the one-chunk case measured beside it for the contrast.

### `cursor`

```
pub fn cursor(source: Iter<Result<string, IoError>>) -> Cursor
```

A cursor over a text source.

### `at`

```
pub fn at(c: Cursor, p: int) -> string
```

The character at `p`, or `""` past the end -- the answer a scanner's bounds check wants, rather
than an error it would have to thread through every helper.

### `has`

```
pub fn has(c: Cursor, p: int) -> bool
```

Whether there is a character at `p`.

### `starts`

```
pub fn starts(c: Cursor, p: int, needle: [string]) -> bool
```

Whether the text at `p` is `needle`, character by character.

### `span`

```
pub fn span(c: Cursor, a: int, b: int) -> string
```

The text of characters `a` up to `b`. Empty when `a` is behind the window, which a scanner that
releases correctly never asks for.

### `find`

```
pub fn find(c: Cursor, needle: [string], from: int) -> int
```

The position of the first `needle` at or after `from`, or -1.

**This is the one operation that can grow the window without bound**, since a needle that is not
there is only known absent at the end of the document -- a text run with no `<` after it, for
instance. A caller that reads untrusted input puts `limited` under the cursor, which is where that
ceiling belongs.

### `run_until`

```
pub fn run_until(c: Cursor, stops: [string], from: int) -> int
```

Where a run of characters that are none of `stops` ends: the position of the first character in
`stops` at or after `from`, or **the end of the document** when there is none. Never -1, which is
the difference from `find` and the reason this exists beside it -- a scanner reading a run wants
one number for "here is where it stops", and asking twice (find, then a bounds check) is what
made the character-at-a-time version expensive.

**It reads the window once per chunk, not once per character.** `at` has to re-read `base` and
`window` on every call because anything may have moved between calls; a run cannot move under
itself, so this hoists both out of the loop and indexes the array directly, going back for more
only when it reaches the end of what is held. That is the whole optimisation: the same work with
two cell reads and a call per *chunk* instead of per character (docs/reviews/tasks/ F263).

Unlike `find`, this cannot grow the window past the first stop -- so a scanner whose stop set
includes the character that ends what it is reading is bounded by the thing it is reading, not by
the document.

### `release`

```
pub fn release(c: Cursor, upto: int)
```

Says that nothing before `upto` will be asked for again, so it can be dropped. **This is the
only thing that bounds a cursor's memory**, and a scanner calls it as it finishes each token.

**It records the position and drops nothing**, which is the difference between a cursor that works
and one that does not: dropping eagerly means copying the rest of the window on every call, and a
scanner calls this once per token. Measured, because the first version did drop eagerly: a 235 KB
document parsed in **75 seconds**, against 463 ms for the same scanner with no releasing at all --
ten thousand events each copying a hundred thousand characters. `compact` does the dropping when
the dead prefix is at least half the window, which makes it amortised constant per character, the
same bargain a growable array makes in the other direction.

### `line`

```
pub fn line(c: Cursor, p: int) -> int
```

Which line position `p` is on, counting from 1 -- for a parser's error messages.

**Counted forward once, not scanned per call.** A scanner asks about increasing positions, so the
common path advances a counter from where it last reached to `p`; the total work over a document
is one pass. Scanning the window from its base on every call is the same answer and was 82% of a
profile, turning a parse into a quadratic -- a cursor's window can be the whole document when
nothing has released, and an element's opening line was asked for per element.

A position already released answers the line the window now starts on, which is the closest true
answer still available. A scanner reporting an error at a position it has released is reporting
it late.

### `window_size`

```
pub fn window_size(c: Cursor) -> int
```

How many characters the window is holding.

**Here because the bound is this type's whole claim.** `release` is documented as the only thing
that keeps a cursor from holding the document, and a claim with no way to observe it is a
comment: `io_cursor` walks a 2,000-character stream twice, releasing in one walk and not in the
other, and asserts the difference. A caller tuning its own release discipline reads this for the
same reason.

### `failure`

```
pub fn failure(c: Cursor) -> Option<IoError>
```

The read failure this cursor saw, if any. **A scanner checks this before calling a document
well-formed**: a source that failed halfway looks like a document that ended, and the two are not
the same answer.

### `cursor_close`

```
pub fn cursor_close(c: Cursor)
```

Closes the source under the cursor.
