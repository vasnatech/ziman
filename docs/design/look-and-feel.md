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

**The recommendation on the table is A + D now, B when hot reload is worth paying for, and not C.**
The first thing to borrow is not CSS at all: it is egui's `Widgets` (§10.1) -- states as a repeated
row.

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
| **F7** | `Theme.dark: bool` (and perhaps `Theme.id`) | `looknfeel.zm` | §6.6 |
| **F8** | Follow the desktop's light/dark preference | `lib/ui/desktop` | platform work, after F7 |
| **F9** | A look as a file -- Option B | new | after F1-F5 |

### 13.2 Questions that decide the features

- **Q1 -- Is hot reload worth a file format?** It is the only thing Option B buys that A and D do
  not, and it is what Textual leads with. Decides F9.
- **Q2 -- APCA or WCAG for the gate?** Radix's guarantees are APCA; `looknfeel_test` is WCAG.
  Decides the shape of D and of F3's test. Still open; **D2 makes it cheaper either way**, since
  OKLab lightness correlates with both.
- **Q3 -- Does a theme ever need to name one control type** (`Button.arc`), or is passing a
  different `Style` to that call enough? **DECIDED 2026-11-17 -- see §13.3/D1.**
- **Q4 -- Density as an axis or as entries in `all()`?** **DECIDED 2026-11-17 -- see §13.3/D4.**
- **Q5 -- Does `looknfeel` ever own the "feel"** (§6.7, key maps), or is the module's name
  aspirational and one key map the right answer?

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
