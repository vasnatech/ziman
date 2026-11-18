# Look and feel: what one is, what Swing and CSS each decided, and what ours could become

Status: **research, 2026-11-17.** Nothing below is built. §2-§5 and §7-§11 are a survey; §6 is a
measurement of this repository; §12 and §13 are the open decisions and exist to be worked through
one at a time. Nothing here has been agreed.

## 1. Why this document

`lib/ui/looknfeel/` was split out of `controls.zm` on 2026-11-17 and holds four looks -- `plain`,
`plain_dark`, `modern`, `modern_dark` -- each a fully written `controls.Style`. Its own header
claims a look and feel carries six kinds of decision: **colour, space, type, shape, depth, state**.
This document asks what other systems put in that list, measures which of the six we actually
implement, and records the options for the next step so the decision is made once and in writing.

Two questions prompted it: "what is constituted as a look and feel?" and "should this be CSS, or
CSS-adjacent?"

The companion document is `docs/design/desktop-applications.md`, which surveyed *toolkits*. This
one surveys *theming*.

## 2. What Swing says a look and feel is

Swing's answer is four layers, and almost every argument about look and feels is an argument about
which of the four you may replace.

| Layer | What it is | Replaceable by a theme? |
|---|---|---|
| 1. Identity and install hooks | `LookAndFeel` subclass: `getName`, `getID`, `getDescription`, `isNativeLookAndFeel`, `initialize`/`uninitialize`, `provideErrorFeedback`, `getDisabledIcon`, `getLayoutStyle` | only by writing a class |
| 2. `UIDefaults` | around a thousand keys of mixed type: colours, fonts, insets, borders, icons, integers, booleans, **and `InputMap`s** | yes -- this is what "theming" means |
| 3. `ComponentUI` delegates | one class per component (`BasicButtonUI`, `MetalTreeUI`) that paints *and* handles events | only by writing classes |
| 4. Resources | images, nine-slice borders, cursors, gradient painters | through layer 2 |

Layer 2 is where the surprises are. Sun put things in it that one would not expect a *look* to own:
`Tree.rowHeight`, `ScrollBar.width`, `TabbedPane.tabInsets`, `Table.rowHeight`,
`OptionPane.buttonOrientation` -- **and `Button.focusInputMap`**. In Swing, which key does what is
part of the look and feel. That is the "and feel" half of the name, and it is the half everyone
forgets.

## 3. Four generations of saying it declaratively

1. **Delegate-based** -- Metal, Windows, GTK, Motif. The look *is* the paint method. Nothing is
   declarative.
2. **Theme behind a delegate** -- `MetalTheme`: six colours (`primary1..3`, `secondary1..3`) and a
   handful of fonts (`controlTextFont` is Dialog 12, `subTextFont` Dialog 10). Fixed code,
   swappable palette. `OceanTheme` is a second instance of this and became Metal's default in
   Java 5. **This is the generation `lib/ui/looknfeel` currently belongs to.**
3. **Declarative XML** -- Synth, and Nimbus and Synthetica on top of it. A `<style>` carries
   `property`, `defaultsProperty`, `state`, `font`, `graphicsUtils`, `insets`, `painter`,
   `imagePainter`, `opaque`, `imageIcon`; a nested `<state>` carries `color`/`font`/`painter` under
   a combination of **ENABLED, MOUSE_OVER, PRESSED, DISABLED, FOCUSED, SELECTED, DEFAULT**; `<bind>`
   attaches a style to components by **regular expression** on name or region. Colour `type` is only
   `FOREGROUND`, `BACKGROUND`, `TEXT_FOREGROUND`, `TEXT_BACKGROUND`, `FOCUS` -- five slots, which is
   why Synth needs a painter for everything else. Two notes: Synth has **no defaults at all**
   (without a style for a region you get a blank canvas), and it is not really declarative --
   `<object class=...>` instantiates arbitrary Java, which Oracle's own page warns about.
4. **Typed token files** -- FlatLaf. Flat `Component.property` keys; values typed by key suffix or by
   an explicit `{float}` / `{scaledInteger}` prefix; `@variables`; `$references` to other keys;
   bracket prefixes for platform and theme (`[dark][gnome]TitlePane.buttonBackground`); wildcards
   (`*.background`); and around twenty colour functions -- `darken`, `lighten`, `saturate`, `fade`,
   `spin`, `changeHue`, `mix`, `tint`, `shade`, `contrast(color,dark,light,threshold)` -- with
   `relative`, `autoInverse`, `derived` and `lazy` options, so a hover colour can be computed at
   paint time from whatever the background turned out to be.

**The arc from 2 to 4 is the lesson of the whole survey: a palette is not a look.** What got added
was shape (`Button.arc`, `Component.focusWidth`, `Component.borderWidth`), metrics (`Button.margin`,
`TabbedPane.tabInsets`), and *derivation* -- one accent in, forty colours out.

## 4. The catalogue

**Bundled with the JDK:** Metal (`DefaultMetalTheme` / `OceanTheme`), Nimbus (Synth-based, added in
6u10), Synth, Windows, Windows Classic, Motif, GTK+, and vendor look and feels for AIX, HP-UX and
macOS (Aqua).

**Third party.** Wikipedia's list carries a "needs updating" tag from May 2020, so maintenance
status below is a reading rather than a cited fact.

| | Basis | Note |
|---|---|---|
| **FlatLaf** (JFormDesigner) | own delegates plus token files | the live one: Flat/IntelliJ/Darcula themes, HiDPI, a Theme Editor, used by NetBeans |
| **Radiance** (formerly Substance) | own delegates plus "skins" | a skin is colour schemes times painters (fill, border, decoration) times shapers. The deepest model of the lot |
| **Insubstantial** | fork of Substance | made when Substance paused; now historical |
| **Synthetica** (Jyloo) | Synth | commercial, pricing on request |
| **JGoodies Windows / Plastic** | own delegates | the classic "fix Metal's typography" pair |
| **JTattoo** | own delegates | around thirteen themes (Acryl, Aero, Noire, Smart) |
| **WebLaF** | own delegates plus skin XML | a look and feel *plus* an extra component library |
| **Quaqua** | Aqua refinement | Mac-faithful; historical |
| **Liquid, Napkin, SeaGlass, BeautyEye, Alloy** (Alloy commercial) | various | effectively dead, kept for the ideas. Napkin's -- make it look hand-drawn so nobody mistakes a mock-up for a product -- is still the best joke in the field |

## 5. The eleven things that constitute a look and feel

Pooling Swing, Synth, FlatLaf, Radiance, GTK, Qt, Textual, egui, Flutter, Material and Radix, eleven
things vary. This is the checklist the rest of the document measures against.

1. **Palette** -- not a colour per role but a colour per *(part, state)*: face, ink, border,
   selection, focus, each over enabled / hover / pressed / disabled / focused / selected / default.
2. **Typography** -- a font per role (family, size, weight, style), not only sizes.
3. **Metrics** -- insets, margins, gaps, and fixed sizes: row height, scrollbar width, tab insets,
   caret width.
4. **Shape** -- corner radii, border widths, focus-ring width and inset, flat against bevelled.
5. **Borders** -- as objects, per component.
6. **Icons** -- the check mark, the radio dot, the tree handle, the arrow. `Component.arrowType` is
   literally a theme key in FlatLaf.
7. **Fills** -- gradients, images, nine-slice; and `opaque` itself.
8. **Elevation** -- shadows, and `Popup.dropShadowOpacity`.
9. **Derivation rules** -- the functions that turn one accent into a theme, and the light/dark switch
   that reverses their direction.
10. **Feel** -- `InputMap`s per component, `LayoutStyle` gaps between related controls,
    `provideErrorFeedback`, scroll increments, button order in dialogs.
11. **Identity** -- name, id, description, `isDark`, `isNativeLookAndFeel`.

## 6. Where `lib/ui/looknfeel` stands, measured

ziman implements **layer 2 only, and deliberately**: `controls.zm` paints, `Style` is about thirty
tokens, and there is no delegate to swap. That is `MetalTheme`'s position (§3 generation 2), not
Swing's -- one set of widgets, many palettes. For a library whose `make check` holds every look to
WCAG AA, that is a good trade and worth keeping.

