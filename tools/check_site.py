#!/usr/bin/env python3
"""Check a static site directory before it is published.

A site under websites/ has no build step, so nothing else in the repository would notice if its page
stopped parsing, lost its title or lost a file: `make check` does not look at websites/. Both deploy
workflows run this, and so can you:

    python3 tools/check_site.py websites/ziman.dev
    python3 tools/check_site.py websites/ziman.dev --examples docs/lib/examples

It checks only what is cheap and certain -- presence, the title, the CNAME text, and that the HTML
has no unclosed or stray tags. It deliberately does not try to validate the page's claims; the
things that can drift are listed in websites/README.md under "Keeping the page true".
"""

import io
import os
import re
import sys
from html.parser import HTMLParser

# Tags that never close, so an unmatched one is not an error.
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source",
        "track", "wbr"}


class Walk(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.open_tags = []
        self.stray = []

    def handle_starttag(self, tag, attrs):
        if tag not in VOID:
            self.open_tags.append((tag, self.getpos()))

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if self.open_tags and self.open_tags[-1][0] == tag:
            self.open_tags.pop()
        else:
            self.stray.append((tag, self.getpos()))


def check_examples(page, examples_dir, problems):
    """Every worked example is linked from the page, and every link points at one that exists.

    The list on ziman.dev is hand-written HTML, so it drifts the moment a module gains an example
    -- silently, because a stale list still renders perfectly well. This makes the drift fail the
    deploy instead: the page is checked against the directory rather than against someone's
    memory."""
    on_disk = set()
    for folder in sorted(os.listdir(examples_dir)):
        d = os.path.join(examples_dir, folder)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith("_example.zm"):
                on_disk.add("%s/%s" % (folder, fn))
    # Digits belong in the name: `a11y` is how accessibility is spelled everywhere, and the first
    # version of this pattern was `[a-z_]+`, which could not see that file at all -- so the page was
    # reported as missing a link it had. The directory side has always used `endswith`, so only this
    # half was blind.
    linked = set(re.findall(r"examples/([a-z0-9]+/[a-z0-9_]+_example\.zm)", page))
    for missing in sorted(on_disk - linked):
        problems.append("index.html does not link docs/lib/examples/%s" % missing)
    for stale in sorted(linked - on_disk):
        problems.append("index.html links docs/lib/examples/%s, which does not exist" % stale)


def srgb_luminance(hex_colour):
    """WCAG 2 relative luminance of a `#rrggbb`."""
    h = hex_colour.lstrip("#")
    channels = [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    lin = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def contrast(a, b):
    la, lb = srgb_luminance(a), srgb_luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def check_code_contrast(page, name, problems):
    """Every colour the code block draws text in, against the background it draws it on.

    **The page's own syntax colouring failed this and nothing said so.** `--accent` and
    `--structure` are darkened for contrast on paper and were being used inside `pre`, whose
    background is dark in both themes: a keyword came out at 3.98:1 and a literal at 2.66:1, the
    second below even the 3:1 floor for large text. The fix is two code-only variables with no
    theme variants; this is what keeps them that way.

    Only the code block, because it is the one place on the page where a foreground and a
    background are both named in the stylesheet and can be checked without rendering anything.
    """
    backgrounds = re.findall(r"--code-bg:\s*(#[0-9a-fA-F]{6})", page)
    if not backgrounds:
        return
    hues = {}
    for var in ("code-fg", "code-dim", "code-key", "code-lit"):
        found = re.findall(r"--%s:\s*(#[0-9a-fA-F]{6})" % var, page)
        if found:
            hues[var] = found
    for i, bg in enumerate(backgrounds):
        for var, values in sorted(hues.items()):
            fg = values[i] if len(values) > i else values[0]
            ratio = contrast(fg, bg)
            if ratio < 4.5:
                problems.append(
                    "%s: --%s (%s) on --code-bg (%s) is %.2f:1, under WCAG AA's 4.5:1 for body "
                    "text" % (name, var, fg, bg, ratio))


def check(directory, examples_dir=None):
    problems = []

    def require_file(name):
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            problems.append("missing %s" % path)
            return None
        return io.open(path, encoding="utf-8").read()

    index = require_file("index.html")
    require_file("logo.svg")

    if examples_dir is not None and index is not None:
        check_examples(index, examples_dir, problems)

    cname = os.path.join(directory, "CNAME")
    if os.path.isfile(cname):
        # The file sets no custom domain on either host (see websites/README.md), but it records
        # which domain the directory is for, so a silent change to it should still be caught.
        want = os.path.basename(directory)
        got = io.open(cname, encoding="utf-8").read().strip()
        if got != want:
            problems.append("CNAME says %r, expected %r (the directory's own name)" % (got, want))

    # Every page, not just index.html: 404.html is served to real visitors too, and nothing else
    # would notice if it stopped parsing.
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".html"):
            continue
        page = io.open(os.path.join(directory, name), encoding="utf-8").read()
        if "<title>" not in page:
            problems.append("%s has no <title>" % name)
        walk = Walk()
        walk.feed(page)
        for tag, (line, col) in walk.open_tags:
            problems.append("%s: <%s> at line %d col %d is never closed" % (name, tag, line, col))
        for tag, (line, col) in walk.stray:
            problems.append("%s: </%s> at line %d col %d closes nothing" % (name, tag, line, col))
        check_code_contrast(page, name, problems)

    return problems


def main(argv):
    examples_dir = None
    if len(argv) == 4 and argv[2] == "--examples":
        examples_dir = argv[3].rstrip("/")
        argv = argv[:2]
    if len(argv) != 2:
        print("usage: check_site.py <site directory> [--examples <examples directory>]",
              file=sys.stderr)
        return 2
    directory = argv[1].rstrip("/")
    if not os.path.isdir(directory):
        print("check_site.py: %s is not a directory" % directory, file=sys.stderr)
        return 2
    problems = check(directory, examples_dir)
    for p in problems:
        print("check_site.py: %s" % p, file=sys.stderr)
    if problems:
        print("check_site.py: %s is NOT fit to publish (%d problem(s))" % (directory, len(problems)),
              file=sys.stderr)
        return 1
    print("check_site.py: %s is fit to publish" % directory)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
