# `paint`

What fills a region, and how a line is drawn.

**The paint set is closed, and that is the central decision of this library.** Java 2D's `Paint`
is an interface whose implementations produce pixels (`createContext` hands back a `Raster`), so
any paint a backend does not recognise forces rasterisation -- which is why PDF output from Java
2D quietly degrades to embedded bitmaps. Cairo makes the same set closed (six cases) and then
reopens it with one callback variant, and its PDF backend rasterises "fallback regions" for
exactly that reason.

These four cases are what PDF has native shading dictionaries for, so every paint here survives
as vectors in both backends. Adding a fifth means adding it to both, on purpose.

`use paint` -- or `use paint.Name` to bring one item in bare.

## Example

```
use geom
use paint

// Colour and the things a shape can be filled with. A `Paint` is solid, a gradient or a pattern,
// and which of those a backend can honour differs -- PDF has gradients and no patterns -- so the
// kind is a value the backend can refuse rather than something it must approximate.
fn demo() -> Result<unit, string> {
    // Components are 0..1 floats, not bytes, because that is the space blending happens in.
    val red = paint.rgb(1.0, 0.0, 0.0)
    printf("{} {} {} {}\n", red.red, red.green, red.blue, red.alpha)
    print(paint.gray(0.5).red)
    print(paint.transparent().alpha)

    // Hex is for the places colour arrives as text -- a config file, a stylesheet, a CSV.
    val from_text = try paint.hex("#3366cc")
    printf("{} {} {}\n", from_text.red, from_text.green, from_text.blue)
    print(is_ok(paint.hex("#zzz")))

    // A gradient is a list of stops: an offset along the axis and the colour there.
    val stops = [paint.stop_of(0.0, paint.white()), paint.stop_of(1.0, red)]
    print(len(stops))
    printf("{} {}\n", stops[0].offset, stops[1].offset)

    // The three kinds of Paint, which is what a backend inspects to decide whether it can draw a
    // fill natively or must refuse it: `pattern` is the one PDF has no answer for.
    val flat = paint.solid(red)
    val gradient = paint.linear(geom.point_of(0.0, 0.0), geom.point_of(100.0, 0.0), stops)
    print(paint.Paint.is_Solid(flat))
    print(paint.Paint.is_LinearGradient(gradient))

    // A stroke is width plus the joins and caps, and dashes are part of it rather than a separate
    // pass, so one value says everything about how a line is drawn.
    val line = paint.stroke_of(2.0)
    printf("{} {}\n", line.width, len(line.dash))
    print(len(paint.dashed(2.0, [4.0, 2.0]).dash))
    return unit
}

fn main() {
    if Err(e) = demo() {
        printf("paint example failed: {}\n", e)
    }
}
```

Output:

```
1.0 0.0 0.0 1.0
0.5
0.0
0.2 0.4 0.8
false
2
0.0 1.0
true
true
2.0 0
2
```

### `Color`

```
pub struct Color {
    red: float
    green: float
    blue: float
    alpha: float
}
```

Components are 0.0 to 1.0, not bytes, because PDF writes colour as decimals (`rg`, `RG`) and a
raster backend converts to bytes once at the end. Bytes in the middle would lose precision in a
gradient for nothing.

### `rgb`

```
pub fn rgb(r: float, g: float, b: float) -> Color
```

### `rgba`

```
pub fn rgba(r: float, g: float, b: float, a: float) -> Color
```

### `gray`

```
pub fn gray(level: float) -> Color
```

### `black`

```
pub fn black() -> Color
```

Functions rather than `pub val`s: a module-qualified global val is not reachable as
`paint.black` -- Ziman asks for `use paint.black` and a bare name -- and a caller who has written
`use paint` should not then need a second import per colour. The same reason `identity()` and
`empty_rect()` in `geom` are functions.

### `white`

```
pub fn white() -> Color
```

### `transparent`

```
pub fn transparent() -> Color
```

### `hex`

```
pub fn hex(text: string) -> Result<Color, string>
```

