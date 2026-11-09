# Terminal: raw mode, one key at a time, and putting back what we changed

Status: **built, 2026-11-09.** `lib/core/terminal.zm`, `src/native_terminal.c`,
`tests/stdlib/terminal_test.zm` (31 checks), six pty cases in `tests/run_terminal_test.sh`, and
§9's `io.stdin()`. All five decisions in §3 survived contact; **§7's sketch did not**, and §7 now
says why -- it was wrong about the language rather than about the terminal.

**§4's "four functions" is now five**, and §8's "no colour or cursor helpers" grew the Ziman half it
said it would — see §4.1 and §8.1. Neither is a reversal: the fifth native exists to give the
alternate screen decision 3.1's guarantee, which is the decision that drove the whole module.

Two of §11's open questions were settled on the day this was written: **`with_raw` is the public
shape** (§6, decision 3.5) and **`io.stdin()` went first** (§9). A third, the module's name, went
to `terminal` as §11 leaned.

## 1. The gap was found by an application, not by a wishlist

The curly port spent several days exercising this library against a real program, and it surfaced
three gaps in order of increasing depth. `client` had no multipart builder, so one got written by
hand in the application. `bytes_to_string` is strict with no lossy sibling, so a UTF-8 replacement
decoder got written by hand in the application. Both of those are **library** gaps: a Ziman program
could close them itself.

The third cannot be closed from Ziman at all. **Nothing in this language can read standard input.**
`io` is a parsing cursor over a stream somebody else opened; `os` can `write_stderr` and has no
read; `process.start` redirects a child's stdio to **files**, not pipes, so a Ziman program cannot
even borrow another program's terminal handling. `curly collections import curl` needs the command
on stdin when it is not given as an argument, and the only trick available was
`fs.read_text("/dev/stdin")` — which works solely because that input ends.

So today no interactive program can be written in Ziman. Not a TUI, not a password prompt, not a
`read -p`, not a confirmation. That is a platform gap, and this document is about the smaller half
of it.

## 2. What already exists makes this smaller than it sounds

The 2D stack is complete and shipped: `geom`, `shape`, `paint`, `scene`, `stroker`, `font`,
`truetype`, `image`, `raster`, and a `layout` engine with paragraphs, grids and tables.
`raster.render(Scene, Options) -> image.Image`. And `server`, `rest`, `derive_routes` and `http`
mean a browser front end is already possible.

**So Ziman can already draw and already serve. The only missing half of a user interface is input
and terminal control** — which is four functions, not a toolkit. That is the whole argument for
doing this rather than something larger.

## 3. Five decisions taken before the first line

**3.1 The terminal is restored however the program ends.** This is the one that must not be got
wrong, and it is a decision rather than an implementation detail: a program that dies in raw mode
leaves the user's shell with no echo and no line editing, and they have to type `reset` blind. That
failure is worse than the crash that caused it. The saved `termios` is restored from an `atexit`
handler registered the first time raw mode is entered, and from the existing shutdown handler's
path.

**3.2 The C half hands over bytes; Ziman decodes keys.** An escape sequence is a table, it is the
part most likely to grow as terminals disagree, and it is testable from a byte array with no
terminal attached. Putting it in Ziman keeps the native surface at four functions and puts the
fiddly half where `ziman test` can reach it.

**3.3 Linux and macOS. Windows is an honest error.** No native in this tree carries a `_WIN32` or
`__APPLE__` guard today, and `os.zm` already sets the precedent for a platform-shaped feature: the
machine figures come from `/proc`, and elsewhere they are
`Err("not supported here: ...")`, **never a made-up number**. `termios` is POSIX, so Linux and macOS
come together; Windows needs a different API entirely and says so rather than pretending.

**3.4 A resize is polled, not delivered.** `os.install_shutdown_handler()` plus
`os.shutdown_requested()` is the shape this language already uses for a signal: the handler sets a
flag and the program asks at a moment of its choosing. A render loop asks `size()` per frame anyway,
so `SIGWINCH` buys nothing a poll does not — and a callback from a signal context is a different and
much worse conversation.

**3.5 Raw mode is entered through a scope, not a pair of calls.** `with_raw(body)` is the public
shape; the `set_raw` pair stays underneath it, because the native needs it and because the scope is
built out of it. §6 is the argument, and the short version is that 3.1 is only half a decision
without this one: `atexit` covers a program that *ends* in raw mode, and nothing but a scope covers
a program that keeps running after an early `return` between two calls.

