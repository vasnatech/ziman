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
trap 'rm -rf "$work"; for p in ${pid:-} ${bytes_pid:-} ${region_pid:-} ${repaint_pid:-}; do kill "$p" 2>/dev/null; done' EXIT

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

# **There was a one-shot `import` to a file here, and it was deleted rather than retried.** Nothing
# ever read the file: `read_pixel` below captures the window for itself, and *retries*, for the
# documented reason that a compositor may not have the contents ready the instant a window is
# mapped. So this capture tested nothing the four checks below do not test better, and could only
# fail -- which it duly did, once, inside a `make check` under full load, having passed ten runs on
# an idle machine. A gate whose failure mode is "the machine was busy" costs more than it proves.
# Whether `import` exists at all is already checked at the top of this file.

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

# **The extreme corners, because the four centres above cannot see the frame move.** Giving
# `XShmPutImage` a destination of 40,40 instead of 0,0 -- a whole frame translated diagonally, and an
# ordinary mistake in a put -- changed none of those four: each sample sits far enough from the
# quadrant boundaries that the colour at it survives a 40 pixel shift, exactly as `ink_sum` survived
# a one-pixel text shift earlier in F270. Two pixels in from each corner cannot survive it: a frame
# put down at an offset leaves the first rows and columns unwritten, and a frame put at the wrong
# size leaves the last ones. Found by breaking the put and watching the gate pass.
check_pixel 2 2 "srgb(255,0,0)" corner_top_left
check_pixel 317 2 "srgb(0,255,0)" corner_top_right
check_pixel 2 237 "srgb(0,0,255)" corner_bottom_left
check_pixel 317 237 "srgb(255,255,255)" corner_bottom_right

# --- the other present path, against the same four triples
#
# `window.present` takes premultiplied floats and `present_bytes` takes bytes, and both repack into
# the visual's own masks in their own loop in `src/native_window.c`. The float one exists because a
# repaint was paying a 4 MB conversion and a 4 MB copy per frame for an intermediate nothing read --
# but adding it meant a second pixel path, and **a second pixel path with no screenshot of it is how
# a renderer gets the channel order wrong in one of two functions and passes everything.** The run
# above is the float path (the program's default, being the one a repaint uses); this is the byte
# one, asserted against the identical expected triples, which is also the only check of
# `native_window.c`'s claim that the two agree.
#
# A second short-lived window rather than a second mode of the first, so a failure says which path
# it was. `check_pixel` reads the global `id`, so it is lent to this window and given back.
# **`ZIMAN_WINDOW_NO_SHM` rides along on this run, so both transports are gated for free.** The
# native prefers a shared-memory image and copies through the socket only when it cannot have one --
# which on any local display means the copying path is unreachable, and a defect in the branch that
# exists for remote displays would wait for someone running over SSH to find it. The pack loop and
# the transport are independent, so one run of each pairing covers both of each: the run above is
# floats over shared memory, this one is bytes over the copying path, and both are held to the same
# four triples.
WINDOW_PRESENT_MODE=bytes ZIMAN_WINDOW_NO_SHM=1 "$BIN" "$PROGRAM" > "$work/bytes.txt" 2>&1 &
bytes_pid=$!
bytes_id=""
for _ in $(seq 1 60); do
    bytes_id="$(grep -o 'window-id [0-9]*' "$work/bytes.txt" 2>/dev/null | awk '{print $2}')"
    [ -n "$bytes_id" ] && break
    sleep 0.1
done
bytes_drawn=0
for _ in $(seq 1 100); do
    if grep -q '^presented bytes' "$work/bytes.txt" 2>/dev/null; then bytes_drawn=1; break; fi
    sleep 0.1
done
if [ -z "$bytes_id" ] || [ "$bytes_drawn" -eq 0 ]; then
    echo "FAIL window_present_bytes (no window or never presented: $(head -3 "$work/bytes.txt" | tr '\n' ' '))"
    fail=1
