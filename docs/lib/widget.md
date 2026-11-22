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
        surface: widget.filled(paint.rgb(0.9, 0.9, 0.9)),
        align: widget.stretched(),
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

    // A name is what a focus or accessibility layer refers to a node by; an index is where it
    // sits. **Prefer the name**: a position changes when a sibling is inserted above it, and
    // focus that moved because an unrelated widget appeared is a bug a user notices and a test
    // does not. Joining a node's index with its ancestors' gives the path an accessibility tree
    // keys an unnamed node by -- `a11y` does that as it walks, so that layout need not build a
    // string for every node of every pass.
    printf(
        "the label is named {} and is child {}\n",
        laid.children[0].children[0].children[0].name,
        laid.children[0].children[0].children[0].index
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
the label is named menu-file and is child 0
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
    // Which face to draw it in: an index into the faces a window gave `widget.Metrics` and
    // `raster.Options.fonts`, 0 being the text face.
    //
    // **An index rather than a name, because the layer below uses one** -- `scene.text_of` takes
    // an `int` and `raster.Options.fonts` is positional -- and because a name here would need a
    // table at paint time that only the application can fill. Meaning is added one layer up:
    // `controls.Style` names the faces it uses by purpose, so no control writes a literal.
    font: int
    // What a focus or accessibility layer refers to this node by, or "" for none. **A name, not an
    // index**: a path changes when a sibling is inserted above it, and focus that moved because an
    // unrelated widget appeared is a bug a user notices and a test does not.
    name: string
    // Whether this run breaks across lines when it is given less width than it wants.
    //
    // **Off by default, and that is the right default rather than the compatible one.** A button's
    // label, a column heading and a table cell are all one line by intent: wrapping them turns a
    // control two rows tall the moment a window narrows. A paragraph says so.
    //
    // **It only wraps where something knows the width.** Inside a column, which passes its own
    // width down; inside a `wide_of`, which states one. In a row, where no child's share is known
    // until every child has been measured, a wrapping label measures its one-line width exactly as
    // it always did -- which is the honest answer to "how wide would you like to be".
    wrap: bool
}
```

A run of text, on one line or wrapped to the width it is given.

**It used to say "no wrapping: a label is one line", and that was not a simplification -- it
was the layout protocol.** `measured` is handed the metrics and not the width it will be laid
into, and the height of wrapped text is a function of the width. So nothing could report how
tall it would be, and every application that needed a paragraph either cut it short or let it
run off the edge. Found by porting a record list, where a free-text reaction measured 420
points on one line and pushed its row's buttons out of the window.

What makes it possible now is `measured_in`: a column knows its own width and passes it down,
so a label inside one is measured against the width it will actually get. See `measured_in`
for why a *row* still does not, and what that costs.

### `Painted`

```
pub struct Painted {
    draw: fn(geom.Rect) -> [scene.Op]
    // What a click or a hit test calls this, as every other named widget has.
    name: string
}
```

A path drawn into whatever box the layout gives it.

**The path is in a unit box**, from (0,0) to (1,1), and is scaled to the bounds when painted.
That is what lets one icon serve a 12-point label and a 40-point button without a second
drawing of it, and it is the whole reason an icon has no size of its own: the control it
belongs to decides, from the text it sits beside.

One colour, because this is a *mark* rather than a picture. A mark tints with the control's
state -- disabled, pressed -- and a picture does not; conflating them is how a disabled button
ends up with a bright icon on a grey label.
Arbitrary drawing: whatever a caller wants, in whatever box the layout gives it.

**The escape hatch every toolkit has** -- Swing's `paintComponent`, GTK's `draw` -- and the one
this one did not. `Drawing` is a single path in a single colour, which is an icon; anything with
two colours in it, or any text, had nowhere to go. A chart is the case that found this.

**A function of the box, not a fixed list of ops.** `Drawing` authors its path in a unit box and
scales it, which is right for an icon and wrong for anything with words in it: scaling a unit
box would scale the type with it, so a wide chart would have larger labels than a narrow one.
Being handed the real box instead keeps a 9-point label 9 points at every size.

**It has no children and no text**, so a reader is told nothing by it -- which is honest rather
than convenient. A canvas that carries meaning owes the caller an accessible form of that
meaning beside it; `chart` publishes its rows as a table for exactly this reason.

### `canvas_of`

```
pub fn canvas_of(name: string, draw: fn(geom.Rect) -> [scene.Op]) -> Widget
```

A canvas: draw whatever you like into the box this is given.

### `Drawn`

```
pub struct Drawn {
    path: shape.Path
    colour: Color
    name: string
}
```

### `Pictured`

```
pub struct Pictured {
    image: int
    // The picture's own size **in points**, which is what `measure` answers and what `fit` fits.
    //
    // **Given rather than discovered.** The widget layer never sees the pixels -- the images table
    // belongs to the painter -- and a tree that had to be laid out against it could not be built
    // before one existed. A caller that decoded the image has its dimensions and its own idea of
    // how many points a pixel is; both are decisions this layer must not make.
    width: float
    height: float
    fit: Fit
    // **What a reader is told, and a picture must declare it.** The same rule a control follows: a
    // picture that gathered its name from somewhere else would be named by whatever happened to sit
    // next to it. Empty means decorative -- a border, a texture -- and a reader is told nothing
    // rather than told something useless.
    alt: string
    name: string
}
```

A decoded picture drawn into whatever box the layout gives it.

**Content, where an icon is a mark.** That line was drawn when `Button.icon` was built: an image
is a `Widget` and an icon is a *field* on a control. Everything about this type follows from it
-- a picture has a size of its own where a mark has none, it keeps its own colours where a mark
tints with the control's state, and it carries alternative text where a mark is silent beside the
label it belongs to.

**`image` is an index into `raster.Options.images`**, for the same reason `Label.font` indexes
`fonts`: the scene carries handles and the application fills the table. `scene.image` has taken
one since the renderer existed; this layer simply never offered a way to say so, which is what
made "no concept of an image at all" true of a toolkit that could already draw one.

### `Fit`

```
pub enum Fit {
    // Its own size, centred in the box. What a thumbnail of a known size wants.
    Natural
    // The largest it can be inside the box with its aspect kept, centred -- the common case for a
    // photograph in a panel whose size the window decides.
    Contain
    // `Contain`, but **never larger than its own size** -- CSS's `scale-down`, and the fit a
    // *preview* wants. The difference shows on small pictures: a 16-point favicon in a 600-point
    // pane is a blurry wall under `Contain` and a 16-point favicon under this, which is what it
    // is. Found by showing an image response in curly, where `Contain` enlarged everything small.
    Shrink
    // The box exactly, aspect ignored. Right for a texture or a gradient and wrong for a face.
    Stretch
}
```

What a picture does when its box is not its own size.

**Three, and the fourth is named here rather than guessed at.** CSS calls these `none`,
`contain` and `fill`; the missing one is `cover`, which fills the box and crops the overflow --
it needs a clip, and a clip in this renderer was a full-surface mask until it learned to narrow
the window instead (2026-11-13). It is affordable now and is the obvious next variant; adding it
will break every switch here, which is the point.

### `picture_box`

```
pub fn picture_box(p: Pictured, within: Rect) -> Rect
```

Where a picture is drawn inside the box it was given.

**Public, because a caller that needs to know where the pixels landed cannot ask the painter.**
A hit test on a photograph, an overlay drawn on top of one, a test asserting a fit: all of them
need this answer and none of them is painting.

### `picture_of`

```
pub fn picture_of(image: int, width: float, height: float, alt: string) -> Widget
```

A picture at its own size, centred in whatever box it is given.

### `picture_fitted`

```
pub fn picture_fitted(image: int, width: float, height: float, alt: string) -> Widget
```

A picture fitted into its box, aspect kept -- what a photograph in a panel wants.

### `picture_shrunk`

```
pub fn picture_shrunk(image: int, width: float, height: float, alt: string) -> Widget
```

The same, except that a picture smaller than its box is left at its own size -- what a *preview*
wants, where the box is whatever the window has spare and the picture is whatever arrived.

## Icons

### `IconSource`

```
pub enum IconSource {
    // A path in a **unit box**, from (0,0) to (1,1), scaled into whatever square the control
    // gives it. `shape` has everything needed to write one and `scene.fill` draws it, so this
    // source costs no new native code and no registry: see `Drawn`.
    Drawn(shape.Path)
    // One character from a face -- an icon font's private-use code point, or a character from the
    // text font where that is what the mark is.
    //
    // **This was blocked until the face stopped being a literal 0.** The whole of the blockage
    // was `paint` passing `0` to `scene.text_of`, so a `Label` could not name the second face a
    // window had loaded; the renderer has taken an index since it existed. `Style.icon_font` says
    // which face, so the control carries the character and the theme carries the font.
    //
    // **The glyph is a string, not a code point**: a caller that has `"\u{e5ca}"` in a constant
    // can put it here, and this layer never has to encode anything.
    //
    // An icon font is asked for by name, so a missing glyph is the caller's to notice -- unlike
    // the disclosure arrows, which were strings in `Style` until a face without them drew `.notdef`
    // in every combo box, and are a path now. See `controls.chevron`. It is one character by convention and nothing enforces that, because an
    // icon font with a two-character ligature is a real thing.
    Glyph(string)
}
```

Where an icon's shape comes from.

**An enum of one, which is the point.** The other four sources are designed and blocked on
things larger than icons -- a glyph from an *icon font* needs `widget` to name a second face
(`scene.text_of` is passed font index `0` here), the desktop's theme needs an SVG reader, a
decoded picture needs a JPEG decoder, and a layered icon wants the others first. Adding a
variant breaks every switch that must handle it, which is how `Drawing` forced its eight sites
to be considered rather than silently defaulted.

### `IconSide`

```
pub enum IconSide {
    Leading
    Trailing
}
```

Which side of the label an icon sits on.

**Not a general alignment**, deliberately: a search glass leads and a clear `x` trails, and
those are the two an interface actually wants. A control that needs an icon somewhere else is
asking for a layout, which is what `Widget` is for.

### `Icon`

```
pub struct Icon {
    source: IconSource
    name: string
    side: IconSide
}
```

An icon on a control: `Button.icon`, and in time a label's.

**An icon is a field on a control and an image is a `Widget`**, which is the decision this type
exists to carry (2026-11-14). Four things follow differently from it: an icon has **no size of
its own** -- the control derives one from the text it sits beside -- it is **monochrome and
tinted by the control's state**, it is **silent to a reader** when the control has a label, and
it *becomes* the control's name when the control has none. An image has its own size, its own
colours, and needs a description.

`name` is for that last rule. A button with a label does not say it; a button with only an icon
is announced by it, so `"delete"` is what a reader will read out and it is not optional for an
icon-only control.

### `icon_of`

```
pub fn icon_of(path: shape.Path, name: string) -> Icon
```

A drawn icon, before the label.

### `trailing_icon`

```
pub fn trailing_icon(path: shape.Path, name: string) -> Icon
```

The same, after the label -- a disclosure chevron, a clear `x`.

### `icon_widget`

```
pub fn icon_widget(icon: Icon, size: float, colour: Color, face: int) -> Widget
```

The icon as a widget of its own, square at `size` points and inked in `colour`.

**The control calls this; nothing else should need to.** It is the one place an icon becomes a
`Widget`, so the rules an icon has -- a square from the text's measure, one colour from the
control's state, no accessibility text of its own -- are applied once rather than per control.

### `glyph_icon`

```
pub fn glyph_icon(glyph: string, name: string) -> Icon
```

A glyph icon from the icon face: `widget.glyph_icon("\u{e5ca}", "done")`.

### `trailing_glyph_icon`

```
pub fn trailing_glyph_icon(glyph: string, name: string) -> Icon
```

### `drawn_of`

```
pub fn drawn_of(path: shape.Path, colour: Color) -> Widget
```

### `named_drawing`

```
pub fn named_drawing(path: shape.Path, colour: Color, name: string) -> Widget
```

### `label_of`

```
pub fn label_of(text: string, size: float, colour: Color) -> Widget
```

### `label_in_face`

```
pub fn label_in_face(text: string, size: float, colour: Color, face: int) -> Widget
```

A label in a named face: a heading in a bold one, a mark in an icon font.

### `wrapping_label`

```
pub fn wrapping_label(text: string, size: float, colour: Color) -> Widget
```

A paragraph: a run of text that breaks across lines to fit the width it is given.

**The width comes from above, not from here.** A column passes its own content width to every
child it measures, so a paragraph inside one wraps to the column. Inside a row -- where no
child's share is known until all of them have been measured -- it measures its one line, which
is what "how wide would you like to be" honestly answers; put it in a `wide_of` if it must wrap
there.

**Breaks between words and never inside one.** A word longer than the line is left overlong
rather than cut, because a word broken at an arbitrary point is unreadable and a word that
overhangs is merely untidy -- and the one case that matters, a URL in a notes field, is worse
hyphenated than overhanging.

### `wrapping_label_in_face`

```
pub fn wrapping_label_in_face(text: string, size: float, colour: Color, face: int) -> Widget
```

### `broken_into_lines`

```
pub fn broken_into_lines(text: string, m: Metrics, font: int, size: float, room: float) -> [string]
```

### `named_label`

```
pub fn named_label(text: string, size: float, colour: Color, name: string) -> Widget
```

### `Border`

```
pub struct Border {
    colour: Color
    width: float
}
```

A border: a colour and a width, together.

**One value rather than two fields on the surface below**, because a width with no colour and a
colour with no width are both a border that half exists, and a struct able to express them is a
struct that eventually will. Either there is a border and it has both its numbers, or there is
none -- which is the same rule `ScrollBar` and `Tabs` follow for the one number they share with
a hit test.

### `Shadow`

```
pub struct Shadow {
    // How far down it falls. **Down and not sideways**, which is the one thing every system agrees
    // on: the light is above, so a shadow that moved horizontally would say the window is lit from
    // a lamp the user cannot see.
    offset: float
    // How far it spreads past the box.
    blur: float
    // How dark it is at the box's own edge, where it is darkest.
    opacity: float
}
```

What a container paints behind and around its children.

**This is the vocabulary a second look and feel needs, and until 2026-11-12 it was one colour
wide.** `Stack` carried a `background: Option<Color>` and nothing else, so every container in
every theme was a flat rectangle: an application could already replace the whole palette --
`theme.plain()` exists to be replaced -- and still could not draw a button that
looked different, because what distinguishes one look and feel from another is *shape*. The
capability was always one layer down (`shape.round_rect_path`, and `paint_focused` has stroked a
path in this very module since F270); only the way to ask for it was missing.
How far a surface stands off the one behind it.

**Depth is the half of a look and feel that colour cannot carry.** Every system says the same
thing in its own units -- Fluent 2 has six shadow tokens from `$shadow2` to `$shadow64`, each a
sharp *key* shadow plus a soft *ambient* one; Material leans on a tonal overlay with a shadow
behind it -- and all of them mean one thing: a menu is in front of the window, and the eye
should not have to read a border to know it.

**Three numbers rather than six tokens**, because a token set is a theme's business and this is
the vocabulary a theme is written in. `offset` is how far the light pushes it down, `blur` how
far it spreads, `opacity` how dark it gets against the thing behind.

**There is no blur in this renderer, so the shadow is rings.** `raster` fills and strokes paths;
it has no convolution and adding one is a project. A soft edge is therefore approximated by
concentric rounded rings of falling alpha -- which is what every toolkit did before a blur was
cheap, and which costs `shadow_steps` fills on the things that have a shadow and nothing at all
on the things that do not. See `paint_surface`.

### `shadow_steps`

```
pub val shadow_steps = 8
```

How many rings a shadow is drawn as.

**Eight, which is a measurement rather than a taste.** The rings are what a blur would be, and
the eye stops counting them at about this many for the blurs a window actually uses (4 to 24
points); fewer reads as bands, and more costs fills for a difference nobody sees. A shadow is
only ever on the few surfaces that are in front of something -- a dialog, a menu, a popup -- so
this is eight fills per raised thing, not per control.

### `shadow_of`

```
pub fn shadow_of(offset: float, blur: float, opacity: float) -> Shadow
```

### `Surface`

```
pub struct Surface {
    // How far **inside** its node's bounds this surface is drawn, on every side.
    //
    // **Paint time only, and that is an invariant rather than a note.** Nothing in layout and
    // nothing in a hit test reads this: a control reserves a ring of room in its padding, draws
    // its resting frame inset by the whole ring, and spends that inset when a state wants a bigger
    // frame. So the drawn frame grows and **the box never does** -- which is what keeps a control
    // from being hovered at a pixel its layout never claimed, and two neighbours from both
    // claiming the boundary between them. egui concedes the wart in its own documentation, saying
    // its `expansion` "makes hovered things misaligned"; it gets away with it by laying out every
    // frame, which this does not.
    //
    // Zero is the whole surface, which is what every container wants.
    inset: float
    // Painted behind the children, before them. `None` draws nothing, which is what most
    // containers want -- a stack per level each painting a fill is how a UI ends up with twenty
    // overdraws per pixel.
    // **A paint, not a colour, since F295.** A look can name a span of its own ramp as a face
    // (`theme.Fill`), and `from == to` is a flat fill -- so solid and gradient are one form with no
    // `Option` and no second field. A gradient's endpoints are *not* carried here: they are
    // resolved against the box in `paint_surface`, the way `Radius` is, because the tree is built
    // before anything knows how big a control turned out to be.
    fill: Option<Paint>
    // Stroked **inside** the container's own bounds, which is the bargain `paint_focused`'s focus
    // ring already makes and for the same reason: a border outside the box overlaps whatever is
    // next to it, and in a dense form that is the neighbouring control. So a border costs **no
    // layout** -- the application's padding is what leaves room for it -- and that is what keeps a
    // second look and feel from moving the frame cost F284 measured, where building and laying out
    // already dominate painting by six to one.
    border: Option<Border>
    // Rounds the fill and the border alike. `Radius.Square` is the common case.
    //
    // **One radius for all four corners**, with `corners` saying which of them it applies to.
    // Four independent radii is what a design tool offers and what no application here has wanted:
    // the asymmetric box that does occur is a tab, and a tab wants one radius on two corners.
    corner: Radius
    // Which corners `corner` rounds. `Corners.All` for everything that is not a tab.
    corners: Corners
    // What falls behind it, if anything. `None` for every surface that is not in front of
    // something, which is nearly all of them.
    shadow: Option<Shadow>
}
```

### `Corners`

```
pub enum Corners {
    All
    Top
    Bottom
}
```

How far a surface's corners are rounded: a number of points, or a proportion of the box.

**A plain float could not say what a theme means, which is the whole reason this is an enum.**
One absolute radius has to serve a 400-point dialog panel, a 24-point button, a 16-point check
box and a 10-point scroll thumb -- and at `6` the button is right, the check box is very nearly a
circle (`shape.round_rect_path` clamps at *half* the shorter side, which on a small square is a
semicircle) and the thumb is a capsule. Until 2026-11-14 the check box carried its own cap for
exactly this, which was a patch on one control rather than a rule.

* `Square` -- no rounding. What `default_style` uses, so the default look is unchanged.
* `Points(p)` -- an absolute radius, as a design system specifies one (`4`, `6`, `8`), **never
  more than a quarter of the box's shorter side**. That cap is what makes one number right at
  every size: it leaves a tall control exactly as asked (a quarter of a 24-point button is 6)
  and keeps a small one a rounded square rather than a blob.
* `Fraction(f)` -- `f` times the shorter side, which is how a *capsule* is said: `Fraction(0.5)`
  is a pill at any size, and that has to stay sayable because a switch, a badge and a scroll
  thumb are drawn that way on purpose. Nothing is capped here; the clamp in `round_rect_path`
  means anything above 0.5 is a capsule too.

**Resolved against the box being painted, not when the surface is built**, because a widget tree
is built before it is laid out: a control knows its look and the layout knows its size, and this
is where the two meet -- the same seam `paint_focused` uses for the focus ring.
**Which** corners a rounding applies to, where `Radius` says how round they are.

**Two kinds rather than four numbers**, for `Radius`'s reason: a look and feel names a shape and
the painter resolves it against the box. A tab is the control that needed this -- an index card
is rounded along its top and square where it meets the page, and with one rounding for all four
corners a tab strip is a row of buttons with a gap under it, which is what `lib/ui` drew until
2026-11-17.

`Bottom` is here because a strip docked to the bottom of a pane is the same control upside down,
and leaving it out would have made the first such strip a change to this enum rather than a line
in a theme. `Left` and `Right` -- the ends of a segmented control -- are not, and adding one is a
compile error at every switch, which is the point.

### `Radius`

```
pub enum Radius {
    Square
    Points(float)
    Fraction(float)
}
```

### `paint_in`

```
pub fn paint_in(p: Paint, box: geom.Rect) -> Paint
```

The radius to draw with, for this rounding and this box. The one place the rules above are
applied, so a painter, a focus ring and a test cannot disagree about them.
A paint resolved against the box being painted.

**The same bargain `radius_in` makes, and for the same reason**: a widget tree is built before
anything knows how big a control will be, so a gradient cannot carry absolute endpoints. A look
names a direction and this turns it into the two points the renderer wants, once, at paint time.

A solid paint passes straight through, which is what every container and every flat look gets.

### `radius_in`

```
pub fn radius_in(r: Radius, box: geom.Rect) -> float
```

### `no_surface`

```
pub fn no_surface() -> Surface
```

A surface that draws nothing, which is what most containers want. Named for what it is, like
`no_insets` and `no_state`.

### `filled`

```
pub fn filled(colour: Color) -> Surface
```

A flat filled surface -- exactly what `background: Some(colour)` meant before this widened, so a
caller that wanted only a background says so in the same number of words.

### `border_of`

```
pub fn border_of(colour: Color, width: float) -> Border
```

### `rounded`

```
pub fn rounded(fill: Color, corner_radius: float) -> Surface
```

A filled surface with rounded corners.

**These three exist because a `Surface` literal cannot be written in a design file.** `fill` and
`border` are `Option`s, and `*_ui.zm`'s dialect -- deliberately small enough for a tool to parse
-- has no syntax for one: `Option<paint.Color>.Some(...)` is three tokens outside it. So a
design could say a container's padding and its alignment but not its border or its corners,
which is half of what `Surface` was added for. A call says it without an `Option` in sight.

Three rather than one, because the cases a caller means are "filled", "outlined" and "both" --
and the one-constructor alternative needs a sentinel for the absent half, which is what
`Option` is here to avoid.

```
pub fn rounded(fill: Color, corner: Radius) -> Surface
```

**A number of points is the common case and stays the short spelling**, with the `Radius` form
for a design that wants a proportion: these pairs are what a `*_ui.zm` file writes, and
`rounded(colour, 6.0)` is what every one of them already said.

### `outlined`

```
pub fn outlined(border: Color, width: float, corner_radius: float) -> Surface
```

A border and nothing behind it -- a "ghost" panel, where whatever is underneath shows through.

```
pub fn outlined(border: Color, width: float, corner: Radius) -> Surface
```

### `edged`

```
pub fn edged(fill: Color, border: Color, width: float, corner_radius: float) -> Surface
```

```
pub fn edged(fill: Color, border: Color, width: float, corner: Radius) -> Surface
```

### `Across`

```
pub enum Across {
    Stretch
    Start
    Centre
    End
}
```

Where a child sits **across** the stack's direction -- vertically in a row, horizontally in a
column.

**`Stretch` is the default because it is what this layer has always done**, and it is what every
box model defaults to: a stack hands each child the parent's full extent across its direction,
so a `Fixed(40, 80)` in a row 60 tall is laid out 40 wide and 60 tall. The other three give the
child its natural size across and place it, which is the thing that was missing.

### `Along`

```
pub enum Along {
    Start
    Centre
    End
}
```

Where the children sit **along** the stack's direction, when they do not fill it.

Only meaningful when there is room left over, which means **no flexible child** -- a `flex()`
eats the spare room by definition, so a stack holding one is already full and this does nothing.
That is not a special case to remember: it falls out of the arithmetic.

### `Alignment`

```
pub struct Alignment {
    across: Across
    along: Along
}
```

How a stack places its children in the room it has.

One field on `Stack` rather than two, for the reason `Surface` is one field rather than four: a
literal names every field in this language, so a struct keeps the churn to one line per site and
keeps the two halves of one idea together.

### `stretched`

```
pub fn stretched() -> Alignment
```

What every stack did before alignment existed, and still does unless it says otherwise: children
stretched across, packed from the start along.

### `centred`

```
pub fn centred() -> Alignment
```

Centred both ways, which is what a button's label and a check box's mark want.

### `Stack`

```
pub struct Stack {
    direction: Direction
    spacing: float
    padding: Insets
    surface: Surface
    align: Alignment
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

### `padded_of`

```
pub fn padded_of(padding: Insets, child: Widget) -> Widget
```

A child with space around it.

**One helper rather than a padded variant of each container.** `row_of` and `column_of` take no
insets, and adding `padded_row_of` and `padded_column_of` would be two more names for one idea
-- then four, once a surface or an alignment wants the same treatment. Wrapping composes
instead: `padded_of(insets, column_of(4.0, kids))` says it, and costs one node that measures
its child plus the insets.

**It is also what a design file needs.** A `*_ui.zm` describing a padded column had to spell out
a whole `Stack` literal -- seven fields, every one of which anything reading the file has to
understand -- where a named call is one entry in a registry. A format meant to be read by a tool
should not need its reader to know the shape of a struct.

### `Fixed`

```
pub struct Fixed {
    width: float
    height: float
    // Which of the two numbers above actually constrains. A `false` means **ask the child**, which
    // is what `wide_of` and `tall_of` are for.
    //
    // **This exists because `fixed_of(w, 0.0, child)` means a height of zero**, and it was being
    // written where "no opinion about the height" was meant. It worked only while a parent
    // stretched the box back out -- `widget.row_of` does, `Across.Centre` does not -- so the same
    // expression drew correctly in one container and vanished in another. That has cost nine
    // separate defects in this library: the table's header band, a link, the slider's thumb and
    // its track, a star, a popup's rows, a check box's mark, a demo band, and the tab strip, which
    // measured zero the moment the design file that happened to be forcing its height stopped.
    // Each presents identically -- nothing drawn, no error, every test green.
    fixes_width: bool
    fixes_height: bool
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

A child given **both** of its sizes, whatever it would have measured.

### `wide_of`

```
pub fn wide_of(width: float, child: Widget) -> Widget
```

A child given a width, measuring its own height.

**The spelling that was missing.** A tab is a column of this shape -- a stated width so the
strip divides evenly, and whatever height its label and padding come to -- and written as
`fixed_of(width, 0.0, ..)` it is a tab of no height that only looks right inside a parent that
stretches it.

### `tall_of`

```
pub fn tall_of(height: float, child: Widget) -> Widget
```

A child given a height, measuring its own width.

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

## The unit

### `Density`

```
pub struct Density {
    px_per_pt: float
}
```

How many device pixels one point is.

**Every length in this layer is a point, and `float` is all a point is.** A distinct type is what
the argument wants -- mixing points and pixels would then be a compile error rather than a
convention -- and this language will not have it: `Pt + Pt` is a type error with no way to define
one, and a box model is nothing but arithmetic, so the strong version would turn every layout
line into `pt_add(pt_mul(x, 2.0), y)`. The convention is enforced at the two edges instead, which
is this type: points go out through `raster.Options.px_per_pt`, and pixels come in through
`to_pt` below.

### `Source`

```
pub enum Source {
    UserSetting
    Display
    Default
}
```

Which of the three inputs decided a density.

Reported rather than discarded **so that a setting being ignored is visible**. A caller who passed
a user setting and reads back `Display` knows their number was out of range; without this the only
symptom is a UI that is the wrong size for a reason nothing explains.

### `Resolved`

```
pub struct Resolved {
    density: Density
    from: Source
}
```

### `resolve_density`

```
pub fn resolve_density(user: Option<float>, display: Option<float>) -> Resolved
```

A user's setting, what the display reports, or 1.0 -- in that order.

**The user wins, because they are the one looking at it.** A display's reported density is a guess
about a person's eyes and desk from the number of pixels on a panel; it is right often enough to
be the default and wrong often enough that overriding it must always be possible. `None` for the
user is the normal case, not an error.

A value outside `(0, 64]` is treated as absent rather than refused, so a mistyped setting leaves a
usable UI instead of no UI -- and `Resolved.from` is what tells the caller it was dropped.

### `density_of`

```
pub fn density_of(px_per_pt: float) -> Density
```

### `to_px`

```
pub fn to_px(d: Density, pt: float) -> float
```

Points to pixels, for handing a size to something that counts pixels.

### `to_pt`

```
pub fn to_pt(d: Density, px: float) -> float
```

Pixels to points, which is **the one conversion the input path needs**: a pointer position arrives
from the window system in pixels and every function in this layer expects points.

### `point_in_pt`

```
pub fn point_in_pt(d: Density, x: float, y: float) -> Point
```

A pointer position in points, which is what `focus` and `controls` take.

### `Layer`

```
pub struct Layer {
    at: Option<Rect>
    // Whether the keyboard belongs to this layer alone while it is present.
    takes_keys: bool
    content: Widget
}
```

One layer of an overlay -- a menu's popup, a dialog, a tooltip.

**Pointer capture is geometry and keyboard capture is declared, and that asymmetry is deliberate.**
A layer given the whole box is the last sibling containing any point, so `widget.at` stops there:
a click beside a centred dialog lands on the layer, finds no control, and goes nowhere. That *is*
modality for the pointer, and it needs no flag because a rectangle already says it. Nothing about
a rectangle says where Tab may go, so the keyboard half has to be said out loud.

`at` is `None` for a layer that covers its parent, which is what a dialog wants, and `Some` for
one placed explicitly, which is what a popup under a menu title wants -- and a popup must be able
to overhang its parent, so a layered node does **not** clip. That is the difference from
`Viewport`, which exists to clip.

### `Stacked`

```
pub struct Stacked {
    layers: [Layer]
    name: string
}
```

### `base_layer`

```
pub fn base_layer(content: Widget) -> Layer
```

The layer everything else sits over: the parent's whole box, and **it does not claim the
keyboard**.

Named apart from `modal_layer` rather than being one constructor with a flag, because the flag's
default is a trap either way round. A base layer that claimed the keyboard would shut every layer
above it out of the tab order -- which is what the first version of this did, and the test for a
tooltip is what found it.

### `modal_layer`

```
pub fn modal_layer(content: Widget) -> Layer
```

A layer over the parent's whole box that **takes the keyboard**: a dialog. The pointer is shut out
by the geometry -- it covers everything -- and the keyboard by this.

### `layer_at`

```
pub fn layer_at(where_: Rect, takes_keys: bool, content: Widget) -> Layer
```

A layer at an explicit rect: a menu's popup, a tooltip. Clicks outside the rect fall through to
what is beneath, which is what lets a menu be dismissed by clicking away from it.

**A layer is only reachable where its ancestors are.** `widget.at` rejects a node whose bounds
exclude the point before it looks at any child, so a popup placed past the edge of the group that
holds it is painted and not clickable. An overlay therefore belongs in the **root's** layer list,
positioned from its anchor's laid bounds -- which is where every toolkit puts popups, for this
reason rather than by convention.

### `layered_of`

```
pub fn layered_of(layers: [Layer]) -> Widget
```

### `keyboard_children`

```
pub fn keyboard_children(l: Laid) -> [Laid]
```

Which of a node's children the **keyboard** may reach: every child, except at a layered node
whose topmost key-taking layer shuts the rest out.

**One function, used by both `focus.controls` and `a11y.tree_of`.** The rule could as easily have
been written into each of those walks, and then a dialog that trapped Tab correctly could still
have been described to a screen reader as though the window behind it were available. A modal
that is modal for one of them and not the other is worse than one that is modal for neither.

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

### `Cursor`

```
pub enum Cursor {
    // The ordinary pointer, and the one anything that does not say otherwise gets.
    Arrow
    // Over something that resizes left and right: a column boundary, an upright divider.
    ResizeAcross
    // Over something that resizes up and down: a row boundary, a flat divider.
    ResizeDown
    // Over text that can be selected.
    Text
    // Over something that will be followed.
    Hand
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
What the pointer should look like over a given point.

**Vocabulary, not a platform call** -- the same split `Role` makes. A widget set knows that the
thing under the pointer is a column boundary; only the backend knows what an X11 cursor font
calls the shape for that, and a window on another platform would answer differently. So this
names the meanings and `desktop.set_cursor` does the mapping, exactly as `atspi.code_of` does
for a role.

### `Role`

```
pub enum Role {
    Button
    CheckBox
    RadioButton
    TextField
    // A field whose contents must not be shown or read out. **A role rather than a flag on
    // `TextField`**, because every layer below has to treat it differently -- a reader announces
    // it so a user knows not to say what they type aloud, and the platform bridge uses the role to
    // decide that this node's text is not for speaking.
    Password
    Slider
    Link
    List
    ListItem
    // A tree and its rows. `State.expanded` has existed since accessibility was built and nothing
    // declared these two, which is the same gap `Tab` and `TabList` were in before F280.
    Tree
    TreeItem
    Tab
    TabList
    Menu
    MenuItem
    // **A value picker, which is not a menu.** A menu offers commands and a combo box offers
    // values, and a reader says different things about them: "File, menu" invites you to look for
    // an action, "Environment, combo box, staging" tells you what is currently chosen. The two are
    // built from the same popup here and announce differently on purpose.
    ComboBox
    ScrollBar
    // A grid of cells with a header over it. **Its rows are nodes and its cells are not**, which is
    // where this stops short of AT-SPI's table interface: that interface answers questions by
    // coordinate ("what is in row 3, column 2"), which needs a C object implementing `AtkTable`
    // rather than a role on a widget. A row whose label is its cells read in order gets a reader
    // through a table usefully today; the coordinate interface is the next instalment, not a
    // different design.
    Table
    // One row of a table, announced with its cells in order.
    TableRow
    // The heading over a column, which is a control: it is what a column is sorted and resized by.
    ColumnHeader
    // A switch is not a check box, and the distinction is the user's rather than the drawing's: a
    // check box is a choice that is committed with the rest of a form, and a switch takes effect
    // the moment it moves. Readers say "on" and "off" for one and "checked" for the other, which
    // is the whole reason this is its own role rather than a check box that looks different.
    Switch
    // A button that stays pressed: the bold in a toolbar, or one segment of a view switch.
    //
    // **Its own role although ATK cannot tell it from a `Switch`** -- both answer 28, which is
    // ATK's toggle button, and `desktop/atspi.zm` says why a switch borrows that number. ARIA does
    // distinguish them (`button[aria-pressed]` against `switch`), so the vocabulary here is the
    // wider of the two and the adapter narrows it. Narrowing in the adapter is recoverable; losing
    // the distinction in the tree is not.
    ToggleButton
    // **A bar a user cannot operate**, which is the thing that makes it unlike every other role
    // here: there is nothing to press, so it is not a tab stop -- and it still has to be in the
    // tree, because "is it still going?" is exactly what a reader has to be able to answer.
    ProgressBar
    // A field with steppers. It is an editable text field as well -- which is why the role exists
    // rather than the control borrowing `TextField`: a reader announces a range and a step for a
    // spin button and nothing of the kind for a field, and the range is the thing a user most
    // needs told before typing into it.
    SpinButton
    // A strip of commands, which is **one** stop in the tab order with its own arrow keys --
    // the reason it is a role and not a row of buttons. Every platform's reader announces a
    // toolbar so that a user knows the arrows will work here and Tab will leave.
    ToolBar
    // A field holding a date, and the month grid that writes one into it. **Two roles, not
    // one**, because they are two things a reader meets separately: the field is where the
    // value is and the calendar is a popup that opens over the window, and a user tabbing
    // through a form hears the first without ever meeting the second.
    DateEditor
    Calendar
    // Somewhere the application says something the user did not go and look for: a result count
    // that changed under their typing, a panel that finished loading, a save that failed.
    //
    // **The politeness is the role, which is ARIA's own answer.** `role="status"` *is* polite and
    // `role="alert"` *is* assertive, so a separate field would be a second way to say a thing the
    // role already says -- and a node whose role and politeness disagreed would have no meaning.
    //
    // **A reader is a cursor**, and everything else here is read when the user arrives at it.
    // These two are the only way for an application to say something without the user having gone
    // anywhere. See `a11y.announcements`.
    Status
    Alert
    // Two panes and the divider between them. **The divider is a `Separator` inside this**, which
    // is how both ATK and GTK have it: the pane is what contains the two halves and publishes
    // where the split sits, and the handle is the thin thing you take hold of.
    SplitPane
    // A thin line that divides, with nothing either side of it. A split pane's divider and a
    // toolbar's rule are the same object to a reader, which is why this is one role and not two.
    Separator
    Dialog
    // A container that groups its children for a reader without being anything itself -- a toolbar,
    // a form section. Worth a node where a plain layout box is not, because a reader navigates by
    // group.
    Group
    // Static text, which is not a control. Produced by `a11y` for a `Text` outside any control, and
    // not declarable here: a label is text, and text already knows it is text.
    Label
    // A picture, which is not text. Produced by `a11y` for a `Picture`, and not declarable here
    // for the same reason: a picture knows what it is, and what a reader needs from it is the
    // alternative text it carries.
    Image
    // The window itself, which `a11y` puts at the root.
    Window
    Unknown
}
```

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
    // Which **part** of this control the focus ring is drawn around, or `""` for the control
    // itself.
    //
    // **A tab strip is why.** One control takes the focus and Left and Right move along it, so
    // the ring went around the whole strip -- which says every tab is focused, when one of them
    // is. ARIA's tab pattern puts the focus on the active tab for the same reason, and a reader
    // already announces the selected one; this is the pointer's half of the same fact.
    //
    // **Opt-in rather than "ring whatever part is selected"**, which would have cost nothing to
    // write and would have been wrong for the two controls that have selected parts and do not
    // want this: a list and a tree are focused *as a whole* and show their selection by
    // highlighting a row, which is what every platform does. A tab strip is the exception, so it
    // is the one that says so.
    //
    // An unknown name rings the control, which is the same forgiving rule `paint_focused` follows
    // for a name the tree no longer holds.
    focus_part: string
    // Whether **Tab** can land here, as opposed to whether this is a control at all.
    //
    // **The two are not the same thing, and a list is what proves it.** A list of two hundred rows
    // is one stop in the tab order, and the arrow keys move within it -- but every row still has to
    // be a node a screen reader can reach and announce as selected or not, and a click still has to
    // report the row it landed on rather than the list. So a row is a control for hit testing and
    // for accessibility, and not for Tab.
    //
    // This was not in the first version of this struct. Building the list found it: without the
    // distinction a row is either a tab stop (so Tab walks two hundred of them) or not a control at
    // all (so `a11y` takes every row's text as the list's own name, and a reader announces the whole
    // list as one string -- which `tests/stdlib/a11y_test.zm` already had a case against).
    tab_stop: bool
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

### `part_of`

```
pub fn part_of(name: string, role: Role, label: string, state: State, child: Widget) -> Widget
```

A control a click and a screen reader can reach but **Tab cannot** -- a row of a list, a cell of a
table. Its container is the tab stop and its own keys are the arrows.

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
    // A path scaled into its box: a mark rather than a picture. See `Drawn`.
    Drawing(Drawn)
    Canvas(Painted)
    // A decoded image drawn into its box: a picture rather than a mark. See `Pictured`.
    Picture(Pictured)
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
    // A grid of tracks with cells placed into them: the one arrangement nesting cannot express,
    // because nesting cannot align across cells in *different rows*. See `Grid`.
    Grid(Grid)
    // Children laid along a line that wraps when it runs out. See `Flowed`.
    Flow(Flowed)
    Scroll(Viewport)
    // Children stacked over one another, later ones on top -- see `Layer`. A menu's popup and a
    // modal dialog are both this.
    Layered(Stacked)
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

### `FaceMetrics`

```
pub struct FaceMetrics {
    text_width: fn(string, float) -> float
    line_height: fn(float) -> float
    // How far below the top of a line its baseline sits.
    //
    // **Added because a baseline cannot be guessed from a line height.** Every text op this layer
    // emitted put its baseline a *whole line* below the top of its box -- which is where the next
    // line begins, not where this one sits -- so every glyph in the widget set was drawn a
    // descent too low, hanging out of the bottom of the box that was meant to centre it. A
    // button's label sat low in its face and a check box's mark low in its square, and no
    // assertion could see it: the boxes were right and only the ink was wrong. Found by someone
    // looking at the gallery.
    ascent: fn(float) -> float
}
```

How wide a string is, and how tall a line of it is, at a given em size, **for one face**.

Two functions rather than a font, so that nothing here can fail and nothing here needs a face.
`font.advance` answers a `Result` -- a standard font that is not Courier cannot be measured
without the file -- and threading that failure through every layout would make a window's shape
a fallible computation. A caller resolves it once, when it loads its font.

### `Metrics`

```
pub struct Metrics {
    faces: [FaceMetrics]
}
```

The faces a window can draw text in.

**An array, because `raster.Options.fonts` is one and `scene.text_of` names a face by index.**
This layer passed a literal `0` to every text op until 2026-11-14, so a `Label` could not name a
second face however many the renderer was given -- which blocked a bold heading and an *icon
font* alike, and the icon font is why this changed.

Index 0 is the text face: what a control draws in unless its style says otherwise. A tree of
widgets naming a face beyond this array still **lays out**, against face 0, because a window's
shape must not depend on which fonts were loaded; the renderer is where the mismatch is reported
("a text run names font 7 but Options.fonts holds 1"), the same way `scene` names an image by
index and `raster` answers for it.

### `metrics_of`

```
pub fn metrics_of(text_width: fn(string, float) -> float, line_height: fn(float) -> float) -> Metrics
```

The measurements of one face, which is what most windows have.

**The ascent is guessed here** -- see `face_metrics_of`, which says what the guess is and when
it is wrong. A caller with a real font should take its metrics from `metrics_for`, which asks
the file.

```
pub fn metrics_of(
    text_width: fn(string, float) -> float,
    line_height: fn(float) -> float,
    ascent: fn(float) -> float
) -> Metrics
```

One face whose three numbers are all given.

### `metrics_of_faces`

```
pub fn metrics_of_faces(faces: [FaceMetrics]) -> Metrics
```

Several faces, in the order `raster.Options.fonts` holds them.

### `face_metrics_of`

```
pub fn face_metrics_of(
    text_width: fn(string, float) -> float,
    line_height: fn(float) -> float
) -> FaceMetrics
```

A face whose ascent is **guessed** at four fifths of its line height.

**A guess, and named as one.** This constructor is for callers with no font -- a test, a layout
computed before any file is read -- and they have no ascent to give. Four fifths is close for
the faces a desktop ships and wrong for one with unusual line spacing, so a caller holding a
real font should use `face_metrics_for`, which asks it, and one that knows its own number
should say so with the three-argument form below.

```
pub fn face_metrics_of(
    text_width: fn(string, float) -> float,
    line_height: fn(float) -> float,
    ascent: fn(float) -> float
) -> FaceMetrics
```

A face whose three numbers are all given.

### `face_at`

```
pub fn face_at(m: Metrics, which: int) -> FaceMetrics
```

The face at `which`, or face 0 when the tree names one this window was not given.

**A layout that refused would make a window's shape depend on its fonts**, which is the one
thing this whole layer is arranged to avoid: `present` takes pixels and knows nothing about
where they came from, and a tree measured against a missing face would be a different tree.

### `width_in`

```
pub fn width_in(m: Metrics, which: int, text: string, size: float) -> float
```

How wide `text` is in the face a label names, at its own size.

### `elided`

```
pub fn elided(text: string, room: float, which: int, size: float, m: Metrics) -> string
```

`text`, cut to fit `room` with an ellipsis, or whole when it already fits.

**Measured with the function the layout will use**, so the answer is the renderer's and not an
estimate of it: `width_in` against the same metrics the widget tree is laid out with.

**Here because a label cannot wrap in a row.** `wrapping_label` takes its width from above, and
a row cannot give one until every child is measured -- so a long caption beside a control has to
be shortened rather than folded. Two applications wrote this loop before it was moved here, and
the second one writing it is what moved it.

**Cut by characters, not by words.** A word boundary would read better and cannot be found
without knowing the script: `len` here is characters and `substring` respects them, but "where
may a line break" is a Unicode question this module has no answer for. A caller with a title
that must break well should wrap it instead.

### `height_in`

```
pub fn height_in(m: Metrics, which: int, size: float) -> float
```

How tall a line is in the face a label names.

### `ascent_in`

```
pub fn ascent_in(m: Metrics, which: int, size: float) -> float
```

How far below the top of a line its baseline sits, in the face a label names.

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

### `metrics_for_faces`

```
pub fn metrics_for_faces(fs: [font.FontRef]) -> Result<Metrics, string>
```

The real metrics of several faces, in the order `raster.Options.fonts` will hold them.

**The order is the contract.** A window passes the same list to both, and index 0 is the text
face; `controls.Style` names the rest by purpose (`text_font`, `icon_font`) so that no control
writes a literal index.

### `face_metrics_for`

```
pub fn face_metrics_for(f: font.FontRef) -> Result<FaceMetrics, string>
```

### `Measured`

```
pub struct Measured {
    size: Size
    // How far below this widget's own top its first baseline sits.
    baseline: Option<float>
}
```

What a widget wants, given nothing: its *natural* size, and where its first line of text sits.

**The baseline is an `Option` and `None` is the common answer**, not a failure. Most widgets have
no text to sit on a line -- a mark, a picture, a spacer -- and a cell asking to be aligned on a
baseline in a row where nothing has one falls back to its top. A sentinel like `0.0` would be
indistinguishable from a real baseline at the very top of a box.

### `measure`

```
pub fn measure(w: Widget, m: Metrics) -> Size
```

The size a widget wants, given nothing: its *natural* size, before any space is shared out.

A `Flex` measures zero, which is what makes it flexible -- it has no natural size, so everything
it is given is leftover space.

### `measured`

```
pub fn measured(w: Widget, m: Metrics) -> Measured
```

The same question, with the baseline a grid needs to align a row on.

**One recursion rather than two.** A separate `baseline_of` walking the tree a second time is a
second set of rules about what a column's first line is, free to disagree with this one -- and
the disagreement would show only in a form, where it matters most.

### `unconstrained`

```
pub fn unconstrained() -> float
```

**No width is known**, which is a real answer and not a missing one: it is what a row asks its
children, and what the top of a tree starts from. A wrapping label answers it with its one-line
width, which is the widest it would ever like to be.
**A function and not a `pub val`**, because a module's global val cannot be reached as
`widget.unconstrained` from outside -- a caller would have to `use widget.unconstrained` to
name the one value this API's own signature asks them for.

### `measured_in`

```
pub fn measured_in(w: Widget, m: Metrics, within: float) -> Measured
```

The same question, told how much width there will be.

**This is the whole of what a wrapping label needed, and why there was none until now.** The
height of wrapped text is a function of the width, and `measured` was never given one -- so a
paragraph could not say how tall it would be and nothing could wrap. `within` is that width:
the room this widget will be laid into, or `unconstrained`.

**A column passes its width down and a row does not**, which is a limit worth stating rather
than hiding. A column divides height and hands every child the same width, so it knows what to
say. A row divides *width*, and no child's share is known until all of them have been measured
-- so asking a child "how tall are you at this width" before deciding the width is the circle
CSS breaks with min-content and max-content passes. This layer does not have those, and
inventing them for one widget would be the wrong place to start. A paragraph in a row measures
one line; wrap it in a `wide_of` to say what it gets.

## The grid

### `Track`

```
pub enum Track {
    // Exactly this many points.
    Points(float)
    // As big as the largest thing in it needs.
    Natural
    // This many shares of whatever is left over. **The same notion as `Flex`**, so the library has
    // one idea of weighted leftover rather than two.
    Share(float)
}
```

How wide a column is, or how tall a row.

**Declared, which is the whole difference from `GridBagLayout`.** There a column's weight is the
invisible *maximum* of the `weightx` its cells happen to carry, so two cells can disagree about
a column and the resolution is a rule nobody remembers. Here a track is one thing that says one
number, and a cell cannot contradict it.

### `Fill`

```
pub enum Fill {
    Nowhere
    Across
    Down
    Both
}
```

Whether a cell's child grows into the room its track gives it.

**Separate from `Anchor`, which is `GridBagLayout`'s good idea**: "do you grow?" and "where do
you sit if you do not?" are different questions, and systems that conflate them make the second
unanswerable.

### `Sideways`

```
pub enum Sideways {
    Leading
    Centre
    Trailing
}
```

Where a cell's child sits in its track when it does not fill it.

**Leading and trailing, not left and right.** This library is heading for Arabic and Hebrew, and
`GridBagLayout` got here first -- `LINE_START` and `LINE_END` were added to Swing in 1.4 for
exactly this. They resolve as left and right until a text direction exists, which is the point:
the vocabulary is free now and a migration later.

### `Upright`

```
pub enum Upright {
    Top
    Middle
    Bottom
    // On the row's shared text baseline. **Falls back to `Top` when nothing in the row has one**,
    // which is why `Measured.baseline` is an `Option`: a row of icons has no line to sit on.
    Baseline
}
```

### `Anchor`

```
pub struct Anchor {
    sideways: Sideways
    upright: Upright
}
```

### `anchor_of`

```
pub fn anchor_of(sideways: Sideways, upright: Upright) -> Anchor
```

### `Column`

```
pub enum Column {
    Next
    At(int)
}
```

Which column a cell starts in.

**`Next` is `GridBagLayout`'s `RELATIVE` without the magic number**, which was `-1`. A form is
built row by row and counting columns by hand is how a cell ends up in the wrong one after
somebody inserts a label.

### `Span`

```
pub enum Span {
    Rest
    Cells(int)
}
```

How many columns a cell covers.

**`Rest` is `REMAINDER` without the magic number**, which was `0` -- a zero that meant "all of
them", the opposite of what it reads like.

### `Cell`

```
pub struct Cell {
    column: Column
    row: int
    across: Span
    down: int
    fill: Fill
    anchor: Anchor
    // Room around this child, inside its track. `column_gap` and `row_gap` are the gutters; this
    // is for the one cell that wants more.
    margin: Insets
    child: Widget
}
```

### `cell_of`

```
pub fn cell_of(row: int, child: Widget) -> Cell
```

A cell with nothing said about it: the next free column of its row, one track, no growth,
leading and on the baseline.

**`Baseline` by default**, because a form is the reason this exists and a form is text beside
text. A cell whose child has no baseline is placed at the top, so the default costs nothing
where it does not apply.

### `Grid`

```
pub struct Grid {
    columns: [Track]
    rows: [Track]
    column_gap: float
    row_gap: float
    cells: [Cell]
    name: string
}
```

A grid: tracks declared once, cells placed into them.

**The track lists may be shorter than the grid is**, and an undeclared track is `Natural`. That
keeps `GridBagLayout`'s convenience for a quick form -- declare nothing and everything sizes to
its content -- while giving a weight exactly one place to live when one is wanted.

### `cell_of`

```
pub fn cell_of(row: int, fill: Fill, anchor: Anchor, child: Widget) -> Cell
```

A cell with everything said in one call.

**For a design file**, which may hold only calls and literals -- no `with`, no local `val`. A
form's shape is exactly the thing a designer should be able to state, so stating it must not
need an expression the dialect forbids.

### `cell_at`

```
pub fn cell_at(row: int, column: int, fill: Fill, anchor: Anchor, child: Widget) -> Cell
```

The same, in a column the caller names rather than the next free one.

### `grid_of`

```
pub fn grid_of(
    columns: [Track],
    rows: [Track],
    column_gap: float,
    row_gap: float,
    cells: [Cell]
) -> Widget
```

Gutters that differ by axis: a form usually wants more room between its columns than its rows.

```
pub fn grid_of(columns: [Track], rows: [Track], gap: float, cells: [Cell]) -> Widget
```

## The flow

### `Flowed`

```
pub struct Flowed {
    direction: Direction
    // Between children on one line.
    gap: float
    // Between lines. **Separate from `gap`**, because they are different distances: items in a
    // row sit closer together than rows do, and one number for both makes a bar of chips look
    // either cramped or ventilated.
    line_gap: float
    // Where a line that does not fill its width sits. **Leading, centre or trailing and not left
    // or right** -- `FlowLayout`'s own constants went the same way in Java 1.4, for the Arabic and
    // Hebrew reason this library is heading towards.
    align: Sideways
    // Where a child shorter than its line sits within it.
    across: Upright
    children: [Widget]
    name: string
}
```

Children laid along a line, wrapping to the next when the line runs out.

**The one thing a `Stack` cannot do.** A row runs off the edge of its parent and nothing
reflows; this breaks. That is what a bar of chips, a list of tags and a toolbar that would
rather wrap than overflow all need, and it is why `FlowLayout` outlived most of Swing.

**`Direction.Row` flows across and wraps downward**, `Direction.Column` flows down and wraps
sideways -- the same axis the direction names everywhere else here.

### `flow_of`

```
pub fn flow_of(gap: float, line_gap: float, children: [Widget]) -> Widget
```

## The border

### `border_of`

```
pub fn border_of(
    top: Widget,
    bottom: Widget,
    leading: Widget,
    trailing: Widget,
    centre: Widget,
    gap: float
) -> Widget
```

Swing's `BorderLayout`, as a function rather than a layout.

**It is an arrangement and not a capability**, which is why it is twelve lines of existing
pieces: a column of top, middle and bottom, with the middle a row of leading, centre and
trailing. A new `Widget` variant would buy nothing that nesting does not already do.

**An absent region costs no gap**, which is the one detail that makes this worth writing down
rather than leaving to each caller. `column_of(gap, [top, middle, bottom])` puts a gap either
side of the middle whether or not `top` and `bottom` are there, so a border with no header is
a border with a mysterious band of space at the top. `Nothing` measures zero but a gap does
not, and that is exactly the kind of arithmetic every caller would get wrong once.

**Leading and trailing, not west and east.** Swing added `LINE_START` and `LINE_END` in 1.4 for
the same reason, and the vertical pair keep their plain names because a page's top does not
mirror in Arabic or Hebrew.

### `is_nothing_widget`

```
pub fn is_nothing_widget(w: Widget) -> bool
```

**An `Expand` holding nothing is still nothing**, which is the case `border_of` creates for
itself: a missing centre is wrapped before it is filtered.
Whether a widget would draw and measure as nothing at all.

**Public because a panel has to tell "no content" from "content that is empty"** -- an empty
answer is an answer, and a panel showing "No medications recorded" when it was handed nothing
at all would be inventing one.

## Layout

### `Laid`

```
pub struct Laid {
    bounds: Rect
    // Which child of its parent this is, and -1 for the root. Joined with its ancestors' it is
    // the position in the tree -- "0", "0/1", "0/1/2" -- which is what an accessibility tree keys
    // a node by when no `name` was given.
    //
    // **An int, where this used to be that whole path as a string.** `a11y` is the only reader
    // there has ever been, and the string was *built for every node of every layout*: 9.7% of a
    // layout pass went to `join_path` and its `concat`, besides the garbage two allocations per
    // node make. `a11y` joins the indices as it descends instead, which reproduces the same paths
    // exactly -- every walk it does descends through real children, so the indices it meets are
    // the ones layout assigned.
    index: int
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

### `node_named`

```
pub fn node_named(l: Laid, name: string) -> Option<Laid>
```

The outermost node carrying `name`, or `None`.

**Not `focus.node_of`, which only ever looks at controls.** A name belongs to any widget that
has a slot for one -- a group, a viewport, a label -- and the two most recent callers wanted
exactly those: the caret's box, which `controls` names so that an input method can be told
where it is, and a canvas, which a builder names so that a press on it can be recognised.
Asking `focus` for either answers `None`, and an empty rectangle is a hit test that silently
misses.

A name is inherited by every child that has none of its own, so the node wanted is the
outermost one carrying it, which is the first a pre-order walk meets.

### `measured_as_nothing`

```
pub fn measured_as_nothing(l: Laid) -> [Laid]
```

Every node `at` passed through on its way down, outermost first, ending at the one it would
have returned. Empty when the point is outside the tree.

**`at` answers "what is here", and that is not the only question.** Routing an input wants the
innermost thing, which is what `at` gives. A tool that has placed some of this tree and not the
rest -- a builder drawing a screen from a file, a recogniser mapping a click back to source --
wants the innermost thing *it* placed, and a list row or a tab is not that: those are made by
`controls`, below the level anyone else named. Without the chain the only way back up is to
take the name apart on a "/", which is a private convention of `controls` and not a promise.

It walks the same way `at` does rather than being built on it: later siblings first, and the
first child whose bounds contain the point wins, so the two can never disagree about which
branch they took.
Every node that paints something and was laid out as nothing.

**The trap this exists for has cost eight victims and every one of them looked identical:
nothing drawn, no error, all tests green.** The table's header band, the link, the slider's
thumb, the star, the popup's rows, the slider's track, the check mark and the look-and-feel
band. A `Fixed` of zero on an axis, a `Scroll`, or a `Drawing` measures zero and *stays* zero
unless its parent stretches it -- `Across.Centre` does not, `stretched()` does -- so the widget
is there, laid out, hit-testable at nothing, and invisible.

**It is checked after layout because it cannot be seen before.** A zero measurement is correct
and ordinary: half the controls in this library are `fixed_of(width, 0.0, ..)` inside a stack
that stretches them, which is exactly the right spelling. What is wrong is a node that still has
no extent *when the layout is finished* and yet has ink to put on the window. No lint can see
that, because it depends on what the parent did.

**Only nodes that paint**, which is the line between a bug and a container: a `Group` with a
fill, or a `Drawing`. A stack with no surface and no size is a grouping, and grouping nothing is
not an error.

Intended as an assertion in an application's own tests -- the gallery holds all of its demos to
it -- rather than as something to call per frame.

### `chain_at`

```
pub fn chain_at(l: Laid, p: Point) -> [Laid]
```

### `contains`

```
pub fn contains(r: Rect, p: Point) -> bool
```

**Half-open on the right and bottom**, so two boxes that share an edge do not both contain the
point on it -- a click on the boundary between two list rows must select one of them, and which
one it is should not depend on which was searched first.

### `role_of`

```
pub fn role_of(l: Laid) -> Role
```

What a laid node declares itself to be, or `Unknown` for anything that is not a control.

**The same table `a11y` reads**, which is the point: `controls.cursor_at` decides the pointer's
shape from it, so a control that declares its role properly gets a cursor for nothing and one
that does not is visibly ordinary. The pointer and the screen reader should not be able to
disagree about what a thing is.

### `text_of`

```
pub fn text_of(l: Laid) -> string
```

The text this node draws, or "" when it draws none.

**So that a tooltip can decline to repeat what is already on screen.** A tip under an ordinary
labelled button, saying the label the user is reading, is noise with a delay on it -- and the
only way to know is to ask whether the words are drawn anywhere under the control.

### `tip_in`

```
pub fn tip_in(l: Laid) -> string
```

The words a tooltip shows for a control: **its description, or its label when it has none.**

**Derived and not a field of its own, which is what makes a tooltip safe.** Every platform's
tooltip advice is the same warning -- a tip that says something other than the accessible name is
a control that reads one way to a sighted user and another to a reader, and the user who needs
the name most gets the one nobody proofread. Taking the words from what the control already
declares makes that disagreement unrepresentable: there is one string, and a tip is a second way
of showing it rather than a second copy of it.

It is also why adding a `tip` to twenty control structs was not the design. An icon button is the
control that needs a tooltip, because its label is the thing a tooltip exists to reveal -- and it
already declares one.

### `is_tab_stop`

```
pub fn is_tab_stop(l: Laid) -> bool
```

Whether a laid node is a control **Tab can land on**. See `Focusable.tab_stop`: a list row is a
control and not a tab stop.

### `is_control`

```
pub fn is_control(l: Laid) -> bool
```

Whether a laid node is a control: its own widget takes focus.

### `caret_name`

```
pub val caret_name = "caret"
```

The name the caret's box carries in the laid-out tree.

**Here rather than in `controls`, where it lived until 2026-11-15**, because the painter has to
recognise it: a caret is drawn only for the control that has the focus, and `collect` is what
decides. A marker the painter must know belongs to the painter's layer. `controls.caret_box`
still puts it there and `desktop.caret_on_screen` still reads it back; both now name it from
here.

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

**No focus, so no caret** (2026-11-15). A text field's caret marks the insertion point of the
field being typed into, and this entry point is not told which that is -- so it draws none, the
same answer it gives for the focus ring. An application that wants either calls `paint_focused`.
Before that rule existed every field drew one at once, which is what a window full of text
fields each carrying a blinking-looking bar turns out to be: `curly gui` showed eight carets in
one frame with the focus on a tree.

### `paint_focused`

```
pub fn paint_focused(l: Laid, m: Metrics, focused: string, ring: Color, width: float) -> scene.Scene
```

### `paint_ops`

```
pub fn paint_ops(l: Laid, m: Metrics) -> [Op]
```

The ops alone, for a caller composing a scene of its own. **No focus, so no caret**, as `paint`.

### `box_path`

```
pub fn box_path(c: Corners, bounds: Rect, radius: float) -> shape.Path
```

A surface's outline: the box, rounded where this look rounds it.

**One function, because the fill and the border have to be the same shape.** They are two fills
in `paint_surface` and a border drawn around a different outline than its fill is a hairline of
background showing through one edge -- the kind of thing that looks like an anti-aliasing bug
and is not.
