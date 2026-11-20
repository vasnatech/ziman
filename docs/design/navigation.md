# Navigation: a history, and what a screen change owes the user

Status: **proposed, 2026-11-20.** Nothing below is written. This is the navigation item of
[the health-record port](health-record-port.md) §4.2, which asked for a design pass before a task.

## 1. The gap is not what it looked like

The item was written as "53 views and 28 dialogs need a screen model with history". Two thirds of
that turn out to be already built or not the library's business.

**The 28 dialogs are done.** `controls.dialog_layer` is modal: the geometry shuts the pointer out,
`takes_keys` shuts Tab out, the scrim says the window is blocked and the shadow says which layer
you are looking at. `dialog_keys` gives Escape and Enter. Nothing is missing.

**"Show one of N" is not a library problem.** A screen is a widget the application builds, and an
application that wants one of fifty-three builds the one. A container that took all fifty-three and
drew one would be asking the caller to construct fifty-two widgets in order to throw them away --
and `Tabs` deliberately holds only the strip for the same reason.

So the library's share is the part an application gets wrong, and gets wrong once per screen:

1. **A history** -- what back means, and what a new move does to a forward entry.
2. **What a screen change owes the user** -- where the focus goes, and what a reader is told.

The second is the one worth building. An application with fifty-three screens has fifty-three
chances to leave a keyboard user focused on a control that no longer exists.

## 2. The history

A value, not a widget. Nothing about it draws.

```ziman
pub struct History {
    // Where we have been, oldest first. Never empty once a first entry is pushed.
    entries: [string]
    // Which one is current. Back and forward move this; they do not shorten `entries`.
    at: int
}

pub fn history_of(first: string) -> History
pub fn went_to(h: History, screen: string) -> History
pub fn back(h: History) -> History
pub fn forward(h: History) -> History
pub fn can_go_back(h: History) -> bool
pub fn can_go_forward(h: History) -> bool
pub fn here(h: History) -> string
```

Three decisions, each a thing that is got wrong:

1. **Moving somewhere new truncates what was ahead.** Back three times, then open something else,
   and the three screens you backed out of are gone -- which is what every browser does and what a
   user expects, because the alternative is a forward button that goes somewhere they never were.
2. **Moving to where you already are does nothing**, rather than pushing a second entry. A list
   that re-selects the open record would otherwise need two Backs to leave it, and the second one
   would look like it had failed.
3. **Back and forward do not rewrite the entries**, only the position. A history that popped on
   back could not offer forward at all, and this is the one piece of state where "it is a stack"
   is the wrong mental model.

## 3. What a screen change owes the user

This is the part worth a module. Three things happen when the screen changes, and an application
that forgets any one of them has a defect a sighted mouse user will never see:

1. **The focus must move into the new screen.** It is on a control that has just stopped existing.
   Left alone, Tab resumes from nowhere -- in practice from the start of the window, so a keyboard
   user is dropped at the top of a navigation bar every time they open a record.
2. **The new screen must be announced.** A reader says what the user moves to; nobody moved, so
   nothing is said. `a11y.announcements` (F329) already carries the mechanism: a screen is a
   `Role.Status` region holding its own title, so arriving at one speaks it.
3. **The screen's title must be a real name**, not a widget somewhere that happens to be a heading.

The shape follows F323's answer and F324's: **a function and a named group, not a control**, and
what is not shown is not laid out.

```ziman
pub fn screen_widget(name: string, title: string, content: widget.Widget, f: Frame) -> widget.Widget
pub fn arrived_at(laid: widget.Laid, screen: string) -> string   // the name to focus
```

**It is not a `Control`**, for P3's reason: a screen that was focusable would be a tab stop in
front of its own content, and the user would press Tab to get past the thing they are looking at.

## 4. What this is not

- **Not a router.** No URLs, no paths, no parsing. A screen name is a string the application
  chooses and the library never interprets.
- **Not a screen registry.** The library does not hold the fifty-three; it holds where you are and
  what happens when that changes.
- **Not animated transitions.** A separate question, and one that wants a clock the library does
  not have -- see `Progress.Busy` and the spinner, which both take a phase from the caller.
- **Nothing about any particular application.** A screen is a titled region with a name. What is
  on it is the caller's, as a dialog's content already is.
