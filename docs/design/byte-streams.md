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
| ~~`io.buffered(source, size)`~~ | **Withdrawn while writing §14.** The justification was "`xml.events` asks for a few bytes at a time" -- but a consumer of an `Iter` takes whole chunks and cannot ask for four bytes, so what a parser needs is a **cursor** over a source, not a chunk combinator. It waits for the parser, by this document's own rule that every line names its caller. |
| `io.limited(source, max)` | **the one ceiling.** Replaces the `max_size` parameter on `compress`'s readers (§6.1), so a limit is one concept rather than a convention repeated per function. |
| `io.text(source) -> Iter<Result<string, IoError>>` | UTF-8 across chunk boundaries: the one thing `Reader` is for. |
| `io.lines(source)` | `fs.lines` off the filesystem. The FHIR bulk reader wants exactly this over a zip entry. |
| `compress.inflating(source)` / `compress.gunzipping(source)` | the decorator that makes a zip of NDJSON readable without expanding it. |
| `xml.events(source) -> Iter<Result<Event, IoError>>` | the Apple Health import, and §8. |

### 6.1 `io.limited` replaces `compress`'s ceilings, and the order is the whole point

**Decided: replace them.** `compress`'s header states the rule those parameters exist for --
"decompressing untrusted data needs a size limit, a few kilobytes can expand to gigabytes" -- and
nine functions each carrying their own `max_size` is that one rule spelled nine times. A layer says
it once.

**But a ceiling belongs on the output, and as a layer it can be put on the wrong side.** The
parameter could not be: `max_size` on `inflate_raw` means "the most bytes it will produce". As a
layer:

```ziman
io.limited(compress.inflating(source), zip_max_entry_bytes)   // right: caps what comes out
compress.inflating(io.limited(source, zip_max_entry_bytes))   // wrong: caps what goes in
```

A zip bomb is small going in and enormous coming out, so the second line reads like a guard and is
none -- it caps the compressed bytes, which were never the problem. This is the cost of the
decision and it is accepted with two mitigations: `compress.inflating`'s own documentation leads
with the correct composition, and a fixture feeds a bomb through the right order and requires
`TooLarge`, so the guard is pinned by a test rather than by a reader's memory.

**For a whole-value reader the ceiling becomes `drain`'s**, which is why nothing is actually lost:
`io.drain(source, max)` already takes one (§6), so `gunzip(data, max)` stays a call with a limit in
it -- the limit is just `drain`'s now instead of a parameter `compress` invented. **The ceiling never
disappears from a signature that decompresses untrusted bytes**, and it should not: a
`decompress(data)` with no limit anywhere in sight is the DoS the module's header warns about, and
"wrap it in `limited` yourself" is not a thing an API should rely on a caller remembering when there
is no stream in view to wrap.

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

1. ~~**Retrofit or convert at the edges?**~~ **Decided: retrofit** -- the inconsistency is the thing
   this document exists to remove, and leaving it means `io.of_*` at every use site forever. §12 has
   the shape, the blast radius and the one refinement that halves it.
2. ~~**Does `limited` replace `compress`'s `max_size` parameters, or sit beside them?**~~
   **Decided: replace them** -- one ceiling, not nine. §6.1 has the reasoning, the ordering hazard
   it introduces (a limit on the input side reads like a guard and is none), and the boundary: a
   whole-value reader of untrusted bytes keeps a limit in its signature, because that limit is
   `drain`'s and there is no stream in view for a caller to wrap.
3. **`io.text` and `fs.lines`: one implementation or two?** Whatever `fs.lines` does about a
   multi-byte sequence split across a read should move into `io` and be called from there.
4. ~~**Is the threshold in §8.1 a setting?**~~ **Decided: a setting** -- §13.

## 12. The retrofit: the inner error only

**Each module keeps its own outer error.** That is the refinement that halves the change, and it is
also the honest reading of §1's two levels:

```
fs.chunks(path, size) -> Result<Iter<Result<[byte], io.IoError>>, FsError>
client.stream(..)     -> Result<StreamResponse, ClientError>
                              // .body: Iter<Result<[byte], io.IoError>>
```

`FsError`'s `NotFound` and `PermissionDenied` say more about a file that will not open than any of
`IoError`'s five ever could, and `ClientError`'s `BadUrl` and `TooManyRedirects` say more about a
request that could not be made. Those are failures of *starting*, they are each module's own
subject, and flattening them into `IoError` would lose detail to buy nothing -- `io.of_fs` and
`io.of_client` exist for the caller who wants one type.

What changes is the **chunk**, where the two modules disagree today and where the five variants are
exactly right.

**For `client` the mapping is total**, which is why nothing is lost: its mid-stream variants are
`Timeout` -> `Timeout`, `Network` -> `Closed` or `Failed`, `BadResponse` -> `Malformed`, `TooLarge`
-> `TooLarge`. For `fs` there is nothing to map from -- the `string` becomes a classified error for
the first time, which means the native's text lands in `Failed(..)`/`Malformed(..)` and a caller can
finally tell a disk failure from a truncated read.

**The blast radius, counted rather than estimated** -- every in-tree consumer of the three:

| | |
|---|---|
| `lib/core/fs.zm`, `lib/http/client/{stream,read}.zm` | the producers |
| `lib/core/csv.zm` | reads `fs.lines`; the one real library consumer |
| `lib/prelude/iter.zm` | prose naming `Iter<Result<string, string>>` as what `fs.lines` answers |
| `docs/lib/examples/core/fs_example.zm`, `.../process_example.zm`, `.../http/client_example.zm`, `.../prelude/iter_example.zm` | four examples, two of them only in prose |
| `tests/lang/fs_lines*.zm` and the chunk fixtures | recordings |
| the North Star's `service/audit.zm` | reads `fs.lines` for the log viewer |

