# Ziman core module reference

Auto-generated from every bundled module's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub enum`/
`pub fn`/`pub native fn`/`pub val`). This is a signature lookup, not an
explanation -- see the manual in docs/lib/ for how to use each module, docs/syntax/ for full semantics and docs/design/journal/
for why each module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/syntax/'s "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.

One exception, not listed below: `lib/prelude/` -- unlike every
module here, it's never `use`-able at all (see its own header comment
and docs/design/journal/'s "Prelude" entry), since everything it declares
(`Option<T>`, plus a handful of native functions) is already bare and
unqualified in every program, the same way `print`/`str`/`parse_int`
are. See docs/syntax/'s "Modules" section for what it contains.

## `a11y`

The accessibility tree: what a screen reader is told about a window drawn from `lib/ui/widget`.

```
pub val root_key = "@"

pub struct Node {
    key: string
    role: Role
    // What a reader says. Never the `name` a control is focused by: see `widget.Focusable`.
    name: string
    description: string
    value: string
    // What the application declared -- see `widget.State`.
    state: State
    // Computed here, from the focus passed to `tree_of`, which is why it is not in `State`.
    focused: bool
    // Whether a Tab could reach it: a control that is a tab stop and is not disabled. Not
    // declarable here, because it is implied by `widget.Focusable.tab_stop` and `state.disabled`,
    // and a third copy of that fact could disagree with the other two.
    focusable: bool
    // In the same coordinates the layout used, so an adapter can answer "what is at this point" and
    // "scroll this into view" without a second tree.
    bounds: Rect
    children: [string]
}

pub struct Tree {
    nodes: [Node]
    root: string
    // The focused node's key, or `None` -- which is a real state, not an error: a window of labels
    // has nothing to focus. A focus naming a control this tree no longer holds is `None` too, for
    // the reason `focus.holds` exists.
    focus: Option<string>
}

pub fn tree_of(l: Laid, title: string, focus: string) -> Tree

pub fn node_of(t: Tree, key: string) -> Option<Node>

pub fn reachable(t: Tree) -> [Node]

pub struct Announcement {
    text: string
    // Polite waits for the reader to finish its sentence; assertive interrupts. A validation
    // failure interrupts; a result count waits.
    assertive: bool
}

pub fn announcements(before: Tree, after: Tree) -> [Announcement]

pub fn no_tree() -> Tree

pub fn announce(n: Node) -> string

pub fn toggle_word(t: Toggle, r: Role) -> string

```

## `atspi`

Publishing an accessibility tree so a screen reader can read it.

```
pub fn announce(said: a11y.Announcement)

pub fn publish(tree: a11y.Tree, app: string) -> Result<unit, string>

pub fn pump()

```

## `batch`

A job repository: a record of what ran, how it went and where it got to, so work that outlives the request that started it can be polled, resumed and told apart from work that died.

```
pub val status_running = "RUNNING"

pub val status_succeeded = "SUCCEEDED"

pub val status_failed = "FAILED"

pub struct Run {
    id: Uuid
    job: string
    status: string
    started_at: Instant
    finished_at: Option<Instant>
    // How many items the job reported writing. Its own count, in its own units.
    items: int
    resume: string
    // Why it failed, when it did. Never set on a run that succeeded.
    message: Option<string>
}

pub struct Note {
    write: fn(datasource.Connection, int, string) -> Result<unit, string>
}

pub struct Job {
    name: string
    run: fn(string, Note) -> Result<int, string>
    done: Option<fn() -> Result<bool, string>>
}

pub fn job(name: string, run: fn(string, Note) -> Result<int, string>) -> Job

pub fn job_unless(

pub fn schema(table: string) -> string

pub fn status(conn: datasource.Connection, table: string, id: Uuid) -> Result<Option<Run>, string>

pub fn recent(

pub fn stale(

pub fn run_now(

pub fn launch(

```

## `bits`

Bitwise and/or/xor/not/shift on `byte`, `int` and `bigint`, and the two scalar-backed sets they back, `BitSet` (an `int`, at most 64 members) and `BigBitSet` (a `bigint`, unbounded).

```
pub native fn and(a: byte, b: byte) -> byte

pub native fn or(a: byte, b: byte) -> byte

pub native fn xor(a: byte, b: byte) -> byte

pub native fn not(a: byte) -> byte

pub native fn and(a: int, b: int) -> int

pub native fn or(a: int, b: int) -> int

pub native fn xor(a: int, b: int) -> int

pub native fn not(a: int) -> int

pub native fn and(a: bigint, b: bigint) -> bigint

pub native fn or(a: bigint, b: bigint) -> bigint

pub native fn xor(a: bigint, b: bigint) -> bigint

pub native fn not(a: bigint) -> bigint

pub native fn shift_left(a: byte, n: int) -> byte

pub native fn shift_right(a: byte, n: int) -> byte

pub native fn shift_left(a: int, n: int) -> int

pub native fn shift_right(a: int, n: int) -> int

pub native fn shift_left(a: bigint, n: int) -> bigint

pub native fn shift_right(a: bigint, n: int) -> bigint

pub native fn popcount(a: byte) -> int

pub native fn popcount(a: int) -> int

pub native fn popcount(a: bigint) -> int

pub native fn bit_length(a: bigint) -> int

pub struct BitSet {
    bits: int
}

pub fn bitset_new() -> BitSet

pub fn bitset_of_int(x: int) -> BitSet

pub fn bitset_to_int(bs: BitSet) -> int

pub fn set(bs: BitSet, i: int) -> BitSet

pub fn remove(bs: BitSet, i: int) -> BitSet

pub fn flip(bs: BitSet, i: int) -> BitSet

pub fn get(bs: BitSet, i: int) -> bool

pub fn intersect(sets: variadic BitSet) -> BitSet

pub fn union(sets: variadic BitSet) -> BitSet

pub fn difference(a: BitSet, b: BitSet) -> BitSet

pub fn is_subset(a: BitSet, b: BitSet) -> bool

pub fn xor(a: BitSet, b: BitSet) -> BitSet

pub fn not(bs: BitSet) -> BitSet

pub fn is_empty(bs: BitSet) -> bool

pub fn len(bs: BitSet) -> int

pub fn bit_length(bs: BitSet) -> int

pub fn to_array(bs: BitSet) -> [int]

pub fn format(bs: BitSet) -> string

pub struct BigBitSet {
    bits: bigint
}

pub fn big_bitset_new() -> BigBitSet

pub fn big_bitset_of_bigint(x: bigint) -> BigBitSet

pub fn big_bitset_to_bigint(bs: BigBitSet) -> bigint

pub fn set(bs: BigBitSet, i: int) -> BigBitSet

pub fn remove(bs: BigBitSet, i: int) -> BigBitSet

pub fn flip(bs: BigBitSet, i: int) -> BigBitSet

pub fn get(bs: BigBitSet, i: int) -> bool

pub fn intersect(sets: variadic BigBitSet) -> BigBitSet

pub fn union(sets: variadic BigBitSet) -> BigBitSet

pub fn difference(a: BigBitSet, b: BigBitSet) -> BigBitSet

pub fn is_subset(a: BigBitSet, b: BigBitSet) -> bool

pub fn xor(a: BigBitSet, b: BigBitSet) -> BigBitSet

pub fn is_empty(bs: BigBitSet) -> bool

pub fn len(bs: BigBitSet) -> int

pub fn bit_length(bs: BigBitSet) -> int

pub fn to_array(bs: BigBitSet) -> [int]

pub fn format(bs: BigBitSet) -> string

```

## `charset`

Text and bytes, across the character encodings a program meets at a boundary.

```
pub val last_code_point = 1114111

pub val replacement = 65533

pub val replacement_text = "�"

pub fn is_code_point(cp: int) -> bool

pub fn is_high_surrogate(unit: int) -> bool

pub fn is_low_surrogate(unit: int) -> bool

pub fn paired(high: int, low: int) -> Option<int>

pub fn surrogates(cp: int) -> Option<[int]>

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

pub fn describe(r: Refusal) -> string

pub fn hex_of(cp: int) -> string

pub fn utf8_length(cp: int) -> int

pub fn utf8_bytes(cp: int) -> Result<[byte], Refusal>

pub fn utf8_text(cp: int) -> Result<string, Refusal>

pub fn code_point_of(ch: string) -> int

pub fn code_points_of(text: string) -> [int]

pub fn utf8_sequence_length(lead: byte) -> int

pub fn is_continuation(b: byte) -> bool

pub fn utf8_complete_prefix(data: [byte]) -> int

pub fn utf8_bytes(text: string) -> [byte]

pub fn utf8_text(data: [byte]) -> Result<string, Refusal>

pub fn is_utf8(data: [byte]) -> bool

pub fn utf16_units(cp: int) -> Result<[int], Refusal>

pub fn utf16be_bytes(text: string) -> Result<[byte], Refusal>

pub fn utf16be_text(data: [byte]) -> Result<string, Refusal>

pub fn utf16le_bytes(text: string) -> Result<[byte], Refusal>

pub fn utf16le_text(data: [byte]) -> Result<string, Refusal>

pub fn utf32be_bytes(text: string) -> [byte]

pub fn utf32be_text(data: [byte]) -> Result<string, Refusal>

pub fn utf32le_bytes(text: string) -> [byte]

pub fn utf32le_text(data: [byte]) -> Result<string, Refusal>

pub fn utf16be_hex(cp: int) -> string

pub fn ascii_bytes(text: string) -> Result<[byte], Refusal>

pub fn ascii_text(data: [byte]) -> Result<string, Refusal>

pub fn latin1_text(data: [byte]) -> Result<string, Refusal>

pub fn latin1_bytes(text: string) -> Result<[byte], Refusal>

pub fn win_ansi_text(data: [byte]) -> Result<string, Refusal>

pub fn win_ansi_bytes(text: string) -> Result<[byte], Refusal>

pub fn win_ansi_byte(cp: int) -> int

pub fn utf8_lossy(data: [byte]) -> string

pub fn utf16be_lossy(data: [byte]) -> string

pub fn utf16le_lossy(data: [byte]) -> string

pub val bom = 65279

pub val bom_text = "﻿"

pub fn without_mark(text: string) -> string

pub fn without_mark(data: [byte], set: Charset) -> [byte]

pub enum Charset {
    Utf8
    Utf16be
    Utf16le
    Utf32be
    Utf32le
    Latin1
    WinAnsi
    Ascii
    // Any set that is one byte per character, carrying its own table -- ISO-8859-2 through -16,
    // the Windows code pages, KOI8, Mac Roman.
    //
    // **One variant rather than twenty-five members**, which keeps every switch in this module to
    // nine arms however many sets ship, and makes a set a *value*: `tables.zm` is generated, and
    // `single_byte` builds one from a table a caller read from wherever it liked. That is the
    // resource-file route, with the filesystem dependency in the application instead of under
    // every text boundary in the library -- `charset` cannot read a file, because `fs` uses
    // `charset` and the loader refuses a circular `use`.
    SingleByte(SingleByteSet)
}

pub struct SingleByteSet {
    name: string
    // The 128 code points for 0x80 to 0xFF, **-1 where the set has no character for that byte**.
    // ISO-8859-3 and -6 have such holes; a byte that lands on one is refused, not guessed.
    upper: [int]
    // The same mapping the other way: code points in order, with the byte for each at the same
    // index, so encoding is a binary search instead of a scan over 128 entries.
    points: [int]
    bytes: [int]
}

pub fn single_byte(name: string, upper: [int]) -> Result<Charset, Refusal>

pub fn single_byte_of(s: SingleByteSet, cp: int) -> int

pub fn name_of(set: Charset) -> string

pub fn charset_named(name: string) -> Option<Charset>

pub fn mark_of(set: Charset) -> [byte]

pub fn sniff(data: [byte]) -> Option<Charset>

pub fn complete_prefix(data: [byte], set: Charset) -> int

pub fn encode(text: string, set: Charset) -> Result<[byte], Refusal>

pub fn encode_lossy(text: string, set: Charset) -> [byte]

pub fn decode_lossy(data: [byte], set: Charset) -> string

pub fn decode(data: [byte], set: Charset) -> Result<string, Refusal>

pub fn decode_chunk(data: [byte], set: Charset) -> Result<string, Refusal>

```

## `chart`

A line or a bar chart, drawn into a scene.

```
pub enum Mark {
    Line
    Bars
}

pub struct Series {
    name: string
    mark: Mark
    points: [float]
    ink: Color
}

pub struct Band {
    from: float
    to: float
    ink: Color
}

pub struct Chart {
    // One per position on the x axis. A series longer or shorter than this is drawn only as far as
    // the two agree -- see `points_shown`.
    labels: [string]
    series: [Series]
    bands: [Band]
}

pub struct ChartStyle {
    ink: Color
    grid: Color
    // The axis labels.
    text_size: float
    font: int
    line_width: float
    // How much of each slot a bar fills, 0 to 1.
    bar_fill: float
    // Room for the y labels on the left and the x labels below.
    left_gutter: float
    bottom_gutter: float
}

pub fn chart_of(labels: [string], series: [Series]) -> Chart

pub fn line_series(name: string, points: [float], ink: Color) -> Series

pub fn bar_series(name: string, points: [float], ink: Color) -> Series

pub fn points_shown(c: Chart, s: Series) -> int

pub fn y_gaps(c: Chart) -> int

pub fn y_range(c: Chart) -> geom.Point

pub fn plot_box(c: Chart, box: Rect, style: ChartStyle) -> Rect

pub fn y_at(c: Chart, value: float, plot: Rect, style: ChartStyle) -> float

pub fn x_at(c: Chart, index: int, plot: Rect, style: ChartStyle) -> float

pub fn chart_scene(c: Chart, box: Rect, style: ChartStyle) -> scene.Scene

```

## `client`

An HTTP/1.1 *client*, written in Ziman on top of `net`, so a Ziman program can call another service (the server side is `server`; the types they share are in `http`).

```
pub enum ClientError {
    BadUrl(string)
    Network(string)
    Timeout
    BadResponse(string)
    TooLarge
    TooManyRedirects
}

pub fn io_error(e: ClientError) -> io.IoError

pub fn client_error_message(e: ClientError) -> string

pub struct Url {
    secure: bool
    host: string
    port: int
    target: string
    userinfo: string
}

pub fn parse_url(url: string) -> Result<Url, string>

```

## `clock`

The system clock and sleeping, the two things Ziman cannot do in pure code.

```
pub native fn now_raw() -> [int]

pub native fn now_iso() -> string

pub native fn now_http_date() -> string

pub native fn monotonic_millis() -> int

pub native fn sleep_millis(ms: int)

```

## `codec`

A value as bytes, in a named media type, and back: the typed `Encoder`/`Decoder` pair and the `TreeFormat`s they can be built from.

```
pub struct Decoder<I> {
    media: mime.MediaType
    decode: fn([byte]) -> Result<I, string>
}

pub struct Encoder<O> {
    media: mime.MediaType
    encode: fn(O) -> [byte]
}

pub struct TreeFormat {
    // What to call it to a person: `JSON`, not `application/json`. The media type is the wire's
    // name for a format and a poor word for a message -- "malformed application/json" reads as
    // machinery where "malformed JSON" reads as a sentence. The Java interface this answers to
    // draws the same distinction, `MediaType.name()` beside `mimeType()`.
    name: string
    media: mime.MediaType
    parse: fn(string) -> Result<Json, string>
    format: fn(Json) -> Result<string, string>
}

pub fn tree_format_of(

pub fn json_format() -> TreeFormat

pub fn yaml_format() -> TreeFormat

pub fn toml_format() -> TreeFormat

pub fn ndjson_format() -> TreeFormat

pub fn csv_format() -> TreeFormat

pub fn formats() -> [TreeFormat]

pub fn tree_formats() -> [TreeFormat]

pub fn sequence_formats() -> [TreeFormat]

pub fn tree_format_for(media: mime.MediaType) -> Option<TreeFormat>

pub fn decoder<I>(media: mime.MediaType, decode: fn([byte]) -> Result<I, string>) -> Decoder<I>

pub fn text_decoder<I>(media: mime.MediaType, decode: fn(string) -> Result<I, string>) -> Decoder<I>

pub fn encoder<O>(media: mime.MediaType, encode: fn(O) -> [byte]) -> Encoder<O>

pub fn text_encoder<O>(media: mime.MediaType, encode: fn(O) -> string) -> Encoder<O>

pub fn encoder_of<O>(f: TreeFormat, to_json: fn(O) -> Json) -> Encoder<O>

pub fn list_encoder_of<O>(f: TreeFormat, to_json: fn(O) -> Json) -> Encoder<[O]>

pub fn decoder_of<I>(f: TreeFormat, from_json: fn(Json) -> Result<I, string>) -> Decoder<I>

pub fn encoders_of<O>(to_json: fn(O) -> Json) -> [Encoder<O>]

pub fn list_encoders_of<O>(to_json: fn(O) -> Json) -> [Encoder<[O]>]

pub fn json_decoder<I>(from_json: fn(Json) -> Result<I, string>) -> Decoder<I>

pub fn json_encoder<O>(to_json: fn(O) -> Json) -> Encoder<O>

pub fn json_list_encoder<O>(to_json: fn(O) -> Json) -> Encoder<[O]>

pub fn count_encoder() -> Encoder<int>

pub fn json_value_list_encoder() -> Encoder<[Json]>

pub fn plain_text_encoder() -> Encoder<string>

pub fn xml_encoder<O>(to_xml: fn(O) -> xml.Document) -> Encoder<O>

pub fn xml_decoder<I>(from_xml: fn(xml.Document) -> Result<I, string>) -> Decoder<I>

pub fn xml_list_encoder<O>(root: string, to_xml: fn(O) -> xml.Document) -> Encoder<[O]>

pub fn json_value_encoder() -> Encoder<Json>

```

## `compress`

Gzip and zlib (deflate) compression, on zlib, and reading a ZIP archive.

```
pub fn gzip(data: [byte]) -> [byte]

pub fn deflate(data: [byte]) -> [byte]

pub fn gunzip(data: [byte], max_size: int) -> Result<[byte], string>

pub fn inflate(data: [byte], max_size: int) -> Result<[byte], string>

pub fn decompress(data: [byte], max_size: int) -> Result<[byte], string>

pub fn deflate_raw(data: [byte]) -> [byte]

pub fn inflate_raw(data: [byte], max_size: int) -> Result<[byte], string>

pub struct ZipEntry {
    name: string
    body: [byte]
}

pub val zip_max_entry_bytes = 64 * 1024 * 1024

pub fn is_zip(data: [byte]) -> bool

pub fn zip_entries(data: [byte]) -> Result<[ZipEntry], string>

pub fn zip_entries_with(data: [byte], max_bytes: int) -> Result<[ZipEntry], string>

```

## `controls`

The widget set: what an input does to a control, and what a control looks like.

