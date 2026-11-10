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

### `move_home`

```
pub fn move_home(e: Editor, extend: bool) -> Editor
```

### `move_end`

```
pub fn move_end(e: Editor, extend: bool) -> Editor
```

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
