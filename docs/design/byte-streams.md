# Byte streams: one pull shape, one error, and the decorators that pay for it

Status: **proposed, 2026-11-02.** Nothing below is written. The shape in §4 and the error in §5 are
decisions taken before the first line, because both land in every signature; everything else is a
sketch to be argued with.

## 1. The pattern already exists three times, unnamed

```
fs.chunks(path, size) -> Result<Iter<Result<[byte],  string>>,      FsError>
fs.lines(path)        -> Result<Iter<Result<string,  string>>,      FsError>
client.stream(..)     -> Result<StreamResponse, ClientError>
                              // .body: Iter<Result<[byte], ClientError>>
```

So this document is not proposing a new concept. `Iter` is already built for it --
`iter_from(next: fn() -> Option<T>, close: fn())` is a pull sequence that **carries its own close**,
which is the part Java needs a `Closeable` interface and a `try`-with-resources statement for -- and
three modules already produce exactly this shape.

**The two levels of `Result` are right, and both libraries agree about them.** Failing before there
is a stream and failing partway through one are different events: `fs.chunks`' own comment draws the
line as "the Err on the call is the file failing to open or a `size` out of range" against "a read
error is a last Err item", and `client.stream` splits a `BadUrl` or an unreadable `Content-Length`
from a `Timeout` or a `Network` the same way. Nothing here proposes collapsing them.

What is missing is everything that would make the three useful together:

* **two different inner error types** for the same kind of failure -- `string` in `fs`,
  `ClientError` in `client`,
* **no combinators at all**: nothing buffers, nothing caps, nothing decompresses a source, nothing
  decodes one as text, nothing drains one back into `[byte]`,
* and **no consumer** in the library takes one. `xml.parse`, `json.parse` and `compress.inflate_raw`
  each take a whole `[byte]`.

Three producers, zero combinators, zero consumers. That is the gap.

**And the inner `string` in `fs` is an artifact of the missing combinator, not a decision**, which is
worth knowing before §11's first question reads as a redesign:

```
native fn chunks_raw(path, size) -> Result<Iter<Result<[byte], string>>, string>
pub    fn chunks(path, size)     -> Result<Iter<Result<[byte], string>>, FsError>   // lift(..)
```

The chunks are produced in C, whose error channel is `string`. `lift` converts the **outer** error
because that failure has already happened by the time it runs; the inner ones do not exist yet --
the iterator produces them lazily, long after `lift` returned -- and converting those needs a
mapping wrapper around an `Iter`, which nothing provides. So the `string` leaks to every caller.
`client.stream`'s chunks, by contrast, are produced by Ziman code constructing `ClientError` inside
the iterator, so structure cost it nothing.

That is the shape of this whole gap in one example: not a library that chose badly, a library with
no way to say the thing.

## 2. The evidence: two caps that are not decisions

The North Star backend documents two limits as parity gaps, and both are this gap:

1. **An Apple Health import must fit in memory twice.** `xml.parse` answers a `Document` -- the whole
   tree -- so an `export.xml` has to fit the request body cap *and then* fit as a tree. The Spring
   backend reads the same file with StAX, a pull parser, and has no such limit. A few years of a
   watch's heart rate is hundreds of megabytes; the backend sets `server.max-body-bytes` to 16 MiB,
   so that import answers **413**.
2. **A FHIR bulk import materializes every entry of its zip.** `compress.zip_entries` answers
   `[ZipEntry]` with each `body` inflated, so a zip of NDJSON is fully expanded in memory before the
   first line is read -- and NDJSON is the one format that needs nothing more than a line at a time.

Neither is a design choice anyone made. Both are the absence of a stream.

**What is not evidence:** 53 functions across `compress`, `crypto`, `encoding`, `json`, `xml` and
`codec` take or answer a whole `[byte]`, and most of them are right to. A SHA-256 of an access token
and a base64 of a 32-byte key are whole-value operations. The argument here is only about the places
where **the size is the caller's, not ours**: an uploaded file, a response being proxied, an archive
entry, a document being parsed.

## 3. What to take from Java, and what to leave

**Take: composition.** `new GZIPInputStream(new BufferedInputStream(in))` is the whole value of
`java.io`, and the FHIR bulk case is exactly that stack -- socket, zip entry, inflate, lines, a JSON
document per line. One mechanism, five layers, bounded memory at every one.

**Take: pull.** A pull source lets the *parser* be a layer. Push (SAX, a callback per element) turns
every consumer into a state machine, which is why StAX exists.

**Take: close on the stream itself**, which `Iter` already has.

**Leave: the four-quadrant hierarchy.** `InputStream`/`OutputStream`/`Reader`/`Writer` exists because
Java bolted charsets on in 1.1 and inheritance was its only tool for composition, so every decorator
had to be written twice. Here a decorator is a function `fn(Iter<..>) -> Iter<..>` and gets written
once.

