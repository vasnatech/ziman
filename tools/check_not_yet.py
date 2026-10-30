#!/usr/bin/env python3
"""Catches a doc claiming a feature is missing once it actually exists.

Three shapes, all of them "the docs say Ziman cannot do X" where the code says otherwise. The
second was added on 2026-10-09 after the North Star section of docs/roadmap/ was found still
listing "No map/dictionary type. Only `[T]` exists" among the things standing between the
language and a backend -- long after `Map<K, V>` and `Set<T>` became built-in types. The entry
two lines below it, for `Option<T>`, had been struck through properly, which is the only reason
the stale one was visible at all: by contrast, to a human reading the file. Nothing checked it.

The language review found this exact mismatch once already: docs/syntax/ said "Not yet:
server-side TLS (`listen_tls`)" after `lib/core/net.zm` had already grown a `pub fn listen_tls`
(the doc was written first and never revisited once the feature landed). This script guards
against that specific regression, mechanically: it collects every `pub fn`/`pub native fn`/
`pub struct`/`pub enum`/`pub val` name declared anywhere under lib/, then scans docs/syntax/
and every lib/**/*.zm doc comment for a "Not yet: ..." phrase and flags one that names an
already-`pub` identifier in backticks.

It only catches this one shape of drift -- a "Not yet" list naming a specific `pub` identifier
that already exists. A "Not yet: HTTP/2, multipart, compression" clause (prose, no backtick'd
name that could ever collide with a real declaration) is unverifiable this way and correctly
never flagged; that class of staleness still needs a human doc sweep, as F5's own audit was.

A "Not yet" inside double quotes is skipped: that is someone citing a wording (this file's own
docs/design/journal/ entry quotes the stale sentence that motivated the check), not claiming it is still true.

Scans source only: docs/syntax/ and docs/design/journal/ (hand-written) and lib/**/*.zm (the doc comments
docs/lib/*.md and docs/CORELIB.md are generated from) -- not the generated .md files
themselves, since they can only ever repeat whatever the source already says.

**Shape three: a "Still ahead" list in docs/roadmap/**, added 2026-10-09 after the North Star
section was found still describing `fs` without directory listing or append, `net` and `http` as
unwritten, and the pure-Ziman-versus-native database question as open -- all of it shipped, some of
it months earlier. It needed its own vocabulary because such a list names *modules*, and a module
is not a `pub fn`: `net` exists because `lib/core/net.zm` does, which is what the check now reads.
An item carrying an "*Update <date>:*" note is skipped, because annotating a stale sentence in
place rather than rewriting it is this file's own convention for recording a resolution -- without
that, the check would fire precisely on the entries someone had fixed properly.

**Shape two: an absence claim in docs/roadmap/.** A list item whose bold headline starts with
"No " is a claim about what the language lacks, and it is flagged when the headline names
something that now exists: a backticked `pub` identifier, or one of the built-in parametric types
read straight out of src/typecheck.c's HARDCODED_GENERIC_TYPES -- so a sixth built-in type is
learned from the source rather than from a list kept here.

Only the **headline** is read, never the item's body. That is what makes striking an item through
the right fix rather than a way to hide from the check: the resolution text under a struck item
names the very thing the headline denied (line 88 now opens with "`Map<K, V>` and `Set<T>` are
built-in types now"), and a body scan would flag every correctly-resolved entry.

A bare type name is only matched when it is backticked, capitalised, or the headline also contains
the word "type" -- otherwise a future "No result caching" entry would trip over `Result`. That
deliberately narrows it: "No optional/nullable type" would not be caught, because the word is
"optional" and the type is `Option`. Like shape one, this catches one shape of drift and is not a
substitute for reading the file. docs/roadmap/ only -- docs/design/journal/ is a dated log whose "No X"
sentences are true of their date by design, so scanning it would report history as staleness.

**Shape four: a "Still missing" list in docs/roadmap/**, added 2026-10-16 after the North Star
section was found still listing `replace` and `index_of` among what the language lacked -- both
shipped. (The first sweep read that as the North Star prose being the stalest part of the file;
asking the file one more question turned up four more stale entries in "Larger, needs its own design
pass", so the pattern is not sectional -- see docs/roadmap/'s own preamble for what it actually is.)

It needed one new vocabulary of its own: a **builtin** is not a `pub fn` either. `replace` and
`index_of` are registered in C (`native_register("replace", ...)` in src/prelude.c) or dispatched
from src/interpreter.c's name table, so every check here read straight past them -- they exist in
neither lib/ nor as a module. Both tables are read from the source for the same reason
HARDCODED_GENERIC_TYPES is: a new builtin should not need an edit in this file to be known about.
Builtins are folded into the identifier index every shape here uses, not just this one.

What this still cannot see, and what the 2026-10-16 sweep had to find by hand: a claim naming no
identifier at all. "Unbounded queue -- `send` never blocks in v0" names `send`, which exists and
always did; what changed is that `channel_new(8)` now bounds it. And "in progress, staged struct ->
enum -> function" names nothing, while every stage it refers to is struck through further down the
same file. Both were stale for weeks. A rule for either would have to compare a claim against
another *entry* rather than against the source, which is a different kind of check and is not
attempted here.

Run directly, or via `make not-yet-check`. Exits 1 and prints each mismatch found; exits 0
("no stale 'not yet' claims found") otherwise.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PUB_DECL = re.compile(r"^pub (?:native )?(?:fn|struct|enum|val) ([A-Za-z_][A-Za-z0-9_]*)")
NOT_YET = re.compile(r"Not yet\b[:,]?\s*(.*)")
# A backticked name, bare (`listen_tls`) or module-qualified (`metrics.render`). The qualified form
# has to be matched here, not just split later: the pattern used to require a bare identifier, so a
# claim naming the thing qualified matched nothing at all and sailed past a declaration that exists.
BACKTICK_NAME = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)`")

# A ROADMAP list item's bold headline, when it opens with "No ": `- **No map/dictionary type.**`,
# nested or not. A struck-through item (`- ~~**No ...**~~`) is the resolved form and is skipped --
# group 1 is what distinguishes them.
ABSENCE_HEADLINE = re.compile(r"^\s*-\s+(~~)?\*\*(No\b[^*]*)\*\*")
# A ROADMAP list item that says what is still to come. The same shape as a "Not yet" claim, but it
# names *modules* rather than functions, which is why it needs its own vocabulary below: `net` and
# `http` are not `pub fn` declarations, so the original check read right past an entry whose whole
# "still ahead" list had shipped (docs/reviews/tasks/, the 2026-10-09 North Star correction).
STILL_AHEAD = re.compile(r"Still ahead\b[:,]?\s*(.*)")
# "Still missing: `replace`, and a general `index_of`/`find`" -- the same shape as a "Not yet"
# claim, in the one file where such a list is a roadmap item rather than a doc sentence.
STILL_MISSING = re.compile(r"Still missing\b[:,]?\s*(.*)")
# A builtin's own registration, and the interpreter's dispatch table: `native_register("replace",`
# and `{"len", B_len},`. Read from the source so a new builtin needs no edit here.
NATIVE_REGISTER = re.compile(r'native_register\(\s*"([A-Za-z_][A-Za-z0-9_]*)"')
BUILTIN_ROW = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*B_[A-Za-z0-9_]+\s*\}')
# An "*Update <date>:*" note inside the same item means the claim above it has already been
# answered in place, which is this file's convention for a resolved entry -- the stale sentence is
# deliberately left standing with the correction underneath it. Without this, the check would fire
# on exactly the entries that have been fixed properly.
UPDATE_NOTE = re.compile(r"\*Update \d{4}-\d{2}-\d{2}")
# src/typecheck.c's own list, so a sixth built-in type needs no edit here.
HARDCODED_TYPES = re.compile(r"HARDCODED_GENERIC_TYPES\[\]\s*=\s*\{([^}]*)\}")


def pub_names():
    names = {}
    for path in glob.glob(os.path.join(ROOT, "lib", "**", "*.zm"), recursive=True):
        rel = os.path.relpath(path, ROOT)
        with open(path) as f:
            for lineno, line in enumerate(f, start=1):
                m = PUB_DECL.match(line)
                if m:
                    names.setdefault(m.group(1), (rel, lineno))
    return names


def builtin_type_names():
    """The built-in parametric types, read from src/typecheck.c rather than listed here."""
    with open(os.path.join(ROOT, "src", "typecheck.c")) as f:
        m = HARDCODED_TYPES.search(f.read())
    if m is None:  # the declaration moved or was renamed: say so rather than silently checking nothing
        print("check_not_yet.py: cannot find HARDCODED_GENERIC_TYPES in src/typecheck.c", file=sys.stderr)
        sys.exit(1)
    return [s.strip().strip('"') for s in m.group(1).split(",") if s.strip().strip('"') not in ("", "NULL")]


def module_names():
    """Every bundled module, by file name: `lib/core/net.zm` means the module `net` exists."""
    found = {}
    for path in glob.glob(os.path.join(ROOT, "lib", "**", "*.zm"), recursive=True):
        base = os.path.basename(path)[:-3]
        found.setdefault(base, os.path.relpath(path, ROOT))
    return found


def builtin_names():
    """Every builtin's name, from its C registration or the interpreter's dispatch table. These are
    neither `pub fn` declarations nor modules, so nothing else here would find them."""
    found = {}
    for path in glob.glob(os.path.join(ROOT, "src", "*.c")):
        rel = os.path.relpath(path, ROOT)
        with open(path) as f:
            text = f.read()
        for lineno, line in enumerate(text.split("\n"), start=1):
            for m in list(NATIVE_REGISTER.finditer(line)) + list(BUILTIN_ROW.finditer(line)):
                found.setdefault(m.group(1), (rel, lineno))
    return found


# **The roadmap is a folder, not a file, since 2026-10-30** (docs/roadmap/). Each of the three
# checks below used to open docs/roadmap/ once; they now walk every part, because an item can sit
# in any of them and a check that read one file would silently stop covering the rest. README.md is
# the preamble and the list of parts, so it has no items of its own and is skipped.
def roadmap_parts():
    """Every part of the roadmap, as (relative path, lines)."""
    found = sorted(glob.glob(os.path.join(ROOT, "docs", "roadmap", "*.md")))
    parts = []
    for path in found:
        if os.path.basename(path) == "README.md":
            continue
        with open(path) as f:
            parts.append((os.path.relpath(path, ROOT), f.read().split("\n")))
    if not parts:
        print("check_not_yet.py: docs/roadmap/ has no parts -- the roadmap checks would pass "
              "vacuously", file=sys.stderr)
        sys.exit(1)
    return parts


def roadmap_items():
    """Every list item in the roadmap, as (relative path, first line number, body). An item runs to
    the next one, so an "*Update <date>: ...*" note written under a claim is read with it."""
    for rel, lines in roadmap_parts():
        starts = [i for i, l in enumerate(lines) if re.match(r"^\s*-\s", l)]
        for idx, start in enumerate(starts):
            end = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
            yield rel, start + 1, "\n".join(lines[start:end])


def still_missing_problems(names):
    """Roadmap list items whose "Still missing" names something that now exists."""
    problems = []
    for rel, lineno, body in roadmap_items():
        if UPDATE_NOTE.search(body):
            continue  # answered in place, which is how the roadmap records a resolution
        m = STILL_MISSING.search(body)
        if m is None:
            continue
        for name in BACKTICK_NAME.findall(m.group(1)):
            bare = name.rsplit(".", 1)[-1]
            if bare in names:
                decl_file, decl_line = names[bare]
                problems.append(
                    "%s:%d: says `%s` is still missing, but it already exists at %s:%d. If it has "
                    "landed, strike the clause through and add an \"*Update <date>: ...*\" note "
                    "saying so, as the other resolved entries do."
                    % (rel, lineno, name, decl_file, decl_line)
                )
    return problems


def still_ahead_problems(modules):
    """Roadmap list items whose "Still ahead" names a module that now exists."""
    problems = []
    for rel, lineno, body in roadmap_items():
        if UPDATE_NOTE.search(body):
            continue  # answered in place, which is how the roadmap records a resolution
        m = STILL_AHEAD.search(body)
        if m is None:
            continue
        for name in BACKTICK_NAME.findall(m.group(1)):
            bare = name.rsplit(".", 1)[-1]
            if bare in modules:
                problems.append(
                    "%s:%d: says `%s` is still ahead, but lib/%s exists. If it has landed, add an "
                    "\"*Update <date>: ...*\" note under the item saying so, as the other resolved "
                    "entries do." % (rel, lineno, name, os.path.basename(modules[bare]))
                )
    return problems


def absence_problems(names, types):
    """Every unresolved "No ..." headline in the roadmap that names something which now exists."""
    problems = []
    for rel, lines in roadmap_parts():
        for lineno, line in enumerate(lines, start=1):
            m = ABSENCE_HEADLINE.match(line)
            if m is None or m.group(1):  # no headline, or struck through, which is the resolved form
                continue
            head = m.group(2)
            for name in BACKTICK_NAME.findall(head):
                bare = name.rsplit(".", 1)[-1]
                if bare in names:
                    decl_file, decl_line = names[bare]
                    problems.append(
                        "%s:%d: says there is no `%s`, but it's already declared pub at %s:%d"
                        % (rel, lineno, name, decl_file, decl_line)
                    )
            words = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", head)
            said_type = "type" in (w.lower() for w in words)
            for ty in types:
                for w in words:
                    # Capitalised, backticked, or lowercased next to the word "type" -- see the
                    # module docstring for why a bare lowercase word alone is not enough.
                    if w == ty or (w.lower() == ty.lower() and (said_type or "`%s`" % w in head)):
                        problems.append(
                            "%s:%d: says there is no %s, but `%s` is a built-in type "
                            "(src/typecheck.c's HARDCODED_GENERIC_TYPES). If it has landed, strike the "
                            "headline through (- ~~**No ...**~~) and say what it became, as the "
                            "`Option<T>` entry does." % (rel, lineno, w, ty)
                        )
                        break
    return problems


def inside_quotes(line, at):
    """Whether position `at` falls inside a double-quoted span of `line`."""
    return line.count('"', 0, at) % 2 == 1


def not_yet_claims(path):
    """Every "Not yet: ..." occurrence in `path`, as (lineno, text-after-the-marker) -- the
    text extends across immediately following comment/prose lines too, since a claim often
    wraps (net.zm's own "Not yet: client certificates, ALPN," continues on the next line)."""
    rel = os.path.relpath(path, ROOT)
    with open(path) as f:
        lines = f.read().split("\n")
    claims = []
    i = 0
    while i < len(lines):
        m = NOT_YET.search(lines[i])
        if m and inside_quotes(lines[i], m.start()):
            # A *quoted* "Not yet" is a citation of some wording, not a claim about today. docs/design/journal/'s
            # own entry about this check quotes the stale sentence that motivated it, three times; without
            # this, adding docs/design/journal/ to the sources made the check report the entry documenting itself.
            m = None
        if m:
            text = m.group(1)
            j = i + 1
            # Keep pulling in continuation lines: a `//`-comment run, or a markdown paragraph
            # line that isn't blank and doesn't start a new list item/heading.
            while j < len(lines):
                nxt = lines[j].strip()
                # A "**Superseded ...**" note ends the claim it corrects: everything it names is what
                # *has* landed, which is the opposite of the claim, so pulling it in would make every
                # correction look like the staleness it documents. (It did, the first time.)
                if nxt.startswith("**Superseded"):
                    break
                if path.endswith(".zm"):
                    if nxt.startswith("//"):
                        text += " " + nxt[2:].strip()
                        j += 1
                        continue
                    break
                if nxt == "" or nxt.startswith("#") or nxt.startswith("- "):
                    break
                text += " " + nxt
                j += 1
            claims.append((rel, i + 1, text))
            i = j
        else:
            i += 1
    return claims


def main():
    names = pub_names()
    # Builtins count as existing for every check here, not just the "Still missing" one. Measured
    # when this was wired in: it adds no findings today (0 of the existing claims name a builtin)
    # and so no false positives either -- it is here to cover the next claim that names one.
    names.update(builtin_names())
    # The hand-written reference and the design journal, both folders since 2026-10-30, plus every
    # bundled module's doc comments.
    sources = (
        sorted(glob.glob(os.path.join(ROOT, "docs", "syntax", "*.md")))
        + sorted(glob.glob(os.path.join(ROOT, "docs", "design", "journal", "*.md")))
        + glob.glob(os.path.join(ROOT, "lib", "**", "*.zm"), recursive=True)
    )
    problems = []
    for path in sources:
        for rel, lineno, text in not_yet_claims(path):
            for name in BACKTICK_NAME.findall(text):
                # A claim may name the thing qualified (`metrics.render`) or bare (`listen_tls`), and
                # declarations are indexed bare, so a qualified mention needs its last segment tried
                # too -- without this, `Not yet: ... (`metrics.render`)` sailed past a declared
                # `pub fn render`, which is the whole class of thing this check exists to catch.
                bare = name.rsplit(".", 1)[-1]
                if bare in names:
                    decl_file, decl_line = names[bare]
                    problems.append(
                        "%s:%d: says `%s` is \"not yet\" supported, but it's already declared "
                        "pub at %s:%d" % (rel, lineno, name, decl_file, decl_line)
                    )
    problems += absence_problems(names, builtin_type_names())
    problems += still_ahead_problems(module_names())
    problems += still_missing_problems(names)
    if problems:
        for p in problems:
            print("check_not_yet.py: %s" % p, file=sys.stderr)
        sys.exit(1)
    print("check_not_yet.py: no stale 'not yet' or 'no such feature' claims found")


if __name__ == "__main__":
    main()
