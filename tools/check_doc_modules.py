#!/usr/bin/env python3
"""Guard the hand-written docs against naming modules and functions that no longer exist.

Why this exists: docs/reviews/tasks.md F138 was a line in docs/TUTORIAL.md that told the reader to
use `collections.map`/`filter`/`fold` -- a module deleted in F122, months earlier. Nothing noticed,
because nothing looks. The generated manual under docs/lib/ cannot go stale (`make lib-manual-check`
regenerates it from lib/ and diffs), and neither can docs/CORELIB.md; the hand-written docs are the
only place a dead module name can sit, and they are the ones a newcomer reads first.

Three checks, all of them exact -- no heuristics, because a guard that cries wolf is a guard nobody
keeps:

  1. A *retired* module named in a `module.member` code span or a `use` line. "Retired" is computed
     from git rather than from a hand-kept list: every module file ever deleted from lib/, minus the
     ones that came back (url was deleted and re-added, so it is not retired). This is the F138
     check, and it is the one that needs git history to be exact.
  2. A `use <name>` line naming something that is not a module. A name containing `/` is a path into
     the reader's own program (`use models/user` in the tutorial) and is skipped: only bare names,
     which can only mean a bundled module, are checked.
  3. A `module.member` span where the module is real but the member is not exported by it. This is
     what catches a rename -- D17 renamed `array_join` to `join`, and the doc comment that described
     the old name was wrong for as long as nobody re-read it.

Scope is the hand-written user-facing docs only. DESIGN.md, ROADMAP.md and MEMORY.md are design and
history records, where "we used to have a `collections` module" is a true sentence that must stay
sayable: between them they mention the retired modules 40+ times on purpose.

Exit 0 and print nothing when clean. Run by `make docs-check`, which `make check-fast` runs.
"""
import re
import subprocess
import sys
from pathlib import Path

# The hand-written docs. Everything under docs/lib/ and docs/CORELIB.md is generated.
HAND_WRITTEN = ["README.md", "docs/TUTORIAL.md", "docs/SYNTAX.md", "docs/CHEATSHEET.md"]

# Spans that look like `module.member` but are not. Keep this list short and each entry explained --
# an entry here is a claim that the guard is wrong, not a way to quiet it.
ALLOW = {
    # A properties key in the properties section, documenting the env-var mapping
    # `server.port` -> `SERVER_PORT`. It is a config key that happens to share a module's name.
    ("docs/SYNTAX.md", "server.port"),
}

# A span whose right-hand side is a file extension is a filename, not a call: `prelude.zm`,
# `arena.c`, `tasks.md`. This matters only when the left-hand side is also a real module name --
# `prelude.zm` is the case that found this -- because every other filename is skipped already by
# not naming a module. Checked against the whole corpus: 113 distinct non-module left-hand names
# appear in these docs and they are overwhelmingly filenames.
FILE_EXTENSIONS = {
    "zm", "c", "h", "py", "sh", "md", "json", "toml", "yaml", "yml", "sql", "txt", "mk",
    "out", "args", "err", "env", "properties", "lock", "log", "o", "d", "a", "so",
}

SPAN = re.compile(r"`([^`\n]+)`")
QUAL = re.compile(r"^([a-z][a-z0-9_]*)\.([a-z_][a-z0-9_]*)")
USE = re.compile(r"^\s*use\s+([a-z][a-z0-9_/]*)")
FENCE = re.compile(r"^\s*```")
# `pub fn`, `pub native fn`, `pub val`, `pub struct|enum|type`
EXPORT = re.compile(
    r"^pub\s+(?:native\s+)?(?:fn|val)\s+([a-z_][a-z0-9_]*)"
    r"|^pub\s+(?:struct|enum|type)\s+([A-Za-z_][A-Za-z0-9_]*)",
    re.M,
)


def current_modules(root):
    """name -> set of exported member names, for every module under lib/."""
    out = {}
    for path in sorted(root.glob("lib/**/*.zm")):
        text = path.read_text(errors="replace")
        names = {a or b for a, b in EXPORT.findall(text)}
        out[path.stem] = names
    return out


def retired_modules(root, present):
    """Module names deleted from lib/ at some point and not present now.

    Returns None (rather than an empty set) when git cannot answer, so the caller can say the check
    was skipped instead of silently reporting "nothing retired".
    """
    try:
        proc = subprocess.run(
            ["git", "log", "--diff-filter=D", "--name-only", "--format=", "--", "lib/"],
            cwd=root, capture_output=True, text=True, timeout=60, check=True,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    deleted = set()
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.endswith(".zm"):
            deleted.add(Path(line).stem)
    return deleted - set(present)


def main():
    root = Path(__file__).resolve().parent.parent
    modules = current_modules(root)
    retired = retired_modules(root, modules)

    problems = []
    for rel in HAND_WRITTEN:
        path = root / rel
        if not path.exists():
            problems.append(f"{rel}: listed in HAND_WRITTEN but missing")
            continue
        in_fence = False
        for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if FENCE.match(line):
                in_fence = not in_fence
                continue

            use = USE.match(line) if in_fence else None
            if use:
                name = use.group(1)
                if "/" not in name and name not in modules:
                    if retired and name in retired:
                        problems.append(
                            f"{rel}:{n}: `use {name}` names a module that was removed from lib/"
                        )
                    else:
                        problems.append(f"{rel}:{n}: `use {name}` names no module under lib/")

            for span in SPAN.findall(line):
                m = QUAL.match(span.strip())
                if not m:
                    continue
                mod, member = m.group(1), m.group(2)
                if (rel, span.strip()) in ALLOW:
                    continue
                if member in FILE_EXTENSIONS and span.strip() == f"{mod}.{member}":
                    continue
                if retired and mod in retired:
                    problems.append(
                        f"{rel}:{n}: `{span}` names `{mod}`, a module that was removed from lib/"
                    )
                elif mod in modules and member not in modules[mod]:
                    problems.append(
                        f"{rel}:{n}: `{span}` -- module `{mod}` exports no `{member}`"
                    )

    if retired is None:
        print("check_doc_modules: no git history available; retired-module check skipped",
              file=sys.stderr)

    if problems:
        print(f"check_doc_modules: {len(problems)} stale reference(s) in the hand-written docs",
              file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
