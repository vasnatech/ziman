#!/usr/bin/env python3
r"""Checks that editors/vscode/syntaxes/ziman.tmLanguage.json gives the scopes it is meant to.

The grammar had no test of any kind. Nothing loaded it, nothing asserted a scope, and three real
gaps sat in it unnoticed until the 2026-10-11 style review read it by hand: `internal` matched
nothing, `#![...]` was not an attribute, and `val`/`var` shared one scope so no theme could tell
them apart (docs/reviews/tasks/ F75, F76). A grammar is a program; this is its test.

How faithful this is. TextMate tries a scope's `patterns` in order and takes the leftmost match,
earliest pattern winning a tie, then continues after it, and a rule may scope its capture groups
rather than the whole match -- that is what `scopes_of` does, resolving
`#include` references through the repository in the order the top-level list gives them. A
`begin`/`end` rule is consumed to its end on the same line rather than across lines, **and its own
nested `patterns` are honoured while looking for that end**, which is what lets an attribute hold a
`]` inside its message. It is therefore a check on the grammar's *rules*, not a renderer: it can
prove a token gets the scope intended, and cannot prove a multi-line string colours correctly.

**That last sentence was where the 2026-10-22 bug came in.** `clear`'s `#[when_unmatched("... an
empty one ('[]', 'Map<K, V> {}') ...")]` defeated the attribute rule -- then a single `match` of
`#!?\[[^\]]*\]`, a negated class that cannot see past a `]`. The scan ended inside `'[]'`, the
rest of the line coloured as code, and the message's closing quote opened a string that ran to the
end of the file: 191 lines of lib/prelude/array.zm as one string literal, measured with
vscode-textmate. Every case in the table below passed throughout, because each was one tidy line
and the damage was on the *next* line. Two things answer that: the attribute cases now use the real
messages from lib/prelude, and `sweep_corpus` reads every attribute the corpus actually writes
rather than trusting this table to have thought of the shape.

Run directly, or via `make grammar-check` (which `make check-fast` includes). Exits 1 and names
every case whose scope is not what the table says it should be, and every attribute in the corpus
the grammar cannot scan to its end.

**The full-fidelity check is a one-off, and this is deliberately not it.** `editors/vscode/
node_modules/` is gitignored, so a guard built on the real engine would need `npm install` and
would skip when the network is down -- which is how two name lists in this repo came to assert
nothing at all (F212). To run the real thing by hand:

    npm i vscode-textmate vscode-oniguruma     # then tokenise lib/**.zm and compare scopes

That is what measured the 191 mis-scoped lines quoted above; this file reproduces the same verdict
from the grammar's rules alone.
"""
import json
import os
import re
import sys

GRAMMAR = "editors/vscode/syntaxes/ziman.tmLanguage.json"

# (source line, the token to look for, the scope it must get). A scope is matched as a prefix, so
# `storage.modifier` accepts `storage.modifier.immutable.ziman`: a theme that styles the general
# name still reaches the specific one, which is what the dotted convention is for.
CLEAR_ATTR = ("#[when_unmatched(\"an immutable collection is emptied by writing an empty one "
               "('[]', 'Map<K, V> {}'), and a Mutable cell has no 'clear' at all\")]")

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
    # The real message from `clear` in lib/prelude/array.zm, and the reason this file grew a
    # nested-pattern simulation: the `]` in `'[]'` ended the old rule's match early, the rest of
    # the line coloured as code, and the trailing quote ran a string to the end of the file.
    (CLEAR_ATTR, CLEAR_ATTR, "meta.attribute"),
    # Its message's own text must be a string *inside* the attribute, not code.
    (CLEAR_ATTR, "'Map<K, V> {}'", "string.quoted.double"),
    # The line after an attribute like that must be ordinary code again. `scopes_of` works one
    # line at a time, so this pins the half of the damage a single line can show.
    ("native fn clear<T>(items: MutableArray<T>)", "native", "storage.modifier"),
    ("#![experimental]", "#![experimental]", "meta.attribute"),
    ("#![deprecated(\"use newmod\")]", "#![deprecated(\"use newmod\")]", "meta.attribute"),
    ("#![repository(VitalSign)]", "#![repository(VitalSign)]", "meta.attribute"),
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
    return flatten(grammar.get("patterns", []), grammar.get("repository", {}), [])


