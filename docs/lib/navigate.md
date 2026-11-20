# `navigate`

Where an application is, and what changing that owes the user.

**Not a router.** No URLs, no paths, nothing parsed: a screen name is a string the application
chooses and this module never interprets. And not a registry -- it does not hold the screens,
only where you are and what happens when that changes.

**"Show one of several" is deliberately absent.** A screen is a widget the caller builds, and a
caller that wants one of fifty builds the one; a container taking all fifty to draw one would be
asking for forty-nine widgets in order to throw them away. `controls.Tabs` holds only the strip
for the same reason.

What is here is the part an application gets wrong once per screen: what back means, and where
the focus and a screen reader go when the screen changes.

`use navigate` -- or `use navigate.Name` to bring one item in bare.

## The history

### `History`

```
pub struct History {
    // Oldest first. Never empty once built.
    entries: [string]
    at: int
}
```

Where we have been, and where in that we are.

**The position moves; the entries do not shrink.** A history that popped on `back` could not
offer `forward` at all, and "it is a stack" is the wrong model for exactly that reason.

### `history_of`

```
pub fn history_of(first: string) -> History
```

### `here`

```
pub fn here(h: History) -> string
```

### `can_go_back`

```
pub fn can_go_back(h: History) -> bool
```

### `can_go_forward`

```
pub fn can_go_forward(h: History) -> bool
```

### `back`

```
pub fn back(h: History) -> History
```

### `forward`

```
pub fn forward(h: History) -> History
```

### `went_to`

```
pub fn went_to(h: History, screen: string) -> History
```

Go somewhere.

**Going where you already are does nothing**, rather than adding a second entry: a list that
re-opens the record already open would otherwise need two `back`s to leave it, and the first
would look as though it had failed.

**And going somewhere new throws away what was ahead**, which is what a browser does and what a
user expects. The alternative is a forward button that goes somewhere they have never been.

## The screen

### `screen_widget`

```
pub fn screen_widget(
    name: string,
    title: string,
    content: widget.Widget,
    f: controls.Frame
) -> widget.Widget
```

A screen: its title, its content, and the region that says it has arrived.

**A function and a named group, not a control** -- F323's answer, for F323's reason. A screen
that was focusable would be a tab stop sitting in front of its own content, so a user would
press Tab to get past the thing they are looking at.

**The title is said as well as drawn.** A reader announces what the user moves to; when the
screen changes nobody has moved, so without this nothing is said at all. The region is the same
one `a11y.announcements` already watches.

### `screen_region`

```
pub fn screen_region(name: string) -> string
```

The name of the region a screen speaks through, which a caller needs to diff two frames.

### `arrived_at`

```
pub fn arrived_at(laid: widget.Laid, screen: string) -> string
```

Where the focus should go now that this screen is showing, or "" if it holds nothing to focus.

**The focus is on a control that has just stopped existing.** Left alone it resumes from the
start of the window, so a keyboard user is dropped at the top of a navigation bar every time
they open a record -- fifty-three chances to get it wrong, in an application with fifty-three
screens.
