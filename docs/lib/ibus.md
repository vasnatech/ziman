# `ibus`

Composing text through an ibus input method, so that Japanese, Chinese and Korean reach `edit`'s
pre-edit model. A Latin script's dead keys and compose sequences arrive the same way.

**F274's model was built two instalments before this on purpose.** Every protocol there is --
XIM, ibus, SDL3, Windows TSF, macOS's `NSTextInputClient` -- delivers the same two things: a
pre-edit string with a caret and a marked run inside it, and a commit string that replaces it.
So `edit.Preedit`, `edit.compose_marked` and `edit.commit` were built and tested against nothing,
and this instalment changed none of them. That claim is now checked rather than asserted: ibus
indexes its caret and its marked run in **characters**, which is what `edit` means by them.

**ibus over XIM, and over taking SDL3 as a second backend** -- the user's choice, and the
cheapest of the three by a distance: XIM is the older protocol and on this desktop ibus is what
answers it anyway, through a compatibility layer; SDL3 would reach macOS and Windows but is a
dependency and a second window backend. This is neither. It needs no library that `make gui` did
not already link, because the accessibility binding brought gio -- see `src/native_ibus.c`.

A loop opens once, tells the method where the caret is, offers it every key, and asks for what
it produced:


```
try ibus.open("curly")
ibus.focused(true)
for event in try window.poll(w) {
    switch event {
        KeyDown(k) {
            // The method first: a key it takes belongs to the composition, not to us.
            if !ibus.press(k) {
                state = step(state, k)
            }
        }
        Char(text) { state = typed(state, text) }
        ...
    }
}
for change in ibus.changes() {
    switch change {
        Composing(c) { entry = controls.entry_composing_marked(entry, c.text, c.caret, c.marked_start, c.marked_end) }
        Committed(text) { entry = controls.entry_commit(entry, text) }
        Closed { entry = controls.entry_commit(entry, "") }
    }
}
```

**The mapping to a widget is the application's, and the reason is not the dependency direction.**
`desktop` exists to adapt a backend's events into `controls.Input`s and could plainly hold an
adapter for these too. It cannot, for the reason `controls.entry_composing` already gives: a
composition is not routed by what is under the pointer or by a focus that falls back, it belongs
to **whatever holds the input-method context, which is the focused text field and nothing else**
-- and which field that is, is the one thing only the application knows. That is also why a
composition is not an `Input`. Four lines in a loop, and they are the application's four lines.

`use ibus` -- or `use ibus.Name` to bring one item in bare.

### `Composition`

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
```

What is being composed right now. The whole pre-edit each time it changes, never a delta, which
is what the protocol sends and what `edit.compose` is built to take.

### `Change`

```
pub enum Change {
    Composing(Composition)
    // The text to insert, which is **the method's and not the pre-edit's**: a conversion commits
    // what was chosen, and that is allowed to be neither what was typed nor what was last shown.
    Committed(string)
    Closed
}
```

What the method did. **A composition that closes is its own change rather than an update to the
empty string**, because those are different things to a field: one replaces what is shown, the
other says there is no composition any more and what is drawn is the document again.

### `open`

```
pub fn open(client: string) -> Result<unit, string>
```

Connects to the running ibus daemon and makes an input context.

**Needs a ziman with the input-method backend** -- `make gui` builds it, an ordinary `ziman` does
not, and without it this answers an error saying so rather than failing quietly. An application
that cannot reach a method should carry on as a plain text field, which is why this is a
`Result` and not a raise: in a Latin script a user may never notice.

### `close`

```
pub fn close()
```

Drops the context. A method holding a composition for a window that has gone is the one leak
that is visible to a user, as a candidate list over an empty desktop.

### `focused`

```
pub fn focused(on: bool)
```

Whether this application holds the keyboard. **A method needs telling**: an unfocused
application that still answered key events would compose into a window the user has left.

### `caret_at`

```
pub fn caret_at(x: int, y: int, width: int, height: int)
```

Where the caret is on screen, in pixels, so the candidate list appears under it. Told rather
than asked, so a window that moved or scrolled simply says it again.

**Screen, not window**, which is two things an application does not have in one place: ibus
defines `SetCursorLocation` in root coordinates, and a window knows only where its caret is
inside itself. `desktop.caret_on_screen` adds the two and hands back exactly these four
numbers, so the call is:


```
if Some(box) = desktop.caret_on_screen(try window.origin(w), laid, density) {
    ibus.caret_at(box.x, box.y, box.width, box.height)
}
```

Until 2026-11-13 the only caller was F274's own fixture, with the numbers written in by hand:
the binding existed and nothing could work out what to pass it.

### `reset`

```
pub fn reset()
```

Abandons any composition. What a field does when it loses the focus or its content is replaced
underneath it -- without it, the next key continues a word the user cannot see any more.

### `press`

```
pub fn press(k: window.KeyPress) -> bool
```

Offers a key press to the method. **True means the key belonged to the composition** and the
application must not act on it: not insert it, not treat it as a shortcut, not move the caret.
False means nothing happened and the key is the application's, exactly as it was before any of
this existed.

**Offer every key, not just the printable ones.** Return commits, Escape abandons, the arrows
move through candidates, and Backspace deletes inside the pre-edit -- a field that filtered
would take back the keys that make a composition finishable.

### `release`

```
pub fn release(k: window.KeyPress) -> bool
```

The same for a release. Separate rather than a boolean parameter, because a call that reads
`press(k, true)` at the site is a release spelled as a press.

### `changes`

```
pub fn changes() -> [Change]
```

What the method has produced since the last call. **Call it every frame**: a window loop is not
a glib main loop and the signals arrive on glib's, so this lends it a turn. It does not block,
so a frame in which nothing is being composed costs nothing.

### `decode`

```
pub fn decode(kinds: [int], texts: [string], numbers: [int]) -> [Change]
```

**Pure, and the reason this is the interesting function in the module**: the whole protocol above
can be tested by handing it three arrays, with no daemon, no engine and no user -- which is the
same bargain `window.decode_events` makes.

A change whose numbers are missing is dropped rather than guessed at, and an unknown kind is
dropped rather than turned into something: a new kind in C must not become a wrong change here.