else
    float_id="$id"
    id="$bytes_id"
    check_pixel 80 60 "srgb(255,0,0)" bytes_nonshm_top_left_red
    check_pixel 240 60 "srgb(0,255,0)" bytes_nonshm_top_right_green
    check_pixel 80 180 "srgb(0,0,255)" bytes_nonshm_bottom_left_blue
    check_pixel 240 180 "srgb(255,255,255)" bytes_nonshm_bottom_right_white
    id="$float_id"
fi
kill "$bytes_pid" 2>/dev/null
bytes_pid=""

# --- the regional present, which has to be right in both directions
#
# `present_region` converts and pushes one rectangle and leaves the rest of the window as it was,
# which is two claims and needs both tested: the program presents the quadrants, floods its whole
# buffer magenta, and presents **only** a 40x40 square at 140,100. So the square must be magenta and
# the four quadrant centres must still hold their original colours -- and since the buffer behind it
# is *entirely* magenta, a region that pushed more than it was asked to turns a quadrant centre
# magenta and fails here rather than somewhere subtler.
WINDOW_PRESENT_MODE=region "$BIN" "$PROGRAM" > "$work/region.txt" 2>&1 &
region_pid=$!
region_id=""
for _ in $(seq 1 60); do
    region_id="$(grep -o 'window-id [0-9]*' "$work/region.txt" 2>/dev/null | awk '{print $2}')"
    [ -n "$region_id" ] && break
    sleep 0.1
done
region_drawn=0
for _ in $(seq 1 100); do
    if grep -q '^presented region' "$work/region.txt" 2>/dev/null; then region_drawn=1; break; fi
    sleep 0.1
done
if [ -z "$region_id" ] || [ "$region_drawn" -eq 0 ]; then
    echo "FAIL window_present_region (no window or never presented: $(head -3 "$work/region.txt" | tr '\n' ' '))"
    fail=1
else
    whole_id="$id"
    id="$region_id"
    check_pixel 160 120 "srgb(255,0,255)" region_inside_magenta
    check_pixel 145 105 "srgb(255,0,255)" region_corner_magenta
    check_pixel 80 60 "srgb(255,0,0)" region_outside_still_red
    check_pixel 240 60 "srgb(0,255,0)" region_outside_still_green
    check_pixel 80 180 "srgb(0,0,255)" region_outside_still_blue
    check_pixel 240 180 "srgb(255,255,255)" region_outside_still_white
    # Two pixels either side of the region's own edge, which is the only thing that catches a region
    # off by one: 139 must be untouched and 140 must be magenta.
    # **Both edges, because one edge catches only one direction of mistake.** The region is
    # x 140..179 and y 100..139, so 139 and 180 must be untouched while 140 and 179 must be magenta.
    # With only the left pair, a region one pixel too *wide* changed nothing either test looked at.
    check_pixel 139 120 "srgb(0,0,255)" region_edge_left_outside
    check_pixel 140 120 "srgb(255,0,255)" region_edge_left_inside
    check_pixel 179 120 "srgb(255,0,255)" region_edge_right_inside
    check_pixel 180 120 "srgb(255,255,255)" region_edge_right_outside
    check_pixel 160 139 "srgb(255,0,255)" region_edge_bottom_inside
    check_pixel 160 140 "srgb(255,255,255)" region_edge_bottom_outside
    id="$whole_id"
fi
kill "$region_pid" 2>/dev/null
region_pid=""

# --- the damage-driven repaint, which is the whole chain joined up
#
# `desktop.repaint` asks `raster.damage_between` what changed, redraws that rectangle with
# `draw_into_region` and sends it with `present_region`. Everything under it is covered by
# `tests/lang`; what only a real window can show is that the four of them joined together put the
# right pixels on a screen. The program reports the three cases a loop actually meets and ends with
# one quadrant recoloured, so the pixels below are the proof that a *partial* repaint of a window
# leaves the other three quadrants exactly as they were.
WINDOW_PRESENT_MODE=repaint "$BIN" "$PROGRAM" > "$work/repaint.txt" 2>&1 &
repaint_pid=$!
repaint_id=""
for _ in $(seq 1 60); do
    repaint_id="$(grep -o 'window-id [0-9]*' "$work/repaint.txt" 2>/dev/null | awk '{print $2}')"
    [ -n "$repaint_id" ] && break
    sleep 0.1
