# `widget`

A widget tree: what a window is made of, laid out, and turned into a `Scene`.

**Everything here is a pure function over immutable values**, and that is the point rather than a
style preference. docs/reviews/tasks/f269-f277.md F271: the tree, its layout and its painting can
be unit-tested with no window, no display and no font, so the only part of a graphical
application that needs a screen is the part that blits the result. curly's terminal interface is
the precedent -- `tui/rows.zm` and `tui/render.zm` are pure and only `tui/app.zm` touches the
terminal -- and it is why that UI could be tested at all.

**Retained, not immediate.** A `Widget` is a value built and kept, not a sequence of calls made
per frame. The immediate-mode libraries have the nicer API and suit a language of immutable
values well, but an accessibility tree cannot be destroyed and rebuilt every frame: assistive
technology has to be told *what changed*, which a tree that persists can say and a call sequence
cannot. docs/design/desktop-applications.md §5 has the argument; F275 is where it is paid for.

**Measurement is injected, not read from a font.** `layout` takes a `Metrics` -- two functions --
so this module never loads a face, never fails, and can be tested against a fake whose numbers
are chosen to make an arithmetic error obvious. `metrics_for` builds the real one from a
`font.FontRef`; `metrics_of` takes the two functions directly, which is what a test wants.

**This is not `graphics/layout`.** That module lays out *documents*: paragraphs, columns and
tables, measured once and printed. This lays out a *window*: a box model with flexible space,
re-run whenever the window changes size. They share `geom` and nothing else, and merging them
would give one module two jobs with different notions of what "fits" means.

`use widget` -- or `use widget.Name` to bring one item in bare.

## Example

```
use geom
use paint
use scene
use widget

// **Measurement is injected, so a layout can be computed with no font and no window.** These two
// functions stand in for a face: ten units of width per character, and a line twice the em size.
// `widget.metrics_for(face)` builds the real pair from a loaded font, and fails there -- once --
// rather than making every box in a window a fallible computation.
fn fake() -> widget.Metrics {
    return widget.metrics_of(
        fn(text: string, size: float) -> float { return (len(text) as float) * 10.0 },
        fn(size: float) -> float { return size * 2.0 }
    )
}

fn boxes(label: string, r: geom.Rect) {
    printf("{}: {},{} {}x{}\n", label, str(r.x), str(r.y), str(r.width), str(r.height))
}

fn main() {
    val m = fake()

    // A widget is a **value**, built and kept rather than a sequence of calls made per frame. That
    // is what lets an accessibility layer be told what *changed* (F275), and what lets this example
    // print a layout without a screen.
    val toolbar = widget.Widget.Group(widget.Stack {
        direction: widget.Direction.Row,
        spacing: 4.0,
        padding: widget.insets_all(2.0),
        background: Option<paint.Color>.Some(paint.rgb(0.9, 0.9, 0.9)),
        // A `flex` after the label pushes everything before it to the left: leftover space is a
        // widget, not a property of its neighbours.
        children: [widget.named_label("File", 8.0, paint.black(), "menu-file"), widget.flex()],
        name: "toolbar"
    })

    // **`expand` is how flexibility crosses a container.** A `flex` has no children and a group
    // takes its natural size, so a content area that fills whatever the toolbar left needs this.
    val content = widget.expand(widget.column_of(0.0, [widget.label_of("body", 8.0, paint.black())]))
    val root = widget.column_of(0.0, [widget.fixed_of(200.0, 20.0, toolbar), content])

    val laid = widget.layout(root, geom.rect_of(0.0, 0.0, 200.0, 100.0), m)
    boxes("toolbar", laid.children[0].bounds)
    boxes("content", laid.children[1].bounds)

    // A name is what a focus or accessibility layer refers to a node by; a path is where it sits.
    // **Prefer the name**: a path changes when a sibling is inserted above it.
    printf(
        "the label is named {} at path {}\n",
        laid.children[0].children[0].children[0].name,
        laid.children[0].children[0].children[0].path
    )

    // Two spacers share leftover space in proportion to their weights, which is why a weight is a
    // number and not a bool.
    val shared = widget.layout(
        widget.row_of(0.0, [widget.flex_of(1.0), widget.flex_of(3.0)]),
        geom.rect_of(0.0, 0.0, 80.0, 10.0),
        m
    )
    boxes("weight 1", shared.children[0].bounds)
    boxes("weight 3", shared.children[1].bounds)

    // Painting answers a `Scene`, which `graphics/raster` rasterises and `graphics/pdf` writes --
    // so a window and a printed page are one description rendered twice. Parents paint before
    // children, so a background sits behind what it is behind.
    printf("the window is {} ops\n", str(scene.op_count(widget.paint(laid, m))))
}
```

Output:

