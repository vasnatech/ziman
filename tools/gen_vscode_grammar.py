#!/usr/bin/env python3
"""Rewrites the two name lists in the VS Code grammar from what the compiler actually has.

Why this exists: the lists were hand-kept, and on 2026-10-19 the grammar still highlighted
`ArrayBuilder`, `StringBuilder`, `sb_append`, `mapb_set`, `setb_add` and `concat_arrays` -- every
one of them deleted during F149-F162 -- while knowing none of the 28 names F178-F180 unprefixed.
It is the same failure `src/lsp.c` had (88 names against 157, F168) and the same answer: a list of
what exists has to be derived from what exists.

Only the `match` strings of `builtin-types` and `builtin-functions` are touched; everything else
in the file is hand-written and left alone.

usage: tools/gen_vscode_grammar.py [ziman-binary]      (rewrites the file in place)
"""
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GRAMMAR = ROOT / "editors" / "vscode" / "syntaxes" / "ziman.tmLanguage.json"

# The generic types a program writes by name, taken from the *signatures* rather than from the
# prose tail under "built-in types": that tail lists `Map`, `Set`, `Option`, `Result` and
# `Channel` but none of the `Mutable*` family, which a program writes constantly. A capitalised
# name of three characters or more in a signature is one of these; the single letters are type
# parameters (`T`, `U`, `K`, `V`, `E`, `A`), which are not types and must not be highlighted as
# one -- highlighting `T` would paint every generic signature in the editor.
TYPE_NAME = re.compile(r"\b([A-Z][A-Za-z0-9]{2,})\b")


def builtins(binary):
    out = subprocess.run([str(binary), "--api", "builtins"], capture_output=True, text=True,
                         check=True).stdout
    names = set(re.findall(r"^(?:builtin |pub )?fn ([A-Za-z_][A-Za-z_0-9]*)", out, re.M))
    # The undeclared tail: dispatched names with a recorded reason they carry no signature.
    tail = out.split("also builtin", 1)
    if len(tail) == 2:
        for line in tail[1].split("built-in types")[0].splitlines():
            if line.startswith("  ") and line.strip():
                names.add(line.split()[0])
    # `$`-prefixed names are emitted by the parser and never written, so they are not highlighted.
    names = {n for n in names if not n.startswith("$")}

    # Signature lines only -- scanning the whole output dragged in `SYNTAX` and `Builtins` from
    # the prose -- and with the function's own type-parameter list removed first, because the
    # bounds inside it (`<T: Ord>`, `<T: Eq + Hash>`) name *constraints*, not types, and
    # highlighting `Ord` as a type would be wrong wherever it is written.
    types = set()
    for line in out.splitlines():
        m = re.match(r"^(?:builtin |pub )?fn [A-Za-z_][A-Za-z_0-9]*", line)
        if m is None:
            continue
        rest = line[m.end():]
        if rest.startswith("<"):
            depth = 0
            for i, ch in enumerate(rest):
                depth += (ch == "<") - (ch == ">")
                if depth == 0:
                    rest = rest[i + 1:]
                    break
        types.update(TYPE_NAME.findall(rest))
    return sorted(names), sorted(types)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "ziman")
    names, types = builtins(binary)
    if not names or not types:
        raise SystemExit("gen_vscode_grammar.py: '--api builtins' gave no names -- refusing to "
                         "write an empty list, which would silently turn the highlighting off")

    text = GRAMMAR.read_text()
    doc = json.loads(text)  # parsed only to validate; the file is rewritten textually to keep
    assert "repository" in doc, "grammar has no repository section"  # its hand-written formatting

    def replace(section, pattern):
        nonlocal text
        block = re.search(r'("%s":\s*\{.*?"match":\s*)"(.*?)"' % section, text, re.S)
        if block is None:
            raise SystemExit("gen_vscode_grammar.py: no '%s' section in the grammar" % section)
        start, end = block.span(2)
        return text[:start] + pattern + text[end:]

    text = replace("builtin-types", r"\\\\b(%s)\\\\b" % "|".join(types))
    text = replace("builtin-functions", r"\\\\b(%s)(?=\\\\s*[(<])" % "|".join(names))
    json.loads(text)  # the rewrite must still parse
    GRAMMAR.write_text(text)
    print("gen_vscode_grammar.py: %d builtin functions, %d builtin types" % (len(names), len(types)))


if __name__ == "__main__":
    main()
