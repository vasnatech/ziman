#!/usr/bin/env python3
"""Checks that editors/vscode/syntaxes/ziman.tmLanguage.json gives the scopes it is meant to.

The grammar had no test of any kind. Nothing loaded it, nothing asserted a scope, and three real
gaps sat in it unnoticed until the 2026-10-11 style review read it by hand: `internal` matched
nothing, `#![...]` was not an attribute, and `val`/`var` shared one scope so no theme could tell
them apart (docs/reviews/tasks.md F75, F76). A grammar is a program; this is its test.

How faithful this is. TextMate tries a scope's `patterns` in order and takes the leftmost match,
earliest pattern winning a tie, then continues after it, and a rule may scope its capture groups
rather than the whole match -- that is what `scopes_of` does, resolving
`#include` references through the repository in the order the top-level list gives them. The two
`begin`/`end` rules (a `"` string and a `\"\"\"` text block) are consumed to their end on the same
line rather than across lines, which is enough for the cases below and is the one place this
differs from a real editor. It is therefore a check on the grammar's *rules*, not a renderer: it
can prove a token gets the scope intended, and cannot prove a multi-line string colours correctly.

Run directly, or via `make grammar-check`. Exits 1 and names every case whose scope is not what the
table says it should be.
"""
import json
import os
import re
import sys

GRAMMAR = "editors/vscode/syntaxes/ziman.tmLanguage.json"

# (source line, the token to look for, the scope it must get). A scope is matched as a prefix, so
# `storage.modifier` accepts `storage.modifier.immutable.ziman`: a theme that styles the general
# name still reaches the specific one, which is what the dotted convention is for.
CASES = [
    # visibility and mutability -- all three visibility levels, and val/var separable (F75, F76)
    ("pub fn f() {", "pub", "storage.modifier"),
    ("internal fn f() {", "internal", "storage.modifier"),
    ("native fn f()", "native", "storage.modifier"),
    ("fn f(xs: variadic int) {", "variadic", "storage.modifier"),
    ("    val x = 1", "val", "storage.modifier.immutable"),
    ("    var y = 2", "var", "storage.modifier.mutable"),
    # attributes, both forms (F75)
    ("#[derive(Json)]", "#[derive(Json)]", "meta.attribute"),
    ("#[deprecated(\"use g\")]", "#[deprecated(\"use g\")]", "meta.attribute"),
    ("#[when_unmatched(\"try str(x)\")]", "#[when_unmatched(\"try str(x)\")]", "meta.attribute"),
    ("#![experimental]", "#![experimental]", "meta.attribute"),
    ("#![deprecated(\"use newmod\")]", "#![deprecated(\"use newmod\")]", "meta.attribute"),
    # the things that already worked, so a fix cannot quietly break them
    ("fn main() {", "fn", "storage.type.function"),
    ("    with base { x: 1 }", "with", "keyword.other"),
    ("struct Point {", "Point", "entity.name.type"),
    ("enum Shape {", "Shape", "entity.name.type"),
    ("    val n = 42", "42", "constant.numeric"),
    ("    val f = 1.5", "1.5", "constant.numeric"),
    ("    val b = true", "true", "constant.language.boolean"),
    ("    val s: string = \"hi\"", "string", "support.type"),
    ("// a comment", "// a comment", "comment"),
]


def rules_in_order(grammar):
    """Every rule the top-level `patterns` reaches, flattened in the order TextMate would try."""
    repo = grammar.get("repository", {})
    out = []

    def add(node):
        if isinstance(node, list):
            for item in node:
                add(item)
            return
        if not isinstance(node, dict):
            return
        inc = node.get("include")
        if inc:
            if inc.startswith("#"):
                add(repo.get(inc[1:], {}))
            return
        if "match" in node or "begin" in node:
            out.append(node)
        if "patterns" in node:
            add(node["patterns"])

    add(grammar.get("patterns", []))
    return out


def scopes_of(line, rules):
    """[(start, end, scope)] for `line`, leftmost match wins, earliest rule breaks a tie."""
    spans = []
    pos = 0
    while pos < len(line):
        best = None
        for rule in rules:
            pattern = rule.get("match") or rule.get("begin")
            try:
                m = re.compile(pattern).search(line, pos)
            except re.error:
                continue  # an Oniguruma-only construct; reported by the self-check below
            if m is None or m.start() == m.end():
                continue
            if best is None or m.start() < best[0].start():
                best = (m, rule)
        if best is None:
            break
        m, rule = best
        end = m.end()
        if "begin" in rule and rule.get("end"):
            closing = re.compile(rule["end"]).search(line, m.end())
            end = closing.end() if closing else len(line)
        if rule.get("name"):
            spans.append((m.start(), end, rule["name"]))
        # A rule may name its capture groups instead of the whole match, which is how `fn name` gives
        # the keyword and the name two different scopes. Without this, such a token reads as having no
        # scope -- which is how the first run of this check reported three false gaps.
        for group, info in (rule.get("captures") or {}).items():
            if not info.get("name"):
                continue
            try:
                start, stop = m.span(int(group))
            except (ValueError, IndexError):
                continue
            if start >= 0:
                spans.append((start, stop, info["name"]))
        pos = max(end, m.start() + 1)
    return spans


def main():
    if not os.path.exists(GRAMMAR):
        print(f"{GRAMMAR}: not found")
        return 1
    with open(GRAMMAR) as f:
        grammar = json.load(f)  # a parse error here is the first thing this check is for
    rules = rules_in_order(grammar)
    if not rules:
        print(f"{GRAMMAR}: the top-level `patterns` reached no rule at all")
        return 1

    problems = []
    for pattern in [r.get("match") or r.get("begin") for r in rules]:
        try:
            re.compile(pattern)
        except re.error as e:
            problems.append(f"pattern {pattern!r} is not a regex this check can read ({e})")

    for line, token, want in CASES:
        spans = scopes_of(line, rules)
        start = line.index(token)
        hit = next((s for s in spans if s[0] <= start and s[1] >= start + len(token)), None)
        if hit is None:
            inside = [s for s in spans if start <= s[0] < start + len(token)]
            got = ("no scope covers it; the scopes inside it are "
                   + ", ".join(s[2] for s in inside) if inside else "no scope at all")
            problems.append(f"{token!r} in {line.strip()!r}: wanted {want}, got {got}")
        elif not hit[2].startswith(want):
            problems.append(f"{token!r} in {line.strip()!r}: wanted {want}, got {hit[2]}")

    print(f"grammar: {len(rules)} rules, {len(CASES)} scope cases checked")
    if not problems:
        return 0
    print()
    for p in problems:
        print(f"{GRAMMAR}: {p}")
    print(f"\n{len(problems)} case(s) do not get the scope they should.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
