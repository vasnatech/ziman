# `desktop`

The desktop backend's adapter: a `window.Event` as a `controls.Input`.

**This module exists so that `lib/ui/` need not know what X11 is.** The control set -- buttons,
fields, lists, menus, dialogs -- is the same logic whatever is drawing it, and the thing that
differs between a desktop, a terminal and a browser is only how an event arrives. So the shared
half names keys (`controls.Key`), each backend has an adapter like this one, and `lib/ui/browser`
will be another of these rather than another copy of the controls.

It was not always so. `controls.Input` used to carry `window.KeyPress` directly, on the argument
that a key vocabulary of its own would be a third copy of fifteen magic numbers. That held with
one backend. With more than one it inverts: a vocabulary in the shared layer is the *one* copy,
and this file is where the numbers stop.

**The keysym numbers are still in exactly one place**, and it is not here: this maps
`window.key_name`'s output, which is the name X11 already agreed on, so adding a key means
touching `window.zm` and this table and nothing else.

`use desktop` -- or `use desktop.Name` to bring one item in bare.

### `input_of`

```
pub fn input_of(e: window.Event, d: Density) -> Option<controls.Input>
```

What a window event means to the control layer, or `None` for one it has no use for.

**Pixels become points here**, which is the input half of the unit: a pointer position arrives
from the display in device pixels and every function in `lib/ui/` expects points. One conversion,
at the edge, for the reason `widget.Density` gives.

`Resize`, `Close`, `FocusIn`, `FocusOut` and `Exposed` answer `None` -- not because they do not
matter, but because they are the *application's* business: a control cannot decide whether a
window should close.

### `stroke_of`

```
pub fn stroke_of(k: KeyPress) -> controls.Keystroke
```

A keystroke with its modifiers, from the platform's own.
