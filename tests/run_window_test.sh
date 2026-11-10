#!/usr/bin/env bash
# The window test that needs a real display: a window really appears, and really shows the pixels it
# was given. Run by tests/run_tests.sh; prints PASS/FAIL/SKIP lines in its format.
#
# tests/stdlib/window_test.zm covers the event protocol, which is pure and runs anywhere. This
# covers the two halves that cannot be:
#
#   * that `present` reaches the screen with the channels in the right order, **checked by
#     screenshotting the real window and reading four pixels out of it**, because the failure mode
#     -- red showing as blue -- is invisible to any test that does not look; and
#   * that input really becomes a `window.Event`, checked by synthesising a key, a click, a scroll,
#     a motion, a resize and a close with `tests/tools/x11_inject.c` and reading back the lines the
#     program prints. **No Ziman code can press a key**, so nothing in the test suite could reach
#     this path, and the first run of it found two defects: a `free()` abort on every window close,
#     and the composed character arriving *before* the keystroke that produced it.
#
# Skips rather than fails when it cannot run, which is four different ways: no display, a ziman
# built without the backend (`make gui` adds it), no xprop/ImageMagick, or no cc and X11 headers for
# the injector. A gate that failed on a headless machine would be turned off within a week.
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

# --- input, which is the half no Ziman code can reach
#
# **The pixel checks run first on purpose**: the resize below moves the quadrant centres, so the two
# halves of this gate cannot be reordered. The close is last, which lets the program exit on its own
# and makes its status worth reading -- see window_clean_exit.
#
# The injector is compiled here rather than by the Makefile, because it must not make libX11 a
# dependency of anything but `make gui`.
if ! command -v cc >/dev/null 2>&1; then
    echo "SKIP window_events (no cc to build tests/tools/x11_inject.c)"
    kill "$pid" 2>/dev/null
    exit "$fail"
fi
if ! cc -O1 -o "$work/inject" tests/tools/x11_inject.c -lX11 2> "$work/cc.txt"; then
    echo "SKIP window_events (cannot build tests/tools/x11_inject.c: $(head -1 "$work/cc.txt"))"
    kill "$pid" 2>/dev/null
    exit "$fail"
fi
inject() { "$work/inject" "$id" "$@"; }

# Waits for a line rather than sleeping after each injection: the program polls on a 50 ms loop, so
# the delay is short but not zero, and a fixed sleep is what makes a gate flaky under load.
await() {
    local pattern="$1" _
    for _ in $(seq 1 60); do
        grep -qF -- "$pattern" "$work/out.txt" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

check_event() {
    local pattern="$1" label="$2"
    if await "$pattern"; then
        echo "PASS window_event_$label"
    else
        echo "FAIL window_event_$label (no line matching '$pattern')"
        fail=1
    fi
}

# A plain letter: the keysym arrives named, with no modifier claimed, and the text it composed
# arrives too. Both, because the module promises both for one keystroke.
inject key s
check_event "event KeyDown s shift=false control=false alt=false" "key"
check_event "event Char [s]" "char"

# **The character must follow the key, not precede it.** It preceded it until this check existed: a
# program that decides from the KeyDown whether to consume the character could not have worked, and
# no test over `decode_events` could have seen it, because the order is the native's.
key_line="$(grep -nF -- "event KeyDown s shift=false control=false" "$work/out.txt" | head -1 | cut -d: -f1)"
char_line="$(grep -nF -- "event Char [s]" "$work/out.txt" | head -1 | cut -d: -f1)"
if [ -n "$key_line" ] && [ -n "$char_line" ] && [ "$key_line" -lt "$char_line" ]; then
    echo "PASS window_event_char_follows_key"
else
    echo "FAIL window_event_char_follows_key (KeyDown at line ${key_line:-none}, Char at line ${char_line:-none})"
    fail=1
fi

# Shift: the keysym stays unshifted and the flag carries the shift, while the *text* is the capital.
# That split is the module's design -- a shortcut wants the key, a text field wants the character --
# so this is the case that proves neither is derived from the other.
inject key a shift
check_event "event KeyDown a shift=true control=false alt=false" "shift_flag"
check_event "event Char [A]" "shift_text"

# A named key, which composes no text at all.
inject key Left
check_event "event KeyDown Left shift=false control=false alt=false" "named_key"

# Control: the flag arrives and **no character does**, because a control code is not text. Asserted
# between the KeyDown and its KeyUp rather than as an absence anywhere, which would pass for the
# wrong reason the moment any other case stopped producing text.
inject key s control
check_event "event KeyDown s shift=false control=true" "control_flag"
if await "event KeyDown s shift=false control=true"; then
    between="$(awk '/event KeyDown s shift=false control=true/{on=1; next} on&&/event KeyUp s/{exit} on' "$work/out.txt" | grep -c 'event Char')"
    if [ "$between" = "0" ]; then
        echo "PASS window_event_control_has_no_char"
    else
        echo "FAIL window_event_control_has_no_char ($between Char events between the KeyDown and its KeyUp)"
        fail=1
    fi
fi

# The pointer, with its coordinates: a button event at the wrong position is the mistake that makes
# every click in an application land one widget away.
inject click 100 70 1
check_event "event PointerDown button=1 at 100,70" "click"
check_event "event PointerUp button=1 at 100,70" "release"
inject move 55 44
check_event "event PointerMove 55,44" "motion"

# The wheel, which X11 reports as buttons 4 and 5 and the native must not: a `Scroll` with a sign,
# both ways, so a backend that reported buttons would fail here rather than in an application.
#
# **The two are sent to different points on purpose.** Both at the same point, and flipping the sign
# in the native passed this pair: each check greps the whole log, so the line the *up* injection
# produced satisfied the check for down. Caught by flipping that sign to see the gate fail, which is
# the only way a check that passes for the wrong reason ever shows itself.
inject scroll 100 70 up
check_event "event Scroll vertical=1 horizontal=0 at 100,70" "scroll_up"
inject scroll 101 71 down
check_event "event Scroll vertical=-1 horizontal=0 at 101,71" "scroll_down"

# A real resize, not a synthetic one: the server generates this ConfigureNotify itself.
inject resize 300 200
check_event "event Resize 300x200" "resize"

# WM_DELETE_WINDOW, which is exactly what a close box sends -- and the program obeys it, so the line
# after it says that `with_window` gave the window back on a normal return.
inject close
check_event "event Close" "close"
check_event "closed" "with_window_returns"

# **The program's own exit status, which is the check that found the first defect here.** The window
# buffer was handed to `XDestroyImage`, which frees it with libc's free, while this build's `malloc`
# is the collector's -- so every close aborted with `free(): invalid size`. Nothing before this
# clause ever let the program exit on its own, so nothing ever saw it.
wait "$pid"
status=$?
pid=""
if [ "$status" -eq 0 ]; then
    echo "PASS window_clean_exit"
else
    echo "FAIL window_clean_exit (the program exited $status: $(tail -2 "$work/out.txt" | tr '\n' ' '))"
    fail=1
fi

exit "$fail"