Against the eleven, measured on 2026-11-17:

### 6.1 Metrics are not themeable at all (item 3)

`controls.zm` carries **fifteen module-level geometry constants** no style can reach:

    val  mark_weight       = 0.14
    pub val min_thumb      = 20.0
    pub val bar_thickness  = 8.0
    pub val divider_thickness = 6.0
    pub val divider_step   = 10.0
    pub val divider_line   = 2.0
    pub val min_column     = 24.0
    pub val grid_width     = 1.0
    pub val column_grip    = 4.0
    pub val radio_height   = 24.0
    pub val track_thickness = 4.0
    pub val thumb_width    = 14.0
    pub val thumb_height   = 18.0
    pub val underline_width = 1.0
    pub val underline_gap  = 1.0

plus per-instance defaults written into the constructors: `row_height: 20.0` on three controls,
`header_height: 22.0`, `tab_width: 100.0`, `indent: 16.0`.

Every one of these is a look-and-feel key in Swing (`ScrollBar.width`, `Tree.rowHeight`,
`Tree.leftChildIndent`, `TabbedPane.tabInsets`, `Table.rowHeight`). **The consequence is that a
theme can recolour the gallery but not tighten it**: compact / comfortable / spacious, which every
design system ships and which is the only route to touch-sized targets, is inexpressible. `space`
reaches padding and nothing else.

### 6.2 Typography is sizes only (item 2)

`Style` has `text_size`, `heading_size`, `small_size` but a single `text_font: int`.
`widget.Metrics` is `{faces: [FaceMetrics]}` -- an indexed list -- and `lib/graphics/font.zm` has no
notion of weight at all: a face is a file the application loaded. So a theme cannot say "headings
are bold"; it can only say "everything is face 1". This is the gap JGoodies Plastic existed to fix
in Metal.

### 6.3 There is no derivation (item 9)

`paint` offers `rgb`, `rgba`, `gray`, `hex`, `rgb8`, `named`, `parse`, `relative_luminance`,
`contrast_ratio`, and the gradient constructors. **There is no HSL conversion and no colour
algebra.** So every look writes every colour by hand, which is why `plain_dark` had to independently
rediscover all four dark-variation rules (white hover veil, deeper shadow, dark `on_accent`, faces
lighter than the window) rather than inheriting them.

### 6.4 Two states are missing (item 1)

Against Synth's seven, `Style` covers pressed (`pressed_face`), hover (as the `hover_tint` overlay),
selected (`selection`), focused (a ring only) -- and disabled with `disabled_ink` **alone**. A
disabled button therefore keeps a live-looking face and merely greys its text, which no platform
does. There is also no default/primary emphasis, and `Button` has no variant field.

### 6.5 `Style` blocks fills the renderer can already draw (item 7)

`paint.Paint` has `Linear`, `Radial` and `Tiled`, and `scene` draws them. `Style.face`, `panel`,
`window` and `selection` are all `Color`. So a bevelled or glossy look -- Aqua, JTattoo's Aero,
Metal's gradient buttons -- cannot be written, not because the graphics layer cannot but because the
theme type will not carry it.

### 6.6 `Theme` has no identity beyond a name (item 11)

`Theme {name, style}`. Nothing can ask whether a look is dark, so nothing can branch on it. There is
a live example in this tree: the gallery's `highlight.zm` syntax palette is written for a light pane
and would need to flip.

### 6.7 The "feel" half is absent (item 10)

Keys are wired in `controls.zm` and fixed. That is defensible -- one key map is better than four --
but worth saying out loud that `looknfeel` is a *look* module.

### 6.8 What is already right, and should not be traded away

- **A look is data, not code.** This is what lets `looknfeel_test` hold every look in `all()` to AA
  on five surfaces. Nothing below should cost that.
- **`style` is a per-call argument.** `button_widget(w.save, style)` -- so per-control and
  per-instance overriding is *already free*, without a key table and without a cascade. Most of what
  CSS selectors buy, this already has.
- **The register is a list, not a switch**, so adding a look opts it into the contrast gate.
- `Style.window` and `Style.accent` were the two tokens a second look turned out to need, and both
  exist now.

## 7. The CSS question: four separable things

"A CSS approach" is four things, and they are independent. Conflating them makes this look like one
decision instead of three.

| | What it is | Cost here | Reading |
|---|---|---|---|
| **(a) Value grammar** | `#rrggbb`, `rgb()`, `hsl()`, `oklch()`, `color-mix()`, relative colours, `light-dark()` | low | take it |
| **(b) Property vocabulary** | the *names*: `border-radius`, `box-shadow`, `padding` | nil | take the naming, not the strings |
| **(c) A text file** | parse a stylesheet at run time | medium | the real question |
| **(d) Selectors and a cascade** | specificity, inheritance, matching a node tree | high | argue against |

**We are already partly in (a).** `paint.parse` accepts `#rgb`, `#rrggbb`, `rgb()`, `rgba()`,
`transparent` and **the 148 CSS named colours** -- its own error message says
`expected #rgb, #rrggbb, rgb(..), rgba(..) or one of the 148 colour names` -- and a comment above it
names `hsl()`, `lab()` and `color()` as the forms it does not take. So the CSS colour question has
already been answered yes, one layer down, and what is open is a short list.

## 8. GTK4 and Qt: the two toolkits that took the plunge

### 8.1 GTK4 is the argument for a scoped version, and against the full one

GTK4's CSS surface is about 110 properties. What is **missing** is the point:

> no `width`, `height`, `display`, `position`, `top`/`left`/`right`/`bottom`, `z-index`, `float`,
> `flex-*`, `grid-*`, `gap`, `overflow`, `text-align`, `vertical-align`, `box-sizing`, `cursor`,
> `clip-path`.

What it has for geometry is `min-width`, `min-height`, the margin and padding longhands, and
`border-spacing` -- "respected by GtkBoxLayout, GtkGridLayout, GtkCenterLayout". **GTK took CSS for
painting and kept layout in C.** That is already our split: `widget.zm` lays out, `controls.zm`
paints. Of the 110, around 30 are border, 14 transition and animation, 8 `-gtk-icon-*`, and 28 font
and text.

Three details matter more than the property list:

- **Selectors are on widget nodes, not widgets.** A `GtkScale` is `scale[.fine-tune]` containing
  `marks.top`, `trough`, `slider`, `[highlight]`, `[fill]`. That is the hidden cost: every control
  must publish a node tree and keep it stable forever, or themes break.
- **22 pseudo-classes**, mapped to state flags (`:hover` to `PRELIGHT`, `:disabled` to
  `INSENSITIVE`, plus `:checked`, `:indeterminate`, `:selected`, `:backdrop`, `:focus-within`,
  `:focus-visible`, `:dir(ltr)`, `:drop(active)`, `:root`). And **even GTK could not keep CSS's
  semantics**: the docs flag `:focus-within` and `:focus-visible` as behaving differently from CSS,
  "unlike CSS", because a toolkit needs them on ancestors.
- **The direction of travel is away from a bespoke dialect.** `@define-color` and the
  `lighter()` / `darker()` / `shade()` / `alpha()` / `mix()` functions are **deprecated** in favour
  of standard `--custom-properties` with `var()` (4.16+), and `hwb()`, `oklab()`, `oklch()`,
  `color()`, `color-mix()` and relative colours are supported. `prefers-color-scheme`,
  `prefers-contrast` and `prefers-reduced-motion` arrived only in **4.20**.

### 8.2 Qt is the cautionary tale, and it is specific

KDAB's *Say No to Qt Style Sheets* (David Faure, 28 April 2022) lists ten concrete failures. Six
transfer directly to any cascade built here:

1. `setFont` and `setPalette` become silently ineffective once a sheet is set -- "No effect (rather
   unexpected)" -- even for a sheet with nothing to do with colour.
2. Styled widgets stop following system colour changes.
3. **"Selectors on properties do not get reevaluated when the properties change"**, forcing "hacks
   such as reset of a style sheet with the associated cost".
4. Sheets "do not mix well with QProxyStyles or other QStyle subclasses", so there is no gradual
   migration.
5. Exempting one window is "very complicated (impacts all CSS rules)".
6. "Actual management of the stylesheet data does not scale (no include mechanism, etc.)", producing
   "super-complicated selectors".

