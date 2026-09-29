#!/usr/bin/env python3
"""Catches a doc claiming a feature is "not yet" implemented once it actually is.

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

Scans only source: docs/SYNTAX.md (hand-written) and lib/**/*.zm (the doc comments
docs/lib/*.md and docs/CORELIB.md are generated from) -- not the generated .md files
themselves, since they can only ever repeat whatever the source already says.

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
BACKTICK_NAME = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)`")


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
        if m:
            text = m.group(1)
            j = i + 1
            # Keep pulling in continuation lines: a `//`-comment run, or a markdown paragraph
            # line that isn't blank and doesn't start a new list item/heading.
            while j < len(lines):
                nxt = lines[j].strip()
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
    sources = [os.path.join(ROOT, "docs", "SYNTAX.md")] + glob.glob(
        os.path.join(ROOT, "lib", "**", "*.zm"), recursive=True
    )
    problems = []
    for path in sources:
        for rel, lineno, text in not_yet_claims(path):
            for name in BACKTICK_NAME.findall(text):
                if name in names:
                    decl_file, decl_line = names[name]
                    problems.append(
                        "%s:%d: says `%s` is \"not yet\" supported, but it's already declared "
                        "pub at %s:%d" % (rel, lineno, name, decl_file, decl_line)
                    )
    if problems:
        for p in problems:
            print("check_not_yet.py: %s" % p, file=sys.stderr)
        sys.exit(1)
    print("check_not_yet.py: no stale 'not yet' claims found")


if __name__ == "__main__":
    main()
