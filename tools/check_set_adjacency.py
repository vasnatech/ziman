#!/usr/bin/env python3
"""A set's members must be consecutive declarations within their file.

Every lib/prelude header states this -- "a set's members sit together within whichever file they
land in" -- and until 2026-10-22 nothing checked it. Renaming `mutable_string_index_of` to
`index_of` that day split `index_of` across 107 lines of string.zm, and the build, the suite and
every other guard stayed green; a reader noticed. This is that reader.

**Within a file, not across them.** A set is deliberately split by the filing rule -- a declaration
lives in the file for its first argument's type, so `len` has five files and `contains` five -- and
`ziman --api builtins` groups a whole set into one block, which is what makes the split readable.
What must not happen is a *file* interleaving one set's members with another's.

Doc comments between members are expected and are not a split: each declaration carries its own, so
that `docs/lib/prelude.md` does not publish a bare signature under a shared one. Only another
declaration between two members counts.
"""
import re
import sys
from pathlib import Path

DECL = re.compile(r"^(?:pub |internal )?(?:builtin |native )?fn ([a-z_][A-Za-z_0-9]*)\s*[<(]")


def split_sets(path):
    """Yield (name, member_count, names_appearing_between) for each non-adjacent set."""
    names = [m.group(1) for m in (DECL.match(line) for line in path.read_text().splitlines()) if m]
    positions = {}
    for i, name in enumerate(names):
        positions.setdefault(name, []).append(i)
    for name, idx in sorted(positions.items()):
        if len(idx) > 1 and idx[-1] - idx[0] + 1 != len(idx):
            between = sorted({names[k] for k in range(idx[0], idx[-1] + 1)} - {name})
            yield name, len(idx), between


def main():
    root = Path(__file__).resolve().parent.parent
    files = sorted((root / "lib" / "prelude").glob("*.zm"))
    if not files:
        print("check_set_adjacency.py: no prelude files found", file=sys.stderr)
        return 1
    bad = 0
    sets = 0
    for path in files:
        for name, count, between in split_sets(path):
            bad += 1
            print(
                "%s: '%s' has %d declarations that are not consecutive -- %s %s between them"
                % (path.relative_to(root), name, count, ", ".join(between),
                   "sits" if len(between) == 1 else "sit"),
                file=sys.stderr,
            )
        text = path.read_text()
        sets += len({m.group(1) for m in (DECL.match(l) for l in text.splitlines()) if m})
    if bad:
        print(
            "\ncheck_set_adjacency.py: %d set(s) split within a file. Every prelude header says a\n"
            "set's members sit together; move them, or move one to the file its first argument's\n"
            "type belongs to." % bad,
            file=sys.stderr,
        )
        return 1
    print("check_set_adjacency.py: %d names across %d prelude files, every set consecutive"
          % (sets, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
