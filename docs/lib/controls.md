# `controls`

The widget set: what an input does to a control, and what a control looks like.

docs/reviews/tasks/f269-f277.md F277, first instalment. `widget` builds and places a tree,
`focus` says what a click lands on, `edit` is a text field's model and `a11y` describes all of it
-- and none of them turns an event into a new state. That is this module, and the two controls
here are the ones that need neither text editing nor scrolling, so the model is the subject and
they are the proof of it.

**The architecture is forced by the language, not chosen.** The usual widget set hands a widget a
callback -- `on_click: fn()` -- which mutates the application's state. Ziman will not compile
that: "a lambda cannot capture 'count' because it is a 'var' ... captures are by value, so the
lambda and the enclosing function never share mutable state". So a control cannot act; it can
only *answer*. Every update here takes a state and an input and returns a new state together with
what happened, and the application keeps the state and acts on what happened. `focus.clicked`
already had exactly that shape, so this is the existing grain of the thing rather than a new idea.

**An application owns its controls' states; this module owns none.** There is no registry of
widgets here and there cannot be one -- no shared mutable state exists to hold it -- so the
application holds whatever its window needs, routes an input with `target`, and calls the update
for the control that input was for.

`use controls` -- or `use controls.Name` to bring one item in bare.

## Example

```
use a11y
use controls
use focus
use geom
use widget
use window

// A fake metrics, so the example needs no font: ten units per character.
fn metrics() -> widget.Metrics {
    return widget.metrics_of(
        fn(text: string, size: float) -> float { return (len(text) as float) * 10.0 },
        fn(size: float) -> float { return size * 2.0 }
    )
}

// **The application's whole state, in one value.** A control cannot act -- Ziman will not let a
// lambda mutate what it captured -- so there is nowhere for a callback to put anything, and the
// window's state has to be a value the program threads through. That is not a style choice here.
struct Window {
    email: controls.Entry
    save: controls.Button
    notify: controls.CheckBox
    focused: string
    saves: int
    // The clipboard, which this layer cannot reach: a control asks for text to be put somewhere and
    // the application is what has somewhere to put it. On a desktop this is the X11 selection, which
    // F273 left as platform work.
    clipboard: string
}

fn view(w: Window) -> widget.Widget {
    val style = controls.default_style()
    return widget.column_of(
        6.0,
        [
            controls.entry_widget(w.email, style, metrics()),
            controls.check_box_widget(w.notify, style),
            controls.button_widget(w.save, style)
        ]
    )
}

// One input, applied: route it, hand it to the control it was for, and keep what came back.
fn step(w: Window, input: controls.Input) -> Window {
    val laid = widget.layout(view(w), geom.rect_of(0.0, 0.0, 200.0, 110.0), metrics())
    val name = controls.target(laid, input, w.focused)
    val bounds = switch focus.node_of(laid, name) {
        Some(one) { one.bounds }
        None { geom.rect_of(0.0, 0.0, 0.0, 0.0) }
    }
    if name == "email" {
        val done = controls.entry_input(w.email, input, bounds, controls.default_style(), metrics())
        val board = if done.copied == "" { w.clipboard } else { done.copied }
        return with w { email: done.entry, focused: name, clipboard: board }
    }
    if name == "save" {
        val done = controls.button_input(w.save, input, bounds)
        val saves = if done.activated { w.saves + 1 } else { w.saves }
        return with w { save: done.button, focused: name, saves: saves }
    }
    if name == "notify" {
        val done = controls.check_box_input(w.notify, input, bounds)
        return with w { notify: done.check_box, focused: name }
    }
    return with w { focused: name }
}

// The middle of a control, found rather than guessed: the layout decides where a row lands, so an
// example with hard-coded coordinates is an example that breaks when a padding changes.
fn centre(w: Window, name: string) -> geom.Point {
    val laid = widget.layout(view(w), geom.rect_of(0.0, 0.0, 200.0, 110.0), metrics())
    return switch focus.node_of(laid, name) {
        Some(one) { geom.point_of(one.bounds.x + one.bounds.width / 2.0, one.bounds.y + one.bounds.height / 2.0) }
        None { geom.point_of(0.0, 0.0) }
    }
}

// A whole click on one control.
fn click(w: Window, name: string) -> Window {
    val at = centre(w, name)
    return step(step(w, controls.Input.Press(at)), controls.Input.Release(at))
}

fn report(w: Window, what: string) {
    val laid = widget.layout(view(w), geom.rect_of(0.0, 0.0, 200.0, 110.0), metrics())
    val tree = a11y.tree_of(laid, "Settings", w.focused)
    val said = switch a11y.node_of(tree, w.focused) {
        Some(one) { a11y.announce(one) }
        None { "(nothing focused)" }
    }
    printf("{}: saves={} focus={} -> {}\n", what, str(w.saves), w.focused, said)
}

fn main() {
    var w = Window {
        email: controls.entry_of("email", "Email", ""),
        save: controls.button_of("save", "Save"),
        notify: controls.check_box_of("notify", "Notify me", widget.Toggle.Off),
        focused: "",
        saves: 0,
        clipboard: ""
    }

    // Click into the field, then type.
    w = click(w, "email")
    w = step(w, controls.Input.Text("a"))
    w = step(w, controls.Input.Text("d"))
    w = step(w, controls.Input.Text("a"))
    report(w, "typed into the field")

    // **One Ctrl-Z, and the whole run goes.** A run of typed characters is one undo step; a delete,
    // a paste or a caret move between them would have ended the run.
    w = step(w, controls.Input.KeyDown(with window_key(122) { control: true }))
    report(w, "undid the typing")

    // A paste is one step whatever its length, which is why it is its own input.
    w = step(w, controls.Input.Paste("ada@example.org"))
    report(w, "pasted an address")

    // Select all and copy: the text comes *out*, because nothing here can reach a clipboard.
    w = step(w, controls.Input.KeyDown(with window_key(97) { control: true }))
    w = step(w, controls.Input.KeyDown(with window_key(99) { control: true }))
    printf("the clipboard now holds: {}\n", w.clipboard)

    // The check box is the top row; a press and a release on it toggles it.
    w = click(w, "notify")
    report(w, "clicked the check box")

    // The button is below it. Pressed and released inside, it fires.
    w = click(w, "save")
    report(w, "clicked Save")

    // **Pressed and released outside, it does not.** Dragging off a button cancels it, which is why
    // `held` is state: a user who changed their mind has not saved anything.
    w = step(w, controls.Input.Press(centre(w, "save")))
    w = step(w, controls.Input.Release(geom.point_of(400.0, 400.0)))
    report(w, "dragged off Save")

    // The keyboard, on whatever has the focus. Space arms on the way down and fires on the way up,
    // exactly as the pointer does.
    val space = window_key(32)
    w = step(w, controls.Input.KeyDown(space))
    w = step(w, controls.Input.KeyUp(space))
    report(w, "pressed Space")
}

fn window_key(keysym: int) -> window.KeyPress {
    return window.KeyPress { keysym: keysym, shift: false, control: false, alt: false, meta: false }
}
```