Plus the structural one: **"If there's no support for a specific customization, there's no
solution."** Qt's own tracker collects these (QTBUG-68671) with maintainers' views ranging from
"document the limitations" to "deprecate stylesheets". The article gives no benchmarks and its
comments carry a fair counterpoint: one developer reports excellent separation of design from
implementation and no measurable cost. It is a trade, not a verdict.

## 9. Textual: the from-scratch precedent

A framework with its own widget set, its own renderer and text output -- the nearest thing to our
position -- chose a CSS subset and shipped it, as TCSS in `.tcss` files.

**Kept:** type, `#id`, `.class` and `*` selectors; fifteen pseudo-classes (`:blur`, `:dark`,
`:disabled`, `:empty`, `:enabled`, `:even`, `:first-child`, `:first-of-type`, `:focus`,
`:focus-within`, `:inline`, `:last-child`, `:last-of-type`, `:light`, `:odd`); descendant and child
combinators; `&` nesting (0.47.0+); `$variables`; specificity ordered ids, then classes, then types;
and `!important`, with the honest warning *"If everything is Important, nothing is Important."*

Its type selectors match class names **including base classes**, which the docs call "a departure
from browser CSS" -- so it diverged too.

**Stated motives**, worth taking at face value: *"separates how your app looks from how it works"*,
*"setting styles in Python can generate a lot of spaghetti code"*, and -- the one to weigh heaviest
-- **live editing under `--dev`, with changes "instantly updated in the terminal"**. Hot-reloading a
theme is a real productivity win and a compiled-in register cannot give it. That is the strongest
argument for a file.

## 10. The struct camp, which is where we already are

### 10.1 egui has the best single idea in the survey

`Visuals` (36 fields, among them `dark_mode: bool`, `disabled_alpha: f32`, `window_shadow`,
`panel_fill`, `faint_bg_color`, `extreme_bg_color`, `code_bg_color`, `warn_fg_color`,
`error_fg_color`) holds `widgets: Widgets`. `Widgets` is **five sub-structs** --
`noninteractive`, `inactive`, `hovered`, `active`, `open` -- each a `WidgetVisuals` with **the same
six fields**:

    bg_fill        -- widgets that must have a fill (slider, checkbox, radio); never transparent
    weak_bg_fill   -- widgets that may optionally have a fill (buttons); may be transparent
    bg_stroke      -- the surrounding frame outline
    fg_stroke      -- the interactive part: button text, slider grab, check mark
    corner_radius  -- the frame's corner radius
    expansion      -- how much larger the frame gets in this state

Lookup is `Widgets::state(WidgetState)` or `Widgets::style(&Response)`.

**This is a better answer than adding flat fields.** §6.4 wants `disabled_face` and `focused_face`;
egui shows that **a state is a row, not a column** -- one shape repeated per state, so a new state
cannot be half-implemented and a look cannot forget one. egui also has `disabled_alpha` with
`gray_out(color)`: a disabled colour *derived* rather than hand-set.

### 10.2 Flutter shows derivation shipped

`ColorScheme.fromSeed(seedColor, brightness, dynamicSchemeVariant, contrastLevel)` produces **46
named roles**, every one of which is also an optional override: `primary`, `onPrimary`,
`primaryContainer`, `onPrimaryContainer`, `primaryFixed`, `primaryFixedDim`, `onPrimaryFixed`,
`onPrimaryFixedVariant`, the same eight for secondary and tertiary, the `error` group of four,
`outline`, `outlineVariant`, `surface`, `onSurface`, `surfaceDim`, `surfaceBright`,
`surfaceContainerLowest`, `surfaceContainerLow`, `surfaceContainer`, `surfaceContainerHigh`,
`surfaceContainerHighest`, `onSurfaceVariant`, `inverseSurface`, `onInverseSurface`,
`inversePrimary`, `shadow`, `scrim`, `surfaceTint`. `contrastLevel` runs from -1.0 to 1.0.

**Material treats contrast as a theme axis, not a test.** We are the one library here that already
measures contrast in CI, so this is something we could do better rather than copy.

## 11. Token taxonomies worth stealing without the syntax

### 11.1 DTCG 2025.10

Stable since 28 October 2025 (Final Community Group Report, Candidate Recommendation). Its value is
the *type system*, not the JSON.

- **Simple types:** `color`, `dimension` (`{value, unit}`, px or rem, **unit required even for 0**),
  `fontFamily` (string or preference-ordered array), `fontWeight` (1-1000 or a name),
  `duration` (`{value, unit}`, ms or s), `cubicBezier` (`[P1x, P1y, P2x, P2y]`), `number`.
- **Composite types:** `border {color, width, style}`; `shadow {color, offsetX, offsetY, blur,
  spread, inset?}` (one or an array); `typography {fontFamily, fontSize, fontWeight, letterSpacing,
  lineHeight}`; `transition {duration, delay, timingFunction}`; `gradient` (stops of
  `{color, position}`); `strokeStyle` (a keyword, or `{dashArray, lineCap}`).
- **Structure:** `$value` (required), `$type`, `$description`, `$extensions`, `$deprecated`; a group
  is any object without `$value` and may set `$type` for its children; **`$extends` is a deep
  merge**; references are `{color.blue}` or a JSON Pointer `$ref`, may chain, and cycles are invalid.
  Tools must not infer a type from a group name.

Two findings bear on §6. **`typography` is a composite**, which says §6.2's fix should be a
`Typography {face, size, weight}` value rather than three parallel fields. And **`$extends` is a
deep merge**, which is exactly "`plain_dark` extends `plain`, overriding six things" -- the
mechanism that would have prevented §6.3's four rediscovered rules.

(Two sub-schemas -- gradient stop shape and `strokeStyle.dashArray` -- came from a summary rather
than verbatim text and should be re-checked against §7-§9 of the spec before implementing.)

### 11.2 Radix's twelve-step scale

The idea to think hardest about: a scale where **the step number is the role**.

| Step | Role | Step | Role |
|---|---|---|---|
| 1 | app background | 7 | element border and focus ring |
| 2 | subtle background | 8 | hovered element border |
| 3 | element background (normal) | 9 | solid background |
| 4 | hovered element background | 10 | hovered solid background |
| 5 | active / selected element background | 11 | low-contrast text |
| 6 | subtle border and separator | 12 | high-contrast text |

With a stated guarantee: **steps 11 and 12 reach Lc 60 and Lc 90 APCA contrast on step 2 of the same
scale.** Most step 9s are built for white foreground text; Sky, Mint, Lime, Yellow and Amber for
dark text on steps 9 and 10.

Why this matters more here than anywhere else in the survey: `Style` names around twenty colours
*individually* and `looknfeel_test` checks them pairwise after the fact. On a scale, 4 and 5 **are**
hover and active by construction -- so `hover_tint` and `pressed_face` stop being independent
decisions that a dark variation can get wrong. The gate changes from "did you get these twenty
right" to "is this a valid scale".

One caveat with teeth: Radix uses **APCA**, while `paint.contrast_ratio` is WCAG 2. That is a real
choice, not a detail.

Material's equivalent is three token tiers -- reference, system, component, each referencing the one
below -- over five HCT tonal palettes of thirteen tones each, from one or two seed colours.

## 12. The four options

- **Option A -- CSS values only.** Add `hsl()`, `oklch()`, `color-mix()` and relative colours to
  `paint.parse`, plus the colour algebra of §6.3. No new concepts; `svg` and `pdf` benefit too; and
  GTK's own deprecation of `shade()` in favour of `color-mix()` is the precedent. Self-contained in
  `paint.zm` with no callers to break.
- **Option B -- a token file with no cascade.** A flat key-to-value table that *compiles to a
  `controls.Style`*: `@variables`, references, one `extends`, validated once at load, returning
  `Result`. No selectors, no specificity, no matching. This is FlatLaf's model and DTCG's, and the
  parser already exists -- `lib/core/ini.zm` has groups, `get_float`/`get_bool`/`get_int`, lists and
  locale-aware values, and already reads icon themes. Buys hot reload (§9) and user themes without a
  compiler. Costs a schema kept in step with `Style`.