```
toolbar: 0.0,0.0 200.0x20.0
content: 0.0,20.0 200.0x80.0
the label is named menu-file at path 0/0/0
weight 1: 0.0,0.0 20.0x10.0
weight 3: 20.0,0.0 60.0x10.0
the window is 3 ops
```

## The tree

### `Direction`

```
pub enum Direction {
    Row
    Column
}
```

### `Insets`

```
pub struct Insets {
    left: float
    top: float
    right: float
    bottom: float
}
```

### `insets_of`

```
pub fn insets_of(left: float, top: float, right: float, bottom: float) -> Insets
```

### `insets_all`

```
pub fn insets_all(amount: float) -> Insets
```

The same amount on all four sides, which is what most padding is.

### `no_insets`

```
pub fn no_insets() -> Insets
```

### `Size`

```
pub struct Size {
    width: float
    height: float
}
```

### `size_of`

```
pub fn size_of(width: float, height: float) -> Size
```

### `Label`

```
pub struct Label {
    text: string
    // The em size, in device units.
    size: float
    colour: Color
    // What a focus or accessibility layer refers to this node by, or "" for none. **A name, not an
    // index**: a path changes when a sibling is inserted above it, and focus that moved because an
    // unrelated widget appeared is a bug a user notices and a test does not.
    name: string
}
```

A run of text. No wrapping: a label is one line, and a paragraph that wraps is a different
widget with a different measurement (and `graphics/layout` already knows how to break one).

### `label_of`

```
pub fn label_of(text: string, size: float, colour: Color) -> Widget
```

### `named_label`

```
pub fn named_label(text: string, size: float, colour: Color, name: string) -> Widget
```

### `Stack`

```
pub struct Stack {
    direction: Direction
    spacing: float
    padding: Insets
    // Painted behind the children, before them. `None` draws nothing, which is what most
    // containers want -- a stack per level each painting a background is how a UI ends up with
    // twenty overdraws per pixel.
    background: Option<Color>
    children: [Widget]
    name: string
}
```

Children in a row or a column, with space between them and around them.

### `row_of`

```
pub fn row_of(spacing: float, children: [Widget]) -> Widget
```

### `column_of`

```
pub fn column_of(spacing: float, children: [Widget]) -> Widget
```

### `Fixed`

```
pub struct Fixed {
    width: float
    height: float
    child: Widget
    name: string
}
```

A child that measures a fixed size, whatever its content would have measured.

**It fixes what it measures, not the box it ends up in.** A stack divides space along its own
direction and hands every child the parent's full extent *across* it -- so a `Fixed(40, 80)` in a
row 60 tall is laid out 40 wide and 60 tall. That is the stretch every box model defaults to, and
choosing otherwise means an alignment rule (top, centre, end), which belongs with the widget set
rather than here.

### `fixed_of`

```
pub fn fixed_of(width: float, height: float, child: Widget) -> Widget
```

### `Expanded`

```
pub struct Expanded {
    weight: float
    child: Widget
}
```

A child made flexible: it takes a share of the leftover space along its parent's direction, and
lays its own child out inside that.

**Without this, flexibility cannot cross a container.** A `Flex` has no children, and a `Group`
always takes its natural size -- so "a toolbar of its own height and a content area filling the
rest" had no spelling, which is the most ordinary layout there is. `Expand` is the one place a
subtree is told to be bigger than it measures.

### `expand_of`

```
pub fn expand_of(weight: float, child: Widget) -> Widget
```

### `expand`

```
pub fn expand(child: Widget) -> Widget
```

### `Viewport`

```
pub struct Viewport {
    offset_x: float
    offset_y: float
    child: Widget
    name: string
}
```

A window onto a child larger than itself.

**A viewport has no natural size, and that is the whole point of it.** `measure` answers nothing
for one, so a viewport must be given its size by a `Sized` or an `Expand` around it. Answering
the child's size instead would make a viewport in a column exactly as tall as its content, which
is a viewport that can never scroll -- a bug that looks right in a screenshot and is invisible in
a layout test that only checks the content fits. Nothing appearing at all is the better failure.

`offset` is how far the content has moved, in content units, **positive meaning the content has
gone up and left**: a scrollbar at the top reads zero. The child is laid out at its natural size
in that direction and the viewport's size across, so a column of rows scrolls vertically and a
wide row scrolls sideways, from the same structure.

### `scroll_of`

```
pub fn scroll_of(offset_y: float, child: Widget) -> Widget
```

### `named_scroll`

```
pub fn named_scroll(name: string, offset_x: float, offset_y: float, child: Widget) -> Widget
```

### `scroll_to_reveal`

```
pub fn scroll_to_reveal(view_extent: float, offset: float, target_start: float, target_extent: float) -> float
```

The offset that brings a target into view, given the one in force now.