def flatten(node, repo, out):
    """Every rule `node` reaches, in the order TextMate would try them."""
    if isinstance(node, list):
        for item in node:
            flatten(item, repo, out)
        return out
    if not isinstance(node, dict):
        return out
    inc = node.get("include")
    if inc:
        if inc.startswith("#"):
            flatten(repo.get(inc[1:], {}), repo, out)
        return out
    if "match" in node or "begin" in node:
        out.append(node)
    if "patterns" in node:
        flatten(node["patterns"], repo, out)
    return out


def leftmost(line, rules, pos):
    """The earliest-starting rule match at or after `pos`, earliest rule breaking a tie."""
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
    return best


def span_end(line, rule, after, repo, spans=None):
    """Where a begin/end rule closes, with its nested patterns consuming what they match first.

    Nested matches are recorded in `spans` as well as consumed, because they carry their own scope
    in a real editor: an attribute's message is `string.quoted.double` *inside* the attribute's
    `meta.attribute`, and a check that only knew the outer span could not tell a message that
    colours as a string from one that colours as code.

    This is the part that makes an attribute holding a `]` work. TextMate takes the leftmost of the
    `end` pattern and the nested ones, so an attribute's opening quote -- which always precedes the
    `]` inside its message -- wins, the string is consumed whole, and the `]` is never offered to
    `end`. Looking for `end` alone is what the single-`match` rule effectively did, and what broke
    191 lines of array.zm.
    """
    inner = flatten(rule.get("patterns") or [], repo, [])
    scan = after
    while True:
        closing = re.compile(rule["end"]).search(line, scan)
        cand = leftmost(line, inner, scan) if inner else None
        if closing is None and cand is None:
            return len(line)
        if cand is not None and (closing is None or cand[0].start() < closing.start()):
            m2, r2 = cand
            stop = m2.end()
            if "begin" in r2 and r2.get("end"):
                stop = span_end(line, r2, m2.end(), repo, spans)
            if spans is not None and r2.get("name"):
                spans.append((m2.start(), stop, r2["name"]))
            nxt = max(stop, m2.start() + 1)
            if nxt <= scan:
                return closing.end() if closing else len(line)
            scan = nxt
            continue
        return closing.end()


def scopes_of(line, rules, repo=None):
    """[(start, end, scope)] for `line`, leftmost match wins, earliest rule breaks a tie."""
    repo = repo if repo is not None else {}
    spans = []
    pos = 0
    while pos < len(line):
        best = leftmost(line, rules, pos)
        if best is None:
            break
        m, rule = best
        end = m.end()
        if "begin" in rule and rule.get("end"):
            end = span_end(line, rule, m.end(), repo, spans)
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


SWEPT = ["lib", "tests/lang", "docs/lib/examples", "editors/vscode"]


def attribute_end(line, start):
    """Where an attribute really ends: the first `]` outside its own double-quoted message.

    An oracle independent of the grammar, which is the point -- comparing the grammar's span
    against "the first `]` on the line" would have agreed with the very bug this file exists to
    catch, since that bug mistakes a `]` inside the message for the end. Returns None for an
    attribute that does not close on this line, which Ziman does not have and the caller reports.
    """
    i = start + (3 if line.startswith("#![", start) else 2)
    while i < len(line):
        if line[i] == '"':
            i += 1
            while i < len(line) and line[i] != '"':
                i += 2 if line[i] == "\\" else 1
            if i >= len(line):
                return None
            i += 1
        elif line[i] == "]":
            return i + 1
        else:
            i += 1
    return None


