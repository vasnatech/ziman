#!/usr/bin/env python3
"""Catches a doc claiming a feature is missing once it actually exists.

Two shapes, both of them "the docs say Ziman cannot do X" where the code says otherwise. The
second was added on 2026-10-09 after the North Star section of docs/ROADMAP.md was found still
listing "No map/dictionary type. Only `[T]` exists" among the things standing between the
language and a backend -- long after `Map<K, V>` and `Set<T>` became built-in types. The entry
two lines below it, for `Option<T>`, had been struck through properly, which is the only reason
the stale one was visible at all: by contrast, to a human reading the file. Nothing checked it.

The language review found this exact mismatch once already: docs/SYNTAX.md said "Not yet:
server-side TLS (`listen_tls`)" after `lib/core/net.zm` had already grown a `pub fn listen_tls`
(the doc was written first and never revisited once the feature landed). This script guards
against that specific regression, mechanically: it collects every `pub fn`/`pub native fn`/
`pub struct`/`pub enum`/`pub val` name declared anywhere under lib/, then scans docs/SYNTAX.md
and every lib/**/*.zm doc comment for a "Not yet: ..." phrase and flags one that names an
already-`pub` identifier in backticks.

It only catches this one shape of drift -- a "Not yet" list naming a specific `pub` identifier
that already exists. A "Not yet: HTTP/2, multipart, compression" clause (prose, no backtick'd
name that could ever collide with a real declaration) is unverifiable this way and correctly
never flagged; that class of staleness still needs a human doc sweep, as F5's own audit was.

A "Not yet" inside double quotes is skipped: that is someone citing a wording (this file's own
DESIGN.md entry quotes the stale sentence that motivated the check), not claiming it is still true.

Scans source only: docs/SYNTAX.md and docs/DESIGN.md (hand-written) and lib/**/*.zm (the doc comments
docs/lib/*.md and docs/CORELIB.md are generated from) -- not the generated .md files
themselves, since they can only ever repeat whatever the source already says.

**Shape two: an absence claim in docs/ROADMAP.md.** A list item whose bold headline starts with
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
substitute for reading the file. ROADMAP.md only -- docs/DESIGN.md is a dated log whose "No X"
sentences are true of their date by design, so scanning it would report history as staleness.

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


def absence_problems(names, types):
    """Every unresolved "No ..." headline in ROADMAP.md that names something which now exists."""
    path = os.path.join(ROOT, "docs", "ROADMAP.md")
    rel = os.path.relpath(path, ROOT)
    problems = []
    with open(path) as f:
        for lineno, line in enumerate(f.read().split("\n"), start=1):
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
            # A *quoted* "Not yet" is a citation of some wording, not a claim about today. DESIGN.md's
            # own entry about this check quotes the stale sentence that motivated it, three times; without
            # this, adding DESIGN.md to the sources made the check report the entry documenting itself.
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
    sources = [os.path.join(ROOT, "docs", "SYNTAX.md"), os.path.join(ROOT, "docs", "DESIGN.md")] + glob.glob(
        os.path.join(ROOT, "lib", "**", "*.zm"), recursive=True
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
    if problems:
        for p in problems:
            print("check_not_yet.py: %s" % p, file=sys.stderr)
        sys.exit(1)
    print("check_not_yet.py: no stale 'not yet' or 'no such feature' claims found")


if __name__ == "__main__":
    main()
