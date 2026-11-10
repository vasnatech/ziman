#!/usr/bin/env bash
# The window test that needs a real display: a window really appears, and really shows the pixels it
# was given. Run by tests/run_tests.sh; prints PASS/FAIL/SKIP lines in its format.
#
# tests/stdlib/window_test.zm covers the event protocol, which is pure and runs anywhere. This
# covers the half that cannot be: that `present` reaches the screen with the channels in the right
# order. **It is checked by screenshotting the real window and reading four pixels out of it**,
# because the failure mode -- red showing as blue -- is invisible to any test that does not look.
#
# Skips rather than fails when it cannot run, which is three different ways: no display, a ziman
# built without the backend (`make gui` adds it), or no xprop/ImageMagick. A gate that failed on a
# headless machine would be turned off within a week.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-./ziman}
# **The default binary has no window backend on purpose**, so this prefers the one `make gui`
# produces when it is there. Without it the run below reaches the "no window backend" message and
# skips, which is the honest outcome rather than a failure.
[ -x ./ziman-gui ] && BIN=./ziman-gui
PROGRAM=tests/integration/window/quadrants.zm

skip() { echo "SKIP window_present ($1)"; exit 0; }

[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || skip "no DISPLAY or WAYLAND_DISPLAY"
command -v xprop >/dev/null || skip "no xprop"
command -v import >/dev/null || skip "no ImageMagick import"

work="$(mktemp -d)"
trap 'rm -rf "$work"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT

"$BIN" "$PROGRAM" > "$work/out.txt" 2>&1 &
pid=$!

# Wait for the window to exist rather than sleeping a guessed amount: the readiness signal is the
# id line, and the window is mapped by the time it is printed.
id=""
for _ in $(seq 1 60); do
    id="$(grep -o 'window-id [0-9]*' "$work/out.txt" 2>/dev/null | awk '{print $2}')"
    [ -n "$id" ] && break
    sleep 0.1
done
if [ -z "$id" ]; then
    if grep -q 'no window backend' "$work/out.txt" 2>/dev/null; then
        skip "this ziman has no window backend -- build with 'make gui'"
    fi
    echo "FAIL window_present (no window id; output: $(head -2 "$work/out.txt" | tr '\n' ' '))"
    exit 1
fi

# **Waited for in a loop, not checked once and slept past.** The first version of this checked for
# the line once and slept half a second, which was enough on an idle machine and not enough under a
# full `make check-fast` -- so the capture caught an undrawn window and four pixel checks failed for
# a reason that had nothing to do with the code under test.
drawn=0
for _ in $(seq 1 100); do
    if grep -q '^presented' "$work/out.txt" 2>/dev/null; then drawn=1; break; fi
    sleep 0.1
done
if [ "$drawn" -eq 0 ]; then
    echo "FAIL window_present (the program never reported presenting: $(head -3 "$work/out.txt" | tr '\n' ' '))"
    exit 1
fi

fail=0
if ! xprop -id "$id" WM_NAME 2>/dev/null | grep -q 'ziman window test'; then
    echo "FAIL window_mapped (xprop does not see a window named 'ziman window test' at id $id)"
    fail=1
else
    echo "PASS window_mapped"
fi

if ! import -window "$id" "$work/shot.png" 2>/dev/null; then
    echo "FAIL window_present (could not screenshot window $id)"
    exit 1
fi

# The four quadrant centres. Compared as exact sRGB triples: these are flat fills, so anything but
# an exact match is a real difference rather than a rounding one.
# Retried, because a compositor may not have the window's contents available the instant it is
# mapped: a capture then reads black. Retrying a read is safe -- the program re-presents the same
# pattern for its whole run, so there is no later state to race against, only an earlier one.
#
# **It treats black as "not ready yet"**, which is why no quadrant in the pattern is black. A case
# that needs to assert a black pixel has to wait on something else -- there is no way to tell "not
# drawn" from "drawn black" through a screenshot.
read_pixel() {
    local x="$1" y="$2" got
    for _ in $(seq 1 25); do
        got="$(import -window "$id" png:- 2>/dev/null | convert png:- -format "%[pixel:p{$x,$y}]" info: 2>/dev/null)"
        case "$got" in
            ""|"gray(0)"|"srgb(0,0,0)") sleep 0.2 ;;
            *) echo "$got"; return ;;
        esac
    done
    echo "$got"
}

check_pixel() {
    local x="$1" y="$2" expected="$3" label="$4" got
    got="$(read_pixel "$x" "$y")"
    if [ "$got" = "$expected" ]; then
        echo "PASS window_pixel_$label"
    else
        echo "FAIL window_pixel_$label (expected $expected, got ${got:-nothing})"
        fail=1
    fi
}

check_pixel 80 60 "srgb(255,0,0)" top_left_red
check_pixel 240 60 "srgb(0,255,0)" top_right_green
check_pixel 80 180 "srgb(0,0,255)" bottom_left_blue
check_pixel 240 180 "srgb(255,255,255)" bottom_right_white

kill "$pid" 2>/dev/null
exit "$fail"