Nine files and some prose, one of them outside this repository. Editing an example forces
`make corelib-reference` and a commit before `make check` passes, so the retrofit is one commit with
the regenerated manuals in it.

**`csv` is also the fourth caller this design has been looking for.** It consumes `fs.lines` today,
so `csv.records(source)` over `io.lines` would read a CSV off a socket or out of a zip entry -- which
is what the North Star's Omron and glucometer imports do through a whole-body `multipart` upload
now.

## 13. The spill threshold is a setting, and it moves what the cap bounds

Two levels, because `server` is a library and does not read properties:

* **`ServeOptions.spill_body_bytes`** -- the library's field, in bytes.
* **`server.spill-body-bytes`** -- the North Star's property beside `server.max-body-bytes`, mapped
  in `config.zm` like every other one.

Three things that follow, and the first is the one to say out loud:

1. **Spilling does not remove `max_body_bytes`. It changes what that number bounds** -- from memory
   to disk, which is the cheaper resource and still not an infinite one. So the Apple Health fix is
   two settings, not one: raise `max-body-bytes` to something disk-sized and set
   `spill-body-bytes` to about a megabyte. A reader who expects the cap to disappear should be
   corrected here rather than in a support thread.
2. **0 means never spill**, which is this codebase's convention for a setting that turns something
   off (`hsts-max-age-seconds`, `max-auth-attempts-per-address`), and it is the **default** -- so
   adding the feature changes no behaviour anywhere until a deployment asks for it.
3. **The temporary file is the server's, not the handler's.** It is created when the threshold is
   crossed and deleted when the request ends, including when the handler fails, which makes one
   contract: **a handler must not keep the source past its return.** Worth stating before anything
   is written, because a handler that stores it and reads later gets a file that is already gone.

## 14. What building step 1 changed about this document

`lib/core/io.zm` exists as of the first implementation commit, with `IoError`, `of_bytes`, `drain`,
`limited`, `mapped`, `text` and `lines`. Three things the writing got wrong or left out:

1. **`buffered` was a cursor wearing the wrong name** (§6). Withdrawn rather than built.
2. **`io` cannot own the converters.** §5 said it would offer `io.of_fs`, `io.of_net` and
   `io.of_client`; that is a cycle -- `io` would import `fs` for the type while `fs` imports `io` for
   the chunk's error. **The converters belong to the module that owns the error**: `fs.io_error(e)`,
   `net.io_error(e)`, `client.io_error(e)`, so the dependency points one way and `io` stays at layer
   0 of `docs/lib/dependencies.md`, which it now is.
3. **The state question had an answer nobody had written down.** `lib/prelude/iter.zm` said an
   `iter_from` closure's state must live in "a native handle ... or a Channel", which was true before
   `Mutable` existed. Every combinator here captures a `Mutable` cell, and that is also how one
   `Iter` wraps another; the prelude's comment now says so.

## 15. What the scanner conversion cost, measured

`xml`'s scanner reads an `io.Cursor` rather than a `[string]` of the whole document, which is step
3's groundwork. Two numbers from it, both on a 235 KB document with some accented text:

* **A `substring` per character is 193x.** The first version of the cursor built its window with
  `substring(text, i, i + 1)`, which walks the string unless the text is all-ASCII: **43 seconds**
  against 223 ms for the array scanner. `chars`, which decodes in one pass, brought it to 568 ms.
  This is the same trap `xml.zm`'s own helpers record one layer up, where `char_at` cost 2.2 s on
  40 KB of CJK -- and it was walked into anyway, which is the argument for measuring a rewrite
  rather than reasoning about it.
* **The cursor itself costs 2x**: 223 ms as an array against 463 ms through the window, after the
  `at` fast path was written out (568 -> 463). **Accepted**, because the alternative is two access
  layers -- `parse` on an array and `events` on a cursor -- which means two copies of seven helpers
  and 23 function signatures, and two copies drift. Filed as F263 to earn back: the overhead is the
  *number* of accessor calls per character rather than one hot line (inlining the hottest helper
  changed nothing, 472 against 463), so `read_text` should read a span between `<` markers instead
  of a character at a time -- which would beat the array version too, that one being
  character-at-a-time as well.

**What this does not yet buy**: nothing. A tree parse still holds the document, because the window
only shrinks when something calls `release` and `parse` builds a `Document` regardless. The
conversion is the half that makes `events` possible, not a win on its own -- which is why it is one
commit with the measurements in it rather than a claim about memory.

## 16. What is built, and the layer §8 missed

Built and measured: `io` (§§4-6), the `fs`/`client` retrofit (§12), `io.cursor`, `xml`'s scanner over
it, `xml.events` and `xml.elements`, and the North Star's Apple Health import over the last of those
-- **1,913 MB to 897 MB** peak resident for a 14.7 MB export, with the parser's live set one record
and a cursor window measured at 510 characters over a chunked source.

**§8 is wrong about one thing, and it is the thing that matters.** It treats the request body as the
last obstacle: spill it to a file, hand the handler a source, done. But the uploads this was for are
`multipart/form-data`, and `server/multipart.zm` parses from `req.body` **in memory** -- so a spilled
body is read straight back in and the ceiling does not move. The chain is spill, then multipart over
a source, then a `rest` family that binds a part's source. **F264** carries it with that order.

What the spill does buy on its own, which is still worth having: a large upload stops being held in
memory by the server, and `server.body_source` exists for whatever streams next.

**`io.Sink` is still unbuilt and still has no caller** (§7), which is the prediction that held: the
export path never needed one.
