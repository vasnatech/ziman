# Desktop applications: binding a toolkit, or 1.2 microseconds a pixel

Status: **proposed, 2026-11-10.** Nothing below is built. The measurement in §3 was taken first and
is the reason the recommendation is what it is; §4 is the constraint that decides the rest.

## 1. The gap

A Ziman program today can be a command-line tool, a terminal interface, or an HTTP server. It cannot
be a desktop application: there is no window, no input event, and no way to put a pixel on a screen.

This document asks what it would take, having looked at what Java Swing, JavaFX, C++ Qt, GTK,
Flutter, Slint and the immediate-mode libraries each decided, and having measured the one thing that
turns out to decide the answer.

**The honest framing first.** This would be the largest single addition in the library's history, and
unlike `terminal` — four natives, because the gap was four functions wide — it does not reduce to
something small. Two of the three routes in §6 are multi-month. The third already works and ships in
curly today. A reader who wants a graphical application next week should read §6's route C and stop.

## 2. What already exists, which is more than it sounds

The 2D stack is complete: `geom`, `shape`, `paint`, `scene`, `stroker`, `font`, `truetype`, `image`,
`raster`, and a `layout` engine with paragraphs, grids and tables — 6,937 lines, rendering one
declarative `Scene` to either a PDF or an `Rgba8` image, with anti-aliased fills under both winding
rules, stroking with caps and joins and dashes, arbitrary clip paths, gradients, embedded TrueType
faces and cached glyph rendering.

That is not a small part of a toolkit. **It is most of one.** A `Scene` of `Op.Fill`, `Op.Stroke`,
`Op.Text`, `Op.DrawImage` and `Op.Nested` is, structurally, JavaFX's scene graph. `layout` is the
text and table engine that every toolkit has to write. `font`/`truetype` is the part people
underestimate twice.

So the missing pieces are narrower than "a GUI toolkit":

1. a **window**, sized and titled, that the OS will show;
2. **input events** — pointer, keyboard, scroll, focus, resize, close;
3. a **presentation path** that gets pixels onto that window;
4. the **OS integration** a real application needs: clipboard, text input and IME, file dialogs,
   cursors, DPI scaling, menus, accessibility;
5. a **widget set** and the state model that drives it.

Items 1-3 are a few hundred lines of C against any windowing library. **Items 4 and 5 are the
project**, and §5 shows that every toolkit's character is decided by how it answers them.

## 3. The measurement that decides the shape

If Ziman can already draw, the obvious design is: build a `Scene` each frame, call `raster.render`,
blit the result. `tests/bench/ui_frame.zm` tries it on a scene shaped like an application window — a
toolbar of eight buttons, a sidebar of forty rows, and thirty lines of body text, 98 ops:

| | ops | time |
| --- | --- | --- |
| 640x480 | 98 | 841 ms |
| 1280x800 | 98 | 1896 ms |
| 1920x1080 | 98 | 3433 ms |
| 1280x800, `samples=1` (no anti-aliasing) | 98 | 1791 ms |
| 1280x800, fills only, no text | 20 | 1671 ms |
| **1280x800, empty scene — nothing drawn at all** | **0** | **1247 ms** |

Figures are from one machine and vary by a few percent between runs — the full frame measured
1917-2014 ms over three consecutive runs and the empty scene 1271-1313 ms, so the ratio below is
stable even though neither absolute number is exact.

Two seconds a frame is not an interactive rate, but the interesting column is the last row. **About
two thirds of a real frame is spent before anything is drawn**, and the floor is linear in pixels:

| empty scene | pixels | time | per pixel |
| --- | --- | --- | --- |
| 320x240 | 76,800 | 89 ms | 1.16 µs |
| 640x480 | 307,200 | 376 ms | 1.22 µs |
| 1280x800 | 1,024,000 | 1247 ms | 1.22 µs |
| 1920x1080 | 2,073,600 | 2584 ms | 1.25 µs |