**Leave: `read(buf, off, len) -> int`, with `-1` for end.** `Option<[byte]>` says it in the type, and
cannot be misread as a length.

**Leave: `mark`/`reset`/`available`.** `available()` is famously not the number of bytes there are,
and `mark` is most of why `BufferedInputStream` is complicated. A parser that needs to look ahead
should buffer explicitly (§6).

**Leave: `Reader` as a parallel world.** It buys exactly one thing worth having -- decoding UTF-8
across a chunk boundary, when a multi-byte sequence is split between two reads. That is a function,
not a hierarchy, and `fs.lines` must already solve it internally; whatever it does should be the one
implementation.

## 4. The shape: a convention over `Iter`, not a new type

> A byte source is an `Iter<Result<[byte], io.IoError>>`. A chunk is a non-empty slice; `None` is a
> clean end; an `Err` ends the stream.

**A convention rather than a `struct Source`**, so that `for chunk in source` keeps working, so
`iter_close` keeps meaning what it means, and so the three existing producers need no wrapper. A
struct would buy a content-length hint and cost a second concept that every signature has to spell.

Two rules the combinators can then rely on:

* **a chunk is never empty** -- an empty slice is not a "nothing yet" signal, because a pull source
  has no such state. A producer with nothing yet blocks or fails.
* **an `Err` is terminal**: after one, a consumer stops and closes. Resumable sources are a thing
  some protocols want, and nothing here needs one; saying so now keeps every combinator simple.

## 5. `io.IoError`: one error, five variants, each named after an answer

This was the decision to make first, because it appears in every signature and is the hardest to
change later. The alternatives were a generic `Iter<Result<[byte], E>>` and `fs`'s current
`Result<[byte], string>`:

* **Generic `E` composes badly.** Every combinator becomes generic over it, and stacking a file
  source under a gzip decoder that fails its own way needs a sum of the two anyway -- so the sum
  arrives, just later and with each caller inventing its own.
* **`string` throws away what callers branch on.** A timeout, a bad gzip header and a disk failure
  are three different answers to a client.

So: one enum, and its variants are named after **what the caller answers**, which is the convention
`ServiceError` already follows in the North Star:

| variant | the failure | what a server does with it |
|---|---|---|
| `Timeout` | the source stopped producing within its deadline | 408 |
| `Closed(string)` | the other end went away mid-stream | nothing to answer; drop |
| `TooLarge(string)` | a `limited` ceiling was passed | 413 |
| `Malformed(string)` | the bytes are not what this layer reads: bad deflate, bad UTF-8, a zip with no directory | 400 -- the caller's data |
| `Failed(string)` | the underlying thing failed: a disk, a socket, an allocation | 500, logged -- ours |

`io_error_message(e)` joins the `*_error_message` family and the `error_message` builtin dispatches
to it, as it does for `ClientError` and `DbError`.

**Converters, not a union of unions.** `FsError`, `NetError` and `ClientError` keep their own types
and `io` offers `io.of_fs`, `io.of_net`, `io.of_client` mapping each onto the five above --
`NetError.Timeout` to `Timeout`, `NetError.Closed` to `Closed`, `NetError.Tls` to `Failed`, and so
on. A `#[from] Net(NetError)` variant would preserve more structure and make every caller match
twice to find out whether it is their fault or ours, which is the question the five answer.

## 6. The combinators, and the caller each one has

Nothing here is included for symmetry; each line names what asks for it.

| | |
|---|---|
| `io.drain(source, max) -> Result<[byte], IoError>` | **the migration path.** Turns a source back into the `[byte]` every existing codec, parser and `crypto` function takes, so streams can arrive without rewriting any of them. The first consumer of every new source. |
| `io.buffered(source, size)` | `xml.events` asks for a few bytes at a time; a 4-byte read must not become a 4-byte syscall. |
| `io.limited(source, max)` | the ceiling that is currently a `max_size` parameter on nine `compress` readers. As a layer it guards a source the decompressor never sees -- which is where a zip bomb should be stopped. |
| `io.text(source) -> Iter<Result<string, IoError>>` | UTF-8 across chunk boundaries: the one thing `Reader` is for. |
| `io.lines(source)` | `fs.lines` off the filesystem. The FHIR bulk reader wants exactly this over a zip entry. |
| `compress.inflating(source)` / `compress.gunzipping(source)` | the decorator that makes a zip of NDJSON readable without expanding it. |
| `xml.events(source) -> Iter<Result<Event, IoError>>` | the Apple Health import, and §8. |

**`xml.parse` should be rewritten over `xml.events`**, not kept beside it: a tree builder that folds
events into a `Document` is a dozen lines, and two parsers for one grammar is how the two drift. The
same question will arise for `json`, where a whole-document parse is right far more often -- NDJSON
already streams by line, so `json` is not on the critical path.

## 7. Sinks are deliberately not symmetric