done
repaint_drawn=0
for _ in $(seq 1 100); do
    if grep -q '^presented repaint' "$work/repaint.txt" 2>/dev/null; then repaint_drawn=1; break; fi
    sleep 0.1
done
if [ -z "$repaint_id" ] || [ "$repaint_drawn" -eq 0 ]; then
    echo "FAIL window_repaint (no window or never presented: $(head -4 "$work/repaint.txt" | tr '\n' ' '))"
    fail=1
else
    check_line() {
        if grep -qF -- "$1" "$work/repaint.txt"; then
            echo "PASS window_repaint_$2"
        else
            echo "FAIL window_repaint_$2 (no line '$1'; got: $(grep '^repaint' "$work/repaint.txt" | tr '\n' '|'))"
            fail=1
        fi
    }
    # A first frame draws everything; the same scene again draws nothing; `forget` forces a full
    # redraw, which is what an Exposed event needs; and one changed quadrant damages that quadrant.
    check_line "repaint first 320x240" first_frame_is_whole
    check_line "repaint unchanged painted nothing" unchanged_paints_nothing
    check_line "repaint after forget 320x240" forget_forces_whole
    check_line "repaint one quadrant 160x120 at 160,120" one_quadrant_only

    driven_id="$id"
    id="$repaint_id"
    # Three quadrants untouched by the partial repaint, and the fourth showing its new colour.
    check_pixel 80 60 "srgb(255,0,0)" repaint_untouched_red
    check_pixel 240 60 "srgb(0,255,0)" repaint_untouched_green
    check_pixel 80 180 "srgb(0,0,255)" repaint_untouched_blue
    check_pixel 240 180 "srgb(255,255,0)" repaint_changed_to_yellow
    # Either side of the damaged quadrant's own edge.
    check_pixel 159 180 "srgb(0,0,255)" repaint_edge_outside
    check_pixel 161 180 "srgb(255,255,0)" repaint_edge_inside
    id="$driven_id"
fi
kill "$repaint_pid" 2>/dev/null
repaint_pid=""

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

# **A character above ASCII, which was dropped outright until 2026-11-13.** The native sends a code
# point and the Ziman half read it as a byte, so everything from 128 up failed the UTF-8 check on
# the way in and the event never arrived -- typing an accented letter did nothing, silently. No
# test over `decode_events` could have found it either, because the unit tests fed it the byte the
# decoder expected rather than the number the native actually sends. Only a real key through a real
# server says which of the two it is.
#
# Opportunistic, because which Latin-1 keysyms exist is the user's keyboard layout and not ours:
# the first one the server admits to having is used, and a layout with none skips rather than
# fails. Each candidate must sit at level 0 to be reached by a press with no modifier.
latin1_text=""
for candidate in "plusminus ±" "section §" "degree °" "sterling £" "mu µ" "eacute é" "adiaeresis ä" "ccedilla ç"; do
    # shellcheck disable=SC2086
    set -- $candidate
    if inject key "$1" 2>/dev/null; then
        latin1_text="$2"
        break
    fi
done
if [ -n "$latin1_text" ]; then
    check_event "event Char [$latin1_text]" "char_above_ascii"
else
    echo "SKIP window_event_char_above_ascii (no Latin-1 keysym on this keymap)"
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
# **Where the window is on the screen, which an input method needs and nothing else here does.**
# ibus's `SetCursorLocation` is defined in root coordinates and an application only knows where its
# caret is inside itself, so without this the candidate list appears at the corner of the display.
# Checked against an independent process asking the same server, which tests the binding rather
# than X's own semantics.
said="$(grep '^origin ' "$work/out.txt" | tail -1 | awk '{print $2}')"
truth="$(inject geometry 2>/dev/null)"
if [ -z "$said" ]; then
    echo "FAIL window_origin (the program printed no origin line)"
    fail=1
elif [ "$said" = "$truth" ]; then
    echo "PASS window_origin ($said)"
else
    echo "FAIL window_origin (window.origin said $said, the server says ${truth:-nothing})"
    fail=1
fi

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