`#rgb`, `#rrggbb` or `#rrggbbaa`, with or without the leading `#`. An Err names what it could not
read, since a colour typo is otherwise a silent black.

### `rgb8`

```
pub fn rgb8(r: int, g: int, b: int) -> Color
```

Bytes rather than fractions, because that is how a document writes a colour: `rgb(240, 248, 255)`
and `#f0f8ff` are the same colour and neither of them is written as a float.

**Out of range clamps rather than refusing**, which is what CSS says to do and what every browser
does: a reader that rejects `rgb(300, 0, 0)` rejects a file everything else draws.

### `rgba8`

```
pub fn rgba8(r: int, g: int, b: int, a: int) -> Color
```

### `relative_luminance`

```
pub fn relative_luminance(c: Color) -> float
```

How light a colour is to the eye, between 0 and 1.

**Not the average of the channels, and not `gray`'s level.** Green carries nearly three quarters
of perceived lightness and blue under a fourteenth, which is why a pure blue on black fails a
contrast check that a pure green passes -- and why a palette chosen by eye from hex digits
drifts. Alpha is ignored: a translucent colour has no luminance until it is over something, and
the caller is the only one who knows what.

### `contrast_ratio`

```
pub fn contrast_ratio(a: Color, b: Color) -> float
```

WCAG's contrast ratio between two colours: 1.0 for a colour against itself, 21.0 for black
against white.

**Symmetric by construction** -- the lighter over the darker -- so there is no "foreground" and
"background" argument to get the wrong way round. AA wants 4.5 for body text and 3.0 for large
text or a control's edge; AAA wants 7.0.

Here rather than in the one application that needed it: `gallery/ui/desktop/highlight.zm` had
this and `relative_luminance` written out, which is a general fact about colour sitting in a
program about syntax colouring. `lib/ui/theme` now asserts every shipped look against it.

### `Oklab`

```
pub struct Oklab {
    lightness: float
    a: float
    b: float
    alpha: float
}
```

A colour in OKLab: a lightness that matches the eye, and two opponent axes.

**This is the space every derived colour in this library is computed in**, and the reason is that
its lightness is perceptually uniform -- equal steps of `lightness` look like equal steps, which
is what makes a tonal scale a scale rather than a list of numbers that happen to be ordered.
HSL's lightness is not that: `lighten` in HSL barely moves a blue and blows out a yellow, and
drifts the hue while doing it. FlatLaf's `autoInverse` option exists to paper over exactly that
-- it reverses the direction of a lighten above 65% lightness -- and there is nothing here for
such a flag to correct.

Alpha is carried through so that a round trip loses nothing. The axes are roughly -0.4 to 0.4 for
colours sRGB can show.

### `Oklch`

```
pub struct Oklch {
    lightness: float
    chroma: float
    hue: float
    alpha: float
}
```

The same colour with the two axes read as a distance from grey and an angle.

**This is the form a palette is built in**: hold `hue` and `chroma` and step `lightness`, and the
result is a ramp whose every entry belongs to the same colour. `hue` is in degrees, 0 to 360.

### `to_oklab`

```
pub fn to_oklab(c: Color) -> Oklab
```

The matrices are Björn Ottosson's, who fitted OKLab in 2020 against a perceptual dataset: sRGB to
linear, linear to a cone response, the cube root of that, and a final rotation.

### `oklab`

```
pub fn oklab(lightness: float, a: float, b: float) -> Color
```

```
pub fn oklab(lightness: float, a: float, b: float, alpha: float) -> Color
```

**Through polar coordinates, so that both notations get the same gamut treatment.** An OKLab
triple outside sRGB has a hue just as an OKLCH one does, and fitting it by reducing chroma is
the right answer whichever way the caller spelled the colour.

### `to_oklch`

```
pub fn to_oklch(c: Color) -> Oklch
```

### `oklch`

```
pub fn oklch(lightness: float, chroma: float, hue: float) -> Color
```

