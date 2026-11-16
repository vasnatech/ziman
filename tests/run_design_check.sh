#!/bin/sh
# The gallery's `*_ui.zm` files, checked against the design dialect.
#
# **The dialect is curly's and the tools are curly's, and that is why this skips rather than
# fails when curly is not beside us.** `lib/ui` does not define the dialect -- a `*_ui.zm` file is
# a convention about how to *write* a UI, invented by the first application to want a builder, and
# the gallery is the second application to follow it. Owning the convention here would mean moving
# 1,326 lines of parser into this repository to gate four files; borrowing it costs this script.
#
# What the three tools assert, in the order they run:
#
#   lint_design   every design function is one expression of calls and literals: no `if`, no loop,
#                 no arithmetic in a body, no function-local `val`.
#   read_design   a builder can parse every component into a tree, with nothing left over. A
#                 construct nobody thought to forbid fails here and not above.
#   write_design  printing that tree back gives the source again, token for token. If it does not,
#                 the reader is guessing somewhere and a builder built on it would save something
#                 nobody wrote.
#
# Run from the repository root:  ./tests/run_design_check.sh ./ziman
set -eu

BIN="${1:-./ziman}"
CURLY="${ZIMAN_CURLY:-../curly/ziman}"
GALLERY="gallery/ui/desktop"

if [ ! -d "$GALLERY" ]; then
    echo "run_design_check.sh: no gallery at $GALLERY"
    exit 1
fi

if [ ! -f "$CURLY/tools/lint_design.zm" ]; then
    echo "SKIP run_design_check.sh: no curly checkout at $CURLY (set ZIMAN_CURLY); the dialect's tools live there"
    exit 0
fi

here="$(pwd)"
bin="$here/${BIN#./}"
gallery="$here/$GALLERY"

run() {
    tool="$1"
    shift
    # The tools take their own module path, and `.` for the ones that load a sibling.
    ( cd "$CURLY" && ZIMAN_MODULE_PATH=src/main/ziman:. "$bin" "tools/$tool.zm" -- "$@" )
}

fail=0
if ! out="$(run lint_design "$gallery" 2>&1)"; then
    fail=1
fi
echo "$out"
# **The lint reports by printing and still exits 0**, which is how `--fmt --check` behaves and how
# a gate built on it goes quietly green. The exit status is not the answer; the word is.
case "$out" in
    *"no findings"*) : ;;
    *) fail=1 ;;
esac

if ! run read_design --check "$gallery"; then
    fail=1
fi
if ! run write_design --check "$gallery"; then
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "run_design_check.sh: the gallery's design files do not hold to the dialect"
    exit 1
fi
echo "run_design_check.sh: the gallery's design files lint, parse and round-trip"
