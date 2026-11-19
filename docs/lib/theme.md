# `theme`

The looks the control set ships with, and the register of them.

**A look and feel is not a colour theme**, which is the whole reason this is a module and not a
palette. A `controls.Style` carries six kinds of decision at once: colour (ink, faces, accent,
selection), *space* (padding and the spacing step), *type* (three sizes and which face each
draws in), *shape* (one corner radius and whether a face is edged), *depth* (what casts a shadow
and how deep), and *state* (how a hover and a press read). A look that changes only the colours
is a recolour; every system that publishes a design language -- Fluent, Material, Adwaita --
changes all six together, and a file here is expected to as well.

**Why this is not in `controls`.** It was, as `default_style` and `modern_style` at the foot of
a 5,000-line file, which made the control set the home of both the vocabulary and every word
spoken in it. They are different things with different readers: someone adding a control reads
`controls`, someone dressing an application reads this. The dependency only points one way --
`controls` defines `Style` and never builds one -- so splitting cost nothing and the circularity
that would have forced the opposite split does not exist.

**One file per look, and a file holds every variation of its own look.** `plain.zm` has
`plain` and `plain_dark`; `modern.zm` has `modern` and `modern_dark`. A variation is the same
design decisions answered for a different surround, so it belongs beside the thing it varies --
and splitting light from dark across two files is how the two drift until only one of them is
really maintained.

**Parts of one module.** Every file here is `use theme`, exactly as `lib/core/charset` is
one module in two files: a caller says `theme.plain()` and `theme.modern_dark()` without
knowing which file either lives in. Function names therefore carry their look's name, which they
would want to anyway.

**Adding one** takes a file here, its name in `THEME_PARTS` and `src/corelib.c`, and an
entry in `all` below -- the last being what makes it show up in an application's theme list and
in `theme_test`'s contrast gate, which every shipped look passes.

**One token has a caller-side obligation**: `Style.window` is the colour behind everything, and
no control draws it. An application passes it to `raster.Options.background`. A look and feel
that could not say what colour the window was is why dark variations could not be written here
before.

`use theme` -- or `use theme.Name` to bring one item in bare.

## `theme.zm`

### `Scheme`

```
pub enum Scheme {
    Light
    Dark
}
```

A look and feel under the name an application offers it by.

**A name and a style, not a constructor**, because the list is what a theme picker wants and a
list of functions is not one -- and because `named` would otherwise be a switch that every new
look has to be added to twice.
Which end of a ramp a look is read from.

**A parameter rather than a second authored look** (D8). `plain_dark` was written by hand and had
to discover four things an inversion gets wrong -- faces lighter than the window, a white hover
veil, `on_accent` flipping, a deeper shadow. Every one of them is what a step *means* on a ramp
read from the other end, so indexing is the whole of dark mode and there is nothing left to get
wrong. Material's `fromSeed` takes a brightness for the same reason, and Radix ships each scale
both ways.

### `Toward`

```
pub enum Toward {
    // A face is darker than the window it sits on: window 1, panel 2, face 3.
    Ink
    // A face is lighter than the window: panel 1, face 2, window 3.
    Away
}
```

Which way a surface moves from the window it sits on.

**This is the one thing a 12-step ramp could not say, and it was measured rather than guessed.**
Converting the four looks that were authored by hand to OKLab lightness shows both light looks
using monotone steps 1 to 5 and disagreeing only about which role takes which step:


```
plain light    window .986   panel .970   face .923   hovered ~.87   pressed .808
modern light   panel 1.000   face .991    window .976  hovered ~.96   pressed .948
```

So "white cards on a grey canvas" -- modern's defining choice, and what every elevation model
since Material has meant -- is exactly "the window is step 3, not step 1". A permutation of three
roles, not a second curve.

**Read only in a light scheme.** Both dark looks measure monotone *ascending* from the window
(plain .239/.302/.372/.458, modern .209/.246/.280/.339), because on an inverted ramp every step
away from 1 is already lighter: "a face is lighter than its window" is what step 3 means there.
There is nothing to choose, which is D8's claim about dark falling out mechanically, checked
against the two looks that were written by hand before any of this existed.

### `ramp`

```
pub fn ramp(seed: Color, s: Scheme) -> [Color]
```

Twelve steps from one seed, with Radix's roles: 1 is the app background, 3 an element, 4 hovered,
5 selected, 7 a border, 9 a solid, 11 and 12 text.

