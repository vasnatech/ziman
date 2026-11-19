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

### `Seeds`

```
pub struct Seeds {
    face: Color
    ink: Color
    // What a press darkens (or lightens) the face to. Stated rather than derived **because a press
    // is the one state every look already had an exact colour for** and derivation would move
    // every shipped pixel to no end.
    pressed: Color
    // What a chosen row is filled with.
    selection: Color
    // What a disabled control recedes towards: the surface behind it.
    panel: Color
    // The ink of a disabled control.
    faint: Color
    // What a hover is veiled with -- **black on a light look and white on a dark one**, because a
    // tenth of black over a dark grey is invisible; Material says the same of its state layer.
    veil: Color
    // How much of the veil. A tenth reads as lit without reading as pressed.
    veil_at: float
    // The look's edge, where it has one. **On the rows rather than on `Style`**, so that a look
    // whose pressed state loses its border can say so.
    edge: Option<widget.Border>
}
```

A look and feel under the name an application offers it by.

**A name and a style, not a constructor**, because the list is what a theme picker wants and a
list of functions is not one -- and because `named` would otherwise be a switch that every new
look has to be added to twice.
What a look's five state rows are grown from.

**A struct rather than nine arguments**, so that a look's rows read as the sentences they are at
the call site and so that F295 can replace this with the two colour ramps without touching four
looks. Each field is a colour some look was already stating; the two the rows *compute* --
hovered and disabled -- are the two no look could state before, because a translucent veil and a
text colour were all `Style` had to say them with.

### `state_rows`

```
pub fn state_rows(s: Seeds) -> controls.States
```

The five rows a look's seeds imply.

**Two of the five are computed and that is the point of the exercise.** `hovered` is the face
under the veil, resolved here rather than painted over a finished scene -- which is what lets a
hover change an edge and a word and not only a fill. `disabled` recedes *as a face* towards the
panel behind it: a disabled control that kept a live face and greyed only its text was the
defect D5 names, and no platform draws it that way.

Derivation happens once, when the look is built, and never at paint time (D2).

### `Look`

```
pub struct Look {
    // What to put in a menu. Capitalised as a title, because it is one.
    name: string
    style: controls.Style
}
```

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

**Three things change that a palette inversion would get wrong**, and they are the reason a
variation lives beside the thing it varies rather than being computed from it:


```
* **the hover veil is white.** `Seeds.veil` is mixed into the face, and a tenth of black over
  a dark grey is invisible -- the state layer has to come from the other end. Material says
  the same in its own words: the overlay is `on-surface`, which flips with the surface;
* **a shadow has to be deeper.** Depth is read as a darkening, and there is less room to
  darken below a dark window, so the same geometry at the same opacity reads as nothing. This
  is a half rather than a fifth;
* **`on_accent` goes dark.** A dark look wants a lighter accent so it reads against the
  window -- and dark text is what reads on a light blue, where white text on it does not.
  Checked rather than chosen: `theme_test` asserts both ends of every pair.
```

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

**Built from `modern` and not from `plain_dark`**, which is the question a second dark variation
raises: inheriting the dark colours would have given this look the plain one's shape, and shape
is what distinguishes it. A variation varies the *surround*, so it starts from its own look and
answers the colour decisions again -- the radius, the border, the padding and the shadow
geometry are all `modern`'s and are not repeated here.

The three traps `plain_dark`'s comment lists apply unchanged: a white hover veil, a deeper
shadow, and dark text on a light accent.

**The faces are lighter than the window rather than darker**, which is the one place a dark look
is not an inversion of a light one. On white, a control is *paler* than its surround and a panel
paler still; flipping that literally would put a near-black control on a dark grey window, where
the edge does all the work and the fill says nothing. Every dark theme worth copying raises the
surface instead -- the further forward a thing is, the lighter it gets -- which is Material's
elevation overlay stated as colour.
