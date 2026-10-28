#!/usr/bin/env python3
"""Check a static site directory before it is published.

A site under websites/ has no build step, so nothing else in the repository would notice if its page
stopped parsing, lost its title or lost a file: `make check` does not look at websites/. Both deploy
workflows run this, and so can you:

    python3 tools/check_site.py websites/ziman.dev

It checks only what is cheap and certain -- presence, the title, the CNAME text, and that the HTML
has no unclosed or stray tags. It deliberately does not try to validate the page's claims; the
things that can drift are listed in websites/README.md under "Keeping the page true".
"""

import io
import os
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


def check(directory):
    problems = []

    def require_file(name):
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            problems.append("missing %s" % path)
            return None
        return io.open(path, encoding="utf-8").read()

    page = require_file("index.html")
    require_file("logo.svg")

    cname = os.path.join(directory, "CNAME")
    if os.path.isfile(cname):
        # The file does not set a Pages custom domain (see websites/README.md), but it records which
        # domain the directory is for, so a silent change to it should still be caught.
        want = os.path.basename(directory)
        got = io.open(cname, encoding="utf-8").read().strip()
        if got != want:
            problems.append("CNAME says %r, expected %r (the directory's own name)" % (got, want))

    if page is not None:
        if "<title>" not in page:
            problems.append("index.html has no <title>")
        walk = Walk()
        walk.feed(page)
        for tag, (line, col) in walk.open_tags:
            problems.append("index.html: <%s> at line %d col %d is never closed" % (tag, line, col))
        for tag, (line, col) in walk.stray:
            problems.append("index.html: </%s> at line %d col %d closes nothing" % (tag, line, col))

    return problems


def main(argv):
    if len(argv) != 2:
        print("usage: check_site.py <site directory>", file=sys.stderr)
        return 2
    directory = argv[1].rstrip("/")
    if not os.path.isdir(directory):
        print("check_site.py: %s is not a directory" % directory, file=sys.stderr)
        return 2
    problems = check(directory)
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