Output:

```
typed into the field: saves=0 focus=email -> text field, Email, ada
undid the typing: saves=0 focus=email -> text field, Email
pasted an address: saves=0 focus=email -> text field, Email, ada@example.org
the clipboard now holds: ada@example.org
clicked the check box: saves=0 focus=notify -> check box, Notify me, checked
clicked Save: saves=1 focus=save -> button, Save
dragged off Save: saves=1 focus=save -> button, Save
pressed Space: saves=2 focus=save -> button, Save
```

## The inputs

### `Wheel`

```
pub struct Wheel {
    at: Point
    // Lines, signed: positive is away from the user. The same convention `window.ScrollStep` uses,
    // because converting between two sign conventions is a bug waiting to be written.
    vertical: int
    horizontal: int
}
```

### `Input`

```
pub enum Input {
    KeyDown(KeyPress)
    KeyUp(KeyPress)
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
    Release(Point)
    Move(Point)
    Wheel(Wheel)
}
```

One thing the user did.

**Keys arrive as `window.KeyPress`, which is a plain struct of a keysym and four flags and not a
platform handle.** A third key vocabulary was the alternative -- `terminal` has one and `window`
has keysyms -- and a third copy of fifteen magic numbers is worse than a dependency on a module
whose types are data. A driver that is not X11 builds `KeyPress` values itself; X11's keysym
numbering is the de facto standard for exactly this.

### `target`

```
pub fn target(l: Laid, input: Input, focused: string) -> string
```

Which control an input is for: the name, or "" for nobody.

**The three rules here are each one a home-grown toolkit gets wrong.**

A key goes to the focus, which is the easy one.

A press goes to whatever is under the pointer *and moves the focus there* -- `focus.clicked`
already decides that, including leaving the focus alone when the press landed on nothing.

**A release goes to the focus, not to what is under the pointer**, which looks wrong and is
right: the press moved the focus to the control being pressed, so routing the release to the
focus delivers it to the control that was pressed even when the pointer has since moved off. A
release routed by position instead would never reach a button the user dragged off, and the
button would stay drawn pressed for ever. The same reasoning covers a move.

**A wheel goes to the control under the pointer and ignores the focus entirely**, because that is
what a user means: scrolling the list they are pointing at, not the one that happens to have the
keyboard. Over nothing it goes nowhere rather than to the focus.

## The button

### `Button`

```
pub struct Button {
    name: string
    label: string
    disabled: bool
    held: bool
}
```

A button, and whether it is being held down.

`held` is state and `ButtonStep.activated` is not, which is the distinction the whole model rests
on: being held is something a user can see and the next frame has to draw, while having fired is
something that happened once and that the application acts on rather than stores.

### `button_of`

```
pub fn button_of(name: string, label: string) -> Button
```

### `ButtonStep`

```
pub struct ButtonStep {
    button: Button
    activated: bool
}
```

### `button_input`

```
pub fn button_input(b: Button, input: Input, bounds: Rect) -> ButtonStep
```

What an input does to a button.

