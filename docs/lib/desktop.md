# `desktop`

The desktop backend's adapter, both ways: a `window.Event` as a `controls.Input`, and a
`scene.Scene` onto the window's pixels.

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
**The repaint half lives here too, and that is the reason this file is `desktop` rather than
`events`.** Joining a renderer to a window is exactly the backend-specific work this module
exists to hold: `raster` knows nothing about windows and `window` knows nothing about scenes --
deliberately, each says so in its own header -- so something has to own a surface, remember what
it last showed, and work out what to send. That something is backend glue, and this is where the
backend glue goes.

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

## Repainting

### `Painter`

```
pub struct Painter {
    surface: raster.Surface
    // `None` before the first frame, and after `forget`.
    shown: Option<scene.Scene>
    // What the surface was made for, so a resize or a changed ratio is noticed rather than refused.
    width_pt: float
    height_pt: float
    px_per_pt: float
}
```

A window's pixels, kept between frames, with the scene they last showed.

**Keeping the last scene is the whole mechanism.** `raster.damage_between` can say what changed
between two scenes, but only if the previous one still exists -- and because a `Scene` is an
immutable value, keeping it costs a reference rather than a copy. That is what makes this a few
lines rather than a change to the widget tree: F271 made the tree pure and rebuilt every frame,
so there is nowhere in a widget to record "I changed", and no need for one.

### `Repaint`

```
pub struct Repaint {
    // The painter for the next frame. Values are immutable, so the remembered scene comes back this
    // way: `p = try desktop.repaint(p, ...).next` is the shape of a loop.
    next: Painter
    // The pixels redrawn, or **`None` when the scene was unchanged** -- in which case nothing was
    // drawn and nothing was sent to the display at all. Worth reading: it is the signal that a loop
    // can afford to wait for the next event rather than spin.
    region: Option<raster.Painted>
}
```

What one `repaint` did.

### `painter_of`

```
pub fn painter_of(width_pt: float, height_pt: float, px_per_pt: float) -> Result<Painter, string>
```

### `forget`

```
pub fn forget(p: Painter) -> Painter
```

Forgets what the window is showing, so the next `repaint` redraws all of it.

**This is what an `Exposed` event needs**, and without it a damage-driven loop is wrong rather
than slow: a compositor can lose a window's contents and ask for them again, and the scene has not
changed, so `damage_between` would correctly answer "nothing" and the window would stay blank. The
same applies after anything else draws over the window behind this code's back.

### `repaint`

```
pub fn repaint(
    p: Painter,
    w: window.Window,
    s: scene.Scene,
    o: raster.Options
) -> Result<Repaint, string>
```

Draws `s` onto the window, redrawing **only what changed** since the last call.

The whole of the damage path in four steps: ask `raster.damage_between` what differs from the scene
this painter last showed, redraw that rectangle with `raster.draw_into_region`, send that same
rectangle with `window.present_region`, and remember the scene for next time. A first frame, or one
after `forget`, redraws everything because the surface holds no known scene.

**A resize makes a new surface rather than an error.** `draw_into_region` refuses a scene whose
size does not match its surface -- correctly, since the two must agree -- but a caller whose window
just resized should not have to notice that, so a changed size or ratio is taken as the resize it
is. The next frame is a full one, which is right: none of the old pixels mean anything.

## Telling an input method where to put itself

### `caret_on_screen`

```
pub fn caret_on_screen(at: window.Position, laid: widget.Laid, d: Density) -> Option<window.Region>
```

Where the caret is **on the screen, in pixels**, which is the one thing an input method needs
that an application cannot work out on its own.

`at` is the window's own corner, from `window.origin`; the caret's box comes out of the laid
tree, where `controls` names it. Both halves are needed because ibus's `SetCursorLocation` is
defined in root coordinates and a window only knows where its caret is inside itself -- without
the first the candidate list sits at the corner of the display, and without the second it sits
at the corner of the window.

**`None` is the ordinary answer**, not an error: a caret is drawn by a field that has the focus,
so a window whose focus is on a button has none and the method should be left where it was.

The arithmetic is here, and the `ibus` call is not, so that this module keeps no dependency on
an input method and stays testable without one. An application writes:


```
if Some(box) = desktop.caret_on_screen(try window.origin(w), laid, density) {
    ibus.caret_at(box.x, box.y, box.width, box.height)
}
```
