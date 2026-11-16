#!/usr/bin/env python3
"""Every control `lib/ui` offers is shown by the gallery.

**This is the gate that makes the gallery maintain itself.** A widget set grows a control and the
gallery is the last place anyone thinks of, so the demo that would have caught the next defect is
the one nobody wrote. Asking the library what it offers -- `ziman --api controls`, which is
generated from the module and cannot go stale -- and then asking whether any demo calls it turns
"remember to add a demo" into a failing build.

It covers the two kinds of thing a control is built with:

  *_widget   a control placed in the tree
  *_layer    a control that opens over the window: a popup, a modal dialog

and it asks only that **some** demo calls each. The mapping is deliberately not one demo per
builder: `entry_widget` is called by both the text field and the text area, and `menu_title_widget`
by the menu demo alone. What matters is that nothing is unshown, not that the files line up.

    python3 tools/check_gallery_covers_controls.py [path/to/ziman]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMOS = os.path.join(ROOT, "gallery", "ui", "desktop", "demos")


def builders(binary):
    """The public builders `controls` offers, from the library's own API listing."""
    out = subprocess.run(
        [binary, "--api", "controls"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout
    found = set()
    for line in out.splitlines():
        m = re.match(r"fn ([a-z_]+(?:_widget|_layer))\(", line.strip())
        if m:
            found.add(m.group(1))
    if not found:
        raise SystemExit("check_gallery_covers_controls: no builders found in `--api controls`")
    return found


def called():
    """Every `controls.<name>(` the demos call."""
    out = set()
    for name in sorted(os.listdir(DEMOS)):
        if not name.endswith(".zm"):
            continue
        text = open(os.path.join(DEMOS, name), encoding="utf-8").read()
        out |= set(re.findall(r"controls\.([a-z_]+)\(", text))
    return out


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./ziman"
    want = builders(binary)
    have = called()
    missing = sorted(want - have)
    if missing:
        print(
            "check_gallery_covers_controls: no demo shows "
            + ", ".join(missing)
            + "\n    add one under gallery/ui/desktop/demos/ and an arm to demos/all.zm",
            file=sys.stderr,
        )
        return 1
    print(f"check_gallery_covers_controls: all {len(want)} controls have a demo")
    return 0


if __name__ == "__main__":
    sys.exit(main())
