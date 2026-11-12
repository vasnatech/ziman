# `edit`

A text field's state and every edit you can make to it: a caret, a selection, and the moves.

docs/reviews/tasks/f269-f277.md F273. **Pure values and pure functions**, so every behaviour a
text field has is checkable without a window, a font or a keyboard -- which matters more here
than anywhere else in `lib/ui`, because a text field that is subtly wrong is worse than no text
field, and "subtly wrong" is exactly what a screenshot cannot show.

**Positions are code-point indices**, matching `chars`, `char_at` and `substring`, which is what
the language gives. They are *not* byte offsets and *not* grapheme indices: a caret moves by
grapheme (see `cluster_before`), but it is stored as the code-point index that movement lands on,
so a caret can never sit inside a multi-byte character and `substring` can always be used
directly.

**What is here and what is not.** The editing model is complete: insert, delete, move by
character, word and line, select, and undo. The **system clipboard is not**, and that is a
platform job rather than a missing function here -- owning a selection on X11 means answering
`SelectionRequest` events with a list of targets and an INCR protocol for large transfers, which
belongs beside `src/native_window.c`. `cut` and `copy` answer the text, and a caller hands it
wherever it keeps it; `paste` takes text back.
A field's whole state.

`caret` is where the next character goes. `anchor` is the other end of the selection: equal to
`caret` when nothing is selected, which is why there is no separate `has_selection` flag to keep
in step with it.

`use edit` -- or `use edit.Name` to bring one item in bare.

## Example

```
use edit

fn show(label: string, e: edit.Editor) {
    printf("{}: {} caret={} selected={}\n", label, e.text, str(e.caret), edit.selected_text(e))
}

fn main() {
    // A field starts with the caret at the end, as one does after being given a value to edit.
    var e = edit.editor_of("hello world")
    show("start", e)

    // Ctrl-Left twice, then select the word: positions are **code-point indices**, which is what
    // `substring` and `char_at` take, so a caret can never land inside a multi-byte character.
    e = edit.move_word_left(e, false)
    show("word left", e)
    e = edit.select_word_at(e, e.caret)
    show("word selected", e)

    // Typing replaces a selection, which is the behaviour a user expects and the reason `insert`
    // handles both cases rather than having a separate `replace`.
    e = edit.insert(e, "there")
    show("typed over it", e)

    // **A caret moves by grapheme, not by code point.** "e" followed by a combining acute is two
    // code points and one character on screen, and a backspace has to take both -- otherwise the
    // user is left with a floating accent they cannot delete.
    var accented = edit.editor_of("café")
    printf("code points in the accented word: {}\n", str(len(accented.text)))
    accented = edit.delete_back(accented)
    printf("after one backspace:  {}\n", accented.text)

    // Punctuation is its own run, which is why a double click on `foo` in `foo.bar` takes `foo`
    // and not the whole thing.
    val dotted = edit.select_word_at(edit.editor_of("foo.bar"), 1)
    printf("double click in foo.bar: {}\n", edit.selected_text(dotted))

    // Undo keeps **states**, not inverse operations: a field's whole state is a string and two
    // integers, and a stack of those cannot disagree with the document.
    val before = edit.editor_of("one")
    val history = edit.record(edit.history_of(50), before)
    val after = edit.insert(before, " two")
    val back = edit.undo(history, after)
    printf("after undo: {} (redo available: {})\n", back.editor.text, str(edit.can_redo(back.history)))

    // **The system clipboard is not here.** `copy` and `cut` answer the text and a caller holds
    // it; owning a real selection means answering the window system's requests, which belongs
    // beside the window natives rather than inside an editor.
    val selected = edit.Editor { text: "abcdef", caret: 4, anchor: 1 }
    printf("copy answers: {}\n", edit.copy(selected))
    printf("cut leaves:   {}\n", edit.cut(selected).text)
}
```

Output:

```
start: hello world caret=11 selected=
word left: hello world caret=6 selected=
word selected: hello world caret=11 selected=world
typed over it: hello there caret=11 selected=
code points in the accented word: 5
after one backspace:  caf
double click in foo.bar: foo
after undo: one (redo available: true)
copy answers: bcd
cut leaves:   aef
```

### `Editor`

```
pub struct Editor {
    text: string
    caret: int
    anchor: int
}
```

### `editor_of`

```
pub fn editor_of(text: string) -> Editor
```

### `empty_editor`

```
pub fn empty_editor() -> Editor
```

### `selection_start`

```
pub fn selection_start(e: Editor) -> int
```

Where the selection starts and ends, low first. Equal when there is no selection.

### `selection_end`

```
pub fn selection_end(e: Editor) -> int
```

### `has_selection`

```
pub fn has_selection(e: Editor) -> bool
```

### `selected_text`

```
pub fn selected_text(e: Editor) -> string
```

## Grapheme clusters

### `cluster_before`

