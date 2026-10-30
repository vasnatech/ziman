# Ziman — Roadmap

Not commitments, not ordered — ideas and known gaps to pull from as v0
proves out. See docs/design/journal/ for the principles these should be judged
against before adding any of them.

**Each entry carries its own status after a `--`**: `not started`, `done`, `first step done`, `postponed`.
That status is the most perishable thing in the file, and nothing checks it. Closing a task in
docs/reviews/tasks/ therefore means searching here for what it touched: on 2026-10-07 four entries were
wrong at once because the work had been done and this file not revisited — a per-IP throttle still listed
as needing a `net.peer_address` that existed, a connection cap still listed as blocked on two things that
had landed, metrics listed as not started with half of it shipped, and a superseded collector design still
pointing a reader at the path that was measured and abandoned.

That is not fixable by a script, and it was tried: `tools/check_not_yet.py` catches a stale "Not yet" when
it names an identifier that is now `pub`, and pointing it at this file produced **40 findings, nearly all
wrong** — an entry like "HTTP/2 -- not started" is followed by a paragraph mentioning `listen_tls`,
`byte_and` and a dozen other real things, none of which it claims are missing. The check works on a terse
`Not yet: a, b, c` list and this file is discursive prose, so the guard here is the habit, not a tool.

**What goes stale, learned 2026-10-16 over two sweeps.** The first sweep was prompted by this file
recommending two *finished* items in one sitting, and found four stale claims all in the North Star
section -- the `io` module (replaced by `fs` long ago), "Still missing: `replace`, and a general
`index_of`/`find`" (all three are builtins), the unbounded queue (bounded channels landed
2026-09-24, which R10's own entry already recorded) and generics "in progress, staged struct → enum
→ function" (all five stages shipped 2026-10-10). That looked structural, and the conclusion written
here was that the North Star prose is where the rot lives because it was written once at the start.

**That was too narrow, and the next question asked of this file found four more elsewhere** -- in
"Larger, needs its own design pass": the `Cell<T>` entry (built 2026-10-14 and its headline never
said so), the `collections` refactor (raised and overtaken the same day, F120 at 00:17), the LLM
benchmark (its own body said "done 2026-10-10" two lines above a sentence still calling the
directory a 10-task set) and the memory entry, whose headline stopped being true on 2026-09-29.
"Near-term, likely next" remains the one section with nothing wrong in it.

**The sharper pattern, and the one to check for:** in all four of the second batch the *body* had
been updated and the **headline had not**. That is the more dangerous shape, because an entry whose
body carries a dated "*Update ... done*" note looks maintained, while the headline is the only part
a reader scans when asking what is left -- so a half-revisited entry misleads more reliably than an
untouched one. Closing a task therefore means fixing the status *in the headline*, not only adding
a note underneath, and reading an entry means reading past its headline before trusting it.

A check for exactly this was prototyped and **dropped**: flag an unstruck item whose body says
something landed while its headline carries no status word. After the fixes above it reported three
items, all false positives -- `Everything above the native floor is real Ziman code`,
`Generics: the "just hardcode it" strategy stopped scaling` and `First-class functions / closures`
are topics and principles, not status claims, and headlines like those are common here. Making it
sound would mean requiring every headline to carry a status, which is the convention this preamble
states and dozens of entries do not follow. So this stays in the habit column too, with the
narrower "Still missing" shape being the only part that mechanized (see tools/check_not_yet.py).

That sweep also found the opposite of the 40-false-positive result above, in one narrow case:
`check_not_yet.py` now has a "Still missing" shape, which works for the same reason the "Not yet"
one does -- it reads only the terse list after the marker, inside one item, skipping any item
carrying an Update note. It caught the `replace`/`index_of` clause and reports nothing else in the
file. It also learned that a **builtin** is neither a `pub fn` nor a module, which is why `replace`
was invisible to every earlier shape. That does not reopen the preamble's conclusion: the discursive
prose is still unguardable, and three of the four stale claims above are things no script will find
-- one names no identifier at all, and another names `send`, which exists and always did.

**Opening a task carries the same obligation, which 2026-10-08 is the first demonstration of.** The C
implementation review (docs/reviews/c-implementation-review-2026-10-08.md) opened twelve, and four entries
here read optimistically because of them -- the regex engine's "linear-time", the formatter, the collector's
"does not leak" and the metrics module's lock-free update. Each now carries a dated note pointing at the task,
rather than being quietly contradicted by a file in the same directory. A fifth, platform support, gained a
cheaper first step from the same review instead of a correction.

## The parts

- [North star: write (a slice of) the health-record backend in Ziman](north-star.md)
- [External review follow-ups (2026-09-23)](external-review-follow-ups.md)
- [Near-term, likely next](near-term.md)
- [Larger, needs its own design pass](larger.md)
- [Explicitly out of scope for now (see docs/design/journal/ "Non-goals")](out-of-scope.md)
