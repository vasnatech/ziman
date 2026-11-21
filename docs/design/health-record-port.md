# Porting health-record's front end: what the toolkit is missing

Status: **in progress, 2026-11-20.** §4.1 is **closed**: F322, F324-F329 are built, gated and in
the gallery. Of §4.2's five, **three are built** -- charts (F332, F333), navigation (F331) and the
virtualised list, which was closed by measuring it out of existence (F334) -- message catalogues
are designed with the build deliberately deferred, and **the file chooser is the one item left**.
P1, P2 and P3 are answered; **P4 is still owed**. The port itself has begun: §7.

## 1. Why this document

`health-record-frontend-node` is a React application that does, in a browser, what `lib/ui` is
meant to do on a desktop. Porting it is the second real application on this toolkit after
`curly gui`, and a much larger one -- so before any of it is written, this is what was measured
about the gap.

**Measured, not guessed.** Every number below came from reading the source on 2026-11-19.

## 2. What the target actually is

**37,244 lines** of TypeScript and TSX: **53 views**, **28 dialogs**, **47 shared components**.

Its own shared vocabulary is small, which is the encouraging part:

    Section  Card  Modal  Btn  IconBtn  ModalActions  Loading  Empty  Err
    Field  NumberInput  NumberField  DateField  TimeField  DateTimeField  SelectField  Textarea

How often each appears across files says what matters:

| thing | files |
|---|---|
| `Card` | ~~62~~ **22 + 7, and they are two different things -- see §7** |
| `Modal` | 51 |
| `Loading` | 48 |
| `Section` | 30 |
| filtering | 59 |
| sorting | 20 |
| searching | 15 |
| `Tooltip` | 16 |
| badge | 11 |
| `<table>` | **4** |

**The last row is the finding that reorders everything.** Only four files use a table. The
application is *card-based*: a record is a card in a list, not a row in a grid. `controls.Table`
is therefore far less central to this port than it looks, and `Card` -- which `lib/ui` does not
have -- is the single most used thing in the application.

**The `Card` count was wrong and the first ported screen is what found it** (§7). 62 was a grep for
the word, and it ran together two components: `CardShell`, a titled box with its own loading and
empty states (**22** files), and `UIPrimitives.Card`, a row with trailing actions (**7**). Only the
first is `controls.Card`.

## 3. What `lib/ui` already covers

Tabs, tree, table, split pane, scroll bar, slider, combo box, menu, context menu, modal dialog,
tooltip, progress bar, switch, check box, radio group, number field, date picker, toolbar,
expander, the grid, flow and border layouts, theming with a dark variation, the accessibility tree
and its AT-SPI bridge, icons from the system theme, and density.

That is most of a toolkit. The gaps below are what is left.

## 4. The gaps, by how many screens hit them

### 4.1 Ready to start — see `docs/reviews/tasks/f322-f328.md`

**A live region** was on this list and is built -- F329, pulled forward because F322 and F325 had
each already routed around it and F324 and F327 would have invented two more shapes. `Role.Status`
and `Role.Alert`, a pure diff of two trees, and a gate that listens from outside the process.

**Autocomplete** is the largest. Twelve bespoke ones -- allergy, diagnosis, document type, food,
immunisation, lab panel, lab test, manufacturer, medication, MIME type, MVX, procedure -- over one
generic `AutocompleteDropdown`. `lib/ui` has the entry, the filtered list and the popup layer, and
no assembly of the three.

**A card**, 62 files. **A busy indicator**, 48: `Progress.Busy` is a bar, and there is no spinner
and no convention for "this panel is still loading". **Sorting**, 20 files: F321 made it possible
by holding values rather than text, and nothing sorts yet -- today `"10"` sorts before `"9"`. **A
search field**, 15. **A badge**, 11.

**The clipboard**, which is every text field in the application. F273 left the seam built --
`Input.Paste` and `EntryStep.clipboard` -- and the platform half unwritten.

### 4.2 Needed its own design pass, so deliberately not a task yet

The tasks file says why this split exists: an item with no checkable done condition becomes a
headline that flattens the reasoning behind it. Four of the five have since had that pass, and
**three of those four turned out smaller than the headline** -- which is the same thing §4.1 kept
finding.

**Charts.** ~~Twelve files, three kinds -- line, bar and composed. `lib/graphics` can draw anything
and there is no chart layer at all. This is the largest single item in the programme and the one
that most decides whether the port looks like the original.~~

**Built 2026-11-20** ([charts](charts.md), [F332](../reviews/tasks/f332.md),
[F333](../reviews/tasks/f333.md)). Two marks, not a grammar of graphics: every axis in the target
is categorical and "composed" is a line series and a bar series in one chart. `lib/graphics/chart.zm`
draws, `controls.ChartView` is the widget and says what a reader is told.

