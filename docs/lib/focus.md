# `focus`

Which control has the keyboard, what a click lands on, and where Tab goes next.

docs/reviews/tasks/f269-f277.md F272. **Small in code and the part that decides whether a toolkit
feels broken**: focus order that does not follow the visual order, or a click that lands on the
wrong widget by a pixel, are both immediately obvious to a user and invisible to a test that only
compares rendered scenes. So all of it is pure functions over a `Laid` tree, and all of it is
asserted against positions and names rather than looked at.

**Focus is held by name, never by path or by node.** A path changes when a sibling is inserted
above it, and a node is a value that is rebuilt every layout -- so either would move focus when
an unrelated part of the window changed, which a user sees as the cursor jumping for no reason.
`widget.Focusable` requires a name for exactly this.

`use focus` -- or `use focus.Name` to bring one item in bare.

## Example

```
use focus
use geom
use paint
use widget

// Measurement is injected, so a window's layout -- and therefore its focus order and its hit
// testing -- can be computed with no font and no screen.
fn m() -> widget.Metrics {
    return widget.metrics_of(
        fn(text: string, size: float) -> float { return (len(text) as float) * 10.0 },
        fn(size: float) -> float { return size * 2.0 }
    )
}

// **A control is a `focusable_of`, not a widget kind.** When F277 adds buttons, a button *is* this
// wrapping a group with a label in it -- so focus, hit testing and accessibility all agree on what
// a control is from one declaration.
fn button(name: string) -> widget.Widget {
    return widget.focusable_of(name, widget.fixed_of(30.0, 10.0, widget.label_of(name, 8.0, paint.black())))
}

fn hit(l: widget.Laid, x: float) -> string {
    return switch focus.control_at(l, geom.point_of(x, 5.0)) {
        Some(one) { one.name }
        None { "nothing" }
    }
}

fn main() {
    val window = widget.row_of(0.0, [button("open"), button("save"), button("quit")])
    val laid = widget.layout(window, geom.rect_of(0.0, 0.0, 90.0, 10.0), m())

    // **Tab order is tree order**, which for an ordinary nesting is the visual order -- and unlike
    // sorting by position it is readable from the code that builds the window, unambiguous for
    // anything not on a grid, and stable when a resize reflows a row.
    print(concat("first:    ", focus.first(laid)))
    print(concat("after it: ", focus.next(laid, focus.first(laid))))
    print(concat("wraps to: ", focus.next(laid, focus.last(laid))))
    print(concat("back from the first: ", focus.previous(laid, focus.first(laid))))

    // Bounds are half-open, so a point on the line between two controls belongs to exactly one of
    // them. A click on the boundary between two list rows has to pick one.
    print(concat("at x=5:   ", hit(laid, 5.0)))
    print(concat("at x=30:  ", hit(laid, 30.0)))
    print(concat("at x=120: ", hit(laid, 120.0)))

    // A click moves the focus to what it hit, and a click on nothing **leaves it alone** -- which
    // is what every desktop toolkit does, because clearing it would silently disarm the keyboard.
    print(concat("click on save:    ", focus.clicked(laid, geom.point_of(35.0, 5.0), "open")))
    print(concat("click on nothing: ", focus.clicked(laid, geom.point_of(200.0, 5.0), "open")))

    // A focus the tree no longer holds restarts rather than answering nothing: a control can
    // disappear between layouts, and the keyboard has to keep working.
    print(concat("holds a deleted control: ", str(focus.holds(laid, "deleted"))))
    print(concat("tab from it:             ", focus.next(laid, "deleted")))
}
```

Output:

```
first:    open
after it: save
wraps to: open
back from the first: quit
at x=5:   open
at x=30:  save
at x=120: nothing
click on save:    save
click on nothing: open
holds a deleted control: false
tab from it:             open
```

### `controls`

```
pub fn controls(l: Laid) -> [Laid]
```

Every control in the tree, in **tree order**: a row's children left to right, a column's top to
bottom, parents before children.

Tree order rather than geometric order, and that is a decision. Sorting by position reads as the
obvious answer and is worse: it is ambiguous for anything not on a grid (two boxes at the same y
but different heights have no natural order), it changes when a window is resized and a wrapped
row reflows, and it means a programmer cannot see the Tab order by reading the code that builds
the window. For an ordinary nesting the two agree, which is why tree order is the one to pick.

### `first`

```
pub fn first(l: Laid) -> string
```

The name of the first control, or "" when the tree has none -- which is a real state, not an
error: a window of labels has nothing to focus and must not pretend otherwise.

### `last`

```
pub fn last(l: Laid) -> string
```

### `next`

```
pub fn next(l: Laid, current: string) -> string
```

The control after `current`, wrapping at the end.

**A `current` the tree no longer contains starts from the beginning** rather than answering "".
A control can disappear between one layout and the next -- a row removed, a panel collapsed --
and focus that was on it must land somewhere a Tab can continue from, or the keyboard stops
working until the user clicks.

### `previous`

```
pub fn previous(l: Laid, current: string) -> string
```

### `holds`

```
pub fn holds(l: Laid, name: string) -> bool
```

Whether `name` is a control this tree still contains. What a caller checks before trusting a
focus it has been holding across layouts.

### `node_of`

```
pub fn node_of(l: Laid, name: string) -> Option<Laid>
```

The laid node of the focused control, or `None`.

### `control_at`

```
pub fn control_at(l: Laid, p: Point) -> Option<Laid>
```

The control a point lands on: the **innermost** one containing it, or `None` for a point that is
outside every control.

Innermost rather than outermost, because controls nest -- a button inside a toolbar inside a
panel -- and a click on the button is a click on the button. `widget.at` finds the deepest node
of any kind; this walks back up from there to the nearest control, which is what makes clicking
a button's *label* report the button.

### `clicked`

```
pub fn clicked(l: Laid, p: Point, current: string) -> string
```

What a click does to the focus: moves it to the control under the pointer, or leaves it alone
when the click landed on nothing focusable.

**Leaves it alone rather than clearing it**, which is the behaviour every desktop toolkit has: a
click on a panel's background should not silently disarm the keyboard, because the user's next
keystroke would then go nowhere and they would not know why.
