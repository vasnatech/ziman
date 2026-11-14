# `charset`

Text and bytes, across the character encodings a program meets at a boundary.

**This is not `encoding.zm`, and the names are close enough to be worth separating here.**
`encoding` is base 2, 8, 16, 32 and 64: arbitrary bytes rendered as safe ASCII and read back,
where the input has no meaning as text at all. This module is the other direction -- *text*
and the bytes that stand for it in a named character set, where every question is about code
points and which of them a set can carry.

**It was written as a charset library rather than as whatever its first callers needed**, and
that is deliberate. **Ten** modules had hand-rolled a piece of this between them, and the
count kept going up as the migration went looking -- the roadmap said four encoders, the first
pass found five, and `io` and `csv` turned up only when the user asked whether they used this.
What was out there:

* **Five UTF-8 encoders**: `toml` and `yaml` (`append_utf8`), `xml` (`append_code_point`,
  which is why it was missed when these were counted as four), `truetype` (`codepoint_text`,
  with no four-byte branch) and `window` (`text_of_codepoint`).
* **Three pieces of the decode side**: `font`'s `codepoint_of`, and a lead-byte width table in
  both `terminal` and `io` -- `io`'s with a continuation test and a chunk-boundary scanner
  beside it, which is now `utf8_complete_prefix` and the best thing the migration recovered.
* `truetype`'s **UTF-16BE decoder**, which wrote a question mark for each half of a surrogate
  pair; two **Latin-1** loops; `pdf`'s 32-entry **CP1252** table; **three** copies of the same
  two surrogate constants; and `csv`'s **byte order mark**.

Each was correct for the one case in front of it and silently wrong just outside, which is what
the four-digit `/ToUnicode` destination in `pdf` had been for as long as it existed. A library
shaped by those callers would have inherited every one of those edges.

**What is here**: the Unicode transformation formats, and the single-byte sets. What is
deliberately *not* here: the legacy multi-byte East Asian sets -- Shift-JIS, GB18030, Big5,
EUC-JP and the rest. Those are megabyte mapping tables rather than arithmetic, they want
generating from the Unicode consortium's files rather than writing, and leaving them out is a
decision rather than an oversight.

`use charset` -- or `use charset.Name` to bring one item in bare.

## Example

```
use charset
use charset.replacement_text

// Text and the bytes that stand for it, in the sets a program meets at a boundary: a file it did
// not write, a font table, a PDF, a protocol header. Every encoder refuses what its set cannot
// carry rather than substituting for it, and every refusal names the set and the character.
fn main() {
    // A code point both ways. `chars` hands back one-character strings, so this is the door
    // between those and the numbers an encoding is defined over.
    print(charset.code_point_of("é"))
    print(charset.code_points_of("Aé😀"))
    switch charset.utf8_text(128512) {
        Ok(ch) { print(ch) }
        Err(r) { print(charset.describe(r)) }
    }

    // The single-byte sets differ in one block, 0x80 to 0x9F, and that block is why a document
    // can have a euro sign and an em dash in it without embedding a font.
    print(charset.win_ansi_byte(8364))
    print(charset.latin1_text([65b, 233b, 255b]))

    // What a set cannot carry, it says so about -- which character, and where.
    switch charset.win_ansi_bytes("şeker") {
        Ok(_) { print("unexpectedly encoded") }
        Err(r) { print(charset.describe(r)) }
    }

    // Reading is strict by default: these bytes are an overlong `/`, the oldest way there is to
    // smuggle a slash past a filter, and they are not UTF-8.
    print(charset.is_utf8([192b, 175b]))
    // ...and lossy when the caller would rather have the text with a mark in it, which is what
    // `truetype` wants from a font's name table.
    print(charset.utf8_lossy([65b, 192b, 175b, 66b]) == concat("A", replacement_text, "B"))

    // A set arrives as data more often than as a decision in the source: as a name in a header,
    // or as the mark a file begins with.
    switch charset.charset_named("iso-8859-1") {
        Some(e) { print(charset.name_of(e)) }
        None { print("no such set") }
    }
    switch charset.sniff([255b, 254b, 0b, 0b]) {
        Some(e) { print(charset.name_of(e)) }
        None { print("no mark") }
    }
    // A mark is not text, so decoding a whole file drops it. (A *chunk* of a stream keeps it:
    // see `decode_chunk`, which is what `io.text` reads with.)
    switch charset.decode([239b, 187b, 191b, 90b, 105b], charset.Charset.Utf8) {
        Ok(text) { print(text) }
        Err(r) { print(charset.describe(r)) }
    }

    // **The thing being converted comes first and the set second**, the same order the prelude
    // uses for `to_lower(s, with_locale)`. Strict and lossy are the same shape, so a caller
    // chooses between refusing and substituting without rearranging anything.
    switch charset.encode("şeker", charset.Charset.Ascii) {
        Ok(_) { print("unexpectedly encoded") }
        Err(r) { print(charset.describe(r)) }
    }
    print(charset.encode_lossy("şeker", charset.Charset.Ascii))
    print(charset.decode_lossy([104b, 233b], charset.Charset.Ascii))

    // And a stream is read in a set the same way: `io.text_in(source, set)` asks `charset` where
    // each chunk may be cut, which is a different question per set -- UTF-16 holds back a high
    // surrogate whose low half has not arrived yet.
    print(charset.complete_prefix([0b, 65b, 216b, 61b], charset.Charset.Utf16be))
}
```