- **Option C -- real CSS with selectors and a cascade.** Needs a node tree with stable names and
  classes, a matcher, specificity rules, a rule cache and invalidation on every state change. Two
  specific reasons to decline: `style` is **already a per-call argument** (§6.8), so the main thing a
  cascade buys is something we have for free; and the one-function rule -- a hit test must read the
  same calculation the widget laid out with -- is hard to hold when the style is resolved by pattern
  match at paint time. Every one of §8.2's ten complaints becomes a ziman bug report.
- **Option D -- a structural palette.** Radix's scale or Material's tiers, replacing
  individually-named colours. Orthogonal to A-C and best combined with A.

**Superseded by §13.3.** Every question in §13.2 was answered on 2026-11-17 and the outcome is: **A
and D taken, C closed, B declined in favour of a live editor.** Option A is D2 and D3; option D is
D8's two twelve-step ramps; option C died with D1, because a theme that never names a control type
leaves nothing for a selector to select; and option B lost to the gallery editor in D12, because the
same decisions that would have made a file cheap also made a format unnecessary. The first thing
borrowed was not CSS at all: it was egui's `Widgets` (§10.1) -- states as a repeated row, which is
D5.

## 13. Agenda

To be worked through one at a time. Nothing here is decided.

### 13.1 Features

| | Feature | Where | Tier |
|---|---|---|---|
| **F1** | Density: the fifteen constants and four instance defaults of §6.1 become theme tokens, derived from `space` | `controls.zm`, `Style` | makes a second *look* possible |
| **F2** | Colour algebra in `paint`: HSL, `lighten`/`darken`, `saturate`, `fade`, `mix`, `tint`/`shade`, `change_hue`, `contrast(c, dark, light)` | `paint.zm` | prerequisite for F3 |
| **F3** | `looknfeel.derive(...)`: a look from one accent, plus overrides | `looknfeel.zm` | needs F2 |
| **F4** | States as a repeated row, after egui's `Widgets` -- including the missing `disabled_face` and emphasis | `Style` | §6.4, §10.1 |
| **F5** | A type scale with a face per role, as a DTCG-style `Typography` composite | `Style` | §6.2, §11.1 |
| **F6** | `Paint` where `Style` says `Color`, keeping `Color` as the contrast-bearing value | `Style` | §6.5 |
| **F7** | `Theme.dark: bool` (and perhaps `Theme.id`) | `looknfeel.zm` | **absorbed into D8** -- `Scheme` is a parameter, so this is read off the register rather than set |
| **F8** | Follow the desktop's light/dark preference | `lib/ui/desktop` | platform work, after F7 |
| **F9** | A look as a file -- Option B | new | after F1-F5 |

### 13.2 Questions that decide the features

- **Q1 -- Is hot reload worth a file format?** **DECIDED 2026-11-17 -- see §13.3/D12.**
- **Q2 -- APCA or WCAG for the gate?** **DECIDED 2026-11-17 -- see §13.3/D7.**
- **Q3 -- Does a theme ever need to name one control type** (`Button.arc`), or is passing a
  different `Style` to that call enough? **DECIDED 2026-11-17 -- see §13.3/D1.**
- **Q4 -- Density as an axis or as entries in `all()`?** **DECIDED 2026-11-17 -- see §13.3/D4.**
- **Q5 -- Does `looknfeel` ever own the "feel"** (§6.7, key maps)? **DECIDED 2026-11-17 -- see
  §13.3/D13.**

- **Q6 -- Do the geometry and input functions take a `Style`?** Raised 2026-11-17 by starting F293.
  Seven functions read the numbers D6 moves and have no style in scope -- `split_at`, `thumb_of`,
  `divider_rect`, `edge_at`, `on_the_bar`, `radios_input`, and privately `bar_box` and
  `table_resized` -- and six are `pub`. `split_at` and `thumb_of` are the one-function-rule
  functions, the single calculation a widget and every gesture share, so theming their numbers
  changes signatures that `tests/`, `gallery/` and curly all call. **This is the cost D6 did not
  foresee, and it is a public API decision rather than an implementation detail.**

- **Q7 -- Whose number wins when an application already hardcoded it?** **DECIDED 2026-11-18 --
  see §13.3/D17.**

### 13.3 Decisions taken

**D1 (Q3) -- a scale, with the library assigning the steps. 2026-11-17.**

A theme never names a control type. `Style` carries a small *scale* per axis, and `controls.zm`
says once, for all looks, which step each control sits on:

    pub struct Shape {
        none:   widget.Radius   // separator, grid line
        small:  widget.Radius   // check box, text field
        medium: widget.Radius   // button, tab, card
        large:  widget.Radius   // popup, dialog
        full:   widget.Radius   // pill, scroll thumb
    }

    fn button_radius(s: Style) -> widget.Radius { return s.shape.medium }
    fn field_radius(s: Style)  -> widget.Radius { return s.shape.small }

**Why.** `Style.corner`'s own comment already argued the flat case -- "A theme with four-point
buttons and square selection is not a theme, it is two" -- and `icon_gap` repeats it. The practice
agreed: across 21 gallery demos, the gallery shell and curly's whole GUI there is exactly **one**
per-call override in the tree, `gallery/ui/desktop/state.zm:211`'s
`with style { text_font: code_face }` for the monospaced code pane. But the comments overclaim by
treating one number as the only alternative to twenty. A scale is the third thing, and it is what
Material's shape scale and Radix's colour scale both are: the theme moves the scale, the design
system owns the assignment. So a look can be squarer overall without being able to make buttons and
selection disagree.

**What it settles.**

- **Option C is closed.** If a theme never names a control type there is nothing for a selector to
  select, and §8.2's ten complaints are not worth buying.
- **Option B (F9) shrinks** to a file that sets scale steps -- five numbers for shape, not twenty
  keys -- so the string-keyed table of §12 is not needed either.