```
pub fn cluster_before(text: string, at: int) -> int
```

Where a backspace at `at` should delete back to: the start of the grapheme cluster that ends
there.

**This is a deliberate subset of UAX #29, not an implementation of it**, and the difference is
worth stating because the alternative was moving by code point and calling it done. Four rules,
chosen because they cover what a user actually types:


```
* a CRLF is one cluster, so a caret never lands between the two halves of a line break;
* a combining mark joins the character before it, so `e` followed by U+0301 deletes as one
  `é` rather than leaving a floating accent;
* a zero-width joiner joins what is on either side of it, which is how emoji are composed;
* two regional indicators are one flag.
```

What it gets wrong: the mark ranges below are the main Unicode blocks of combining marks rather
than the full `Grapheme_Extend` property, so a rare mark outside them is treated as its own
cluster -- the caret stops in a place a user would not expect, which is a visible but recoverable
wrongness. Hangul syllable sequences and Indic conjuncts are also not handled. Doing this
properly needs character property tables the language does not have yet, and a half-table that
claimed to be UAX #29 would be worse than a rule that says what it is.

### `cluster_after`

```
pub fn cluster_after(text: string, at: int) -> int
```

Where a forward delete at `at` should delete up to: the end of the cluster that starts there.

## Moving

### `move_left`

```
pub fn move_left(e: Editor, extend: bool) -> Editor
```

**An unextended left press with a selection goes to the selection's start, not one before the
caret.** Every editor behaves this way and it is easy to get wrong: a user who has selected a
word and presses Left expects the caret at its beginning, not inside it.

### `move_right`

```
pub fn move_right(e: Editor, extend: bool) -> Editor
```

## Lines

### `line_start`

```
pub fn line_start(text: string, at: int) -> int
```

The index just after the newline before `at`, or 0.

### `line_end`

```
pub fn line_end(text: string, at: int) -> int
```

The index of the newline at or after `at`, or the length.

### `move_home`

```
pub fn move_home(e: Editor, extend: bool) -> Editor
```

**Home and End are per line**, which is what they mean in every editor. For a single-line field
they answer 0 and the length exactly as they always did.

### `move_end`

```
pub fn move_end(e: Editor, extend: bool) -> Editor
```

### `move_up`

```
pub fn move_up(e: Editor, extend: bool) -> Editor
```

The caret one line up or down, keeping its column where the line is long enough.

**The column is counted in characters, not measured in pixels**, which is the one simplifying
choice here and is worth naming: in a proportional face, moving down a line and back up can land
a column or two from where it started, where a pixel-measured caret would return exactly. Doing
it properly needs the metrics threaded through every movement, and a text area is most often used
for code and data -- the things people put in a request body -- where the difference is small.
`controls.caret_at_x` already measures when a *click* places the caret, so the pixel answer is
there when this is worth revisiting.

### `move_down`

```
pub fn move_down(e: Editor, extend: bool) -> Editor
```

### `line_index`

```
pub fn line_index(text: string, at: int) -> int
```

Which line `at` falls on, counting from 0 -- what a widget needs to draw a caret.

### `select_all`

```
pub fn select_all(e: Editor) -> Editor
```

### `place_caret`

```
pub fn place_caret(e: Editor, at: int, extend: bool) -> Editor
```

The caret moved to an index, as a click does. `extend` is a shift-click, which keeps the anchor.

## Words

### `word_before`

```
pub fn word_before(text: string, at: int) -> int
```

The start of the word at or before `at`: Ctrl-Left.

### `word_after`

```
pub fn word_after(text: string, at: int) -> int
```

The end of the word at or after `at`: Ctrl-Right.

### `move_word_left`

```
pub fn move_word_left(e: Editor, extend: bool) -> Editor
```

### `move_word_right`

```
pub fn move_word_right(e: Editor, extend: bool) -> Editor
```

### `select_word_at`

```
pub fn select_word_at(e: Editor, at: int) -> Editor
```

The word under an index, selected: a double click.

Selects the run the index is *in*, including a run of spaces -- double-clicking the gap between
two words selects the gap, which is what a user who then types expects to replace.

## Changing the text

### `insert`

```
pub fn insert(e: Editor, what: string) -> Editor
```

Replaces the selection with `what`, or inserts at the caret when there is none.

### `delete_back`

```
pub fn delete_back(e: Editor) -> Editor
```

Backspace: the selection if there is one, otherwise the grapheme before the caret.

### `delete_forward`

```
pub fn delete_forward(e: Editor) -> Editor
```

Delete: the selection if there is one, otherwise the grapheme after the caret.

### `delete_word_back`

```
pub fn delete_word_back(e: Editor) -> Editor
```

## The clipboard, as far as this goes

### `copy`

```
pub fn copy(e: Editor) -> string
```

What a copy would put on the clipboard: the selection, or "" when there is none.