```
pub fn oklch(lightness: float, chroma: float, hue: float, alpha: float) -> Color
```

### `lighten`

```
pub fn lighten(c: Color, amount: float) -> Color
```

**`amount` is a distance in lightness, not a percentage of anything.** `lighten(c, 0.1)` raises
the colour's lightness by a tenth of the full range, and because that range is perceptual the
same tenth looks like the same step on a blue as on a yellow. A percentage of the *remaining*
headroom -- which is what a naive implementation does -- gives a different visual step for every
starting colour, so a theme written with it has to be re-tuned per colour.

### `darken`

```
pub fn darken(c: Color, amount: float) -> Color
```

### `saturate`

```
pub fn saturate(c: Color, amount: float) -> Color
```

Chroma has no fixed ceiling -- how much a hue can hold depends on the hue -- so this floors at
grey and lets `encoded` clip whatever sRGB cannot show.

### `desaturate`

```
pub fn desaturate(c: Color, amount: float) -> Color
```

### `change_lightness`

```
pub fn change_lightness(c: Color, lightness: float) -> Color
```

The three absolute setters, for a palette that states its steps rather than nudging them: this is
what a tonal ramp is built from, holding two coordinates and walking the third.

### `change_chroma`

```
pub fn change_chroma(c: Color, chroma: float) -> Color
```

### `change_hue`

```
pub fn change_hue(c: Color, hue: float) -> Color
```

### `fade`

```
pub fn fade(c: Color, alpha: float) -> Color
```

Alpha set outright, in sRGB, because transparency is not a colour: putting it through OKLab and
back would clip the colour's channels for no reason at all.

### `mix`

```
pub fn mix(a: Color, b: Color, at: float) -> Color
```

`at` is 0.0 for `a` and 1.0 for `b`, an interpolation parameter and not a weight -- the reading
`stop_of` already uses for a gradient's offset.

**Mixed on the opponent axes rather than through hue and chroma.** Interpolating a hue angle
between a grey and a colour means interpolating an angle that one end does not have, and taking
the short way round two saturated colours swings the mixture through hues in neither of them.
Rectangular mixing is also what CSS's `color-mix(in oklab, ...)` does, for the same reason.

### `tint`

```
pub fn tint(c: Color, at: float) -> Color
```

### `shade`

```
pub fn shade(c: Color, at: float) -> Color
```

### `contrast`

```
pub fn contrast(c: Color, dark: Color, light: Color) -> Color
```

Whichever of two inks reads better on `c`.

**Decided by `contrast_ratio`, which is the gate's own function**, so a colour chosen here cannot
be one the contrast test then rejects in favour of the alternative -- the choice and the check
agree by construction. Judging by lightness instead, which is the usual shortcut, picks the wrong
ink at exactly the mid-tones where the two are closest and it matters most.

Two candidates rather than a computed ink, because a theme's dark and light inks are decisions
someone made: this answers which of them to use, not what they should be.

### `named`

```
pub fn named(name: string) -> Option<Color>
```

**The 148 colour names CSS and SVG share**, or `None` for a name that is not one of them. Case
is folded under the invariant locale, because a colour name read out of a file is protocol text:
a Turkish locale folds the `I` of `Indigo` to a dotless `i` and the name stops matching.

**`transparent` is deliberately absent**, because it is not one of the 148 -- it is a keyword CSS
accepts wherever a colour is allowed, and `parse` answers it. `none` is likewise not a colour but
an instruction not to paint, which only the caller can act on.

The table is generated from the list every browser agrees on (Python's
`PIL.ImageColor.colormap`, the CSS3/SVG set) and then committed, because the set is closed: the
last addition was `rebeccapurple` in 2014, in memory of Eric Meyer's daughter. One value is
checkable in a line -- `python3 -c "from PIL import ImageColor;
print(ImageColor.colormap['papayawhip'])"`.

Grouped by first letter rather than written flat for the reason `charset.table_named` is: a flat
chain of this length pays for every name before the one it wants, and the grouping turns 148
comparisons into at most 21.