**The step number is the role**, which is the whole point of a scale and what makes D5's rows
stop being independent choices: 4 *is* hovered and 5 *is* active by construction, so a dark
variation has nothing to get wrong about them.

The seed's hue is kept exactly; its chroma is a scale factor, not a target. Gamut is `paint`'s
business -- it maps by reducing chroma rather than clipping channels, which F292 established
after clipping moved `lighten(blue, 0.100)` by 0.060 and a test caught it.

### `accent_ramp`

```
pub fn accent_ramp(seed: Color, s: Scheme) -> [Color]
```

The accent ramp of a seed: the same twelve roles on the curve a solid colour needs.

### `step_hovered`

```
pub val step_hovered = 4
```

The step a role takes, named once so that no look and no test writes a bare index.

**Named because a bare `n[4]` is a number twice**: once for the step and once for the meaning,
and the second one is the one that rots. These are §11.2's table, and the three surfaces that
move with `Toward` are not here -- they are `derive`'s business, because they are the one thing
a look chooses.

### `step_pressed`

```
pub val step_pressed = 5
```

### `step_border`

```
pub val step_border = 7
```

### `step_solid`

```
pub val step_solid = 9
```

### `step_ink`

```
pub val step_ink = 12
```

### `Fill`

```
pub struct Fill {
    from: int
    to: int
    // Degrees clockwise from the top, which is CSS's convention and the one a designer will say
    // out loud. Ignored when `from == to`.
    angle: float
}
```

A face, as a look writes it: a span of this look's own ramp.

**Steps, not colours** (D10). A look that wanted a gradient would otherwise have to state two
colours that the ramp already contains, and the gate would have to go looking for them; naming
steps keeps the promise that **every stop in a built style is provably a ramp step**, so the
ramp can be checked once instead of the stops being extracted per look.

**`from == to` is a flat fill**, which is why there is no `Option` here and no second field:
solid and gradient are one form, and every shipped look happens to write the flat one.

### `flat`

```
pub fn flat(step: int) -> Fill
```

A flat fill of one step: what both shipped looks write.

### `paint_of`

```
pub fn paint_of(f: Fill, steps: [Color]) -> Paint
```

The paint a `Fill` names, against a ramp.

**Endpoints as fractions of the box**, resolved later by `widget.paint_in` against the control
actually being painted -- a widget tree is built before anything knows how big a control is, so
a gradient cannot carry absolute points. See `widget.radius_in`, the precedent this follows.

### `Seeds`

```
pub struct Seeds {
    neutral: Color
    accent: Color
    surfaces: Toward
    // Whether a face wears its edge.
    //
    // **A ramp always has a border colour -- step 7 is "element border" by role -- but whether a
    // look draws it is not something two colour seeds imply.** It is the same kind of choice as
    // `Toward` and as the shape scale: `plain` is square, filled and unedged on purpose, so that
    // an application replacing it has nothing to undo, and `modern` is edged because on a white
    // window the border is what says "this is a control".
    edged: bool
}
```

The two colours a look is grown from, and which way its surfaces go.

**Two seeds, not twenty colours** (D8). What a look used to state -- a face, a pressed face, a
selection, a disabled ink, a veil, a border, an accent -- are steps of two ramps, so the only
things left to choose are the hue of the neutrals, the hue of the accent, and whether a face sits
above its window or below it.

### `derive`

```
pub fn derive(seeds: Seeds, s: Scheme) -> controls.Style
```

A whole look, from two seeds and a scheme.

**The override list is the measurement** (D8). `plain()` and `modern()` are this plus their
deliberate exceptions, and if either needed twenty the derivation would be wrong -- which we
would then know rather than argue about. The exceptions that remain are not colours: they are
the padding, the shape scale and the shadow, which are a look's own vocabulary and not something
two colour seeds could imply.

**Derivation happens here, once, when the look is built** -- never at paint time (D2).

### `Look`

```
pub struct Look {
    // What to put in a menu. Capitalised as a title, because it is one. **Computed** for a dark
    // row -- "Plain" and "Plain dark" are one family's two schemes, not two names someone typed.
    name: string
    style: controls.Style
    // Which end of the ramp this row was read from.
    //
    // **Read off the register rather than set by hand**, which is what absorbs F7: an application
    // asking "is the look I am showing a dark one" was answering it by matching the name, and a
    // look called "Midnight" would have broken that. The register knows, because it built the row.
    scheme: Scheme
}
```