```
pub struct Wheel {
    at: Point
    // Lines, signed: positive is away from the user. The same convention `window.ScrollStep` uses,
    // because converting between two sign conventions is a bug waiting to be written.
    vertical: int
    horizontal: int
}

pub enum Key {
    Enter
    Backspace
    Delete
    Tab
    Escape
    Space
    Left
    Up
    Right
    Down
    PageUp
    PageDown
    Home
    End
    Char(string)
    Other(string)
}

pub struct Keystroke {
    key: Key
    shift: bool
    control: bool
    alt: bool
    meta: bool
}

pub fn keystroke_of(key: Key) -> Keystroke

pub fn char_of(k: Key) -> string

pub enum Input {
    KeyDown(Keystroke)
    KeyUp(Keystroke)
    // The text the keyboard composed, which is a separate event from the key that composed it --
    // see `window.Event`, where the same split is explained at length.
    Text(string)
    // Text from the system clipboard, **handed in rather than read**: nothing in this module can
    // reach a clipboard, and F273 left owning an X11 selection as platform work beside
    // `native_window.c`. It is a separate variant from `Text` because the two differ in a way a
    // user feels -- a paste is one undo step whatever its length, where typing coalesces a run of
    // keystrokes into one.
    Paste(string)
    Press(Point)
    // The secondary button, which is the one that asks "what can I do here?".
    //
    // **A variant and not a flag on `Press`**, because the two mean different things and nearly
    // every control wants only the first. A `Press` carrying a button number is a number each of
    // the twenty-odd places that match on a press has to remember to check, and the one that
    // forgets fires its command on a right-click -- which is exactly what this library did until
    // this variant existed: `desktop.input_of` threw the platform's button away, so a right-click,
    // a middle-click and a left-click were the same event and every button in every window
    // activated on all three.
    SecondaryPress(Point)
    Release(Point)
    Move(Point)
    Wheel(Wheel)
}

pub fn target(l: Laid, input: Input, focused: string, pressed: string) -> string

pub fn pressed_after(pressed: string, input: Input, name: string) -> string

pub fn cursor_at(l: Laid, at: Point) -> widget.Cursor

pub struct Button {
    name: string
    label: string
    // What the button shows beside its label, if anything.
    //
    // **A field rather than a widget in the label slot**, which is the decision recorded in
    // `widget.Icon`: it is what lets a design file name an icon without the dialect reaching
    // inside a control, and it is why the icon has no size of its own -- `button_widget` derives
    // one from `Style.text_size` below.
    //
    // **A button with no label is announced by its icon's name**, which is the one case where an
    // icon is not decoration. A button with both says only its label.
    icon: Option<widget.Icon>
    // A count or a word beside the label, part of the same fact. See `Badge`, which explains why
    // this is a field here rather than a widget a caller puts next to the button.
    badge: Option<Badge>
    disabled: bool
    held: bool
}

pub fn button_of(name: string, label: string) -> Button

pub fn icon_button_of(name: string, label: string, icon: widget.Icon) -> Button

pub struct ButtonStep {
    button: Button
    activated: bool
}

pub fn button_input(b: Button, input: Input, bounds: Rect) -> ButtonStep

pub fn button_widget(b: Button, f: Frame) -> widget.Widget

pub struct CheckBox {
    name: string
    label: string
    disabled: bool
    checked: Toggle
    held: bool
}

pub fn check_box_of(name: string, label: string, checked: Toggle) -> CheckBox

pub struct CheckStep {
    check_box: CheckBox
    toggled: bool
}

pub fn check_box_input(c: CheckBox, input: Input, bounds: Rect) -> CheckStep

pub fn check_box_widget(c: CheckBox, f: Frame) -> widget.Widget

pub struct Switch {
    name: string
    label: string
    disabled: bool
    on: Toggle
    held: bool
}

pub struct SwitchStep {
    control: Switch
    toggled: bool
}

pub fn switch_of(name: string, label: string, on: Toggle) -> Switch

pub fn switch_input(s: Switch, input: Input, bounds: Rect) -> SwitchStep

pub fn switch_widget(s: Switch, f: Frame) -> widget.Widget

pub struct Expander {
    name: string
    title: string
    disabled: bool
    // Whether the section is showing. The application owns it, as it owns which tab is chosen.
    open: bool
    held: bool
}

pub struct ExpanderStep {
    expander: Expander
    toggled: bool
}

pub fn expander_of(name: string, title: string, open: bool) -> Expander

pub fn expander_input(e: Expander, input: Input, bounds: Rect) -> ExpanderStep

pub fn expander_widget(e: Expander, content: widget.Widget, f: Frame) -> widget.Widget

pub enum Progress {
    Fraction(float)
    Busy(float)
}

pub struct ProgressBar {
    name: string
    label: string
    progress: Progress
}

pub fn progress_bar_of(name: string, label: string, fraction: float) -> ProgressBar

pub fn busy_bar_of(name: string, label: string, phase: float) -> ProgressBar

pub fn progress_bar_widget(b: ProgressBar, f: Frame) -> widget.Widget

pub struct NumberField {
    entry: Entry
    least: float
    most: float
    // What a stepper, an arrow key and a wheel notch move by.
    step: float
}

pub struct NumberStep {
    field: NumberField
    // Whether the number this field holds is different from the one it held. **Not whether the text
    // changed**: moving the caret through `100` changes neither, and typing the `.` of `1.5` changes
    // the text while the number is still on its way.
    changed: bool
}

pub fn number_field_of(

pub fn number_in(n: NumberField) -> Option<float>

pub fn number_or(n: NumberField, fallback: float) -> float

pub fn stepped(n: NumberField, by: float) -> NumberField

pub fn committed(n: NumberField) -> NumberField

pub fn number_field_input(n: NumberField, input: Input, bounds: Rect, f: Frame) -> NumberStep

pub fn number_field_widget(n: NumberField, f: Frame) -> widget.Widget

pub struct Hover {
    // The control the pointer is on, "" for none.
    on: string
    // Milliseconds the pointer has been on it without moving to another control.
    waited: int
}

pub fn no_hover() -> Hover

pub fn hovering(h: Hover, name: string, by: int) -> Hover

pub fn tip_due(h: Hover, after: int) -> bool

pub fn tip_words(h: Hover, laid: Laid) -> string

pub fn tip_layer(words: string, over: Rect, within: Rect, f: Frame) -> widget.Layer

pub struct Ink {
    from: int
    to: int
    colour: Color
}

pub struct Entry {
    name: string
    // What a reader calls the field. **Declared, not taken from the text inside**, because the text
    // inside a field is its *value* -- a field announced by its contents would introduce itself as
    // whatever the user last typed.
    label: string
    editor: edit.Editor
    history: edit.History
    disabled: bool
    // Readable but not editable. The caret still moves and text can still be selected and copied,
    // which is the difference from disabled and the reason both exist.
    readonly: bool
    // A press has happened and the pointer has not come up: a move extends the selection.
    dragging: bool
    // Whether the last change was a typed character, so that a run of them is **one** undo step.
    //
    // Without this, Ctrl-Z after typing a word steps back one letter at a time, which is not what
    // undo means to anyone. The rule is the one editors use: consecutive `Text` inserts coalesce,
    // and anything else -- a delete, a paste, a caret move, a click -- ends the run.
    typing: bool
    // How many lines tall. **1 is a field and anything more is a text area**, and the difference is
    // only three things: Enter inserts a newline instead of being left for a default button, Up and
    // Down move by line instead of doing nothing, and the widget draws a column of lines instead of
    // one run.
    //
    // One control rather than two because the rest -- clusters, words, selection, undo, the click
    // that places a caret -- is identical, and `edit` never cared how many lines its text had: a
    // newline has always been an ordinary character in that string. A separate `TextArea` would have
    // been a second copy of all of it to gain a field.
    rows: int
    // How far the text is scrolled, in points. **A text area scrolls itself**, because the only
    // thing that knows where the caret went is the code that moved it -- a caller would have to
    // recompute the caret's line to follow it, which is the control's own arithmetic done twice.
    // Zero and unused for a one-line field.
    scroll: float
    // Text being composed through an input method, which is **not** in `editor.text` until it is
    // committed -- see `edit.Preedit`. `None` is the ordinary state and the only one most fields
    // are ever in.
    preedit: Option<edit.Preedit>
    // Shown as a run of bullets rather than as itself, and **published to a screen reader the same
    // way**: the real text leaves this module only when the application asks the field for it.
    //
    // A field, not a second control, for `rows`'s reason: everything else about a password field
    // is a text field -- the caret, the selection, the undo, the click that places a caret -- and
    // the three things that differ are what is drawn, what `Role` it declares, and that it will
    // not put its contents on the clipboard.
    masked: bool
    // Runs of the text drawn in their own colour -- syntax colouring, and anything else that
    // paints a span rather than changing it. Empty for every ordinary field. See `Ink`.
    //
    // **Ignored by a masked field.** The indices count characters of the text, and a masked field
    // draws bullets instead -- so a span would colour the wrong ones, and there is nothing in a
    // password worth colouring.
    inks: [Ink]
}

pub fn entry_of(name: string, label: string, text: string) -> Entry

pub fn text_area_of(name: string, label: string, text: string, rows: int) -> Entry

pub fn password_of(name: string, label: string, text: string) -> Entry

pub fn shown_text(e: Entry) -> string

pub fn entry_composing(e: Entry, text: string, caret: int) -> EntryStep

pub fn entry_composing_marked(e: Entry, text: string, caret: int, from: int, to: int) -> EntryStep

pub fn entry_commit(e: Entry, text: string) -> EntryStep

pub struct EntryStep {
    entry: Entry
    // Text the field is asking to be put on the system clipboard, or "". **An output, because this
    // module cannot reach a clipboard** -- the same seam `Input.Paste` is the other half of. An
    // application keeps it in a value of its own until the platform side of F273 exists.
    copied: string
    // Whether the text changed -- not the caret, not the selection. What a form watches to know it
    // is dirty, and the reason it is separate from the new state rather than something a caller
    // works out by comparing two strings.
    changed: bool
}

pub fn entry_input(e: Entry, input: Input, bounds: Rect, f: Frame) -> EntryStep

pub fn caret_at_x(text: string, size: float, m: widget.Metrics, x: float) -> int

pub fn caret_at_x(text: string, size: float, m: widget.Metrics, x: float, face: int) -> int

pub fn entry_widget(e: Entry, f: Frame) -> widget.Widget

pub struct List {
    name: string
    label: string
    // **The rows, as values rather than as text.** Swing's `JList` takes a `ListModel` of objects
    // and renders each through a cell renderer, which is how a list of dates sorts and prints as
    // dates rather than as whatever `toString` happened to give. `Value` is this library's answer
    // to the same question and is already the `Table`'s, so a list and a table holding the same
    // column hold it the same way -- see `Value`, and `Table.rows`.
    items: [Value]
    // The selected row, or -1 for none -- which is a real state, not an error: an empty list has no
    // selection, and neither does one the user has not touched.
    selected: int
    // How far the rows have scrolled, in the same units the layout uses. Not a row index: a list
    // scrolled by whole rows cannot show half of one, and half of one is what tells a user there is
    // more below.
    offset: float
    disabled: bool
    // What a reader calls this and its rows.
    //
    // **Fields rather than constants, so a menu can be a list.** A menu's popup is a list of rows
    // with one highlighted, scrolled when it is long, driven by the same arrows -- all of which is
    // `List` already. The only honest difference is what a screen reader should call it, so that is
    // the only thing a menu has to change. Duplicating the selection, the scrolling and the key
    // handling to rename two roles would have been the alternative.
    role: widget.Role
    item_role: widget.Role
    // How a `Number`, a `Date` or an `Exact` is written out. The `Table` carries one for the same
    // reason: a value knows what it is, and only a locale knows how it is spelled.
    locale: Locale
    // The drag state of the bar down its right edge, as a tree has.
    bar: ScrollBar
}

pub fn list_of(name: string, label: string, items: [Value]) -> List

pub fn list_of(name: string, label: string, items: [Value], l: Locale) -> List

pub fn list_of(name: string, label: string, items: [string]) -> List

pub fn list_text_at(l: List, i: int) -> string

pub fn no_values() -> [Value]

pub fn as_text_values(items: [string]) -> [Value]

pub struct ListStep {
    list: List
    // The selection changed. An output, as `ButtonStep.activated` is: the application acts on it
    // and stores the list.
    moved: bool
    // A row was chosen -- Enter on the selection. Separate from `moved`, because selecting a row and
    // opening it are different intentions and a list that conflated them would open on every arrow
    // key.
    activated: bool
    // The user has just scrolled to the bottom: "there may be more, fetch it".
    //
    // **A transition and not a state**, which is the whole of the design. A flag that stayed true
    // while the user sat at the bottom would have an application fetching on every frame it drew
    // -- and a list that is showing everything there is never fires it at all, because there is
    // nothing past the end to ask for.
    //
    // This is what the target reaches for an IntersectionObserver to do, and what a list that
    // grows by thirty rows at a time needs from a toolkit. It is not virtualisation: nothing here
    // renders a thousand rows, and the application decides how many exist.
    reached_end: bool
}

pub fn list_at_end(l: List, bounds: Rect, f: Frame) -> bool

pub fn list_input(l: List, input: Input, bounds: Rect, f: Frame) -> ListStep

pub fn list_widget(l: List, f: Frame) -> widget.Widget

pub fn row_name(l: List, index: int) -> string

pub struct Tabs {
    name: string
    label: string
    items: [string]
    // The selected tab. **Not -1**, unlike a list's: a strip says what is shown beneath it, and
    // "nothing is shown" is not a state a tab strip can be in. A strip with no tabs is the one
    // exception, and an application with no tabs should not be building one.
    selected: int
    // The width of every tab. **One number, used by both the widget and the hit test**, which is
    // what keeps a click landing on the tab it looks like it landed on: tabs measured to fit their
    // own labels would need the font metrics in `tabs_input` as well, and a layout computed twice
    // is a layout that can disagree with itself. A caller wanting a snug strip sets this to fit
    // the longest label.
    tab_width: float
    disabled: bool
}

pub fn tabs_of(name: string, label: string, items: [string]) -> Tabs

pub struct TabStep {
    tabs: Tabs
    // The selection changed, so what is beneath the strip has to change with it.
    moved: bool
}

pub fn tabs_input(t: Tabs, input: Input, bounds: Rect) -> TabStep

pub fn tabs_widget(t: Tabs, f: Frame) -> widget.Widget

pub fn tab_name(t: Tabs, index: int) -> string

pub struct TreeRow {
    label: string
    // How deep, 0 at the top.
    depth: int
    // Whether it can be opened. **Declared rather than worked out from the next row's depth**,
    // because a branch with nothing in it yet is still a branch: a tree that inferred this would
    // draw an empty folder as a leaf, and a user would have no way to see that it is empty rather
    // than missing.
    expandable: bool
    expanded: bool
}

pub fn tree_row(label: string, depth: int) -> TreeRow

pub fn tree_branch(label: string, depth: int, expanded: bool) -> TreeRow

pub struct Tree {
    name: string
    label: string
    rows: [TreeRow]
    // The selected row, **as an index into `rows` and not into the visible ones**. Collapsing a
    // branch must not silently move the selection onto a different thing, and an index into a
    // projection does exactly that.
    selected: int
    offset: float
    // The drag state of the bar down its right edge. **Only the drag**: where the thumb sits
    // comes from `offset` and the row count, which this already has, and a second copy of either
    // is a second thing to keep in step.
    bar: ScrollBar
    disabled: bool
}

pub fn tree_of(name: string, label: string, rows: [TreeRow]) -> Tree

pub fn visible_rows(t: Tree) -> [int]

pub struct TreeStep {
    tree: Tree
    // The selection changed.
    moved: bool
    // A row was chosen -- Enter or Space on it. A branch opening is not a choice.
    activated: bool
    // A branch was opened or closed, which is a different thing from either: what is on screen
    // changed and the selection did not.
    toggled: bool
}

pub fn tree_input(t: Tree, input: Input, bounds: Rect, f: Frame) -> TreeStep

pub fn parent_of(t: Tree, index: int) -> int

pub fn tree_widget(t: Tree, f: Frame) -> widget.Widget

pub fn tree_row_name(t: Tree, index: int) -> string

pub struct ScrollBar {
    name: string
    // Where in the thumb the pointer took hold, measured from the thumb's start, or `None` when
    // nothing is being dragged.
    grabbed: Option<float>
    // What one notch of the wheel moves. **Given rather than guessed**, because the only sensible
    // amount is a row of whatever is being scrolled and this control cannot know what that is.
    step: float
    disabled: bool
}

pub fn scroll_bar_of(name: string, step: float) -> ScrollBar

pub struct Thumb {
    start: float
    length: float
}

pub fn thumb_of(style: Style, track: float, content: float, view: float, offset: float) -> Thumb

pub fn on_the_bar(style: Style, bounds: Rect, p: Point, content: float, view: float) -> bool

pub fn scrolls(content: float, view: float) -> bool

pub struct ScrollBarStep {
    bar: ScrollBar
    // The offset the scrolled control should take. Handed back rather than stored, because the
    // control owns it.
    offset: float
    moved: bool
}

pub fn scroll_bar_input(

pub fn scroll_bar_widget(

pub val thumb_name = "thumb"

pub fn paint_scroll_bars(l: Laid, over: scene.Scene, f: Frame) -> scene.Scene

pub struct Menu {
    name: string
    // What the collapsed control shows: the menu's name, "File".
    title: string
    // What a reader calls it, which for a menu is the same thing.
    //
    // **Two fields for one string, which a combo box is why.** This struct held both menus and
    // value pickers until 2026-11-16, and a picker's collapsed box shows its *value* while a reader
    // must still hear a fixed name -- "staging, combo box" and not "staging, combo box, staging".
    // The picker is `ComboBox` now and the two fields are the same here, kept because a menu whose
    // accessible name differs from its title is a thing an application may yet want.
    label: string
    items: List
    open: bool
    disabled: bool
}

pub fn menu_of(name: string, title: string, items: [string]) -> Menu

pub fn collapsed_text(m: Menu) -> string

pub struct ComboBox {
    name: string
    // What a reader calls it, and it does **not** change with the value: a control that introduced
    // itself as its own current value would announce "staging, combo box, staging". What the
    // collapsed box *shows* is the value; what it is *called* is this.
    label: string
    items: List
    open: bool
    disabled: bool
}

pub struct ComboBoxStep {
    combo: ComboBox
    // The item now chosen, or -1 when the value did not change.
    //
    // **Set by a closed arrow key as well as by the popup**, which is the whole difference from
    // `MenuStep.chosen`: a combo box changes value without ever opening, so an application that
    // only watched the popup would miss most of the changes a keyboard user makes.
    chosen: int
    // The popup closed without choosing. Separate from `chosen` for `MenuStep`'s reason.
    dismissed: bool
}

pub fn combo_box_of(name: string, label: string, items: [Value], selected: int) -> ComboBox

pub fn combo_box_of(

pub fn combo_box_of(name: string, label: string, items: [string], selected: int) -> ComboBox

pub fn combo_box_value(c: ComboBox) -> string

pub fn combo_box_chosen(c: ComboBox) -> Value

pub fn combo_box_text(c: ComboBox) -> string

pub fn combo_box_widget(c: ComboBox, f: Frame) -> widget.Widget

pub fn combo_box_popup_layer(c: ComboBox, at: Rect, f: Frame) -> widget.Layer

pub fn combo_box_input(

pub struct MenuStep {
    menu: Menu
    // The item chosen, or -1. An output: the application acts on it and stores the menu.
    chosen: int
    // The menu closed without choosing -- Escape, or a click away from it. Separate from `chosen`,
    // because a caller that treated them alike would act on a dismissal.
    dismissed: bool
}

pub fn menu_input(

pub fn menu_title_widget(menu: Menu, f: Frame) -> widget.Widget

pub fn menu_popup_layer(menu: Menu, at: Rect, f: Frame) -> widget.Layer

pub enum CalendarView {
    Days
    Months
    Years
}

pub struct DateField {
    name: string
    label: string
    // What is chosen. **Always a real date**, which is the whole benefit of not being editable.
    on: time.LocalDate
    disabled: bool
    // Where the keyboard is in the open calendar, and whose month the calendar shows.
    //
    // **Not the value.** A user paging through months with the arrows has chosen nothing yet, and
    // a calendar that moved the value as the highlight moved would write a date into the field on
    // the way past -- the number field's `committed` rule, met again.
    at: time.LocalDate
    open: bool
    view: CalendarView
    // **Injected and not read from a global**, for `Style`'s reason exactly: a module that reached
    // for `locale.default()` could not be tested against two locales, and which day the week
    // starts on is precisely the thing that differs between them. `date_field_of` takes the
    // default so that the common case says nothing, and a caller with two panels in two languages
    // can still set it per field.
    locale: Locale
}

pub struct DateStep {
    field: DateField
    changed: bool
}

pub fn date_field_of(name: string, label: string, on: time.LocalDate) -> DateField

pub fn date_field_of(name: string, label: string, on: time.LocalDate, l: Locale) -> DateField

pub fn date_in(d: DateField) -> time.LocalDate

pub fn date_text(d: DateField) -> string

pub fn calendar_box(d: DateField, laid: Laid, within: Rect, f: Frame) -> Rect

pub fn date_field_input(d: DateField, input: Input, laid: Laid, within: Rect, f: Frame) -> DateStep

pub fn date_field_widget(d: DateField, f: Frame) -> widget.Widget

pub fn calendar_layer(d: DateField, laid: Laid, within: Rect, f: Frame) -> Option<widget.Layer>

pub enum ToolItem {
    Command(Button)
    // A rule between two groups of commands. **Not a `Separator` control**, deliberately: that
    // role means a draggable divider here, and `cursor_for` turns it into a resize arrow from the
    // shape of its box. A toolbar's rule is decoration and must not promise a drag.
    Divider
}

pub struct Toolbar {
    name: string
    label: string
    items: [ToolItem]
    // Which command the arrows are on, and -1 when the strip holds none. Always an index that
    // names a `Command`: `stepped_along` skips the dividers so the arrows never stop on one.
    at: int
    // The command the pointer is holding down, or -1.
    //
    // **The toolbar's own, where every other control reads `pressed_in(f, name)`.** The frame
    // carries the name `target` answered, and `target` answers the *tab stop* -- which here is the
    // toolbar, not the command inside it. So the strip has to remember which of its parts is down.
    held: int
    disabled: bool
    // Whatever did not fit, offered as a menu.
    //
    // **Its rows are never stored**, only its open-or-shut and its highlight: which commands
    // overflow depends on the width the strip was given, and a stored list of them is a second
    // answer to that question, free to disagree with the one the window drew.
    more: ContextMenu
}

pub struct ToolbarStep {
    toolbar: Toolbar
    // The name of the command that fired, or "".
    //
    // **A name and not an index**, because an application routes by name everywhere else here and
    // because an index renumbers the moment a divider is added above it.
    fired: string
}

pub fn toolbar_of(name: string, label: string, items: [ToolItem]) -> Toolbar

pub fn tool_name(t: Toolbar, index: int) -> string

pub fn toolbar_more_name(t: Toolbar) -> string

pub fn toolbar_fits(t: Toolbar, width: float, f: Frame) -> int

pub fn toolbar_widget(t: Toolbar, width: float, f: Frame) -> widget.Widget

pub fn toolbar_layer(t: Toolbar, laid: Laid, width: float, within: Rect, f: Frame) -> Option<widget.Layer>

pub fn toolbar_input(

pub struct ChartView {
    name: string
    label: string
    data: chart.Chart
    // The row a keyboard or a pointer is on, or -1. **A chart's selection is a reading**, which is
    // what a hover readout shows and what a reader is told.
    at: int
}

pub fn chart_view_of(name: string, label: string, data: chart.Chart) -> ChartView

pub fn chart_rows(v: ChartView) -> [[Value]]

pub fn chart_columns(v: ChartView) -> [Column]

pub fn chart_said(v: ChartView, l: Locale) -> string

pub struct ChartStep {
    view: ChartView
    // The reading moved. What an application watches to show a readout beside the chart.
    moved: bool
}

pub fn chart_input(v: ChartView, input: Input, bounds: Rect, f: Frame) -> ChartStep

pub fn chart_slot_at(v: ChartView, p: Point, bounds: Rect, f: Frame) -> int

pub fn chart_style_of(f: Frame) -> chart.ChartStyle

pub fn chart_widget(v: ChartView, f: Frame) -> widget.Widget

pub struct Card {
    panel: Panel
    // Beside the title, and decoration: the title is what a reader hears, so an icon that
    // repeated it would be said twice. See `Button.icon`, which is the one case where it is not.
    icon: Option<widget.Icon>
}

pub fn card_of(name: string, title: string, empty: string) -> Card

pub fn card_loading(c: Card) -> Card

pub fn card_ready(c: Card) -> Card

pub fn card_failed(c: Card, why: string) -> Card

pub fn card_widget(c: Card, content: widget.Widget, actions: widget.Widget, f: Frame) -> widget.Widget

pub struct Badge {
    text: string
    // **Loud wears the accent**, for a count that is asking to be noticed: unread messages, things
    // needing attention. Quiet is the ordinary face, for a status word that is merely true.
    loud: bool
}

pub fn badge_of(text: string) -> Badge

pub fn count_badge_of(n: int) -> Option<Badge>

pub fn badge_widget(b: Badge, f: Frame) -> widget.Widget

pub struct Search {
    entry: Entry
}

pub struct SearchStep {
    search: Search
    // The text changed, which for a search field is the only output that matters: there is nothing
    // to commit, because what is typed *is* the query.
    changed: bool
}

pub fn search_of(name: string, label: string) -> Search

pub fn search_text(s: Search) -> string

pub fn search_clear_name(s: Search) -> string

pub fn search_input(s: Search, input: Input, bounds: Rect, f: Frame) -> SearchStep

pub fn search_widget(s: Search, f: Frame) -> widget.Widget

pub enum Waiting {
    // Nothing has been asked for yet.
    Idle
    Loading
    // There is content, and it is what the caller passed.
    Ready
    // It did not work. The words are the caller's, because only the caller knows what failed.
    Failed(string)
}

pub struct Panel {
    name: string
    label: string
    // What to say when it is `Ready` and holds nothing -- "No medications recorded". **Separate
    // from `Idle`'s silence**, because an empty answer is an answer.
    empty: string
    state: Waiting
    // How far round the spinner is, 0 to 1, advanced by the application. `lib/ui` has no clock;
    // see `Progress.Busy`, which takes its phase the same way and for the same reason.
    phase: float
}

pub fn panel_of(name: string, label: string, empty: string) -> Panel

pub fn panel_loading(p: Panel) -> Panel

pub fn panel_ready(p: Panel) -> Panel

pub fn panel_failed(p: Panel, why: string) -> Panel

pub fn said_waiting(p: Panel, holds_anything: bool) -> string

pub fn waiting_is_urgent(p: Panel) -> bool

pub fn spinner_widget(phase: float, ink: Color, side: float) -> widget.Widget

pub fn panel_widget(p: Panel, content: widget.Widget, f: Frame) -> widget.Widget

pub struct Autocomplete {
    entry: Entry
    // Every candidate. **Held whole and not pre-filtered by the caller**, because the control has
    // to be able to say "three of a hundred and twenty" -- and because a caller filtering for it
    // would be a second implementation of the matching rule, free to disagree with the one that
    // decides what the popup shows.
    all: [string]
    // The suggestion list's own state: which row is highlighted, how far it has scrolled.
    //
    // **Its `items` are not state.** Which suggestions exist is a function of what has been typed,
    // so they are filled in from the filter before anything reads them. The toolbar's overflow
    // menu keeps its rows the same way and for the same reason: a stored copy of a derived list is
    // a second answer to a question that already has one.
    suggestions: List
    open: bool
    // Whether a value matching no suggestion may be committed.
    //
    // **False by default, because "autocomplete" means completing from a known set.** A diagnosis,
    // a medication, a MIME type: the value is a code and a field that accepted anything typed into
    // it would be a field that silently accepts bad data. A caller whose field is a free note says
    // so.
    free_text: bool
    // The last value committed, which is what the text reverts to when a user abandons what they
    // were typing.
    chosen: string
}

pub struct AutocompleteStep {
    field: Autocomplete
    // The value committed by this input, or "" for none. **A string and not an index**, because a
    // free-text field commits something that is in no list and an index could not name it.
    chosen: string
    changed: bool
}

pub fn autocomplete_of(name: string, label: string, all: [string]) -> Autocomplete

pub fn autocomplete_text(a: Autocomplete) -> string

pub fn autocomplete_matches(a: Autocomplete) -> [string]

pub fn autocomplete_said(a: Autocomplete) -> string

pub fn autocomplete_highlighted(a: Autocomplete) -> string

pub fn autocomplete_input(

pub fn autocomplete_widget(a: Autocomplete, f: Frame) -> widget.Widget

pub fn autocomplete_popup_layer(a: Autocomplete, at: Rect, f: Frame) -> Option<widget.Layer>

pub fn autocomplete_popup_height(a: Autocomplete, f: Frame) -> float

pub struct ContextMenu {
    menu: Menu
    // Where it was asked for, in the window's own points. Meaningless while it is shut, which is
    // why nothing reads it unless `menu.open`.
    at: Point
}

pub fn context_menu_of(name: string, label: string, items: [string]) -> ContextMenu

pub fn context_menu_open(c: ContextMenu) -> bool

pub fn context_menu_box(c: ContextMenu, within: Rect, f: Frame) -> Rect

pub fn context_menu_key(input: Input) -> bool

pub fn context_menu_opened_at(c: ContextMenu, p: Point) -> ContextMenu

pub fn context_menu_input(c: ContextMenu, input: Input, within: Rect, f: Frame) -> ContextStep

pub struct ContextStep {
    context: ContextMenu
    // The item chosen, or -1, and whether it closed without choosing -- `MenuStep`'s two outputs,
    // carried through unchanged.
    chosen: int
    dismissed: bool
}

pub fn context_menu_layer(c: ContextMenu, within: Rect, f: Frame) -> Option<widget.Layer>

pub enum DialogAnswer {
    Confirm
    Cancel
    Nothing
}

pub fn dialog_keys(input: Input) -> DialogAnswer

pub fn dialog_layer(name: string, title: string, content: widget.Widget, f: Frame) -> widget.Layer

pub val divider_step = 10.0

pub struct SplitPane {
    name: string
    label: string
    // Which way the panes sit: `Row` is two panes side by side with an upright divider, `Column`
    // one above the other. **The direction of the stack, not of the divider**, which is the
    // confusion `JSplitPane.HORIZONTAL_SPLIT` is famous for -- a "horizontal split" there is a
    // *vertical* divider. This field is the stack's direction and nothing else, so there is
    // nothing to get backwards.
    direction: widget.Direction
    // Where the divider starts, in points from the box's start along `direction`. Read it through
    // `split_at`, never directly: this is what the user asked for and that is what they can have.
    position: float
    // The least either pane may be squeezed to, in points. Zero lets a pane be dragged away
    // entirely, which is what a plain pane does everywhere and is why it is the default.
    first_min: float
    second_min: float
    // Where in the divider the pointer took hold, measured from the divider's start, or `None`
    // when nothing is being dragged. `ScrollBar.grabbed` carries the same thing for the same
    // reason: a divider that jumped so its start met the pointer would move the moment it was
    // touched, before anything was dragged.
    grabbed: Option<float>
    disabled: bool
}

pub fn split_pane_of(name: string, label: string, direction: widget.Direction, position: float) -> SplitPane

pub fn divider_name(s: SplitPane) -> string

pub fn split_at(s: SplitPane, style: Style, extent: float) -> float

pub fn divider_rect(s: SplitPane, style: Style, bounds: Rect) -> Rect

pub struct SplitStep {
    split: SplitPane
    // Whether the divider ended up somewhere else. **Where it ended up is on `split`**, unlike
    // `ScrollBarStep.offset`: a scroll bar reports an offset whose owner is the control being
    // scrolled, where a split pane owns its own divider and has nobody to hand the number to.
    moved: bool
}

pub fn split_pane_input(s: SplitPane, input: Input, bounds: Rect, f: Frame) -> SplitStep

pub fn split_pane_widget(

pub enum Value {
    Text(string)
    // A measured quantity: written with the locale's own separators.
    Number(float)
    // A counted quantity, likewise.
    Whole(int)
    // **A number that identifies rather than counts**: a year, a port, a status code, an id.
    //
    // **Never grouped and never localised**, which is the distinction `Whole` cannot make and the
    // reason this variant exists. `1,766` is a wrong year, `8,080` is a wrong port, and `404` is
    // not four hundred and four. Swing reaches the same place by giving the column its own
    // renderer; this says it on the value, where the fact actually lives.
    Code(int)
    // **A quantity that must not be approximated**: money, a dose, a rate. `Number` is a `float`
    // and `0.1 + 0.2` is not `0.3` in one -- so a column of prices held as `Number` is the oldest
    // arithmetic bug there is, printed into a table. This is the variant a currency column uses.
    Exact(decimal)
    // A whole number past what an `int` holds.
    Big(bigint)
    Date(time.LocalDate)
    // **Drawn as a check box**, which is `JTable`'s single most useful default and the reason
    // anybody notices `getColumnClass` exists.
    Flag(Toggle)
    Mark(widget.Icon)
    // A cell with nothing in it, which is **not** the empty string: a missing measurement and a
    // measurement of "" are different facts, and a reader should not be told they are the same.
    Blank
}

pub fn value_text(v: Value, l: Locale) -> string

pub fn value_or_blank(v: Option<string>) -> Value

pub fn value_or_blank(v: Option<float>) -> Value

pub fn value_or_blank(v: Option<decimal>) -> Value

pub fn value_or_blank(v: Option<time.LocalDate>) -> Value

pub fn value_or_blank(v: Option<Toggle>) -> Value

pub struct Column {
    title: string
    width: float
    // Where this column's cells sit in it: text reads from the start, a number from the end.
    //
    // **On the column, not on the cell**, which is what Swing's renderer-per-column and JIDE's
    // `CellStyleTable` both say in their own way. A column of numbers with one of them centred is
    // a mistake every time, so the model does not let it be said.
    align: widget.Along
}

pub fn column_of(title: string, width: float) -> Column

pub fn numeric_column_of(title: string, width: float) -> Column

pub struct Table {
    name: string
    label: string
    columns: [Column]
    // The cells, a row at a time.
    //
    // **A row shorter than `columns` is not an error**: `value_at` answers `Blank` past its end. A
    // table built from a query has a missing value in it sooner or later, and the alternative is
    // every caller padding its rows or this control raising on data it could simply draw.
    rows: [[Value]]
    // The locale its numbers and dates are written in.
    //
    // **Injected and not read from a global**, for `Style`'s reason: a module that reached for
    // `locale.default()` could not be tested against two locales, and a thousands separator is
    // exactly the thing that differs.
    locale: Locale
    // The selected row, or -1 for none -- a real state, as `List.selected` explains at length.
    selected: int
    // Whether the strip of column titles is drawn at all.
    //
    // **A flag, where this was `header_height: 0.0`.** A grid of values inside a dialog has
    // nothing to put in a header, and that is still this table rather than a second control -- but
    // the height is `header_height(m, style)` now and no longer a number a caller can zero. Saying
    // it in a bool says the thing itself: a table either has titles over its columns or it does
    // not, and a caller that wanted a shorter band was never what the zero meant.
    headers: bool
    // How far the rows have scrolled down.
    offset: float
    // How far the columns have scrolled sideways. **One number, read by the header and by the
    // body**, which is what locks the two together: a header scrolling on its own offset is a
    // header that drifts from its columns, and it drifts by exactly the bug you cannot see in a
    // screenshot.
    offset_x: float
    // Which column the rows are ordered by, or -1 for the order they were given in.
    //
    // **The data is never reordered.** `selected` is an index into `rows`, so sorting the array
    // would silently move the selection to a different record -- and the order the caller gave is
    // a fact about the data that a view should not be able to destroy. `sorted_order` is an index
    // projection, which is `Tree.visible_rows`'s pattern.
    sorted_by: int
    ascending: bool
    // Which column's trailing edge is being dragged, or -1.
    resizing: int
    // How far the pointer was from that edge when it took hold, so a resize does not snap the edge
    // to the pointer on the first press. `ScrollBar.grabbed` and `SplitPane.grabbed` carry the same
    // thing for the same reason.
    grabbed: Option<float>
    bar: ScrollBar
    disabled: bool
}

pub fn table_of(name: string, label: string, columns: [Column], rows: [[Value]]) -> Table

pub fn table_of(

pub fn table_of(name: string, label: string, columns: [Column], rows: [[string]]) -> Table

pub fn value_at(t: Table, row: int, column: int) -> Value

pub fn cell_at(t: Table, row: int, column: int) -> string

pub fn compare_values(a: Value, b: Value) -> int

pub fn sorted_order(t: Table) -> [int]

pub fn display_at(t: Table, row: int) -> int

pub fn row_shown_at(t: Table, position: int) -> int

pub fn sorted_on(t: Table, column: int) -> Table

pub fn row_label(t: Table, row: int) -> string

pub fn column_x(t: Table, index: int) -> float

pub fn columns_width(t: Table) -> float

pub fn table_body(t: Table, style: Style, bounds: Rect, m: widget.Metrics) -> Rect

pub fn edge_at(t: Table, style: Style, bounds: Rect, p: Point, m: widget.Metrics) -> int

pub fn table_row_name(t: Table, row: int) -> string

pub fn column_header_name(t: Table, column: int) -> string

pub struct TableStep {
    table: Table
    // The selection changed.
    moved: bool
    // A row was chosen -- Enter or Space on the selection. Separate from `moved` for `ListStep`'s
    // reason: arrowing to a row and opening it are different intentions.
    activated: bool
    // A column's width changed. **A third output rather than folded into `moved`**, because an
    // application that saves its column widths wants to know about this and does not want to write
    // a file every time the user arrows down a row.
    resized: bool
}

pub fn table_input(t: Table, input: Input, bounds: Rect, f: Frame) -> TableStep

pub fn table_widget(t: Table, f: Frame) -> widget.Widget

pub struct Radios {
    name: string
    label: string
    items: [string]
    // The chosen option, or -1 for none. **A real state**: a group a user has not touched and that
    // was given no default has nothing chosen, and inventing one would answer a question nobody
    // asked.
    selected: int
    disabled: bool
}

pub fn radios_of(name: string, label: string, items: [string], selected: int) -> Radios

pub fn radio_name(r: Radios, index: int) -> string

pub struct RadioStep {
    radios: Radios
    moved: bool
}

pub fn radios_input(r: Radios, input: Input, bounds: Rect, f: Frame) -> RadioStep

pub fn radios_widget(r: Radios, f: Frame) -> widget.Widget

pub val thumb_width = 14.0

pub val thumb_height = 18.0

pub struct Slider {
    name: string
    label: string
    value: float
    least: float
    most: float
    // What one arrow key moves, and what the value is snapped to. **Zero means no snapping**,
    // which is a continuous slider and a real thing to want -- a colour picker's hue has no
    // steps.
    step: float
    // Where in the thumb the pointer took hold, measured from the thumb's start, or `None` when
    // nothing is being dragged.
    //
    // **A distance and not a flag**, which is what makes a drag resume from where the thumb
    // already is: a slider that recomputed the value from the pointer alone jumped the thumb's
    // own width the instant it was touched, and then tracked the pointer from there. `ScrollBar`
    // and `SplitPane` carry the same offset for the same reason.
    grabbed: Option<float>
    disabled: bool
}

pub fn slider_of(name: string, label: string, least: float, most: float, value: float) -> Slider

pub fn snapped_value(least: float, most: float, step: float, want: float) -> float

pub fn slider_at(s: Slider, width: float) -> float

pub struct SliderStep {
    slider: Slider
    moved: bool
}

pub fn slider_input(s: Slider, input: Input, bounds: Rect) -> SliderStep

pub fn slider_widget(s: Slider, f: Frame) -> widget.Widget

pub struct Link {
    name: string
    // What it says.
    label: string
    // Where it goes, which this layer never reads. It is handed back on activation so an
    // application does not have to keep a second table from names to destinations.
    href: string
    held: bool
    disabled: bool
}

pub fn link_of(name: string, label: string, href: string) -> Link

pub struct LinkStep {
    link: Link
    // **Followed, not clicked**: a press that drags off and releases elsewhere is not a follow,
    // which is `ButtonStep.activated`'s rule and the reason this is built on a button.
    followed: bool
}

pub fn link_input(l: Link, input: Input, bounds: Rect) -> LinkStep

pub fn link_widget(l: Link, f: Frame) -> widget.Widget

pub fn split_pane_cursor(s: SplitPane, style: Style, bounds: Rect, p: Point) -> widget.Cursor

pub fn table_cursor(t: Table, style: Style, bounds: Rect, p: Point, m: widget.Metrics) -> widget.Cursor

pub struct Type {
    face: int
    size: float
    line: float
}

pub struct TypeScale {
    // An application's own headings. No control draws one.
    heading: Type
    // Text a control contains, and **every caret calculation**: a caret measured in one role and
    // drawn in another drifts by a character per character, which `Style.text_font` already caused
    // once.
    body: Type
    // A control's own name: a button's words, a tab's title, a check box's label.
    label: Type
    // A hint, a footnote, a count beside a heading.
    caption: Type
    // A monospaced pane. The gallery's Code tab is the first caller this layer ever had for a
    // second face, and it used to reach it by overriding `text_font` at the call site.
    code: Type
}

pub struct Shape {
    // A chosen row in a list, a tree or a table: a subtler shape than a control's own face.
    small: widget.Radius
    // A control's face: a button, a field, a tab, a menu's title, a scroll bar's thumb.
    medium: widget.Radius
    // What stands in front of the window: a menu's popup, a combo box's list, a dialog's panel.
    // **Rounded further than a face**, which is what every system does -- Material puts a dialog at
    // 28 and a button at 20 -- and what the single `corner` token could not say.
    large: widget.Radius
    // As round as the box allows: a radio's dot, a capsule.
    full: widget.Radius
}

pub struct Space {
    none: float
    xs: float
    s: float
    m: float
    l: float
    xl: float
}

pub struct Minimum {
    // The least a control the pointer must hit may measure on either axis.
    target: float
    // The shortest a scroll bar's thumb may become, however long the content is. A thumb that
    // shrank without limit would vanish on exactly the document it matters most on.
    thumb: float
}

pub struct Stroke {
    // One device line: a table's grid, a link's underline, and the gap between the two -- the rule
    // sits one hairline below the text, so the gap and the line move together.
    hair: float
    // A line meant to be read *as* a line: a split pane's divider.
    line: float
}

pub struct Style {
    padding: Insets
    ink: Color
    face: Color
    // The surface a control is drawn **on**, as against `face`, which is what a control is drawn
    // **as**.
    //
    // **Added because there was no such distinction and a dialog proved it.** A dialog's panel was
    // `style.face`, and so is every button's face -- so the buttons in the first dialog anyone
    // looked at were invisible: the text was there, floating, with no edge or fill to say it could
    // be pressed. The same shape had already cost a split pane's divider, drawn in `face` beside
    // a control in `face`. Two colours for two things is what every other widget set has, and one
    // colour for both is a default that hides controls.
    //
    // **So the plain look does not keep its pixels here**, where F286 deliberately did: a look
    // that cannot show a button on a panel is not a look to preserve.
    panel: Color
    // What the window itself is painted, behind every control.
    //
    // **A look and feel could not say what colour the window was**, which is the same gap
    // `accent` had: the gallery wrote `rgb8(246, 247, 249)` into its own `raster.Options` and
    // curly wrote `rgb(0.98, 0.98, 0.99)` into its own, so the one colour with the largest area in
    // the window was the one the theme did not own. It is the reason a dark look and feel was not
    // expressible here before -- dark controls on a white window is not a dark theme, it is a
    // mistake.
    //
    // **Not drawn by any control**, because no control owns the window: an application passes this
    // to `raster.Options.background` (or fills with it) and the controls sit on top. That is the
    // one token here with a caller-side obligation, and `lib/ui/theme`'s header says so too.
    window: Color
    // The lines between a table's cells.
    //
    // **A table without them is a list with gaps in it.** Rows of numbers in columns need
    // something to tell the eye which cell a figure belongs to -- which is what every spreadsheet
    // and every table in every toolkit draws, and what this one was missing. Light enough to
    // recede, dark enough to be a line: it has to read against both the row's own face and the
    // window behind it.
    grid: Color
    // Under the text being composed through an input method, and under the run of it the method
    // is converting right now. **Two, because they mean different things to the person typing**:
    // the first says "this is not committed yet" and the second says "this is the part your next
    // keystroke changes", and a field that drew them alike would lose the distinction a candidate
    // list is navigated by.
    composing_face: Color
    converting_face: Color
    // The marks a tree's branch draws when it is closed and when it is open.
    //
    // **In the style rather than written into the control**, because this layer is handed whatever
    // font bytes an application found and cannot know what is in them: a face without triangles
    // would draw two empty boxes and leave every folder looking identical. An application that
    // knows its font can put anything here, including `">"` and `"v"`.
    // What covers the window behind a dialog. **Translucent rather than opaque**, because a modal
    // dialog should say "this is waiting for you" and not "the window is gone" -- a user needs to
    // see what they were doing to decide what to answer.
    scrim: Color
    // The one colour a look and feel is recognised by: a focus ring, a selected row, whatever a
    // theme wants to say is *this application's*.
    //
    // **`Style` had none, which is why the gallery hard-coded a blue into its own paint call.**
    // Every system has this token and calls it something different -- primary, accent, brand --
    // and the thing they agree on is that it is one colour, named once, and that everything
    // identifying belongs to it. Without it a theme can be a different grey and nothing more.
    accent: Color
    // What reads **on** the accent: a primary button's words, a selected row's text. Not derived,
    // because a light accent wants dark text and a dark one wants light, and guessing from
    // luminance is how a theme ends up with grey-on-grey at exactly one value.
    on_accent: Color
    // The five state rows: what this control is drawn as at rest, hovered, pressed, disabled and
    // selected. See `States`, and D5 in docs/design/look-and-feel.md.
    //
    // **This replaced four loose fields** -- `pressed_face`, `disabled_ink`, `selection` and
    // `hover_tint` -- and `face_border` with them, because an edge is part of what a state looks
    // like and two sources for one border is a disagreement waiting to happen. Each of the four
    // could say one thing about one state: a fill, or a text colour, or a translucent veil painted
    // over a finished scene that could change neither an edge nor a word.
    states: States
    // The spacing scale, the floors density may not cross, and the widths of lines. See each
    // struct above: they are three groups rather than one because only the first scales.
    space: Space
    minimum: Minimum
    stroke: Stroke
    // What falls behind a surface that is in front of the window: a menu's popup, a combo box's
    // list, a dialog's panel.
    //
    // **Only those three, and that is the rule rather than an omission.** A shadow says "this is
    // in front of that", so a window where every control had one would be saying nothing at all --
    // and would pay eight extra fills per control to say it. `None` turns it off for a flat look
    // and feel, which is what a theme following GNOME's Adwaita would want.
    //
    // **Not a state's business**, unlike the border and the corner `Parts` carries: a popup is in
    // front of the window whatever the pointer is doing.
    raised: Option<widget.Shadow>
    // The rounding scale. **This was one number**, shared by every control's face and by a list's
    // and a tree's chosen row, on the argument that a theme with four-point buttons and square
    // selection is not a theme but two. The argument was right and the conclusion too narrow: a
    // scale the *library* indexes keeps the coherence (nothing can disagree, because nothing but
    // `controls` chooses) while letting a look be rounder on a dialog than on a button, which every
    // system is. See D1 in docs/design/look-and-feel.md.
    //
    // **A `widget.Radius` rather than a number**, because a number could not serve the sizes a
    // theme has to cover at once: `Points(6)` is right on a button and nearly circular on a check
    // box, which is why the check box used to carry a cap of its own. The cap is now part of what
    // `Points` means, and a theme that wants a capsule says `Fraction(0.5)` -- see
    // `widget.Radius`, which has the whole argument.
    shape: Shape
    // The type scale. **This was four loose fields** -- `text_size`, `heading_size`, `small_size`
    // and `text_font` -- of which two, `heading_size` and `small_size`, were read by nothing at all
    // while two applications wrote their own. See `TypeScale`.
    text: TypeScale
    // Which face an icon *glyph* comes from: an index into the faces a window gave
    // `widget.Metrics` and `raster.Options.fonts`.
    //
    // **Not a role in the type scale**, because an icon is not text: it has no line height to
    // stretch, and its size is an em square taken from whatever role is drawing beside it.
    //
    // Named by purpose here so that no control writes a literal index. `widget` works in indices
    // because the renderer does; meaning belongs at this layer.
    icon_font: int
    // The space between an icon and the label it sits beside.
    //
    // **One number, like `corner_radius` above and for the same reason**: a theme with a wide gap
    // on buttons and a tight one everywhere else is not a theme, it is two. It is spacing rather
    // than the icon's size, which comes from `text_size` -- an icon is an em square, the rule an
    // icon font follows, so it grows with the text instead of with the padding.
    icon_gap: float
}

pub struct Frame {
    style: Style
    metrics: widget.Metrics
    hovered: string
    pressed: string
    focused: string
}

pub fn frame_of(style: Style, m: widget.Metrics) -> Frame

pub struct Parts {
    // **A paint, not a colour** (D10): a look can name a span of its own ramp as a face, and
    // `from == to` is a flat fill -- so solid and gradient are one form with no `Option` and no
    // second field. What a look *writes* is `theme.Fill`, two step numbers and an angle; this is
    // what `derive` builds from it and what a control draws.
    face: Paint
    ink: Color
    border: Option<widget.Border>
    // How far the drawn frame grows beyond its resting inset. **Paint time only**: see
    // `widget.Surface.inset` and the invariant in D5 -- no state may change a control's bounds,
    // because a hit test has to read the same calculation the layout did.
    expansion: float
    // This state's own rounding, where it differs from the shape scale. `None` is the scale's
    // answer, which is what every state wants until one does not.
    corner: Option<widget.Radius>
}

pub struct States {
    rest: Parts
    hovered: Parts
    pressed: Parts
    disabled: Parts
    selected: Parts
}

pub enum Mood {
    Rest
    Hovered
    Pressed
    Disabled
    Selected
}

pub fn mood_of(disabled: bool, pressed: bool, selected: bool, hovered: bool) -> Mood

pub fn parts_for(style: Style, mood: Mood) -> Parts

pub fn hovered_in(f: Frame, name: string) -> bool

pub fn reserve_of(style: Style) -> float

pub fn pressed_in(f: Frame, name: string) -> bool

pub fn face_for(m: widget.Metrics, want: int) -> int

pub fn line_for(m: widget.Metrics, t: Type) -> float

pub fn row_height(m: widget.Metrics, style: Style) -> float

pub fn header_height(m: widget.Metrics, style: Style) -> float

pub fn indent_for(style: Style) -> float

pub fn is_text_input(input: Input) -> bool

```

