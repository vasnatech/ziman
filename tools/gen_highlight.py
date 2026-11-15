#!/usr/bin/env python3
"""Colour Ziman as Ziman: a tokenizer that marks up a `.zm` file for the web.

**Why this exists rather than a grammar borrowed from another language.** The TextMate grammar
under `editors/vscode/` colours Ziman in VS Code and nowhere else; everywhere else -- the 78
```ziman fences in the docs, every `.zm` in GitHub's file view, the taste on ziman.dev -- the
language renders as plain text. Borrowing was measured and declined on 2026-10-28: Rust's grammar
colours 70.9% of Ziman's keyword tokens, and the reason to say no was never the 29% but the label.
Tagging a fence ```rust says Ziman is a language it is not.

**The keywords are read from `src/lexer.c`, not written here.** That is the whole point of the
check below: a language that adds a keyword and forgets its highlighter has a highlighter that
quietly stops being right, and the only way to notice is to compare against the compiler. What it marks is
keywords, literals (strings, text blocks, numbers, `true`/`false`), comments and attributes.
**Types and identifiers are left plain on purpose**: the editor's grammar scopes them as
`support.type` rather than as keywords, a signature whose every type is bold is a signature nobody
reads, and telling a *type* from an identifier needs the compiler rather than a tokenizer.

**The markup is the vocabulary ziman.dev already had**: `<b>` for a keyword, `<span class="grn">`
for a literal, `<span class="dim">` for an attribute, `<span class="cmt">` for a comment. The first
three were hand-written into the page by someone colouring one example by hand; this writes them
from the source so the page cannot drift from the language.

Usage:

    python3 tools/gen_highlight.py websites/ziman.dev/taste.zm      # print the marked-up block
    python3 tools/gen_highlight.py --in-place websites/ziman.dev/index.html
    python3 tools/gen_highlight.py --check websites/ziman.dev/index.html

`--in-place` rewrites every `<pre data-ziman="path">...</pre>` in an HTML file from the file it
names, and `--check` says whether doing so would change anything -- which is what `make
highlight-check` runs, so a page and the program it shows cannot disagree.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def keywords():
    """Ziman's keywords, read from the compiler's own table.

    One source of truth, and the reason a `make` target checks this file against it: the table in
    `src/lexer.c` is what the language *is*, and anything else is a copy waiting to go stale.
    """
    source = open(os.path.join(ROOT, "src", "lexer.c"), encoding="utf-8").read()
    table = re.search(r"static const Keyword KEYWORDS\[\] = \{(.*?)\n\};", source, re.S)
    if not table:
        raise SystemExit("gen_highlight: cannot find the KEYWORDS table in src/lexer.c")
    return set(re.findall(r'\{"([A-Za-z_]+)"', table.group(1)))


# **The lexer's table split the way the editor's grammar splits it**, so the web and VS Code tell
# the same story about the same word. `editors/vscode/syntaxes/ziman.tmLanguage.json` scopes
# `int`/`string`/`Map` as `support.type`, not as `keyword.control` -- a signature whose every type
# is bold is a signature nobody can read, and the page's hand-coloured example made the same choice
# by eye before any of this existed.
#
# A type is left plain and a literal is coloured as one. The partition is asserted total below: a
# keyword added to the compiler and not named here stops this program rather than quietly becoming
# whatever the last `else` says.
TYPES = {
    "int", "float", "bool", "string", "bigint", "decimal", "byte",
    "Result", "Channel", "Map", "Set",
}
LITERALS = {"true", "false"}
KEYWORDS = keywords() - TYPES - LITERALS
UNCLASSIFIED = (TYPES | LITERALS) - keywords()
if UNCLASSIFIED:
    raise SystemExit(
        "gen_highlight: %s is named here and is not a keyword in src/lexer.c" % sorted(UNCLASSIFIED)
    )


def marked(source):
    """One `.zm` file as HTML, escaped and marked up. Returns the inside of a `<pre>`."""
    out = []
    i = 0
    n = len(source)
    while i < n:
        c = source[i]
        # A comment runs to the end of its line.
        if c == "/" and source.startswith("//", i):
            end = source.find("\n", i)
            end = n if end < 0 else end
            out.append(span("cmt", source[i:end]))
            i = end
            continue
        # A text block: three quotes, and the quotes inside it are ordinary text.
        if source.startswith('"""', i):
            end = source.find('"""', i + 3)
            end = n if end < 0 else end + 3
            out.append(span("grn", source[i:end]))
            i = end
            continue
        if c == '"':
            end = i + 1
            while end < n and source[end] != '"':
                # A backslash takes the next character with it, which is how `\"` stays inside.
                end += 2 if source[end] == "\\" else 1
            end = min(end + 1, n)
            out.append(span("grn", source[i:end]))
            i = end
            continue
        # An attribute: `#[derive(Json)]`, up to its closing bracket.
        if c == "#" and source.startswith("#[", i):
            end = source.find("]", i)
            end = n if end < 0 else end + 1
            out.append(span("dim", source[i:end]))
            i = end
            continue
        # A number, including the suffixes and separators the lexer takes (`3b`, `1_000`, `0xFF`).
        if c.isdigit():
            end = i
            while end < n and (source[end].isalnum() or source[end] in "._"):
                # A `.` is part of the number only when a digit follows it: `1.5` is one number and
                # `items.len` is not.
                if source[end] == "." and not (end + 1 < n and source[end + 1].isdigit()):
                    break
                end += 1
            out.append(span("grn", source[i:end]))
            i = end
            continue
        # A word: a keyword, or anything else.
        if c.isalpha() or c == "_":
            end = i
            while end < n and (source[end].isalnum() or source[end] == "_"):
                end += 1
            word = source[i:end]
            if word in KEYWORDS:
                out.append("<b>%s</b>" % escaped(word))
            elif word in LITERALS:
                out.append(span("grn", word))
            else:
                out.append(escaped(word))
            i = end
            continue
        out.append(escaped(c))
        i += 1
    return "".join(out)


