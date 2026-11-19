#!/usr/bin/env python3
"""One line per bundled module, taken from the module's own header comment.

**Why this is not a list in a shell script.** `gen_corelib_reference.sh` kept a hand-written
`describe()` case with one arm per module, and an unknown module fell through to `echo ""` -- so
42 of the 92 bundled modules shipped in docs/CORELIB.md with a bare signature dump and no
description at all, while README.md called that file "every bundled module's `pub` signatures" and
CHEATSHEET.md called it "every module and every function in it". Nothing said so: the `git diff`
gate compares the file against what the generator produces, and the generator produced the blanks.

Every module header already begins `<name>.zm -- <what it is>`, which `tools/gen_lib_manual.py`
has been using as each page's introduction all along. Taking the same sentence here means the
reference, the manual and the module cannot drift apart -- the rule that file's own comment
already states about the `#![experimental]` label.

Prints `<name>\t<sentence>` for every module it can find, and fails naming any module whose header
does not start that way, because a module with nothing to say about itself is the thing worth
noticing rather than papering over.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where a bundled module can live: a `.zm` directly in one of these, or a folder of parts whose
# name is the module (`lib/core/charset`, `lib/ui/theme`, the three under `lib/http`).
FLAT = ["lib", "lib/core", "lib/db", "lib/graphics", "lib/http", "lib/time",
        "lib/ui", "lib/ui/desktop", "lib/ui/terminal"]
PARTS = ["lib/core/charset", "lib/ui/theme", "lib/http/server", "lib/http/client", "lib/http/rest"]


def modules():
    found = {}
    for d in FLAT:
        folder = os.path.join(ROOT, d)
        if not os.path.isdir(folder):
            continue
        for fn in sorted(os.listdir(folder)):
            if fn.endswith(".zm") and fn != "prelude.zm":
                found.setdefault(fn[:-3], os.path.join(folder, fn))
    for d in PARTS:
        name = os.path.basename(d)
        first = os.path.join(ROOT, d, name + ".zm")
        if os.path.exists(first):
            found[name] = first
    return found


def summary(path, name):
    """The header's first sentence, with the `<name>.zm -- ` prefix removed."""
    # **Two things a naive scan gets wrong, both found by running it.** `number.zm` opens with a
    # `use locale` before its header, so stopping at the first non-comment line finds nothing; and
    # only some headers begin `<name>.zm -- `, while others (`controls.zm`, `widget.zm`) open
    # straight into the sentence. So: skip to the first comment block wherever it starts, take it
    # whole, and strip the `<name>.zm -- ` opening only if it is there.
    with open(path, encoding="utf-8") as f:
        head = [next(f, "") for _ in range(60)]
    start = next((i for i, l in enumerate(head) if l.startswith("//")), None)
    if start is None:
        return None
    lines = []
    for line in head[start:]:
        if line.startswith("//"):
            lines.append(line[2:].strip())
        else:
            break
    if not lines:
        return None
    text = " ".join(x for x in lines if x)
    text = re.sub(r"^%s\.zm\s*--\s*" % re.escape(name), "", text)
    if text.startswith(name + ".zm"):
        return None
    m = re.match(r"(.+?[.!?])(\s|$)", text)
    out = (m.group(1) if m else text).strip()
    return out[:1].upper() + out[1:] if out else None


def main():
    bad = []
    for name, path in sorted(modules().items()):
        said = summary(path, name)
        if said is None:
            bad.append(name)
            continue
        print("%s\t%s" % (name, said))
    for name in bad:
        print("gen_module_summaries.py: %s has no header sentence of the form "
              "'%s.zm -- what it is'" % (name, name), file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