## `crypto`

Hashes, message authentication, key derivation, password hashing, one authenticated cipher and base64, on OpenSSL's libcrypto (already linked for TLS) and, for bcrypt, libxcrypt.

```
pub native fn sha1(data: [byte]) -> [byte]

pub native fn sha256(data: [byte]) -> [byte]

pub native fn sha384(data: [byte]) -> [byte]

pub native fn sha512(data: [byte]) -> [byte]

pub native fn hmac_sha1(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha256(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha384(key: [byte], data: [byte]) -> [byte]

pub native fn hmac_sha512(key: [byte], data: [byte]) -> [byte]

pub native fn pbkdf2_hmac_sha256(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]

pub native fn pbkdf2_hmac_sha512(password: [byte], salt: [byte], iterations: int, length: int) -> [byte]

pub fn bcrypt_hash(password: string, cost: Option<int> = 10) -> Result<string, string>

pub native fn bcrypt_verify(password: string, hash: string) -> bool

pub native fn argon2id_raw(

pub fn argon2id_hash(

pub fn argon2id_verify(password: string, encoded: string) -> bool

pub fn password_hash(password: string) -> Result<string, string>

pub fn password_verify(password: string, stored: string) -> bool

pub fn password_needs_rehash(stored: string) -> bool

pub native fn aes_gcm_encrypt(key: [byte], plaintext: [byte]) -> Result<[byte], string>

pub native fn aes_gcm_decrypt(key: [byte], blob: [byte]) -> Result<[byte], string>

pub native fn ecdsa_p256_generate() -> Result<string, string>

pub native fn ecdsa_p256_public_pem(private_key_pem: string) -> Result<string, string>

pub native fn ecdsa_p256_public_point(key_pem: string) -> Result<[byte], string>

pub native fn ecdsa_p256_sign(private_key_pem: string, data: [byte]) -> Result<[byte], string>

pub native fn ecdsa_p256_verify(public_key_pem: string, data: [byte], signature: [byte]) -> bool

pub native fn bytes_xor(a: [byte], b: [byte]) -> [byte]

pub native fn bytes_equal(a: [byte], b: [byte]) -> bool

pub fn base64_encode(data: [byte]) -> string

pub fn base64_decode(s: string) -> Result<[byte], string>

pub fn base64url_encode(data: [byte]) -> string

pub fn base64url_decode(s: string) -> Result<[byte], string>

```

## `csv`

CSV (RFC 4180) as `Json`: a table is a `Json` array of objects, one per data row and keyed by the header row, so any `#[derive(Json)]` type decodes from a row with its own `_from_json` and encodes back.

```
pub fn parse_rows(text: string) -> Result<[[string]], string>

pub fn parse_rows(text: string, delimiter: string) -> Result<[[string]], string>

pub fn parse(text: string) -> Result<Json, string>

pub fn default_charset() -> charset.Charset

pub fn parse(data: [byte], set: charset.Charset) -> Result<Json, string>

pub fn parse(data: [byte]) -> Result<Json, string>

pub fn parse(text: string, delimiter: string) -> Result<Json, string>

pub fn parse_typed(text: string) -> Result<Json, string>

pub fn parse_typed(text: string, delimiter: string) -> Result<Json, string>

pub fn parse_columns(text: string, columns: [string]) -> Result<Json, string>

pub fn parse_columns(text: string, columns: [string], delimiter: string) -> Result<Json, string>

pub fn parse_columns_typed(text: string, columns: [string]) -> Result<Json, string>

pub fn parse_columns_typed(text: string, columns: [string], delimiter: string) -> Result<Json, string>

pub fn records(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records(path: string, delimiter: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records_typed(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn records_typed(path: string, delimiter: string) -> Result<Iter<Result<Json, string>>, string>

pub fn format(rows: [[string]]) -> string

pub fn format(rows: [[string]], delimiter: string) -> Result<string, string>

pub fn format(rows: Json) -> Result<string, string>

pub fn format(rows: Json, sep: string) -> Result<string, string>

```

## `datasource`

The module a program calls to use a database, the shape of JDBC: a connection handle plus execute/query taking SQL text and *bound* parameters (a value is never spliced into the SQL string), transactions as plain begin/commit/rollback, and rows handed back as `db.Row`.

```
pub struct Registry {
    drivers: [Driver]
}

pub fn registry(drivers: [Driver]) -> Registry

pub enum Connection {
    Direct(Conn)
    Pool(PoolState)
}

pub fn dialect(conn: Connection) -> string

pub fn open(reg: Registry, url: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn query_one(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Option<Row>, DbError>

pub fn begin(conn: Connection) -> Result<unit, DbError>

pub fn commit(conn: Connection) -> Result<unit, DbError>

pub fn rollback(conn: Connection) -> Result<unit, DbError>

pub fn open_pool(reg: Registry, url: string, size: int) -> Result<Connection, DbError>

pub fn open_pool_with(reg: Registry, url: string, size: int, check_after_seconds: int) -> Result<Connection, DbError>

pub fn acquire(conn: Connection) -> Result<Connection, DbError>

pub fn acquire_timeout(conn: Connection, timeout_ms: int) -> Result<Connection, DbError>

pub fn release(conn: Connection, taken: Connection)

pub fn discard(conn: Connection, taken: Connection)

pub fn with_transaction<R>(conn: Connection, work: fn(Connection) -> Result<R, DbError>) -> Result<R, DbError>

```

## `db`

What every database module shares: the value, row and error types, the readers that turn a `SqlValue` into an ordinary value, and the parser for database URLs.

```
pub struct Conn {
    dialect: string
    close: fn()
    execute: fn(string, [SqlValue]) -> Result<int, DbError>
    query: fn(string, [SqlValue]) -> Result<[Row], DbError>
    rows: fn(string, [SqlValue]) -> Result<Iter<Result<Row, DbError>>, DbError>
}

pub struct Driver {
    name: string
    prefixes: [string]
    poolable: fn(string) -> bool
    idle_check: bool
    open: fn(string) -> Result<Conn, DbError>
}

pub enum SqlValue {
    Null
    Int(int)
    Float(float)
    Text(string)
    Bool(bool)
    Bytes([byte])
}

pub enum DbError {
    NotFound(string)
    Constraint(string)
    Connection(string)
    Sql(string)
}

pub struct Row {
    columns: [string]
    values: [SqlValue]
}

pub fn db_error_message(e: DbError) -> string

pub fn row_get(row: Row, column: string) -> Result<SqlValue, DbError>

pub fn sql_as_text(v: SqlValue) -> Result<string, DbError>

pub fn sql_as_int(v: SqlValue) -> Result<int, DbError>

pub fn sql_as_bool(v: SqlValue) -> Result<bool, DbError>

pub fn sql_as_float(v: SqlValue) -> Result<float, DbError>

pub fn sql_as_decimal(v: SqlValue) -> Result<decimal, DbError>

pub fn sql_lift<T>(r: Result<T, string>, what: string) -> Result<T, DbError>

pub struct UrlParam {
    name: string
    value: string
}

pub struct DbUrl {
    user: string
    password: string
    host: string
    port: int
    database: string
    params: [UrlParam]
}

pub fn percent_decode(s: string) -> Result<string, DbError>

pub fn url_param(u: DbUrl, name: string) -> Option<string>

pub fn parse_url(url: string, prefixes: [string], default_port: int) -> Result<DbUrl, DbError>

pub struct QueryResult {
    rows: [Row]
    affected: int
}

pub fn open_rows(

```

## `derive_entity`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#[derive(Entity)]`, written in Ziman: the column list an entity's queries name, and -- when the attribute carries a table -- the table name and the `eql.Entity` metamodel beside it.

```
pub fn attributes() -> [meta.AttrSpec]

pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_json`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#[derive(Json)]`, written in Ziman: `<name>_to_json`, `<name>_from_json` and the accumulating `<name>_from_json_all`.

```
pub fn from_json(d: meta.Decl, members: [meta.Field]) -> Result<string, string>

pub fn from_json_all(d: meta.Decl, members: [meta.Field]) -> Result<string, string>

pub fn enum_to_json(d: meta.Decl, variants: [meta.Variant]) -> Result<string, string>

pub fn enum_from_json(d: meta.Decl, variants: [meta.Variant]) -> Result<string, string>

pub fn generate(d: meta.Decl) -> Result<string, string>

pub fn attributes() -> [meta.AttrSpec]

pub fn to_json(d: meta.Decl, members: [meta.Field]) -> Result<string, string>

```

## `derive_ord`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#[derive(Ord)]`, written in Ziman.

```
pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_properties`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#[derive(Properties)]`, written in Ziman: `<name>_from_properties` and `<name>_keys`, which bind a struct to the keys under a prefix in `lib/core/properties.zm`'s `Properties`.

```
pub fn attributes() -> [meta.AttrSpec]

pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_reflect`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#[derive(Reflect)]`: prints the declaration a generator was handed, and generates nothing.

```
pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_repository`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#![derive(Repository(Entity))]`, written in Ziman: the CRUD every repository writes the same way.

```
pub fn generate_module(t: meta.Target) -> Result<string, string>

pub fn generate_query(t: meta.Target, f: meta.Function) -> Result<string, string>

```

## `derive_routes`

`#![derive(Routes)]` on a module, and `#[route(..)]` on the functions it routes to: the endpoint table written from the handlers' own signatures.

```
pub fn attributes() -> [meta.AttrSpec]

pub fn generate_module(m: meta.ModuleTarget) -> Result<string, string>

```

## `derive_row`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#[derive(Row)]`, written in Ziman: `<name>_from_row` and `<name>_to_params`, which map a struct to and from one SQL result row.

```
pub fn emit_to_sql(t: meta.Type, src: string, dst: string, n: Mutable<int>) -> Result<string, string>

pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_validate`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#[derive(Validate)]`, written in Ziman.

```
pub fn attributes() -> [meta.AttrSpec]

pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `derive_xml`

**Experimental** (see docs/design/journal/ "Experimental modules"). `#[derive(Xml)]`, written in Ziman: a struct's `<name>_to_xml`/`<name>_from_xml` over `xml.Document`, and the `_to_element`/`_from_element` pair a nested struct composes from.

```
pub fn attributes() -> [meta.AttrSpec]

pub fn generate(d: meta.Decl) -> Result<string, string>

```

## `desktop`

The desktop backend's adapter, both ways: a `window.Event` as a `controls.Input`, and a `scene.Scene` onto the window's pixels.

```
pub fn set_cursor(w: window.Window, c: widget.Cursor) -> Result<unit, string>

pub fn input_of(e: window.Event, d: Density) -> Option<controls.Input>

pub fn stroke_of(k: KeyPress) -> controls.Keystroke

pub struct Painter {
    surface: raster.Surface
    // `None` before the first frame, and after `forget`.
    shown: Option<scene.Scene>
    // What the surface was made for, so a resize or a changed ratio is noticed rather than refused.
    width_pt: float
    height_pt: float
    px_per_pt: float
}

pub struct Repaint {
    // The painter for the next frame. Values are immutable, so the remembered scene comes back this
    // way: `p = try desktop.repaint(p, ...).next` is the shape of a loop.
    next: Painter
    // The pixels redrawn, as one entry per region, and **empty when the scene was unchanged** --
    // in which case nothing was drawn and nothing was sent to the display at all. Worth reading:
    // it is the signal that a loop can afford to wait for the next event rather than spin.
    //
    // **Several, because two distant changes are cheaper as two rectangles than as the box
    // between them** -- see `raster.damage_regions`. It was one `Option<Painted>` until
    // 2026-11-14; a caller that only wants "did anything change" asks `len(regions) > 0`, and one
    // that wants the extent can union them.
    regions: [raster.Painted]
}

pub fn painter_of(width_pt: float, height_pt: float, px_per_pt: float) -> Result<Painter, string>

pub fn painter_fitting(

pub fn forget(p: Painter) -> Painter

pub fn repaint(

pub fn caret_on_screen(

pub fn input_in(

```

## `edit`

A text field's state and every edit you can make to it: a caret, a selection, and the moves.

```
pub struct Editor {
    text: string
    caret: int
    anchor: int
}

pub fn editor_of(text: string) -> Editor

pub fn empty_editor() -> Editor

pub fn selection_start(e: Editor) -> int

pub fn selection_end(e: Editor) -> int

pub fn has_selection(e: Editor) -> bool

pub fn selected_text(e: Editor) -> string

pub fn cluster_before(text: string, at: int) -> int

pub fn cluster_after(text: string, at: int) -> int

pub fn move_left(e: Editor, extend: bool) -> Editor

pub fn move_right(e: Editor, extend: bool) -> Editor

pub fn line_start(text: string, at: int) -> int

pub fn line_end(text: string, at: int) -> int

pub fn move_home(e: Editor, extend: bool) -> Editor

pub fn move_end(e: Editor, extend: bool) -> Editor

pub fn move_up(e: Editor, extend: bool) -> Editor

pub fn move_down(e: Editor, extend: bool) -> Editor

pub fn line_index(text: string, at: int) -> int

pub fn select_all(e: Editor) -> Editor

pub fn place_caret(e: Editor, at: int, extend: bool) -> Editor

pub fn word_before(text: string, at: int) -> int

pub fn word_after(text: string, at: int) -> int

pub fn move_word_left(e: Editor, extend: bool) -> Editor

pub fn move_word_right(e: Editor, extend: bool) -> Editor

pub fn select_word_at(e: Editor, at: int) -> Editor

pub fn insert(e: Editor, what: string) -> Editor

pub fn delete_back(e: Editor) -> Editor

pub fn delete_forward(e: Editor) -> Editor

pub fn delete_word_back(e: Editor) -> Editor

pub fn copy(e: Editor) -> string

pub fn cut(e: Editor) -> Editor

pub fn paste(e: Editor, what: string) -> Editor

pub struct History {
    past: [Editor]
    future: [Editor]
    limit: int
}

pub fn history_of(limit: int) -> History

pub fn record(h: History, before: Editor) -> History

pub fn can_undo(h: History) -> bool

pub fn can_redo(h: History) -> bool

pub struct Step {
    editor: Editor
    history: History
}

pub fn undo(h: History, current: Editor) -> Step

pub fn redo(h: History, current: Editor) -> Step

pub struct Preedit {
    // What is being composed. Not in `Editor.text` until a commit.
    text: string
    // Where in `Editor.text` it sits.
    at: int
    // Where the caret is **inside `text`**, which the input method moves as a user converts.
    caret: int
    // The run being converted right now, as code-point indices into `text`. Equal when there is
    // no such run, which is the ordinary case for a dead key or a compose sequence.
    highlight_start: int
    highlight_end: int
}

pub struct Composing {
    editor: Editor
    preedit: Preedit
}

pub fn compose(e: Editor, text: string, caret: int) -> Composing

pub fn compose_marked(e: Editor, text: string, caret: int, from: int, to: int) -> Composing

pub fn commit(e: Editor, p: Preedit, text: string) -> Editor

pub fn shown(e: Editor, p: Option<Preedit>) -> string

pub fn shown_caret(e: Editor, p: Option<Preedit>) -> int

pub fn shown_highlight(e: Editor, p: Option<Preedit>) -> Option<Span>

pub struct Span {
    start: int
    end: int
}

```

## `encoding`

Base 2, 8, 16, 32 and 64 over `[byte]`, one door per base, each an `_encode` answering text and a `_decode` answering `Result<[byte], string>`.

```
pub fn base2_encode(data: [byte]) -> string

pub fn base2_decode(text: string) -> Result<[byte], string>

pub fn base8_encode(data: [byte]) -> string

pub fn base8_decode(text: string) -> Result<[byte], string>

pub fn base16_encode(data: [byte]) -> string

pub fn base16_decode(text: string) -> Result<[byte], string>

pub fn base32_encode(data: [byte]) -> string

pub fn base32_decode(text: string) -> Result<[byte], string>

pub native fn base64_encode(data: [byte]) -> string

pub native fn base64_decode(s: string) -> Result<[byte], string>

pub native fn base64url_encode(data: [byte]) -> string

pub native fn base64url_decode(s: string) -> Result<[byte], string>

```

## `eql`

**Experimental** (see docs/design/journal/ "Experimental modules"). An **entity query language**: JPQL's shape, over a Ziman struct instead of a Java class.

```
pub struct Entity {
    name: string
    table: string
    columns: [string]
    // The columns that identify one row, in field order: the fields marked `#[pk]`.
    //
    // **Empty is allowed and means "no key was declared"**, not "the first column". A reader that
    // needs one -- anything addressing a single row -- refuses an empty `keys` by name rather than
    // guessing, because the guess was right for all 27 entities in the North Star application and
    // would have been silently wrong for the first table that is keyed on something else.
    //
    // More than one is a compound key, which is simply two fields marked. Nothing here objects to
    // that; a *reader* that can only address a single-column key is the thing that has to say so.
    keys: [string]
    // The struct field each column belongs to, in the same order -- what a *query* names where
    // `columns` is what the *database* has.
    //
    // **Equal to `columns` unless a field carries `#[column(name: "..")]`** (2026-10-29), and the
    // paragraph above this struct is why that matters: field name, column name, what a query may
    // write and what `from_row` reads were one list "by construction", and an override makes them
    // two. So a query still says `e.start`, this pairs it with `start_date`, and the column name
    // appears in the SQL and nowhere a reader of the struct has to think about it.
    //
    // The two lists are parallel. Nothing here enforces that, because the only thing that builds a
    // mismatched pair is a caller writing one by hand, and `column_for` falls back to the field's
    // own name rather than reading past the end of `columns`.
    fields: [string]
}

pub fn entity(name: string, table: string, columns: [string]) -> Entity

pub fn entity(name: string, table: string, columns: [string], fields: [string]) -> Entity

pub fn keyed_entity(name: string, table: string, columns: [string], keys: [string]) -> Entity