- **F1, F4 and option D become one mechanism, not three.** Density is a space scale, states are a
  state scale (§10.1's rows), colour is a tonal scale (§11.2). Each is "a scale the theme sets and
  the library indexes".
- Call-site override with `with style { ... }` stays, for the one case a year that wants it.

**D2 (F2) -- OKLab/OKLCH is the one working space, and derivation happens once. 2026-11-17.**

`paint` gains a single colour algebra working in OKLab: `to_oklab`, `oklab`, `oklch`, and over them
`lighten`, `darken`, `saturate`, `desaturate`, `fade`, `mix`, `tint`, `shade`, `change_hue`, and
`contrast(c, dark, light)`. **No second space.** The functions keep plain names and their doc says
which space they work in; two vocabularies for one operation is the thing `Style.corner`'s comment
argues against for shape, and it is no better for colour.

**Why OKLab and not HSL.** D1 made colour a tonal scale, and "even steps of lightness at constant
hue and chroma" is OKLCH's definition and something HSL cannot do -- HSL's lightness is not
perceptual, so even steps look uneven and drift in hue. The sharpest evidence is FlatLaf's own API:
its `autoInverse` option "inverts the amount if lightness is above 65% (lighten) or below 35%
(darken)", which is a workaround for HSL's non-uniformity and has nothing to correct in OKLab. GTK
reached the same place from the other side -- its `shade()` and `mix()` are deprecated in favour of
standard `oklch()` and `color-mix()` -- and Material's HCT exists precisely so that tone equals L*,
which OKLCH gives for much less code.

**What it costs.** Less than it looks: `relative_luminance` already contains `linear_channel`, the
sRGB-to-linear step, so what is missing is a matrix and a cube root.

**Derivation is computed once, when the look is built -- not at paint time.** FlatLaf has a
`derived` mode that recomputes against a component's actual background; we already get that case by
a different route, because `hover_tint` is a translucent overlay composited by
`widget.paint_hovered` and adapts to whatever face it lands on without computing anything.
Compositing answers the case the flag exists for, and a look that computes at paint time stops
being data -- which §6.8 says is the one property not to trade.

**D3 (F2) -- `paint.parse` reads notations; functions do expressions. 2026-11-17.**

`parse` gains `hsl()`, `hsla()`, `oklch()` and `oklab()`, which are ways of *writing one colour* --
exactly its existing contract, and forms that turn up in real SVG files and design-tool exports. It
does **not** gain `color-mix()`, relative colours (`oklch(from X ...)`) or `light-dark()`: those are
expressions over colours and belong in the typed algebra, where a mistake is a compile error rather
than a load-time one.

The line is: **parse denotes, functions compute.** Note that `parse` accepting `hsl()` does not
reintroduce HSL as a working space -- it is a notation converted to a `Color` on the way in, and
everything after is D2.

**D4 (Q4) -- density is an axis, not entries in the register. 2026-11-17.**

`all()` stays four looks. Density is a transform over a style:

    pub enum Density { Compact, Comfortable, Spacious }
    pub fn at_density(s: controls.Style, d: Density) -> controls.Style

**Why.** The contrast gate decided it. `looknfeel_test` holds **every look in `all()`** to AA on
five surfaces, and that gate is what makes adding a look safe -- but density changes no colour at
all. Crossing four looks with three densities gives twelve entries of which eight are
colour-identical to another, so the gate would do the same work three times and a theme menu would
show twelve rows to offer four looks. An axis also composes for nothing: compact dark modern needs
no entry and no name, and nobody has to remember to add it.

**The objection, and the answer.** A transform can produce a combination nobody tested, where an
entry in `all()` is tested by construction. The answer is to split the tests by what they actually
test: the colour gate keeps holding four looks to AA, and a **geometry** test asserts invariants
across all three densities -- no control shorter than its text plus padding, no target below the
minimum. That catches a class of defect twelve colour-identical rows never would.

**The pattern this sets, which now holds twice.** A variation on an axis is **a function from
`Style` to `Style`**, not another entry in the register. D1 gave the library the scales; D4 gives
the application the axes over them. `emphasised`, below, is the third instance.

**D5 (F4) -- five state rows, each carrying colour, border and geometry. 2026-11-17.**

    pub struct Parts {
        face: Color
        ink: Color
        border: Option<widget.Border>
        expansion: float                  // paint-time only -- see the invariant below
        corner: Option<widget.Radius>     // overrides the shape scale for this state
    }

    pub struct States {
        rest:     Parts
        hovered:  Parts
        pressed:  Parts
        disabled: Parts
        selected: Parts
    }

**Five rows, one mechanism.** Every control's paint function asks one question -- what are this
control's parts in this state -- so a state cannot be half-implemented and a look cannot forget one.
This retires `pressed_face`, `disabled_ink`, `selection`, and `hover_tint` with the generic overlay
in `widget.paint_hovered`; roughly twenty paint functions start selecting their own row, and the
gallery's pixels move.

**Focus and emphasis are deliberately not rows.** Focus is additive -- a control can be focused
*and* hovered *and* pressed at once, so it cannot be one of N exclusive rows; it stays a ring over
whatever state applies, drawn from `accent`. egui agrees and has no focused row among its five.
Emphasis (Synth's `DEFAULT`, a primary button) is an **axis**, since a primary button is emphasised
while at rest, hovered, pressed and disabled alike: it is `looknfeel.emphasised(style)`, the third
instance of D4's pattern.

### The expansion invariant

**No state may change a control's bounds.** `expansion` is a paint-time outset and is never an
input to layout or to hit testing.

This matters because `expansion` is the only field here that could break the one-function rule -- a
hit test must read the same calculation the widget laid out with. egui's own documentation concedes
the wart, saying `expansion` "makes hovered things misaligned"; it gets away with it by re-laying
out every frame, which we do not do. If expansion grew the box, a control would be hovered at a
pixel its layout never claimed, two neighbours could both claim the boundary, and the result is
flicker at every edge.

So: **the rest state draws its frame inset from its bounds, and a hovered state spends that inset.**
Layout and hit testing use the resting geometry always; expansion is bounded by the resting inset.
The drawn frame grows, the box never does, nothing overlaps a neighbour, and the invariant holds by
construction rather than by care.

`corner` needs no such rule -- a radius affects neither bounds nor a rectangular hit test.

**D6 (F1) -- a six-step space scale, and minimums that density cannot touch. 2026-11-17.**

    pub struct Space {
        none: float   // 0
        xs:   float   // 2
        s:    float   // 4   -- today's `space`
        m:    float   // 8
        l:    float   // 12
        xl:   float   // 16  -- a tree's indent, a section gap
    }

    pub struct Minimum {
        target: float   // 24.0 -- WCAG 2.2 SC 2.5.8
        thumb:  float   // 20.0
    }

Six steps, as Material and most design systems ship, so the scale maps one-to-one onto design-tool
output. `at_density` (D4) moves `Space` and **does not touch `Minimum`**.

**Why minimums are a separate group.** Three of the nineteen numbers are not spacing at all but
minimum targets -- `min_thumb 20.0`, `min_column 24.0`, `radio_height 24.0` -- and two of them are
exactly **24**, which is also WCAG 2.2's minimum target size (SC 2.5.8). If a Compact density scaled
them, the library's densest mode would be the one that fails an accessibility criterion, and the
gate would have to be told to permit it. Compact tightens padding and gaps; a scroll thumb and a
radio stay grabbable. This also gives the new geometry test something **absolute** to assert at
every density rather than a ratio: every interactive control at least `minimum.target` on its
smaller axis.

**The fourth group.** Four of the nineteen are **stroke widths**, not spacing: `grid_width 1.0`,
`divider_line 2.0`, `underline_width 1.0`, `mark_weight 0.14`. A hairline is a hairline at every
density, so these do not belong on the space scale either -- they want their own small group, and
`mark_weight` is a *ratio* rather than a length and should stay one.

**Recommendation attached, not decided: assert that every step is used.** Six steps for a widget set
with about thirty inset sites means `xs` and `l` start life with no caller, and today's stray 6s
(table padding, radio insets) normalise to `s` or `m` -- pixels move, which is what removing the 6
costs. An unused scale entry is a decision a look author must make for no reason, and it is exactly
the sort of thing nothing notices: compare `gen_lib_map.py`, which raised `KeyError` for an unknown
length of time because no target ran it. A test asserting each step has at least one caller would
catch a dead step at the point it dies.

**D7 (Q2) -- WCAG 2 gates, OKLab lightness derives. 2026-11-17.**

`looknfeel_test` keeps holding every look in `all()` to **WCAG 2 AA** with `paint.contrast_ratio`,
and F3's derivation picks tones by **distance in OKLab lightness**, the space D2 already chose.

**Why the gate stays WCAG.** It is the claim the library makes to its users, and it is what
accessibility law actually references -- EN 301 549, Section 508 and the ADA all point at WCAG 2.x
AA. "We meet APCA Lc 75" is not an answer an auditor accepts today.

**Why derivation is not WCAG.** Checking a pair and choosing a tone are different jobs. WCAG 2's
ratio is a poor perceptual model and is **not polarity-aware** -- it scores dark-on-light and
light-on-dark alike when they do not look alike -- and that is worst for the dark half of `all()`.
Walking a tone until the ratio clears 4.5 would also make the derivation and the gate the same
function, so the gate would stop being an independent check.

**Why not APCA.** It is the better model and it is what Radix's published guarantees are quoted in
(Lc 60 and Lc 90 on step 2), but it is a **draft whose constants have moved between revisions**, and
pinning those inside a gate that must stay green is a cost with no deadline attached. D2 already
bought a perceptual lightness: OKLab's L is uniform by construction, so "far enough apart" is
answerable without a second model. That is also what Radix's scale *is* by construction, whatever
its guarantee is quoted in. Revisit if WCAG 3 lands.

**D8 (F3) -- the shipped looks are derived, from two 12-step ramps, with the scheme as a
parameter. 2026-11-17.**

    pub enum Scheme { Light, Dark }
    pub fn ramp(seed: Color, s: Scheme) -> [Color]          // 12 steps, Radix's roles
    pub fn derive(seeds: Seeds, s: Scheme) -> controls.Style

    pub fn plain() -> controls.Style {
        return with derive(seed_plain, Scheme.Light) {
            scrim: ..., collapsed_mark: ">", expanded_mark: "v"
        }
    }

Three decisions in one.

**The shipped looks are derived, with overrides.** `plain()` becomes `derive(...)` plus a short list
of deliberate exceptions. The derivation is then exercised by everything we ship, so it cannot rot
-- the same argument that makes the register a list, so a new look opts into the gate. And **the
override list is a measurement of the derivation**: if `plain()` needs twenty overrides the rule is
wrong, and we find out rather than guess. The cost is that the four looks' exact colours move once,
so any pixel baseline is re-blessed.

**Two ramps of twelve steps, with Radix's published roles.** A neutral seed and an accent seed,
each expanded to twelve steps whose meanings are §11.2's table. D1 already said the library indexes
a scale; this is a scale whose steps have documented meanings, which **makes D5's rows stop being
independent choices**: `hovered` is step 4 and `pressed` is step 5, not two more colours a dark
variation can get wrong.

    window = neutral[1]   panel = neutral[2]   rest = neutral[3]
    hovered = neutral[4]  pressed = neutral[5] border = neutral[7]
    ink = neutral[12]     accent = accent[9]   selection = accent[4]

**The scheme is a parameter, not a second authored look.** `derive` indexes the ramp from one end or
the other, and that produces all four of §6.3's dark rules mechanically rather than as rules: faces
lighter than the window is simply what step 3 means on an inverted ramp, the hover veil reverses
because the veil is another step, `on_accent` flips with the accent ramp, and the shadow deepens
with the neutral one. This is why Material's `fromSeed` takes `brightness` and Radix ships each
scale both ways.

### The register is computed, not listed -- and the gate must not shrink

D4 kept `all()` at four looks because density changes no colour. **A scheme does change colour**, so
if `all()` dropped to two looks the AA gate would quietly stop testing the dark half -- a gate
shrinking because a mechanism got better is the worst possible outcome here.

So `all()` is **computed as looks times schemes**: two looks give four rows, the menu shows four, the
gate tests four colour-distinct styles, and adding a fifth look adds two rows without anyone
remembering to. D4's reasoning holds -- nothing colour-identical is duplicated and density stays out
of the register -- and **F7 is absorbed**: `Theme`'s scheme is read off the register rather than set
by hand.

**D9 (F5) -- a five-role type scale of composites, and a clamped face index. 2026-11-17.**

    pub struct Type {
        face: int        // index into widget.Metrics.faces
        size: float
        line: float      // line height
    }

    pub struct TypeScale { heading: Type, body: Type, label: Type, caption: Type, code: Type }

Five roles, each a complete answer, with `controls.zm` assigning each control a role -- the D1
pattern again. This replaces three loose sizes and one shared face index.

**Weight comes from the face index, and the index is clamped.** A look says which face each role
uses, exactly as `text_font` and `icon_font` already do; the application supplies the files. The
library **clamps an out-of-range index to face 0** rather than failing to draw. That matters because
`gallery.zm` records that the renderer "refuses a run naming a face the options do not hold", and
works around it by loading the text face twice so index 1 is always real. With a clamp, a
single-face application renders a look that wants bold -- it simply renders it regular.

Teaching `font.zm` about family and weight is the better long-run answer and was declined here as
scope: it is a font-matching layer with its own fallback policy, not part of a type scale. Synthetic
bold was declined outright -- stroking a glyph changes its advances, so measured text and drawn text
would disagree, which is precisely the defect `Style.text_font` already caused once when a caret was
positioned by one face's metrics and drawn in another's.

**Two knock-ons.** The clamp retires the gallery's load-it-twice workaround. And a `code` role
retires `gallery/ui/desktop/state.zm:211`'s `with style { text_font: code_face }` -- **the only
per-call style override in the tree**, and the one case that looked like a counterexample to D1.

**One consequence to settle when it is built**: `Type.line` is new, and `widget.Metrics` already
carries a line-height function per face which the caret arithmetic reads. Two sources of line height
is one too many, and the caret is exactly where that goes wrong.

**D10 (F6) -- fills carry a paint, marks stay flat, and a gradient is a span of ramp steps.
2026-11-17.**

    // What a look WRITES -- authoring:
    pub struct Fill { from: int, to: int, angle: float }   // steps of this look's ramp

    // What a control READS -- built by `derive`:
    pub struct Parts { face: paint.Paint, ink: Color, ... }

`face`, `panel`, `window` and `selection` become paints; `ink`, `border`, `grid` and the caret stay
`Color`. **A fill is a thing a gradient makes sense for; a mark is not** -- a gradient buys a mark
nothing and costs the gate its clarity.

**The gate gets stricter, not vaguer.** §12 worried that a gradient face has no luminance and the
gate would need "a representative colour". D8 removes the problem: a gradient is a span between two
*known ramp steps*, so the ink must clear 4.5 against **every stop** and a gradient face passes only
if it holds at both ends. Better still, every stop in a built style is provably a ramp step, so the
ramp can be checked once instead of extracting stops per look.

**`Fill` authors, `Paint` is drawn** -- the same two-layer shape as D8's seeds and derived style. One
consequence worth having: **`from == to` is a flat fill**, so solid and gradient are one form with no
`Option` and no second field.

**D11 (F8) -- the window layer reports the preference, the application decides, and it is
followed live. 2026-11-17.**

    // lib/ui/desktop, beside display_scale()
    pub enum Preference { NoPreference, Dark, Light }
    pub fn color_scheme() -> Preference
    // and a scheme change arrives like a resize

**This is much cheaper than §13.1 claimed.** F8 was listed as "platform work", but GIO's D-Bus is
**already linked into `ziman-gui`**: `src/native_ibus.c` calls `g_dbus_connection_call_sync` and
`g_dbus_connection_signal_subscribe`, and the Makefile says why -- "it costs nothing, because
at-spi2-atk already brought it. That is the whole reason ibus is a binding and not a second
backend." The portal call is a pattern already in the tree, not a new dependency.

**The split.** Platform code stays with the platform, policy stays with the application, and
`looknfeel` stays pure data -- which is what lets its tests be tests, and what keeps it working on a
machine with no portal running. Same split as `Style.window`, which the library names and the
application hands to `raster.Options`.

**Followed live, not read once.** GNOME's automatic dark mode switches **at sunset**, so a
startup-only read goes stale during an ordinary session and the application reads as ignoring the
user's setting.

### The portal, and the two traps in it

`org.freedesktop.portal.Settings`, namespace `org.freedesktop.appearance`, key `color-scheme` (type
`u`):

    0 = no preference     1 = DARK     2 = LIGHT

**1 is dark and 2 is light** -- not alphabetical, and easy to invert. Unknown values are to be
treated as 0.

1. **`ReadOne` is current and `Read` is deprecated, but older portals answer `ReadOne` with "No such
   method"** -- so the fallback to `Read` is required, not optional. Ghostty hit exactly this.
2. **Some desktops historically never emitted `SettingChanged`** (a KDE bug fixed around Plasma
   5.24), so a stale value must degrade to "no preference" and must never hang.

**D12 (Q1, F9) -- a live editor in the gallery, and no file format. 2026-11-17.**

The gallery's look-and-feel demo gains controls over the seeds and the scale steps, repainting every
demo live; a look is then designed with instant feedback and the result pasted into a `.zm` file as
two seeds. **F9 is closed, not deferred to a date.**

**The arithmetic changed while this document was being written.** F9 was ranked thirteenth on the
assumption that a look is thirty-five literals, which a file could carry and a slider could not.
After D6, D8 and D9 a look is **two seeds, a scheme, six scale steps and a short override list** --
so the thing that made a format necessary is gone. You cannot put a slider on thirty-five literals;
you can put one on two seeds and six numbers.

What a file would uniquely have bought is an end user theming an application they did not compile,
and there is no such caller. §13.1's own history is the argument: `Style.window` and `Style.accent`
were found **by an application needing them**, not by guessing, and a format designed against an
imagined requirement is the kind of thing this repository has learned to wait on. If an outside
caller ever appears, D8 is what will make a file safe -- because a look is derived, a loader can run
the AA check and **refuse** a file, which was never possible for hand-written literals.

The pain Q1 was really about is the loop: `rm -f ziman-gui && make gui` is mandatory and easy to
forget. The editor answers that without a schema, a validator, or a new way for a look to be invalid
at run time.

**D13 (Q5) -- the module is renamed `theme`, and the "feel" stays out of it. 2026-11-17.**

`lib/ui/looknfeel/` becomes `lib/ui/theme/`; `looknfeel.plain()` becomes `theme.plain()`. One key
map stays wired into `controls.zm`, and the module stops implying otherwise.

**Why rename rather than park the question.** The module carries a name describing something it does
not do and is not going to do: key bindings belong with the controls that read them, and a user
picking "Modern dark" from a menu must not silently change what Home does -- which is exactly what
Swing shipped, and why the same Swing application answers keys differently on Windows and on macOS.

**Why now.** The rename is at its cheapest today. The module was created on 2026-11-17 with four
looks and a handful of callers; it touches `LOOKNFEEL_PARTS` in the Makefile, the header rule in
`make/c.mk`, three includes and four registry rows in `src/corelib.c`, `GROUPS` in
`tools/gen_lib_manual.py`, and the callers in `gallery/` and `curly/`. Every week it waits, that list
grows.

**Attached recommendation: `Theme` becomes `Look`.** `theme.Theme` stutters, and D8 already made
"look" the word that carries meaning -- the register is two *looks* across two schemes. So `all()`
returns `[Look]`, and `Look { name, style }`.

**A real gap this question surfaced, which is not a look.** Dialog button order, Home/End versus
Cmd-arrow, and the double-click interval are **platform conventions**, not theme choices -- Swing
put them in `UIDefaults` and `LayoutStyle` and so conflated them with the look. They belong in their
own value, chosen by the platform rather than from a theme menu, and they are **not** tracked by this
document.

This document keeps its own name, and its references to `looknfeel` before this line are historical.

**D14 (F293) -- `Type.line` is a multiplier on the face's own line height. 2026-11-17.**

    pub struct Type { face: int, size: float, line: float }   // line: 1.0 is the font's own leading

    fn line_of(m: widget.Metrics, t: Type) -> float {
        return m.faces[t.face].line_height(t.size) * t.line
    }

**One source of truth with one theme-owned factor.** `widget.FaceMetrics.line_height` is a function
of size computed from the real font, and it stays authoritative; a theme may only stretch it. So the
caret arithmetic keeps reading one calculation, which is the whole reason this needed deciding --
`Style.text_font` already caused a defect by measuring with one face's metrics and drawing in
another's.

**The precedent is in the same struct.** `FaceMetrics.ascent` exists because "a baseline cannot be
guessed from a line height": every glyph in the widget set was drawn a descent too low, invisible to
every assertion because the boxes were right and only the ink was wrong. The font knows its own
ascent, descent and gap; a theme does not, and a theme written against a Latin face does not know
what CJK or Devanagari need. An absolute line height would have repeated that mistake with a
different number.

**It composes with D4.** Density scales the factor -- compact 1.2, comfortable 1.4, spacious 1.6 --
so a dense table gets tighter text as well as tighter padding, which is the half of density a user
actually sees. An absolute value could not be scaled without knowing the face.

**D15 (F293) -- a fourth group: the proportions the library owns. 2026-11-17.**

Mapping D6's nineteen numbers onto its three groups for real left **four with no home**, so there is
a fourth, and it is not themed:

    pub val mark_weight = 0.14      // a ratio, not a length  (D6 already exempted this)
    pub val divider_step = 10.0     // how far an arrow key moves a divider: an input increment
    pub val thumb_width = 14.0      // a slider thumb's proportions, which are the library's
    pub val thumb_height = 18.0

**A theme governs spacing, minimums, strokes, shape and type. It does not govern how far an arrow
key moves, or what shape a slider thumb is.** Bending those onto the scale would have changed the
thumb's shape and made a compact density alter a keyboard increment, which is not density.

`underline_gap` becomes `stroke.hair` rather than a space step: the rule sits **one hairline below
the text**, so the gap and the line move together when a density thickens strokes, and the 1 does
not have to become the scale's smallest 2.

The full mapping, with the one deliberate pixel move:

    min_thumb 20     -> minimum.thumb        grid_width 1      -> stroke.hair
    min_column 24    -> minimum.target       divider_line 2    -> stroke.line
    radio_height 24  -> minimum.target       underline_width 1 -> stroke.hair
    bar_thickness 8  -> space.m              underline_gap 1   -> stroke.hair
    column_grip 4    -> space.s              divider_thickness 6 -> space.m   (6 -> 8, pixels move)
    track_thickness 4 -> space.s

**A finding worth its own task: the slider thumb is 14 x 18**, which is under the 24 x 24 that D6
just made a floor for everything else (WCAG 2.2 SC 2.5.8). It is grabbable inside a taller row, so
this is a question rather than a defect, but it is the one place the floor does not reach.

**Found while building F293, 2026-11-18: the type scale has no readers at all.**

`Style.heading_size` and `Style.small_size` are **read nowhere** -- not in `lib/`, `tests/`,
`gallery/`, `docs/lib/examples/` or curly. They are declared in `Style`, set by `plain()`, and that
is the end of them. Meanwhile **both applications declare their own**: `gallery_ui.zm` and
`accessibility_ui.zm` and their siblings each carry `val heading_size = 12.0` and
`val small_size = 11.0`, and so does curly's `request_ui.zm` -- at 12 where the theme says 16.

This is the shape `Style.window` had before D-nothing fixed it, and the tasks README already has the
sentence for it: *"A capability added and never adopted is indistinguishable from one that was never
added."*

**It changes what D9 is for.** The premise was that the type scale carries only sizes and wants
faces per role. The deeper problem is that **a heading is not a control** -- the control set has no
heading in it, so `controls` was never going to read `heading`. The readers of a type scale are
**applications**, which is exactly why two of them wrote their own literals. So D9's five roles are
right and the work is not only in `Style`: it is in making the gallery and curly read the scale
instead of inventing one each.

**Which raises a question that is not F293's to settle** (see Q7): the theme says a heading is 16
and both applications draw 12. Making them read the scale changes pixels in two applications unless
the looks change instead.

**D17 (Q7, F293) -- the looks move to the practice, and the scale gains its readers.
2026-11-18.**

    pub struct Type { face: int, size: float, line: float }
    pub struct TypeScale { heading: Type, body: Type, label: Type, caption: Type, code: Type }

`Style`'s four loose type fields -- `text_size`, `heading_size`, `small_size`, `text_font` -- become
one scale. `icon_font` stays out of it: an icon is not text, it has no line height to stretch, and
its size is an em square taken from whatever role draws beside it.

**The looks adopt the practice rather than the reverse.** `heading` is 12, not the 16 the token
used to claim, because 12 is what both applications were actually drawing from their own
`val heading_size`. So adopting the scale moved **no pixels anywhere**, including curly's committed
geometry baseline, which passed untouched.

**The library reads two roles and the applications read the rest, which is the point.** `body` is
text a control *contains* -- a field's content, a list's rows, a table's cells, a link, and **every
caret calculation**; `label` is a control's own name -- a button, a tab, a menu title, a check box,
a radio. `heading` and `caption` have no control reader because **a heading is not a control**, and
that is exactly why two applications invented one. The gallery's and curly's design files now read
`style.text.heading.size` and `style.text.caption.size` -- 24 files, around 78 readers, where there
were none.

**`code` became a role rather than an index poked into a field.** The gallery used to write
`with style { text_font: code_face }`; it now says `as_code(style)`, which hands the `Entry` a style
whose `body` *is* the look's `code` role. An `Entry` cannot know it is showing code, so naming the
role is the application's job and this is what that looks like.

**Two tokens made live that would otherwise have been dead on arrival.** `Type.line` is read by
`controls.line_for`, the one function a caret and the box it sits in both measure with -- and
`at_density` stretches it, gently (0.9/1.0/1.15) rather than by `space`'s factor, because 0.75 of a
font's line height overlaps one line's descenders with the next line's ascenders. `Type.face` is
read through `controls.face_for`, which **clamps** an index the window has not loaded instead of
letting the renderer refuse the run -- retiring the gallery's load-the-text-face-twice workaround.

**Density moves the leading and never the sizes**, and `theme_test` asserts it. A compact mode that
shrank the text would be a zoom, and it is the one change a reader with low vision cannot undo from
their own settings. Material's density does the same.

**D18 (Q8, F293) -- a row is measured, not written down. 2026-11-18.**

The last four numbers of §6.1 -- `row_height` 20 on `List`, `Tree` and `Table`, `header_height` 22,
`indent` 16, `tab_width` 100 -- were fields on the control **instances**, so no look and no density
reached them: `at_density(Compact)` tightened every padding and gap in the window and left every
table row exactly twenty points tall.

**Three of the four are derived and the fourth stays a field:**

    pub fn row_height(m, style)    = line_for(m, style.text.body) + style.space.s
    pub fn header_height(m, style) = line_for(m, style.text.label) + style.space.m
    pub fn indent_for(style)       = style.space.xl
    Tabs.tab_width                 -- stays a field

**Why not a token set to 20.** A token is the same number in a different place: it can still
disagree with the face the theme asks for, and nothing would say so. A row is the one box in this
library whose height *is* a line of text, so it is measured like one -- by `line_for`, the same
function the row's label is laid out with, which is what keeps the box and the text in it from
reading different numbers.

**Why `tab_width` is not derived with them.** It is the one of the four that genuinely varies per
instance: the gallery's file tabs are 170 and its demo tabs 110, and curly sets its own. The other
three were never set to anything but their default in any caller.

**What it cost: twelve functions now take `widget.Metrics`** -- `list_input`, `list_widget`,
`tree_input`, `tree_widget`, `table_input`, `table_widget`, `table_body`, `table_cursor`, `edge_at`,
`menu_input`, `menu_popup_layer`, `combo_box_input`, `combo_box_popup_layer` -- which is the same
argument `entry_widget`, `check_box_widget`, `radios_widget` and `link_widget` already take, and
exactly what `Tabs.tab_width`'s own comment said a fitted tab strip would need. In the menu's
functions the `Menu` parameter was renamed `menu` so that `m` keeps meaning the metrics, as it does
everywhere else in the file.

**`Table.header_height: 0.0` became `Table.headers: bool`.** Zero was how a caller said "a grid of
values in a dialog, with nothing to put in a header"; the height is now the theme's, so the
capability is said as the thing itself. One function, `band_height`, answers how tall the band is
here, so the widget, the body and the hit test cannot disagree.

**Two hard-coded paddings came with them**: a list's and a tree's rows were `insets_of(4, 2, 4, 2)`
and a table's cells `insets_of(6, 2, 6, 2)`. Both are now `s` beside the text and `xs` above and
below -- the same air at the default density, and the thing that keeps a row from clipping its own
label at a compact one, where `row_height` is a line plus a smaller `s`.

**Pixels moved, as this decision said they would.** With a real font a row is no longer exactly 20:
curly's committed `window_geometry.txt` moved by under a point per sidebar row (20 -> 20.7) and was
re-blessed. In the control tests, where the metrics make a line twice the point size, a row is 28 --
which is why those tests now read `row_tall()`, `on_row(n, x)` and `in_body(n, x)` instead of the
literal 20s and the y values that only meant "row one" while a row was twenty tall.

### 13.4 Build order

**Raised as F291 to F297** in [docs/reviews/tasks/f291-f297.md](../reviews/tasks/f291-f297.md), one
task per step, each naming the traps that step will hit. Derived from the dependencies above, not
from the ranking in §13.1 -- which was written before any of these decisions existed.

0. **D13 -- the rename, first.** Cheapest now, purely mechanical, and doing it first means every
   diff below is written once rather than rewritten.
1. **D2 and D3 -- `paint`.** OKLab and OKLCH, the algebra over them, `contrast`, and `parse`'s two
   new notations. Self-contained in one file with no callers to break, and everything after needs it.
2. **The `Style` restructure -- D1, D5, D6, D9, D10 together.** The shape scale, the space scale
   with its minimum and stroke groups, the five-role type scale, the five state rows and the fills
   all touch one struct, so they are one change rather than five. The fifteen constants and four
   instance defaults of §6.1 go here. Largest diff, and the gallery's pixels move.
3. **D8 -- `derive`, the ramps, and the computed register.** The four looks become derived; the AA
   gate is re-blessed once; the override lists are the measurement of whether the derivation is
   right.
4. **D11 -- following the scheme.** `window.color_scheme()` and the live signal, with both
   compatibility paths.
5. **D12 -- the gallery editor.**
6. **The two recommendations attached but not decided**: the test that every scale step has at least
   one caller (D6), and resolving the two sources of line height (D9).

## 14. Sources

Swing and third-party look and feels: [LookAndFeel](https://www.cs.unb.ca/programmingclub/docs/java/javax/swing/LookAndFeel.html),
[UIManager](https://docs.oracle.com/en%2Fjava%2Fjavase%2F22%2Fdocs%2Fapi%2F%2F/java.desktop/javax/swing/UIManager.html),
[UIDefaults](https://developer.classpath.org/doc/javax/swing/UIDefaults.html),
[Key Look-and-Feel Classes, O'Reilly *Java Swing*](https://www.oreilly.com/library/view/java-swing/156592455X/ch26s02.html),
[Synth file format](https://docs.oracle.com/javase/8/docs/api/javax/swing/plaf/synth/doc-files/synthFileFormat.html),
[Synth tutorial](https://docs.oracle.com/javase/tutorial/uiswing/lookandfeel/synth.html),
[NimbusLookAndFeel](https://checkerframework.org/jdk-api/jdk/javax/swing/plaf/nimbus/NimbusLookAndFeel.html),
[DefaultMetalTheme](https://cs-214.epfl.ch/jdk-21.0.9-docs/api/java.desktop/javax/swing/plaf/metal/DefaultMetalTheme.html),
[MetalTheme](https://www.matools.com/doc/java8/javax/swing/plaf/metal/MetalTheme.html),
[FlatLaf properties files](https://www.formdev.com/flatlaf/properties-files/),
[FlatLaf how to customize](https://www.formdev.com/flatlaf/how-to-customize/),
[Pluggable look and feel](https://en.wikipedia.org/wiki/Pluggable_look_and_feel),
[A peek into Substance internals](https://www.pushing-pixels.org/2009/04/22/a-peek-into-substance-internals.html),
[Radiance/Substance docs](https://github.com/kirill-grouchnikov/radiance/blob/1f124dd95bd6949d489f380dabdee47ac11272ef/docs/substance/substance.md).

CSS and CSS-adjacent: [GTK4 CSS properties](https://docs.gtk.org/gtk4/css-properties.html),
[GTK4 CSS overview](https://docs.gtk.org/gtk4/css-overview.html),
[GTK3 CSS overview](https://docs.gtk.org/gtk3/css-overview.html),
[Say No to Qt Style Sheets, KDAB](https://kdab.com/say-no-to-qt-style-sheets),
[Style Sheets vs QStyle](https://forum.qt.io/post/33602),
[QTBUG-68671](https://bugreports-test.qt.io/browse/QTBUG-68671),
[Textual CSS guide](https://textual.textualize.io/guide/CSS/),
[CSS in the terminal, McGugan](https://www.willmcgugan.com/blog/tech/post/css-in-the-terminal-with-python-and-textual),
[CSS color-mix() support](https://developer.chrome.com/docs/css-ui/css-color-mix),
[Avalonia style selectors](https://docs.avaloniaui.net/docs/styling/style-selectors),
[Slint widget styles](https://docs.slint.dev/latest/docs/slint/reference/std-widgets/style/).

Struct themes and token taxonomies: [egui Visuals](https://docs.rs/egui/latest/egui/style/struct.Visuals.html),
[egui Widgets](https://docs.rs/egui/latest/egui/style/struct.Widgets.html),
[egui WidgetVisuals](https://docs.rs/egui/latest/egui/style/struct.WidgetVisuals.html),
[ColorScheme.fromSeed](https://api.flutter.dev/flutter/material/ColorScheme/ColorScheme.fromSeed.html),
[Design Tokens Format Module 2025.10](https://www.designtokens.org/TR/2025.10/format/),
[DTCG announcement](https://w3c.github.io/cg-reports/design-tokens/CG-FINAL-format-20251028/),
[Radix: understanding the scale](https://radix.nodejs.cn/colors/docs/palette-composition/understanding-the-scale),
[Material 3 colour system](https://developer.android.com/design/ui/wear/guides/styles/color/system).

Two pages were blocked by the fetch proxy and read from mirrors instead: `www.w3.org` (the DTCG
report, read at designtokens.org) and `www.radix-ui.com` (read at radix.nodejs.cn).
