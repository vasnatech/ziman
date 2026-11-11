# `window`

A window, the pixels in it, and what the user did -- the platform floor under a graphical
application.

**This module draws nothing.** `lib/graphics` describes a drawing as a `Scene` and rasterises it;
this puts the resulting pixels on screen. The two meet at `present`, which takes a flat buffer of
premultiplied RGBA floats and **knows nothing about where it came from** -- not a `raster.Surface`,
not an `image.Image`, so this module does not depend on `lib/graphics` at all and the same program
works against a renderer that takes two seconds a frame and one that takes two milliseconds. That
is the whole reason the seam is this narrow, and it is why `present` takes three primitives
instead of one type that would have coupled the two.

**Everything about events is in Ziman, not in C.** The native hands over a flat array of integers
and `decode_events` turns it into `Event`s, exactly as `terminal` reads bytes in C and decodes
keys in Ziman. The decoder, the key names and the modifier tests are therefore reachable from
`ziman test` with no display attached, which is where almost all of the behaviour worth testing
is.

See docs/design/desktop-applications.md, and docs/reviews/tasks/f269-f277.md F269 for what this
is and is not meant to be yet.

`use window` -- or `use window.Name` to bring one item in bare.

## Example

```
use window

// **The event protocol is pure, and that is where the behaviour is.** The native hands over a flat
// array of integers -- five per event, oldest first -- and `decode_events` turns them into `Event`s.
// So the whole protocol can be exercised with no window, no display and no user, which is what this
// example does and what `tests/stdlib/window_test.zm` gates.
//
// The integers below are what `src/native_window.c` would have written for: a key press, the text
// that keystroke produced, a left click at 10,20, and the user asking to close the window.
fn decoding() {
    val raw = concat(
        concat([1, 115, 4, 0, 0], [3, 115, 0, 0, 0]),
        concat([4, 10, 20, 1, 0], [9, 0, 0, 0, 0])
    )
    for event in window.decode_events(raw) {
        print(describe(event))
    }
}

// **A keystroke arrives as two events and a program wants both.** `KeyDown` carries the keysym and
// the modifiers, so a shortcut can test for Control-S; `Char` carries the text the layout actually
// produced, so a text field inserts what the user typed. Computing either from the other is how a
// program gets a non-US keyboard wrong.
fn describe(event: window.Event) -> string {
    return switch event {
        KeyDown(k) { format("KeyDown {} control={}", window.key_name(k), str(k.control)) }
        KeyUp(k) { format("KeyUp {}", window.key_name(k)) }
        Char(text) { format("Char {}", text) }
        PointerDown(p) { format("PointerDown button {} at {},{}", str(p.button), str(p.x), str(p.y)) }
        PointerUp(p) { format("PointerUp button {}", str(p.button)) }
        PointerMove(p) { format("PointerMove {},{}", str(p.x), str(p.y)) }
        Scroll(s) { format("Scroll {} {}", str(s.horizontal), str(s.vertical)) }
        Resize(s) { format("Resize {}x{}", str(s.width), str(s.height)) }
        // A request, not a fact: nothing has closed, and a program that ignores it keeps running --
        // which is what makes "save before quitting?" possible.
        Close { "Close requested" }
        FocusIn { "FocusIn" }
        FocusOut { "FocusOut" }
        Exposed { "Exposed" }
    }
}

// An unknown kind is dropped rather than turned into something. C can add an event kind without
// this module deciding it means a key the user never pressed.
fn forward_compatible() {
    print(str(len(window.decode_events([99, 1, 2, 3, 4]))))
}

// **Opening a window is deliberately not shown here**, and the reason is a property of the default
// build rather than of this module: `src/native_window.c` keeps its backend behind `ZIMAN_X11`, so
// a plain `ziman` -- the one that runs this manual's examples -- has none, and every window native
// answers an error. `make gui` produces `ziman-gui`, which has one.
//
// The shape, for when you have that build: `with_window` is a scope, so the window is closed
// however the body ends, and `present` takes `width * height * 4` **premultiplied RGBA floats** --
// what a renderer's canvas already holds, which is why it takes that and not bytes. A repaint loop
// keeps one `raster.Surface` and draws into it, so nothing is allocated per frame:
//
//     val surface = try raster.surface_of(320.0, 240.0, 1.0)
//     window.with_window<unit>("title", 320, 240, fn(w) -> Result<unit, string> {
//         try raster.draw_into(surface, scene, options)
//         val px = raster.premultiplied_of(surface)
//         try window.present(w, px.data, px.width, px.height)
//         for event in try window.poll(w) { ... }
//         return ok(unit)
//     })
//
// `present_bytes` is the one to use for pixels that are already bytes -- a decoded PNG, or a buffer
// built a pixel at a time.
fn main() {
    decoding()
    forward_compatible()
}
```