def span(cls, text):
    return '<span class="%s">%s</span>' % (cls, escaped(text))


def escaped(text):
    """`&`, `<` and `>`, and nothing else.

    A quote needs escaping in an attribute and not in element content, and a page full of `&quot;`
    where it means `"` is a page nobody can read in a diff -- which is how the hand-written block
    this replaces was written, and the one thing about it worth keeping.
    """
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


BLOCK = re.compile(r'(<pre data-ziman(?:-out)?="([^"]+)">)(.*?)(</pre>)', re.S)


def rewritten(page):
    """The page with every `data-ziman` block filled from the file it names.

    **Two kinds, and the second is the point of the first.** `data-ziman` is a program, marked up;
    `data-ziman-out` is what that program prints, escaped and not marked up at all -- a highlighter
    colouring a program's *output* would be inventing syntax that is not there. Together they let a
    page show a program and its real output, with `--check` standing between them and a page that
    says something the repository no longer does.
    """
    def one(match):
        path = os.path.join(ROOT, match.group(2))
        if not os.path.exists(path):
            raise SystemExit("gen_highlight: %s names %s, which does not exist" % (page_name, match.group(2)))
        source = open(path, encoding="utf-8").read().rstrip("\n")
        body = escaped(source) if "-out=" in match.group(1) else marked(source)
        return match.group(1) + body + match.group(4)

    return BLOCK.sub(one, page)


# A fenced block in Markdown, with the file it shows named in a comment above it. Markdown has no
# attributes, so the marker is an HTML comment -- invisible everywhere it is rendered, and the same
# promise `data-ziman` makes in a page.
FENCE = re.compile(r"(<!-- ziman: ([^\s]+) -->\n```[a-z]*\n)(.*?)(```)", re.S)


def rewritten_markdown(page):
    """The Markdown with every marked fence filled from the file it names.

    **Not marked up, because Markdown has nowhere to put it**: GitHub renders a fence with whatever
    grammar it knows, and Ziman is not one of them (that is the whole entry this tool came from).
    What this keeps true is the *text* -- that the block in the README is the program it says it
    is, which is a claim the README makes in the sentence under it.
    """
    def one(match):
        path = os.path.join(ROOT, match.group(2))
        if not os.path.exists(path):
            raise SystemExit("gen_highlight: %s names %s, which does not exist" % (page_name, match.group(2)))
        return match.group(1) + open(path, encoding="utf-8").read().rstrip("\n") + "\n" + match.group(4)

    return FENCE.sub(one, page)


def main(argv):
    global page_name
    if len(argv) == 3 and argv[1] in ("--in-place", "--check"):
        page_name = argv[2]
        before = open(page_name, encoding="utf-8").read()
        after = rewritten_markdown(before) if page_name.endswith(".md") else rewritten(before)
        if argv[1] == "--check":
            if before == after:
                blocks = len(FENCE.findall(before)) if page_name.endswith(".md") else len(BLOCK.findall(before))
                print("gen_highlight: %d block(s) in %s match their sources" % (blocks, page_name))
                return 0
            print("gen_highlight: %s is not what its sources produce -- run "
                  "`python3 tools/gen_highlight.py --in-place %s`" % (page_name, page_name), file=sys.stderr)
            return 1
        open(page_name, "w", encoding="utf-8").write(after)
        found = FENCE.findall(after) if page_name.endswith(".md") else BLOCK.findall(after)
        print("gen_highlight: wrote %d block(s) into %s" % (len(found), page_name))
        return 0
    if len(argv) == 2 and not argv[1].startswith("-"):
        print("<pre>%s</pre>" % marked(open(argv[1], encoding="utf-8").read().rstrip("\n")))
        return 0
    print(__doc__.strip().splitlines()[-1], file=sys.stderr)
    print("usage: gen_highlight.py <file.zm> | --in-place <page.html> | --check <page.html>",
          file=sys.stderr)
    return 2


page_name = ""

if __name__ == "__main__":
    sys.exit(main(sys.argv))