### `Family`

```
pub struct Family {
    name: string
    light: controls.Style
    dark: controls.Style
}
```

A look's two schemes, which are one look.

**Two seeds give two rows, and that is why the register is computed.** D4 could keep `all()` as
a list because density changes no colour -- a dense look is the same look. A scheme *does* change
colour, so a register of two entries would leave the AA gate testing only the light half: a gate
that quietly stopped testing half the library because a mechanism got better is the worst
outcome available here. Adding a family below adds two rows to the menu and two to the gate,
without anyone remembering to.

### `Density`

```
pub enum Density {
    Compact
    Comfortable
    Spacious
}
```

How much room a look is given, as an axis over it rather than an entry in the register.

**Density is not a look.** `all()` below lists four looks, and crossing them with three
densities would list twelve -- eight of them colour-identical to another, so the contrast gate
would do the same work three times over while a theme menu offered four looks in twelve rows.
An axis also composes for nothing: compact dark modern needs no entry and no name, and nobody
has to remember to add it. See D4 in docs/design/look-and-feel.md.

### `at_density`

```
pub fn at_density(s: controls.Style, d: Density) -> controls.Style
```

The same look with less or more room: the same colours, the same shapes, a different scale.

**The scale moves and the floors do not.** `Style.minimum.target` is WCAG 2.2's 24 points
(SC 2.5.8) and a narrow window is not a reason to hand somebody a smaller target, so `minimum`
is untouched here -- which is the whole reason D6 made it a separate group. `stroke` is
untouched for its own reason: a hairline is a hairline at every density, and a grid line that
thickened in a spacious theme would stop being a grid line and start being a border.

**What moves is `space` and `padding`**, which is where the room in a window actually is.

**Three factors rather than three hand-written scales**, because a scale written out per density
is three places for a step to drift: `xs` could end up larger than `s` in exactly one of them and
nothing would say so.

### `densities`

```
pub fn densities() -> [Density]
```

Every density, in the order an application offers them.

### `all`

```
pub fn all() -> [Look]
```

Every look and feel here, each variation as its own entry.

**The register is a list and not a map** so that the order is the one an application shows: a
map would sort by name and put "Modern dark" before "Plain". `named` searches it; nothing needs
the lookup to be fast, because a theme is chosen by a person.

### `named`

```
pub fn named(name: string) -> Option<controls.Style>
```

The look and feel of that name, or nothing.

**Case-sensitive and exact.** A near-match would be a guess, and an application that gets a
stored preference wrong should show the default rather than something close to what was asked
for.

### `default_style`

```
pub fn default_style() -> controls.Style
```

What an application gets if it expresses no preference: `plain`.

**A function rather than a constant**, so that what the default *is* can change without every
caller changing -- which is the whole reason the name exists beside `plain()`.

### `ink_contrast`

```
pub fn ink_contrast(style: controls.Style) -> float
```

How legible a look's body text is on its own control faces, as a WCAG contrast ratio.

**Here rather than only in the test**, because an application building a style of its own wants
the same question answered, and a number a test knows how to compute is a number the library
should be able to answer. `paint.contrast_ratio` has the formula; AA wants 4.5.

## `plain.zm`

### `plain`

```
pub fn plain() -> controls.Style
```

### `plain_dark`

```
pub fn plain_dark() -> controls.Style
```

The same look on a dark window.

**It used to be a list of overrides that had to discover four things an inversion gets wrong** --
faces lighter than the window, a white hover veil, `on_accent` flipping, a deeper shadow. Every
one of them is what a step *means* on a ramp read from the other end, so the whole of it is now
the second argument. That is D8's claim about dark falling out mechanically, and this file is
what it cost: one word.

## `modern.zm`

### `modern`

```
pub fn modern() -> controls.Style
```

### `modern_dark`

```
pub fn modern_dark() -> controls.Style
```

The same look on a dark window.

**A dark look is not an inversion of a light one**, and this file used to be the proof: eleven
colours written out again, four of which were the things an inversion gets wrong. It is the
scheme now. On white a control is *paler* than its surround and a panel is paler still; on black
the same three steps come out darker than the window and the elevation reads the same way round
-- the further forward a thing is, the lighter it gets, which is Material's elevation overlay
stated as a ramp index.
