# Porting health-record's front end: what the toolkit is missing

Status: **in progress, 2026-11-19.** The gap list in §4.1 is **closed**: F322, F324-F329 are built, gated and in the gallery. Nothing in §4.2 is started -- each of those five needs its own design pass first -- and decisions P1, P2 and P4 are still owed. No screen of the target has been ported yet.

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
| `Card` | 62 |
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

### 4.2 Needs its own design pass, so deliberately not a task yet

The tasks file says why this split exists: an item with no checkable done condition becomes a
headline that flattens the reasoning behind it.

**Charts.** Twelve files, three kinds -- line, bar and composed. `lib/graphics` can draw anything
and there is no chart layer at all. This is the largest single item in the programme and the one
that most decides whether the port looks like the original.

**Message catalogues.** **1,709 keys in four languages** (en, es, fr, tr). `locale` formats
numbers, dates and names; nothing in the library holds a translated string. Without this the port
is English-only, which is a regression rather than a port -- and because it touches every screen,
the decision belongs before the porting starts even if the work lands later.

**Navigation.** 53 views and 28 dialogs need a screen model with history. The gallery's
one-demo-at-a-time is a prototype of it, not an answer to it.

**A virtualised list.** `Viewport` scrolls, but every row is laid out. A record list of thousands
is a question about the accessibility tree as much as about painting.

**A file chooser.** The documents view needs open and save. Portal, GTK, or hand-built is a
decision, not a detail.

### 4.3 Out of scope unless asked

A map (leaflet, 1 file), barcode scanning through a camera (zxing), QR rendering, and
drag-to-reorder (dnd-kit, 1 file -- dashboard card order).

## 5. Decisions this programme owes

- **P1 — Charts: build a layer, or port those twelve views without them first?**
  Separable, and the largest item. A port that opens with empty panels where the graphs were is a
  worse demonstration than one that is missing two screens.

- **P2 — Message catalogues: in scope now, or English-only for a first version?**
  This shapes the port's code everywhere, so the answer is wanted before the porting begins, even
  if the building waits.

- **P3 — Is a card a control or a function?**
  `border_of` set the precedent that an arrangement stays a function (F320). A card is used 62
  times, which argues the other way, and it may want a role of its own for a reader.

  **Deferred on 2026-11-19, deliberately**: taken when F323 is started rather than now, with the
  other six behind it. The argument for waiting is that F322 and F325 will have said more about
  how much a reader needs named by then -- an autocomplete and a sorted table are both cases where
  the answer "a group with a name" either earns itself or does not.

- **P4 — The map and the barcode scanner: drop, stub, or find a desktop equivalent?**

## 6. The shape of the work

1. **The ready-to-start controls**, F322 to F328, each with a gallery demo. The gallery is how
   this toolkit has found every defect it has, so a control without a demo is a control nobody has
   looked at.
2. **Charts**, once P1 is answered.
3. **The platform gates** -- catalogues, navigation, the file chooser -- which are what decide
   whether the port is a port or a demonstration.
4. **Then the port itself.**