Output:

```
233
[65, 233, 128512]
😀
128
Aéÿ
WinAnsi cannot represent U+015F at 0
false
true
Latin-1
UTF-32LE
Zi
ASCII cannot represent U+015F at 0
[63, 101, 107, 101, 114]
h�
2
```

## Code points

### `last_code_point`

```
pub val last_code_point = 1114111
```

The last code point there is. Above this nothing is text.

### `replacement`

```
pub val replacement = 65533
```

What a decoder puts where it could not read a character: U+FFFD, the replacement character.

**It is a code point like any other**, so this module never substitutes it silently -- a
decoder that cannot read its input answers an error and lets the caller decide. The value is
here because callers that *do* want to substitute should all use the same one.

### `replacement_text`

```
pub val replacement_text = "�"
```

The same character as a one-character string, which is what the lossy decoders below write in
place of what they could not read. **The literal is the character itself**, because ziman has
no `\u` escape and a decoder that cannot name its own output is not much of a decoder.

### `is_code_point`

```
pub fn is_code_point(cp: int) -> bool
```

Whether `cp` is a character: in range, and not half of a surrogate pair.

**A surrogate is not a code point**, which is the rule that keeps UTF-16 honest: D800 to DFFF
exist only as the two halves of an encoded astral character, and text containing one on its
own is text no reader can agree on.

### `is_high_surrogate`

```
pub fn is_high_surrogate(unit: int) -> bool
```

### `is_low_surrogate`

```
pub fn is_low_surrogate(unit: int) -> bool
```

### `paired`

```
pub fn paired(high: int, low: int) -> Option<int>
```

The code point a surrogate pair stands for, or `None` when the two are not a pair.

### `surrogates`

```
pub fn surrogates(cp: int) -> Option<[int]>
```

The surrogate pair that stands for `cp`, or `None` for anything inside the basic plane --
which needs no pair and is written as itself.

## What went wrong

### `Refusal`

```
pub struct Refusal {
    // The set that could not do it.
    //
    // **The value and not its name**, which it was until the `Charset` rename: eight refusal
    // sites each passed a literal `"UTF-16BE"` or `"WinAnsi"`, where a typo would have been
    // invisible until somebody read the message. `describe` spells it with `name_of`.
    charset: Charset
    // The code point at fault, or -1 when the input was bytes rather than text.
    code_point: int
    // Where in the input, in characters for text and in bytes for bytes.
    at: int
    why: string
}
```

**One shape for every failure here, and it names the set.** A caller that cannot represent a
character usually has advice of its own to add -- `pdf` tells you to embed a font -- and it
can only add it to a message that does not already presume the reason.

### `describe`

```
pub fn describe(r: Refusal) -> string
```

### `hex_of`

```
pub fn hex_of(cp: int) -> string
```

A code point as the digits after `U+`: at least four, more when it takes more.

## UTF-8

### `utf8_length`

```
pub fn utf8_length(cp: int) -> int
```

How many bytes `cp` takes in UTF-8, or 0 when it is not a character.

### `utf8_bytes`

```
pub fn utf8_bytes(cp: int) -> Result<[byte], Refusal>
```

One code point's UTF-8 bytes. The arithmetic five modules had each written out.

### `utf8_text`

```
pub fn utf8_text(cp: int) -> Result<string, Refusal>
```

One code point as a one-character string. **The function five modules had each written for
themselves**, which is how this module came to exist.

### `code_point_of`

```
pub fn code_point_of(ch: string) -> int
```

The code point a one-character string holds, or -1 for the empty string.

**It takes a character rather than a string and an index** because that is the unit the
prelude works in: `chars` and `char_at` both answer one of these, so a caller already has one
in hand and does not have to think about byte offsets to use this.