def sweep_corpus(rules, repo):
    """Every attribute the corpus actually writes, not the ones this file thought of.

    CASES is hand-written, so it only ever covers shapes someone anticipated -- and the message
    that broke the grammar had been in lib/prelude for a pass before anyone looked. This reads the
    real attribute lines and asserts the whole `#[...]` is one `meta.attribute` span, which is the
    property that fails the moment the rule cannot scan past something inside the message.

    A positive invariant comes with it, for the reason tools/overload_sets.sh has one: at least one
    attribute in the corpus must carry a `]` inside its message, or this is measuring nothing.
    """
    problems, scanned, with_bracket = [], 0, 0
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for base in SWEPT:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, base)):
            dirnames[:] = [d for d in dirnames if d != "node_modules"]
            for name in sorted(filenames):
                if not name.endswith(".zm"):
                    continue
                path = os.path.join(dirpath, name)
                with open(path) as f:
                    lines = f.read().splitlines()
                for n, line in enumerate(lines, 1):
                    stripped = line.lstrip()
                    if not stripped.startswith("#[") and not stripped.startswith("#!["):
                        continue
                    scanned += 1
                    start = len(line) - len(stripped)
                    rel = os.path.relpath(path, root)
                    want_end = attribute_end(line, start)
                    if want_end is None:
                        problems.append(f"{rel}:{n}: this attribute does not close on its own "
                                        f"line, which Ziman has no form for: {stripped[:60]!r}")
                        continue
                    body = line[start:want_end]
                    # Only a `]` inside a quoted message counts. Reading from the first quote to
                    # the end of the body instead counts the attribute's own closing bracket, which
                    # made this say 17 when the real answer is 2.
                    if any("]" in msg for msg in re.findall(r'"((?:[^"\\]|\\.)*)"', body)):
                        with_bracket += 1
                    spans = scopes_of(line, rules, repo)
                    attr = next((s for s in spans if s[0] == start
                                 and s[2].startswith("meta.attribute")), None)
                    if attr is None:
                        problems.append(f"{rel}:{n}: the attribute gets no meta.attribute scope "
                                        f"at all: {stripped[:60]!r}")
                    elif attr[1] != want_end:
                        problems.append(
                            f"{rel}:{n}: the attribute scope covers columns {start}-{attr[1]} but "
                            f"the attribute ends at {want_end} -- {line[attr[1]:want_end][:40]!r} "
                            f"colours as code, and a quote in it runs a string into the lines "
                            f"after")
    if scanned and with_bracket == 0:
        problems.append("no attribute in the corpus carries a ']' inside its message any more, so "
                        "the nested-pattern simulation above is measuring nothing -- put the shape "
                        "back in editors/vscode/showcase.zm (see its #[deprecated] line)")
    return problems, scanned, with_bracket


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

    repo = grammar.get("repository", {})
    for line, token, want in CASES:
        spans = scopes_of(line, rules, repo)
        start = line.index(token)
        hit = next((s for s in spans if s[0] <= start and s[1] >= start + len(token)), None)
        if hit is None:
            inside = [s for s in spans if start <= s[0] < start + len(token)]
            got = ("no scope covers it; the scopes inside it are "
                   + ", ".join(s[2] for s in inside) if inside else "no scope at all")
            problems.append(f"{token!r} in {line.strip()!r}: wanted {want}, got {got}")
        elif not hit[2].startswith(want):
            problems.append(f"{token!r} in {line.strip()!r}: wanted {want}, got {hit[2]}")

    swept, scanned, with_bracket = sweep_corpus(rules, repo)
    problems += swept

    print(f"grammar: {len(rules)} rules, {len(CASES)} scope cases checked, "
          f"{scanned} corpus attributes scanned to their end "
          f"({with_bracket} with a ']' inside the message)")
    if not problems:
        return 0
    print()
    for p in problems:
        print(f"{GRAMMAR}: {p}")
    print(f"\n{len(problems)} case(s) do not get the scope they should.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