**Scrolls by the least that works, and does nothing when the target is already visible**, which
is what makes arrowing through a list feel like a list rather than a slideshow: the selection
walks to the bottom edge and then the content creeps by one row at a time. Re-centring on every
move is the common alternative and it makes the whole page jump under a user who moved one step.

A target taller than the view is aligned to its start, because showing its top is more useful
than showing its middle and there is no offset that shows all of it.

### `clamp_offset`

```
pub fn clamp_offset(offset: float, view_extent: float, content_extent: float) -> float
```

What `offset` may be, so a caller cannot scroll past either end: never below zero, and never
further than the content's overhang. A content smaller than its view clamps to zero, which is why
this is a `max` of zero and not just a `min`.

## Semantics

### `Role`

```
pub enum Role {
    Button
    CheckBox
    RadioButton
    TextField
    Slider
    Link
    List
    ListItem
    Tab
    TabList
    Menu
    MenuItem
    ScrollBar
    Dialog
    // A container that groups its children for a reader without being anything itself -- a toolbar,
    // a form section. Worth a node where a plain layout box is not, because a reader navigates by
    // group.
    Group
    // Static text, which is not a control. Produced by `a11y` for a `Text` outside any control, and
    // not declarable here: a label is text, and text already knows it is text.
    Label
    // The window itself, which `a11y` puts at the root.
    Window
    Unknown
}
```

What kind of control this is -- the first thing a screen reader says, before the name: "button,
Save" and not "Save", because a user who cannot see it needs to know what pressing it would do.

**The vocabulary lives here, on the widget, rather than in `lib/ui/a11y`**, and that is the whole
of what F275 means by accessibility in the first widget rather than the fiftieth. A role declared
where a widget is built is a field that F277's every widget must fill in; a role computed
afterwards by an accessibility layer is a guess about a tree that was not designed to be guessed
about, which is the retrofit this ordering exists to avoid. `a11y` is a projection of what is
declared here, and owns no vocabulary of its own.

The list is the roles that have a counterpart in **both** of the platform APIs this has to reach
one day -- AT-SPI on Unix and UI Automation on Windows -- because a role only one of them knows
becomes a lie on the other platform. `Unknown` is deliberate and is what `focusable_of` produces:
while F277 does not exist, a control whose kind nothing has declared should say so rather than
claim to be a button.

### `Toggle`

```
pub enum Toggle {
    Off
    On
    Mixed
}
```

A check with three states, because every platform accessibility API has three and a bool has two.
The third is a parent checkbox whose children disagree -- "partly checked", which is a state a
user can see and so a state a reader has to be able to say.

### `State`

```
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
```

The parts of a control's state the application declares when it builds the tree.

**Declared, never derived, which is why `focused` is not here.** Focus is held by name, outside
the tree, precisely so that rebuilding the tree cannot move it -- so a `focused` field on a
widget would be a second copy of that fact, free to disagree with the first. `a11y.Node` carries
`focused` because it is computed there, from the focus the caller passes in. The two kinds of
state are different kinds of thing, so they are different fields of different types.

`Option` on the two that are not simply absent-or-false: a control that cannot be checked at all
is not the same as a checkbox that is unchecked, and a reader says "unchecked" for the second and
nothing for the first. A `bool` there would make every button in the window announce itself as
unchecked.

### `no_state`

```
pub fn no_state() -> State
```

A state that declares nothing: what an ordinary control has.

### `Focusable`

```
pub struct Focusable {
    name: string
    role: Role
    label: string
    description: string
    // The control's content: a field's text, a slider's reading. Not its label.
    value: string
    state: State
    child: Widget
}
```

A subtree that can take keyboard focus, under a name.

**Focus is a property of the tree, not of a widget kind.** There are no controls yet -- F277 adds
buttons and entries -- and when there are, a button will *be* a `Focusable` wrapping a group with
a label in it rather than a new kind that happens to be focusable. Marking it here means focus,
hit testing and accessibility all agree on what a control is, from one declaration.

The name is required rather than optional: an unnamed focusable cannot be focused, since focus is
held by name so that inserting a sibling above a control does not move it.

**`name` and `label` are not the same thing and conflating them is the bug this split exists to
prevent.** `name` is the identifier focus is held by, chosen by the programmer and never shown;
`label` is what a person hears. A toolkit that uses one for both makes a screen reader announce
"btn_save_2", which is the single most common defect in a UI that draws its own widgets. `label`
is empty in the common case, and then `a11y` takes the name from the text inside -- a button
wrapping a label called "Save" announces "Save" with nothing declared.

### `focusable_of`

```
pub fn focusable_of(name: string, child: Widget) -> Widget
```

A focusable subtree whose kind nothing has declared -- `Role.Unknown`, no label, no state.

Kept at two arguments, because it is what the focus and hit-testing tests want and they are not
about semantics. `control_of` is the one to reach for when building something a person will use.