### `parse`

```
pub fn parse(text: string) -> Result<Color, string>
```

A colour as a document writes one: `#f0f`, `#ff00ff`, `#ff00ff80`, `rgb(255, 0, 255)`,
`rgb(100%, 0%, 100%)`, `rgba(255, 0, 255, 0.5)`, `rgb(255 0 255 / 50%)`, any of the 148 names, or
`transparent`.

**An Err rather than a fallback**, for `hex`'s reason: a colour that silently reads as black is a
wrong drawing with nothing to point at. What this deliberately does not accept is the rest of CSS
Color -- `hsl()`, `lab()`, `color()` -- and the two keywords that are not colours at all:
`currentColor` says to use somebody else's colour and `none` says to paint nothing, and only the
caller knows what either means (`svg.Fill` is one answer).

Hex needs its `#` here, although `hex` itself does not: without it `fff` and `red` are the same
kind of word, and guessing between a name and three hex digits is how `beef` becomes a colour.

### `Stop`

```
pub struct Stop {
    offset: float
    color: Color
}
```

One stop of a gradient: `offset` runs 0.0 to 1.0 along the gradient's own axis.

### `stop_of`

```
pub fn stop_of(offset: float, color: Color) -> Stop
```

### `Linear`

```
pub struct Linear {
    from: Point
    to: Point
    stops: [Stop]
}
```

### `Radial`

```
pub struct Radial {
    centre: Point
    radius: float
    stops: [Stop]
}
```

### `Tiled`

```
pub struct Tiled {
    image: int
    transform: Matrix
}
```

An image, placed by a matrix. The handle is an index into the scene's image table (phase 4);
paints do not own images, so the same image can back several paints and be written once.

### `Paint`

```
pub enum Paint {
    Solid(Color)
    LinearGradient(Linear)
    RadialGradient(Radial)
    Pattern(Tiled)
}
```

### `solid`

```
pub fn solid(c: Color) -> Paint
```

### `flat_of`

```
pub fn flat_of(p: Paint) -> Color
```

The one colour a paint is, or the one it starts at.

**For the callers that are structurally a colour**, not for painting: a run of text tinted under
an input method's composition, a mark, anything a gradient would buy nothing. A caller that can
draw a paint should draw the paint.

### `linear`

```
pub fn linear(from: Point, to: Point, stops: [Stop]) -> Paint
```

### `radial`

```
pub fn radial(centre: Point, radius: float, stops: [Stop]) -> Paint
```

### `pattern`

```
pub fn pattern(image: int, transform: Matrix) -> Paint
```

The constructor exists so the variant is reachable the same way the others are; **no backend
draws it yet**, and both say so rather than painting nothing (lib/graphics/pdf.zm).

### `Cap`

```
pub enum Cap {
    Butt
    Round
    Square
}
```

### `Join`

```
pub enum Join {
    Miter
    Round
    Bevel
}
```

### `Stroke`

```
pub struct Stroke {
    width: float
    cap: Cap
    join: Join
    // Past this ratio a miter join becomes a bevel. Without the fallback a near-180 degree join
    // produces a spike thousands of units long, which is the classic stroking bug.
    miter_limit: float
    // Empty for a solid line. Otherwise alternating on and off lengths.
    dash: [float]
    dash_phase: float
}
```

How a line is drawn. **Parameters, not an outline**: PDF has native `w`, `J`, `j`, `M` and `d`
operators, so the PDF backend passes these straight through and a stroked hairline stays a
hairline at any zoom. Only the raster backend turns a stroke into a fillable outline (phase 5).
Java 2D's `Stroke.createStrokedShape` is the right fallback and the wrong only path.

### `stroke_of`

```
pub fn stroke_of(width: float) -> Stroke
```

The common case: butt caps, miter joins, PDF's and PostScript's default limit of 10, no dash.

### `dashed`

```
pub fn dashed(width: float, dash: [float]) -> Stroke
```