`bounds` is the rectangle the layout gave it -- `focus.node_of` answers that -- and it is needed
for one rule: **a press inside followed by a release outside does not fire.** Dragging off a
button cancels it, which every toolkit does and which is the reason `held` exists at all rather
than the button simply reporting each click. A button that fired on any release would fire when
the user changed their mind.

The keyboard mirrors the pointer rather than firing on the way down: **Space arms and fires on
release**, so it can be cancelled the same way, while **Enter fires immediately**, because Enter
is the key a default button answers and a user pressing it has already decided. That split is the
platforms' own and is deliberate here.

### `button_widget`

```
pub fn button_widget(b: Button, style: Style) -> widget.Widget
```

The button as a widget: a control declaring its role, around a background and its label.

The role and the label are declared here, which is what F275 is for -- a reader announces
"button, Save" from this one call, and nothing downstream has to guess that a group of rectangles
was a button.

## The check box

### `CheckBox`

```
pub struct CheckBox {
    name: string
    label: string
    disabled: bool
    checked: Toggle
    held: bool
}
```

### `check_box_of`

```
pub fn check_box_of(name: string, label: string, checked: Toggle) -> CheckBox
```

### `CheckStep`

```
pub struct CheckStep {
    check_box: CheckBox
    toggled: bool
}
```

### `check_box_input`

```
pub fn check_box_input(c: CheckBox, input: Input, bounds: Rect) -> CheckStep
```

What an input does to a check box: the button's rules, and then the toggle.

**A mixed box becomes checked, never unchecked**, which is not what "toggle" suggests and is what
every platform does: a mixed box means some of the things under it are on, and a user clicking it
is asking for all of them, not none. Going to unchecked would throw away the choices that were
already made.

### `check_box_widget`

```
pub fn check_box_widget(c: CheckBox, style: Style) -> widget.Widget
```

The check box as a widget: the mark, then the label, and the state declared so that a reader says
"check box, Notify me, partly checked" without anything else being told.

## The entry

### `Entry`

```
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
}
```

A single-line text field.

The editing itself is `lib/ui/edit`: this holds an `Editor` and a `History` and decides which
input means which edit. Keeping the two apart is what let the grapheme and word rules be tested
against 32 cases with no widget, no layout and no font in sight.

### `entry_of`

```
pub fn entry_of(name: string, label: string, text: string) -> Entry
```

### `EntryStep`

```
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
```

### `entry_input`

```
pub fn entry_input(e: Entry, input: Input, bounds: Rect, style: Style, m: widget.Metrics) -> EntryStep
```

What an input does to a field.

`bounds`, `style` and `m` are all here for one reason: a press carries a pixel and a caret is an
index, so placing the caret where the user clicked needs the same measurement the layout used.

### `caret_at_x`

```
pub fn caret_at_x(text: string, size: float, m: widget.Metrics, x: float) -> int
```

The caret index a pixel offset into the text means.

**The nearest boundary, not the character the pixel is inside.** Clicking the left half of a
character puts the caret before it and the right half after it, which is what every text field
does and what a user means by clicking "between" two letters. Taking the character the pixel
landed in instead makes the caret feel one place behind the pointer, which is the kind of thing
nobody reports and everybody notices.

Measured as prefixes rather than as a sum of per-character widths, because a sum ignores kerning
and would disagree with the layout that drew the text. That makes this O(n^2) in the length of
the field, which is the right trade for one line of text and the wrong one for a document.

### `caret_name`

```
pub val caret_name = "caret"
```

The name the caret's box carries in the laid-out tree, so that a test can find it and an
application can draw attention to it.

### `entry_widget`

```
pub fn entry_widget(e: Entry, style: Style, m: widget.Metrics) -> widget.Widget
```

The field as a widget.

**The caret is placed by the layout rather than computed.** The row is the text before the caret,
then a one-unit box, then the text after it -- so the same metrics that measure the text place the
caret, and the two cannot drift apart. Computing a caret's x separately is how a caret ends up
half a character off in one font and right in another.

A selection is the same trick: the selected run is its own label with a background behind it.

Takes a `Metrics` where `button_widget` does not, because a caret's height is a text measurement
and guessing it from the em size is wrong for any face whose line height is not 1.2.

## The style

### `Style`

```
pub struct Style {
    text_size: float
    padding: Insets
    ink: Color
    face: Color
    pressed_face: Color
    disabled_ink: Color
}
```

How a control is drawn. Injected rather than global, for the reason `widget.Metrics` is: a module
that reads a theme from somewhere cannot be tested against two themes, and an application with
two panels cannot style them differently.

### `default_style`

```
pub fn default_style() -> Style
```

A plain light style. **Not a theme and not a design** -- F277 says in as many words that pixel
parity with a platform's widgets is not a goal, so this is a legible default and the thing an
application replaces.

## The keys

### `is_text_input`

```
pub fn is_text_input(input: Input) -> bool
```

Whether this input could move a caret or change text -- what the next instalment's entry asks
before handing an input to `edit`, and here rather than there because the answer is about `Input`.