## 4. The native surface: four functions

```
native fn is_tty() -> bool
native fn size_raw() -> Result<[int], string>              // [rows, cols]
native fn set_raw(on: bool) -> Result<unit, string>
native fn read_raw(timeout_ms: int) -> Result<[byte], string>   // empty on timeout
```

**`is_tty` comes first because a program must be able to refuse.** A TUI whose stdin is a pipe
should say so, not render a frame into a file. Everything else here is undefined when this is false.

**`size_raw` answers `[rows, cols]` because Ziman has no tuple type.** A `Size { rows, cols }`
struct is the better public shape and belongs in the Ziman half; the native crossing stays a plain
array, as `os.load_average` already returns `[float]`.

**`read_raw` takes a timeout, and that is load-bearing twice.** A render loop must not block
forever, or a redraw on resize never happens. And the ESC ambiguity cannot be resolved without one:
`ESC` alone and the first byte of `ESC [ A` are the same byte, so the decoder reads again with a
short timeout and treats nothing-arrived as a real Escape. An empty result means the timeout
expired, which is not an error — `Err` is reserved for a read that failed.

**`read_raw` answers bytes, not characters.** A UTF-8 character and an escape sequence both arrive
in pieces, and a reader that tried to answer characters would have to buffer and decode — which is
§3.2's job, in Ziman.

**Built: `read_raw` answers exactly one byte**, which the signature above does not say and which is
a decision. §5 gives the public reader as `read_key(timeout_ms) -> Result<Option<Key>, string>` —
no reader object, no module state — and that signature is only honest if a read never returns bytes
the decoder did not ask for. A bufferful would leave the tail of a burst *somewhere*, and
"somewhere" is module-level mutable state in a module that otherwise has none. It costs a terminal
nothing: the continuation bytes of a character and the rest of a sequence are already in the
driver's buffer by the time the decoder asks, so those reads do not wait. A thousand-character paste
is a thousand syscalls, and bracketed paste is a non-goal.

### 4.1 The fifth native: `set_screen`

```
native fn set_screen(on: bool) -> Result<unit, string>
```

**It is here for decision 3.1, not for the sequence.** A Ziman caller can write `ESC [ ?1049h`
itself — `terminal.enter_alternate_screen()` answers exactly that string, and §8.1's helpers are
public. What a caller cannot do is be undone by an `atexit` handler or a signal handler, and that is
the whole difference: **a program that dies with the alternate screen up leaves the user staring at
a dead frame**, the identical failure 3.1 is about and as bad as a shell with no echo.

So `with_screen(body)` is the public shape, exactly as `with_raw` is, and the C half's `restore_now`
writes `ESC [ ?1049l ESC [ ?25h` before it restores the `termios`. `write` is async-signal-safe
where `printf` is not, so the sequence goes out as bytes with no stdio in the way.

**Separable from raw mode, and that is why it is a second flag rather than a mode of the first.** A
password prompt wants raw mode and not the alternate screen; a pager wants the reverse; a
full-screen TUI nests both in either order. Each tracks itself and the restore undoes whichever is
still on. One consequence worth writing down: `claim_fatal_signals` had to become **idempotent**,
because both scopes call it — the first version cleared its record of an earlier claim, so leaving
one scope would have left our handler installed with no way to put the original back.

**What `with_screen` does not cover is `SIGKILL`**, the same single hole decision 3.1 has.

## 5. The Ziman half: `Key`, and a decoder that loses nothing

```
pub enum Key {
    Char(string)        // one character, possibly multi-byte
    Enter
    Tab
    Backspace
    Escape
    Delete
    Up    Down    Left    Right
    Home  End     PageUp  PageDown  Insert
    Function(int)       // F1..F12
    Ctrl(string)        // "a".."z"
    Unknown([byte])     // a sequence this table does not know
}
```

**`Unknown` carries the bytes rather than being dropped**, for the reason `Json`'s unknown-tag
errors name the tag: a key this table has not met yet is a gap in the table, and a program that
logs the bytes can have it fixed. Silently swallowing input is how a terminal feature becomes
impossible to diagnose from the outside.

The public reader sits on top:

```
pub struct Size { rows: int  cols: int }

pub fn size() -> Result<Size, string>
pub fn read_key(timeout_ms: int) -> Result<Option<Key>, string>    // None on timeout
```

## 6. The question that mattered: a pair or a scope? — a scope

