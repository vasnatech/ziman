#!/usr/bin/env python3
"""Every name in CORELIB_MODULES must have a source make can find.

A module is bundled into the binary by listing it in `CORELIB_MODULES`, which asks make for
`build/<name>_source.h` through the generic rule `$(BUILD)/%_source.h: %.zm` -- resolved against
the `vpath` a few lines below it. A name with no `<name>.zm` on that path asks make for a file that
does not exist.

**Make says so plainly, and only on a clean build.** `charset` was listed there from the day it was
split into parts (2026-11-14) until 2026-11-18, four days in which every build succeeded: the
header generated before the split was still sitting in `build/`, so make had nothing to do, did
nothing, and said nothing. Deleting that one file produced
`No rule to make target 'build/charset_source.h'` -- which is what anyone cloning the repository
got, on a public repository, for four days.

The Makefile's own comment beside `THEME_PARTS` had even named `charset` as the exception that
proved the rule. A thing can be written down and not fixed; this runs instead.

A multi-file bundled module (`server`, `client`, `rest`, `charset`, `theme`) belongs in its own
`*_PARTS` variable and NOT in `CORELIB_MODULES` -- each part gets an explicit rule naming its
directory, and every part registers under the module's one name in `src/corelib.c`.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    makefile = open(os.path.join(ROOT, "Makefile"), encoding="utf-8").read()
    modules = re.search(r"^CORELIB_MODULES := (.*)$", makefile, re.M)
    vpath = re.search(r"^vpath %\.zm (.*)$", makefile, re.M)
    if modules is None or vpath is None:
        print("check_corelib_sources: cannot find CORELIB_MODULES or the vpath in the Makefile")
        return 1
    dirs = vpath.group(1).split()
    missing = [
        name
        for name in modules.group(1).split()
        if not any(os.path.exists(os.path.join(ROOT, d, name + ".zm")) for d in dirs)
    ]
    if missing:
        for name in missing:
            print(
                "check_corelib_sources: CORELIB_MODULES names '%s', but no '%s.zm' is on the vpath "
                "(%s). A clean build will fail with \"No rule to make target "
                "'build/%s_source.h'\". If it is a multi-file bundled module, it belongs in a "
                "*_PARTS variable instead." % (name, name, " ".join(dirs), name)
            )
        return 1
    print("check_corelib_sources: %d bundled module(s), every source findable" % len(modules.group(1).split()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
