#!/usr/bin/env python3
"""Compiles the Ziman examples in docs/SYNTAX.md, so one cannot quietly stop being valid Ziman.

docs/SYNTAX.md is hand-written and nothing ever compiled it. When this was added, 2 of its 45
code blocks did not compile as shown, in the two ways that matter most:

  * `struct AddCase { a: int, b: int, expected: int }` -- struct fields are separated by
    whitespace, never commas. A reader copying that line gets a syntax error from a document
    whose whole job is to say what the syntax is.
  * an example declaring a module-level `val props` and a function taking a parameter also
    named `props`, which Ziman rejects.

Four more compiled only once the `use` they relied on was added. This is the same class of
defect as a diagnostic recommending syntax that does not exist, and the fix is the one already
proven for docs/lib: make the examples real files and let the build fail.

How a block is treated depends on its fence's info string:

    ```ziman                 must compile as a module (`ziman --check`)
    ```ziman fragment        statements; wrapped in `fn main() { ... }` first
    ```ziman fails           must NOT compile -- the block demonstrates an error on purpose
    ```ziman ignore: <why>   not compiled, and the reason is required so the next reader knows
    ```                      not Ziman at all (a grammar, a shell command, program output)

`fails` is as load-bearing as the rest: an example whose point is that the compiler rejects it
("this Result is discarded") stops demonstrating anything the day it starts compiling, and that
is a silent failure no other check would see.

What this does not do: run anything. A block that compiles may still print something other than
what its comments claim, and the `ignore:` blocks (a BNF grammar, a `{ ... }` placeholder, a
signature with no body, a two-file example) are unverifiable this way by construction. Those
still need a human, the way F5's audit was.

Run directly, or via `make syntax-check`. Exits 1 and names every block that disagrees with its
tag, by its line in SYNTAX.md.
"""
import os
import re
import subprocess
import sys
import tempfile

DOC = "docs/SYNTAX.md"
ZIMAN = os.environ.get("ZIMAN", "./ziman")


def blocks(text):
    """Every fenced block, as (line of the first body line, info string, body)."""
    out = []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        if lines[i].startswith("```"):
            info = lines[i][3:].strip()
            start = i + 1
            j = start
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            out.append((start + 1, info, "\n".join(lines[start:j])))
            i = j + 1
        else:
            i += 1
    return out


def check(body, mode, tmp):
    """Compile `body` under `mode`; returns None when it behaved, else what went wrong."""
    source = body
    if mode == "fragment":
        # Any `use` the fragment opens with has to stay at the top -- an import is not a statement --
        # and the rest goes in a function that returns a Result, so a fragment demonstrating `try` is
        # compiled the way the reader would really write it rather than rejected for the wrapper's sake.
        kept, rest = [], []
        for line in body.split("\n"):
            (kept if (not rest and re.match(r"\s*use ", line)) else rest).append(line)
        indented = "\n".join("    " + l if l.strip() else l for l in rest)
        source = (
            "\n".join(kept)
            + "\n\nfn example() -> Result<unit, string> {\n"
            + indented
            + "\n    return unit\n}\n\nfn main() {}\n"
        )
    elif not re.search(r"\bfn main\(", body):
        source = body + "\n\nfn main() {}\n"
    path = os.path.join(tmp, "example.zm")
    with open(path, "w") as f:
        f.write(source)
    r = subprocess.run([ZIMAN, "--check", path], capture_output=True, text=True)
    output = (r.stdout + r.stderr).replace(path, "<example>")
    if mode == "fails":
        if r.returncode == 0:
            return "tagged `fails` but it compiles -- the error it demonstrates is gone, so the example no longer shows anything"
        return None
    if r.returncode != 0:
        first = ""
        m = re.search(r"ziman: ([^\n]+)", output)
        if m:
            first = m.group(1)
        return first or output.strip().split("\n")[0]
    return None


def main():
    text = open(DOC).read()
    counts = {"module": 0, "fragment": 0, "fails": 0, "ignore": 0, "untagged": 0}
    problems = []
    untagged_ziman = []
    with tempfile.TemporaryDirectory() as tmp:
        for line, info, body in blocks(text):
            words = info.split()
            if not words or words[0] != "ziman":
                counts["untagged"] += 1
                # A block nobody tagged that still looks like Ziman is how this check gets
                # quietly bypassed, so say so rather than skipping in silence.
                if re.search(r"^\s*(pub |fn |val |var |use |struct |enum |native |#\[)", body, re.M):
                    untagged_ziman.append(line)
                continue
            rest = info[len("ziman"):].strip()
            if rest.startswith("ignore"):
                counts["ignore"] += 1
                if ":" not in rest or not rest.split(":", 1)[1].strip():
                    problems.append((line, "tagged `ignore` with no reason after the colon"))
                continue
            mode = rest if rest in ("fragment", "fails") else "module"
            if rest and rest not in ("fragment", "fails"):
                problems.append((line, f"unknown tag '{rest}' (expected fragment, fails or ignore: <why>)"))
                continue
            counts[mode] += 1
            bad = check(body, mode, tmp)
            if bad:
                problems.append((line, bad))

    print(
        "SYNTAX.md examples: {module} compiled, {fragment} fragments, {fails} expected to fail, "
        "{ignore} ignored, {untagged} not Ziman".format(**counts)
    )
    for line in untagged_ziman:
        problems.append((line, "looks like Ziman but its fence has no `ziman` tag, so nothing compiles it"))
    if not problems:
        return 0
    print()
    for line, why in sorted(problems):
        print(f"{DOC}:{line}: {why}")
    print(f"\n{len(problems)} block(s) disagree with their tag.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