**Message catalogues.** **1,709 keys in four languages** (en, es, fr, tr). `locale` formats
numbers, dates and names; nothing in the library holds a translated string. Without this the port
is English-only, which is a regression rather than a port -- and because it touches every screen,
the decision belongs before the porting starts even if the work lands later.

**Designed 2026-11-20, build deferred** ([message catalogues](message-catalogues.md)) -- see P2.
The measurement shrank it too: there are **no plural forms in the target at all**, so the
expensive, locale-specific half of a catalogue library is not needed to port this application.

**Navigation.** ~~53 views and 28 dialogs need a screen model with history. The gallery's
one-demo-at-a-time is a prototype of it, not an answer to it.~~

**Built 2026-11-20** ([navigation](navigation.md), [F331](../reviews/tasks/f331.md)). Two thirds
of the item was already built or was never the library's business: the 28 dialogs are done
(`dialog_layer` is modal), and "show one of N screens" is the application's switch, not the
toolkit's. What was missing was a history and what a screen change owes a reader --
`navigate.History` and `screen_widget`.

**A virtualised list.** ~~`Viewport` scrolls, but every row is laid out. A record list of
thousands is a question about the accessibility tree as much as about painting.~~

**Closed 2026-11-20 without building one** ([F334](../reviews/tasks/f334.md)). The target has no
virtualisation library at all: it pages on the server (`PAGE_SIZE = 20`) and reveals incrementally
(`visibleCount` growing by 30 on an `IntersectionObserver`). Nothing ever asks a list to draw a
thousand rows. What was missing was one signal -- a list saying it had been scrolled to the end --
which is now `ListStep.reached_end` and `list_at_end`.

**A file chooser.** The documents view needs open and save. Portal, GTK, or hand-built is a
decision, not a detail.

### 4.3 Out of scope unless asked

A map (leaflet, 1 file), barcode scanning through a camera (zxing), QR rendering, and
drag-to-reorder (dnd-kit, 1 file -- dashboard card order).

## 5. Decisions this programme owes

- **P1 — Charts: build a layer, or port those twelve views without them first?**
  Separable, and the largest item. A port that opens with empty panels where the graphs were is a
  worse demonstration than one that is missing two screens.

  **Decided 2026-11-20: build the layer, and it is built** -- [charts](charts.md), F332 and F333.
  What settled it was measuring the twelve files rather than weighing the two options. The target
  uses **two marks on a categorical axis**, no pie, no scatter, no stacking, no time axis, no
  animation; "composed" is a line and a bar in one chart. That is a weekend, not a programme, so
  the premise of the question -- that the layer is big enough to be worth skipping -- was false.

- **P2 — Message catalogues: in scope now, or English-only for a first version?**
  This shapes the port's code everywhere, so the answer is wanted before the porting begins, even
  if the building waits.

  **Decided 2026-11-20: design now, build later.** The shape is written down in
  [message catalogues](message-catalogues.md) so that every screen is built against it from the
  first one; the 1,709 keys get loaded whenever that is wanted. Retrofitting across finished
  screens is the expensive half.

  **The measurement changed what the design needs.** There are no plural forms in the target at
  all -- `count` appears in fifteen calls and every one passes it as a *value* -- so the expensive,
  locale-specific part of a catalogue library is not needed to port this application.

- **P3 — Is a card a control or a function?**
  `border_of` set the precedent that an arrangement stays a function (F320). A card is used 62
  times, which argues the other way, and it may want a role of its own for a reader.

  **Deferred on 2026-11-19, deliberately**: taken when F323 is started rather than now, with the
  other six behind it. The argument for waiting is that F322 and F325 will have said more about
  how much a reader needs named by then -- an autocomplete and a sorted table are both cases where
  the answer "a group with a name" either earns itself or does not.

  **Decided 2026-11-20: a card is a function**, returning a named group with `Role.Group`. The
  waiting was worth it, because what settled it was not F322 or F325 but **F324**, which was not
  on the list when P3 was written.

  The question turned out not to be control-or-function at all. It is **focusable or not**. A
  `Control` is a tab stop, and 62 cards on a dashboard would be 62 extra presses between a user
  and the thing they came for. Everything a card was wanted a control *for* is available without
  being one: a named group gives a reader "Medications, group" and gives a click a name to report,
  which is what `part_of` already does for the search field's clear mark and the panel's status
  region.

  **And the target's own card is a panel with a header.** `CardShell` takes `title`, `icon`,
  `loading`, `empty` and an action -- which is `controls.Panel` plus a title row, built in F324 a
  day before this was decided. So a card composes the panel rather than restating it, and the
  four-state rule ("no medications recorded" is not the same statement as "not yet") comes with
  it for free, on all 62.