§3.1 says the terminal is always restored, and `atexit` covers the process ending. It does not cover
a program that *keeps running* after it is done with raw mode — a TUI that drops to a shell prompt,
or a one-off that asks for a password and then prints a report.

Ziman has no `defer` and no destructors, so the choice is:

```
// (a) a pair, and the caller is responsible
try terminal.set_raw(true)
...
try terminal.set_raw(false)

// (b) a scope, and the module is responsible
try terminal.with_raw(fn() -> Result<unit, string> {
    ...
})
```

**(b) is the one I would take**, because (a)'s failure mode is the whole problem in §3.1 and no
amount of documentation prevents an early `return` between the two calls. A closure cannot leak the
mode: the module restores it on the way out whether the body answered `Ok`, answered `Err`, or
propagated one. The cost is that the body cannot `return` out of the enclosing function, which in a
render loop is no cost at all.

Worth noting it is not either/or — `with_raw` can be the public shape with `set_raw` underneath it,
exported or not.

**Decided 2026-11-09: (b), as decision 3.5.** `with_raw` is what a caller reaches for; `set_raw`
stays as the native the scope is built from. Whether it is `pub` is left to the module: the honest
test is whether anything in §7 needs it, and if nothing does it stays private rather than being
exported for a hypothetical caller who would be taking on exactly the risk the scope removes.

## 7. What it has to make writable (the only real test of the API)

The sketch this section was written with **does not compile**, and finding that out is the most
useful thing building the module did. It was:

```
switch key {
    Char("q") { running = false }      // a literal in a payload pattern
    Up { select_previous() }
    ...                                 // and the rest left out
}
```

Two language rules it walked into, both in docs/syntax/functions-and-types.md §`switch`:

* **A payload pattern binds a name; it cannot match a literal.** `Char("q")` is a *syntax* error
  ("expected a name for the variant's payload"). It has to be `Char(c) { if c == "q" { .. } }`.
* **There is no default arm, by design** -- "it would bring back the silent case this exists to
  remove" -- so a `switch` over `Key` needs **all eighteen arms**, every time. The `...` in the
  sketch was not shorthand for tedium; it was shorthand for something the compiler refuses.

That is right for an enum a program consumes exhaustively and wrong for the way a TUI reads keys,
which is "I care about six of these". So the dispatch is the per-variant predicate the same section
blesses (`if Shape.is_Circle(s)` remains fine for a single-variant test):

```
val outcome = terminal.with_raw<unit>(fn() -> Result<unit, string> {
    var running = true
    while running {
        val screen = try terminal.size()
        draw(screen)                                   // escape sequences, written with `print`
        switch try terminal.read_key(50) {
            Some(key) {
                if terminal.Key.is_Up(key)        { select_previous() }
                if terminal.Key.is_Down(key)      { select_next() }
                if terminal.Key.is_Enter(key)     { send_selected() }
                if terminal.key_name(key) == "q"  { running = false }
            }
            None {}                                    // timed out; redraw, in case of a resize
        }
    }
    return ok(unit)
})
```

**This is why `key_name` is in the module rather than in an application.** It was written as a
helper for a help line, and it turns out to be the practical way to ask "was that `q`?" -- one call
instead of `switch`-and-compare. A `tui` module (§8) would wrap this into a key-binding table; until
one exists, the predicates and the name are the API.

Nothing in the corrected sketch needs a fifth native, which was the question this section was for.

## 8. Non-goals

* **No widget toolkit.** Boxes, lists, text fields and focus are an application's business until
  two applications want the same ones; then they are a `tui` module written entirely in Ziman on
  top of this one.
* **No mouse.** It is another escape-sequence family and another mode to enable and restore.
  `Unknown` means a mouse report will not crash a program that did not ask for one.
* **No terminfo or termcap.** The sequences xterm and its imitators send are hardcoded. A database
  lookup is a dependency and a file-format parser for a problem nobody in this tree has yet.
* **No colour or cursor helpers in the native half.** Output is `print` with escape sequences;
  constants and small helpers can live in Ziman later, and need no C. **They do now — see §8.1.**
* **No Windows**, per §3.3.
* **No `read_line`.** See §9 — it is a different gap.

### 8.1 The painting helpers, which are the "later" above

`at`, `clear_screen`, `clear_to_line_end`, `hide_cursor`/`show_cursor`,
`enter_alternate_screen`/`leave_alternate_screen`, `Color` with `fg`/`bg`, and
`bold`/`dim`/`reverse`/`reset_style`. All in Ziman, all pure, no C. Three things about them:

