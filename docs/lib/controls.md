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
use theme
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
    // What the last press went to, until the release. See `controls.pressed_after`: a move during
    // a drag belongs to the control that took the press, not to whatever has the keyboard.
    pressed: string
    saves: int
    // The clipboard, which this layer cannot reach: a control asks for text to be put somewhere and
    // the application is what has somewhere to put it. On a desktop this is the X11 selection, which
    // F273 left as platform work.
    clipboard: string
}

fn view(w: Window) -> widget.Widget {
    // **The frame, not a style and a metrics**: the look, the window's measurements and the three
    // names the window holds, so that a control can tell what state it is in. See `controls.Frame`.
    val f = with controls.frame_of(theme.plain(), metrics()) { focused: w.focused, pressed: w.pressed }
    return widget.column_of(
        6.0,
        [
            controls.entry_widget(w.email, f),
            controls.check_box_widget(w.notify, f),
            controls.button_widget(w.save, f)
        ]
    )
}

// One input, applied: route it, hand it to the control it was for, and keep what came back.
fn step(w: Window, input: controls.Input) -> Window {
    val laid = widget.layout(view(w), geom.rect_of(0.0, 0.0, 200.0, 110.0), metrics())
    val name = controls.target(laid, input, w.focused, w.pressed)
    val held = controls.pressed_after(w.pressed, input, name)
    val bounds = switch focus.node_of(laid, name) {
        Some(one) { one.bounds }
        None { geom.rect_of(0.0, 0.0, 0.0, 0.0) }
    }
    if name == "email" {
        val done = controls.entry_input(w.email, input, bounds, controls.frame_of(theme.plain(), metrics()))
        val board = if done.copied == "" { w.clipboard } else { done.copied }
        return with w { email: done.entry, focused: name, pressed: held, clipboard: board }
    }
    if name == "save" {
        val done = controls.button_input(w.save, input, bounds)
        val saves = if done.activated { w.saves + 1 } else { w.saves }
        return with w { save: done.button, focused: name, pressed: held, saves: saves }
    }
    if name == "notify" {
        val done = controls.check_box_input(w.notify, input, bounds)
        return with w { notify: done.check_box, focused: name, pressed: held }
    }
    return with w { focused: name, pressed: held }
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
        pressed: "",
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
```

One thing the user did.

### `target`

```
pub fn target(l: Laid, input: Input, focused: string, pressed: string) -> string
```

Which control an input is for: the name, or "" for nobody.

**The three rules here are each one a home-grown toolkit gets wrong.**

A key goes to the focus, which is the easy one.

A press goes to whatever is under the pointer *and moves the focus there* -- `focus.clicked`
already decides that, including leaving the focus alone when the press landed on nothing.

**A release and a move go to whatever took the press** -- the pointer grab -- and to the focus
only when no button is down. Routing them by position instead would never reach a button the
user dragged off, and the button would stay drawn pressed for ever.

**This used to route them to the focus alone, and that silently broke every control Tab cannot
reach.** The reasoning was that a press moves the focus to the control being pressed, so the
focus *is* what was pressed -- true only for a tab stop. A scroll bar is not one (`focus.clicked`
leaves the focus where it was), so the bar took the press, set `grabbed`, and then every move
went to some control elsewhere on screen: the thumb could be grabbed and not dragged, measured
in the gallery as a thumb that stayed at pixel 126 through a 230-pixel drag. The split pane's
divider is in the same position. Both are exactly the controls a pointer works and a keyboard
does not, which is the set this rule was least able to serve.

`pressed` is the name this function last answered for a `Press`, which the application holds
between frames -- `pressed_after` is the one-liner that maintains it.

**A wheel goes to the control under the pointer and ignores the focus entirely**, because that is
what a user means: scrolling the list they are pointing at, not the one that happens to have the
keyboard. Over nothing it goes nowhere rather than to the focus.

**To the nearest *tab stop* under the pointer, not the innermost control**, which is the same
resolution a press gets and for the same reason. `focus.control_at` answers a list's row or a
tree's item -- a `part_of`, something a click and a reader can reach and that no application
holds a state for -- so a wheel routed that way named a control nobody could handle and was
dropped on the floor. **The wheel had never scrolled a list or a tree in any application.**
Found by painting a scroll bar: the indicator sat still while the window said it should move.

### `pressed_after`

```
pub fn pressed_after(pressed: string, input: Input, name: string) -> string
```

The pointer grab after this input: the name `target` answered for a press, held until the
release.

**Held by the application, because a control cannot act.** Nothing in this module remembers
anything between two inputs; an application already keeps `focused` for the same reason, and
this is its twin for the pointer. `name` is what `target` just answered, so the pair is always
used together:


```
val name = controls.target(laid, input, focused, s.pressed)
val next = with s { pressed: controls.pressed_after(s.pressed, input, name) }
```

**A release clears it even when it lands on nothing**, which is what stops a grab from outliving
the gesture that made it -- a bar left grabbed would swallow every later move in the window.

### `cursor_at`

```
pub fn cursor_at(l: Laid, at: Point) -> widget.Cursor
```

The pointer's shape over this point, from whatever control is under it.

**A window-wide question answered from the tree, so every application gets it without wiring
one.** An application calls this on each move and hands the answer to `desktop.set_cursor`; the
gallery showed an arrow over everything until it did, including over a split pane's divider,
which is the one control whose whole affordance *is* the cursor -- a line you cannot tell is
draggable until you are already dragging it.

**Decided by the control's role**, which is the same table accessibility reads, so a control
that declares itself properly gets a cursor for nothing and one that does not is visibly
ordinary. That is the right pressure: `a11y` and the pointer should not disagree about what a
thing is.

**A separator's direction comes from its own box, not from a second field.** A divider between
two columns is tall and thin and moves sideways; one between two rows is wide and short and
moves up and down. The box is the fact, already laid out and exact -- where an orientation flag
would be a second copy of it, free to disagree, which is the mistake `thumb_of` and `split_at`
exist to avoid.

**A column header is left to `table_cursor`.** Its shape depends on *where in the header* the
pointer is -- an edge resizes, the middle sorts -- which is a question about one control's
geometry rather than about which control is under the pointer, so it does not belong here.

## The button

### `Button`

```
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
    // **The one a form is expecting to be pressed**: GTK's `suggested-action`, macOS's default
    // button, the filled one standing beside the outlined ones. A dialog's confirm, a sign-in's
    // "Sign in", a wizard's "Next".
    //
    // **At most one in a group**, which this cannot enforce and a caller must mean: two prominent
    // buttons side by side say the same thing twice and so say nothing.
    //
    // **Drawn with the style's `Selected` row**, which is the accent pair every look already
    // defines. A button is never "selected" in a list's sense, so there is no second meaning to
    // collide with -- and it inherits that row's rule of *keeping its colour under the pointer*,
    // which for this control is right rather than merely convenient: a prominent button is the one
    // being offered, and a hover repainting it in the neutral hover colour would read as it
    // turning into something else.
    //
    // **What a look still cannot say** is what a suggested button looks like *independently* of a
    // selection -- the two share one row. No look has wanted to part them yet; the day one does,
    // `States` grows a row rather than this field changing meaning.
    prominent: bool
    // **This button opens something rather than doing something**, and wears the disclosure
    // chevron to say so: the difference between "add one" and "choose which one to add".
    //
    // **A flag, not a second icon.** The caret is the toolkit's own mark -- `ComboBox` and a
    // sorted column header already draw this exact chevron -- and a button that let an application
    // supply its own would be the one place in a window where a menu announces itself differently.
    // It is also why this is not `icon` with `Trailing`: a menu button usually has a mark of its
    // own as well, and the segmented add button this was measured against is a plus *and* a caret.
    drops: bool
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

### `icon_button_of`

```
pub fn icon_button_of(name: string, label: string, icon: widget.Icon) -> Button
```

A button with an icon before its label: `with button_of(..) { icon: .. }` says the same thing,
and this is here because an icon button is common enough to deserve one call.

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
pub fn button_widget(b: Button, f: Frame) -> widget.Widget
```

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
pub fn check_box_widget(c: CheckBox, f: Frame) -> widget.Widget
```

The check box as a widget: the mark, then the label, and the state declared so that a reader says
"check box, Notify me, partly checked" without anything else being told.

## The switch

### `Switch`

```
pub struct Switch {
    name: string
    label: string
    disabled: bool
    on: Toggle
    held: bool
}
```

A setting that takes effect as it moves.

**Not a check box that looks different**, which is the whole reason it is here. A check box is a
choice a user makes and commits with the rest of a form; a switch is the state of something, and
moving it *is* the action -- which is why every platform gives the two different roles and why a
reader says "on" for one and "checked" for the other. A dialog's "Remember me" is a check box; a
window's "Dark mode" is a switch.

Same shape as `CheckBox` otherwise, down to `held`: it is a button that carries a `Toggle`.

### `SwitchStep`

```
pub struct SwitchStep {
    control: Switch
    toggled: bool
}
```

**The field is `control` and not `switch`**, which is a keyword here and cannot be a field name.
Every other step names its field after its control -- `check_box`, `button`, `entry` -- so this
is the one place that convention cannot be followed, rather than a different idea.

### `switch_of`

```
pub fn switch_of(name: string, label: string, on: Toggle) -> Switch
```

### `switch_input`

```
pub fn switch_input(s: Switch, input: Input, bounds: Rect) -> SwitchStep
```

A press or Space moves it, exactly as it does a check box: the difference between the two
controls is what it means, not how it is worked.

### `switch_widget`

```
pub fn switch_widget(s: Switch, f: Frame) -> widget.Widget
```

**The track is the accent when it is on**, which is what `accent` and `on_accent` are for: the
one colour a look is recognised by, and what reads on it. The five state rows still govern the
off track and the ink, so a hovered or disabled switch is the look's hovered or disabled face --
but an *on* switch is not a state of the control, it is its value, and a value that said
`states.selected.face` would be a pale tint of the accent where every platform draws a solid one.

## The toggle button

### `ToggleButton`

```
pub struct ToggleButton {
    name: string
    label: string
    icon: Option<widget.Icon>
    disabled: bool
    // The pointer is down now.
    held: bool
    // And this is what survives it.
    on: Toggle
}
```

A button that stays pressed.

**`Button.held` is not this.** `held` is the pointer being down this instant -- the distinction
the button's own comment draws, between what the next frame has to draw and what happened once.
A toggle keeps its answer after the finger leaves: the bold in a toolbar, or one segment of a
view switch.

**`Toggle` rather than `bool`**, as `CheckBox` and `Switch` both carry, and for the same reason
they do: `Mixed` is a real answer here. A bold button over a selection that is partly bold is
neither on nor off, and a `bool` would make the application pick one and lie.

Field for field a `CheckBox` wearing a button's face, which is what lets the two share their
input rules below rather than growing a second copy of them.

### `ToggleButtonStep`

```
pub struct ToggleButtonStep {
    button: ToggleButton
    toggled: bool
}
```

### `toggle_button_of`

```
pub fn toggle_button_of(name: string, label: string, on: Toggle) -> ToggleButton
```

### `icon_toggle_button_of`

```
pub fn icon_toggle_button_of(
    name: string,
    label: string,
    icon: widget.Icon,
    on: Toggle
) -> ToggleButton
```

### `toggle_button_input`

```
pub fn toggle_button_input(b: ToggleButton, input: Input, bounds: Rect) -> ToggleButtonStep
```

**Through `button_input`, like the check box**, so that press, drag-off, Space and Enter are the
button's rules and not three copies of them. `next` is the check box's own cycle.

### `toggle_button_widget`

```
pub fn toggle_button_widget(b: ToggleButton, f: Frame) -> widget.Widget
```

The toggle as a widget: a button's face, and `on` in the `selected` row of the look.

**`mood_of`'s `selected` slot is the whole drawing.** A pressed toggle wears whatever the look
says a selected control wears, so a theme restyles toggles by restyling selection and never
learns this control exists.

## The toggle group

### `ToggleGroup`

```
pub struct ToggleGroup {
    name: string
    label: string
    items: [ToggleButton]
    // **One at a time, or any number.** The only thing this changes is the input rule -- the
    // drawing is the same either way, because a segmented switch and a toolbar of toggles differ
    // in what they mean and not in what they look like.
    exclusive: bool
    // Which item the arrow keys are on. A group is one tab stop, as a toolbar and a list are.
    at: int
    disabled: bool
}
```

A row of toggle buttons that belong together: a segmented view switch, or a toolbar's bold,
italic and underline.

**Not `Tabs`, deliberately.** `tabs_widget` says why it cannot be: the chosen tab wears the
body's colour and is rounded along its top only, *"which is what makes a strip read as index
cards instead of as a segmented button"*. A segmented button is what this is, and the library
had ruled the look out without offering it anywhere else.

**Not `Radios` either.** Those are a form field -- a column of dots and words, with no room for
an icon. A toolbar's toggles are an icon each and nothing else.

**Its items are whole `ToggleButton`s**, where `Tabs` and `Radios` hold `[string]` and an index.
That is a departure and it is the point: every item needs its own name, its own icon and its own
state, and a caller should be able to gather buttons they already have. An index into labels
could say none of that.

### `ToggleGroupStep`

```
pub struct ToggleGroupStep {
    group: ToggleGroup
    // Which item moved, or -1. **Not a bool**: a caller with one handler per item needs to know
    // which, and in the exclusive case the one that went *off* is not news -- the one that came on
    // is.
    changed: int
}
```

### `toggle_group_of`

```
pub fn toggle_group_of(name: string, label: string, items: [ToggleButton]) -> ToggleGroup
```

### `exclusive_group_of`

```
pub fn exclusive_group_of(
    name: string,
    label: string,
    items: [ToggleButton],
    chosen: int
) -> ToggleGroup
```

The segmented case: one on at a time, and never none.

### `chosen_in`

```
pub fn chosen_in(g: ToggleGroup) -> Option<int>
```

Which item is on, for the exclusive case. **An `Option`, because a group built by hand may have
none on** -- `exclusive_group_of` cannot be given a bad index, but `toggle_group_of` takes the
items as they come.

### `choosing`

```
pub fn choosing(g: ToggleGroup, at: int) -> ToggleGroup
```

The same group with `at` chosen and the rest off.

**The setter `chosen_in` is the reader of.** An application that restores which view it was on,
or that moves the choice because something else happened, has to be able to say so -- and
without this it would reach into `items` and set the toggles itself, which is the one place the
exclusivity rule could be got wrong.

An index outside the group leaves it alone, rather than turning everything off: `exclusive_group_of`
promises exactly one is lit, and a setter that could break that promise is not a setter for it.

### `group_item_name`

```
pub fn group_item_name(g: ToggleGroup, index: int) -> string
```

### `toggle_group_input`

```
pub fn toggle_group_input(g: ToggleGroup, input: Input, laid: Laid, f: Frame) -> ToggleGroupStep
```

What a press or a key does to a group.

**Takes the laid tree rather than a box**, for the toolbar's reason: the items are an icon, a
word or both, every one a different width, and their laid boxes are the only thing that knows
where they ended up. Dividing a box into equal shares is a tab strip's arithmetic, not this.

### `toggle_group_widget`

```
pub fn toggle_group_widget(g: ToggleGroup, f: Frame) -> widget.Widget
```

The group as a widget: **one tab stop holding a part per item, on a shared track**.

One tab stop is what ARIA says for both shapes this covers -- a `radiogroup` for the exclusive
one and a `toolbar` for the other -- and what `Toolbar` and `List` already do here. Each item
declares its own `checked`, as a tab's part declares its own `selected`, *"because that is what
a reader reads"*.

**The track is what makes it one control instead of three.** The first version drew the items
edge to edge and they still read as three buttons that happened to touch: every one carried its
own face and its own border, so the eye counted three objects. The answer is not to take the
corners off -- `widget.Corners` offers `Top` and `Bottom` and no sides -- but to put something
behind them: a chosen segment wears the look's selected face, an unchosen one is **transparent**
and lets the track through, and the group reads as one object with a lit part. That is the
relationship the segmented controls this was measured against use, and it needs no new geometry.

## The expander

### `Expander`

```
pub struct Expander {
    name: string
    title: string
    disabled: bool
    // Whether the section is showing. The application owns it, as it owns which tab is chosen.
    open: bool
    held: bool
}
```

A titled section that folds away.

**A button that happens to hide something**, which is both the design and the accessibility
story. Neither ATK nor ARIA has an "expander" role, and neither is missing one: a disclosure is
a button whose `expanded` state says what pressing it did, and a reader announcing "Advanced,
collapsed, button" has told the user everything there is to know. `State.expanded` has been in
`widget` since accessibility was built, and until now a tree row was the only thing that set it.

**The header is the control and the section is not.** This is a named group holding a focusable
header and, when it is open, the content -- rather than one control with the content inside it,
which would nest every button in the section underneath a button and hand a reader a tree it
cannot describe. It is the number field's rule the other way up: there one control had to
swallow its parts, here the parts must not be swallowed.

### `ExpanderStep`

```
pub struct ExpanderStep {
    expander: Expander
    toggled: bool
}
```

### `expander_of`

```
pub fn expander_of(name: string, title: string, open: bool) -> Expander
```

### `expander_input`

```
pub fn expander_input(e: Expander, input: Input, bounds: Rect) -> ExpanderStep
```

**These are the header's bounds and not the section's**, which is the one thing a caller has to
get right and the reason the header is what carries the name: `boxes.bounds_of(laid, name)`
finds the header, so a press in the content is outside this box by construction and folds
nothing away under the user's hand. Every other control here takes its own box the same way.

### `expander_widget`

```
pub fn expander_widget(e: Expander, content: widget.Widget, f: Frame) -> widget.Widget
```

## The progress bar

### `Progress`

```
pub enum Progress {
    Fraction(float)
    Busy(float)
}
```

How far along something is, or only that it is going on at all.

**Two cases, not a number with a sentinel.** A bar whose value is `-1.0` or `None` for "I do not
know" is a bar every caller must remember to check, and the one that forgets draws a job that is
finished as a job that has not started. They are also announced differently -- a determinate bar
has a percentage to read and an indeterminate one has only "busy" -- so the difference has to
survive as far as the reader, which a sentinel does not.

`Busy` carries a **phase**, 0 to 1, and the application advances it. `lib/ui` has no clock and is
not going to grow one: a library that reads the time cannot be tested by handing it a value, and
every animation here is already a function of a number somebody else owns.

### `ProgressBar`

```
pub struct ProgressBar {
    name: string
    label: string
    progress: Progress
}
```

### `progress_bar_of`

```
pub fn progress_bar_of(name: string, label: string, fraction: float) -> ProgressBar
```

**A name it can be announced by, and no input function at all.** A progress bar is the one
control here that a user cannot operate -- there is nothing to press and no state to change --
so it has no `*_input`, is not a tab stop, and still has to be in the accessibility tree,
because "is it still going?" is exactly the question a reader has to be able to answer.

### `busy_bar_of`

```
pub fn busy_bar_of(name: string, label: string, phase: float) -> ProgressBar
```

### `progress_bar_widget`

```
pub fn progress_bar_widget(b: ProgressBar, f: Frame) -> widget.Widget
```

## The number field

### `NumberField`

```
pub struct NumberField {
    entry: Entry
    least: float
    most: float
    // What a stepper, an arrow key and a wheel notch move by.
    step: float
}
```

A field that only ever holds something on its way to being a number, with a pair of steppers.

**It carries text, not a number, and that is the decision the control exists to get right.** A
field holding a `float` cannot represent what a user is in the middle of typing: `-` is not a
number and is on the way to one, so is `1.`, so is an empty field. The spin buttons that
infuriate on every platform are the ones that store the number -- they rewrite `-` to `0`, or
refuse the keystroke, and either way the field cannot be typed into backwards.

So the text is the truth while typing and the number is derived. **Nothing is clamped until it is
committed**, because clamping each keystroke makes a range impossible to type into: into a field
bounded 10 to 20, typing `1` would clamp to `10` and the `5` after it would arrive at `105`.

### `NumberStep`

```
pub struct NumberStep {
    field: NumberField
    // Whether the number this field holds is different from the one it held. **Not whether the text
    // changed**: moving the caret through `100` changes neither, and typing the `.` of `1.5` changes
    // the text while the number is still on its way.
    changed: bool
}
```

### `number_field_of`

```
pub fn number_field_of(
    name: string,
    label: string,
    value: float,
    least: float,
    most: float,
    step: float
) -> NumberField
```

### `number_in`

```
pub fn number_in(n: NumberField) -> Option<float>
```

The number the field holds, or nothing while its text is on the way to one.

**An `Option`, because "no number yet" is a real state and zero is a lie.** A field showing `-`
has no value, and a caller handed `0.0` for it would write a port of zero into a config.

### `number_or`

```
pub fn number_or(n: NumberField, fallback: float) -> float
```

### `stepped`

```
pub fn stepped(n: NumberField, by: float) -> NumberField
```

The field with its value moved by `by` steps, clamped and rewritten.

**A stepper commits**, which is why it clamps where typing does not: pressing it is not being in
the middle of anything, so there is no half-typed state to protect. From a field whose text is
not a number at all it starts from the low end, which is the only answer that is not a guess.

### `committed`

```
pub fn committed(n: NumberField) -> NumberField
```

The field with its text clamped into range and rewritten in the canonical form: what Enter and
losing the focus do.

**Empty stays empty.** A field a user cleared has not been given an out-of-range number, and
filling it with the minimum on the way past would overwrite what they were about to type.

### `number_field_input`

```
pub fn number_field_input(n: NumberField, input: Input, bounds: Rect, f: Frame) -> NumberStep
```

### `number_field_widget`

```
pub fn number_field_widget(n: NumberField, f: Frame) -> widget.Widget
```

## The tooltip

### `Hover`

```
pub struct Hover {
    // The control the pointer is on, "" for none.
    on: string
    // Milliseconds the pointer has been on it without moving to another control.
    waited: int
}
```

How long the pointer has been still on one control.

**The application's number, because `lib/ui` has no clock and is not going to grow one.** A
library that read the time could not be tested by handing it a value, and the pause before a
tooltip is the one thing about a tooltip that a test has to be able to control. So the window
says how much time has passed and this says what that means.

It is a value threaded through the loop, like the focus: there is exactly one pointer, so there
is exactly one of these, and no two controls can both believe they are being hovered.

### `no_hover`

```
pub fn no_hover() -> Hover
```

### `hovering`

```
pub fn hovering(h: Hover, name: string, by: int) -> Hover
```

The hover after another `by` milliseconds, with the pointer now on `name`.

**Moving to another control starts the wait again**, which is the behaviour that makes a tooltip
bearable: a pointer crossing a toolbar of eight buttons must not leave eight tips behind it, and
it will not, because each one resets the count the previous was accumulating.

### `tip_due`

```
pub fn tip_due(h: Hover, after: int) -> bool
```

Whether a tip should be showing: the pointer is on something and has been for long enough.

### `tip_words`

```
pub fn tip_words(h: Hover, laid: Laid) -> string
```

The words to show for the control the hover is on, or "" when there are none.

**"" is a real answer and the common one.** Most controls carry their name in their own face, and
a tip repeating a label a user is already reading is noise with a delay on it. So a tip appears
for a control whose words are not already on screen -- an icon button, a field with a range --
and for nothing else, without anything having to be switched off per control.

**`widget.node_named` and not `focus.node_of`, because a hover is not a focus.** `node_of`
answers "the laid node of the focused control" and finds **tab stops only** -- which is right
for a focus and wrong for a pointer, because a pointer lands on *parts*: a list's row, a tab, a
tree's item, none of which Tab can reach. So this answered "" for every part, and a part drawn
without words is exactly the control a tip exists for. Found by the health-record port, whose
sidebar collapses to a rail of icon-only list rows: 17 rows, 17 names the hover reported and 17
empty answers, with no tip ever shown and nothing to say why. The tests here had only ever asked
it about buttons, which are tab stops.

### `tip_layer`

```
pub fn tip_layer(words: string, over: Rect, within: Rect, f: Frame) -> widget.Layer
```

A tooltip over the window: the words in a raised panel, below the control if it fits and above it
if it does not.

**It takes no keys, and it is static text rather than a control.** A layer that took the
keyboard would let a tooltip steal the focus by appearing, which is the one thing it must never
do -- it is a consequence of the pointer resting, and the user never asked for it.

Its words are in the accessibility tree the way any drawn text is, and there is **no
`Role.Tooltip`** -- deliberately, not by omission. A tooltip here shows what the control already
declares (see `widget.tip_in`), so a reader gets those words from the control itself, announced
when the control is reached. A role of its own would add a second thing in the tree saying the
same sentence, and nothing would be reachable that is not reachable now.

## The entry

### `Ink`

```
pub struct Ink {
    from: int
    to: int
    colour: Color
}
```

A single-line text field.

The editing itself is `lib/ui/edit`: this holds an `Editor` and a `History` and decides which
input means which edit. Keeping the two apart is what let the grapheme and word rules be tested
against 32 cases with no widget, no layout and no font in sight.
A run of a field's text drawn in a colour of its own.

**Presentation, not model.** `edit` owns the text and knows nothing about this; a field with no
inks draws exactly as it always did, byte for byte, which is why `inked_run` answers a single
label when the list is empty. What it buys is the one thing a text pane could not do -- show
code as code -- without a second text stack beside `Entry`, which would have cost the caret, the
keyboard and the accessibility that made the response pane a field in the first place.

`from` and `to` are character indices into the field's text, the same units `edit` counts in.
The spans must be **in order and must not overlap**: a colourer walks the text once and that is
what walking it once produces, so sorting here would be paying every frame for a mistake nobody
makes.

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
    // How far the text is scrolled, in points. **A text area scrolls itself**, because the only
    // thing that knows where the caret went is the code that moved it -- a caller would have to
    // recompute the caret's line to follow it, which is the control's own arithmetic done twice.
    // Zero and unused for a one-line field.
    scroll: float
    // The bar down the trailing edge of a text area, and whether it is being dragged.
    //
    // **A control, where this used to be an ornament.** `paint_scroll_bars` walks a laid tree and
    // draws a bar over every `Scroll` it finds, which made a text area *look* scrollable and leave
    // the pointer with nothing to take hold of -- a thumb that could be seen, aimed at and pressed,
    // and that did not move. `List` has carried a real `ScrollBar` for exactly this reason; this is
    // the same arrangement, and the two now answer a drag the same way.
    //
    // Unused by a one-line field, which scrolls along its own axis and not down.
    bar: ScrollBar
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
```

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

### `password_of`

```
pub fn password_of(name: string, label: string, text: string) -> Entry
```

A field for a secret: drawn as bullets, announced as a password, and refusing the clipboard.

**One line, always.** A multi-line password is not a thing, and the multi-line widget would draw
the text, so `entry_widget` sends a masked field down the one-line path whatever `rows` says
rather than leaving a way to build a field that masks in one place and not the other.

### `shown_text`

```
pub fn shown_text(e: Entry) -> string
```

**What is drawn and what is published -- which must be the same string the caret is measured
against.** A mask is one bullet per character, so every index into the text is the same index
into this; measuring the real text and drawing the mask would put the caret under the wrong
bullet in any font where a bullet is not the width of an `e`.

### `entry_composing`

```
pub fn entry_composing(e: Entry, text: string, caret: int) -> EntryStep
```

**How an input method reaches a field, and why it is not an `Input`.** A composition is not
routed by what is under the pointer or by a focus that falls back; it belongs to whatever holds
the input-method context, which is the focused text field and nothing else. Making it an
`Input` would have put two arms on every button, list and tab strip in this module for an event
none of them can ever receive.

### `entry_composing_marked`

```
pub fn entry_composing_marked(e: Entry, text: string, caret: int, from: int, to: int) -> EntryStep
```

### `entry_commit`

```
pub fn entry_commit(e: Entry, text: string) -> EntryStep
```

What the method finally chose, which replaces the composition. **An empty commit is how a
composition is abandoned**: the method says it chose nothing, the pre-edit goes, and the text
is as it was.

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
pub fn entry_input(e: Entry, input: Input, bounds: Rect, f: Frame) -> EntryStep
```

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

```
pub fn caret_at_x(text: string, size: float, m: widget.Metrics, x: float, face: int) -> int
```

The same, measuring in a named face.

**A fifth argument rather than a changed signature**: the four-argument form is what a caller
with one face wants, which is most of them, and an entry in a second face is the one that has to
say so. `entry_input` passes `style.text.body.face`, so a field drawn in a face measures its clicks
in the same one -- a caret placed by the wrong face's widths lands between the wrong letters,
and only on the text where the two faces differ.

### `entry_widget`

```
pub fn entry_widget(e: Entry, f: Frame) -> widget.Widget
```

## The list

### `List`

```
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
    // **How roomy this list's rows are, when the look's answer is not the right one.** `None`
    // takes `Style.row_air`, which is what every list did and most should.
    //
    // **A window has more than one kind of list.** F343 moved row air onto the look and argued
    // that "a look saying how roomy its lists are is the look's job" -- which is right, and is
    // not the whole of it. The health-record port set `row_air: 24` to give its navigation rail
    // the forty-point rows of the web application it copies, and every combo box in the window
    // dropped a list of forty-point rows out of a twenty-eight-point box with it. One number was
    // being asked to describe a rail somebody scans and a dropdown somebody points at once.
    //
    // **Still air, and still added to a measured line**, so this is not `row_height` returning to
    // the control: the line is measured as before and only the air around it is the caller's. A
    // list that stored its height would be wrong the moment a look changed its type.
    air: Option<float>
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
pub fn list_of(name: string, label: string, items: [Value]) -> List
```

```
pub fn list_of(name: string, label: string, items: [Value], l: Locale) -> List
```

```
pub fn list_of(name: string, label: string, items: [string]) -> List
```

A list of plain text, which is what most of them are.

**Kept so that a list of words says nothing about types**, and so that every caller written
before `Value` existed still compiles -- `table_of` keeps the same door for the same reason.

### `list_text_at`

```
pub fn list_text_at(l: List, i: int) -> string
```

What a row says, spelled in the list's own locale.

**Here rather than at every call site**, because the alternative is each caller reaching for
`value_text` and the list's locale together, and the one that forgets prints a date in the root
locale while the rows above it are in the user's. Out of range gives "", which an empty list and
an untouched selection both make real.

### `no_values`

```
pub fn no_values() -> [Value]
```

An empty list of values, which an empty literal cannot be: see `autocomplete_of`.

### `as_text_values`

```
pub fn as_text_values(items: [string]) -> [Value]
```

Plain text as values.

Public as well as used here: a caller that builds its rows as strings -- a filter, a search --
needs the same conversion the text constructors do, and the alternative is every one of them
writing the same four-line loop.

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
```

### `list_at_end`

```
pub fn list_at_end(l: List, bounds: Rect, f: Frame) -> bool
```

Whether the last row is in view.

**Public, because an application that fetches on arrival needs to ask rather than wait**: a
screen whose first page does not fill the viewport has already reached the end before the user
has touched anything, and no scroll will ever tell it so.

### `list_input`

```
pub fn list_input(l: List, input: Input, bounds: Rect, f: Frame) -> ListStep
```

What an input does to a list.

`bounds` is the box the layout gave it, needed for two things: turning a click's y into a row,
and knowing how far one page is. `m` is what a row is measured with: see `row_height`, which the
widget lays the rows out by, so the hit test and the layout cannot disagree.

### `mark_slot`

```
pub fn mark_slot(style: Style) -> float
```

**How wide the column a row's mark sits in is**, whether or not the row has words beside it.

A list of files, or a menu, wants its marks in a column: an icon placed at the leading edge of
each row lines up only while every icon is the same width, and the moment one is not the column
wobbles. A slot fixes that -- and it is what makes **collapsing a menu to its marks leave them
exactly where they were**, which is the behaviour this was added for. Without it an icon-only
row centred its mark and a worded row did not, so the icons jumped sideways as the menu closed.

**Twice the body size**, which is the proportion the web application this was measured against
uses: a 28-point slot around a 15-point glyph. Derived rather than a number, so a look that
enlarges its type gets a wider column without saying so.

### `list_widget`

```
pub fn list_widget(l: List, f: Frame) -> widget.Widget
```

### `row_name`

```
pub fn row_name(l: List, index: int) -> string
```

A row's name, which focus and accessibility both address it by. The list's name and the index,
because a row has no name of its own and an index alone would collide with another list's.

## The tabs

### `Tabs`

```
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
```

A strip of tabs, one of them selected, choosing what is shown beneath it.

**Selection follows the focus.** Arrowing along the strip changes what is shown, with no second
keystroke to confirm -- the automatic-activation model, which is the one to use unless revealing
a panel is expensive. So `TabStep` reports only that the selection `moved`, and there is no
`activated` beside it: a tab that had to be opened as well as reached is the manual model, and
nothing has asked for it.

**One tab stop, as a list is**: Tab reaches the strip and the arrows move within it, so a window
with four tabs does not spend four stops on them. Each tab is a `widget.part_of` -- something a
click and a reader can reach and Tab cannot.

**Not a `List` with another role**, though a menu is one. A menu differs from a list only in what
a reader calls it, which is why `List.role` is a field; a tab strip differs in three ways -- it
runs along a row, its arrows are Left and Right, and it does not scroll -- and three differences
is a control.

### `tabs_of`

```
pub fn tabs_of(name: string, label: string, items: [string]) -> Tabs
```

### `TabStep`

```
pub struct TabStep {
    tabs: Tabs
    // The selection changed, so what is beneath the strip has to change with it.
    moved: bool
}
```

### `tabs_input`

```
pub fn tabs_input(t: Tabs, input: Input, bounds: Rect) -> TabStep
```

What an input does to a tab strip.

`bounds` is the box the layout gave the strip, which turns a press's x into a tab -- the same
trick `list_input` plays with y and `row_height`, and sound for the same reason: the widget lays
every tab out at `tab_width` from the strip's left edge, so the arithmetic here is the layout's
own and not a guess at it.

**A press selects, where a button fires on the release.** Dragging off a tab and letting go still
leaves it chosen, which is what every tab strip does and what a list does here: a tab is a place
to be rather than an action to commit to, so there is nothing to cancel.

### `tabs_widget`

```
pub fn tabs_widget(t: Tabs, f: Frame) -> widget.Widget
```

The strip as a widget: a `TabList` control holding one `Tab` part per label.

The parts carry `selected`, as a list's rows do, because that is what a reader reads -- "tab,
History, selected" -- and an index on the container would make every adapter work out for itself
which child the number meant.

### `tab_name`

```
pub fn tab_name(t: Tabs, index: int) -> string
```

A tab's name, which focus and accessibility both address it by. The strip's name and the index,
as a list's rows are: a tab has no name of its own and a bare index would collide with another
strip's.

## The tree

### `TreeRow`

```
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
```

A row of a tree, as the application hands it over.

**Flat with a depth, not nested.** A nested structure would have to be walked to draw, walked to
hit-test and walked again to move the selection, and every one of those walks is the same
projection -- the rows in order, with the ones under a closed branch left out. Doing it once, as
a list, is what lets the rest of this control be the list's code with an indent.

### `tree_row`

```
pub fn tree_row(label: string, depth: int) -> TreeRow
```

### `tree_branch`

```
pub fn tree_branch(label: string, depth: int, expanded: bool) -> TreeRow
```

### `Tree`

```
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
```

A tree: rows at depths, branches that open and close, one selected.

**One tab stop and a node per visible row**, which is `List`'s bargain again. Not a `List` with
a depth, though: the arrows mean different things here -- Left closes a branch or climbs to its
parent, Right opens one or steps into it -- and that contract is the difference between a tree
and an indented list.

### `tree_of`

```
pub fn tree_of(name: string, label: string, rows: [TreeRow]) -> Tree
```

### `visible_rows`

```
pub fn visible_rows(t: Tree) -> [int]
```

The rows that are on screen, in order, as indices into `rows`: everything except what sits under
a closed branch. **One pass, not a visibility test per row** -- the test is "is any ancestor
closed", and walking back up from each row to answer it is the same walk done n times.

### `TreeStep`

```
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
```

### `tree_input`

```
pub fn tree_input(t: Tree, input: Input, bounds: Rect, f: Frame) -> TreeStep
```

What an input does to a tree.

`bounds` turns a press into a row the way the list does, and into a *column* besides: a press on
the mark at the left of a branch opens or closes it, and a press anywhere else on the row selects
it. Both computed from `indent_for`, which the widget also lays out by, as `row_height` is for
the rows -- and `m` is what measures a row: see `row_height`.

### `parent_of`

```
pub fn parent_of(t: Tree, index: int) -> int
```

The row this one hangs under: the nearest earlier row one level shallower. -1 at the top.

### `tree_widget`

```
pub fn tree_widget(t: Tree, f: Frame) -> widget.Widget
```

The tree as a widget: a `Tree` control over one `TreeItem` part per visible row.

**The parts nest, though the rows are flat and the layout is a plain column.** A reader asks a
node for its children, so a branch whose rows were siblings of it would tell a user that seven
things exist and nothing about which is inside which -- an indented list again, from the one
side that cannot see the indent. The first version of this did exactly that and the AT-SPI dump
showed it: seven tree items in a row under the tree.

Nesting costs nothing in layout, because a part adds no box of its own and the rows keep their
absolute indent: a column of columns of fixed-height rows stacks exactly as one column of them
does, which is what keeps the hit test's `y / row_height` true.

### `tree_row_name`

```
pub fn tree_row_name(t: Tree, index: int) -> string
```

A row's name. The tree's name and the index **into `rows`, not into the visible ones**, so that
opening a branch above a row does not rename it.

## The scroll bar

### `ScrollBar`

```
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
```

Where a scrolled view is, and how much of it there is, drawn beside what it scrolls.

**A view of someone else's offset, not the owner of one.** The offset lives in the control that
scrolls -- a `List`, a `Tree`, an `Entry` -- and a bar keeping a second copy is two numbers that
can disagree about one thing. So this holds only what a bar has that nothing else does: whether
a drag is in progress and where the thumb was taken hold of. Everything else is passed in and
handed back.

**Beside what it scrolls, never over it.** An overlaid bar is inside the scrolled control's own
box, so a press on the bar is a press the control reads as a row -- and the control would have
to know the bar's width to tell them apart, which is the bar's geometry computed in two places.
Laid out as a sibling, the two boxes do not overlap and neither has to know about the other.

**Not a tab stop.** Every platform agrees: the content takes the focus and its arrows scroll it,
so a scrollbar in the tab order is a stop that does nothing a keyboard user needs. It is also
why this control publishes no value -- a reader walks the rows, which this layer publishes
whether they are scrolled into view or not, so the bar is an affordance for the eye and the one
reader who needs it is the one who cannot see.

### `scroll_bar_of`

```
pub fn scroll_bar_of(name: string, step: float) -> ScrollBar
```

### `Thumb`

```
pub struct Thumb {
    start: float
    length: float
}
```

How tall the thumb is and where it starts, inside a track of `track` points.

**One function, used by the widget and by every gesture**, which is the rule this layer keeps
learning: a thumb drawn by one calculation and dragged by another is a thumb that jumps under
the pointer. `length` is the view's share of the content, floored at `min_thumb` so that a very
long list still leaves something to take hold of; `start` is the offset's share of the overhang,
spread over the track that is left.

### `thumb_of`

```
pub fn thumb_of(style: Style, track: float, content: float, view: float, offset: float) -> Thumb
```

### `on_the_bar`

```
pub fn on_the_bar(style: Style, bounds: Rect, p: Point, content: float, view: float) -> bool
```

Whether a point is in the strip a bar is drawn in, down the inside of a box's right edge.

**Over the content, not beside it**, which is what overlay bars do everywhere and what keeps
this from being a layout change: a gutter reserved in a control's own box would have to be
reserved by the control, measured by the painter and subtracted by the hit test, and the three
would be three chances to disagree about one number. What it costs is the last few points of a
row, which on a list of text is the empty end of a line.

### `scrolls`

```
pub fn scrolls(content: float, view: float) -> bool
```

Whether there is anything to scroll. An application asks before placing a bar at all; a bar with
nothing to do draws an empty track rather than vanishing, so the layout does not move when a
list grows past its box.

### `ScrollBarStep`

```
pub struct ScrollBarStep {
    bar: ScrollBar
    // The offset the scrolled control should take. Handed back rather than stored, because the
    // control owns it.
    offset: float
    moved: bool
}
```

### `scroll_bar_input`

```
pub fn scroll_bar_input(
    b: ScrollBar,
    input: Input,
    bounds: Rect,
    content: float,
    view: float,
    offset: float,
    f: Frame
) -> ScrollBarStep
```

What an input does to a bar, given the numbers whose owner is elsewhere.

A press on the thumb takes hold of it; a press on the track above or below pages towards the
pointer by a viewful, which is what a track click does everywhere; a move while held drags; a
release lets go. A wheel scrolls by `step`.

### `scroll_bar_widget`

```
pub fn scroll_bar_widget(
    b: ScrollBar,
    width: float,
    track: float,
    content: float,
    view: float,
    offset: float,
    f: Frame
) -> widget.Widget
```

The bar as a widget: a track with the thumb inside it.

The thumb is a fixed box above a flexible one, which is what places it without arithmetic the
hit test would have to repeat -- the space above the thumb is `thumb_of`'s `start`, drawn as a
gap, so the layout and `scroll_bar_input` are reading the one calculation.

### `thumb_name`

```
pub val thumb_name = "thumb"
```

The name the thumb's box carries in the laid-out tree, so a test can find it.

### `paint_scroll_bars`

```
pub fn paint_scroll_bars(l: Laid, over: scene.Scene, f: Frame) -> scene.Scene
```

Every viewport in a laid-out tree gets a bar, painted over the scene that is already there.

**A pass over the laid tree, not a widget**, and that is the whole answer to the question F284
could not settle: a thumb's length is the view's share of the content, and nothing knows the
view extent until the layout has run. Measuring that by laying the window out twice costs 5 ms
a frame at 200 rows. Here the number is simply there -- a viewport's own box is the view, the
box its child was placed in is the content, and `Viewport.offset_y` is where it sits. This is
the seam `widget.paint_focused` already uses for the focus ring, which could not be known at
widget-build time either.

**An indicator, not a control.** It says there is more and where you are; the wheel and the
arrows are what move it. Dragging needs the bar to have a box of its own that a press can be
routed to, which means the scrolled control reserving a gutter in its own layout -- a different
change, with `scroll_bar_input` already written and tested for when it happens.

Drawn **inside** the viewport's right edge, which is where an overlay bar goes and why it is
translucent: a few points of the content are behind it, and over a list of text that is the
trailing end of a line nobody reads.

**Prefer `paint_scrolled`.** This appends every bar in the tree to a finished scene, and a scene
is a flat list in paint order, so the bars land over *everything* that was painted -- including a
dialog and its scrim. A window with no layers cannot tell the difference; one with a dialog draws
the scrolled list behind it a bright, undimmed bar straight down the front of the dialog. Kept
because a caller that composes its own scene a piece at a time still wants it, one piece at a
time.

### `paint_scrolled`

```
pub fn paint_scrolled(
    l: Laid,
    m: widget.Metrics,
    focused: string,
    ring: Color,
    width: float,
    f: Frame
) -> scene.Scene
```

`widget.paint_focused`, with every viewport's scroll bar drawn **over that viewport and under
whatever is painted after it**: the one call an application's loop needs.

The two-call form above cannot do this and no caller of it can either, because the order is
already gone by the time there is a scene to append to. `widget.paint_adorned` draws an
ornament where its node sits in the walk, which is what a bar is.

### `bar_gutter`

```
pub fn bar_gutter(style: Style) -> float
```

How wide a scroll bar is, **as one number shared by everything that draws one, hit-tests one or
makes room for one**. Three numbers here would be a bar you can see in one place, grab in
another and whose gutter is a third width.

### `viewport_of`

```
pub fn viewport_of(name: string, offset_x: float, offset_y: float, child: widget.Widget, f: Frame) -> widget.Widget
```

A viewport with the room this module's bar needs kept clear at its trailing edge.

**Named `viewport_of` and not `scrolled`**, because `scrolled` is already a private step
function in this module and ziman reads one name as one overload set: a public member beside a
private one is a set nothing outside can call, and the error says the *public* function is
private.

**The call an application's screen makes**, in place of `widget.named_scroll`. A bar is drawn
over the last `bar_gutter` points of a viewport's content, and `widget` cannot reserve them
because it neither draws the bar nor has a look to ask how wide it is. Written out at the call
site instead, the number would be in as many places as there are screens, and the one that was
not updated would be a viewport whose content ran under its own bar.

## Menus and dialogs

### `Menu`

```
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
```

A title on a menu bar and the items that drop from it.

**A menu is a title and a `List`**, not a new widget with its own selection and scrolling. The
popup is a list of rows with one highlighted, scrolled when it is long, moved by the same arrows;
the only honest difference is what a reader calls it, which is why `List.role` is a field.

### `menu_of`

```
pub fn menu_of(name: string, title: string, items: [string]) -> Menu
```

### `collapsed_text`

```
pub fn collapsed_text(m: Menu) -> string
```

What the collapsed control shows: the chosen value for a choice, the title for a menu.

## The combo box

### `ComboBox`

```
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
```

**A picker of values: a collapsed box showing the current one, and a popup list to change it.**

**Its own control since 2026-11-16, where it was a `Menu` carrying `Role.ComboBox` before.** The
`Menu` struct's comment argued the other way and is worth answering rather than deleting: "one
structure, because a dropdown *is* a popup list with a collapsed title, and building a second one
would have been a copy of the popup, the keyboard handling and the dismissal to change two role
names". The first half of that is right and this control depends on it -- the popup, the
dismissal and the open keyboard are **shared**, not copied, through `open_popup_input` and
`list_widget`. The second half was not: a menu and a combo box differ in the half that was never
examined, which is what happens when the control is **closed**.


```
closed         menu                    combo box
Up / Down      nothing                 change the value, without opening
Home / End     nothing                 first / last value
a letter       nothing                 jump to the next value starting with it
Enter, Space   open                    open
Alt+Down       nothing                 open
```

A menu has nothing to change while closed -- its items are commands and picking one runs it. A
combo box holds a value, and every platform lets the arrows move it without the list ever
appearing. That is not a role name; it is a different control, and it was missing: `curly gui`
showed it as a method picker whose Down key opened a list instead of selecting `POST`.

**Not editable.** The value is always one of `items`. An editable combo box -- a text field with
a dropdown attached -- is a different control again, joining this to the whole of `Entry`
(caret, selection, input method), and it is deliberately left for when something asks.

### `ComboBoxStep`

```
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
```

### `combo_box_of`

```
pub fn combo_box_of(name: string, label: string, items: [Value], selected: int) -> ComboBox
```

```
pub fn combo_box_of(
    name: string,
    label: string,
    items: [Value],
    selected: int,
    l: Locale
) -> ComboBox
```

```
pub fn combo_box_of(name: string, label: string, items: [string], selected: int) -> ComboBox
```

A picker of plain words, which is what most of them are. See `list_of`'s own text door.

### `combo_box_value`

```
pub fn combo_box_value(c: ComboBox) -> string
```

The value it is showing, or "" when nothing is chosen -- which an empty item list makes real.

### `combo_box_chosen`

```
pub fn combo_box_chosen(c: ComboBox) -> Value
```

**The value itself, not its spelling.** `combo_box_value` is what the box *shows*; this is what
the application *has*, and the two stopped being the same thing the moment a combo could hold a
date. An application that read the text back and parsed it would be undoing the formatting it
just asked for, in the locale it asked for it in.

### `combo_box_text`

```
pub fn combo_box_text(c: ComboBox) -> string
```

**What the collapsed box shows: the value, or the label when nothing is chosen.**

A picker that rendered blank looks broken rather than unset, and an empty selection is real --
`combo_box_of` stores -1 for an index outside the items. Separate from `combo_box_value`, which
answers "" in that case and is what the accessibility node publishes: a reader must hear that
nothing is chosen, where an eye needs something in the box to read.

### `combo_box_widget`

```
pub fn combo_box_widget(c: ComboBox, f: Frame) -> widget.Widget
```

The collapsed control: what it shows, and the same face every other control wears.

### `combo_box_popup_layer`

```
pub fn combo_box_popup_layer(c: ComboBox, at: Rect, f: Frame) -> widget.Layer
```

The popup, as a layer. The same one a menu uses -- a list is a list.

### `combo_box_input`

```
pub fn combo_box_input(
    c: ComboBox,
    input: Input,
    at: Rect,
    popup_at: Rect,
    f: Frame
) -> ComboBoxStep
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
pub fn menu_input(
    menu: Menu,
    input: Input,
    title_bounds: Rect,
    popup_bounds: Rect,
    f: Frame
) -> MenuStep
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
pub fn menu_title_widget(menu: Menu, f: Frame) -> widget.Widget
```

The title as it sits on the bar. A control in its own right, so Tab reaches the menu and a reader
announces it.

### `menu_popup_layer`

```
pub fn menu_popup_layer(menu: Menu, at: Rect, f: Frame) -> widget.Layer
```

The open popup, as a layer for the **root's** layer list.

`at` is where the caller decided it goes, from the title's laid bounds -- see `widget.layer_at`
for why it cannot be a child of the title. A placed layer, so a click away from it falls through
to what is beneath as well as closing the menu.

Takes the keyboard while it is open, because an open menu owns the arrows.

## The date field

### `CalendarView`

```
pub enum CalendarView {
    Days
    Months
    Years
}
```

Which grid the calendar is showing.

**Three views and not a separate popup each**, which is what every date picker settled on: the
header is a button, and pressing it zooms out -- days to months to years -- while choosing
zooms back in. A year twelve years away is then four presses rather than a hundred and
forty-four on a month arrow.

### `DateField`

```
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
```

A date, picked off a calendar.

**A value picker and not a field with a button beside it**, which is the combo box's shape and
the combo box's argument: one box showing what is chosen, one mark saying it opens, one tab
stop, one thing a reader names. A text field with a calendar button next to it is two controls
that have to agree about one value, and the first thing they disagree about is what a
half-typed date means.

**Not editable in this version**, deliberately, which is why the value is a `LocalDate` and not
an `Entry`. A field that could be typed into would have to hold `2026-11-` -- not a date, and
plainly on the way to one -- so the value would have to be text and every reader of it would
have to cope with it not being a date yet. That argument is sound and it is the number field's,
but it is only worth paying for when there is typing to support. There is not, so the value
here is a date, and the day typing arrives the `Entry` comes back with it.

### `DateStep`

```
pub struct DateStep {
    field: DateField
    changed: bool
}
```

### `date_field_of`

```
pub fn date_field_of(name: string, label: string, on: time.LocalDate) -> DateField
```

```
pub fn date_field_of(name: string, label: string, on: time.LocalDate, l: Locale) -> DateField
```

### `date_in`

```
pub fn date_in(d: DateField) -> time.LocalDate
```

The date the field holds.

**A `LocalDate` and not an `Option`**, which is what a picker buys: there is no way to put a
non-date into this control, so there is no way for a caller to have to check.

### `date_text`

```
pub fn date_text(d: DateField) -> string
```

What the box shows.

**ISO, and not a locale's own order.** `yyyy-MM-dd` is unambiguous everywhere, where `03/04`
means two different days on two sides of an ocean -- and this library has no source for each
locale's date *pattern*, only for its month and day names. Inventing one here would be guessing
in a format whose whole job is to be read correctly.

### `calendar_box`

```
pub fn calendar_box(d: DateField, laid: Laid, within: Rect, f: Frame) -> Rect
```

Where the calendar goes: under the field, pushed back inside the window.

**One function, asked by the layer and by the hit test**, which is `context_menu_box`'s rule and
the reason it is a rule: worked out twice, a click could land on the fourth of the month while
the fifth was drawn under the pointer.

### `date_field_input`

```
pub fn date_field_input(d: DateField, input: Input, laid: Laid, within: Rect, f: Frame) -> DateStep
```

### `date_field_widget`

```
pub fn date_field_widget(d: DateField, f: Frame) -> widget.Widget
```

**The combo box's shape exactly**: the value, a flexible gap, and the mark that says it opens.
One box, one tab stop, one thing a reader names -- where a text field with a calendar button
beside it is two controls that have to agree about one value.

### `calendar_layer`

```
pub fn calendar_layer(d: DateField, laid: Laid, within: Rect, f: Frame) -> Option<widget.Layer>
```

The month grid, or nothing while it is shut.

## The toolbar

### `ToolItem`

```
pub enum ToolItem {
    Command(Button)
    // A rule between two groups of commands. **Not a `Separator` control**, deliberately: that
    // role means a draggable divider here, and `cursor_for` turns it into a resize arrow from the
    // shape of its box. A toolbar's rule is decoration and must not promise a drag.
    Divider
}
```

A strip of commands, which is not a row of buttons.

**One tab stop, and the arrows move inside it.** That is the whole reason this is a control and
not an arrangement: a row of eight buttons puts eight stops in the tab order, so a user reaching
the field below it presses Tab nine times. Every platform's toolbar is one stop that the arrows
walk, which is also what `Role.ToolBar` tells a reader -- that the arrows will work here and
that Tab will leave.

**The commands are drawn by `button_inside`**, the same function an ordinary button is drawn by,
so a theme cannot reach one and not the other. What they are *not* is `Focusable`s of their own,
which is exactly the tab strip's arrangement: parts for a click and for a reader, one stop for
Tab.

### `Toolbar`

```
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
```

### `ToolbarStep`

```
pub struct ToolbarStep {
    toolbar: Toolbar
    // The name of the command that fired, or "".
    //
    // **A name and not an index**, because an application routes by name everywhere else here and
    // because an index renumbers the moment a divider is added above it.
    fired: string
}
```

### `toolbar_of`

```
pub fn toolbar_of(name: string, label: string, items: [ToolItem]) -> Toolbar
```

### `tool_name`

```
pub fn tool_name(t: Toolbar, index: int) -> string
```

The part name a command answers to: the strip's name and the index, as a tab's is.

### `toolbar_more_name`

```
pub fn toolbar_more_name(t: Toolbar) -> string
```

The trailing button that opens the overflow.

### `toolbar_fits`

```
pub fn toolbar_fits(t: Toolbar, width: float, f: Frame) -> int
```

How many of the leading items fit in `width`, measured rather than estimated.

**`widget.measure` is the same measurement layout will make**, so the strip cannot decide that
seven fit and then draw an eighth over the edge. Room for the overflow button is reserved before
anything is counted when not everything fits -- which is the arithmetic that is wrong in every
toolbar that hides one command behind a "more" button wide enough for three.

### `toolbar_widget`

```
pub fn toolbar_widget(t: Toolbar, width: float, f: Frame) -> widget.Widget
```

### `toolbar_layer`

```
pub fn toolbar_layer(t: Toolbar, laid: Laid, width: float, within: Rect, f: Frame) -> Option<widget.Layer>
```

The overflow popup, or nothing while it is shut.

### `toolbar_input`

```
pub fn toolbar_input(
    t: Toolbar,
    input: Input,
    laid: Laid,
    width: float,
    within: Rect,
    f: Frame
) -> ToolbarStep
```

**Takes the laid tree, where a tab strip takes a box and divides it.** A strip of tabs is a row
of equal columns, so arithmetic finds the one under a point; a toolbar's commands are an icon,
a word and a rule, every one a different width. Their laid boxes are the only thing that knows
where they ended up, and `cursor_at` reads parts out of the tree for the same reason.

## The chart

### `ChartView`

```
pub struct ChartView {
    name: string
    label: string
    data: chart.Chart
    // The row a keyboard or a pointer is on, or -1. **A chart's selection is a reading**, which is
    // what a hover readout shows and what a reader is told.
    at: int
}
```

A chart, and the table a reader hears instead of it.

**A chart is the one thing in a window a screen reader cannot read at all.** The usual answers
are a text summary, which is a guess at what matters, or nothing, which is most of the web.

**This library's answer is that a chart's accessible form is its table.** `Table` already holds
`Value`, already spells a number or a date in a locale and already sorts; a reader already knows
how to walk one. A chart and a table of the same numbers are the same `[[Value]]`, so what a
reader hears cannot drift from what is drawn -- there is one of it.

**And so a chart *is* focusable**, unlike a card or a screen. It carries information reachable
no other way, so it is a tab stop and its rows are what it announces.

### `chart_view_of`

```
pub fn chart_view_of(name: string, label: string, data: chart.Chart) -> ChartView
```

### `chart_rows`

```
pub fn chart_rows(v: ChartView) -> [[Value]]
```

The chart as a table: one row per label, one column per series.

**Built from the same `Chart` the drawing is built from**, which is the whole point: a reader
and an eye are shown the same numbers because there is only one set of them.

### `chart_columns`

```
pub fn chart_columns(v: ChartView) -> [Column]
```

### `chart_said`

```
pub fn chart_said(v: ChartView, l: Locale) -> string
```

What the control says about the reading it is on.

### `ChartStep`

```
pub struct ChartStep {
    view: ChartView
    // The reading moved. What an application watches to show a readout beside the chart.
    moved: bool
}
```

### `chart_input`

```
pub fn chart_input(v: ChartView, input: Input, bounds: Rect, f: Frame) -> ChartStep
```

**Left and right walk the readings, which is the whole keyboard a chart needs.** A chart a
keyboard cannot walk is a picture, and a picture of numbers is what this is trying not to be.

### `chart_slot_at`

```
pub fn chart_slot_at(v: ChartView, p: Point, bounds: Rect, f: Frame) -> int
```

Which reading a point is over.

**Worked out from the same `plot_box` the drawing uses**, so the readout cannot name one slot
while the pointer stands over another -- the defect the calendar cost twenty-two points to.

### `chart_style_of`

```
pub fn chart_style_of(f: Frame) -> chart.ChartStyle
```

The look's own colours and sizes, as a chart wants them.

### `chart_widget`

```
pub fn chart_widget(v: ChartView, f: Frame) -> widget.Widget
```

## The card

### `Card`

```
pub struct Card {
    panel: Panel
    // Beside the title, and decoration: the title is what a reader hears, so an icon that
    // repeated it would be said twice. See `Button.icon`, which is the one case where it is not.
    icon: Option<widget.Icon>
}
```

A titled box with a body and, if it wants them, actions.

**A function and not a control** (P3, decided 2026-11-20). The question turned out not to be
control-or-function but **focusable or not**: a `Control` is a tab stop, and the sixty-two cards
this exists for would be sixty-two extra presses between a user and what they came for.
Everything a card wanted a control *for* it gets without being one -- a named group gives a
reader "Medications, group" and gives a click a name to report.

**A card is a panel with a title**, which is why this holds one rather than restating it. The
target's own `CardShell` takes a title, an icon, `loading` and `empty`; the last two are
`Panel`'s four states, so every card gets the rule that "no medications recorded" and "not yet"
are different statements, rather than sixty-two chances to forget it.

### `card_of`

```
pub fn card_of(name: string, title: string, empty: string) -> Card
```

### `card_loading`

```
pub fn card_loading(c: Card) -> Card
```

### `card_ready`

```
pub fn card_ready(c: Card) -> Card
```

### `card_failed`

```
pub fn card_failed(c: Card, why: string) -> Card
```

### `card_widget`

```
pub fn card_widget(c: Card, content: widget.Widget, actions: widget.Widget, f: Frame) -> widget.Widget
```

The card: a header, a rule, and the panel's body under it.

**The actions sit in the header and not under the body**, which is where the target puts them
and where they stay put: a row of buttons below a body that grows is a row that moves every time
the data changes.

## The badge

### `Badge`

```
pub struct Badge {
    text: string
    // **Loud wears the accent**, for a count that is asking to be noticed: unread messages, things
    // needing attention. Quiet is the ordinary face, for a status word that is merely true.
    loud: bool
}
```

A count or a word beside the thing it belongs to.

**The whole of the difficulty is accessibility**, not drawing. A badge drawn beside a label is
two nodes a reader meets separately -- "Medications", then "3" -- where the user sees one fact.
So it is a **field on a control** rather than a widget beside one, exactly as `Button.icon` is,
and the control folds it into what it announces.

### `badge_of`

```
pub fn badge_of(text: string) -> Badge
```

### `count_badge_of`

```
pub fn count_badge_of(n: int) -> Option<Badge>
```

A count, capped.

**The cap is here and not in twelve callers**, which is the only reason this is a constructor
rather than a literal: "99+" is a convention, and twelve applications each choosing where to cut
off would give one window three different answers. Zero is not a badge at all -- a count of
nothing is nothing to say, and a badge showing "0" is a dot that means "no news" in a place a
user has learnt means "news".

### `badge_widget`

```
pub fn badge_widget(b: Badge, f: Frame) -> widget.Widget
```

## The search field

### `Search`

```
pub struct Search {
    entry: Entry
}
```

A field for searching, which is an entry and two things beside it.

**Worth having once rather than fifteen times.** It is a composition -- an entry, a mark, a
button -- but the button is a control *inside* a control, which is the arrangement the number
field had to get right and the tooltip demo had to name. Fifteen applications assembling it
would be fifteen chances to make the clear button a second tab stop.

### `SearchStep`

```
pub struct SearchStep {
    search: Search
    // The text changed, which for a search field is the only output that matters: there is nothing
    // to commit, because what is typed *is* the query.
    changed: bool
}
```

### `search_of`

```
pub fn search_of(name: string, label: string) -> Search
```

### `search_text`

```
pub fn search_text(s: Search) -> string
```

### `search_clear_name`

```
pub fn search_clear_name(s: Search) -> string
```

The clear button's own name, which a caller needs to route a press by.

### `search_input`

```
pub fn search_input(s: Search, input: Input, bounds: Rect, f: Frame) -> SearchStep
```

**Escape clears the field**, which is what every search box does and the reason the key is worth
spending here: a user who wants their list back should not have to select the text first.

**A press on the clear mark clears it and leaves the focus in the field**, because the next
thing a user does after clearing a search is type another one. A clear button that took the
focus would make them click back.

### `search_widget`

```
pub fn search_widget(s: Search, f: Frame) -> widget.Widget
```

The field: the magnifier, the text, and the clear mark once there is something to clear.

**The clear mark is a part and not a control**, so it is somewhere a click lands and a reader
can name without becoming a second stop in the tab order. A search box that put its clear button
in the tab order would make Tab out of a search field take two presses, in every form that has
one.

**It appears only when there is something to clear**, because a button that does nothing is
worse than no button: the pointer changes over it and promises otherwise.

## The panel

### `Waiting`

```
pub enum Waiting {
    // Nothing has been asked for yet.
    Idle
    Loading
    // There is content, and it is what the caller passed.
    Ready
    // It did not work. The words are the caller's, because only the caller knows what failed.
    Failed(string)
}
```

What a panel has to show, which is four things and not two.

**A loading flag is not enough**, which is the whole reason this is an enum. A panel that is
empty and one that is still fetching look the same to a boolean and must not look the same to a
user: "no records" and "not yet" are different facts, and a panel that showed "no records" while
a query was still running has told the user something untrue. A failure is a third fact again,
and the one most often drawn as an empty list.

### `Panel`

```
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
```

A panel, and what it is doing.

### `panel_of`

```
pub fn panel_of(name: string, label: string, empty: string) -> Panel
```

### `panel_loading`

```
pub fn panel_loading(p: Panel) -> Panel
```

### `panel_ready`

```
pub fn panel_ready(p: Panel) -> Panel
```

### `panel_failed`

```
pub fn panel_failed(p: Panel, why: string) -> Panel
```

### `said_waiting`

```
pub fn said_waiting(p: Panel, holds_anything: bool) -> string
```

What a reader is told the panel is doing.

**The words a live region speaks**, which is why this is public and why F329 came first: a panel
that finishes while the user is looking elsewhere has to be able to say so, and before there was
a live region every application would have invented its own way of not saying it.

### `waiting_is_urgent`

```
pub fn waiting_is_urgent(p: Panel) -> bool
```

Whether a failure should interrupt the reader rather than wait its turn.

**Only a failure does.** A panel that finished is news the user can hear when the reader gets to
it; a panel that could not is news they are about to act on without.

### `spinner_widget`

```
pub fn spinner_widget(phase: float, ink: Color, side: float) -> widget.Widget
```

### `panel_widget`

```
pub fn panel_widget(p: Panel, content: widget.Widget, f: Frame) -> widget.Widget
```

The panel: its content when it is ready, and what it is doing when it is not.

**What it is not showing is not laid out at all**, which is the expander's rule (F304) and here
it is what makes the keyboard behave: a Tab cannot land in a form that is still being fetched,
because there is no form there yet. A panel that drew a spinner *over* its content would have
every control underneath still reachable, still focusable, and still editable -- which is how a
user edits a record that is about to be replaced by the one still arriving.

**The live region is part of the panel and not the caller's job** (F329). A panel that finishes
while the user is looking elsewhere has to say so, and if each application wrote that itself
they would each write it differently -- which is the argument that pulled the live region
forward in the first place.

## The autocomplete

### `Autocomplete`

```
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
```

A field that suggests from a known set as it is typed into.

**The most asked-for control in the application this toolkit is being measured against**: twelve
bespoke ones over one generic dropdown. Every piece of it already existed here -- the entry, the
filtered list, the popup layer -- and none of the assembly did, which is where the behaviour
lives.

**It is a combo box that can be typed into**, and the differences from `ComboBox` are the whole
design: the list narrows as you type, the set is held whole so the control can say how many of
it match, and a value that is in no suggestion is either refused or kept depending on what the
field is for.

### `AutocompleteStep`

```
pub struct AutocompleteStep {
    field: Autocomplete
    // The value committed by this input, or "" for none. **A string and not an index**, because a
    // free-text field commits something that is in no list and an index could not name it.
    chosen: string
    changed: bool
}
```

### `autocomplete_of`

```
pub fn autocomplete_of(name: string, label: string, all: [string]) -> Autocomplete
```

### `autocomplete_text`

```
pub fn autocomplete_text(a: Autocomplete) -> string
```

What the user has typed.

### `autocomplete_matches`

```
pub fn autocomplete_matches(a: Autocomplete) -> [string]
```

The candidates the typed text matches.

**Case-insensitive substring, and the case folding is deliberately the locale-free one.**
`to_lower(s, locale)` would be more correct for *display* and is wrong here: under `tr_TR` it
maps `I` to `ı`, so a Turkish user typing an ASCII `i` would stop matching `Ibuprofen`. Matching
wants one fold for everybody, which is what the single-argument form gives.

**It is ASCII-only, and that is a real limit rather than a simplification.** `to_lower` leaves
every byte above 0x7f alone, so `Ärztin` does not match `ärztin`. Full case folding is a
roadmap item (`lib/core/locale.zm`'s header says so); until it lands, an accented candidate
matches only if its case already agrees.

### `autocomplete_said`

```
pub fn autocomplete_said(a: Autocomplete) -> string
```

What a reader is told about the narrowing.

**This is the half of an autocomplete everybody leaves out**, and it is why the whole set is
held rather than a filtered one: a list that silently goes from a hundred rows to three has told
a sighted user something and a blind user nothing.

**It rides on the control's description**, which is where it can go today: `a11y` has no live
region, so there is no way to say a thing *as it changes* -- only to say it when a reader next
reaches the control. That gap is recorded rather than worked around.

### `autocomplete_highlighted`

```
pub fn autocomplete_highlighted(a: Autocomplete) -> string
```

The highlighted suggestion, or "" when none is.

### `autocomplete_input`

```
pub fn autocomplete_input(
    a: Autocomplete,
    input: Input,
    bounds: Rect,
    popup: Rect,
    f: Frame
) -> AutocompleteStep
```

What an input does to the field.

**The five decisions this control had to take**, written here because they are what separates a
usable autocomplete from the twelve hand-rolled ones it is meant to replace:

 1. **Typing filters and opens**, and clears the highlight. A highlight that survived the text
    changing under it would point at a row that is no longer the row it pointed at.
 2. **The arrows do not write into the field.** Some toolkits fill the entry as you arrow
    ("inline autocomplete"); it reads well and it means a user who arrows past their answer has
    had their typing destroyed. Moving the highlight is looking, Enter is choosing -- which is
    the calendar's rule and the number field's.
 3. **The popup stays open when nothing matches**, and says so. A popup that vanished would be
    indistinguishable from one that broke, and the user would not know whether to keep typing.
 4. **Enter with nothing highlighted does not take the first match.** That is how a form ends up
    holding a diagnosis nobody chose. It commits the typed text when the field allows free text
    and abandons it when the field does not.
 5. **Escape shuts the popup and keeps the typing**; a second Escape gives the typing up. One
    key, one step, which is what the calendar's Escape does between its views.

### `autocomplete_widget`

```
pub fn autocomplete_widget(a: Autocomplete, f: Frame) -> widget.Widget
```

The field itself: an entry that says what it is and how many suggestions it has.

### `autocomplete_popup_layer`

```
pub fn autocomplete_popup_layer(a: Autocomplete, at: Rect, f: Frame) -> Option<widget.Layer>
```

The suggestions, or nothing while the field is shut.

**An `Option`, as the context menu's is**: a caller holding a closed field has nothing to anchor
an empty layer to, and the menu demo's zero-sized layer holding `Nothing` is what that costs.

### `autocomplete_popup_height`

```
pub fn autocomplete_popup_height(a: Autocomplete, f: Frame) -> float
```

## The context menu

### `ContextMenu`

```
pub struct ContextMenu {
    menu: Menu
    // Where it was asked for, in the window's own points. Meaningless while it is shut, which is
    // why nothing reads it unless `menu.open`.
    at: Point
}
```

Commands offered where the pointer is.

**A menu that hangs off nothing**, which is the whole of the difference and all of the new
work. A `Menu` is opened by pressing a title that says "File", so the title's laid box answers
both where the popup goes and how wide it is -- which is why `menu_popup_layer` can leave that
arithmetic to its caller. A context menu is opened by a gesture at a point. There is no box to
take anything from, so it has to measure its own rows and place itself, and to do that without
falling off the edge of a window whose size it is told rather than knows.

**The rows, the arrows, Home and End, Escape and the highlight are `Menu`'s**, reused whole
rather than written a second time. What is new is the gesture, the measuring and the placement.

### `context_menu_of`

```
pub fn context_menu_of(name: string, label: string, items: [string]) -> ContextMenu
```

### `context_menu_open`

```
pub fn context_menu_open(c: ContextMenu) -> bool
```

### `context_menu_box`

```
pub fn context_menu_box(c: ContextMenu, within: Rect, f: Frame) -> Rect
```

Where the popup goes: at the pointer, pushed back inside the window.

**The same `held_within` a tooltip uses**, and for the same reason: a menu hanging off the
right-hand edge is a menu with its commands cut in half. Downwards unless there is no room
below, in which case upwards from the pointer -- which is what a secondary click near the foot
of a window needs, and the case a developer with a large screen never meets.

**One function, asked by both the layer and the input.** The popup's box decides where it is
painted *and* whether a click landed in it; worked out twice, a menu could be dismissed by a
click on one of its own rows.

### `context_menu_key`

```
pub fn context_menu_key(input: Input) -> bool
```

Whether this input is the keyboard asking for a context menu.

**A context menu reachable only by the mouse is an inaccessible control**, and the two keys that
reach one are the same everywhere: the Menu key beside the right-hand Alt, and Shift+F10 for the
keyboards that have no Menu key. Neither is in `Key`, because neither is a key any *control*
dispatches on -- they arrive as `Other`, which is what that variant is for.

**Where it then opens is the application's to say**, because only the application knows where
the focus is. `context_menu_opened_at` is the other half.

### `context_menu_opened_at`

```
pub fn context_menu_opened_at(c: ContextMenu, p: Point) -> ContextMenu
```

Opened at a point of the caller's choosing: under the focused control, for the keyboard.

### `context_menu_input`

```
pub fn context_menu_input(c: ContextMenu, input: Input, within: Rect, f: Frame) -> ContextStep
```

**Opened by the gesture itself**, so that the one place deciding what the secondary button
means is this library rather than every application that wants a context menu. An application
hands its inputs over and reads the step, exactly as it does for a menu on a bar.

### `ContextStep`

```
pub struct ContextStep {
    context: ContextMenu
    // The item chosen, or -1, and whether it closed without choosing -- `MenuStep`'s two outputs,
    // carried through unchanged.
    chosen: int
    dismissed: bool
}
```

### `context_menu_layer`

```
pub fn context_menu_layer(c: ContextMenu, within: Rect, f: Frame) -> Option<widget.Layer>
```

The popup, or nothing while it is shut.

**An `Option`, where `menu_popup_layer` returns a layer and leaves "is it open" to the caller.**
A context menu is the only popup here with nowhere to hang off, so a caller holding a closed one
has no anchor to build an empty layer from -- which is exactly what the menu demo does today,
with a zero-sized layer holding `Nothing`.

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
pub fn dialog_layer(
    name: string,
    title: string,
    content: widget.Widget,
    actions: widget.Widget,
    f: Frame
) -> widget.Layer
```

A modal layer: a title in a header, `content`, and `actions` in a footer.

**Modal, so the geometry shuts the pointer out and `takes_keys` shuts Tab out** -- a click beside
the dialog lands on the layer, finds no control, and goes nowhere, and the tab order holds only
what is inside. `widget.modal_layer` is where both of those are explained.

**Three regions rather than a title stacked on a blob.** The title and the buttons used to be
two of the caller's own children in one padded column, so every dialog decided for itself how
far its title sat from its fields and whether its buttons had any air above them -- and two
dialogs in one application answered differently. A header and a footer are separated by a rule
and keep their own padding, which is what makes a stack of fields read as the middle of
something rather than as the whole of it.

**`Nothing` for `actions` draws no footer**, rule and all: a dialog that only says something has
nothing to put down there, and a rule under its last line would promise a row that never comes.

The content between them is still the caller's: a dialog is a frame and a pair of keys, not a
thing that knows what it is asking.

## The split pane

### `divider_step`

```
pub val divider_step = 10.0
```

What one arrow key moves the divider by.

**A module val rather than a field, unlike `ScrollBar.step`.** A scroll bar's step is a row of
something this module cannot see, so it has to be given one; a divider moves in points, and a
point is a point whatever is either side of it.

### `SplitPane`

```
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
```

Two panes with a divider between them, which the user can drag.

**The first control here that owns a layout rather than filling one**, and it is still not a
layout container: `split_pane_widget` takes the two panes as widgets and returns a widget, so
`widget` keeps every placement rule and this keeps the state and the gestures. A `Stack` whose
children's sizes came from a control's state would have put a control inside the box model.

**The position is points from the start, not a fraction of the box.** A fraction would need no
extent at all -- two `Expanded` weights say it, and the first draft was written that way -- and
it is the wrong model: it makes a sidebar grow when the window grows, where every file browser
ever written keeps the sidebar's width and gives the new room to the document. Points cost this
control the one thing a fraction had for free, which is that a number in points has to be
clamped against an extent the state does not know, and that is what `split_at` is for.

**`position` is written only by a gesture, and read through `split_at`.** So a window squeezed
narrow enough to push the divider does not overwrite what the user chose: the clamp happens on
the way out, the stored number survives, and widening the window again puts the divider back
where they left it. Storing the clamped value instead -- which is what writing it on every
layout would do -- loses that, and it is the complaint `JSplitPane` keeps a `lastDividerLocation`
to answer.

### `split_pane_of`

```
pub fn split_pane_of(name: string, label: string, direction: widget.Direction, position: float) -> SplitPane
```

### `divider_name`

```
pub fn divider_name(s: SplitPane) -> string
```

The divider's own name, which focus and accessibility address it by.

**The divider takes the focus, not the pane.** A pane that was a tab stop would be a stop in
front of whatever is inside it, so Tab would reach the split before reaching the tree in its
first half; and the thing a keyboard user wants to move is the divider. Same construction as
`row_name`: the control's name and a suffix, because a part has no name of its own.

### `split_at`

```
pub fn split_at(s: SplitPane, style: Style, extent: float) -> float
```

Where the divider starts, in points from the start of a box `extent` long.

**One function, read by the widget and by every gesture** -- `thumb_of`'s rule. A divider drawn
at one number and dragged by another is a divider that jumps out from under the pointer on the
first press.

### `divider_rect`

```
pub fn divider_rect(s: SplitPane, style: Style, bounds: Rect) -> Rect
```

The box the divider occupies, which is what a press is tested against.

### `SplitStep`

```
pub struct SplitStep {
    split: SplitPane
    // Whether the divider ended up somewhere else. **Where it ended up is on `split`**, unlike
    // `ScrollBarStep.offset`: a scroll bar reports an offset whose owner is the control being
    // scrolled, where a split pane owns its own divider and has nobody to hand the number to.
    moved: bool
}
```

### `split_pane_input`

```
pub fn split_pane_input(s: SplitPane, input: Input, bounds: Rect, f: Frame) -> SplitStep
```

What an input does to a split pane, given the box the whole pane occupies.

A press on the divider takes hold of it, a move while held drags it, a release lets go. A press
anywhere else does nothing -- **the panes' own contents are not this control's business**, and
`target` has already decided that an input landing inside a pane belongs to whatever is in
there. The arrows move the divider by `divider_step`, Home and End push it as far as the
minimums allow.

**The arrows that move it are the ones that lie along the stack**: Left and Right for a `Row`,
Up and Down for a `Column`. The other pair is left alone rather than treated as the same thing,
because a divider that moved sideways when you pressed Down would be reporting that this control
does not know which way it is facing.

### `split_pane_widget`

```
pub fn split_pane_widget(
    s: SplitPane,
    extent: float,
    first: widget.Widget,
    second: widget.Widget,
    f: Frame
) -> widget.Widget
```

The pane as a widget: the first pane at its size, the divider, and the second pane taking what
is left.

**`extent` is given rather than measured**, which is the one thing this control asks of its
application that no other control here does. The box model places a tree it is handed; a
position in points cannot be turned into a layout without knowing how much room there is, and
the application already holds that number because it is the `bounds` it passes to
`split_pane_input`. Passing last frame's extent is exactly right: it is the extent the user was
looking at when they dragged.

**The second pane is `expand`ed rather than sized.** The first pane and the divider are fixed,
so whatever rounding is left over goes to the second pane and there is no seam -- where two
fixed sizes that did not add up to the box would leave a line of background showing between
them, on some window widths and not others.

## The table

### `MarkedValue`

```
pub struct MarkedValue {
    mark: widget.Icon
    words: string
}
```

One value in a table cell.

**An enum, where Swing's `TableModel` has `Object` and `getColumnClass`.** `JTable` looks a
renderer up by the class the model reports for a column -- reflection doing a job a type can
do, and a `ClassCastException` at paint time where this is a compile error. Every variant here
is one of `JTable`'s own default renderers; the difference is that the set is closed, so a new
kind of value is a compile error in the one switch that draws them rather than a cell that
silently falls back to `toString`.

**Why a table needs this at all**: a column of numbers held as text cannot be right-aligned on
its digits without the caller padding them, cannot be formatted for a locale, and sorts `10`
before `9`. Holding the value and formatting late is what fixes all three at once.
An icon and the words beside it. **A struct and not two payloads on the variant**, because a
pattern here binds one value: `Marked(mark, words)` is a syntax error, and every multi-field
variant in this library is a struct for the same reason.

### `marked`

```
pub fn marked(mark: widget.Icon, words: string) -> Value
```

### `Value`

```
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
    // **An icon and the words beside it**, which `Mark` and `Text` cannot be together.
    //
    // This is the commonest row in any desktop: a file beside its kind, a place beside its sign, a
    // screen beside its symbol in a sidebar. A list row holds one value, so before this the choice
    // was an icon *or* a name, and every caller that wanted both had to stop using `List` and lay
    // out buttons by hand -- which is a tab strip with none of a listbox's selection or keys.
    //
    // **The words are the value**, and the icon is a mark on it: `value_text` answers the string,
    // so sorting, filtering and what a reader is told are all unchanged by decorating a row. An
    // icon a reader would need is one that should have been words.
    Marked(MarkedValue)
    // A cell with nothing in it, which is **not** the empty string: a missing measurement and a
    // measurement of "" are different facts, and a reader should not be told they are the same.
    Blank
}
```

### `value_text`

```
pub fn value_text(v: Value, l: Locale) -> string
```

What a value reads as: what a text cell draws and what a reader hears.

**Formatted here and not by the caller**, which is the whole benefit of holding the value: a
number takes the locale's own separators and a date its own order, so the same table reads
correctly in `tr_TR` and `en_US` without anybody passing a format down.

### `value_or_blank`

```
pub fn value_or_blank(v: Option<string>) -> Value
```

A value that may not be there.

**`Blank` is already the `None` case**, so these are a convenience and not a second way to say
it. A `Value` whose payloads were `Option`s would give the library two spellings for "nothing
here" -- `Blank` and `Number(None)` -- and two spellings of one fact is what this module keeps
designing against. A nullable column maps straight through instead:


```
push(cells, controls.value_or_blank(row.price))
```

**There is deliberately none for `Option<int>`**, because an `int` does not say whether it is a
quantity or an identifier -- `Whole` groups and `Code` does not, and only the caller knows
which it meant. See `Value.Code`.

```
pub fn value_or_blank(v: Option<float>) -> Value
```

```
pub fn value_or_blank(v: Option<decimal>) -> Value
```

```
pub fn value_or_blank(v: Option<time.LocalDate>) -> Value
```

```
pub fn value_or_blank(v: Option<Toggle>) -> Value
```

### `Column`

```
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
```

One column of a table.

### `column_of`

```
pub fn column_of(title: string, width: float) -> Column
```

### `numeric_column_of`

```
pub fn numeric_column_of(title: string, width: float) -> Column
```

A column of numbers, which read from the right so their digits line up. The reason this is worth
a second constructor rather than a field to set: a table of measurements has one of these per
column and spelling out a struct literal each time is how a right-aligned column gets forgotten.

### `Table`

```
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
```

A grid of cells, a header over it, one row selected.

**What this is, by JIDE's own division of its product line**: `JideTable` -- a table with a
header, selection, scrolling in both directions and columns the user can resize. Sorting,
filtering, cell spanning, a tree in the first column and a nested table in a row are all things
JIDE layers *over* that base, and they are deferred here in the same order and for the same
reason: each is a model on top of the rows, and a base that cannot show rows well makes none of
them better.

**The header is outside the scrolled view, vertically, and inside it horizontally.** That is the
whole of what makes a table a table rather than a list of rows -- the titles stay while the rows
go by -- and it is the one thing that complicates the arithmetic, because the view the rows
scroll in is *not* the control's box. Everything that needs that box goes through `table_body`
rather than working it out again, which is `thumb_of`'s rule: five copies of
`bounds.height - header_height` is five chances to forget one.

### `table_of`

```
pub fn table_of(name: string, label: string, columns: [Column], rows: [[Value]]) -> Table
```

```
pub fn table_of(
    name: string,
    label: string,
    columns: [Column],
    rows: [[Value]],
    l: Locale
) -> Table
```

```
pub fn table_of(name: string, label: string, columns: [Column], rows: [[string]]) -> Table
```

A table of plain text, which is what most of them are.

**Kept so that a table of strings says nothing about types**, and so that every caller written
before `Value` existed still compiles. A column of words is a column of words; the enum is for
the table that has a number, a date or a flag in it.

### `value_at`

```
pub fn value_at(t: Table, row: int, column: int) -> Value
```

The value in a cell, or `Blank` for a cell that is not there. See `Table.rows`.

### `cell_at`

```
pub fn cell_at(t: Table, row: int, column: int) -> string
```

What is in a cell, as text: what a text cell draws and what a reader hears.

**Still a `string` and still this name**, because that is what every caller of it wants -- the
typed value is `value_at`, and the two exist for different readers.

### `compare_values`

```
pub fn compare_values(a: Value, b: Value) -> int
```

How two values order against each other, for sorting a column.

**This is what F321 was for.** A column of text sorts "10" before "9", which makes a sorted
table worse than an unsorted one -- the user believes an order that is not there. Holding the
value means a number compares as a number and a date as a date.

**Different kinds fall back to their text**, which is the honest answer for a column somebody
filled with two sorts of thing: there is no meaning to "is this date larger than that word",
and refusing to sort would be worse than ordering them predictably.

### `sorted_order`

```
pub fn sorted_order(t: Table) -> [int]
```

The rows in the order they are shown, as indices into `rows`.

**An index projection and not a reordering.** `selected` is an index into `rows`, so sorting the
array itself would silently move the selection to a different record -- and the order a caller
gave is a fact about the data that a view must not be able to destroy. `Tree.visible_rows` is
the same shape for the same reason.

**A `Blank` sorts to the end whichever way the column is pointing.** A missing measurement is
not smaller than every number and larger than none of them; it is absent, and the end is where
every spreadsheet puts it. Flipping it with the rest would make reversing a column move the
holes from the top to the bottom, which reads as data moving.

### `display_at`

```
pub fn display_at(t: Table, row: int) -> int
```

Where a data row sits on screen, or -1 when it is not shown.

**The inverse of `sorted_order`, and the reason it exists at all**: a press and an arrow key
both answer in *visual* positions while `selected` is a *data* index, and a control that
confused the two would select a different record than the one under the pointer. That is the
calendar's twenty-two points in another costume.

### `row_shown_at`

```
pub fn row_shown_at(t: Table, position: int) -> int
```

The data row shown at a visual position, or -1.

### `sorted_on`

```
pub fn sorted_on(t: Table, column: int) -> Table
```

Sorting by a column: the first press orders it upward, the next reverses it, and the third
clears the sort.

**Three states and not two**, because the order a caller gave is meaningful -- a query already
ordered by date, a list already in the order a user arranged it -- and a table that could only
toggle between two sorts would have thrown that away the moment somebody touched a header.

### `row_label`

```
pub fn row_label(t: Table, row: int) -> string
```

A row as a reader hears it: its cells in order, comma separated.

**The columns' order, not the row's length**, so a short row is read as the empty cells it has
rather than stopping early -- a reader that said "Aspirin, 100mg" where the screen shows an
empty dose column has told the user the wrong thing about the data.

**Each cell as `value_text` writes it**, so a reader hears the number a sighted user sees --
separators, date order and "checked" for a flag -- rather than whatever the value happens to
look like in the source.

### `column_x`

```
pub fn column_x(t: Table, index: int) -> float
```

Where a column starts, measured from the start of the first one and before any sideways scroll.

**Defined up to and including `len(columns)`**, where it answers the width of them all -- so the
trailing edge of column `i` is `column_x(t, i + 1)` and the hit test, the header and the rows
are reading one function rather than three loops that must agree.

### `columns_width`

```
pub fn columns_width(t: Table) -> float
```

### `table_body`

```
pub fn table_body(t: Table, style: Style, bounds: Rect, m: widget.Metrics) -> Rect
```

The box the rows are in: the control's box without the header.

**Every calculation about rows goes through here** -- the press that picks a row, the page a
PageDown moves, the reveal that brings a selection into view, the scroll bar's track and
whether a press landed on it. The header is the only thing between them and `bounds`, and it is
the kind of subtraction that gets written four times and forgotten once.

### `edge_at`

```
pub fn edge_at(t: Table, style: Style, bounds: Rect, p: Point, m: widget.Metrics) -> int
```

### `table_row_name`

```
pub fn table_row_name(t: Table, row: int) -> string
```

### `column_header_name`

```
pub fn column_header_name(t: Table, column: int) -> string
```

### `TableStep`

```
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
```

### `table_input`

```
pub fn table_input(t: Table, input: Input, bounds: Rect, f: Frame) -> TableStep
```

What an input does to a table.

**Three gestures in order of who owns the pointer**, the opening `list_input` has for the same
reason: a resize in progress keeps it, then the scroll bar, then the rows. A press that is none
of those -- on a header, on empty space below the last row -- does nothing at all.

### `table_widget`

```
pub fn table_widget(t: Table, f: Frame) -> widget.Widget
```

The table as a widget: a header over a viewport of rows.

**One control and one tab stop**, as a list is: Tab reaches the table and the arrows move within
it. The rows are `part_of` -- something a click and a reader reach and Tab does not -- and so are
the column headers, which is what will make a header sortable without a second tab stop per
column.

## Radio buttons

### `Radios`

```
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
```

A choice of one from several.

**The control is the group, not the button**, which is the whole difference from a check box. A
lone radio button is a control a user cannot undo: there is no gesture that unselects one, so a
single radio is a switch that can only ever be turned on. What a user is choosing is which of N,
and that is one piece of state, so it is one control with one tab stop and one name -- which is
also what makes the arrows work, since a group is the only thing that knows what "the next one"
is.

**A column, and only a column.** A row of options needs every option to be the same width to be
hit-tested -- the compromise `Tabs.tab_width` makes -- and an option's label is a sentence where
a tab's is a word. A row of two short options is a real layout and this does not do it yet.

### `radios_of`

```
pub fn radios_of(name: string, label: string, items: [string], selected: int) -> Radios
```

### `radio_name`

```
pub fn radio_name(r: Radios, index: int) -> string
```

An option's own name, which focus and accessibility address it by.

### `RadioStep`

```
pub struct RadioStep {
    radios: Radios
    moved: bool
}
```

### `radios_input`

```
pub fn radios_input(r: Radios, input: Input, bounds: Rect, f: Frame) -> RadioStep
```

What an input does to a group.

**The arrows select, they do not merely move.** That is what a radio group does everywhere and
it is the opposite of a list: there is no second keystroke to confirm, because a radio group
*is* the confirmation -- it has no state between "looking at" and "chosen". A group that needed
Space after an arrow would leave a keyboard user with a selection the screen disagrees with.

**Both pairs of arrows, unlike a split pane.** A column of options is navigated with Up and Down
by every keyboard user and with Left and Right by every one who learned radios on a form where
they were in a row; neither is wrong about what "the next option" means.

### `radios_widget`

```
pub fn radios_widget(r: Radios, f: Frame) -> widget.Widget
```

The group as a widget: an option per row, each a part of the group.

**The options carry `checked`, and the group carries nothing**, which is what a reader reads:
it announces "radio button, Weekly, checked" for the one you are on. A group holding an index
would make every adapter work out for itself which child the number meant.

## The slider

### `thumb_width`

```
pub val thumb_width = 14.0
```

How thick the track is, and how wide the thumb is. **Shared by the widget and the hit test**,
which is `bar_thickness`'s rule.

### `thumb_height`

```
pub val thumb_height = 18.0
```

How tall the thumb is drawn.

**Given rather than taken from the row**, because a thumb is a thing of its own size: a slider
in a 60-point row should not have a 60-point thumb. The first version placed it in a layer of
height zero -- a rect `layer_at` honoured exactly -- so the slider drew a track and nothing to
drag, which is the same mistake the link made with a `Fixed` of height zero on the same
afternoon. A box whose size nobody states is a box of nothing.

### `Slider`

```
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
```

A number chosen by dragging.

**The range and the step are the control's, not the application's.** A slider that took a
fraction and left the arithmetic outside would make every caller write the same three lines,
and get the clamp wrong in at least one of them -- which is the shape `scroll_to_reveal` and
`clamp_offset` already exist to prevent elsewhere.

### `slider_of`

```
pub fn slider_of(name: string, label: string, least: float, most: float, value: float) -> Slider
```

### `snapped_value`

```
pub fn snapped_value(least: float, most: float, step: float, want: float) -> float
```

A value clamped into the range and snapped to the step.

**One function, used by every gesture and by the constructor**, so a slider can never hold a
value it would refuse to be dragged to -- which is the state that makes a control look broken
the first time a user touches it.

### `slider_at`

```
pub fn slider_at(s: Slider, width: float) -> float
```

Where the thumb's left edge sits, in points from the start of the track.

**One function, read by the widget and by every gesture** -- `thumb_of`'s rule, for the reason
it keeps being restated: a thumb drawn by one calculation and dragged by another jumps under
the pointer.

### `SliderStep`

```
pub struct SliderStep {
    slider: Slider
    moved: bool
}
```

### `slider_input`

```
pub fn slider_input(s: Slider, input: Input, bounds: Rect) -> SliderStep
```

What an input does to a slider.

**A press anywhere on the track takes the value under it**, rather than paging towards it as a
scroll bar does. The two look alike and mean different things: a scroll bar moves a view and a
page is the unit a reader moves by, where a slider *is* the value and a user pressing at the
three-quarter mark is asking for three quarters. It takes hold at the same time, so a press and
a drag are one gesture.

### `slider_widget`

```
pub fn slider_widget(s: Slider, f: Frame) -> widget.Widget
```

The slider as a widget: a track with a thumb on it.

**It is not told how wide it is, and that is the fix for a real defect.** This took a `width`
and placed the thumb at `slider_at(s, width)` -- while `slider_input` computed the same
position from the `bounds` the layout actually gave the control. Two numbers for one thing, and
they agreed only while a caller passed the width it then laid the slider out in. The gallery
put one in a `widget.expand`, so the drawn thumb and the one the press looked for were hundreds
of points apart: taking hold of the thumb you could see was a press on the track, and the
slider jumped. This module keeps relearning the same rule -- `thumb_of` states it, `split_at`
states it -- and this time the answer is to remove the second number rather than to document
it.

**The thumb is placed by two flexible spaces instead**, whose weights are the value's share of
the range and the rest of it. A stack divides its leftover room in the ratio of its flexible
children, and the leftover room here is exactly the track less the thumb -- so the thumb lands
where `slider_at` says in whatever box the layout gives it, by construction, with no width for
anyone to pass wrongly.

## The link

### `Link`

```
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
```

Text that goes somewhere.

**A control, not a label**, which is the point: a link has to be reachable by Tab, operable by
Enter, and announced as a link, and a coloured label is none of those. What it does *not* do is
go anywhere -- this layer has no browser and no idea what a URL means -- so it answers that it
was activated and the application decides.

### `link_of`

```
pub fn link_of(name: string, label: string, href: string) -> Link
```

### `LinkStep`

```
pub struct LinkStep {
    link: Link
    // **Followed, not clicked**: a press that drags off and releases elsewhere is not a follow,
    // which is `ButtonStep.activated`'s rule and the reason this is built on a button.
    followed: bool
}
```

### `link_input`

```
pub fn link_input(l: Link, input: Input, bounds: Rect) -> LinkStep
```

### `link_widget`

```
pub fn link_widget(l: Link, f: Frame) -> widget.Widget
```

The link as a widget: its text, underlined.

**The underline is the signal, not the colour.** A link told apart from body text by hue alone
is a link a colour-blind reader cannot find, and `Style` has no link colour to set -- which is
the right way round: the underline works in every theme and on every display, where a blue that
reads on white is a blue that vanishes on navy. A theme that wants a coloured link is the
argument for a `Style.link` field, and nothing has made that argument yet.

**It measures its own text, which is why it takes a `Metrics`.** The first version put the
label and the rule in a stretched column and said in a comment that the column would be as wide
as its widest child -- and a column is as wide as the box it is *given*, which at the root of a
layout is the whole window. So the underline ran the width of the pane with the words at one
end of it. A control that wants to be the size of its text has to ask how wide its text is;
`check_box_widget` takes a `Metrics` to make a square for the same kind of reason.

## What the pointer is over

### `split_pane_cursor`

```
pub fn split_pane_cursor(s: SplitPane, style: Style, bounds: Rect, p: Point) -> widget.Cursor
```

The shape the pointer should take over a split pane's box.

**Asked of the control rather than worked out by the application**, which is the whole reason
it is here: where the divider is is `split_at`'s answer, and an application computing it a
second time to decide a cursor would be the two-calculations bug this module keeps designing
against -- a cursor that changes shape a few points away from the line it belongs to.

### `table_cursor`

```
pub fn table_cursor(t: Table, style: Style, bounds: Rect, p: Point, m: widget.Metrics) -> widget.Cursor
```

The shape the pointer should take over a table's box: the resize arrows over a column's
trailing edge in the header, and nothing anywhere else.

**`edge_at` is the same function the press uses**, so the shape and the gesture agree by
construction: the pointer cannot show arrows where a drag would select a row.

## The style

### `Type`

```
pub struct Type {
    face: int
    size: float
    line: float
}
```

How a control is drawn. Injected rather than global, for the reason `widget.Metrics` is: a module
that reads a theme from somewhere cannot be tested against two themes, and an application with
two panels cannot style them differently.
The spacing scale a theme sets and the control set indexes.

**Six steps, as Material and most design systems ship**, so a scale maps one-to-one onto what a
design tool exports. `controls` says **once** which step each control uses, and a theme never
names a control -- which is the whole shape of this layer and the reason a selector language
would have nothing to select.

**This replaced eleven numbers rather than one.** Every control here invented its own insets --
the tree 4 and 2, the table 6 and 2, a radio 4 and 2, a cell 6 and 2 -- so a theme asking for
more room had nothing to set. The single `space` token that stood here before was the step of a
scale whose other steps did not exist.
The corner roundings a theme sets and the control set indexes.

**Four steps, every one with a reader.** D1's sketch had five, with a `none`; it was dropped
rather than shipped empty, because a look that wants square corners says `Square` in each of
these and a step nothing reads is a decision a look author has to make for nothing.

**`full` is a role rather than a preference.** A radio's dot is a circle in every look here,
including the square one, because round-against-square is how a user tells a radio from a check
box -- so `plain` sets `full` to `Fraction(0.5)` while setting every other step to `Square`. A
look may square it and would be wrong to.
One role of a theme's type scale: which face it is drawn in, how big, and how far apart lines of
it sit.

**`line` is a multiplier, not a height.** `widget.FaceMetrics.line_height` is a function of size
computed from the font and it stays authoritative; a theme may only stretch it. The precedent is
in the same struct: `FaceMetrics.ascent` exists because "a baseline cannot be guessed from a line
height", and every glyph in this widget set was once drawn a descent too low with every box
correct. The font knows its own ascent, descent and gap, and a theme written against a Latin face
does not know what CJK or Devanagari need. See D14 in docs/design/look-and-feel.md.

**`face` is an index into the faces an application loaded**, and it is clamped rather than
trusted: see `face_for`.

### `TypeScale`

```
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
```

The type scale a theme sets and its readers index.

**Five roles, and the library reads two of them.** `body` is text a control *contains* -- a
field's content, a list's rows, a table's cells, a link -- and `label` is a control's own name:
a button, a tab, a menu's title, a check box, a radio. The other three have **application**
readers rather than control ones, and that is not an oversight: a heading is not a control, which
is why `controls` was never going to draw one.

**That gap is why this exists.** `Style` used to carry `heading_size` and `small_size` and
**nothing read either of them** -- while the gallery's design files and curly's each declared
`val heading_size = 12.0` and `val small_size = 11.0` for themselves, at 12 where the theme said
16. Two applications invented a type scale because the one in the theme was unreachable from
where they needed it. The sizes here are what those applications actually draw, so adopting the
scale moves no pixels; see Q7/D17.

### `Shape`

```
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
```

### `Space`

```
pub struct Space {
    none: float
    xs: float
    s: float
    m: float
    l: float
    xl: float
}
```

### `Minimum`

```
pub struct Minimum {
    // The least a control the pointer must hit may measure on either axis.
    target: float
    // The shortest a scroll bar's thumb may become, however long the content is. A thumb that
    // shrank without limit would vanish on exactly the document it matters most on.
    thumb: float
}
```

The sizes a density may not go below.

**Its own group precisely so that density cannot reach it.** `target` is 24 points, which is also
WCAG 2.2's minimum target size (SC 2.5.8) -- and two of the three numbers that became this were
already exactly 24. Had a compact density scaled them, the library's densest mode would have been
the one that fails an accessibility criterion, and the gate would have had to be told to permit
it. Compact tightens padding and gaps; a scroll thumb and a radio stay grabbable.

### `Stroke`

```
pub struct Stroke {
    // One device line: a table's grid, a link's underline, and the gap between the two -- the rule
    // sits one hairline below the text, so the gap and the line move together.
    hair: float
    // A line meant to be read *as* a line: a split pane's divider.
    line: float
}
```

The widths of the lines the control set draws.

**Not on the space scale, because a hairline is a hairline at every density.** A grid line that
doubled in a spacious theme would stop being a grid line and start being a border.

### `Style`

```
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
    // **How much air a list's row has above and below its line of text**, which is what makes a
    // list comfortable or dense.
    //
    // **A number on the look and not on a `List`**, which is the distinction the comment on
    // `row_height` is making: a row carrying its own height is a number true by coincidence, and
    // a look saying how roomy its lists are is the look's job. It is still added to a measured
    // line rather than replacing one, so a theme that enlarges its type still gets rows that fit.
    //
    // **Defaults to `space.s`**, which is what `row_height` used to add -- so every look that does
    // not mention it is unchanged. It needed naming because `space.s` is *also* the gap between
    // two chips and the gap between an icon and its label: a sidebar that wanted roomier rows had
    // to make every small gap in the window roomier with it, which is the complaint this answers.
    row_air: float
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
```

### `Frame`

```
pub struct Frame {
    style: Style
    metrics: widget.Metrics
    hovered: string
    pressed: string
    focused: string
}
```

The look, the window's measurements, and the three names the window holds: everything a control
needs to draw itself that is not the control.

**One value rather than a longer parameter list, and this is the third time the question came
up.** D16 gave twenty functions a `Style` when a look had to reach them; D18 gave thirteen of
them a `widget.Metrics` when a row's height became a measurement rather than a number; F294
needed the pointer to reach a paint function, and F296 (the desktop's scheme) and F297 (the
designer's live overrides) each want to hand one more fact down the same path. A fourth and a
fifth parameter is not a design, it is a queue -- so the facts travel together and the signature
stops moving. Those later tasks add a **field** here.

**The three names, and why they are names.** Hover, the press and the focus are facts about the
pointer and the keyboard rather than about any widget: a `hovered` flag inside the tree would be
a second copy of each, free to disagree, and a control left lit after the pointer had gone is
exactly how that disagreement shows. The window holds one of each and they arrive here, which is
what `widget.paint_hovered` used to do for the one case a translucent veil could serve. See D5
and D19 in docs/design/look-and-feel.md.

**`pressed` is for the controls that do not model their own press.** A button, a check box and a
link carry `held`, and a scroll bar, a split pane, a table's column edge and a slider carry a
`grabbed` that holds the grab's offset and not merely a bool -- those read their own field. A
menu's item, a list's row and a tab have no such field and read this one. One source each, never
both: `mood_of` is handed a bool either way.

### `rounded`

```
pub fn rounded(style: Style) -> Style
```

The same look with every control's corner made full: a round button, a pill-shaped entry.

**`Parts.corner` is per mood, so saying "round" by hand is five edits** -- rest, hovered,
pressed, disabled and selected -- and five chances to leave one square, which shows only when a
pointer happens to be over it. This says it once.

`shape.full` rather than a number, because the look already names this: it is what a radio's
circle is drawn with, and a theme that redefines `full` gets its own idea of round here too.

**A style, not a control flag.** Roundness is a property of a look, and a `Button.round` would
be a control arguing with its theme -- which is the argument `Style` exists to settle. A caller
draws through a frame built from this: `controls.frame_of(controls.rounded(f.style), f.metrics)`.

### `frame_of`

```
pub fn frame_of(style: Style, m: widget.Metrics) -> Frame
```

A frame with nothing hovered, pressed or focused: what a control looks like at rest.

**The resting frame is the one tests and one-shot renderers want**, and naming it here keeps the
three empty strings out of thirty call sites. An application fills the names in from what it
already holds: `with controls.frame_of(style, m) { hovered: st.hovered, pressed: st.pressed,
focused: st.focused }`.

### `Parts`

```
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
```

What a control is drawn as in one state: its fill, its text, its edge, and the two shape
properties a state is allowed to change.

**One struct, so a state cannot be half-implemented.** Before this, a state was whichever of
four loose fields happened to cover it: `pressed_face` was a fill and could not say the text
changed, `disabled_ink` was a text colour and could not say the fill receded -- which is why a
disabled button kept a live-looking face and merely greyed its words, something no platform
does. A look that sets a row sets all of it.

### `States`

```
pub struct States {
    rest: Parts
    hovered: Parts
    pressed: Parts
    disabled: Parts
    selected: Parts
}
```

The five states a control can be in, each with its own parts.

**Five rows and one question.** Every paint function asks `parts_for` once -- what are my parts
in this state -- rather than reaching for a different field per state, so a look cannot forget
one and a control cannot invent its own answer. See D5 in docs/design/look-and-feel.md.

**Focus and emphasis are deliberately not rows here.** A control can be focused *and* hovered
*and* pressed at once, so focus cannot be one of five exclusive rows: it stays a ring drawn over
whatever state applies, from `accent`. Emphasis is an axis over a whole look -- a primary button
is emphasised at rest, hovered, pressed and disabled alike.

### `Mood`

```
pub enum Mood {
    Rest
    Hovered
    Pressed
    Disabled
    Selected
}
```

Which row a control is in. **Exclusive, which is the point**: one question with one answer, so
two states cannot both be half-painted.

### `mood_of`

```
pub fn mood_of(disabled: bool, pressed: bool, selected: bool, hovered: bool) -> Mood
```

The one place the precedence lives.

**`disabled` beats `pressed` beats `selected` beats `hovered`.** With five exclusive rows one of
them has to win, and each of these is the answer a desktop gives: a disabled control is disabled
whatever the pointer is doing; a press is the most recent thing the user did; and **a selected
row keeps its colour while the pointer crosses it**, because a selection that vanishes under the
pointer reads as the selection being lost -- GTK and macOS both keep it.

Every control calls this with its own four facts, so the ordering cannot drift between two
controls that happen to disagree about which matters more.

### `parts_for`

```
pub fn parts_for(style: Style, mood: Mood) -> Parts
```

The parts of a control in one mood: an index into the look's five rows, and nothing else.

### `hovered_in`

```
pub fn hovered_in(f: Frame, name: string) -> bool
```

Whether the pointer is on the control with this name.

**A comparison rather than a flag**, which is the whole reason the frame carries names: the
window holds one name and every control compares itself against it, so no two controls can both
think they are hovered and none can be left lit after the pointer has gone.

### `reserve_of`

```
pub fn reserve_of(style: Style) -> float
```

The ring of room a control leaves for a state that wants a bigger frame: the largest expansion
any of the five rows asks for.

**Derived rather than a token**, for D18's reason: a number a look had to set is a number a look
could set wrong -- too small and a hovered frame clips, too large and every control grows for
nothing -- and the rows already say how far they expand. It is also what makes
`reserve_of(style) - p.expansion` non-negative for every row, which **is** the expansion
invariant: no state can push a frame outside the box its layout claimed.

### `pressed_in`

```
pub fn pressed_in(f: Frame, name: string) -> bool
```

Whether the pointer is being held on the control with this name. For the controls that do not
model their own press -- a menu's item, a list's row, a tab. A button, a check box and a link
carry `held`, and a scroll bar, a split pane, a table's column and a slider carry a `grabbed`
that holds the grab's offset: those read their own field, and never this.

### `face_for`

```
pub fn face_for(m: widget.Metrics, want: int) -> int
```

A control's own face: its fill, edged and rounded as the style asks.

**Every control that has a face goes through here**, which is the point: a look and feel set on
`Style` reaches a button, a field, a tab, a menu title, a dialog's panel and a scroll bar's thumb
without each of them being taught about borders. A control that painted its own
`widget.filled(...)` would be the one that stayed flat when the theme changed, and the way that
bug presents is one widget out of fifteen looking wrong.
A face index an application actually loaded.

**Clamped rather than trusted, which the gallery had to work around.** `gallery.zm` records that
the renderer "refuses a run naming a face the options do not hold", and loads its text face twice
so that index 1 is always real. With a clamp a single-face application renders a look that asks
for bold -- it renders it regular, which is the right failure and the one D9 chose.

### `line_for`

```
pub fn line_for(m: widget.Metrics, t: Type) -> float
```

How tall one line of a role is: **the font's own line height, stretched by the theme's factor.**
One function, so that a caret and the box it sits in cannot read different numbers.

### `row_height`

```
pub fn row_height(m: widget.Metrics, style: Style) -> float
```

How tall one row of a list, a tree or a table is: **a line of body text, with the small step of
the space scale around it.**

**Derived rather than a field on the control, which is what it was** -- `List`, `Tree` and
`Table` each carried `row_height: 20.0`, set by nothing but their own constructors. Twenty is a
line of twelve-point text and a little air, written down once as a number and thereafter true
only by coincidence: a look that enlarged its type clipped its own rows, and `at_density`
tightened every padding and gap in the window and left every row exactly twenty points tall.
A row is the one box in this library whose height *is* a line of text, so it is measured like
one -- by `line_for`, which is also what the row's label is laid out with, so the box and the
text in it cannot read different numbers. See Q8 in docs/design/look-and-feel.md.

**Not a theme token either.** A token set to 20 is the same number in a different place: it can
still disagree with the face the theme asks for, and nothing would say so.

### `list_row_height`

```
pub fn list_row_height(l: List, m: widget.Metrics, style: Style) -> float
```

How tall one row of **this** list is: `row_height`, unless the list asked for its own air.

**One function, so the widget and the hit test cannot read different numbers** -- which is the
whole of why a row's height is derived rather than stored. `list_input` divides a pointer's y by
this and `list_widget` lays its rows out by it; a list whose air the two disagreed about would
select the row above the one you clicked, and more of them the further down you went.

### `header_height`

```
pub fn header_height(m: widget.Metrics, style: Style) -> float
```

How tall a table's header band is: a line of **label** text -- the role a column title is drawn
in -- with the medium step around it.

**A step wider than a row's**, which is what every table does and what the old pair of numbers
said too (22 against 20): the header is a band rather than a row, and the air around the titles
is what separates them from the data.

### `indent_for`

```
pub fn indent_for(style: Style) -> float
```

How far one level of a tree indents, and the width of the mark a branch draws.

**`space.xl`, which had no other caller**: sixteen points was the indent and sixteen points is
the top of the space scale, so the scale already held the number the tree was keeping for
itself. Density reaches it through `space` like everything else.

## The keys

### `is_text_input`

```
pub fn is_text_input(input: Input) -> bool
```

Whether this input could move a caret or change text -- what the next instalment's entry asks
before handing an input to `edit`, and here rather than there because the answer is about `Input`.