- **P4 — The map and the barcode scanner: drop, stub, or find a desktop equivalent?**
  **Still owed.** One file each (§4.3), so this blocks nothing and is the last decision standing.

## 6. The shape of the work

1. ~~**The ready-to-start controls**, F322 to F328, each with a gallery demo.~~ **Done.** The
   gallery is how this toolkit has found every defect it has, so a control without a demo is a
   control nobody has looked at -- and rendering a demo found a defect that the tests had missed
   in every single one of F323, F326, F332 and F333.
2. ~~**Charts**, once P1 is answered.~~ **Done** -- F332, F333.
3. **The platform gates** -- catalogues, navigation, the file chooser -- which are what decide
   whether the port is a port or a demonstration. Navigation is built, catalogues are designed,
   **the file chooser is the one left**.
4. **Then the port itself**, which is where the work now is: §7.

## 7. The port

The application lives in `health-record/health-record-frontend-ziman` and **not in this
repository**, which is the whole of the rule it is held to: `lib/ui` is a general toolkit and the
port is what *finds* its gaps, never what shapes it. Its own README records what each screen found.

### Ported

| screen | lines in the target | |
|---|---|---|
| `AllergiesView.tsx` | 63 | the list: a fetch on a spawned thread, the four states, a row per record. |
| `AllergyDialog.tsx` | 94 | the form: thirteen fields, the allergen catalogue that fills in five of them, `POST`/`PUT`/`DELETE`, and a confirmation before a delete. |
| `AssessmentsVisualView.tsx` | 165 | the charts: one trend per screening instrument, grouped, ordered by when each was taken, with the severity band on the card and on the line. |

**The first was chosen for being the smallest real one**, and it was not a toy: `Section`, `Card`,
`Loading`, `Empty`, a criticality pill, a modal and a confirm dialog, in 63 lines. The second is
**the first form built on this toolkit at all**, and is where six controls met their first real
caller -- the autocomplete, the combo box, the entry, the date field, the modal and the grid.

### What the first three screens found

Three defects fixed, and all three were the same kind: **a control that documented a behaviour it
did not have**.

- [F335](../reviews/tasks/f335.md) -- **a quiet badge was invisible**, at 1.0005:1 against the
  window. Fixed with the hairline F323 used for the card's identical invisibility.
- [F336](../reviews/tasks/f336.md) -- **a text area measured 0 x 0**. `Entry.rows` says "how many
  lines tall" and the viewport inside it measures as nothing, so the field vanished in any column.
- [F337](../reviews/tasks/f337.md) -- **an axis of counts had gridlines at 11.5**. `axis_number`
  already refused to print `1.0` for a count and said why; it could only decide how to print the
  value it was handed, not where the lines went.

**Neither could have been found in the gallery, and both for the same reason**: each demo supplies
what the control was failing to ask for. The text area demo wraps itself in `tall_of(170.0)`; the
badge demo sits beside a button, where a missing capsule reads as "the quiet kind" rather than as
"missing". A gallery proves a control draws. **An application proves it draws in the company it
will keep.**

Four more things the form found are recorded in [F336](../reviews/tasks/f336.md) and in the
application's README: a dialog has no width of its own, a modal with a popup in it needs **three**
layout passes rather than two, `DateField` cannot be empty, and `Autocomplete` suggests strings so
a suggestion cannot carry a badge.

**And the third screen made one finding a decision rather than an observation.** `Badge.loud` is a
boolean; the allergy row wanted three colours and the assessment card wants **five**, which are a
*scale* -- MINIMAL to SEVERE, where telling one band from the next is the whole point. Two screens
have now asked, and the second asked for something a third colour would not answer. That is the
evidence this document said to wait for, and a badge that carries a colour is the first `lib/ui`
task the port has earned rather than guessed at.

And one correction to this document. **§2's "`Card` | 62 files" counted two different components
as one.** `CardShell` -- a titled box with its own loading and empty states, which is what
`controls.Card` models -- is used in **22** files. `UIPrimitives.Card` -- a *row*, content leading,
edit and delete trailing, a hairline beneath, no title -- is used in **7**. The 62 came from a
grep for the word. The row is the one this screen needed, and `lib/ui` has no control for it.

Four more findings from the list screen are recorded in the application's README and deliberately
not acted on, because one screen is not evidence enough to change a toolkit. The two that look
like real gaps rather than preferences:

- **No scrollable container.** Everything that scrolls in `lib/ui` is a control that draws its own
  rows. A column of arbitrary widgets uses `widget.named_scroll` directly -- and then the wheel
  does not reach it, because `controls.target` routes a wheel to the nearest *tab stop*.
- **The application loop, written out for the third time.** Fonts, density, look, window, poll, lay
  out, paint, publish, diff, sleep: the same ~200 lines as the gallery and as `curly gui`.

Both should wait for a second screen to say the same thing.