* **They answer strings rather than writing.** A renderer calling a `move_to` that wrote and then a
  `print` that wrote would hand the terminal a frame in dozens of pieces, and the user would watch
  it arrive. Composing the whole frame and writing once is what makes a paint look instantaneous.
* **They are functions, not `pub val` constants,** which is forced rather than chosen: a module's
  `pub val` cannot be reached as `terminal.name` from another module, so a constant exported that
  way would be unusable by the only callers it has.
* **Eight colours and `Default`, no 256-colour or true-colour.** A different sequence family, not
  universally supported, and a TUI that needs them can write its own escape — the same restraint as
  "no terminfo" above.

**Two gaps they ran into, both worth knowing before writing any escape sequence in Ziman:**

1. **There is no way to write a control character in a string literal.** `src/lexer.c` takes `\n`,
   `\t`, `\r`, `\"` and `\\` and nothing else — no hex and no unicode escape. ESC comes from
   `bytes_to_string([27b])`, computed once into a module-level `val` (which does work, and is
   evaluated once).
2. **There was no `os.write_stdout`.** `print` appends a newline, which per frame scrolls the screen
   out from under the thing being drawn, so a TUI could not paint at all. Added symmetric with the
   existing `os.write_stderr`, including going through the same stdio stream `print` uses — a native
   writing to descriptor 1 directly would interleave wrongly with a buffered `print`.

## 9. The other half of the gap, which is not this module

There are **two** missing things, and conflating them would make this module the wrong shape.

1. **Interactive terminal input** — this document.
2. **Plain standard input as a byte stream** — for a pipe, which has nothing to do with a terminal.

curly needs (2) today: `collections import curl` with no command argument reads the command from
stdin, and it currently reads `/dev/stdin` as a whole file because there is no alternative. That
works for input that ends and is wrong for anything else.

(2) is smaller and belongs elsewhere: an `io` source, something like `io.stdin() ->
Iter<Result<[byte], IoError>>`, which then composes with `io.text`, `io.lines` and `io.cursor`
exactly as `fs.chunks` already does. **One native, one line in `io.zm`, no new module** — and it
closes the `/dev/stdin` hack properly. It is worth doing first, because it is the cheaper half and
the one an existing program is already working around.

**Built 2026-11-09, and the estimate held**: `src/native_io.c` with one function, `io.stdin()` in
`lib/core/io.zm`, the include and the register call in `src/main.c`. `io` was already in
`CORELIB_MODULES` and in `corelib.c`'s table, so four of §10's six edits did not apply.

Three things it taught, each one a smaller version of something this module will hit:

* **The never-empty-chunk rule has to be kept in Ziman.** A native cannot build an `Option`, so the
  C half says end-of-input with an empty `Ok` and `io.stdin()` turns that into the `None` the
  convention promises. That is decision 3.2 in miniature, and it is why the C half of *this* module
  can hand over bytes without knowing what a `Key` is.
* **One `read(2)`, not a loop that fills the buffer.** A pipe hands over what has been written and a
  terminal a line at a time, so filling 64 KiB first would make an interactive reader wait for 64
  KiB of typing. `read_raw`'s timeout in §4 is the same property stated as a deadline.
* **The test suite could not feed standard input at all**, and worse, a fixture inherited the
  *runner's* stdin — so a program that read it would have consumed the suite's own input or blocked
  on a terminal. `tests/run_tests.sh` now takes a `<name>.in` and gives every other fixture
  `/dev/null`. This module's decoder needs no terminal (§3.2, §10), so it needs nothing further
  here; but `is_tty()` answering `false` under the suite is now a fact rather than a hope.

## 10. The cost

Adding a core module is six edits, and the Makefile documents five of them:

* `lib/core/terminal.zm` — the `native fn` declarations, `Key`, the decoder, `with_raw`, `size`.
* `src/native_terminal.c` and `.h` — four functions, the saved `termios`, the `atexit` handler, and
  `native_terminal_register()`.
* `src/main.c` — the include and the `native_terminal_register()` call.
* `Makefile` — `terminal` added to `CORELIB_MODULES`.
* `src/corelib.c` — one line in the `CORE_MODULES` table.
* `docs/lib/terminal.md` and `docs/lib/examples/core/terminal_example.zm` — every one of the 64
  modules has both, and `make check` has targets that notice when one is missing.

