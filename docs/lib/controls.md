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
    w = step(w, controls.Input.KeyDown(held(controls.Key.Char("z"), true)))
    report(w, "undid the typing")

    // A paste is one step whatever its length, which is why it is its own input.
    w = step(w, controls.Input.Paste("ada@example.org"))
    report(w, "pasted an address")

    // Select all and copy: the text comes *out*, because nothing here can reach a clipboard.
    w = step(w, controls.Input.KeyDown(held(controls.Key.Char("a"), true)))
    w = step(w, controls.Input.KeyDown(held(controls.Key.Char("c"), true)))
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
    val space = held(controls.Key.Space, false)
    w = step(w, controls.Input.KeyDown(space))
    w = step(w, controls.Input.KeyUp(space))
    report(w, "pressed Space")
}

// A keystroke with modifiers. Keys are named here, not numbered: the control set owns its own
// vocabulary so that the same buttons and fields work over a desktop window, a terminal or a
// browser, each of which has an adapter of its own -- `lib/ui/desktop` is the one for X11.
fn held(k: controls.Key, control: bool) -> controls.Keystroke {
    return controls.Keystroke { key: k, shift: false, control: control, alt: false, meta: false }
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

### `Key`

```
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
```

A key, as this layer names it.

**Named rather than numbered, and owned here rather than borrowed from a backend.** The first
version of this took `window.KeyPress` -- a keysym and four flags -- on the argument that a third
key vocabulary was worse than depending on a module whose types are plain data. That was right
with one backend and wrong with four: a shared control set cannot import X11, so this is not a
third copy, it is the one copy, and each backend converts into it.

`Char` carries the character the key sits on unshifted, so a shortcut is `Char("c")` whether or
not Shift is down -- which is what a program dispatching Ctrl-C wants. `Other` keeps a name the
backend chose, so an unhandled key can be logged rather than silently dropped.

### `Keystroke`

```
pub struct Keystroke {
    key: Key
    shift: bool
    control: bool
    alt: bool
    meta: bool
}
```

A key with the modifiers that were down with it.

### `keystroke_of`

```
pub fn keystroke_of(key: Key) -> Keystroke
```

A keystroke with nothing held down.

### `char_of`

```
pub fn char_of(k: Key) -> string
```

The character a `Char` key sits on, or "" for every other key -- so a dispatch on a shortcut
letter needs no nested pattern.

### `Input`

```
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
    Release(Point)
    Move(Point)
    Wheel(Wheel)
}
```

One thing the user did.

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

### `text_area_of`

```
pub fn text_area_of(name: string, label: string, text: string, rows: int) -> Entry
```

A field `rows` lines tall: Enter inserts a newline, Up and Down move by line, and the text is
drawn a line at a time.

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

## The list

### `List`

```
pub struct List {
    name: string
    label: string
    items: [string]
    // The selected row, or -1 for none -- which is a real state, not an error: an empty list has no
    // selection, and neither does one the user has not touched.
    selected: int
    // How far the rows have scrolled, in the same units the layout uses. Not a row index: a list
    // scrolled by whole rows cannot show half of one, and half of one is what tells a user there is
    // more below.
    offset: float
    row_height: float
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
}
```

A list of rows, one of them selected, scrolled by itself.

**One tab stop with two hundred accessibility nodes.** The list takes the focus; the arrow keys
move the selection inside it; each row is a `widget.part_of` -- a control a click and a reader can
reach and Tab cannot. That distinction did not exist before this widget needed it, and the
alternative was a list that either put two hundred stops in the tab order or announced all two
hundred rows as its own name.

### `list_of`

```
pub fn list_of(name: string, label: string, items: [string]) -> List
```

### `ListStep`

```
pub struct ListStep {
    list: List
    // The selection changed. An output, as `ButtonStep.activated` is: the application acts on it
    // and stores the list.
    moved: bool
    // A row was chosen -- Enter on the selection. Separate from `moved`, because selecting a row and
    // opening it are different intentions and a list that conflated them would open on every arrow
    // key.
    activated: bool
}
```

### `list_input`

```
pub fn list_input(l: List, input: Input, bounds: Rect) -> ListStep
```

What an input does to a list.

`bounds` is the box the layout gave it, needed for two things: turning a click's y into a row,
and knowing how far one page is.

### `list_widget`

```
pub fn list_widget(l: List, style: Style) -> widget.Widget
```

The list as a widget: a control holding a viewport of rows, each row a part of the list.

**The rows carry `selected` rather than the list carrying an index**, because that is what a
reader reads: it announces "list item, Ada, selected", and an index on the container would make
every adapter work out for itself which child the number meant.

### `row_name`

```
pub fn row_name(l: List, index: int) -> string
```

A row's name, which focus and accessibility both address it by. The list's name and the index,
because a row has no name of its own and an index alone would collide with another list's.

## Menus and dialogs

### `Menu`

```
pub struct Menu {
    name: string
    title: string
    items: List
    open: bool
    disabled: bool
}
```

A title on a menu bar and the items that drop from it.

**A menu is a title and a `List`**, not a new widget with its own selection and scrolling. The
popup is a list of rows with one highlighted, scrolled when it is long, moved by the same arrows;
the only honest difference is what a reader calls it, which is why `List.role` is a field.

### `menu_of`

```
pub fn menu_of(name: string, title: string, items: [string]) -> Menu
```

### `MenuStep`

```
pub struct MenuStep {
    menu: Menu
    // The item chosen, or -1. An output: the application acts on it and stores the menu.
    chosen: int
    // The menu closed without choosing -- Escape, or a click away from it. Separate from `chosen`,
    // because a caller that treated them alike would act on a dismissal.
    dismissed: bool
}
```

### `menu_input`

```
pub fn menu_input(m: Menu, input: Input, title_bounds: Rect, popup_bounds: Rect) -> MenuStep
```

What an input does to a menu.

`title_bounds` is the box of the title on the bar and `popup_bounds` the box of the open popup,
both needed because **a click's meaning depends on which of the two it is in** -- and on neither,
which is a dismissal.

The popup is not a child of the title, so this cannot find either box itself: `widget.layer_at`
explains why an overlay lives in the root's layer list, and the caller that put it there is the
one that knows where it went.

### `menu_title_widget`

```
pub fn menu_title_widget(m: Menu, style: Style) -> widget.Widget
```

The title as it sits on the bar. A control in its own right, so Tab reaches the menu and a reader
announces it.

### `menu_popup_layer`

```
pub fn menu_popup_layer(m: Menu, style: Style, at: Rect) -> widget.Layer
```

The open popup, as a layer for the **root's** layer list.

`at` is where the caller decided it goes, from the title's laid bounds -- see `widget.layer_at`
for why it cannot be a child of the title. A placed layer, so a click away from it falls through
to what is beneath as well as closing the menu.

Takes the keyboard while it is open, because an open menu owns the arrows.

## Dialogs

### `DialogAnswer`

```
pub enum DialogAnswer {
    Confirm
    Cancel
    Nothing
}
```

What a dialog's keys mean. Returned rather than acted on, for the reason everything here is: this
module cannot close a window or save a record.

### `dialog_keys`

```
pub fn dialog_keys(input: Input) -> DialogAnswer
```

**Escape cancels and Enter confirms**, which are the two keys a dialog owes a user who never
reaches for the mouse. Everything else belongs to whatever is inside it.

Enter is deliberately not "the focused button": a dialog's default action is the dialog's, not the
focus's, or tabbing to Cancel and pressing Enter would confirm.

### `dialog_layer`

```
pub fn dialog_layer(name: string, title: string, content: widget.Widget, style: Style) -> widget.Layer
```

A modal layer holding `content` under a title.

**Modal, so the geometry shuts the pointer out and `takes_keys` shuts Tab out** -- a click beside
the dialog lands on the layer, finds no control, and goes nowhere, and the tab order holds only
what is inside. `widget.modal_layer` is where both of those are explained.

The content is the caller's: a dialog is a frame and a pair of keys, not a thing that knows what
it is asking.

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