pub fn keyed_entity(

pub struct Registry {
    // **Keyed on the folded name**, because SQL identifiers are case-insensitive and every lookup
    // folds what it was given. Folding happens once per entity here instead of once per entity per
    // comparison, which is what a linear scan over this was doing.
    by_name: Map<string, Entity>
    // The names as they were registered, in order, for the refusal message. The keys above are
    // folded and a map has no order, so "registered: vitalsign, bodymeasurement" is neither what
    // the caller wrote nor the order they wrote it in.
    names: [string]
}

pub fn registry(entities: [Entity]) -> Registry

pub struct Binding {
    name: string
    value: SqlValue
    values: [SqlValue]
    list: bool
}

pub fn bind(name: string, value: SqlValue) -> Binding

pub fn bind(name: string, value: int) -> Binding

pub fn bind(name: string, value: float) -> Binding

pub fn bind(name: string, value: string) -> Binding

pub fn bind(name: string, value: bool) -> Binding

pub fn bind(name: string, value: [byte]) -> Binding

pub fn bind(name: string, value: Uuid) -> Binding

pub fn bind(name: string, value: Instant) -> Binding

pub fn bind(name: string, value: LocalDate) -> Binding

pub fn bind(name: string, value: LocalTime) -> Binding

pub fn bind(name: string, value: LocalDateTime) -> Binding

pub fn bind(name: string, value: Duration) -> Binding

pub fn bind(name: string, value: ZoneOffset) -> Binding

pub fn bind(name: string, value: ZoneId) -> Binding

pub fn bind_list(name: string, values: [SqlValue]) -> Binding

pub fn bind_list(name: string, values: [string]) -> Binding

pub fn bind_list(name: string, values: [int]) -> Binding

pub fn bind_list(name: string, values: [Uuid]) -> Binding

pub struct Translated {
    sql: string
    statement: Statement
    bindings: [string]
    lists: [string]
    query: string
    registry: Registry
    // The sizes already worked out, for a query with a collection parameter. See `Shaped`.
    shaped: [Shaped]
}

pub fn translate(reg: Registry, query: string) -> Result<Translated, DbError>

pub fn arrange(t: Translated, bound: [Binding]) -> Result<[SqlValue], DbError>

pub fn prepare(query: Translated, bound: [Binding]) -> Result<Prepared, DbError>

pub struct Prepared {
    sql: string
    params: [SqlValue]
}

pub fn list<E>(

pub fn one<E>(

pub fn insert_one<E>(

pub fn update_one<E>(

pub fn find_by_key<E>(

pub fn find_maybe<E>(

pub fn exists(conn: datasource.Connection, e: Entity, keys: [SqlValue]) -> Result<bool, DbError>

pub fn find_all<E>(

pub fn count_all(conn: datasource.Connection, e: Entity) -> Result<int, DbError>

pub fn delete_where(conn: datasource.Connection, e: Entity, matching: [Binding]) -> Result<int, DbError>

pub fn delete_one(

pub fn delete_all(conn: datasource.Connection, e: Entity) -> Result<int, DbError>

pub fn execute(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<int, DbError>

pub fn rows(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<[Row], DbError>

pub fn count(conn: datasource.Connection, query: Translated, bound: [Binding]) -> Result<int, DbError>

```

## `expr`

**Experimental** (see docs/design/journal/ "Experimental modules"). An expression language over `Json`, for rules and text that are *data* rather than source.

```
pub struct MemberNode {
    target: Expr
    name: string
    // `?.` rather than `.`: a `null` target answers `null` instead of failing. §8 of the design
    // is why absence is an error by default and this is how you say otherwise.
    safe: bool
}

pub struct IndexNode {
    target: Expr
    key: Expr
}

pub struct UnaryNode {
    op: string
    operand: Expr
}

pub struct BinaryNode {
    op: string
    left: Expr
    right: Expr
}

pub struct TernaryNode {
    test: Expr
    then: Expr
    otherwise: Expr
}

pub struct CallNode {
    name: string
    args: [Expr]
}

pub enum Expr {
    Lit(Json)
    // A bare name, resolved against the context object's keys.
    Root(string)
    Member(MemberNode)
    Index(IndexNode)
    Unary(UnaryNode)
    Binary(BinaryNode)
    Ternary(TernaryNode)
    Call(CallNode)
}

pub fn parse(source: string) -> Result<Expr, string>

pub fn must_parse(source: string) -> Expr

pub fn eval(e: Expr, ctx: Json) -> Result<Json, string>

pub fn eval_bool(e: Expr, ctx: Json) -> Result<bool, string>

pub fn eval_string(e: Expr, ctx: Json) -> Result<string, string>

pub fn names(e: Expr) -> [string]

pub fn kind_of(v: Json) -> string

```

## `focus`

Which control has the keyboard, what a click lands on, and where Tab goes next.

```
pub fn controls(l: Laid) -> [Laid]

pub fn first(l: Laid) -> string

pub fn last(l: Laid) -> string

pub fn next(l: Laid, current: string) -> string

pub fn previous(l: Laid, current: string) -> string

pub fn holds(l: Laid, name: string) -> bool

pub fn node_of(l: Laid, name: string) -> Option<Laid>

pub fn control_at(l: Laid, p: Point) -> Option<Laid>

pub fn clicked(l: Laid, p: Point, current: string) -> string

pub fn tab_stop_at(l: Laid, p: Point) -> Option<Laid>

```

## `font`

The font model: which faces a document may use, and what they measure.

```
pub enum StandardFont {
    Helvetica
    HelveticaBold
    HelveticaOblique
    HelveticaBoldOblique
    Times
    TimesBold
    TimesItalic
    TimesBoldItalic
    Courier
    CourierBold
    CourierOblique
    CourierBoldOblique
    Symbol
    ZapfDingbats
}

pub fn base_font_name(f: StandardFont) -> string

pub enum FontRef {
    Standard(StandardFont)
    Embedded(Face)
}

pub fn standard(f: StandardFont) -> FontRef

pub fn load(data: [byte]) -> Result<FontRef, string>

pub fn covers(f: FontRef, text: string) -> Result<Option<int>, string>

pub fn advance(f: FontRef, text: string, size: float) -> Result<float, string>

pub fn ascent(f: FontRef, size: float) -> float

pub fn descent(f: FontRef, size: float) -> float

pub fn line_height(f: FontRef, size: float) -> float

pub fn glyphs_of(f: Face, text: string) -> Result<[int], string>

pub fn hex4(cp: int) -> string

```

## `fs`

The filesystem: files, directories and paths.

```
pub enum FsError {
    NotFound(string)
    Denied(string)
    IsDirectory(string)
    NotDirectory(string)
    Exists(string)
    Other(string)
}

pub fn fs_error_message(e: FsError) -> string

pub fn read_text(path: string) -> Result<string, FsError>

pub fn read_text_in(path: string, set: charset.Charset) -> Result<string, FsError>

pub fn lines_in(

pub fn lines(path: string) -> Result<Iter<Result<string, io.IoError>>, FsError>

pub fn chunks(path: string, size: int) -> Result<Iter<Result<[byte], io.IoError>>, FsError>

pub fn io_error(e: FsError) -> io.IoError

pub fn walk(dir: string) -> Result<Iter<Result<string, string>>, FsError>

pub fn write_text(path: string, text: string) -> Result<int, FsError>

pub fn append_text(path: string, text: string) -> Result<int, FsError>

pub fn write_text_atomic(path: string, text: string) -> Result<int, FsError>

pub fn read_bytes(path: string) -> Result<[byte], FsError>

pub fn write_bytes(path: string, data: [byte]) -> Result<int, FsError>

pub fn append_bytes(path: string, data: [byte]) -> Result<int, FsError>

pub native fn exists(path: string) -> bool

pub native fn is_dir(path: string) -> bool

pub native fn is_file(path: string) -> bool

pub fn size(path: string) -> Result<int, FsError>

pub fn modified_seconds(path: string) -> Result<int, FsError>

pub fn list_dir(path: string) -> Result<[string], FsError>

pub fn resolve(name: string) -> Option<string>

pub fn remove(path: string) -> Result<unit, FsError>

pub fn remove_all(path: string) -> Result<unit, FsError>

pub fn make_dir(path: string) -> Result<unit, FsError>

pub fn make_dirs(path: string) -> Result<unit, FsError>

pub fn rename(from: string, to: string) -> Result<unit, FsError>

pub fn copy_file(from: string, to: string) -> Result<int, FsError>

pub fn temp_dir() -> string

pub fn path_join(pieces: variadic string) -> string

pub fn file_name(path: string) -> string

pub fn parent_dir(path: string) -> string

pub fn extension(path: string) -> string

```

## `geom`

Points, rectangles and affine transforms.

```
pub struct Point {
    x: float
    y: float
}

pub fn point_of(x: float, y: float) -> Point

pub struct Rect {
    x: float
    y: float
    width: float
    height: float
}

pub fn rect_of(x: float, y: float, width: float, height: float) -> Rect

pub fn rect_between(a: Point, b: Point) -> Rect

pub fn normalized(r: Rect) -> Rect

pub fn right(r: Rect) -> float

pub fn bottom(r: Rect) -> float

pub fn is_empty(r: Rect) -> bool

pub fn rect_contains(r: Rect, p: Point) -> bool

pub fn rect_union(a: Rect, b: Rect) -> Rect

pub fn empty_rect() -> Rect

pub struct Matrix {
    a: float
    b: float
    c: float
    d: float
    e: float
    f: float
}

pub fn identity() -> Matrix

pub fn translation(dx: float, dy: float) -> Matrix

pub fn scaling(sx: float, sy: float) -> Matrix

pub fn rotation(radians: float) -> Matrix

pub fn shearing(sx: float, sy: float) -> Matrix

pub fn matrix_concat(outer: Matrix, inner: Matrix) -> Matrix

pub fn transform_point(m: Matrix, p: Point) -> Point

pub fn transform_rect(m: Matrix, r: Rect) -> Rect

pub fn determinant(m: Matrix) -> float

pub fn invert(m: Matrix) -> Result<Matrix, string>

pub fn rotation_of(m: Matrix) -> float

pub fn scale_x_of(m: Matrix) -> float

pub fn scale_y_of(m: Matrix) -> float

pub fn shear_of(m: Matrix) -> float

```

## `http`

What an HTTP server and an HTTP client have in common: the request and response types, the method enum, status codes by name, header lookup and percent-encoding.

```
pub native fn is_token(s: string) -> bool

pub native fn is_field_value(s: string) -> bool

pub val status_continue = 100

pub val status_ok = 200

pub val status_created = 201

pub val status_accepted = 202

pub val status_no_content = 204

pub val status_moved_permanently = 301

pub val status_found = 302

pub val status_not_modified = 304

pub val status_bad_request = 400

pub val status_unauthorized = 401

pub val status_forbidden = 403

pub val status_not_found = 404

pub val status_method_not_allowed = 405

pub val status_not_acceptable = 406

pub val status_request_timeout = 408

pub val status_conflict = 409

pub val status_payload_too_large = 413

pub val status_unsupported_media_type = 415

pub val status_unprocessable_entity = 422

pub val status_too_many_requests = 429

pub val status_header_fields_too_large = 431

pub val status_internal_server_error = 500

pub val status_not_implemented = 501

pub val status_service_unavailable = 503

pub enum HttpMethod {
    Get
    Head
    Post
    Put
    Patch
    Delete
    Options
    Other(string)
}

pub fn method_parse(name: string) -> HttpMethod

pub fn method_name(m: HttpMethod) -> string

pub struct Header {
    name: string
    value: string
}

pub struct Spill {
    path: string
    offset: int
    size: int
}

pub enum Body {
    Bytes([byte])
    Spilled(Spill)
}

pub struct Request {
    method: HttpMethod
    target: string
    path: string
    query: string
    version: string
    headers: [Header]
    // Where the bytes are: in memory, or in a file the server spilled them to (`Body`). Not
    // `[byte]` since 2026-11-03.
    body: Body
    // The address at the other end of the connection this request arrived on, as text ("192.0.2.7" or
    // "2001:db8::1"), with no port. `None` on a Request that did not come off a socket -- one built by
    // hand in a test, or by `write_error`'s stub -- which is the honest answer rather than a placeholder.
    //
    // **The transport peer, and only that.** Not `X-Forwarded-For`: that header is written by whatever
    // is in front, and a client that is not behind a proxy can put anything in it, so a server reading
    // it by default would let every caller choose its own identity -- backwards for the thing an
    // address is wanted for, which is telling one client from another in order to rate-limit (B10). An
    // application behind a proxy it trusts reads the header itself, knowing how many hops it owns;
    // `server.forwarded_for` is there to help once that decision has been made explicitly.
    //
    // A field rather than a synthetic header, deliberately: a header could be spoofed by a client
    // sending that exact name unless the parser stripped it, and relying on that is the same subtlety
    // this field exists to avoid. Where the value comes from should be visible in the type.
    peer: Option<string>
}

pub struct Response {
    status: int
    headers: [Header]
    body: [byte]
}

pub fn request(method: HttpMethod, target: string, headers: [Header], body: [byte]) -> Request

pub fn response_text(r: Response) -> Result<string, string>

pub fn body_size(body: Body) -> int

pub fn body_bytes(body: Body) -> Result<[byte], io.IoError>

pub fn body_source(body: Body) -> Result<Iter<Result<[byte], io.IoError>>, io.IoError>

pub fn request_text(req: Request) -> Result<string, string>

pub fn header(req: Request, name: string) -> Option<string>

pub fn bearer_token(req: Request) -> Option<string>

pub struct TraceParent {
    // 32 lowercase hex characters, the same for every hop of one request.
    trace_id: string
    // 16 lowercase hex characters, this hop's own. A callee makes a child rather than reusing it.
    span_id: string
    // The one flag anything acts on: whether the trace was sampled for recording.
    sampled: bool
}

pub val traceparent_header_name = "traceparent"

pub fn traceparent_parse(value: string) -> Option<TraceParent>

pub fn traceparent_new() -> TraceParent

pub fn traceparent_child(p: TraceParent) -> TraceParent

pub fn traceparent_value(p: TraceParent) -> string

pub fn traceparent_of(req: Request) -> TraceParent

pub fn traceparent_header_for(p: TraceParent) -> Header

pub fn header_value(headers: [Header], name: string) -> Option<string>

pub struct CookiePair {
    name: string
    value: string
}

pub fn request_cookies(req: Request) -> [CookiePair]

pub fn request_cookie(req: Request, name: string) -> Option<string>

pub struct SetCookie {
    name: string
    value: string
    path: string
    domain: string
    max_age_seconds: int
    secure: bool
    http_only: bool
    same_site: string
}

pub fn session_cookie(name: string, value: string) -> SetCookie

pub fn set_cookie_header(c: SetCookie) -> Header

pub fn delete_cookie_header(name: string, path: string, domain: string) -> Header

pub fn content_type(headers: [Header]) -> Option<mime.MediaType>

pub fn quality(headers: [Header], media: mime.MediaType) -> float

pub fn accepts(headers: [Header], media: mime.MediaType) -> bool

pub fn best_media(headers: [Header], candidates: [mime.MediaType]) -> Option<mime.MediaType>

pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>

pub fn percent_encode(s: string, plus_for_space: bool) -> string

```

## `ibus`

Composing text through an ibus input method, so that Japanese, Chinese and Korean reach `edit`'s pre-edit model.

```
pub struct Composition {
    // The pre-edit. Not in the document until a commit.
    text: string
    // Where the caret sits **inside `text`**, in characters, which the method moves as a
    // conversion changes.
    caret: int
    // The run being converted, as character indices into `text`. Equal when nothing is marked,
    // which is the ordinary case: a dead key, a compose sequence, and every composition before its
    // first conversion all mark nothing.
    marked_start: int
    marked_end: int
}

pub enum Change {
    Composing(Composition)
    // The text to insert, which is **the method's and not the pre-edit's**: a conversion commits
    // what was chosen, and that is allowed to be neither what was typed nor what was last shown.
    Committed(string)
    Closed
}

pub fn open(client: string) -> Result<unit, string>

pub fn close()

pub fn focused(on: bool)

pub fn caret_at(x: int, y: int, width: int, height: int)

pub fn reset()

pub fn press(k: window.KeyPress) -> bool

pub fn release(k: window.KeyPress) -> bool

pub fn changes() -> [Change]

pub fn decode(kinds: [int], texts: [string], numbers: [int]) -> [Change]

```

## `icons`

The desktop's own icons, by name.

```
pub enum Kind {
    // One size only: `16x16/actions` holds 16-pixel icons and nothing else.
    Fixed
    // A range, which is what a `scalable/` directory of SVGs is.
    Scalable
    // A nominal size that stretches by `Threshold` either way. **This is the default**, which
    // matters: a directory section with a `Size` and nothing else is a threshold directory with a
    // threshold of 2, not a fixed one.
    Threshold
}

pub struct SizeDir {
    // Relative to the theme's own directory: `16x16/actions`, `scalable/devices`.
    path: string
    size: int
    scale: int
    kind: Kind
    min: int
    max: int
    threshold: int
}

pub struct Indexed {
    name: string
    roots: [string]
    dirs: [SizeDir]
}

pub struct Theme {
    asked: string
    // The search path this was resolved with, so every later lookup walks the same directories the
    // chain came from. **Carried rather than re-read**: `search_path()` is a function of the
    // environment, and a theme that answered one way at startup must not answer another way later
    // because something changed `XDG_DATA_DIRS` -- which is also what lets a caller (or a test)
    // point this at a directory of its own.
    path: [string]
    chain: [Indexed]
}

pub struct Found {
    path: string
    // The nominal size of the directory it came from, which is not the size asked for: a 16-pixel
    // request can land in a 22-pixel directory when that is the closest a theme has.
    size: int
    scalable: bool
}

pub fn search_path() -> [string]

pub fn theme(name: string) -> Result<Theme, string>

pub fn theme(name: string, path: [string]) -> Result<Theme, string>

pub fn find(t: Theme, name: string, size: int) -> Option<Found>

pub fn find(t: Theme, name: string, size: int, scale: int) -> Option<Found>

pub fn find_svg(t: Theme, name: string, size: int) -> Option<Found>

pub fn find_svg(t: Theme, name: string, size: int, scale: int) -> Option<Found>

pub fn icon_path(t: Theme, name: string, size: int) -> Result<shape.Path, string>

pub fn icon_path(t: Theme, name: string, size: int, least: float) -> Result<shape.Path, string>

```

## `image`

A pixel buffer, PNG in and out, JPEG in.

```
pub enum Pixels {
    Gray8
    Rgb8
    Rgba8
}

pub fn channels_of(f: Pixels) -> int

pub struct Image {
    width: int
    height: int
    format: Pixels
    data: [byte]
}

pub fn image_of(width: int, height: int, format: Pixels, data: [byte]) -> Result<Image, string>

pub fn pixel_at(i: Image, x: int, y: int) -> Result<Color, string>

pub struct Canvas {
    width: int
    height: int
    format: Pixels
    data: MutableArray<byte>
}

pub fn canvas(width: int, height: int, format: Pixels) -> Result<Canvas, string>

pub fn set(c: Canvas, x: int, y: int, colour: Color) -> Result<unit, string>

pub fn snapshot_image(c: Canvas) -> Image

pub fn alpha_channel(i: Image) -> Option<Image>

pub fn without_alpha(i: Image) -> Image

pub fn encode_png(i: Image) -> [byte]

pub fn decode_png(data: [byte], max_pixels: int) -> Result<Image, string>

pub fn crc32(data: [byte]) -> int

pub fn jpeg_dimensions(data: [byte]) -> Result<[int], string>

pub fn decode_jpeg(data: [byte], max_pixels: int) -> Result<Image, string>

```

## `ini`

The freedesktop desktop-entry format: groups of `Key=Value`, with localised keys.

```
pub struct Entry {
    key: string
    locale: Option<string>
    value: string
}

pub struct Group {
    name: string
    entries: [Entry]
}

pub struct Ini {
    groups: [Group]
}

pub fn default_charset() -> charset.Charset

pub fn parse(text: string) -> Result<Ini, string>

pub fn parse(data: [byte], set: charset.Charset) -> Result<Ini, string>

pub fn parse(data: [byte]) -> Result<Ini, string>

pub fn group(f: Ini, name: string) -> Option<Group>

pub fn value(g: Group, key: string) -> Option<string>

pub fn value_for(g: Group, key: string, l: Locale) -> Option<string>

pub fn value_for(g: Group, key: string, tag: string) -> Option<string>

pub fn list_of(text: string) -> [string]

pub fn list_of(text: string, separator: string) -> [string]

pub fn list_in(g: Group, key: string) -> [string]

pub fn list_in(g: Group, key: string, separator: string) -> [string]

pub fn get_bool(g: Group, key: string) -> Result<bool, string>

pub fn get_int(g: Group, key: string) -> Result<int, string>

pub fn get_float(g: Group, key: string) -> Result<float, string>

pub fn get_string(g: Group, key: string) -> Result<string, string>

pub fn unescaped(text: string) -> string

pub fn escaped(text: string) -> string

pub fn format(f: Ini) -> string

pub fn list_text(items: [string]) -> string

pub fn list_text(items: [string], separator: string) -> string

pub fn entry_of(key: string, value: string) -> Entry

pub fn entry_in(key: string, l: string, value: string) -> Entry

pub fn entry_list(key: string, items: [string]) -> Entry

pub fn entry_list(key: string, items: [string], separator: string) -> Entry

pub fn group_of(name: string, entries: [Entry]) -> Group

pub fn ini_of(groups: [Group]) -> Ini

```

## `io`

**Experimental** (see docs/design/journal/ "Experimental modules"). Byte streams: a pull source of `[byte]` chunks, one error for every layer over it, and the layers themselves.

```
pub enum IoError {
    Timeout
    Closed(string)
    TooLarge(string)
    Malformed(string)
    Failed(string)
}

pub fn io_error_message(e: IoError) -> string

pub fn of_bytes(data: [byte]) -> Iter<Result<[byte], IoError>>

pub fn stdin() -> Iter<Result<[byte], IoError>>

pub fn mapped<T, E, F>(

pub fn limited(source: Iter<Result<[byte], IoError>>, max: int) -> Iter<Result<[byte], IoError>>

pub fn taking(source: Iter<Result<[byte], IoError>>, n: int) -> Iter<Result<[byte], IoError>>

pub fn skipping(source: Iter<Result<[byte], IoError>>, n: int) -> Iter<Result<[byte], IoError>>

pub fn drain(source: Iter<Result<[byte], IoError>>, max: int) -> Result<[byte], IoError>

pub fn text(source: Iter<Result<[byte], IoError>>) -> Iter<Result<string, IoError>>

pub fn text_in(

pub fn lines(source: Iter<Result<[byte], IoError>>) -> Iter<Result<string, IoError>>

pub fn lines_in(

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

pub fn cursor(source: Iter<Result<string, IoError>>) -> Cursor

pub fn at(c: Cursor, p: int) -> string

pub fn has(c: Cursor, p: int) -> bool

pub fn starts(c: Cursor, p: int, needle: [string]) -> bool

pub fn span(c: Cursor, a: int, b: int) -> string

pub fn find(c: Cursor, needle: [string], from: int) -> int

pub fn run_until(c: Cursor, stops: [string], from: int) -> int

pub fn release(c: Cursor, upto: int)

pub fn line(c: Cursor, p: int) -> int

pub fn window_size(c: Cursor) -> int

pub fn failure(c: Cursor) -> Option<IoError>

pub fn cursor_close(c: Cursor)

```

## `json`

JSON as a `Json` tree: a native parser and serializer, and Ziman helpers for reading and building trees.

```
pub enum Number {
    Int(int)
    Float(float)
    Bigint(bigint)
    Decimal(decimal)
}

pub enum Json {
    JNull
    JBool(bool)
    JNumber(Number)
    JString(string)
    JArray([Json])
    JObject(Map<string, Json>)
}

pub fn parse(s: string) -> Result<Json, string>

pub fn default_charset() -> charset.Charset

pub fn parse_strict(s: string) -> Result<Json, string>

pub fn parse(data: [byte]) -> Result<Json, string>

pub fn parse(data: [byte], set: charset.Charset) -> Result<Json, string>

pub fn parse_strict(data: [byte]) -> Result<Json, string>

pub fn parse_exact(s: string) -> Result<Json, string>

pub fn parse_exact(data: [byte]) -> Result<Json, string>

pub fn to_bytes(j: Json) -> [byte]

pub fn format(j: Json) -> string

pub fn get(j: Json, key: string) -> Option<Json>

pub fn get_path(j: Json, path: variadic string) -> Option<Json>

pub fn require(j: Json, key: string) -> Result<Json, string>

pub fn require_string(j: Json, key: string) -> Result<string, string>

pub fn require_int(j: Json, key: string) -> Result<int, string>

pub fn require_float(j: Json, key: string) -> Result<float, string>

pub fn require_bigint(j: Json, key: string) -> Result<bigint, string>

pub fn require_decimal(j: Json, key: string) -> Result<decimal, string>

pub fn require_bool(j: Json, key: string) -> Result<bool, string>

pub fn to_map<V>(j: Json, decode: fn(Json) -> Result<V, string>) -> Result<Map<string, V>, string>

pub fn from_map<V>(m: Map<string, V>, encode: fn(V) -> Json) -> Json

pub fn to_string_map(j: Json) -> Result<Map<string, string>, string>

pub fn snake_keys(j: Json) -> Result<Json, string>

pub fn format(j: Json, indent: int) -> string

pub fn json_to_json(j: Json) -> Json

pub fn json_from_json(j: Json) -> Result<Json, string>

pub fn int_to_json(n: int) -> Json

pub fn number_of(j: Json) -> Option<Number>

pub fn int_from_json(j: Json) -> Result<int, string>

pub fn bigint_to_json(n: bigint) -> Json

pub fn bigint_from_json(j: Json) -> Result<bigint, string>

pub fn float_to_json(f: float) -> Json

pub fn float_from_json(j: Json) -> Result<float, string>

pub fn decimal_to_json(d: decimal) -> Json

pub fn decimal_from_json(j: Json) -> Result<decimal, string>

pub fn bool_to_json(b: bool) -> Json

pub fn bool_from_json(j: Json) -> Result<bool, string>

pub fn string_to_json(s: string) -> Json

pub fn string_from_json(j: Json) -> Result<string, string>

```

## `jwt`

JSON Web Tokens (RFC 7519), in two families: **a shared secret** (HS256, HS384, HS512) and **a P-256 key pair** (ES256, added 2026-10-29).

```
pub enum JwtError {
    Malformed(string)
    UnsupportedAlgorithm(string)
    BadSignature
    Expired
    NotYetValid
    MissingClaim(string)
    WrongIssuer
    WrongAudience
}

pub fn jwt_error_message(e: JwtError) -> string

pub fn algorithm_for_key(key: [byte]) -> string

pub fn claims(subject: string, expires_in_seconds: int) -> Json

pub fn with_claim(claims: Json, name: string, value: Json) -> Json

pub fn sign(claims: Json, key: [byte]) -> Result<string, string>

pub fn sign(claims: Json, key: [byte], algorithm: string) -> Result<string, string>

pub fn sign(claims: Json, private_key_pem: string) -> Result<string, string>

pub fn sign(claims: Json, private_key_pem: string, algorithm: string) -> Result<string, string>

pub fn verify_at(token: string, key: [byte], now_seconds: int, leeway_seconds: int) -> Result<Json, JwtError>

pub fn verify(token: string, key: [byte]) -> Result<Json, JwtError>

pub fn verify(token: string, public_key_pem: string) -> Result<Json, JwtError>

pub fn verify_at(

pub fn verify_claims(

pub fn verify_claims(

pub fn subject(claims: Json) -> Option<string>

pub fn issuer(claims: Json) -> Option<string>

pub fn audience(claims: Json) -> [string]

```

## `layout`

Flows, wrapping, tables and pagination: the layer that turns headings, paragraphs and tables into coordinates.

```
pub enum Align {
    Left
    Right
    Centre
    Justify
}

pub struct Span {
    text: string
    font: int
    size: float
    fill: Paint
}

pub fn span_of(text: string, face: int, size: float, fill: Paint) -> Span

pub struct ParaStyle {
    align: Align
    // Baseline to baseline. Zero means "from the fonts on the line", which is the usual case.
    leading: float
    space_before: float
    space_after: float
    indent_first: float
    indent_left: float
    indent_right: float
    // Lines that may not be stranded at the bottom or the top of a page. 2 and 2 is the
    // typographic convention; 0 turns the rule off.
    orphans: int
    widows: int
}

pub fn para_style() -> ParaStyle

pub struct Para {
    spans: [Span]
    style: ParaStyle
}

pub struct RuleLine {
    thickness: float
    fill: Paint
    space_before: float
    space_after: float
}

pub enum ColumnWidth {
    Fixed(float)
    Proportional(float)
    Auto
}

pub struct Column {
    width: ColumnWidth
    align: Align
}

pub fn column(width: ColumnWidth, align: Align) -> Column

pub struct Cell {
    blocks: [Block]
}

pub fn cell(blocks: [Block]) -> Cell

pub struct Row {
    cells: [Cell]
}

pub fn row(cells: [Cell]) -> Row

pub struct TableStyle {
    // A rule under the header. Zero for none.
    header_rule: float
    rule_fill: Paint
    cell_padding: float
    row_gap: float
}

pub struct Grid {
    columns: [Column]
    // Repeated at the top of every page the table continues onto. Separate from `rows` and not a
    // flag on the first one, because repeating it is the single thing that makes a long table
    // readable -- and because a hand-written export has to copy the header-drawing code into its
    // page-break branch to get it.
    header: [Row]
    rows: [Row]
    style: TableStyle
}

pub enum Block {
    Paragraph(Para)
    Table(Grid)
    Spacer(float)
    Rule(RuleLine)
    PageBreak
}

pub fn table_style(padding: float, gap: float, rule: float, rule_fill: Paint) -> TableStyle

pub fn table(columns: [Column], header: [Row], rows: [Row], style: TableStyle) -> Block

pub fn paragraph(spans: [Span], style: ParaStyle) -> Block

pub fn text_block(body: string, face: int, size: float, fill: Paint, style: ParaStyle) -> Block

pub fn spacer(height: float) -> Block

pub fn rule(thickness: float, fill: Paint, before: float, after: float) -> Block

pub struct Insets {
    top: float
    right: float
    bottom: float
    left: float
}

pub fn insets(all: float) -> Insets

pub struct Band {
    height: float
    draw: fn(int, int) -> [Op]
}

pub struct PageTemplate {
    width: float
    height: float
    margins: Insets
    header: Option<Band>
    footer: Option<Band>
}

pub fn page_template(width: float, height: float, margin: float) -> PageTemplate

pub struct Document {
    blocks: [Block]
    fonts: [FontRef]
}

pub fn document(blocks: [Block], fonts: [FontRef]) -> Document

pub fn measure(b: Block, width: float, d: Document) -> Result<float, string>

pub struct Split {
    fits: Option<Block>
    rest: Option<Block>
}

pub fn split_block(b: Block, width: float, room: float, d: Document) -> Result<Split, string>

pub fn draw(b: Block, at: Point, width: float, d: Document) -> Result<[Op], string>

pub fn paginate(d: Document, t: PageTemplate) -> Result<[Scene], string>

```

## `locale`

**Experimental** (see docs/design/journal/ "Experimental modules"). A `Locale`, and the process-wide default that locale-aware functions use when given none.

```
pub fn of(asked: string) -> Locale

pub fn name_of(l: Locale) -> string

pub fn root() -> Locale

pub fn system() -> Locale

pub fn default() -> Locale

pub fn set_default(l: Locale)

pub fn use_system() -> Locale

pub fn month_name(l: Locale, month: int, full: bool) -> string

pub fn day_name(l: Locale, day: int, full: bool) -> string

pub fn first_weekday(l: Locale) -> int

pub fn decimal_separator(l: Locale) -> string

pub fn thousands_separator(l: Locale) -> string

```

## `log`

Levelled logging to stderr or to a file.

```
pub enum Level {
    Debug
    Info
    Warn
    Error
}

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

pub fn logger(

pub fn with_prefix(l: Logger, prefix: string) -> Logger

pub fn with_request_id(l: Logger, id: string) -> Logger

pub fn rank(level: Level) -> int

pub fn format(level: Level) -> string

pub fn parse_level(text: string) -> Result<Level, string>

pub fn enabled(l: Logger, level: Level) -> bool

pub fn write(l: Logger, level: Level, message: string)

pub fn debug(l: Logger, message: string) { write(l, Level.Debug, message) }

pub fn info(l: Logger, message: string) { write(l, Level.Info, message) }

pub fn warn(l: Logger, message: string) { write(l, Level.Warn, message) }

pub fn error(l: Logger, message: string) { write(l, Level.Error, message) }

```

## `markdown`

**Experimental** (see docs/design/journal/ "Experimental modules"). Markdown as a document tree: a reader over a declared subset, a writer, and the escaping that makes generated documents safe.

```
pub enum Align {
    AlignNone
    AlignLeft
    AlignRight
    AlignCenter
}

pub struct Link {
    text: [Inline]
    destination: string
    title: Option<string>
}

pub enum Inline {
    Text(string)
    Emphasis([Inline])
    Strong([Inline])
    Code(string)
    Link(Link)
    Image(Link)
    SoftBreak
    HardBreak
}

pub struct Heading {
    level: int
    content: [Inline]
}

pub struct CodeBlock {
    info: string
    text: string
}

pub struct ListItem {
    blocks: [Block]
}

pub struct List {
    ordered: bool
    start: int
    tight: bool
    items: [ListItem]
}

pub struct Table {
    headers: [[Inline]]
    alignments: [Align]
    rows: [[[Inline]]]
}

pub enum Block {
    Heading(Heading)
    Paragraph([Inline])
    CodeBlock(CodeBlock)
    BlockQuote([Block])
    List(List)
    Table(Table)
    ThematicBreak
}

pub struct Document {
    blocks: [Block]
}

pub fn document(blocks: [Block]) -> Document

pub fn heading(level: int, text: string) -> Block

pub fn paragraph(text: string) -> Block

pub fn code_block(info: string, text: string) -> Block

pub fn link(text: string, destination: string) -> Inline

pub fn image(alt: string, destination: string) -> Inline

pub fn text_table(headers: [string], alignments: [Align], rows: [[string]]) -> Result<Block, string>

pub fn text_list(ordered: bool, items: [string]) -> Block

pub fn format(doc: Document) -> Result<string, string>

pub fn to_bytes(doc: Document) -> Result<[byte], string>

pub fn format(block: Block) -> Result<string, string>

pub fn flatten_cell(text: string) -> string

pub fn parse(text: string) -> Result<Document, string>

pub fn parse_strict(text: string) -> Result<Document, string>

pub fn parse(data: [byte]) -> Result<Document, string>

pub fn text_of(doc: Document) -> string

```

## `math`

Floating-point math beyond the four basic operators, the rounding functions (`floor`/`ceil`/`round` to an `int`, and `floor_float`/`ceil_float`/`round_float` that keep a `float`), and `decimal`'s own rounding and integer power.

```
pub val pi = 3.14159265358979323846

pub val e = 2.71828182845904523536

pub native fn abs(x: int) -> int

pub native fn abs(x: float) -> float

pub native fn abs(x: bigint) -> bigint

pub native fn abs(x: decimal) -> decimal

pub native fn sign(x: int) -> int

pub native fn sign(x: float) -> int

pub native fn sign(x: bigint) -> int

pub native fn sign(x: decimal) -> int

pub native fn min<T: Ord>(values: variadic T) -> T

pub native fn max<T: Ord>(values: variadic T) -> T

pub native fn sqrt(x: float) -> float

pub native fn pow(base: float, exponent: float) -> float

pub native fn exp(x: float) -> float

pub native fn log(x: float) -> float

pub native fn log2(x: float) -> float

pub native fn log10(x: float) -> float

pub native fn sin(x: float) -> float

pub native fn cos(x: float) -> float

pub native fn tan(x: float) -> float

pub native fn asin(x: float) -> float

pub native fn acos(x: float) -> float

pub native fn atan(x: float) -> float

pub native fn atan2(y: float, x: float) -> float

pub native fn floor(x: float) -> int

pub native fn ceil(x: float) -> int

pub native fn round(x: float) -> int

pub native fn floor_float(x: float) -> float

pub native fn ceil_float(x: float) -> float

pub native fn round_float(x: float) -> float

pub native fn floor(d: decimal) -> bigint

pub native fn ceil(d: decimal) -> bigint

pub native fn round(d: decimal) -> bigint

pub native fn round(d: decimal, scale: int) -> decimal

pub native fn floor(d: decimal, scale: int) -> decimal

pub native fn ceil(d: decimal, scale: int) -> decimal

pub native fn pow(base: decimal, exponent: int) -> decimal

pub fn is_nan(x: float) -> bool

pub fn is_infinite(x: float) -> bool

pub fn is_finite(x: float) -> bool

```

## `memdb`

**Experimental** (see docs/design/journal/ "Experimental modules"). A small SQL engine in pure Ziman, holding its tables in memory, for tests.

```
pub fn driver() -> Driver

pub fn strict_driver() -> Driver

pub struct Template {
    database: Database
    strict: bool
}

pub fn template(url: string, setup: [string]) -> Result<Template, DbError>

pub fn seeded(t: Template) -> Driver

```

## `meta`

**Experimental** (see docs/design/journal/ "Experimental modules"). A declaration, as a generator sees it.

```
pub enum Type {
    Int
    Float
    Bool
    String
    Bigint
    Decimal
    Byte
    // `unit` is the type with one value; `void` is a function that declares no return type at
    // all. Two variants because they are two things, even though most generators treat them
    // alike.
    Unit
    Void
    Array(Type)
    // **`SetOf`/`MapOf`/`ResultOf`/`ChannelOf` carry the suffix because `Set`, `Map`, `Result`
    // and `Channel` are builtin type names and the language will not take one as a variant
    // name.** The four that collide are the four that are renamed; `Array`, `Func`, `Optional`
    // and `Named` do not, and are left alone rather than renamed for symmetry.
    SetOf(Type)
    ChannelOf(Type)
    MapOf(MapType)
    ResultOf(ResultType)
    Func(FuncType)
    // `Option<T>`. **A variant of its own, which the compiler has no equivalent of**: it
    // represents an `Option` as a struct in some positions and an enum in others, so the old
    // model's `is_option` had to test name and arity and deliberately ignore kind. `meta` is an
    // interface rather than a mirror and may normalise; every generator asks this question
    // first, and now it is a pattern rather than a guess.
    Optional(Type)
    // A user-declared struct or enum. Which of the two it is, is not here: no generator reads
    // the distinction for a type it merely *refers* to.
    Named(NamedType)
    // `T` in `struct Box<T>` -- a type parameter in the body of the declaration that binds it.
    Var(string)
}

pub struct MapType {
    key: Type
    value: Type
}

pub struct ResultType {
    ok: Type
    err: Type
}

pub struct FuncType {
    params: [Type]
    returns: Type
}

pub struct NamedType {
    name: string
    args: [Type]
}

pub fn int_type() -> Type { return Type.Int }

pub fn float_type() -> Type { return Type.Float }

pub fn bool_type() -> Type { return Type.Bool }

pub fn string_type() -> Type { return Type.String }

pub fn bigint_type() -> Type { return Type.Bigint }

pub fn decimal_type() -> Type { return Type.Decimal }

pub fn byte_type() -> Type { return Type.Byte }

pub fn unit_type() -> Type { return Type.Unit }

pub fn void_type() -> Type { return Type.Void }

pub fn array_type(elem: Type) -> Type { return Type.Array(elem) }

pub fn set_type(elem: Type) -> Type { return Type.SetOf(elem) }

pub fn channel_type(elem: Type) -> Type { return Type.ChannelOf(elem) }

pub fn optional_type(elem: Type) -> Type { return Type.Optional(elem) }

pub fn var_type(name: string) -> Type { return Type.Var(name) }

pub fn map_type(key: Type, value: Type) -> Type

pub fn result_type(ok: Type, err: Type) -> Type

pub fn func_type(params: [Type], returns: Type) -> Type

pub fn no_label() -> Option<string> { return Option<string>.None }

pub fn some_label(value: string) -> Option<string> { return Option<string>.Some(value) }

pub fn no_args() -> [AttrArg]

pub fn decl_arg(d: Decl) -> Option<string>

pub fn decl_arg(d: Decl, name: string) -> Option<string>

pub fn decl_attr_arg(d: Decl, name: string) -> Option<AttrArg>

pub fn named_type(name: string, args: [Type]) -> Type

pub struct AttrArg {
    name: string
    value: string
    // How it was written: `"string"`, `"int"` or `"name"`.
    //
    // **Added after the design said it was unnecessary** (docs/design/validation.md §5f), which
    // claimed the value's text was enough because "the compiler hands text over and the generator
    // decides what it means". True of a generator -- and false of anything that has to *show* an
    // attribute, because the lexer takes the quotes off a string, so `name: "start_date"` and
    // `values: type_words` arrive identical. `attr_text` printed one of them wrongly whichever way
    // it guessed, and the generator that exists to show exactly what it was handed
    // (`derive_reflect`) is the wrong place for a cosmetic lie.
    kind: string
}

pub struct Attr {
    name: string
    args: [AttrArg]
}

pub struct Field {
    name: string
    type: Type
    // The attributes written on this field, in source order.
    //
    // **A list of attributes, not a `pk: bool` and a `column: Option<string>`.** `#[pk]` was the
    // only field attribute when this was `[string]`, and that version said an attribute with an
    // argument "is not modelled yet, and will be when one exists rather than guessed at now".
    // One exists: `#[column(name: "start_date")]` (2026-10-29), so the names became
    // `Attr`s with arguments -- the shape the §13 note deferred, settled by a real reader rather
    // than guessed. `has_attr` reads the same as it always did, so a generator asking only
    // whether `#[pk]` is there did not change.
    attrs: [Attr]
}

pub struct Variant {
    name: string
    payload: Option<Type>
}

pub enum Body {
    Struct([Field])
    Enum([Variant])
}

pub struct Decl {
    // The arguments the derive was written with, in source order.
    //
    // **On the declaration rather than a second parameter to `generate`.** Four generators
    // already take one parameter and exactly one target takes an argument; an argument *about*
    // the declaration belongs on it rather than beside it.
    //
    // **A list of named arguments, not one string** (2026-10-30, user-generators.md §5b). It was
    // `arg: Option<string>`, because `Entity` was the only target that took anything and took one
    // bare table name -- which meant `#[derive(Endpoint(method: POST, path: "/orders"))]` could
    // not be written at all. A target now goes through the same reader a field attribute does, so
    // `#[derive(Entity("medications"))]` is one argument named `value` (`#[min(8)]`'s convention)
    // and `decl_arg(d)` is how a generator asks for it.
    args: [AttrArg]
    // The declaration's own name, as written: `LocalDate`.
    name: string
    // The same name in the spelling generated functions use: `local_date`, so a generator writes
    // `local_date_compare` without repeating the compiler's conversion rule.
    snake_name: string
    // The declaration's own type parameters, bare: `["T", "U"]` for `struct Box<T, U>`, empty
    // for one that is not generic. A bound (`<T: Hash>`) is not here -- no generator reads one.
    type_params: [string]
    body: Body
}

pub fn field(name: string, type: Type, attrs: [Attr]) -> Field

pub fn attr(name: string) -> Attr

pub fn attr_with(name: string, args: [AttrArg]) -> Attr

pub struct AttrSpec {
    name: string
    args: string
    // Which declaration may carry it: `"field"` for a struct field, `"function"` for a function.
    // A string rather than an enum to match the rest of this type -- `args` is itself a little
    // grammar in a string, and `AttrArg.kind` is text for the reason its own comment gives.
    on: string
}

pub fn attr_spec(name: string, args: string) -> AttrSpec

pub fn attr_spec(name: string, args: string, on: string) -> AttrSpec

pub fn attr_arg(name: string, value: string, kind: string) -> AttrArg

pub fn has_attr(f: Field, name: string) -> bool

pub fn attr_value(f: Field, attr: string, arg: string) -> Option<string>

pub fn attr_flag(f: Field, attr: string, arg: string) -> bool

pub fn attr_has_arg(f: Field, attr: string, arg: string) -> bool

pub fn attr_text(a: Attr) -> string

pub fn unit_variant(name: string) -> Variant

pub fn payload_variant(name: string, payload: Type) -> Variant

pub fn struct_decl(

pub fn enum_decl(

pub fn type_text(t: Type) -> string

pub fn bare_name(name: string) -> string

pub fn fresh(n: Mutable<int>, hint: string) -> string

pub fn type_params_bounded(d: Decl, bound: string) -> string

pub fn type_text(d: Decl) -> string

pub struct Param {
    name: string
    type: Type
    // The attributes written on this parameter, in source order -- `#[path]`, `#[query(name: "q")]`.
    //
    // **Added 2026-11-01, and `src/ast.h`'s `Param.attrs` had been waiting for it**: the slot was
    // already on the shared `Param` and its comment said "this list is always empty on a function
    // parameter, which is why it lives on the shared `Param` without meaning anything there". What
    // fills it is a route binding one request value per parameter (`#[path] id: Uuid`) instead of
    // the struct-plus-reader-function an endpoint needed per path or query before.
    attrs: [Attr]
}

pub struct Function {
    name: string
    params: [Param]
    returns: Type
    // Attributes written on it -- `#[route(method: GET, path: "/api/me/things")]` -- exactly as
    // `Field.attrs` carries a field's. The compiler reads none of them; it only checks that some
    // generator this module imports declared the name for this position (`attr_spec`'s `on`).
    attrs: [Attr]
}

pub struct Target {
    // The entity, reified from wherever it is declared: its fields are the columns, and the ones
    // carrying `#[pk]` are what addresses one row.
    entity: Decl
    // `#![derive(Repository(Medication, label: "Medication"))]`'s label -- the entity as a person says
    // it, for a `NotFound` message that reaches an API response. `None` when none was given, and
    // then the entity's own name is the label.
    label: Option<string>
    // `#![derive(Repository(Notification, crud: false))]` makes this false: name the entity, generate
    // none of the nine CRUD functions. A module with only derived queries wants exactly that.
    crud: bool
    // The functions this module already defines, so a generator can decline to replace one.
    // **A hand-written function wins**, which is what makes the attribute safe to add to a
    // repository that has already been written.
    defined: [string]
}

pub fn param(name: string, type: Type) -> Param

pub fn param_with(name: string, type: Type, attrs: [Attr]) -> Param

pub fn param_has_attr(p: Param, name: string) -> bool

pub fn param_attr_value(p: Param, attr: string, arg: string) -> Option<string>

pub fn param_role(p: Param, names: [string]) -> Option<string>

pub fn function(name: string, params: [Param], returns: Type, attrs: [Attr]) -> Function

pub fn fn_has_attr(f: Function, name: string) -> bool

pub fn fn_attr_arg(f: Function, attr: string, arg: string) -> Option<string>

pub struct ModuleTarget {
    // The module's own name, without its folder or `.zm` -- `shared_link` for
    // `controller/shared_link.zm`. **A generator needs it to name what it exports**: a qualified
    // read of another module's val is refused, so thirty modules each exporting a bare `routes`
    // would collide in one import list. Found by writing a generator that had to ask for it as an
    // attribute argument instead.
    name: string
    // `#![derive(Routes(prefix: "/api"))]`'s arguments, as a declaration target's are.
    args: [AttrArg]
    // The module's functions, each with its own `attrs` -- which is where a `#[route(..)]` is read.
    functions: [Function]
    // What the module declares, so a generator skips a name that is already written by hand.
    defined: [string]
}

pub fn module_target(name: string, args: [AttrArg], functions: [Function], defined: [string]) -> ModuleTarget

pub fn module_arg(m: ModuleTarget, name: string) -> Option<string>

pub fn functions_with(m: ModuleTarget, attr: string) -> [Function]

pub fn target(entity: Decl, label: Option<string>, crud: bool, defined: [string]) -> Target

pub fn label_of(t: Target) -> string

pub fn fields_of(t: Target) -> [Field]

pub fn keys_of(t: Target) -> [Field]

pub fn defines(t: Target, name: string) -> bool

```

## `metrics`

Counters, gauges and histograms that the threads of a server share, and a render of them in the Prometheus text format for something to scrape.

```
pub fn counter(name: string, help: string) -> bool

pub fn gauge(name: string, help: string) -> bool

pub fn histogram(name: string, help: string, buckets: [float]) -> bool

pub fn increment(name: string)

pub fn increment(name: string, labels: Map<string, string>)

pub fn increment(s: Series)

pub fn add(name: string, delta: float)

pub fn add(name: string, delta: float, labels: Map<string, string>)

pub fn add(s: Series, delta: float)

pub fn set(name: string, value: float)

pub fn set(name: string, value: float, labels: Map<string, string>)

pub fn set(s: Series, value: float)

pub fn gauge_add(name: string, delta: float)

pub fn gauge_add(name: string, delta: float, labels: Map<string, string>)

pub fn gauge_add(s: Series, delta: float)

pub fn observe(name: string, value: float)

pub fn observe(name: string, value: float, labels: Map<string, string>)

pub fn observe(s: Series, value: float)

pub struct Series {
    handle: int
}

pub fn series(name: string, labels: Map<string, string>) -> Series

pub fn series(name: string) -> Series

pub fn series_of(key_name: string, key: string) -> Series

pub fn render() -> string

pub fn reset()

pub fn record_runtime()

```

## `migrate`

Versioned schema migrations: a directory of plain `.sql` files, applied in order, once each.

```
pub struct Migration {
    version: int
    name: string
    file: string
    sql: string
    checksum: string
}

pub fn checksum(sql: string) -> string

pub fn migration(version: int, name: string, sql: string) -> Migration

pub struct TypeToken {
    token: string
    // What each engine spells this token as, keyed by the dialect `datasource.dialect` answers.
    //
    // **A map rather than a field per engine.** It was `sqlite`/`postgres`/`mysql` as three
    // fields with a three-branch lookup, so a fourth engine meant editing this struct, the table
    // and the lookup -- which is exactly the shape `datasource` lost when a driver became a value.
    // The first thing `memdb` did on meeting a real migration directory was fail here, with
    // "uses {{uuid}} and there is no spelling of it for 'memdb'", which is the right error and
    // was being raised by the wrong design.
    spellings: Map<string, string>
}

pub val type_tokens = [

pub fn substitute(sql: string, dialect: string, file: string) -> Result<string, DbError>

pub fn split_statements(sql: string) -> Result<[string], DbError>

pub fn load(dir: string) -> Result<[Migration], DbError>

pub fn load_for(dir: string, dialect: string) -> Result<[Migration], DbError>

pub fn apply(conn: Connection, migrations: [Migration]) -> Result<int, DbError>

pub fn run(conn: Connection, dir: string) -> Result<int, DbError>

```

## `mime`

Media types (MIME types, RFC 6838): `application/json`, `text/html; charset=utf-8`.

```
pub struct MediaType {
    type: string
    subtype: string
    params: Map<string, string>
}

pub fn application_json() -> MediaType

pub fn application_ndjson() -> MediaType

pub fn application_yaml() -> MediaType

pub fn application_toml() -> MediaType

pub fn application_xml() -> MediaType

pub fn application_form() -> MediaType

pub fn application_octet_stream() -> MediaType

pub fn application_pdf() -> MediaType

pub fn application_zip() -> MediaType

pub fn application_gzip() -> MediaType

pub fn text_plain() -> MediaType

pub fn text_html() -> MediaType

pub fn text_csv() -> MediaType

pub fn image_png() -> MediaType

pub fn image_jpeg() -> MediaType

pub fn image_gif() -> MediaType

pub fn image_svg() -> MediaType

pub fn parse(text: string) -> Result<MediaType, string>

pub fn essence(m: MediaType) -> string

pub fn format(m: MediaType) -> string

pub fn param(m: MediaType, name: string) -> Option<string>

pub fn charset(m: MediaType) -> Option<string>

pub fn with_param(m: MediaType, name: string, value: string) -> MediaType

pub fn suffix(m: MediaType) -> Option<string>

pub fn is_json(m: MediaType) -> bool

pub fn is_text(m: MediaType) -> bool

pub fn matches(pattern: MediaType, candidate: MediaType) -> bool

pub fn of_extension(extension: string) -> Option<MediaType>

pub fn of_path(path: string) -> Option<MediaType>

```

## `mysql`

**Experimental** (see docs/design/journal/ "Experimental modules"). A MySQL / MariaDB client, the third engine behind the `datasource` module, which reaches it through the `db.Driver` that `driver()` below answers: `datasource.open(datasource.registry([mysql.driver()]), "mysql://user:...@host/db")`.

```
pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}

pub fn connect(url: string) -> Result<Connection, DbError>

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn close(conn: Connection)

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn driver() -> Driver

```

## `navigate`

Where an application is, and what changing that owes the user.

```
pub struct History {
    // Oldest first. Never empty once built.
    entries: [string]
    at: int
}

pub fn history_of(first: string) -> History

pub fn here(h: History) -> string

pub fn can_go_back(h: History) -> bool

pub fn can_go_forward(h: History) -> bool

pub fn back(h: History) -> History

pub fn forward(h: History) -> History

pub fn went_to(h: History, screen: string) -> History

pub fn screen_widget(

pub fn screen_region(name: string) -> string

pub fn arrived_at(laid: widget.Laid, screen: string) -> string

```

## `ndjson`

Newline-delimited JSON: one JSON value per line, written on top of `json`.

```
pub fn default_charset() -> charset.Charset

pub fn parse(data: [byte], set: charset.Charset) -> Result<[Json], string>

pub fn parse(data: [byte]) -> Result<[Json], string>

pub fn parse(text: string) -> Result<[Json], string>

pub fn records(path: string) -> Result<Iter<Result<Json, string>>, string>

pub fn format(items: [Json]) -> string

```

## `net`

Blocking TCP: connect, listen/accept, read, write, close.

```
pub enum NetError {
    ConnectionRefused(string)
    HostNotFound(string)
    Timeout(string)
    Closed(string)
    Io(string)
    Tls(string)
}

pub enum TlsVerify {
    Off
    Chain
    Full
}

pub struct Socket {
    handle: int
}

pub struct Listener {
    handle: int
}

pub fn net_error_message(e: NetError) -> string

pub fn connect(host: string, port: int) -> Result<Socket, NetError>

pub fn connect_timeout(host: string, port: int, timeout_ms: int) -> Result<Socket, NetError>

pub fn connect_unix(path: string) -> Result<Socket, NetError>

pub fn listen_unix(path: string) -> Result<Listener, NetError>

pub fn listen(host: string, port: int) -> Result<Listener, NetError>

pub fn listen_tls(host: string, port: int, cert_file: string, key_file: string) -> Result<Listener, NetError>

pub struct TlsServerOptions {
    cert_file: string
    key_file: string
    client_ca_file: Option<string>
    require_client_cert: bool
    alpn: [string]
}

pub fn tls_server_options(cert_file: string, key_file: string) -> TlsServerOptions

pub fn listen_tls(host: string, port: int, options: TlsServerOptions) -> Result<Listener, NetError>

pub fn listener_port(l: Listener) -> int

pub fn peer_address(s: Socket) -> Option<string>

pub fn wait_readable(sockets: [Socket], timeout_ms: int) -> [int]

pub fn accept(l: Listener) -> Result<Socket, NetError>

pub fn read(s: Socket, max: int) -> Result<[byte], NetError>

pub fn read_exact(s: Socket, count: int) -> Result<[byte], NetError>

pub fn write(s: Socket, data: [byte]) -> Result<int, NetError>

pub fn read_text(s: Socket, max: int) -> Result<string, NetError>

pub fn write_text(s: Socket, text: string) -> Result<int, NetError>

pub fn lines(s: Socket) -> Iter<Result<string, NetError>>

pub fn lines(s: Socket, max_line_bytes: int) -> Iter<Result<string, NetError>>

pub fn set_timeout(s: Socket, millis: int) -> Result<unit, NetError>

pub fn start_tls(s: Socket, host: string) -> Result<unit, NetError>

pub fn start_tls(s: Socket, host: string, verify: TlsVerify, ca_file: Option<string>) -> Result<unit, NetError>

pub struct TlsClientOptions {
    verify: TlsVerify
    ca_file: Option<string>
    cert_file: Option<string>
    key_file: Option<string>
    alpn: [string]
}

pub fn tls_client_options() -> TlsClientOptions

pub fn start_tls(s: Socket, host: string, options: TlsClientOptions) -> Result<unit, NetError>

pub fn connect_tls(host: string, port: int) -> Result<Socket, NetError>

pub fn connect_tls(

pub fn connect_tls(host: string, port: int, options: TlsClientOptions) -> Result<Socket, NetError>

pub fn negotiated_protocol(s: Socket) -> Option<string>

pub fn peer_certificate_subject(s: Socket) -> Option<string>

pub fn close(s: Socket)

pub fn close(l: Listener)

```

## `number`

Numbers as text and as bytes: formatting and parsing with separators, hex, shortest-text floats, and the fixed-width big-endian integers binary protocols use.

```
pub val byte_min = 0b

pub val byte_max = 255b

pub val int_min = -9223372036854775808

pub val int_max = 9223372036854775807

pub val float_min = -1.7976931348623157e+308

pub val float_max = 1.7976931348623157e+308

pub val float_min_positive = 5e-324

pub native fn float_to_text(x: float) -> string

pub native fn to_hex(b: byte) -> string

pub native fn byte_from_hex(s: string) -> Result<byte, string>

pub native fn to_hex(x: int) -> string

pub native fn int_from_hex(s: string) -> Result<int, string>

pub native fn to_hex(data: [byte]) -> string

pub native fn bytes_from_hex(s: string) -> Result<[byte], string>

pub fn to_hex(n: bigint) -> string

pub fn bigint_from_hex(s: string) -> Result<bigint, string>

pub fn parse_byte(s: string) -> Result<byte, string>

pub struct NumberFormat {
    thousands_separator: string
    decimal_separator: string
    // **The fraction's shape, as `0` and `#` after the point state it.** `min_decimal_places` is
    // how many places are always written (the `0`s); `max_decimal_places` is the most allowed (the
    // `0`s plus the `#`s), and `None` means the value keeps its own. `.00` is min 2 / max 2 -- a
    // fixed two places; `.##` is min 0 / max 2 -- up to two, trailing zeros dropped; `.0#` is
    // min 1 / max 2. A pattern with no point at all is min 0 / max None.
    //
    // Until 2026-10-24 this was one `decimal_places: Option<int>`, which could only say "exactly
    // N", so `#,##0.00` and `#,###.##` formatted identically -- the one piece of real
    // `DecimalFormat` semantics that was missing rather than merely absent.
    min_decimal_places: int
    max_decimal_places: Option<int>
    // The integer side's minimum width, zero-padded: the count of `0`s in the integer part, so
    // `00000` writes 42 as `00042`. It only ever pads -- a value whose integer part is `0` keeps
    // that digit whatever this says, since a number has to have one.
    min_integer_digits: int
}

pub fn number_format_for(l: Locale) -> NumberFormat

pub fn number_format_plain() -> NumberFormat

pub fn parse_pattern(pattern: string) -> Result<NumberFormat, string>

pub fn parse_pattern(pattern: string, l: Locale) -> Result<NumberFormat, string>

pub fn format(s: string, fmt: NumberFormat) -> string

pub fn parse(s: string, fmt: NumberFormat) -> string

pub fn format(x: int, fmt: NumberFormat) -> string

pub fn format(x: float, fmt: NumberFormat) -> string

pub fn format(x: bigint, fmt: NumberFormat) -> string

pub fn format(x: decimal, fmt: NumberFormat) -> string

pub fn format(x: byte, fmt: NumberFormat) -> string

pub native fn u16_to_bytes(n: int) -> [byte]

pub native fn u32_to_bytes(n: int) -> [byte]

pub native fn u16_from_bytes(b: [byte], offset: int) -> int

pub native fn u32_from_bytes(b: [byte], offset: int) -> int

pub native fn i32_from_bytes(b: [byte], offset: int) -> int

pub native fn i64_from_bytes(b: [byte], offset: int) -> int

pub native fn f64_from_bytes(b: [byte], offset: int) -> float

pub native fn f32_from_bytes(b: [byte], offset: int) -> float

pub native fn f64_to_bytes(f: float) -> [byte]

```

## `os`

The process and its environment: environment variables, command-line arguments, the working directory, ending or shutting down the program, and what can be asked of the machine it runs on (CPUs, memory, disks, load, uptime).

```
pub fn get_env(name: string) -> Option<string>

pub native fn set_env(name: string, value: string) -> bool

pub native fn args() -> [string]

pub fn cwd() -> Result<string, string>

pub native fn pid() -> int

pub native fn collect_garbage()

pub native fn collector_enabled() -> bool

pub native fn open_descriptors() -> int

pub native fn exit(code: int)

pub fn fatal(message: string)

pub native fn write_stdout(text: string)

pub native fn write_stderr(text: string)

pub native fn install_shutdown_handler()

pub native fn shutdown_requested() -> bool

pub native fn cpu_count() -> int

pub native fn hostname() -> string

pub native fn name() -> string

pub native fn arch() -> string

pub native fn load_average() -> Result<[float], string>

pub native fn uptime_seconds() -> Result<int, string>

pub native fn memory_total() -> Result<int, string>

pub native fn memory_available() -> Result<int, string>

pub native fn process_memory() -> Result<int, string>

pub native fn disk_total(path: string) -> Result<int, string>

pub native fn disk_free(path: string) -> Result<int, string>

pub native fn disk_used(path: string) -> Result<int, string>

pub struct CpuTimes {
    busy: int
    total: int
}

pub fn cpu_times() -> Result<CpuTimes, string>

pub fn cpu_usage(before: CpuTimes, after: CpuTimes) -> float

pub fn cpu_usage(sample_millis: int) -> Result<float, string>

pub native fn process_cpu_seconds() -> Result<float, string>

pub native fn process_uptime_seconds() -> Result<int, string>

pub native fn thread_count() -> int

```

## `otp`

Time-based one-time passwords (RFC 6238, over RFC 4226's HOTP) of the kind authenticator apps show: six digits that change every 30 seconds, from a secret shared once, by QR code or by typing it.

```
pub fn generate_secret() -> string

pub fn base32_encode(data: [byte]) -> string

pub fn base32_decode(text: string) -> Result<[byte], string>

pub fn hotp(key: [byte], counter: int, digits: int) -> string

pub fn totp(key: [byte], unix_seconds: int) -> string

pub fn totp(key: [byte], unix_seconds: int, digits: int) -> string

pub fn totp_matching_step(key: [byte], code: string, unix_seconds: int, window: int) -> Option<int>

pub fn totp_matching_step(

pub fn totp_valid(key: [byte], code: string, unix_seconds: int, window: int) -> bool

pub fn totp_valid(key: [byte], code: string, unix_seconds: int, window: int, digits: int) -> bool

pub fn provisioning_uri(issuer: string, account: string, secret: string) -> string

```

## `paging`

One page of a long list: the request that asks for a page, the page that comes back, and the either/or a list endpoint answers -- all of it, or the page that was asked for.

```
pub val default_page_size = 20

pub val max_page_size = 200

pub val max_page_number = 1000000000

pub struct PageRequest {
    page: int
    size: int
}

pub struct Page<T> {
    content: [T]
    page: int
    size: int
    total_elements: int
    total_pages: int
}

pub enum Listing<T> {
    All([T])
    Paged(Page<T>)
}

pub fn page_request_within(

pub fn page_request_of(page: Option<int>, size: Option<int>) -> Result<Option<PageRequest>, string>

pub fn page_of<T>(content: [T], request: PageRequest, total: int) -> Page<T>

pub fn listing_to_json<T>(listing: Listing<T>, item: fn(T) -> Json) -> Json

pub fn listing_encoder<O>(to_json: fn(O) -> Json) -> codec.Encoder<Listing<O>>

```

## `paint`

What fills a region, and how a line is drawn.

```
pub struct Color {
    red: float
    green: float
    blue: float
    alpha: float
}

pub fn rgb(r: float, g: float, b: float) -> Color

pub fn rgba(r: float, g: float, b: float, a: float) -> Color

pub fn gray(level: float) -> Color

pub fn black() -> Color

pub fn white() -> Color

pub fn transparent() -> Color

pub fn hex(text: string) -> Result<Color, string>

pub fn rgb8(r: int, g: int, b: int) -> Color

pub fn rgba8(r: int, g: int, b: int, a: int) -> Color

pub fn relative_luminance(c: Color) -> float

pub fn contrast_ratio(a: Color, b: Color) -> float

pub struct Oklab {
    lightness: float
    a: float
    b: float
    alpha: float
}

pub struct Oklch {
    lightness: float
    chroma: float
    hue: float
    alpha: float
}

pub fn to_oklab(c: Color) -> Oklab

pub fn oklab(lightness: float, a: float, b: float) -> Color

pub fn oklab(lightness: float, a: float, b: float, alpha: float) -> Color

pub fn to_oklch(c: Color) -> Oklch

pub fn oklch(lightness: float, chroma: float, hue: float) -> Color

pub fn oklch(lightness: float, chroma: float, hue: float, alpha: float) -> Color

pub fn lighten(c: Color, amount: float) -> Color

pub fn darken(c: Color, amount: float) -> Color

pub fn saturate(c: Color, amount: float) -> Color

pub fn desaturate(c: Color, amount: float) -> Color

pub fn change_lightness(c: Color, lightness: float) -> Color

pub fn change_chroma(c: Color, chroma: float) -> Color

pub fn change_hue(c: Color, hue: float) -> Color

pub fn fade(c: Color, alpha: float) -> Color

pub fn mix(a: Color, b: Color, at: float) -> Color

pub fn tint(c: Color, at: float) -> Color

pub fn shade(c: Color, at: float) -> Color

pub fn contrast(c: Color, dark: Color, light: Color) -> Color

pub fn named(name: string) -> Option<Color>

pub fn parse(text: string) -> Result<Color, string>

pub struct Stop {
    offset: float
    color: Color
}

pub fn stop_of(offset: float, color: Color) -> Stop

pub struct Linear {
    from: Point
    to: Point
    stops: [Stop]
}

pub struct Radial {
    centre: Point
    radius: float
    stops: [Stop]
}

pub struct Tiled {
    image: int
    transform: Matrix
}

pub enum Paint {
    Solid(Color)
    LinearGradient(Linear)
    RadialGradient(Radial)
    Pattern(Tiled)
}

pub fn solid(c: Color) -> Paint

pub fn flat_of(p: Paint) -> Color

pub fn linear(from: Point, to: Point, stops: [Stop]) -> Paint

pub fn radial(centre: Point, radius: float, stops: [Stop]) -> Paint

pub fn pattern(image: int, transform: Matrix) -> Paint

pub enum Cap {
    Butt
    Round
    Square
}

pub enum Join {
    Miter
    Round
    Bevel
}

pub struct Stroke {
    width: float
    cap: Cap
    join: Join
    // Past this ratio a miter join becomes a bevel. Without the fallback a near-180 degree join
    // produces a spike thousands of units long, which is the classic stroking bug.
    miter_limit: float
    // Empty for a solid line. Otherwise alternating on and off lengths.
    dash: [float]
    dash_phase: float
}

pub fn stroke_of(width: float) -> Stroke

pub fn dashed(width: float, dash: [float]) -> Stroke

```

## `pdf`

A `scene.Scene` rendered to a PDF document.

```
pub struct PageSize {
    width: float
    height: float
}

pub fn a4() -> PageSize

pub fn letter() -> PageSize

pub fn page_size_of(width: float, height: float) -> PageSize

pub struct Options {
    size: PageSize
    // The faces a scene's `TextRun.font` indexes into -- standard or embedded, mixed freely.
    fonts: [FontRef]
    // The images a scene's `DrawImage` indexes into. Images are document-level for the same reason
    // fonts are: one XObject however many pages place it.
    images: [image.Image]
    title: string
    author: string
    compress: bool
}

pub fn options() -> Options

pub fn render(s: Scene, o: Options) -> Result<[byte], string>

pub fn render_pages(pages: [Scene], o: Options) -> Result<[byte], string>

```

## `pdfobj`

PDF's object layer: objects, dictionaries, streams, the cross-reference table.

```
pub struct Entry {
    key: string
    value: Obj
}

pub fn entry(key: string, value: Obj) -> Entry

pub struct StreamObj {
    dict: [Entry]
    data: [byte]
}

pub enum Obj {
    Null
    Bool(bool)
    Int(int)
    Real(float)
    // Written `/Name`.
    Name(string)
    // Written `(escaped)`.
    Str(string)
    // Written `<hex>` -- for a byte string, and for Identity-H text in phase 3, where two bytes per
    // glyph is not representable as a literal.
    Bytes([byte])
    Array([Obj])
    Dict([Entry])
    Stream(StreamObj)
    // `N 0 R`, an indirect reference.
    Ref(int)
}

pub fn stream(dict: [Entry], data: [byte]) -> Obj

pub fn flate_stream(dict: [Entry], data: [byte]) -> Obj

pub struct PdfWriter {
    bodies: MutableArray<[byte]>
    filled: MutableArray<bool>
}

pub fn writer() -> PdfWriter

pub fn add(w: PdfWriter, o: Obj) -> int

pub fn reserve(w: PdfWriter) -> int

pub fn put(w: PdfWriter, number: int, o: Obj) -> Result<unit, string>

pub fn finish(w: PdfWriter, root: int, info: [Entry]) -> Result<[byte], string>

pub fn real_text(x: float) -> string

pub fn hex_of(data: [byte]) -> string

pub fn check(data: [byte]) -> Result<unit, string>

```

## `pg`

**Experimental** (see docs/design/journal/ "Experimental modules"). A PostgreSQL client, the second backend behind the `datasource` module, which reaches it through the `db.Driver` that `driver()` below answers: `datasource.open(datasource.registry([pg.driver()]), "postgres://user:password@host:port/database")`.

```
pub native fn float_to_text(f: float) -> string

pub struct Connection {
    requests: Channel<Request>
    guard: Guard
}

pub fn connect(url: string) -> Result<Connection, DbError>

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn close(conn: Connection)

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn driver() -> Driver

```

## `process`

Starting another program, waiting for it, signalling it.

```
pub struct Process {
    handle: int
}

pub struct EnvVar {
    name: string
    value: string
}

pub fn env(name: string, value: string) -> EnvVar

pub fn start(

pub fn wait(p: Process) -> Result<int, string>

pub fn terminate(p: Process) -> Result<unit, string>

pub fn kill(p: Process) -> Result<unit, string>

pub fn interrupt(p: Process) -> Result<unit, string>

pub fn wait_for(p: Process, millis: int) -> Result<Option<int>, string>

pub fn release(p: Process)

pub fn pid(p: Process) -> int

pub fn run(

pub fn output(

```

## `properties`

Spring-Boot-style configuration: `key=value` files, profiles, `${placeholder}` substitution, and environment-variable overrides, all reporting errors that name the key and where its value came from.

```
pub struct Entry {
    key: string
    value: string
    source: string
}

pub struct Properties {
    entries: [Entry]
    profile: string
}

pub fn lookup(props: Properties, key: string) -> Option<Entry>

pub fn parse(text: string, source: string) -> Result<Properties, string>

pub fn default_charset() -> charset.Charset

pub fn parse(data: [byte], source: string, set: charset.Charset) -> Result<Properties, string>

pub fn parse(data: [byte], source: string) -> Result<Properties, string>

pub fn load(name: string) -> Result<Properties, string>

pub fn get_string(e: Entry) -> Result<string, string>

pub fn get_string(props: Properties, key: string) -> Result<string, string>

pub fn get_int(e: Entry) -> Result<int, string>

pub fn get_int(props: Properties, key: string) -> Result<int, string>

pub fn get_float(e: Entry) -> Result<float, string>

pub fn get_float(props: Properties, key: string) -> Result<float, string>

pub fn get_bool(e: Entry) -> Result<bool, string>

pub fn get_bool(props: Properties, key: string) -> Result<bool, string>

pub fn key(prefix: string, name: string) -> string

pub fn check_unknown(props: Properties, known: variadic string) -> Result<bool, string>

pub fn report(props: Properties, keys: variadic string) -> string

```

## `random`

Randomness from the operating system.

```
pub native fn bytes(n: int) -> [byte]

pub fn below(n: int) -> int

pub fn between(low: int, high: int) -> int

pub fn fraction() -> float

pub fn boolean() -> bool

pub fn choice<T>(items: [T]) -> Option<T>

pub fn shuffle<T>(items: [T]) -> [T]

pub fn text(n: int, alphabet: string) -> string

pub fn alpha(n: int) -> string

pub fn numeric(n: int) -> string

pub fn alpha_numeric(n: int) -> string

```

## `raster`

The second backend: a `Scene` to an `Image`.

```
pub struct Options {
    format: image.Pixels
    // Anti-aliasing: vertical sub-scanlines per pixel row. 1 is none, 4 is good, 16 is as much as
    // this method can usefully give. Horizontal coverage is *exact* either way -- see `add_span` --
    // so this number only quantises the vertical direction.
    samples: int
    // What the canvas starts as. `None` is transparent, which an `Rgb8` or `Gray8` output then
    // flattens to black, because those formats have nowhere to keep the transparency.
    background: Option<Color>
    // The images `DrawImage` and `paint.Pattern` index into, exactly as `pdf.Options` carries them.
    images: [image.Image]
    // The faces a `TextRun` indexes, the same table `pdf.Options` has. **Only embedded faces can be
    // drawn here**: the standard fourteen are names a PDF reader resolves against fonts it already
    // has, and a raster device has no such library to resolve against.
    fonts: [font.FontRef]
    // The same guard `image.decode_png` takes, for the same reason: a scene's size can come from a
    // request, and a 50000x50000 page should be a message rather than 40 GB of allocation.
    max_pixels: int
    // How many device pixels one unit of the scene becomes.
    //
    // **This is what makes the scene's units points rather than pixels**, and it is here rather
    // than on `scene.Scene` deliberately: a `Scene` still does not know what a unit is -- its own
    // comment promises that the backend decides -- and this is the backend deciding. The PDF
    // backend reads the same scene as points at 1/72 inch and wants no such field.
    //
    // 1.0 means a point is a pixel, which is the default and what every existing caller gets.
    // 1.5 means a 100-unit box is drawn 150 pixels wide, with glyph *outlines* scaled before being
    // rasterised rather than a 12-unit bitmap stretched -- so text at 1.5 is 18 real pixels of
    // type, not a blurred 12.
    //
    // A fractional ratio leaves a one-unit line straddling two pixels, which reads grey rather
    // than black. Accepted here, as CSS and Flutter accept it: snapping thin edges to whole pixels
    // is a rendering feature, not a property of the unit.
    px_per_pt: float
}

pub fn options() -> Options

pub struct Surface {
    target: Target
    // How many rasterised glyphs to keep -- `glyph_cache_limit` unless a caller says otherwise.
    //
    // **A field rather than only a constant, so that the bound can be tested.** An untested bound
    // in a long-lived process is the thing `Target`'s comment was worried about, and a fixture
    // cannot reach 8192 distinct glyphs without a font and a minute; with a surface of limit 2 it
    // takes three renders. `with s { glyph_limit: 2 }` is how a test asks.
    glyph_limit: int
}

pub val glyph_cache_limit = 8192

pub fn surface_of(width_pt: float, height_pt: float, px_per_pt: float) -> Result<Surface, string>

pub fn surface_fitting(

pub fn surface_pixels_held(c: Surface) -> int

pub fn surface_width(c: Surface) -> int

pub fn surface_height(c: Surface) -> int

pub fn cached_glyphs(c: Surface) -> int

pub fn render_into(c: Surface, s: Scene, o: Options) -> Result<image.Image, string>

pub fn draw_into(c: Surface, s: Scene, o: Options) -> Result<unit, string>

pub struct Painted {
    x: int
    y: int
    width: int
    height: int
}

pub fn draw_into_region(c: Surface, s: Scene, o: Options, region: Rect) -> Result<Painted, string>

pub fn damage_regions(previous: Scene, current: Scene, o: Options, most: int) -> [Rect]

pub fn damage_between(previous: Scene, current: Scene, o: Options) -> Option<Rect>

pub fn draw_into_regions(

pub struct Premultiplied {
    width: int
    height: int
    data: MutableArray<float>
}

pub fn premultiplied_of(c: Surface) -> Premultiplied

pub fn render(s: Scene, o: Options) -> Result<image.Image, string>

```

## `regex`

Regular expressions: a native engine that runs in time linear in the text (no backreferences), with patterns compiled once and cached.

```
pub struct Regex {
    handle: int
    groups: int
    pattern: string
    names: [string]
}

pub struct Match {
    start: int
    end: int
    text: string
    groups: [Option<string>]
    names: [string]
}

pub fn compile(pattern: string) -> Result<Regex, string>

pub fn is_match(re: Regex, text: string) -> bool

pub fn find_from(re: Regex, text: string, from: int) -> Option<Match>

pub fn find(re: Regex, text: string) -> Option<Match>

pub fn find_all(re: Regex, text: string) -> [Match]

pub fn group(m: Match, n: int) -> Option<string>

pub fn group_named(m: Match, name: string) -> Option<string>

pub fn replace(re: Regex, text: string, replacement: string) -> string

pub fn replace_all(re: Regex, text: string, replacement: string) -> string

pub fn split(re: Regex, text: string) -> [string]

pub fn escape(text: string) -> string

```

## `rest`

**Experimental** (see docs/design/journal/ "Experimental modules"). A REST toolkit over `http` and `server`: an endpoint is a method, a URL pattern and a handler, a table of them is compiled once, and `dispatch` answers a request from it.

```
pub struct Params {
    names: [string]
    values: [string]
}

```

## `scene`

The display list: drawing operations as values, and a scene as a tree of them.

```
pub struct TextRun {
    text: string
    at: Point
    size: float
    font: int
    fill: Paint
    // Which bytes of `text` each run of glyphs came from. Empty means one glyph per codepoint, left
    // to right, which a backend can derive for itself -- and which stops being true the moment
    // shaping exists. Carried from the start because PDF's `/ToUnicode` needs it (phase 3) and
    // because adding it later would change a struct every backend reads. Cairo's
    // `cairo_show_text_glyphs` takes the same three-part input for the same reason.
    clusters: [Cluster]
}

pub struct Cluster {
    bytes: int
    glyphs: int
}

pub struct Filled {
    path: Path
    fill: Paint
}

pub struct Stroked {
    path: Path
    fill: Paint
    stroke: Stroke
}

pub struct Placed {
    image: int
    into: Rect
}

pub struct Group {
    transform: Matrix
    clip: Option<Path>
    ops: [Op]
}

pub enum Op {
    Fill(Filled)
    StrokePath(Stroked)
    Text(TextRun)
    DrawImage(Placed)
    Nested(Group)
}

pub fn fill(path: Path, using: Paint) -> Op

pub fn stroke(path: Path, using: Paint, how: Stroke) -> Op

pub fn text(run: TextRun) -> Op

pub fn text_of(content: string, at: Point, size: float, font: int, using: Paint) -> Op

pub fn image(handle: int, into: Rect) -> Op

pub fn group(transform: Matrix, ops: [Op]) -> Op

pub fn clipped(clip: Path, ops: [Op]) -> Op

pub fn group_of(transform: Matrix, clip: Option<Path>, ops: [Op]) -> Op

pub struct Scene {
    width: float
    height: float
    background: Option<Paint>
    ops: [Op]
}

pub fn scene_of(width: float, height: float) -> Scene

pub fn with_ops(s: Scene, ops: [Op]) -> Scene

pub fn with_background(s: Scene, p: Paint) -> Scene

pub fn op_count(s: Scene) -> int

```

## `schedule`

Periodic background work inside a long-running process.

```
pub enum Schedule {
    // Start a run every N milliseconds, the first one immediately.
    EveryMillis(int)
    // Run once a day, at this many seconds past 00:00:00 UTC.
    DailyAtUtc(int)
    // Run whenever the UTC wall clock matches a cron pattern (`cron`).
    Cron(CronSpec)
}

pub struct CronSpec {
    seconds: [bool]
    minutes: [bool]
    hours: [bool]
    days_of_month: [bool]
    months: [bool]
    days_of_week: [bool]
    dom_restricted: bool
    dow_restricted: bool
}

pub fn every_millis(ms: int) -> Result<Schedule, string>

pub fn daily_at_utc(hour: int, minute: int) -> Result<Schedule, string>

pub fn cron(expr: string) -> Result<Schedule, string>

pub struct Job {
    name: string
    schedule: Schedule
    run: fn() -> Result<unit, string>
}

pub fn job(name: string, schedule: Schedule, run: fn() -> Result<unit, string>) -> Job

pub fn run_until(jobs: [Job], stop: Channel<unit>, on_error: fn(string, string))

pub fn cron_matches_at(spec: CronSpec, epoch_second: int) -> bool

```

## `server`

An HTTP/1.1 *server* toolkit, written in Ziman on top of `net`: parse a request off a socket, route it, write a response.

```
pub enum HttpError {
    BadRequest(string)
    BodyTooLarge
    HeadersTooLarge
    NotImplemented(string)
    Timeout
    Closed
    Io(string)
}

pub struct Session {
    sock: Socket
    buffer: [byte]
    timeout_ms: int
    head_timeout_ms: int
    fresh: bool
    // The largest request body `read_request` will accept, or 0 for this module's 1 MiB default.
    // `serve` fills it from `ServeOptions.max_body_bytes`; the `open_session*` constructors leave it
    // 0, so a caller reading requests off its own socket keeps the default until it says otherwise.
    //
    // It is here rather than only on `ServeOptions` because `read_request(session)` is the public
    // entry point and the North Star backend calls it directly -- a limit the server knows has to
    // reach it somehow, and widening that signature would break every such caller (D21, D22).
    max_body_bytes: int
    // Past how many bytes a request body is written to a file instead of held in memory, or 0 --
    // the default, and this codebase's convention for off -- to never spill. `serve` fills it from
    // `ServeOptions.spill_body_bytes`; the `open_session*` constructors leave it 0, so a caller
    // reading requests off its own socket holds every body until it says otherwise.
    //
    // **This does not remove `max_body_bytes`; it changes what that number bounds**, from memory to
    // disk. A body over the cap is still refused with 413 -- so an upload that should be accepted
    // needs both a disk-sized cap and a threshold below it (docs/design/byte-streams.md section 13).
    //
    // **The file is the server's.** `serve` deletes it when the request ends, including when the
    // handler fails; a caller running its own loop calls `discard_body` itself. Either way a
    // handler must not keep the body, or a source over it, past its own return.
    spill_body_bytes: int
}

pub struct Exchange {
    request: Request
    session: Session
}

pub fn open_session(sock: Socket) -> Session

pub fn open_session_with_timeout(sock: Socket, timeout_ms: int) -> Session

pub fn open_session_with_timeouts(sock: Socket, timeout_ms: int, head_timeout_ms: int) -> Session

```

## `shape`

Paths, and the five segment types everything drawable reduces to.

```
pub struct Quad {
    control: Point
    end: Point
}

pub struct Cubic {
    control1: Point
    control2: Point
    end: Point
}

pub enum Segment {
    MoveTo(Point)
    LineTo(Point)
    QuadTo(Quad)
    CubicTo(Cubic)
    Close
}

pub enum Winding {
    NonZero
    EvenOdd
}

pub struct Path {
    segments: [Segment]
    winding: Winding
    // The control-point hull, **computed once when the path is made** rather than on every
    // question about it.
    //
    // **Profiled 2026-11-14, and it was the largest single cost in a regional repaint**: a
    // one-pixel damage region on a 901-op scene took 10.8 ms, of which 37% was recomputing these
    // boxes -- `path_bounds` -> `bounds_builder` -> `include` -> `rect_union` -- for ops that
    // then failed the window test and drew nothing. The scene is rebuilt every frame, so the
    // walk happened every frame, per op, to answer a question each path could have answered at
    // birth.
    //
    // **Free for the shapes a user interface is made of.** A rectangle's hull is the rectangle; a
    // round rect's, an ellipse's and a circle's are too, because their control points all lie
    // inside the box and the curves touch all four edges. Those constructors set this from their
    // own argument and walk nothing. The general ones (`built`, `path_of`, `polygon_path`) walk
    // their segments once, which is what every draw used to do.
    //
    // It is not an optimisation to be undone later: `path_bounds` is the same function it was,
    // and `tests/lang/graphics_shape.zm` checks each constructor's stored box against a walk of
    // its own segments, so the analytic shortcuts cannot drift from the definition.
    bounds: Rect
}

pub struct PathBuilder {
    segments: MutableArray<Segment>
}

pub fn path_builder() -> PathBuilder

pub fn move_to(b: PathBuilder, p: Point)

pub fn line_to(b: PathBuilder, p: Point)

pub fn quad_to(b: PathBuilder, control: Point, p: Point)

pub fn cubic_to(b: PathBuilder, c1: Point, c2: Point, p: Point)

pub fn close_path(b: PathBuilder)

pub fn built(b: PathBuilder, winding: Winding) -> Path

pub fn path_of(segments: [Segment], winding: Winding) -> Path

pub fn empty_path() -> Path

pub fn line_path(from: Point, to: Point) -> Path

pub fn rect_path(r: Rect) -> Path

pub fn polygon_path(points: [Point], closed: bool) -> Path

pub fn ellipse_path(r: Rect) -> Path

pub fn circle_path(centre: Point, radius: float) -> Path

pub fn round_rect_path(r: Rect, radius: float) -> Path

pub fn round_rect_path(r: Rect, tl: float, tr: float, br: float, bl: float) -> Path

pub fn round_rect_path(r: Rect, rx: float, ry: float) -> Path

pub fn ring_path(outer: Path, inner: Path) -> Path

pub fn transform_path(p: Path, m: Matrix) -> Path

pub fn path_bounds(p: Path) -> Rect

pub fn path_tight_bounds(p: Path) -> Rect

pub struct Polyline {
    points: [Point]
    closed: bool
}

pub fn flattened(p: Path, tolerance: float) -> [Polyline]

```

## `sqlite`

The SQLite driver behind `datasource`.

```
pub struct Connection {
    handle: int
}

pub fn open(path: string) -> Result<Connection, DbError>

pub fn close(conn: Connection)

pub fn execute(conn: Connection, sql: string, params: variadic SqlValue) -> Result<int, DbError>

pub fn query(conn: Connection, sql: string, params: variadic SqlValue) -> Result<[Row], DbError>

pub fn rows(conn: Connection, sql: string, params: variadic SqlValue) -> Result<Iter<Result<Row, DbError>>, DbError>

pub fn driver() -> Driver

```

## `sql`

**Experimental** (see docs/design/journal/ "Experimental modules"). SQL as a value: a statement tree, a parser that reads one, and a formatter that writes it back.

```
pub fn fold_name(name: string) -> string

pub struct Token {
    kind: string
    text: string
    // `text` upper-cased once, for a `name` token, and empty for every other kind.
    //
    // **The parser asks "is this word FROM?" far more often than there are tokens** -- a single
    // bare alias runs the whole `clause_words` list past one token -- and each such question used
    // to upper-case the token's text again, allocating a string to throw away. Folding once, here,
    // is what turns that from a per-question cost into a per-token one.
    upper: string
    value: int
    // Where this token sits in the source, so a run of tokens can be recovered as the text it was
    // written as. That is what lets the parts this module does not model be **preserved verbatim**
    // rather than re-assembled from tokens with guessed spacing.
    start: int
    end: int
}

pub fn tokenize(sql: string) -> Result<[Token], DbError>

pub enum ConstraintRule {
    PrimaryKey([string])
    Unique([string])
    Check(Expr)
    ForeignKey(ForeignKeyRule)
    // A shape this module does not model, carried by the constraint's `text` and written back
    // verbatim.
    Opaque
}

pub struct ForeignKeyRule {
    // The child's columns -- this table's.
    columns: [string]
    table: string
    // The parent's columns. Empty where none were named, which means the parent's primary key.
    references: [string]
    // `CASCADE`, `SET NULL`, `SET DEFAULT`, `RESTRICT`, `NO ACTION`, or empty for the default --
    // which *is* `NO ACTION`, spelled differently so a reader can tell "not written" from
    // "written as the default".
    on_delete: string
    on_update: string
}

pub struct TableConstraint {
    // From `CONSTRAINT <name>`, or empty. An engine quotes it in its message when it is there.
    name: string
    rule: ConstraintRule
    // The constraint exactly as written, brackets and spacing included. `format` writes it for an
    // `Opaque` rule, and an engine's message quotes the expression inside it for an unnamed
    // `CHECK` -- which is what SQLite quotes, spacing and all.
    text: string
}

pub struct Column {
    name: string
    type_name: string
    not_null: bool
    primary_key: bool
    unique: bool
    default_value: Option<SqlValue>
    // The constraints written in this column's own position that this module models: a bare
    // `CHECK (e)` and a bare `REFERENCES t (c)`, each with this column as its column list. They
    // stay here rather than moving to the table's list so `format` writes them back where they
    // were -- the sample application's migrations put every foreign key in column position.
    //
    // A *named* column constraint (`CONSTRAINT k CHECK (..)`) is **not** modelled and goes to
    // `extra` whole, so nothing of it is lost and nothing of it is enforced.
    constraints: [TableConstraint]
    // Every constraint on this column that is modelled by neither the four flags above nor
    // `constraints`, as the text it was written as: `COLLATE NOCASE`, `AUTOINCREMENT`,
    // `GENERATED ALWAYS AS ..`, a named constraint. Preserved rather than modelled, so `format`
    // writes back what it read.
    extra: string
}

pub struct BinNode {
    op: string
    left: Expr
    right: Expr
}

pub struct NotNode {
    inner: Expr
}

pub struct NullNode {
    inner: Expr
    negated: bool
}

pub struct ColRef {
    table: string
    name: string
    // `table` and `name` folded once, by `fold_name`, for a resolver to compare against.
    //
    // **The same bargain `Token.upper` makes one layer down, and for the same measured reason.**
    // A name is *resolved* far more often than it is written: `memdb`'s `resolve_column` runs per
    // column reference per row, and it folded both of these on every call -- `sql::fold_name` was
    // **14.3%** of tests/bench/memdb_ops.zm, which is `to_lower` allocating a string to throw away.
    // Folding here turns that from a per-resolution cost into a per-reference one.
    //
    // **The written spelling stays in `table` and `name`**, because an error message quotes what
    // the user typed: "no such column: Ada" names the column they wrote, not the one we matched on.
    // That is the whole reason this is two more fields rather than folding in place.
    folded_table: string
    folded_name: string
}

pub fn col_ref(table: string, name: string) -> ColRef

pub struct CallNode {
    name: string
    args: [Expr]
    // `COUNT(DISTINCT x)`: the duplicates are dropped before the aggregate sees them.
    distinct: bool
}

pub struct LikeNode {
    subject: Expr
    pattern: Expr
    negated: bool
}

pub struct InNode {
    subject: Expr
    options: [Expr]
    negated: bool
}

pub struct BetweenNode {
    subject: Expr
    low: Expr
    high: Expr
    negated: bool
}

pub struct WhenArm {
    condition: Expr
    value: Expr
}

pub struct CaseNode {
    arms: [WhenArm]
    fallback: [Expr]
}

pub enum Expr {
    Lit(SqlValue)
    Col(ColRef)
    Param(int)
    Not(NotNode)
    Bin(BinNode)
    IsNull(NullNode)
    Call(CallNode)
    // A scalar subquery, `(SELECT ..)`, which must answer one row of one column.
    Sub(SubNode)
    // `EXISTS (SELECT ..)`. `NOT EXISTS` is `Not(Exists(..))` -- `NOT` has its own precedence
    // level now, so it needs no flag of its own here.
    Exists(SubNode)
    InSub(InSubNode)
    Cast(CastNode)
    Like(LikeNode)
    In(InNode)
    Between(BetweenNode)
    Case(CaseNode)
    Star
    // `t.*`, holding the table name or alias it was written with. A separate variant rather than a
    // qualifier on `Star`, so `count(*)` and every other reader of a bare star is untouched.
    TableStar(string)
}

pub struct TableRef {
    // The table's name, or empty for a derived table -- a `(SELECT ..)` in the `FROM`, which has a
    // query instead of a name.
    name: string
    alias: string
    // The query a derived table is. Empty for a named table; one element otherwise. An array
    // rather than an `Option`, the way `CaseNode.fallback` is, so a `TableRef` need not name the
    // type that contains it.
    query: [SelectStmt]
    // `AS x (a, b)` -- the names the derived table's columns take, which may be shorter than the
    // query's list and rename only its first columns. Empty where none were written.
    column_aliases: [string]
}

pub struct Join {
    kind: string
    table: TableRef
    on: Option<Expr>
    using: [string]
}

pub struct SubNode {
    query: SelectStmt
}

pub struct InSubNode {
    subject: Expr
    query: SelectStmt
    negated: bool
}

pub struct CastNode {
    inner: Expr
    type_name: string
}

pub struct SelectItem {
    expr: Expr
    alias: string
}

pub struct OrderTerm {
    expr: Expr
    descending: bool
}

pub struct Assignment {
    column: string
    value: Expr
}

pub struct SetOp {
    kind: string
    // `ALL`, `DISTINCT`, or empty where neither was written -- kept as the word it was rather than
    // reduced to a flag. `DISTINCT` is the default and changes nothing, but SQLite *refuses* it
    // after a set operator where PostgreSQL accepts it, so an engine has to be able to tell
    // `UNION DISTINCT` from `UNION`; a formatter that normalised one into the other would turn SQL
    // one engine rejects into SQL it accepts.
    quantifier: string
    query: SelectStmt
}

pub fn set_op_keeps_all(op: SetOp) -> bool

pub struct SelectStmt {
    distinct: bool
    items: [SelectItem]
    // Empty when the statement has no `FROM`: `SELECT 1` is legal in both real engines and is the
    // liveness probe `datasource`'s pool sends, so it has to be legal here. Otherwise exactly one
    // element, with `joins` carrying the rest.
    from: [TableRef]
    joins: [Join]
    filter: Option<Expr>
    groups: [Expr]
    having: Option<Expr>
    order: [OrderTerm]
    limit: Option<Expr>
    offset: Option<Expr>
    // The set operations applied to this query, left to right. Empty for a simple `SELECT`.
    //
    // On a *compound* query the head carries them, and the head's `order`, `limit` and `offset`
    // belong to the **whole** compound rather than to its first arm -- which is what SQL means by
    // them, since an arm may not have an `ORDER BY` of its own (`SELECT .. ORDER BY .. UNION ..` is
    // an error in both engines). An arm's own `set_ops`, `order`, `limit` and `offset` are always
    // empty, so a reader can treat an arm as a simple query.
    set_ops: [SetOp]
}

pub struct InsertStmt {
    table: string
    columns: [string]
    rows: [[Expr]]
    // `RETURNING a, b` or `RETURNING *`, as select items so an alias and an expression both work.
    // Empty where the statement has no such clause, which is the overwhelming majority.
    returning: [SelectItem]
}

pub struct UpdateStmt {
    table: string
    sets: [Assignment]
    filter: Option<Expr>
    returning: [SelectItem]
}

pub struct DeleteStmt {
    table: string
    filter: Option<Expr>
    returning: [SelectItem]
}

pub struct CreateStmt {
    table: string
    columns: [Column]
    // The table-level constraints -- `PRIMARY KEY (a, b)`, `FOREIGN KEY ... REFERENCES ...`,
    // `UNIQUE (..)`, `CHECK (..)`, each with the name a `CONSTRAINT` prefix gave it.
    constraints: [TableConstraint]
    if_not_exists: bool
}

pub enum AlterAction {
    AddColumn(Column)
    AddConstraint(string)
}

pub struct AlterStmt {
    table: string
    action: AlterAction
}

pub enum IndexOrder {
    Unwritten
    Ascending
    Descending
}

pub struct IndexColumn {
    // **An expression, not a name.** `CREATE INDEX i ON t (lower(email))` indexes an expression,
    // and a bare column is simply the smallest one -- it parses to `Expr.Col` and writes back as
    // the name it came from, so the common case costs nothing to read or to print.
    target: Expr
    // The collation named with `COLLATE`, or empty where none was.
    collation: string
    order: IndexOrder
}

pub struct CreateIndexStmt {
    name: string
    table: string
    columns: [IndexColumn]
    unique: bool
    if_not_exists: bool
    // A partial index's own `WHERE`: `CREATE INDEX i ON t (a) WHERE b IS NOT NULL`. `None` where
    // none was written, which is every index that covers the whole table.
    predicate: Option<Expr>
}

pub struct DropStmt {
    table: string
    if_exists: bool
}

pub enum Statement {
    Select(SelectStmt)
    Insert(InsertStmt)
    Update(UpdateStmt)
    Delete(DeleteStmt)
    Create(CreateStmt)
    CreateIndex(CreateIndexStmt)
    Alter(AlterStmt)
    Drop(DropStmt)
    Begin
    Commit
    Rollback
    // A statement recognised only well enough to pass through -- `PRAGMA`, `SET`, `ANALYZE`,
    // `VACUUM` -- carrying its own text so `format` writes back exactly what was read. Empty for
    // an empty statement.
    Passthrough(string)
}

pub fn parse(sql: string) -> Result<Statement, DbError>

pub fn parse_many(text: string) -> Result<[Statement], DbError>

pub fn parse_file(path: string) -> Result<[Statement], DbError>

pub fn format_value(v: SqlValue) -> string

pub fn format_expr(e: Expr) -> string

pub fn format_constraint(tc: TableConstraint) -> string

pub fn format(s: Statement) -> string

```

## `stroker`

The outline of a stroked path, as a path.

```
pub fn outline(p: Path, how: Stroke) -> Result<Path, string>

pub fn outline_with(p: Path, how: Stroke, tolerance: float) -> Result<Path, string>

```

## `svg`

An SVG file as shapes: one `d` attribute, or a whole document.

```
pub fn path_data(d: string) -> Result<shape.Path, string>

pub fn in_unit_box(p: shape.Path, view: geom.Rect) -> shape.Path

pub fn icon_path(d: string, view: geom.Rect) -> Result<shape.Path, string>

pub enum Fill {
    Color(paint.Color)
    Current
    Nothing
}

pub struct Figure {
    path: shape.Path
    fill: Fill
    stroke: Fill
    // Width, caps, joins and dashes, as the document asked for them. Meaningful when `stroke` is
    // not `Nothing`; `stroker.outline` turns it into an area.
    edge: paint.Stroke
    // `opacity` times `fill-opacity`, 0.0 to 1.0. A figure at 0 is dropped before it gets here, so
    // this is above 0 and at most 1.
    opacity: float
}

pub struct Drawing {
    // The `viewBox`, or `0 0 width height` when the root gives those instead, or the union of what
    // the figures cover when it gives neither.
    view: geom.Rect
    figures: [Figure]
}

pub fn parse(text: string) -> Result<Drawing, string>

pub fn parse(data: [byte]) -> Result<Drawing, string>

pub fn drawing_of(doc: xml.Document) -> Result<Drawing, string>

pub fn filled_path(d: Drawing) -> Result<shape.Path, string>

pub fn solid_path(d: Drawing, least: float) -> Result<shape.Path, string>

pub fn icon(text: string) -> Result<shape.Path, string>

pub fn icon(data: [byte]) -> Result<shape.Path, string>

```

## `template`

**Experimental** (see docs/design/journal/ "Experimental modules"). Templates for generated code: the shape of the output, visible in the source.

```
pub struct Sheet {
    templates: [Named]
}

pub struct Row {
    cells: [Binding]
}

pub struct Binding {
    name: string
    value: Json
}

pub fn text(name: string, value: string) -> Binding

pub fn flag(name: string, value: bool) -> Binding

pub fn rows(name: string, value: [Row]) -> Binding

pub fn row(cells: [Binding]) -> Row

pub fn context(bindings: [Binding]) -> Json

pub fn parse(source: string) -> Result<Sheet, string>

pub fn must_parse(source: string) -> Sheet

pub fn render(sheet: Sheet, name: string, ctx: Json) -> Result<string, string>

pub fn render_text(source: string, ctx: Json) -> Result<string, string>

pub fn render(sheet: Sheet, name: string, bindings: [Binding]) -> Result<string, string>

```

## `terminal`

**Experimental** (see docs/design/journal/ "Experimental modules"). Raw mode, one key at a time, and putting back what we changed.

```
pub enum Key {
    Char(string)
    Enter
    Tab
    Backspace
    Escape
    Delete
    Up
    Down
    Left
    Right
    Home
    End
    PageUp
    PageDown
    Insert
    Function(int)
    Ctrl(string)
    Unknown([byte])
}

pub struct Size {
    rows: int
    cols: int
}

pub native fn is_tty() -> bool

pub fn size() -> Result<Size, string>

pub fn with_raw<T>(body: fn() -> Result<T, string>) -> Result<T, string>

pub fn with_screen<T>(body: fn() -> Result<T, string>) -> Result<T, string>

pub fn read_key(timeout_ms: int) -> Result<Option<Key>, string>

pub fn decode_key(bytes: [byte]) -> Key

pub fn key_name(key: Key) -> string

pub fn at(row: int, col: int) -> string

pub fn clear_screen() -> string

pub fn clear_to_line_end() -> string

pub fn hide_cursor() -> string

pub fn show_cursor() -> string

pub fn enter_alternate_screen() -> string

pub fn leave_alternate_screen() -> string

pub enum Color {
    Black
    Red
    Green
    Yellow
    Blue
    Magenta
    Cyan
    White
    Default
}

pub fn fg(c: Color) -> string

pub fn bg(c: Color) -> string

pub fn bold() -> string

pub fn dim() -> string

pub fn reverse() -> string

pub fn reset_style() -> string

```

## `test`

Assertions for #[test] functions (see docs/syntax/ "Testing"): `ziman test` runs every #[test] function it finds, and a failed assertion here is what makes one fail.

```
pub fn fail(message: string)

pub fn assert(cond: bool)

pub fn assert_false(cond: bool)

pub fn assert_eq<T: Eq>(expected: T, actual: T)

pub fn assert_ne<T: Eq>(unexpected: T, actual: T)

pub fn assert_near(expected: float, actual: float, tolerance: float)

pub fn assert_lt<T: Ord>(actual: T, bound: T)

pub fn assert_le<T: Ord>(actual: T, bound: T)

pub fn assert_gt<T: Ord>(actual: T, bound: T)

pub fn assert_ge<T: Ord>(actual: T, bound: T)

pub fn assert_contains(haystack: string, needle: string)

pub fn assert_contains<T: Eq>(items: [T], x: T)

pub fn assert_contains<T: Eq + Hash>(items: Set<T>, x: T)

pub fn assert_contains<K: Eq + Hash, V>(m: Map<K, V>, key: K)

pub fn assert_not_contains(haystack: string, needle: string)

pub fn assert_not_contains<T: Eq>(items: [T], x: T)

pub fn assert_not_contains<T: Eq + Hash>(items: Set<T>, x: T)

pub fn assert_not_contains<K: Eq + Hash, V>(m: Map<K, V>, key: K)

pub fn assert_len(expected: int, items: string)

pub fn assert_len<T>(expected: int, items: [T])

pub fn assert_len<T: Eq + Hash>(expected: int, items: Set<T>)

pub fn assert_len<K: Eq + Hash, V>(expected: int, m: Map<K, V>)

pub fn assert_empty(items: string)

pub fn assert_empty<T>(items: [T])

pub fn assert_empty<T: Eq + Hash>(items: Set<T>)

pub fn assert_empty<K: Eq + Hash, V>(m: Map<K, V>)

pub fn assert_not_empty(items: string)

pub fn assert_not_empty<T>(items: [T])

pub fn assert_not_empty<T: Eq + Hash>(items: Set<T>)

pub fn assert_not_empty<K: Eq + Hash, V>(m: Map<K, V>)

pub fn assert_all_match<T>(items: [T], test: fn(T) -> bool)

pub fn assert_any_match<T>(items: [T], test: fn(T) -> bool)

pub fn assert_none_match<T>(items: [T], test: fn(T) -> bool)

pub fn assert_entry<K: Eq + Hash, V: Eq>(m: Map<K, V>, key: K, value: V)

pub fn assert_starts_with(s: string, prefix: string)

pub fn assert_ends_with(s: string, suffix: string)

pub fn assert_blank(s: string)

pub fn assert_not_blank(s: string)

pub fn assert_matches(s: string, pattern: string)

pub fn assert_not_matches(s: string, pattern: string)

pub fn assert_some<T>(opt: Option<T>) -> T

pub fn assert_none<T>(opt: Option<T>)

pub fn assert_ok<T, E>(r: Result<T, E>) -> T

pub fn assert_err<T, E>(r: Result<T, E>) -> E

```

## `theme`

The looks the control set ships with, and the register of them.

```
pub enum Scheme {
    Light
    Dark
}

pub enum Toward {
    // A face is darker than the window it sits on: window 1, panel 2, face 3.
    Ink
    // A face is lighter than the window: panel 1, face 2, window 3.
    Away
}

pub fn ramp(seed: Color, s: Scheme) -> [Color]

pub fn accent_ramp(seed: Color, s: Scheme) -> [Color]

pub val step_hovered = 4

pub val step_pressed = 5

pub val step_border = 7

pub val step_solid = 9

pub val step_ink = 12

pub struct Fill {
    from: int
    to: int
    // Degrees clockwise from the top, which is CSS's convention and the one a designer will say
    // out loud. Ignored when `from == to`.
    angle: float
}

pub fn flat(step: int) -> Fill

pub fn paint_of(f: Fill, steps: [Color]) -> Paint

pub struct Seeds {
    neutral: Color
    accent: Color
    surfaces: Toward
    // Whether a face wears its edge.
    //
    // **A ramp always has a border colour -- step 7 is "element border" by role -- but whether a
    // look draws it is not something two colour seeds imply.** It is the same kind of choice as
    // `Toward` and as the shape scale: `plain` is square, filled and unedged on purpose, so that
    // an application replacing it has nothing to undo, and `modern` is edged because on a white
    // window the border is what says "this is a control".
    edged: bool
}

pub fn derive(seeds: Seeds, s: Scheme) -> controls.Style

pub struct Look {
    // What to put in a menu. Capitalised as a title, because it is one. **Computed** for a dark
    // row -- "Plain" and "Plain dark" are one family's two schemes, not two names someone typed.
    name: string
    style: controls.Style
    // Which end of the ramp this row was read from.
    //
    // **Read off the register rather than set by hand**, which is what absorbs F7: an application
    // asking "is the look I am showing a dark one" was answering it by matching the name, and a
    // look called "Midnight" would have broken that. The register knows, because it built the row.
    scheme: Scheme
}

pub struct Family {
    name: string
    light: controls.Style
    dark: controls.Style
}

pub enum Density {
    Compact
    Comfortable
    Spacious
}

pub fn at_density(s: controls.Style, d: Density) -> controls.Style

pub fn density_name(d: Density) -> string

pub fn densities() -> [Density]

pub fn all() -> [Look]

pub fn named(name: string) -> Option<controls.Style>

pub fn default_style() -> controls.Style

pub fn ink_contrast(style: controls.Style) -> float

```

## `thread`

What a thread can ask about itself, and values that are different on every thread.

```
pub native fn id() -> int

pub native fn name() -> string

pub native fn set_name(name: string)

pub native fn is_main() -> bool

pub native fn stack_remaining() -> int

pub struct ThreadLocal<T> {
    id: int
    init: fn() -> T
}

pub fn local<T>(init: fn() -> T) -> ThreadLocal<T>

pub fn get<T>(key: ThreadLocal<T>) -> T

pub fn set<T>(key: ThreadLocal<T>, value: T)

pub fn has<T>(key: ThreadLocal<T>) -> bool

pub fn reset<T>(key: ThreadLocal<T>)

```

## `time`

Dates, times, durations and instants, modeled on java.time and written in ordinary Ziman (the only native code is `clock.now_raw`, which reads the system clock).

```
pub fn is_leap_year(year: int) -> bool

pub struct DateTimeFormat {
    parts: [DateTimePart]
    width: int
    // Whether a `Z` or an `XXX` is in the pattern, computed once with `width` because every
    // `format`/`parse` of an `Instant` asks: an offset the pattern carries is rendered and read in
    // place, and one it does not is the suffix outside it that this module has always written.
    carries_offset: bool
}

pub fn datetime_format_of(pattern: string) -> DateTimeFormat

pub val iso_date_format = datetime_format_of("yyyy-MM-dd")

pub val iso_time_format = datetime_format_of("HH:mm:ss")

pub val iso_time_nanos_format = datetime_format_of("HH:mm:ss.SSSSSSSSS")

pub val iso_datetime_format = datetime_format_of("yyyy-MM-ddTHH:mm:ss")

pub val iso_datetime_nanos_format = datetime_format_of("yyyy-MM-ddTHH:mm:ss.SSSSSSSSS")

pub struct LocalDate {
    year: int
    month: int
    day: int
}

pub fn localdate_of(year: int, month: int, day: int) -> Result<LocalDate, string>

pub fn to_epoch_day(d: LocalDate) -> int

pub fn localdate_from_epoch_day(epoch_day: int) -> LocalDate

pub fn add_days(d: LocalDate, days: int) -> LocalDate

pub fn add_months(d: LocalDate, months: int) -> LocalDate

pub fn add_years(d: LocalDate, years: int) -> LocalDate

pub fn diff_days(a: LocalDate, b: LocalDate) -> int

pub fn localdate_compare(a: LocalDate, b: LocalDate) -> int

pub fn format(d: LocalDate, fmt: DateTimeFormat) -> string

pub fn format(d: LocalDate, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_localdate(s: string, fmt: DateTimeFormat) -> Result<LocalDate, string>

pub fn parse_localdate(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalDate, string>

pub fn parse_localdate(s: string) -> Result<LocalDate, string>

pub fn format(d: LocalDate) -> string

pub struct LocalTime {
    hour: int
    minute: int
    second: int
    nano: int
}

pub fn localtime_of(hour: int, minute: int, second: int, nano: int) -> Result<LocalTime, string>

pub fn add_nanos(t: LocalTime, nanos: int) -> LocalTime

pub fn add_seconds(t: LocalTime, seconds: int) -> LocalTime

pub fn add_minutes(t: LocalTime, minutes: int) -> LocalTime

pub fn add_hours(t: LocalTime, hours: int) -> LocalTime

pub fn diff_nanos(a: LocalTime, b: LocalTime) -> int

pub fn localtime_compare(a: LocalTime, b: LocalTime) -> int

pub fn format(t: LocalTime, fmt: DateTimeFormat) -> string

pub fn format(t: LocalTime, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_time(s: string, fmt: DateTimeFormat) -> Result<LocalTime, string>

pub fn parse_time(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalTime, string>

pub fn parse_time(s: string) -> Result<LocalTime, string>

pub fn format(t: LocalTime) -> string

pub struct Duration {
    seconds: int
    nanos: int
}

pub fn duration_of_seconds(seconds: int, nanos: int) -> Duration

pub fn duration_zero() -> Duration

pub fn add(a: Duration, b: Duration) -> Duration

pub fn negate(d: Duration) -> Duration

pub fn subtract(a: Duration, b: Duration) -> Duration

pub fn duration_compare(a: Duration, b: Duration) -> int

pub fn format(d: Duration) -> string

pub fn parse_duration(s: string) -> Result<Duration, string>

pub struct LocalDateTime {
    date: LocalDate
    time: LocalTime
}

pub fn localdatetime_of(date: LocalDate, time: LocalTime) -> LocalDateTime

pub fn add_nanos(dt: LocalDateTime, nanos: int) -> LocalDateTime

pub fn add_seconds(dt: LocalDateTime, seconds: int) -> LocalDateTime

pub fn add_minutes(dt: LocalDateTime, minutes: int) -> LocalDateTime

pub fn add_hours(dt: LocalDateTime, hours: int) -> LocalDateTime

pub fn add_days(dt: LocalDateTime, days: int) -> LocalDateTime

pub fn add_months(dt: LocalDateTime, months: int) -> LocalDateTime

pub fn add_years(dt: LocalDateTime, years: int) -> LocalDateTime

pub fn localdatetime_compare(a: LocalDateTime, b: LocalDateTime) -> int

pub fn diff(a: LocalDateTime, b: LocalDateTime) -> Duration

pub fn format(dt: LocalDateTime, fmt: DateTimeFormat) -> string

pub fn format(dt: LocalDateTime, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_datetime(s: string, fmt: DateTimeFormat) -> Result<LocalDateTime, string>

pub fn parse_datetime(s: string, fmt: DateTimeFormat, l: Locale) -> Result<LocalDateTime, string>

pub fn parse_datetime(s: string) -> Result<LocalDateTime, string>

pub fn format(dt: LocalDateTime) -> string

pub struct ZoneOffset {
    total_seconds: int
}

pub fn zoneoffset_utc() -> ZoneOffset

pub fn zoneoffset_of_seconds(total_seconds: int) -> Result<ZoneOffset, string>

pub fn zoneoffset_of_hours_minutes(hours: int, minutes: int) -> Result<ZoneOffset, string>

pub fn zoneoffset_compare(a: ZoneOffset, b: ZoneOffset) -> int

pub fn format(z: ZoneOffset) -> string

pub fn parse_offset(s: string) -> Result<ZoneOffset, string>

pub struct ZoneId {
    name: string
    rules: ZoneRules
}

pub fn zoneid_utc() -> ZoneId

pub fn zoneid_of(name: string) -> Result<ZoneId, string>

pub fn zoneid_offset_at(z: ZoneId, i: Instant) -> ZoneOffset

pub fn zoneid_is_dst_at(z: ZoneId, i: Instant) -> bool

pub fn zoneid_abbreviation_at(z: ZoneId, i: Instant) -> string

pub fn format(z: ZoneId) -> string

pub fn parse_zone(s: string) -> Result<ZoneId, string>

pub struct Instant {
    seconds: int
    nanos: int
}

pub fn instant_of(seconds: int, nanos: int) -> Instant

pub fn instant_now() -> Instant

pub fn add(i: Instant, d: Duration) -> Instant

pub fn subtract(i: Instant, d: Duration) -> Instant

pub fn diff(a: Instant, b: Instant) -> Duration

pub fn instant_compare(a: Instant, b: Instant) -> int

pub fn to_localdatetime(i: Instant, offset: ZoneOffset) -> LocalDateTime

pub fn to_instant(dt: LocalDateTime, offset: ZoneOffset) -> Instant

pub fn localdatetime_now(offset: ZoneOffset) -> LocalDateTime

pub fn localdate_now(offset: ZoneOffset) -> LocalDate

pub fn localtime_now(offset: ZoneOffset) -> LocalTime

pub fn to_localdatetime(i: Instant, z: ZoneId) -> LocalDateTime

pub fn to_instant(dt: LocalDateTime, z: ZoneId) -> Instant

pub fn format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat) -> string

pub fn format(i: Instant, offset: ZoneOffset, fmt: DateTimeFormat, l: Locale) -> string

pub fn parse_instant(s: string, fmt: DateTimeFormat) -> Result<Instant, string>

pub fn parse_instant(s: string, fmt: DateTimeFormat, l: Locale) -> Result<Instant, string>

pub fn parse_instant(s: string) -> Result<Instant, string>

pub fn format(i: Instant) -> string

pub fn to_json(d: LocalDate) -> Json

pub fn to_localdate(j: Json) -> Result<LocalDate, string>

pub fn to_json(t: LocalTime) -> Json

pub fn to_localtime(j: Json) -> Result<LocalTime, string>

pub fn to_json(d: Duration) -> Json

pub fn to_duration(j: Json) -> Result<Duration, string>

pub fn to_json(dt: LocalDateTime) -> Json

pub fn to_localdatetime(j: Json) -> Result<LocalDateTime, string>

pub fn to_json(z: ZoneOffset) -> Json

pub fn to_zoneoffset(j: Json) -> Result<ZoneOffset, string>

pub fn to_json(z: ZoneId) -> Json

pub fn to_zoneid(j: Json) -> Result<ZoneId, string>

pub fn to_json(i: Instant) -> Json

pub fn to_instant(j: Json) -> Result<Instant, string>

pub fn to_sql(v: LocalDate) -> SqlValue

pub fn to_localdate(v: SqlValue) -> Result<LocalDate, DbError>

pub fn to_sql(v: LocalTime) -> SqlValue

pub fn to_localtime(v: SqlValue) -> Result<LocalTime, DbError>

pub fn to_sql(v: Duration) -> SqlValue

pub fn to_duration(v: SqlValue) -> Result<Duration, DbError>

pub fn to_sql(v: LocalDateTime) -> SqlValue

pub fn to_localdatetime(v: SqlValue) -> Result<LocalDateTime, DbError>

pub fn to_sql(v: ZoneOffset) -> SqlValue

pub fn to_zoneoffset(v: SqlValue) -> Result<ZoneOffset, DbError>

pub fn to_sql(v: ZoneId) -> SqlValue

pub fn to_zoneid(v: SqlValue) -> Result<ZoneId, DbError>

pub fn to_sql(v: Instant) -> SqlValue

pub fn to_instant(v: SqlValue) -> Result<Instant, DbError>

```

## `toml`

**Experimental** (see docs/design/journal/ "Experimental modules"). TOML 1.0 on top of `json`: a document is a `Json` object, so any `#[derive(Json)]` type decodes from it with its own `_from_json`.

```
pub fn default_charset() -> charset.Charset

pub fn parse(data: [byte], set: charset.Charset) -> Result<Json, string>

pub fn parse(data: [byte]) -> Result<Json, string>

pub fn parse(text: string) -> Result<Json, string>

pub fn format(j: Json) -> Result<string, string>

```

## `truetype`

Reading the TrueType tables the rest of `lib/graphics` needs, and nothing else.

```
pub struct Face {
    data: [byte]
    units_per_em: int
    glyph_count: int
    // `loca` holds u32 offsets rather than u16 halves.
    long_loca: bool
    // Table offsets, or -1 when the table is absent.
    head: int
    hhea: int
    hmtx: int
    maxp: int
    cmap: int
    // **The cmap subtable to use, chosen once here rather than on every lookup.** `glyph_for` used
    // to call `best_subtable` per character, and that walks every record in the table reading
    // `u16`s and `u32`s out of the byte array -- which measured as 281 ms of a 347 ms frame on a
    // page of text, more than the rasterising it feeds. -1 when the face has no usable subtable,
    // which `glyph_for` reports in the same words it always did.
    cmap_subtable: int
    // Its format, 4 or 12, so the lookup does not re-read the two bytes per character either.
    cmap_format: int
    name: int
    os2: int
    loca: int
    glyf: int
    num_h_metrics: int
    ascender: int
    descender: int
    line_gap: int
    x_min: int
    y_min: int
    x_max: int
    y_max: int
}

pub fn parse(data: [byte]) -> Result<Face, string>

pub fn bbox(f: Face) -> Rect

pub fn glyph_for(f: Face, codepoint: int) -> Result<Option<int>, string>

pub fn advance_of(f: Face, glyph: int) -> Result<int, string>

pub fn face_id(f: Face) -> int

pub fn name_of(f: Face, name_id: int) -> Result<Option<string>, string>

pub fn weight_of(f: Face) -> Result<int, string>

pub fn cap_height_of(f: Face) -> Result<int, string>

pub fn is_italic(f: Face) -> Result<bool, string>

pub fn glyph_outline(f: Face, glyph: int) -> Result<shape.Path, string>

```

## `url`

The percent-encoding both `http` and `db` need, in one place.

```
pub fn percent_decode(s: string, plus_is_space: bool) -> Result<string, string>

pub fn percent_encode(s: string, plus_for_space: bool) -> string

```

## `uuid`

UUIDs: random version 4, time-ordered version 7, and the canonical text form.

```
pub struct Uuid {
    bytes: [byte]
}

pub fn uuid_new() -> Uuid

pub fn uuid_new_v7() -> Uuid

pub fn timestamp_millis(u: Uuid) -> Option<int>

pub fn uuid_of(bytes: [byte]) -> Result<Uuid, string>

pub fn uuid_nil() -> Uuid

pub fn uuid_max() -> Uuid

pub fn is_nil(u: Uuid) -> bool

pub fn format(u: Uuid) -> string

pub fn parse(s: string) -> Result<Uuid, string>

pub fn version(u: Uuid) -> int

pub fn uuid_to_json(u: Uuid) -> Json

pub fn uuid_from_json(j: Json) -> Result<Uuid, string>

pub fn uuid_to_sql(u: Uuid) -> SqlValue

pub fn uuid_from_sql(v: SqlValue) -> Result<Uuid, DbError>

pub fn uuid_compare(a: Uuid, b: Uuid) -> int

```

## `validate`

**Experimental** (see docs/design/journal/ "Experimental modules"). The rules `#[derive(Validate)]` generates calls to, and the messages they answer.

```
pub struct Violation {
    field: string
    message: string
}

pub fn violation(field: string, message: string) -> Violation

pub fn first(messages: [Option<string>]) -> Option<string>

pub fn with_message(found: Option<string>, message: string) -> Option<string>

pub fn not_blank(value: string) -> Option<string>

pub fn not_blank(value: Option<string>) -> Option<string>

pub fn not_empty(value: string) -> Option<string>

pub fn not_empty<T>(values: [T]) -> Option<string>

pub fn not_empty<T>(values: Option<[T]>) -> Option<string>

pub fn one_of(value: string, allowed: [string]) -> Option<string>

pub fn one_of(value: Option<string>, allowed: [string]) -> Option<string>

pub fn one_of(values: [string], allowed: [string]) -> Option<string>

pub fn one_of(values: Option<[string]>, allowed: [string]) -> Option<string>

pub fn size_at_least(value: string, min: int) -> Option<string>

pub fn size_at_least(value: Option<string>, min: int) -> Option<string>

pub fn size_at_most(value: string, max: int) -> Option<string>

pub fn size_at_most(value: Option<string>, max: int) -> Option<string>

pub fn size_between(value: string, min: int, max: int) -> Option<string>

pub fn size_between(value: Option<string>, min: int, max: int) -> Option<string>

pub fn at_least(value: int, bound: int) -> Option<string>

pub fn at_least(value: Option<int>, bound: int) -> Option<string>

pub fn at_most(value: int, bound: int) -> Option<string>

pub fn at_most(value: Option<int>, bound: int) -> Option<string>

pub fn positive(value: int) -> Option<string>

pub fn positive(value: Option<int>) -> Option<string>

pub fn positive(value: float) -> Option<string>

pub fn positive(value: Option<float>) -> Option<string>

pub fn email(value: string) -> Option<string>

pub fn email(value: Option<string>) -> Option<string>

pub fn is_email(value: string) -> bool

pub fn matches(value: string, pattern: string) -> Option<string>

pub fn matches(value: Option<string>, pattern: string) -> Option<string>

```

## `widget`

A widget tree: what a window is made of, laid out, and turned into a `Scene`.

```
pub enum Direction {
    Row
    Column
}

pub struct Insets {
    left: float
    top: float
    right: float
    bottom: float
}

pub fn insets_of(left: float, top: float, right: float, bottom: float) -> Insets

pub fn insets_all(amount: float) -> Insets

pub fn no_insets() -> Insets

pub struct Size {
    width: float
    height: float
}

pub fn size_of(width: float, height: float) -> Size

pub struct Label {
    text: string
    // The em size, in device units.
    size: float
    colour: Color
    // Which face to draw it in: an index into the faces a window gave `widget.Metrics` and
    // `raster.Options.fonts`, 0 being the text face.
    //
    // **An index rather than a name, because the layer below uses one** -- `scene.text_of` takes
    // an `int` and `raster.Options.fonts` is positional -- and because a name here would need a
    // table at paint time that only the application can fill. Meaning is added one layer up:
    // `controls.Style` names the faces it uses by purpose, so no control writes a literal.
    font: int
    // What a focus or accessibility layer refers to this node by, or "" for none. **A name, not an
    // index**: a path changes when a sibling is inserted above it, and focus that moved because an
    // unrelated widget appeared is a bug a user notices and a test does not.
    name: string
}

pub struct Painted {
    draw: fn(geom.Rect) -> [scene.Op]
    // What a click or a hit test calls this, as every other named widget has.
    name: string
}

pub fn canvas_of(name: string, draw: fn(geom.Rect) -> [scene.Op]) -> Widget

pub struct Drawn {
    path: shape.Path
    colour: Color
    name: string
}

pub struct Pictured {
    image: int
    // The picture's own size **in points**, which is what `measure` answers and what `fit` fits.
    //
    // **Given rather than discovered.** The widget layer never sees the pixels -- the images table
    // belongs to the painter -- and a tree that had to be laid out against it could not be built
    // before one existed. A caller that decoded the image has its dimensions and its own idea of
    // how many points a pixel is; both are decisions this layer must not make.
    width: float
    height: float
    fit: Fit
    // **What a reader is told, and a picture must declare it.** The same rule a control follows: a
    // picture that gathered its name from somewhere else would be named by whatever happened to sit
    // next to it. Empty means decorative -- a border, a texture -- and a reader is told nothing
    // rather than told something useless.
    alt: string
    name: string
}

pub enum Fit {
    // Its own size, centred in the box. What a thumbnail of a known size wants.
    Natural
    // The largest it can be inside the box with its aspect kept, centred -- the common case for a
    // photograph in a panel whose size the window decides.
    Contain
    // `Contain`, but **never larger than its own size** -- CSS's `scale-down`, and the fit a
    // *preview* wants. The difference shows on small pictures: a 16-point favicon in a 600-point
    // pane is a blurry wall under `Contain` and a 16-point favicon under this, which is what it
    // is. Found by showing an image response in curly, where `Contain` enlarged everything small.
    Shrink
    // The box exactly, aspect ignored. Right for a texture or a gradient and wrong for a face.
    Stretch
}

pub fn picture_box(p: Pictured, within: Rect) -> Rect

pub fn picture_of(image: int, width: float, height: float, alt: string) -> Widget

pub fn picture_fitted(image: int, width: float, height: float, alt: string) -> Widget

pub fn picture_shrunk(image: int, width: float, height: float, alt: string) -> Widget

pub enum IconSource {
    // A path in a **unit box**, from (0,0) to (1,1), scaled into whatever square the control
    // gives it. `shape` has everything needed to write one and `scene.fill` draws it, so this
    // source costs no new native code and no registry: see `Drawn`.
    Drawn(shape.Path)
    // One character from a face -- an icon font's private-use code point, or a character from the
    // text font where that is what the mark is.
    //
    // **This was blocked until the face stopped being a literal 0.** The whole of the blockage
    // was `paint` passing `0` to `scene.text_of`, so a `Label` could not name the second face a
    // window had loaded; the renderer has taken an index since it existed. `Style.icon_font` says
    // which face, so the control carries the character and the theme carries the font.
    //
    // **The glyph is a string, not a code point**: a caller that has `"\u{e5ca}"` in a constant
    // can put it here, and this layer never has to encode anything.
    //
    // An icon font is asked for by name, so a missing glyph is the caller's to notice -- unlike
    // the disclosure arrows, which were strings in `Style` until a face without them drew `.notdef`
    // in every combo box, and are a path now. See `controls.chevron`. It is one character by convention and nothing enforces that, because an
    // icon font with a two-character ligature is a real thing.
    Glyph(string)
}

pub enum IconSide {
    Leading
    Trailing
}

pub struct Icon {
    source: IconSource
    name: string
    side: IconSide
}

pub fn icon_of(path: shape.Path, name: string) -> Icon

pub fn trailing_icon(path: shape.Path, name: string) -> Icon

pub fn icon_widget(icon: Icon, size: float, colour: Color, face: int) -> Widget

pub fn glyph_icon(glyph: string, name: string) -> Icon

pub fn trailing_glyph_icon(glyph: string, name: string) -> Icon

pub fn drawn_of(path: shape.Path, colour: Color) -> Widget

pub fn named_drawing(path: shape.Path, colour: Color, name: string) -> Widget

pub fn label_of(text: string, size: float, colour: Color) -> Widget

pub fn label_in_face(text: string, size: float, colour: Color, face: int) -> Widget

pub fn named_label(text: string, size: float, colour: Color, name: string) -> Widget

pub struct Border {
    colour: Color
    width: float
}

pub struct Shadow {
    // How far down it falls. **Down and not sideways**, which is the one thing every system agrees
    // on: the light is above, so a shadow that moved horizontally would say the window is lit from
    // a lamp the user cannot see.
    offset: float
    // How far it spreads past the box.
    blur: float
    // How dark it is at the box's own edge, where it is darkest.
    opacity: float
}

pub val shadow_steps = 8

pub fn shadow_of(offset: float, blur: float, opacity: float) -> Shadow

pub struct Surface {
    // How far **inside** its node's bounds this surface is drawn, on every side.
    //
    // **Paint time only, and that is an invariant rather than a note.** Nothing in layout and
    // nothing in a hit test reads this: a control reserves a ring of room in its padding, draws
    // its resting frame inset by the whole ring, and spends that inset when a state wants a bigger
    // frame. So the drawn frame grows and **the box never does** -- which is what keeps a control
    // from being hovered at a pixel its layout never claimed, and two neighbours from both
    // claiming the boundary between them. egui concedes the wart in its own documentation, saying
    // its `expansion` "makes hovered things misaligned"; it gets away with it by laying out every
    // frame, which this does not.
    //
    // Zero is the whole surface, which is what every container wants.
    inset: float
    // Painted behind the children, before them. `None` draws nothing, which is what most
    // containers want -- a stack per level each painting a fill is how a UI ends up with twenty
    // overdraws per pixel.
    // **A paint, not a colour, since F295.** A look can name a span of its own ramp as a face
    // (`theme.Fill`), and `from == to` is a flat fill -- so solid and gradient are one form with no
    // `Option` and no second field. A gradient's endpoints are *not* carried here: they are
    // resolved against the box in `paint_surface`, the way `Radius` is, because the tree is built
    // before anything knows how big a control turned out to be.
    fill: Option<Paint>
    // Stroked **inside** the container's own bounds, which is the bargain `paint_focused`'s focus
    // ring already makes and for the same reason: a border outside the box overlaps whatever is
    // next to it, and in a dense form that is the neighbouring control. So a border costs **no
    // layout** -- the application's padding is what leaves room for it -- and that is what keeps a
    // second look and feel from moving the frame cost F284 measured, where building and laying out
    // already dominate painting by six to one.
    border: Option<Border>
    // Rounds the fill and the border alike. `Radius.Square` is the common case.
    //
    // **One radius for all four corners**, with `corners` saying which of them it applies to.
    // Four independent radii is what a design tool offers and what no application here has wanted:
    // the asymmetric box that does occur is a tab, and a tab wants one radius on two corners.
    corner: Radius
    // Which corners `corner` rounds. `Corners.All` for everything that is not a tab.
    corners: Corners
    // What falls behind it, if anything. `None` for every surface that is not in front of
    // something, which is nearly all of them.
    shadow: Option<Shadow>
}

pub enum Corners {
    All
    Top
    Bottom
}

pub enum Radius {
    Square
    Points(float)
    Fraction(float)
}

pub fn paint_in(p: Paint, box: geom.Rect) -> Paint

pub fn radius_in(r: Radius, box: geom.Rect) -> float

pub fn no_surface() -> Surface

pub fn filled(colour: Color) -> Surface

pub fn border_of(colour: Color, width: float) -> Border

pub fn rounded(fill: Color, corner_radius: float) -> Surface

pub fn rounded(fill: Color, corner: Radius) -> Surface

pub fn outlined(border: Color, width: float, corner_radius: float) -> Surface

pub fn outlined(border: Color, width: float, corner: Radius) -> Surface

pub fn edged(fill: Color, border: Color, width: float, corner_radius: float) -> Surface

pub fn edged(fill: Color, border: Color, width: float, corner: Radius) -> Surface

pub enum Across {
    Stretch
    Start
    Centre
    End
}

pub enum Along {
    Start
    Centre
    End
}

pub struct Alignment {
    across: Across
    along: Along
}

pub fn stretched() -> Alignment

pub fn centred() -> Alignment

pub struct Stack {
    direction: Direction
    spacing: float
    padding: Insets
    surface: Surface
    align: Alignment
    children: [Widget]
    name: string
}

pub fn row_of(spacing: float, children: [Widget]) -> Widget

pub fn column_of(spacing: float, children: [Widget]) -> Widget

pub fn padded_of(padding: Insets, child: Widget) -> Widget

pub struct Fixed {
    width: float
    height: float
    // Which of the two numbers above actually constrains. A `false` means **ask the child**, which
    // is what `wide_of` and `tall_of` are for.
    //
    // **This exists because `fixed_of(w, 0.0, child)` means a height of zero**, and it was being
    // written where "no opinion about the height" was meant. It worked only while a parent
    // stretched the box back out -- `widget.row_of` does, `Across.Centre` does not -- so the same
    // expression drew correctly in one container and vanished in another. That has cost nine
    // separate defects in this library: the table's header band, a link, the slider's thumb and
    // its track, a star, a popup's rows, a check box's mark, a demo band, and the tab strip, which
    // measured zero the moment the design file that happened to be forcing its height stopped.
    // Each presents identically -- nothing drawn, no error, every test green.
    fixes_width: bool
    fixes_height: bool
    child: Widget
    name: string
}

pub fn fixed_of(width: float, height: float, child: Widget) -> Widget

pub fn wide_of(width: float, child: Widget) -> Widget

pub fn tall_of(height: float, child: Widget) -> Widget

pub struct Expanded {
    weight: float
    child: Widget
}

pub fn expand_of(weight: float, child: Widget) -> Widget

pub fn expand(child: Widget) -> Widget

pub struct Density {
    px_per_pt: float
}

pub enum Source {
    UserSetting
    Display
    Default
}

pub struct Resolved {
    density: Density
    from: Source
}

pub fn resolve_density(user: Option<float>, display: Option<float>) -> Resolved

pub fn density_of(px_per_pt: float) -> Density

pub fn to_px(d: Density, pt: float) -> float

pub fn to_pt(d: Density, px: float) -> float

pub fn point_in_pt(d: Density, x: float, y: float) -> Point

pub struct Layer {
    at: Option<Rect>
    // Whether the keyboard belongs to this layer alone while it is present.
    takes_keys: bool
    content: Widget
}

pub struct Stacked {
    layers: [Layer]
    name: string
}

pub fn base_layer(content: Widget) -> Layer

pub fn modal_layer(content: Widget) -> Layer

pub fn layer_at(where_: Rect, takes_keys: bool, content: Widget) -> Layer

pub fn layered_of(layers: [Layer]) -> Widget

pub fn keyboard_children(l: Laid) -> [Laid]

pub struct Viewport {
    offset_x: float
    offset_y: float
    child: Widget
    name: string
}

pub fn scroll_of(offset_y: float, child: Widget) -> Widget

pub fn named_scroll(name: string, offset_x: float, offset_y: float, child: Widget) -> Widget

pub fn scroll_to_reveal(view_extent: float, offset: float, target_start: float, target_extent: float) -> float

pub fn clamp_offset(offset: float, view_extent: float, content_extent: float) -> float

pub enum Cursor {
    // The ordinary pointer, and the one anything that does not say otherwise gets.
    Arrow
    // Over something that resizes left and right: a column boundary, an upright divider.
    ResizeAcross
    // Over something that resizes up and down: a row boundary, a flat divider.
    ResizeDown
    // Over text that can be selected.
    Text
    // Over something that will be followed.
    Hand
}

pub enum Role {
    Button
    CheckBox
    RadioButton
    TextField
    // A field whose contents must not be shown or read out. **A role rather than a flag on
    // `TextField`**, because every layer below has to treat it differently -- a reader announces
    // it so a user knows not to say what they type aloud, and the platform bridge uses the role to
    // decide that this node's text is not for speaking.
    Password
    Slider
    Link
    List
    ListItem
    // A tree and its rows. `State.expanded` has existed since accessibility was built and nothing
    // declared these two, which is the same gap `Tab` and `TabList` were in before F280.
    Tree
    TreeItem
    Tab
    TabList
    Menu
    MenuItem
    // **A value picker, which is not a menu.** A menu offers commands and a combo box offers
    // values, and a reader says different things about them: "File, menu" invites you to look for
    // an action, "Environment, combo box, staging" tells you what is currently chosen. The two are
    // built from the same popup here and announce differently on purpose.
    ComboBox
    ScrollBar
    // A grid of cells with a header over it. **Its rows are nodes and its cells are not**, which is
    // where this stops short of AT-SPI's table interface: that interface answers questions by
    // coordinate ("what is in row 3, column 2"), which needs a C object implementing `AtkTable`
    // rather than a role on a widget. A row whose label is its cells read in order gets a reader
    // through a table usefully today; the coordinate interface is the next instalment, not a
    // different design.
    Table
    // One row of a table, announced with its cells in order.
    TableRow
    // The heading over a column, which is a control: it is what a column is sorted and resized by.
    ColumnHeader
    // A switch is not a check box, and the distinction is the user's rather than the drawing's: a
    // check box is a choice that is committed with the rest of a form, and a switch takes effect
    // the moment it moves. Readers say "on" and "off" for one and "checked" for the other, which
    // is the whole reason this is its own role rather than a check box that looks different.
    Switch
    // **A bar a user cannot operate**, which is the thing that makes it unlike every other role
    // here: there is nothing to press, so it is not a tab stop -- and it still has to be in the
    // tree, because "is it still going?" is exactly what a reader has to be able to answer.
    ProgressBar
    // A field with steppers. It is an editable text field as well -- which is why the role exists
    // rather than the control borrowing `TextField`: a reader announces a range and a step for a
    // spin button and nothing of the kind for a field, and the range is the thing a user most
    // needs told before typing into it.
    SpinButton
    // A strip of commands, which is **one** stop in the tab order with its own arrow keys --
    // the reason it is a role and not a row of buttons. Every platform's reader announces a
    // toolbar so that a user knows the arrows will work here and Tab will leave.
    ToolBar
    // A field holding a date, and the month grid that writes one into it. **Two roles, not
    // one**, because they are two things a reader meets separately: the field is where the
    // value is and the calendar is a popup that opens over the window, and a user tabbing
    // through a form hears the first without ever meeting the second.
    DateEditor
    Calendar
    // Somewhere the application says something the user did not go and look for: a result count
    // that changed under their typing, a panel that finished loading, a save that failed.
    //
    // **The politeness is the role, which is ARIA's own answer.** `role="status"` *is* polite and
    // `role="alert"` *is* assertive, so a separate field would be a second way to say a thing the
    // role already says -- and a node whose role and politeness disagreed would have no meaning.
    //
    // **A reader is a cursor**, and everything else here is read when the user arrives at it.
    // These two are the only way for an application to say something without the user having gone
    // anywhere. See `a11y.announcements`.
    Status
    Alert
    // Two panes and the divider between them. **The divider is a `Separator` inside this**, which
    // is how both ATK and GTK have it: the pane is what contains the two halves and publishes
    // where the split sits, and the handle is the thin thing you take hold of.
    SplitPane
    // A thin line that divides, with nothing either side of it. A split pane's divider and a
    // toolbar's rule are the same object to a reader, which is why this is one role and not two.
    Separator
    Dialog
    // A container that groups its children for a reader without being anything itself -- a toolbar,
    // a form section. Worth a node where a plain layout box is not, because a reader navigates by
    // group.
    Group
    // Static text, which is not a control. Produced by `a11y` for a `Text` outside any control, and
    // not declarable here: a label is text, and text already knows it is text.
    Label
    // A picture, which is not text. Produced by `a11y` for a `Picture`, and not declarable here
    // for the same reason: a picture knows what it is, and what a reader needs from it is the
    // alternative text it carries.
    Image
    // The window itself, which `a11y` puts at the root.
    Window
    Unknown
}

pub enum Toggle {
    Off
    On
    Mixed
}

pub struct State {
    disabled: bool
    selected: bool
    checked: Option<Toggle>
    expanded: Option<bool>
    readonly: bool
    required: bool
    invalid: bool
    multiline: bool
}

pub fn no_state() -> State

pub struct Focusable {
    name: string
    role: Role
    label: string
    description: string
    // The control's content: a field's text, a slider's reading. Not its label.
    value: string
    state: State
    // Which **part** of this control the focus ring is drawn around, or `""` for the control
    // itself.
    //
    // **A tab strip is why.** One control takes the focus and Left and Right move along it, so
    // the ring went around the whole strip -- which says every tab is focused, when one of them
    // is. ARIA's tab pattern puts the focus on the active tab for the same reason, and a reader
    // already announces the selected one; this is the pointer's half of the same fact.
    //
    // **Opt-in rather than "ring whatever part is selected"**, which would have cost nothing to
    // write and would have been wrong for the two controls that have selected parts and do not
    // want this: a list and a tree are focused *as a whole* and show their selection by
    // highlighting a row, which is what every platform does. A tab strip is the exception, so it
    // is the one that says so.
    //
    // An unknown name rings the control, which is the same forgiving rule `paint_focused` follows
    // for a name the tree no longer holds.
    focus_part: string
    // Whether **Tab** can land here, as opposed to whether this is a control at all.
    //
    // **The two are not the same thing, and a list is what proves it.** A list of two hundred rows
    // is one stop in the tab order, and the arrow keys move within it -- but every row still has to
    // be a node a screen reader can reach and announce as selected or not, and a click still has to
    // report the row it landed on rather than the list. So a row is a control for hit testing and
    // for accessibility, and not for Tab.
    //
    // This was not in the first version of this struct. Building the list found it: without the
    // distinction a row is either a tab stop (so Tab walks two hundred of them) or not a control at
    // all (so `a11y` takes every row's text as the list's own name, and a reader announces the whole
    // list as one string -- which `tests/stdlib/a11y_test.zm` already had a case against).
    tab_stop: bool
    child: Widget
}

pub fn focusable_of(name: string, child: Widget) -> Widget

pub fn control_of(name: string, role: Role, child: Widget) -> Widget

pub fn part_of(name: string, role: Role, label: string, state: State, child: Widget) -> Widget

pub fn labelled_control(name: string, role: Role, label: string, child: Widget) -> Widget

pub enum Widget {
    // Occupies nothing and paints nothing. The honest answer for "no child here", rather than an
    // `Option<Widget>` at every site that holds one.
    Nothing
    Text(Label)
    // A path scaled into its box: a mark rather than a picture. See `Drawn`.
    Drawing(Drawn)
    Canvas(Painted)
    // A decoded image drawn into its box: a picture rather than a mark. See `Pictured`.
    Picture(Pictured)
    Group(Stack)
    Sized(Fixed)
    // Flexible space. **Its weight is how leftover space is shared**, so two spacers of 1 and 3
    // split it one quarter to three quarters. A weight of zero takes nothing, which is how a
    // spacer is switched off without removing it from the tree.
    Flex(float)
    // A subtree that is flexible rather than its natural size -- see `Expanded`.
    Expand(Expanded)
    // A subtree that can take keyboard focus -- see `Focusable`. Transparent to layout: it
    // occupies exactly what its child does.
    Control(Focusable)
    // A window onto a child larger than itself -- see `Viewport`. **Not transparent to layout**,
    // which is what separates it from every other wrapper here.
    // A grid of tracks with cells placed into them: the one arrangement nesting cannot express,
    // because nesting cannot align across cells in *different rows*. See `Grid`.
    Grid(Grid)
    // Children laid along a line that wraps when it runs out. See `Flowed`.
    Flow(Flowed)
    Scroll(Viewport)
    // Children stacked over one another, later ones on top -- see `Layer`. A menu's popup and a
    // modal dialog are both this.
    Layered(Stacked)
}

pub fn flex() -> Widget

pub fn flex_of(weight: float) -> Widget

pub struct FaceMetrics {
    text_width: fn(string, float) -> float
    line_height: fn(float) -> float
    // How far below the top of a line its baseline sits.
    //
    // **Added because a baseline cannot be guessed from a line height.** Every text op this layer
    // emitted put its baseline a *whole line* below the top of its box -- which is where the next
    // line begins, not where this one sits -- so every glyph in the widget set was drawn a
    // descent too low, hanging out of the bottom of the box that was meant to centre it. A
    // button's label sat low in its face and a check box's mark low in its square, and no
    // assertion could see it: the boxes were right and only the ink was wrong. Found by someone
    // looking at the gallery.
    ascent: fn(float) -> float
}

pub struct Metrics {
    faces: [FaceMetrics]
}

pub fn metrics_of(text_width: fn(string, float) -> float, line_height: fn(float) -> float) -> Metrics

pub fn metrics_of(

pub fn metrics_of_faces(faces: [FaceMetrics]) -> Metrics

pub fn face_metrics_of(

pub fn face_metrics_of(

pub fn face_at(m: Metrics, which: int) -> FaceMetrics

pub fn width_in(m: Metrics, which: int, text: string, size: float) -> float

pub fn height_in(m: Metrics, which: int, size: float) -> float

pub fn ascent_in(m: Metrics, which: int, size: float) -> float

pub fn metrics_for(f: font.FontRef) -> Result<Metrics, string>

pub fn metrics_for_faces(fs: [font.FontRef]) -> Result<Metrics, string>

pub fn face_metrics_for(f: font.FontRef) -> Result<FaceMetrics, string>

pub struct Measured {
    size: Size
    // How far below this widget's own top its first baseline sits.
    baseline: Option<float>
}

pub fn measure(w: Widget, m: Metrics) -> Size

pub fn measured(w: Widget, m: Metrics) -> Measured

pub enum Track {
    // Exactly this many points.
    Points(float)
    // As big as the largest thing in it needs.
    Natural
    // This many shares of whatever is left over. **The same notion as `Flex`**, so the library has
    // one idea of weighted leftover rather than two.
    Share(float)
}

pub enum Fill {
    Nowhere
    Across
    Down
    Both
}

pub enum Sideways {
    Leading
    Centre
    Trailing
}

pub enum Upright {
    Top
    Middle
    Bottom
    // On the row's shared text baseline. **Falls back to `Top` when nothing in the row has one**,
    // which is why `Measured.baseline` is an `Option`: a row of icons has no line to sit on.
    Baseline
}

pub struct Anchor {
    sideways: Sideways
    upright: Upright
}

pub fn anchor_of(sideways: Sideways, upright: Upright) -> Anchor

pub enum Column {
    Next
    At(int)
}

pub enum Span {
    Rest
    Cells(int)
}

pub struct Cell {
    column: Column
    row: int
    across: Span
    down: int
    fill: Fill
    anchor: Anchor
    // Room around this child, inside its track. `column_gap` and `row_gap` are the gutters; this
    // is for the one cell that wants more.
    margin: Insets
    child: Widget
}

pub fn cell_of(row: int, child: Widget) -> Cell

pub struct Grid {
    columns: [Track]
    rows: [Track]
    column_gap: float
    row_gap: float
    cells: [Cell]
    name: string
}

pub fn cell_of(row: int, fill: Fill, anchor: Anchor, child: Widget) -> Cell

pub fn cell_at(row: int, column: int, fill: Fill, anchor: Anchor, child: Widget) -> Cell

pub fn grid_of(

pub fn grid_of(columns: [Track], rows: [Track], gap: float, cells: [Cell]) -> Widget

pub struct Flowed {
    direction: Direction
    // Between children on one line.
    gap: float
    // Between lines. **Separate from `gap`**, because they are different distances: items in a
    // row sit closer together than rows do, and one number for both makes a bar of chips look
    // either cramped or ventilated.
    line_gap: float
    // Where a line that does not fill its width sits. **Leading, centre or trailing and not left
    // or right** -- `FlowLayout`'s own constants went the same way in Java 1.4, for the Arabic and
    // Hebrew reason this library is heading towards.
    align: Sideways
    // Where a child shorter than its line sits within it.
    across: Upright
    children: [Widget]
    name: string
}

pub fn flow_of(gap: float, line_gap: float, children: [Widget]) -> Widget

pub fn border_of(

pub fn is_nothing_widget(w: Widget) -> bool

pub struct Laid {
    bounds: Rect
    // Which child of its parent this is, and -1 for the root. Joined with its ancestors' it is
    // the position in the tree -- "0", "0/1", "0/1/2" -- which is what an accessibility tree keys
    // a node by when no `name` was given.
    //
    // **An int, where this used to be that whole path as a string.** `a11y` is the only reader
    // there has ever been, and the string was *built for every node of every layout*: 9.7% of a
    // layout pass went to `join_path` and its `concat`, besides the garbage two allocations per
    // node make. `a11y` joins the indices as it descends instead, which reproduces the same paths
    // exactly -- every walk it does descends through real children, so the indices it meets are
    // the ones layout assigned.
    index: int
    // The widget's own `name`, or the **nearest named ancestor's**, or "".
    //
    // Inherited rather than per node, and that is what makes a click on a button's *label* report
    // the button: a `Control` wraps a group that wraps a label, so without inheritance the only
    // node carrying the name would be the outermost one, and the two or three nodes a pointer
    // actually lands on would all answer "". A descendant with a name of its own overrides it, so
    // a control nested inside a named panel still answers for itself.
    name: string
    widget: Widget
    children: [Laid]
}

pub fn layout(w: Widget, within: Rect, m: Metrics) -> Laid

pub fn at(l: Laid, p: Point) -> Option<Laid>

pub fn node_named(l: Laid, name: string) -> Option<Laid>

pub fn measured_as_nothing(l: Laid) -> [Laid]

pub fn chain_at(l: Laid, p: Point) -> [Laid]

pub fn contains(r: Rect, p: Point) -> bool

pub fn role_of(l: Laid) -> Role

pub fn text_of(l: Laid) -> string

pub fn tip_in(l: Laid) -> string

pub fn is_tab_stop(l: Laid) -> bool

pub fn is_control(l: Laid) -> bool

pub val caret_name = "caret"

pub fn paint(l: Laid, m: Metrics) -> scene.Scene

pub fn paint_focused(l: Laid, m: Metrics, focused: string, ring: Color, width: float) -> scene.Scene

pub fn paint_ops(l: Laid, m: Metrics) -> [Op]

pub fn box_path(c: Corners, bounds: Rect, radius: float) -> shape.Path

```

## `window`

A window, the pixels in it, and what the user did -- the platform floor under a graphical application.

```
pub enum Preference {
    NoPreference
    Dark
    Light
}

pub enum Event {
    KeyDown(KeyPress)
    KeyUp(KeyPress)
    // One character of text the server composed, as a string, which is **one code point and not
    // one byte**. It was a byte until 2026-11-13, and that made it ASCII only: the native sends a
    // code point, this read it as a byte, and anything above 127 failed the UTF-8 check on the way
    // in and was dropped. Typing an accented letter did nothing at all, silently.
    Char(string)
    PointerDown(PointerPress)
    PointerUp(PointerPress)
    PointerMove(Position)
    Scroll(ScrollStep)
    Resize(Size)
    // The user asked to close the window -- the close box, or the window manager. **It is a
    // request, not a fact**: nothing has closed, and a program that ignores it keeps running, which
    // is what makes "save before quitting?" possible.
    Close
    // The desktop changed what appearance it asks for.
    //
    // **Delivered like a resize, and followed for the same reason.** GNOME's automatic dark mode
    // switches at *sunset*, so a program that read the preference once at startup goes stale in the
    // middle of an ordinary session and reads as ignoring the setting. A window that is open when
    // the sun goes down gets this.
    SchemeChanged(Preference)
    FocusIn
    FocusOut
    // The server wants a region drawn again. A program that repaints every frame can ignore it; one
    // that paints only on change must not.
    Exposed
}

pub struct KeyPress {
    // The X11 keysym. A number rather than an enum of every key there is: `key_name` names the ones
    // worth naming and the rest keep their value, which is the same choice `terminal.Key.Unknown`
    // makes and for the same reason -- a program that logs an unknown key is how the gap gets
    // closed.
    keysym: int
    // The hardware keycode, which is the number the keyboard sent rather than the meaning a layout
    // gave it. Nothing in this module uses it and an application almost never should -- `keysym` is
    // what a shortcut compares against. It is here because an **input method is defined to take
    // both** (`ibus.key`), and a layout-sensitive method handed a fabricated zero would be a
    // silently wrong answer rather than a missing one.
    keycode: int
    shift: bool
    control: bool
    alt: bool
    // The "logo"/"command" key. Named for what it does rather than for one platform's label.
    meta: bool
}

pub struct Position {
    x: int
    y: int
}

pub struct PointerPress {
    x: int
    y: int
    // 1 left, 2 middle, 3 right. The wheel is never a button here: X11 reports it as buttons 4 and
    // 5 and the native turns those into `Scroll`, so this module's callers never learn that.
    button: int
}

pub struct ScrollStep {
    x: int
    y: int
    horizontal: int
    vertical: int
}

pub struct Size {
    width: int
    height: int
}

pub struct Window {
    handle: int
}

pub fn color_scheme() -> Preference

pub fn display_scale() -> Option<float>

pub fn with_window<T>(

pub fn present(

pub struct Region {
    x: int
    y: int
    width: int
    height: int
}

pub fn present_region(

pub fn present_bytes(w: Window, pixels: [byte], width: int, height: int) -> Result<unit, string>

pub fn size(w: Window) -> Result<Size, string>

pub fn set_cursor_code(w: Window, kind: int) -> Result<unit, string>

pub fn set_icon(w: Window, pixels: [byte], width: int, height: int) -> Result<unit, string>

pub fn id(w: Window) -> Result<int, string>

pub fn origin(w: Window) -> Result<Position, string>

pub fn poll(w: Window) -> Result<[Event], string>

pub fn decode_events(raw: [int]) -> [Event]

pub fn key_name(k: KeyPress) -> string

pub fn put_clipboard(w: Window, text: string) -> Result<unit, string>

pub fn clipboard(w: Window) -> Result<string, string>

```

## `xml`

**Experimental** (see docs/design/journal/ "Experimental modules"). XML 1.0 as a document tree: a reader, a writer, and a small path finder.

```
pub struct Name {
    namespace: Option<string>
    local: string
    prefix: Option<string>
}

pub struct Attribute {
    name: Name
    value: string
}

pub struct Instruction {
    target: string
    data: string
}

pub enum Node {
    Element(Element)
    Text(string)
    CData(string)
    Comment(string)
    ProcessingInstruction(Instruction)
}

pub struct Element {
    name: Name
    attributes: [Attribute]
    children: [Node]
}

pub struct Declaration {
    version: string
    encoding: Option<string>
    standalone: Option<bool>
}

pub struct Document {
    declaration: Option<Declaration>
    prolog: [Node]
    root: Element
    epilog: [Node]
}

pub struct XmlLimits {
    max_bytes: int
    max_depth: int
    max_attributes: int
}

pub fn xml_limits() -> XmlLimits

pub fn name(local: string) -> Name

pub fn ns_name(uri: string, prefix: string, local: string) -> Name

pub fn element(n: Name, attributes: [Attribute], children: [Node]) -> Element

pub fn empty(local: string) -> Element

pub fn text_element(local: string, content: string) -> Element

pub fn opt_element(local: string, value: Option<string>) -> Option<Node>

pub fn req_element(local: string, value: string) -> Option<Node>

pub fn attribute(local: string, value: string) -> Attribute

pub fn document(root: Element) -> Document

pub fn same_name(a: Name, b: Name) -> bool

pub fn attr(e: Element, local: string) -> Option<string>

pub fn attr_ns(e: Element, uri: string, local: string) -> Option<string>

pub fn attr_or(e: Element, local: string, fallback: string) -> string

pub fn children(e: Element) -> [Element]

pub fn children_named(e: Element, local: string) -> [Element]

pub fn child(e: Element, local: string) -> Option<Element>

pub fn text_of(e: Element) -> string

pub fn parse_bool(text: string) -> Result<bool, string>

pub fn find(e: Element, path: string) -> [Element]

pub fn find_ns(e: Element, uri: string, path: string) -> [Element]

pub fn find_first(e: Element, path: string) -> Option<Element>

pub enum Event {
    // The `<?xml ..?>` declaration, if the document has one. First, and at most once.
    Declaration(Declaration)
    Start(StartTag)
    End(Name)
    Text(string)
    CData(string)
    Comment(string)
    ProcessingInstruction(Instruction)
}

pub struct StartTag {
    name: Name
    attributes: [Attribute]
}

pub struct Reader {
    cs: io.Cursor
    limits: XmlLimits
    // The set the bytes are being read in, so that a declaration naming one can be **checked**
    // against it rather than guessed at -- see `read_declaration`.
    set: charset.Charset
    pos: Mutable<int>
    stack: Mutable<[Frame]>
    // The `End` owed for a self-closing tag, which is reported as two events.
    owed: Mutable<Option<Name>>
    started: Mutable<bool>
    seen_root: Mutable<bool>
    done: Mutable<bool>
}

pub fn default_charset() -> charset.Charset

pub fn reader(source: Iter<Result<[byte], io.IoError>>, limits: XmlLimits) -> Reader

pub fn reader(

pub fn events(

pub fn events(source: Iter<Result<[byte], io.IoError>>, limits: XmlLimits) -> Iter<Result<Event, string>>

pub fn events(source: Iter<Result<[byte], io.IoError>>) -> Iter<Result<Event, string>>

pub fn next_event(r: Reader) -> Option<Result<Event, string>>

pub fn parse(text: string) -> Result<Document, string>

pub fn parse(text: string, limits: XmlLimits) -> Result<Document, string>

pub fn elements(source: Iter<Result<[byte], io.IoError>>, limits: XmlLimits) -> Iter<Result<Element, string>>

pub fn elements(source: Iter<Result<[byte], io.IoError>>) -> Iter<Result<Element, string>>

pub fn parse(data: [byte]) -> Result<Document, string>

pub fn parse(data: [byte], limits: XmlLimits) -> Result<Document, string>

pub fn format(doc: Document) -> Result<string, string>

pub fn format(doc: Document, indent: string) -> Result<string, string>

pub fn xml_to_bytes(doc: Document) -> Result<[byte], string>

pub fn format(e: Element) -> Result<string, string>

```

## `yaml`

**Experimental** (see docs/design/journal/ "Experimental modules"). A strict YAML 1.2 subset on top of `json`: a document is a `Json` tree, so any `#[derive(Json)]` type decodes from it with its own `_from_json`.

```
pub fn default_charset() -> charset.Charset

pub fn parse(data: [byte], set: charset.Charset) -> Result<Json, string>

pub fn parse(data: [byte]) -> Result<Json, string>

pub fn parse(text: string) -> Result<Json, string>

pub fn format(j: Json) -> string

```

## `zone`

The IANA time zone database, read from the operating system's compiled zone files (`/usr/share/zoneinfo/America/New_York` and so on, in the TZif format), for `time`'s named zones.

```
pub struct ZoneType {
    utoff: int
    dst: bool
    name: string
}

pub struct RuleDate {
    kind: int
    month: int
    week: int
    weekday: int
    day: int
    seconds: int
}

pub struct PosixRule {
    standard: ZoneType
    daylight: ZoneType
    has_dst: bool
    start: RuleDate
    end: RuleDate
}

pub struct ZoneRules {
    transitions: [int]
    type_of: [int]
    utoffs: [int]
    dsts: [bool]
    names: [string]
    has_rule: bool
    rule: PosixRule
}

pub fn parse_posix_rule(text: string) -> Result<PosixRule, string>

pub val no_rule = PosixRule {

pub fn parse_rules(data: [byte]) -> Result<ZoneRules, string>

pub fn load_rules(name: string) -> Result<ZoneRules, string>

pub fn type_at(rules: ZoneRules, t: int) -> ZoneType

```