`src/*.c` is wildcarded into the build, so there is no object-list edit.

The decoder is the only part with real test surface, and it needs no terminal: feed it byte arrays
and assert the `Key`. That is the argument in §3.2 paying off.

**Built: the six were seven.** `src/corelib.c` needs *two* edits, not one — the line in the
`CORE_MODULES` table that the Makefile's comment names, and an `#include "terminal_source.h"` for
the generated header, which it does not. The build fails with `TERMINAL_SOURCE undeclared ... did
you mean TEMPLATE_SOURCE?`, a good error pointing at the wrong fix. Worth correcting in the
Makefile's comment the next time something is added there.

**What a terminal-shaped module needs beyond the six.** The decoder is testable with no terminal and
that covers the table, but it covers none of the half decision 3.1 is about. Raw mode, the restore
and `read_key`'s assembly of a real burst were driven through a **pty** from Python (`pty.fork`,
`TIOCSWINSZ` for a known size, `tcgetattr` on the master to read the mode back). Four cases, all
passing:

| what ends the program | restored by | result |
|---|---|---|
| the scope returns, program keeps running | `with_raw` | `echo/canon` **at a mark printed while still running** — the §6 case, which `atexit` would not have caught |
| normal exit in raw mode | `atexit` | restored |
| a fatal Ziman error in raw mode | `atexit` | restored, and the stack trace prints readably (OPOST back) |
| `SIGTERM` in raw mode | the signal handler | restored, and the process still dies of signal 15 |

The fourth is the one `atexit` cannot do, and the reason the C half claims terminating signals —
**but only those still at `SIG_DFL`**, so a program that called `os.install_shutdown_handler()`
keeps its own drain and reaches `atexit` the ordinary way. `SIGKILL` remains unfixable, and is the
one honest hole in decision 3.1.

**And it is a gate, not a one-off.** `tests/terminal_pty.py` plus `tests/run_terminal_test.sh` run
those four cases from `tests/run_tests.sh`, the way `tests/run_shutdown_test.sh` already runs the
real-signal tests — the same precedent and the same PASS/FAIL format. A system with no pty prints
`SKIP` rather than failing. The restore comparison was checked against the harness's own raw sample
to make sure it is not vacuous: it rejects the raw settings and accepts the restored ones, and
breaking one expected key takes the whole suite to exit 1.

## 11. Open questions

1. ~~**`with_raw` or `set_raw`, or both?**~~ **Answered 2026-11-09**: `with_raw` is the public
   shape, `set_raw` is underneath it. It became decision 3.5, because it constrains every signature
   in §4 the way the other four do.
2. ~~**Does `io.stdin()` (§9) go first?**~~ **Answered 2026-11-09: yes, and it is built.** §9 has
   what it cost and the three things it taught.
3. ~~**`terminal` or `tty`?**~~ **Answered 2026-11-09: `terminal`**, as this leaned. The module is
   about more than the device: `Key`, the decoder and `key_name` are not tty concepts.
4. ~~**Is `Function(int)` worth it?**~~ **Answered 2026-11-09: yes, and it cost less than
   feared** — three table rows (SS3 `ESC O P`-`S`, the Linux console's `ESC [ [ A`-`E`, and the
   `ESC [ <n> ~` numbers), not a dozen. The numbers have historical gaps at 16 and 22, and the table
   leaves them `Unknown` rather than guessing which F-key a terminal meant.
5. **Bracketed paste?** A TUI that accepts pasted text wants to know a burst is a paste and not
   typing. It is one more mode to enable and restore, and it can wait for an application that wants
   it — but it is the first thing I would add after mouse.
6. **What happens to raw mode across `process.run`?** A child that writes to the same terminal while
   the parent holds raw mode will look wrong. Probably the application's problem, but worth a
   sentence in the module's own docs. **Still open** — nothing built so far runs a child.

7. **New, from building it: should `Key` carry modifiers?** The table answers `Unknown` for
   `ESC [ 1;5A` (Ctrl-Up) rather than `Up`, because a shortcut bound to Up must not fire on a key
   the user did not press — see §7. That is correct and it is also a gap: Ctrl-Up is a key people
   bind. Adding a `Modified { key: Key, ctrl: bool, alt: bool, shift: bool }` variant would mean
   every caller's `is_Up` check misses the modified form, which is the kind of change that wants an
   application asking for it first.