**So the cost is not the drawing. It is touching pixels from the interpreter at all.** Anti-aliasing
is 5%. All 78 text runs together are about 12%. A glyph cache — the fix one would reach for first —
would buy almost nothing, because the glyphs were never the problem.

60 fps at 1280x800 needs 16 ns per pixel. The interpreter delivers 1,220. **It is ~75x short, and
that is with an empty scene**, so no amount of scene-level cleverness closes it. This is not a
criticism of `raster.zm`, which is doing per-pixel compositing in a tree-walking interpreter and
doing it correctly; it is a statement about where the floor is.

Three consequences, and they shape everything below:

- **A pure-Ziman painting loop cannot drive a window.** Not with damage regions either: a 240x24
  list row still costs ~7 ms of floor, so a text caret blinking in a form would eat a frame budget.
- **Whatever renders a window must be C.** That is not a concession; it is what JavaFX (Prism),
  Flutter (Impeller, formerly Skia), Swing (Java2D's native pipelines) and every browser also do.
  The managed language describes the scene; native code turns it into pixels.
- **`raster.zm` keeps its job** — the one it has, which is documents and images, where two seconds
  is irrelevant. It also becomes the *oracle*: a C renderer can be tested by rendering the same
  `Scene` both ways and comparing pixels, which is a far better gate than screenshots.

**Acted on, and the numbers above are now history.** F270 moved the per-pixel loops into
`src/native_raster.c` in three instalments: `clear_to` and `finish`, which this section identified;
then the composite inside `fill_shape` and `blit` for the solid-paint case; then the coverage pass
both feed. Measured by alternating two binaries in the same minute, three rounds each, because this
machine's absolute figures moved 2x between batches an hour apart with no code change:

| 1280x800 | all Ziman | now | ratio |
| --- | --- | --- | --- |
| empty scene | 2548 ms | 36 ms | 70.7x |
| fills only | 3295 ms | 72 ms | 45.7x |
| full frame | 3859 ms | 232 ms | 16.6x | `tests/lang/graphics_raster.zm`, 418
lines of asserted pixels across anti-aliasing, both winding rules, strokes, gradients, alpha and all
three output formats, is byte-identical across the change -- which is the oracle argument above,
used rather than described.

**The conclusion is unchanged and better evidenced**, and the remaining cost has moved somewhere
this section did not predict. 232 ms is still 14x short of 60 fps at that size -- but of it, 36 ms
is the canvas and 36 the fills, so **most of what is left is text, and that part is not
rasterisation at all**: glyph tiles are cached, and the cost was `truetype` allocating a `Result`
per byte read inside a binary search over the font's character map.

The prediction that the renderer has to be C is demonstrated rather than implied. The prediction
that the *drawing* is where the time goes held only until the drawing was fast, after which the
table parsing beside it was -- and **that part did not need C at all**, just an accessor that
allocates once instead of five times. Worth recording as a caution against this document's own
framing: "move it to C" was right four times and wrong the fifth, where the answer was to stop
allocating.

## 4. The constraint that decides the rest: there is no FFI

**Ziman cannot call a library it was not compiled with.** There is no `dlopen`, no `dlsym`, no
libffi, nothing in `src/` that loads code at run time. Every native is hand-written C in
`src/native_*.c`, registered at startup, and linked statically; the binary's whole external surface
is libc, sqlite3, OpenSSL, zlib and pthreads.

This is why "just bind Qt" is not a small sentence. Binding any toolkit means **writing C for every
call you want to make**, by hand or by generator, and compiling it into `ziman`. It is a decision
about the interpreter's dependencies, not a library addition.

It also rules out the thing a scripting language would normally do here, which is to lean on an
existing binding ecosystem. There isn't one, and the first GUI binding would also be the first FFI
story.

## 5. What four families of toolkit decided

| | examples | widgets drawn by | what it costs |
| --- | --- | --- | --- |
| **Native-widget binders** | Qt, GTK, wxWidgets | the OS / the toolkit's own C(++) code | a large binding; you inherit look, IME and accessibility free |
| **Own-renderer, retained tree** | **Swing**, **JavaFX**, Flutter, Slint, Avalonia | the toolkit, onto one surface | you write everything; you look the same everywhere, native nowhere |
| **Own-renderer, immediate mode** | Dear ImGui, egui | the toolkit, rebuilt every frame | simplest API; accessibility and text are the known weak points |
| **Web in a window** | Electron, Tauri | a browser engine | huge dependency, or the system webview; the UI language is not Ziman |

What each actually contributes to this decision:

**Swing is the strongest evidence that route B works**, and it is routinely misremembered. Its
components are *lightweight*: Swing draws them itself, in Java, onto a small number of heavyweight
native windows, with a pluggable look-and-feel. That is exactly "compose a scene, rasterise it, blit
it", shipped in production for two decades. It also contributes the threading rule every toolkit
since has kept — all UI work happens on one thread (the EDT), and touching a component from another
is undefined. §9 adopts it.

**JavaFX contributes the architecture split this document recommends.** A retained scene graph in
the managed language, CSS for styling, observable properties for binding, and **a native rendering
pipeline (Prism) underneath**, because drawing pixels in Java was not viable either. Ziman's `Scene`
is already the top half of that. JavaFX also contributes a warning: it was removed from the JDK in
11 and lives as the separate OpenJFX project under GPL v2 with the Classpath exception — a GUI layer
that ships separately from its language drifts out of the default experience, which is an argument
for `lib/graphics`-adjacent modules living in this repository rather than beside it.

**Qt is the best toolkit here and the worst fit.** It is C++, not C, so a binding needs a C shim
over the C++ API before Ziman sees anything; it relies on its own code generator (`moc`) for signals
and slots; and its licensing is LGPL-or-commercial, where **whether static linking satisfies the
LGPL is genuinely disputed** — community discussion turns on whether a statically linked
application can honour the requirement that a user replace their copy of the library, with the
common advice being to link dynamically or buy a commercial licence. Ziman links everything
statically today. That is a legal question, not an engineering one, and it should be settled by
counsel rather than by a design document, but it is sufficient on its own to put Qt behind GTK.

**GTK is the plausible binder.** It is C, so no shim layer; it is LGPL and normally linked
dynamically; and crucially it is **described by machine-readable metadata** (GObject-Introspection's
GIR files) from which a binding can be *generated* rather than typed. That matters more here than
anywhere, because this project already generates code from declarations — `#[derive]`, the
repository generators, `meta`. A GIR-driven binding generator is a very Ziman-shaped answer to the
no-FFI problem.

**Flutter is route B taken to its conclusion**, and it prices it honestly: its own engine draws
every pixel, the UI is identical on every platform, platform-native styling takes deliberate effort,
and the engine is several megabytes in every binary. The recurring criticism — that it approximates
native widgets rather than using them — is the cost of this route stated by its users.

**The immediate-mode libraries contribute the one thing to avoid.** They are the most pleasant APIs
in the table, and state lives with the application rather than inside widgets, which suits a language
of immutable values well. But rebuilding the tree every frame makes accessibility hard: you cannot
destroy and recreate an accessibility tree each frame, because assistive technology needs to be told
*what changed* — that a row moved, not that everything is new. The known mitigation is a retained
tree underneath an immediate-looking API, which is to say: a retained tree. **AccessKit** exists
precisely for own-renderer toolkits, abstracts UI Automation on Windows and AT-SPI's D-Bus
interfaces on Unix, has been adopted by egui, and — decisively for us — **ships a C API covering
both its data structures and its platform adapters**. C is the only surface Ziman has.

**Electron and Tauri contribute the reminder that route C already works.** curly's frontend is React
served by Ziman's own HTTP server, and `curly serve` went in this week. For a large class of
applications that is not a compromise; it is the answer.

## 6. Three routes, and the recommendation

**Route A — bind GTK, generated from GIR.** Native widgets, native look, and §§10-11's hard parts
come free because the toolkit already solved them. Costs: a binding generator plus a hand-written
core (main loop, signal dispatch, and a bridge between Ziman's GC and GObject's reference counting,
which is where bindings like this usually crash); a new external dependency for the interpreter;
and widgets whose state lives in C, which sits awkwardly in a language of immutable values.

**Route B — own widgets on `lib/graphics`, with a renderer in C.** One codebase, one appearance
everywhere, and the widget set, layout and styling all in Ziman, where `Scene` and `layout` already
are. Costs: §3 says the renderer must be C, so this route *also* starts with a significant C
component — and then you still owe text editing, IME, accessibility, DPI, cursors, dialogs and
platform conventions, each of which took the toolkits in §5 years.

**Route C — a browser UI, served by `server`.** Works today. Zero new native code. The UI language
is TypeScript, not Ziman.

**The recommendation is A, phased, with C remaining the default answer for most applications and
`lib/graphics` keeping the custom-drawing job described in §8.**

The reasoning is §4 plus §§10-11 rather than taste. Route B's appeal is that Ziman already has the
drawing stack — but §3 removes exactly that advantage by moving the renderer into C, so B's starting
cost is similar to A's while its *total* cost is far higher, because it buys none of the OS
integration. Put plainly: the attractive half of B is already built and cannot be used for this, and
the unattractive half is years of work that a binding gets for nothing. A gets a usable application
soonest, and it is the route whose hardest problems are someone else's.

The case against A that must be stated: it makes the interpreter depend on a desktop toolkit, and a
generated binding is a large body of code nobody reads. §14 prices both.

## 7. Route A in outline

Four pieces, in dependency order:

**7.1 A GIR-driven generator.** GIR is XML, and `xml` is in the library, so the generator can be
Ziman reading GIR and emitting `src/native_gtk_*.c` plus `lib/gtk/*.zm`. It generates the subset a
declaration names rather than all of GTK: a whole-API binding is tens of thousands of functions and
almost all of them are never called. **This is the only part that must be designed carefully**,
because everything else is volume.

**7.2 The hand-written core.** The parts a generator cannot produce: entering and leaving the main
loop, dispatching a signal back into a Ziman closure, and the lifetime bridge. The last is the one
that bites — a Ziman closure held as a GTK callback is a GC root that C owns, and a GObject held by a
Ziman value is a reference C must drop when the value dies.

**7.3 Widgets as handles.** A `Widget` is an opaque handle, as `Socket` and `FontRef` already are, so
nothing pretends a GTK object is a Ziman value. Properties are functions, not fields.

**7.4 A drawing area that takes a `Scene`.** One native that hands an `Rgba8` buffer to a
`GtkDrawingArea`, which is how §8's custom drawing reaches the screen.

## 8. Where `lib/graphics` still earns its place

§3 says it cannot repaint a window at 60 fps. It is excellent at everything else, and a real
application needs that: charts, diagrams, report previews, a document view, an image editor's canvas.
**Those repaint on change, not on a clock.** Two seconds is wrong for a resize and fine for
rendering a chart once when its data arrives — and the same `Scene` exports to PDF, so a chart on
screen and the chart in the printed report are one description rendered twice. That is a feature
neither route A's widgets nor route C's browser gives for free.

## 9. Threading: one UI thread, which Ziman's model already wants

Every toolkit in §5 requires that UI work happen on one thread. Ziman's concurrency model fits this
unusually well, and the fit is worth stating because it was discovered the hard way elsewhere: a
`Mutable` **belongs to the thread that made it** and raises a run-time error if another touches it.
In a thread-per-request server that is a trap (it was, in curly's `server/api.zm`, where the fix was
a thread owning the value behind a channel). In a GUI it is the invariant the toolkit wants anyway.

So: the UI thread owns the widget tree and all mutable UI state; background work runs on `spawn`ed
threads and talks back over a `Channel`; and the main loop drains that channel as an event source.
Swing's rule, enforced by the language rather than by documentation.

## 10. Text input is the hard part, not keys

`terminal`'s `Key` decoder was 450 lines for escape sequences. A desktop text field is harder by a
large factor, and the reason is **input methods**: composing Japanese, Korean or Chinese text means a
pre-edit buffer the application must display with the right styling, a candidate window positioned
at the caret, and commit events that replace the pre-edit. Dead keys and compose sequences do the
same thing for Latin scripts with diacritics. Then: bidirectional text, grapheme-cluster-aware caret
movement, double-click word selection by locale rules, drag selection, the system clipboard with
multiple flavours, undo, and platform-specific key bindings a user expects to work.

Route A gets all of this. Route B owes all of it, and this section is why B's estimate in §14 is a
year rather than a quarter. Note that the toolkits that own their text engine report IME and keyboard
behaviour as the place where their own users notice the difference.

## 11. Accessibility, decided now or never

A UI that draws its own widgets is, to a screen reader, a blank rectangle unless it deliberately says
otherwise. Retrofitting this is notoriously harder than building it in, because the accessibility
tree has to mirror a widget tree that was not designed to be mirrored.

Route A inherits it. Route B must adopt **AccessKit** — the C API is the only reason B is possible at
all here — and must do it in the first widget, not the fiftieth.

Either way this is a decision, not a detail. A language whose GUI story cannot be used with a screen
reader has excluded people, and saying so in a design document is cheaper than discovering it in a
procurement questionnaire.

## 12. How it would be tested

This project gates what it builds, and `terminal` is the precedent: it could not be tested without a
pty, so a pty harness was written and made a gate.

- **The renderer**: render a `Scene` through the C renderer and through `raster.zm` and compare
  pixels. `image.encode_png` already exists, and `raster.zm` is the reference implementation. This is
  the strongest gate available and it costs almost nothing.
- **Layout and widget logic**: pure functions over a tree, unit-testable with no window — the same
  split curly's TUI used, where `tui/rows.zm` and `tui/render.zm` are pure and only `tui/app.zm`
  touches the terminal.
- **Interaction**: synthetic event injection into an offscreen window, asserting on the resulting
  scene rather than on screenshots.
- **The binding generator**: generate, compile, and call one function of each shape.

## 13. Non-goals

- **Mobile.** Android and iOS are different enough that pretending otherwise distorts the design.
- **A visual designer or a markup dialect.** Widgets in code first; FXML and QML both came later.
- **Theming to pixel parity with each platform.** Route A gets the platform's own widgets; route B
  should not promise an imitation it cannot keep.
- **GPU acceleration.** Software rendering onto a window is enough for application UI, and a GPU
  path is a later optimisation with its own portability story.
- **Replacing route C.** An HTTP server plus a browser remains the right answer for many
  applications and is the only one that works today.

## 14. The cost

| | new C | new Ziman | elapsed |
| --- | --- | --- | --- |
| Route A, a usable subset (window, layout containers, buttons, entries, lists, menus, dialogs) | 2-4k, mostly generated | 2-3k | months |
| Route B, to the same point | 3-5k (renderer, windowing, IME, AccessKit) | 8-12k (widgets, text editing, styling) | a year or more |
| Route C | 0 | 0 | works today |

For comparison: `lib/graphics` is 6,937 lines and is one layer of route B.

The number that should decide is not in the table. **Route A's hard problems are already solved by
somebody else; route B's are not.**

## 15. Open questions

1. **A or B?** §6 recommends A; the case for B is that it keeps the whole stack in Ziman and makes
   the language's own drawing model the UI model, which has a coherence A never will. This is the
   decision the document exists to put.
2. **If A: GTK 4 only, or also a Windows and macOS story?** GTK runs on all three and looks native on
   one. A binding per platform is three bindings; a cross-platform binder that looks native nowhere
   is route B with extra steps.
3. **Does the interpreter take a desktop dependency at all,** or does a GUI build become a separate
   target the way the sanitizer builds are? A `ziman` that will not start without GTK installed is a
   worse command-line tool.
4. **Should the C renderer of §3 happen regardless of route?** It would make `lib/graphics` fast
   enough for interactive document views and large exports, and it is the one piece both A and B
   want. It may be the right first task whatever is decided here.
5. **Is there a fifth route — a Ziman-to-the-browser story that is not TypeScript?** Route C's only
   real defect is that the UI language is not Ziman. That is a different document.
6. **What does route A do about `Scene`?** §7.4 blits an `Rgba8` buffer, which §3 says is slow to
   produce. A GTK drawing area could instead be handed a native rendering of the same `Scene`, which
   is question 4 again from the other side.

## 16. Sources

External facts above were checked rather than recalled; where the public record is thin or
contested, the text says so.

- Qt licensing and the static-linking question: [Qt forum](https://forum.qt.io/post/560435),
  [Qt Centre](https://qtcentre.org/threads/29551-Commercial-license-vs-LGPL-license?p=138692),
  [static linking without a commercial licence](https://forum.qt.io/post/395602),
  [SD Times on Qt's licensing change](https://sdtimes.com/?p=16727). **Community opinion, not
  counsel** — no official 2026 term was located, and the threads disagree.
- JavaFX as a separate project, its release line and licence:
  [OpenJFX release notes](https://docs.oracle.com/en/java/java-components/javafx/25/release-notes/javafx-release-notes.pdf),
  [proposed JavaFX 26 schedule](https://mail.openjdk.org/pipermail/openjfx-dev/2025-December/058321.html),
  [JavaFX links, July 2026](https://foojay.io/today/javafx-links-of-july-2026/),
  [Debian package tracker](https://tracker.debian.org/pkg/libopenjfx-java).
- Immediate versus retained mode, and the accessibility objection:
  [Dear ImGui on immediate mode](https://www.mintlify.com/ocornut/imgui/core-concepts/immediate-mode),
  [immediate versus retained, compared](https://instagit.com/ocornut/imgui/immediate-mode-gui-vs-retained-mode-gui/),
  [why immediate mode (Rust forum)](https://users.rust-lang.org/t/graphics-why-immediate-mode/93356),
  [performance is not the deciding factor](https://forrestthewoods.com/blog/proving-immediate-mode-guis-are-performant).
- AccessKit's scope, platform adapters and C API:
  [crate documentation](https://docs.rs/crate/accesskit/latest),
  [README](https://docs.rs/crate/accesskit/0.17.0/source/README.md),
  [project overview](https://rustrepo.com/repo/AccessKit-accesskit-rust-gui);
  AT-SPI and ATK background: [Fedora package notes](https://packages.fedoraproject.org/pkgs/at-spi2-core/atk/).
- Owning the renderer, and what it costs:
  [Flutter's rendering engine](https://dev.to/oliviasparker/flutters-rendering-engine-explained-and-why-it-changes-how-you-think-about-cross-platform-3nn8),
  [why Flutter does not use OEM widgets](https://medium.com/@xster/why-flutter-doesnt-use-oem-widgets-94746e812510),
  [text as cross-platform UI's hardest problem](https://dev.to/pickuma/native-all-the-way-until-you-need-text-cross-platform-uis-hardest-problem-53pi).
- Windowing and input libraries, including SDL3's input-method handling:
  [Dear ImGui platform backends compared](https://instagit.com/ocornut/imgui/imgui-platform-backends-comparison/),
  [SDL versus GLFW](https://gamedev.net/forums/topic/681518-sdl-glfw-what-are-some-reasons-to-select-one-over-the-other/).
