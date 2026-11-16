#!/usr/bin/env python3
"""The gallery's colourer knows every word the compiler does, and no word it does not.

**Why a gate rather than a shared list.** The keywords live in `src/lexer.c`, which is what the
language *is*; `tools/gen_highlight.py` reads them straight out of it for the website, and the
gallery cannot -- it is a Ziman program colouring text at runtime, with no C to parse. So its list
is a copy, and a copy with nothing checking it is a colourer that quietly stops being right the
first time the language grows a keyword. The only way to notice is to ask the compiler, which is
this.

It checks the same partition `gen_highlight.py` asserts: keywords, the types it deliberately
leaves plain, and the two literals. Every word in the table belongs to exactly one of the three,
and nothing claims a word the table does not have.

    python3 tools/check_gallery_keywords.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COLOURER = os.path.join(ROOT, "gallery", "ui", "desktop", "highlight.zm")


def compiler_keywords():
    source = open(os.path.join(ROOT, "src", "lexer.c"), encoding="utf-8").read()
    table = re.search(r"static const Keyword KEYWORDS\[\] = \{(.*?)\n\};", source, re.S)
    if not table:
        raise SystemExit("check_gallery_keywords: cannot find the KEYWORDS table in src/lexer.c")
    return set(re.findall(r'\{"([A-Za-z_]+)"', table.group(1)))


def listed(name):
    """The strings returned by a `pub fn <name>() -> [string]` in the colourer."""
    source = open(COLOURER, encoding="utf-8").read()
    body = re.search(
        r"pub fn " + name + r"\(\) -> \[string\] \{\s*return \[(.*?)\]\s*\}", source, re.S
    )
    if not body:
        raise SystemExit(f"check_gallery_keywords: cannot find {name}() in {COLOURER}")
    return set(re.findall(r'"([A-Za-z_]+)"', body.group(1)))


def main():
    table = compiler_keywords()
    keywords = listed("keywords")
    types = listed("types")
    literals = listed("literals")
    claimed = keywords | types | literals

    problems = []
    missing = table - claimed
    if missing:
        problems.append(
            "the compiler has keywords the gallery's colourer does not account for: "
            + ", ".join(sorted(missing))
        )
    invented = claimed - table
    if invented:
        problems.append(
            "the gallery's colourer claims words the compiler does not have: "
            + ", ".join(sorted(invented))
        )
    for a, b, an, bn in (
        (keywords, types, "keywords", "types"),
        (keywords, literals, "keywords", "literals"),
        (types, literals, "types", "literals"),
    ):
        both = a & b
        if both:
            problems.append(f"in both {an} and {bn}: " + ", ".join(sorted(both)))

    if problems:
        for p in problems:
            print("check_gallery_keywords: " + p, file=sys.stderr)
        print(
            "check_gallery_keywords: update gallery/ui/desktop/highlight.zm to match src/lexer.c",
            file=sys.stderr,
        )
        return 1
    print(
        f"check_gallery_keywords: {len(table)} keywords, split {len(keywords)} coloured, "
        f"{len(types)} left plain, {len(literals)} literal"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