`java.io` has an `OutputStream` for every `InputStream`, and that symmetry is not earned here:

* **Inputs are unbounded; outputs are generated.** A user uploads whatever their watch produced; a
  response is something this server made, and the biggest of them (an export of a whole record) is
  bounded by the record.
* **`codec.Encoder` is `T -> [byte]` on purpose**, and its own documentation argues why: a format
  joins by being able to write *a value*, not by being able to write into a stream. A streaming
  encoder would be a second interface beside it, not a replacement.

So `io.Sink` waits for a caller that needs it -- the likeliest being an export of a record too large
to hold, or proxying a response body through. When it comes, it is `fn([byte]) -> Result<unit,
IoError>` plus a close, and the same combinators mirrored.

## 8. The hard half: the server's request body

**A streaming parser alone removes nothing.** `server` reads a request body in one pass into a
`[byte]`, capped by `max_body_bytes` -- its own comment says it "streams nothing" -- and `rest` hands
a handler an already-parsed `Request`. That cap is the wall a user hits, not the parser.

Three ways out, in the order I would try them:

1. **Spill above a threshold.** `serve` keeps reading the body, but past N bytes it writes to a
   temporary file and the handler is given `fs.chunks` over it. The cap becomes a disk question
   rather than a memory one, `Request` keeps its shape for every one of the 200-odd endpoints that
   wants a small body, and nothing about keep-alive changes. Costs a write and a read of the upload.
2. **A socket-backed body.** No copy, and the handler must drain or close it before the connection's
   next request -- which keep-alive and pipelining make a contract, not a detail. This is where it
   should end up, and it wants §4 settled first.
3. **A streaming route family in `rest`**, so only upload endpoints opt in and the other 200 are
   untouched. Mostly a consequence of (2) rather than an alternative to it.

**Zip is the one format where the layer cannot be a plain decorator**, and `compress.zm`'s header
already explains why: a ZIP's index is the central directory *at the end of the file*, which is what
makes it readable at all when an entry was stream-written with a data descriptor. A streaming zip
reader must either buffer to the end to find the directory -- which is the thing we are trying not to
do -- or walk local headers and handle the descriptor case. That is a design decision of its own, and
the honest first answer for the backend is to spill the upload to a file (1) and keep reading the
directory.

## 9. Staging

1. `lib/core/io.zm`: `IoError`, `io_error_message`, the converters, `drain`, `buffered`, `limited`,
   `text`, `lines`. No producer changes; `drain` makes it immediately usable.
2. `compress.inflating`/`gunzipping` over a source, with `limited` taking over the ceiling.
3. `xml.events`, and `xml.parse` rewritten as a fold over it.
4. `server`: the body spilled above a threshold, handed to a handler as a source (§8.1).
5. The North Star's Apple Health import rewritten from "parse, then walk the tree" to "handle each
   element as it arrives", and its FHIR bulk reader from `zip_entries` to a per-entry source.

Steps 1-3 are a library with fixtures and no callers waiting; step 4 is where the parity gap closes.

## 10. Non-goals

* **Asynchrony.** This is pull and blocking, on the thread that asked. `serve` already has a thread
  per connection and a bounded pool; nothing here proposes changing that.
* **Seeking or random access.** A zip's directory wants it (§8) and nothing else does.
* **Resumable sources.** An `Err` is terminal (§4).
* **A `Channel` or actor-shaped stream.** `Iter` is a sequence, not a queue between threads.
* **Converting the 53 whole-`[byte]` functions.** Most are right as they are (§2).
* **Replacing `codec`.** §7.

## 11. Open questions

1. **Retrofit or convert at the edges?** Changing `fs.chunks`, `fs.lines` and `client.stream` to
   `IoError` is a breaking change to three public signatures, against three `io.of_*` calls at every
   use site forever. I would retrofit -- the modules are young and the inconsistency is the thing
   this document exists to remove -- but it is a change to published API.

   **§1 makes this cheaper than it looks for `fs`.** Its inner `string` is there because the
   conversion had nowhere to happen, so the retrofit is `io.map_err` over the `Iter` the native
   already returns -- the same combinator §6 needs anyway -- and not a change to how the bytes are
   produced. `client.stream` is the one that is genuinely a signature change, since its chunks carry
   a `ClientError` that some caller may be matching on today.
2. **Does `limited` replace `compress`'s `max_size` parameters, or sit beside them?** Replacing them
   is cleaner and makes nine signatures shorter; it also means a caller who forgets the layer has no
   ceiling, where today the parameter cannot be forgotten.
3. **`io.text` and `fs.lines`: one implementation or two?** Whatever `fs.lines` does about a
   multi-byte sequence split across a read should move into `io` and be called from there.
4. **Is the threshold in §8.1 a setting?** `server.spill-body-bytes` beside `max-body-bytes`, or a
   constant with a comment.