### `code_points_of`

```
pub fn code_points_of(text: string) -> [int]
```

### `utf8_sequence_length`

```
pub fn utf8_sequence_length(lead: byte) -> int
```

How many bytes the character beginning with `lead` occupies, or 0 when no character can begin
with that byte -- a continuation byte, or one of the five values that are not lead bytes at all.

**0 rather than 1 for a byte that cannot lead**, which is the opposite of what a reader
scanning for the next character wants: it wants to step over the bad byte and carry on. That
decision belongs to the reader, which is why this says what is true and `terminal` writes the
`if width == 0 { 1 }` itself.

### `is_continuation`

```
pub fn is_continuation(b: byte) -> bool
```

Whether `b` continues a character rather than beginning one: `10xxxxxx`, which is to say 0x80
through 0xBF. Compared as a range rather than masked, because `is_continuation(b)` reads better
than `bits.and(b, 192b) == 128b` and means the same thing.

### `utf8_complete_prefix`

```
pub fn utf8_complete_prefix(data: [byte]) -> int
```

How many bytes at the front of `data` are whole characters -- which is to say **where a reader
must cut so that nothing is left holding half of one**.

**This is what a chunked reader needs and a decoder cannot give it.** A 4 KiB read that ends in
the middle of a `ö` is not malformed, it is unfinished: the rest of that character arrives in
the next chunk. Decoding each chunk on its own instead is a bug that appears only on the files
that happen to split there, which is why `io.text` holds the remainder back and asks this where
to cut.

**It says where to cut, not whether the bytes are valid.** Walking back from the end over at
most three continuation bytes is enough, a character being at most four bytes: at the lead byte
it asks how long that character should be and keeps it only if all of it is here. A byte that
cannot lead, where a lead byte was expected, is left inside the prefix on purpose -- "this is
not UTF-8" is the decoder's answer to give, and this function would only be guessing at it.

### `utf8_bytes`

```
pub fn utf8_bytes(text: string) -> [byte]
```

A string's own bytes. **The one encoder here that answers no `Result`**: a ziman string is
UTF-8 already, so there is nothing to refuse. It exists so that every set in this module has
the same pair of functions, which is what lets `encode` below dispatch over all of them.

### `utf8_text`

```
pub fn utf8_text(data: [byte]) -> Result<string, Refusal>
```

