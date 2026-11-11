# `atspi`

Publishing an accessibility tree so a screen reader can read it.

**`a11y` builds a tree and this is what reads it out.** F275 split in two on purpose: the half
that is expensive to retrofit is the vocabulary -- a role and a state per widget, declared where
the widget is built -- and that shipped first, because fifty widgets without it is the retrofit
the ordering exists to avoid. This is the other half, and it is plumbing: take the tree `a11y`
already produces and put it on the bus.

**AT-SPI over D-Bus, through ATK**, which was the choice between two routes. AccessKit's C API
would have reached Windows as well and needs a Rust toolchain to build ziman, where a C compiler
is all it takes today; this reaches Unix and needs nothing that was not already installed. The
narrower reach was taken over the heavier build.

A loop publishes once per frame and pumps every frame:


```
val tree = a11y.tree_of(laid, "curly", focused)
try atspi.publish(tree, "curly")
atspi.pump()
```

Publishing replaces the previous tree rather than patching it, for the same reason `a11y` rebuilds
rather than mutating: the widget tree is rebuilt every frame, so there is no identity to diff
against. A tree is tens of nodes where the pixels it describes are millions.

`use atspi` -- or `use atspi.Name` to bring one item in bare.

### `publish`

```
pub fn publish(tree: a11y.Tree, app: string) -> Result<unit, string>
```

Puts `tree` on the accessibility bus under the application name `app`.

**Needs a ziman with the accessibility backend** -- `make gui` builds it, an ordinary `ziman` does
not, and without it this answers an error saying so rather than failing quietly. It also needs a
session accessibility bus: `at-spi2-registryd`. An application that cannot publish should carry
on drawing, so the error is returned rather than raised.

### `pump`

```
pub fn pump()
```

Lets the bridge answer what the bus has asked. **Call it every frame**: a window loop is not a
glib main loop and the bridge dispatches on glib's, so this lends it a turn. It does not block,
so a frame with nothing asking costs nothing.