Output:

```
KeyDown s control=true
Char s
PointerDown button 1 at 10,20
Close requested
0
```

### `Event`

```
pub enum Event {
    KeyDown(KeyPress)
    KeyUp(KeyPress)
    // One character of text the server composed. Today this is a single byte's worth, which is
    // ASCII only -- F274 is where composed and multi-byte text arrives, and until then a program
    // that needs more than Latin text should know this is the limit rather than discover it.
    Char(string)
    PointerDown(PointerPress)
    PointerUp(PointerPress)
    PointerMove(Position)
    Scroll(ScrollStep)
    Resize(Size)
    // The user asked to close the window -- the close box, or the window manager. **It is a
    // request, not a fact**: nothing has closed, and a program that ignores it keeps running, which
    // is what makes "save before quitting?" possible.
    Close
    FocusIn
    FocusOut
    // The server wants a region drawn again. A program that repaints every frame can ignore it; one
    // that paints only on change must not.
    Exposed
}
```

What happened, as an application cares about it.

**A key press and the text it produced are separate events**, and both are reported for one
keystroke. A shortcut wants to know that `S` was pressed with Control held; a text field wants the
character the layout and any dead keys actually produced. Deriving either from the other is how
keyboard layouts go wrong -- a program that computes text from a keysym gets a German keyboard
subtly wrong, and one that computes shortcuts from text cannot tell Ctrl-S from a typed `s`.

**The `KeyDown` comes first and the `Char` second**, which is a promise rather than an accident: a
program that decides from the key whether to consume the character needs to see the key first.
They arrived the other way round until a real key press was finally synthesised against a real
window (`tests/run_window_test.sh`), because the order is the backend's and no test over
`decode_events` can see it.

### `KeyPress`

```
pub struct KeyPress {
    // The X11 keysym. A number rather than an enum of every key there is: `key_name` names the ones
    // worth naming and the rest keep their value, which is the same choice `terminal.Key.Unknown`
    // makes and for the same reason -- a program that logs an unknown key is how the gap gets
    // closed.
    keysym: int
    shift: bool
    control: bool
    alt: bool
    // The "logo"/"command" key. Named for what it does rather than for one platform's label.
    meta: bool
}
```

### `Position`

```
pub struct Position {
    x: int
    y: int
}
```

### `PointerPress`

```
pub struct PointerPress {
    x: int
    y: int
    // 1 left, 2 middle, 3 right. The wheel is never a button here: X11 reports it as buttons 4 and
    // 5 and the native turns those into `Scroll`, so this module's callers never learn that.
    button: int
}
```

### `ScrollStep`

```
pub struct ScrollStep {
    x: int
    y: int
    horizontal: int
    vertical: int
}
```

### `Size`

```
pub struct Size {
    width: int
    height: int
}
```

### `Window`

```
pub struct Window {
    handle: int
}
```

A window, which is only a handle: the pixels and the connection live in C.

### `display_scale`

```
pub fn display_scale() -> Option<float>
```

How many pixels the display says a point is, or `None` when it will not say.

**One of the three inputs to that ratio, not the answer**: a user's own setting outranks it, and
1.0 is the fallback when neither speaks. The arbitration is `widget.resolve_density`, which is
pure and knows nothing about X11; this is only the part that has to ask the platform.

Read from `Xft.dpi` over 96, the number every X toolkit has looked at for thirty years. `None`
covers three cases a caller treats identically -- no display, no such setting, and a build with
no window backend -- because in all three the answer is "fall through to the next input".

**Per display, not per monitor.** A laptop beside a HiDPI screen reports one number for both, and
a window dragged between them does not change it. Per-monitor density needs RandR and the
window's position, and is a different function that does not exist yet.

### `with_window`

```
pub fn with_window<T>(
    title: string,
    width: int,
    height: int,
    body: fn(Window) -> Result<T, string>
) -> Result<T, string>
```

**Opens a window, runs `body`, and closes it however `body` ends** -- answers, fails, or dies.

A scope rather than an `open`/`close` pair for the reason `terminal.with_raw` is one: an early
return in the middle of a program should not leak the thing it opened. It is **not** guarding
against a crash, which is the difference from `with_raw` worth knowing: a terminal's settings
outlive the process, so `terminal` restores them from a signal handler, while a window does not --
the display server drops it when the connection goes. So there is no signal machinery here and
none is missing.