### `control_of`

```
pub fn control_of(name: string, role: Role, child: Widget) -> Widget
```

A control that says what it is. The label is left empty to take it from the text inside, which is
what a button or a list item wants.

### `labelled_control`

```
pub fn labelled_control(name: string, role: Role, label: string, child: Widget) -> Widget
```

A control that says what it is, what it is called and what state it is in -- the full form, which
`with` makes readable at a call site:


```
widget.labelled_control("notify", widget.Role.CheckBox, "Notify me", box)
```

### `Widget`

```
pub enum Widget {
    // Occupies nothing and paints nothing. The honest answer for "no child here", rather than an
    // `Option<Widget>` at every site that holds one.
    Nothing
    Text(Label)
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
    Scroll(Viewport)
}
```

### `flex`

```
pub fn flex() -> Widget
```

### `flex_of`

```
pub fn flex_of(weight: float) -> Widget
```

## Measurement

### `Metrics`

```
pub struct Metrics {
    text_width: fn(string, float) -> float
    line_height: fn(float) -> float
}
```

How wide a string is, and how tall a line of it is, at a given em size.

Two functions rather than a font, so that nothing here can fail and nothing here needs a face.
`font.advance` answers a `Result` -- a standard font that is not Courier cannot be measured
without the file -- and threading that failure through every layout would make a window's shape
a fallible computation. A caller resolves it once, when it loads its font.

### `metrics_of`

```
pub fn metrics_of(text_width: fn(string, float) -> float, line_height: fn(float) -> float) -> Metrics
```

### `metrics_for`

```
pub fn metrics_for(f: font.FontRef) -> Result<Metrics, string>
```

The real metrics of a face.

**Fails here rather than per measurement**, which is the whole reason it exists: `font.advance`
answers a `Result` -- one of the standard fourteen that is not Courier cannot be measured without
the file -- and a `Metrics` whose functions could fail would make every box in a window a
fallible computation. So the face is probed once, now, and a face that answers once answers for
every string; the closures then treat a later failure as a zero width, which cannot happen for a
face that got past this line.

### `measure`

```
pub fn measure(w: Widget, m: Metrics) -> Size
```

The size a widget wants, given nothing: its *natural* size, before any space is shared out.

A `Flex` measures zero, which is what makes it flexible -- it has no natural size, so everything
it is given is leftover space.

## Layout

### `Laid`

```
pub struct Laid {
    bounds: Rect
    // The position in the tree: "" for the root, then "0", "0/1", "0/1/2". Stable for a tree whose
    // shape does not change, and the thing to use when no `name` was given.
    path: string
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
```

A widget with a place on the screen.

`widget` is the node itself, so `paint` and a hit test can read its styling without walking the
original tree in parallel; its children are in `children` here and are **not** read from
`widget`. That duplication is deliberate -- the alternative is a second tree walk that has to
stay in step with this one, which is the bug every retained UI makes once.

### `layout`

```
pub fn layout(w: Widget, within: Rect, m: Metrics) -> Laid
```

Places `w` inside `within` and every descendant inside that.

**A widget is given the space it is given, not the space it asked for.** A stack's children are
measured to decide how to divide the room, but a child wider than its share is not clipped here
and not shrunk: it is laid out at its share and overflows, which `paint` then draws outside the
parent. Clipping is a decision for a scroll container (F277) and silently shrinking would hide
a layout mistake rather than show it.

## Hit testing

### `at`

```
pub fn at(l: Laid, p: Point) -> Option<Laid>
```

The deepest node whose bounds contain `p`, or `None` when the point is outside the tree.

**Later siblings win, because they are painted later and so are on top.** Searching in tree order
and keeping the first match would report the node *underneath* an overlapping one, which is the
bug that makes a dialog's buttons unclickable while looking perfectly correct in a screenshot.

### `contains`

```
pub fn contains(r: Rect, p: Point) -> bool
```

**Half-open on the right and bottom**, so two boxes that share an edge do not both contain the
point on it -- a click on the boundary between two list rows must select one of them, and which
one it is should not depend on which was searched first.

### `is_control`

```
pub fn is_control(l: Laid) -> bool
```

Whether a laid node is a control: its own widget takes focus.

## Painting

### `paint`

```
pub fn paint(l: Laid, m: Metrics) -> scene.Scene
```

The laid-out tree as a `Scene`, parents before children so a background sits behind what it is
behind.

Text is placed on its **baseline**, which is why a label's y is its bounds' top plus the line
height rather than the top itself: a glyph drawn at the top of its box has its ascenders above
the box and looks like it has escaped.

### `paint_ops`

```
pub fn paint_ops(l: Laid, m: Metrics) -> [Op]
```

The ops alone, for a caller composing a scene of its own.