**This does not touch the system clipboard** -- see the module header. A caller holds the text
and hands it back to `paste`, and a platform layer that owns a real selection plugs in at that
seam rather than inside the editor.

### `cut`

```
pub fn cut(e: Editor) -> Editor
```

Cut answers the text and leaves the editor without it. Two functions rather than one returning
both, because the language has no tuples and a struct for this would be read once and never
again: `copy(e)` then `cut(e)` is the pair, and `cut` alone is the common case.

### `paste`

```
pub fn paste(e: Editor, what: string) -> Editor
```

## Undo

### `History`

```
pub struct History {
    past: [Editor]
    future: [Editor]
    limit: int
}
```

A stack of states, newest last, with the redo side kept separately.

**States, not operations.** An undo stack of inverse operations is smaller and is the wrong
trade here: a text field's whole state is a string and two integers, and a stack of them cannot
disagree with the document, which is the bug every operation-based undo eventually has.

### `history_of`

```
pub fn history_of(limit: int) -> History
```

### `record`

```
pub fn record(h: History, before: Editor) -> History
```

Records `before` as a state to come back to. Called with the state *before* a change, by whatever
applies the change.

**Recording clears the redo side**, which is what every editor does: once you have typed
something new, the future you undid is not reachable any more and offering it would re-apply an
edit to a document it no longer fits.

### `can_undo`

```
pub fn can_undo(h: History) -> bool
```

### `can_redo`

```
pub fn can_redo(h: History) -> bool
```

### `Step`

```
pub struct Step {
    editor: Editor
    history: History
}
```

The state to go back to, and the history with that step moved to the redo side. `current` is
where the editor is now, which is what becomes redoable.

### `undo`

```
pub fn undo(h: History, current: Editor) -> Step
```

### `redo`

```
pub fn redo(h: History, current: Editor) -> Step
```

## Composing with an input method

### `Preedit`

```
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
```

Text being composed and not yet committed.

docs/reviews/tasks/f269-f277.md F274, first instalment: the model, which every input method
protocol agrees on even though none of them agrees on anything else. XIM, ibus, SDL3, Windows
TSF and macOS all deliver the same two things -- a pre-edit string with a caret and a
highlighted run inside it, and a commit string that replaces it -- so this can be built and
tested before the platform half is chosen, and will not have to change when it is.

**Beside the text, never in it.** A pre-edit spliced into `Editor.text` would be seen by every
move, selection and undo as ordinary text, and none of them means anything over a composition:
a caret cannot leave a pre-edit, and an undo cannot step into one. Holding it apart is what
keeps `Editor` exactly as true as it was -- `text` is what is committed, always, and a field
that loses its connection to the input method loses the composition rather than silently
keeping half a word.

### `Composing`

```
pub struct Composing {
    editor: Editor
    preedit: Preedit
}
```

An editor and the composition sitting in it, which `compose` answers together because starting
one can change both: the selection it replaces is gone from the editor, and the pre-edit has to
know where it landed.

### `compose`

```
pub fn compose(e: Editor, text: string, caret: int) -> Composing
```

Start or update a composition at the caret.

**A selection is deleted first, as typing would.** Beginning to compose over selected text
replaces it -- that is what every platform does, and the alternative leaves a user watching
their selection survive a word they have already started typing over.

Updating is the same call: an input method sends the whole pre-edit each time it changes rather
than a delta, so there is nothing to merge, and the editor already has no selection by then.

### `compose_marked`

```
pub fn compose_marked(e: Editor, text: string, caret: int, from: int, to: int) -> Composing
```

The same, with the run the input method is converting marked.

### `commit`

```
pub fn commit(e: Editor, p: Preedit, text: string) -> Editor
```

What a commit leaves behind: the composed text becomes ordinary text, and the caret follows it.

**The committed string is the input method's, not the pre-edit's.** They are usually the same
and are allowed not to be -- a conversion commits what was chosen, which may be neither what was
typed nor what was last shown.

### `shown`

```
pub fn shown(e: Editor, p: Option<Preedit>) -> string
```

The text as it looks while composing: what is committed, with the pre-edit spliced in where it
sits.

**One function for what is drawn and what is measured**, which is the rule a caret lives or
dies by: a field that measures the committed text and draws the composed one puts the caret
under the wrong character for as long as a composition lasts.

### `shown_caret`

```
pub fn shown_caret(e: Editor, p: Option<Preedit>) -> int
```

Where the caret is in what `shown` answers. **Inside the pre-edit while one is open**, because
that is where a user is typing -- an input method moves it through the composition as the
conversion changes, and a caret pinned to the start of it would not follow.

### `shown_highlight`

```
pub fn shown_highlight(e: Editor, p: Option<Preedit>) -> Option<Span>
```

The highlighted run, in the coordinates `shown` answers, or `None` when there is nothing marked.

### `Span`

```
pub struct Span {
    start: int
    end: int
}
```

A run of text, as two code-point indices.