### `present`

```
pub fn present(
    w: Window,
    pixels: MutableArray<float>,
    width: int,
    height: int
) -> Result<unit, string>
```

Puts a frame on the window: `width * height * 4` **premultiplied** RGBA floats, row-major, four
per pixel -- the layout a renderer's canvas already holds, which `raster.premultiplied_of` hands
over for a `raster.Surface`.

**Floats rather than bytes, because the bytes were pure cost.** A repaint used to convert the
canvas into `width * height * 4` fresh bytes, which this function then copied out again to pack
into the display's own format: three passes over 4 MB a frame at 1280x800, for an intermediate
nothing else read. The profile put that conversion at the largest single item of a repaint once
the canvas stopped being reallocated, and the collector complained about it unprompted. Now the
floats reach the display's packing in one pass with nothing allocated.

`present_bytes` is still there for a caller holding an `image.Image` -- a decoded PNG, or a
pattern built by hand. This one is for the repaint loop.

Alpha is ignored rather than blended. A window has no backdrop to blend against, and compositing
is the job of whatever produced the pixels. **Ignored, not multiplied in**: the premultiplication
is undone first, so a half-transparent shape shows at its own colour rather than dimmed towards
black, which is what the byte path has always done and what this stays identical to.

### `Region`

```
pub struct Region {
    x: int
    y: int
    width: int
    height: int
}
```

A rectangle of a window, in device pixels. `raster.Painted` is the same four numbers coming the
other way, which is how a regional redraw hands over exactly what it touched.

### `present_region`

```
pub fn present_region(
    w: Window,
    pixels: MutableArray<float>,
    width: int,
    height: int,
    region: Region
) -> Result<unit, string>
```

Puts **one rectangle** of a frame on the window and leaves the rest of it alone.

The buffer is still the whole frame -- `width` and `height` describe it, as `present` does -- and
`region` says which part of it changed. Everything outside is left as the window already had it,
which is what makes this worth having: the pixels are converted and pushed for the region alone,
so a repaint of a caret costs a caret rather than a window.

Pair it with `raster.draw_into_region`, which answers the device rectangle it painted:


```
val painted = try raster.draw_into_region(surface, scene, options, changed)
val px = raster.premultiplied_of(surface)
try window.present_region(w, px.data, px.width, px.height, window.Region {
    x: painted.x, y: painted.y, width: painted.width, height: painted.height
})
```

**A region reaching outside the frame is clipped, not refused**, because a window manager reports
damage in its own coordinates and may well report some that has nothing left to cover; a region
entirely outside presents nothing and says so by returning normally. An empty one is a caller
mistake and is refused.

### `present_bytes`

```
pub fn present_bytes(w: Window, pixels: [byte], width: int, height: int) -> Result<unit, string>
```

Puts `pixels` on the window: `width * height * 4` bytes, RGBA, row-major, no padding -- the layout
`image.Pixels.Rgba8` already has, so an `image.Image`'s `data` needs no conversion.

For a frame a renderer just drew, prefer `present`: this form costs a 4 MB conversion and a copy
per frame that `present` does not. This is the one to use when the pixels are *already* bytes --
a decoded image, or a buffer built a pixel at a time.

Alpha is ignored rather than blended, as in `present`.

### `size`

```
pub fn size(w: Window) -> Result<Size, string>
```

The window's size as the display server last reported it, which is not necessarily what was
asked for: a window manager may have resized it on the way to the screen.

### `id`

```
pub fn id(w: Window) -> Result<int, string>
```

The display server's id for this window. **For tests, not for applications**: F269's gate
screenshots the real window with `import -window <id>` and compares pixels, which needs the id
the window manager knows it by.

### `poll`

```
pub fn poll(w: Window) -> Result<[Event], string>
```

Every event waiting, oldest first, or an empty array when nothing happened. Never blocks.

### `decode_events`

```
pub fn decode_events(raw: [int]) -> [Event]
```

**Pure, and the reason this is the interesting function in the module**: the whole event protocol
can be tested by handing it an array of integers, with no window, no display and no user.

### `key_name`

```
pub fn key_name(k: KeyPress) -> string
```

The name of a key worth naming, or its keysym as text. The same bargain `terminal.key_name`
makes: the keys a program dispatches on are named, and anything else keeps its number so that a
program can log it rather than silently treat it as a key the user did not press.