UTF-8 bytes as text, refusing everything that is not UTF-8 rather than substituting for it:
a byte that cannot lead, a character cut short by the end of the input or by the next one, a
**surrogate** (which is what CESU-8 and Java's modified UTF-8 write and UTF-8 does not), a
value **past the last code point**, and an **overlong** encoding -- a character written in
more bytes than it needs, which is the oldest way there is to smuggle a `/` past a filter.

Checked here and converted by the native, rather than built a character at a time: the scan
allocates nothing and `bytes_to_string` is one pass.

### `is_utf8`

```
pub fn is_utf8(data: [byte]) -> bool
```

Whether `data` is UTF-8, for the callers that want the question and not the text. Allocates
nothing: `url.percent_decode` asks this of every parameter that contained an escape.

## UTF-16

### `utf16_units`

```
pub fn utf16_units(cp: int) -> Result<[int], Refusal>
```

The UTF-16 code units for `cp`: one for the basic plane, a surrogate pair above it.

### `utf16be_bytes`

```
pub fn utf16be_bytes(text: string) -> Result<[byte], Refusal>
```

### `utf16be_text`

```
pub fn utf16be_text(data: [byte]) -> Result<string, Refusal>
```

### `utf16le_bytes`

```
pub fn utf16le_bytes(text: string) -> Result<[byte], Refusal>
```

The little-endian order, which is what a Windows API, a `.strings` file and most of the
"UTF-16" in the world actually mean.

### `utf16le_text`

```
pub fn utf16le_text(data: [byte]) -> Result<string, Refusal>
```

## UTF-32

### `utf32be_bytes`

```
pub fn utf32be_bytes(text: string) -> [byte]
```

### `utf32be_text`

```
pub fn utf32be_text(data: [byte]) -> Result<string, Refusal>
```

**The one format whose bytes can hold something that is not a character at all.** Four bytes
reach past U+10FFFF and into the surrogate block, where UTF-8's and UTF-16's own shapes do
not, so a decoder here has to ask rather than assume.

### `utf32le_bytes`

```
pub fn utf32le_bytes(text: string) -> [byte]
```

### `utf32le_text`

```
pub fn utf32le_text(data: [byte]) -> Result<string, Refusal>
```

### `utf16be_hex`

```
pub fn utf16be_hex(cp: int) -> string
```

A code point as its UTF-16BE code units in hexadecimal: four digits inside the basic plane,
eight -- a **surrogate pair** -- above it.

**This is what a PDF `/ToUnicode` destination is, and it is not the code point's own digits.**
Writing those instead, four of them, makes U+1F600 come out as `<F600>`: a private-use
character, so the text draws correctly and *extracts* as something else, which is the exact
failure `/ToUnicode` exists to prevent.

Anything that is not a character answers `FFFD`, the replacement character's own units,
because the caller is writing a table entry and has nowhere to put an error.

## The single-byte sets

### `ascii_bytes`

```
pub fn ascii_bytes(text: string) -> Result<[byte], Refusal>
```

The 128 characters every set in this module agrees on. **Encoding to it is how a caller asks
"is this text plain?"** before putting it somewhere that only promises ASCII -- a header
name, a PDF name object, an identifier.

### `ascii_text`

```
pub fn ascii_text(data: [byte]) -> Result<string, Refusal>
```

Unlike Latin-1, **decoding can fail**: a byte over 0x7F is not an ASCII character, and the
set has nothing to say about what it might mean. Guessing here is how text becomes mojibake.

### `latin1_text`

```
pub fn latin1_text(data: [byte]) -> string
```

ISO-8859-1, where every byte is the code point of the same number. **Decoding cannot fail**,
which is the property that makes it the fallback every other single-byte set is compared to.

### `latin1_bytes`

```
pub fn latin1_bytes(text: string) -> Result<[byte], Refusal>
```

### `win_ansi_text`

```
pub fn win_ansi_text(data: [byte]) -> string
```

### `win_ansi_bytes`

```
pub fn win_ansi_bytes(text: string) -> Result<[byte], Refusal>
```

### `win_ansi_byte`

```
pub fn win_ansi_byte(cp: int) -> int
```

The WinAnsi byte for a code point, or -1 when the set has none.

## Reading what is not quite right

### `utf8_lossy`

```
pub fn utf8_lossy(data: [byte]) -> string
```

**Strict by default, lossy when the caller asks.** Every decoder above refuses rather than
guesses, which is the only defensible default for a library: a decoder that substitutes
silently turns a bug in one program into wrong text in every program downstream of it.

These three are the other half of that bargain, and the Unicode standard describes them: read
what is well-formed, put U+FFFD where the input was not, and carry on. **A caller wants this
when the text is cosmetic and the alternative is losing something that is not** -- `truetype`
reads a font's name table this way, because a name table that is not quite UTF-16 is a broken
*name* and the glyphs are elsewhere and work.

One mark per ill-formed sequence, not per byte: the standard calls it the maximal subpart, and
it is why `E1 80 41` reads as one mark and an `A` rather than two marks and an `A`.

### `utf16be_lossy`

```
pub fn utf16be_lossy(data: [byte]) -> string
```

### `utf16le_lossy`

```
pub fn utf16le_lossy(data: [byte]) -> string
```

## The byte order mark

### `bom`

```
pub val bom = 65279
```

U+FEFF, the character a file may begin with to say which encoding it is in.

**A mark is not text.** `decode` drops a leading one and `encode` writes none, so a round
trip through this module neither invents a mark nor leaves one in the string -- which is the
bug behind every spreadsheet whose first column is headed with an invisible character and
`id`.

### `bom_text`

```
pub val bom_text = "﻿"
```

The same character as a one-character string, for a caller that meets the mark **in text**
rather than in bytes -- `csv` strips one off a spreadsheet's first heading, where the bytes are
long since decoded and the mark has become an invisible character at the front of a cell.

## The sets as values

### `Charset`

```
pub enum Charset {
    Utf8
    Utf16be
    Utf16le
    Utf32be
    Utf32le
    Latin1
    WinAnsi
    Ascii
}
```

**The functions above are how a set is called; this is how one is chosen.** A charset arrives
as data far more often than as a decision in the source -- an HTTP `charset=` parameter that
`mime.charset` hands back as a string, an XML declaration, a byte order mark, a dropdown in a
dialog -- and a caller holding a name has no way to reach a function. Everything below
delegates upwards.

**Named `Charset` and not `Encoding`**, which it was until an hour after it was written. This
module's header argues at length that it is not `encoding.zm` -- base 2 through 64 over bytes
that are not text -- and then calling its central type `Encoding` would have put
`use charset.Encoding` next to `use encoding` in the same file. `charset.Charset` stutters
slightly and is what Java calls the same thing; `Set` was never available, the prelude having
one.

### `name_of`

```
pub fn name_of(set: Charset) -> string
```

The set's usual written name, which is also the name its refusals carry.

### `charset_named`

```
pub fn charset_named(name: string) -> Option<Charset>
```

The set a written name means, ignoring case, hyphens, underscores, spaces and dots, so that
`utf-8`, `UTF8` and `utf_8` are one answer. The aliases are the ones that turn up in headers
and declarations: `iso-8859-1`, `cp1252`, `windows-1252`, `us-ascii`, `ucs-2`.

### `mark_of`

```
pub fn mark_of(set: Charset) -> [byte]
```

The mark that announces this set, or no bytes for the sets that have none -- which is every
single-byte set, there being no order to mark.

### `sniff`

```
pub fn sniff(data: [byte]) -> Option<Charset>
```

The set `data` announces with a byte order mark, or `None` when it begins with no mark --
which is not the same as "not text": most files have no mark and are UTF-8.

**UTF-32LE is tested before UTF-16LE** because UTF-16LE's mark is its first two bytes, and
asking in the other order reads every UTF-32LE file as UTF-16LE beginning with a NUL.

### `complete_prefix`

```
pub fn complete_prefix(data: [byte], set: Charset) -> int
```

How many bytes at the front of `data` form whole characters **in this set** -- where a reader
taking a stream in chunks must cut so that nothing is left holding half of one.

**The generalisation of `utf8_complete_prefix`, and the reason this enum exists.** `io.text`
could read exactly one set until this did: its carry logic is general -- hold back the
remainder, decode the rest, report a remainder at the end of the stream as truncation -- and
the only thing in it that knew about UTF-8 was this question. `io.text_in(source, set)` is that
function with this one answering instead.

Each set holds back something different: UTF-8 up to three continuation bytes, UTF-16 an odd
byte **and a dangling high surrogate** (whose low half is in the next chunk), UTF-32 up to
three bytes of a four-byte character, and a single-byte set nothing at all, every byte being a
whole character.

### `encode`

```
pub fn encode(text: string, set: Charset) -> Result<[byte], Refusal>
```

`text` in `set`, with no byte order mark: prepend `mark_of(set)` to write one.

**The thing being converted comes first and the set second**, which is the prelude's own order
for a conversion with a cultural parameter -- `to_lower(s, with_locale)` -- and the order
`io.text_in(source, set)` uses. These four read `encode(text, set)`, `decode(data, set)`,
`decode_lossy(data, set)`, `complete_prefix(data, set)`; the single-argument ones
(`name_of`, `mark_of`) take the set alone.

### `encode_lossy`

```
pub fn encode_lossy(text: string, set: Charset) -> [byte]
```

`data` read as `set` the lossy way: what is well-formed, with U+FFFD for what is not. The
single-byte sets have no ill-formed input, so for those this is `decode` without the `Result`.
`text` in `set`, with **`?` where the set has no byte for a character** -- the encoder's half of
the bargain `decode_lossy` makes, and it was missing until 2026-11-14: the module offered a
lossy reader and a strict writer, which is a position nobody chose.

**The substitute is `?` and not U+FFFD**, because U+FFFD is a *decoder's* mark for input it
could not read, and most of the sets here cannot encode it anyway -- WinAnsi has no byte for
U+FFFD, so a lossy WinAnsi encoder that reached for one would fail on its own substitute.

**Ask for this deliberately and rarely.** `pdf` refuses rather than substituting, and the
reason is in its own comment: a `?` where a patient's name should be is worse than an error,
because nobody can tell it happened. A legacy export where `?` is the agreed answer is what
this is for.

### `decode_lossy`

```
pub fn decode_lossy(data: [byte], set: Charset) -> string
```

As `decode`, including dropping a leading mark, but lossy. The chunk-wise counterpart is
`utf8_lossy` and friends, which never looked at a mark.

### `decode`

```
pub fn decode(data: [byte], set: Charset) -> Result<string, Refusal>
```

`data` read as `set`, **dropping a leading mark for that same set**: a mark is how the bytes said
what they were, not part of what they said. For a whole file or message; a stream's chunks want
`decode_chunk`.

### `decode_chunk`

```
pub fn decode_chunk(data: [byte], set: Charset) -> Result<string, Refusal>
```

`data` read as `set` with **no mark handling at all**, which is what a piece of a stream needs:
a mark is only a mark at the beginning, and a chunk is not a beginning. `decode` above would
drop one from the middle of a stream -- U+FEFF is a legal zero-width no-break space there --
and would also quietly change what `io.text` has always yielded for a file that starts with
one. A reader that wants the mark dropped should drop it once, where it knows it is first.
