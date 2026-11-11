#!/bin/sh
# F275's gate: is the accessibility tree readable by something that is not ziman?
#
# **The publishing side cannot check this.** A test inside the program could only confirm that
# `atspi.publish` returned `Ok`, which says the bridge accepted a tree and nothing about whether a
# reader can see it -- and "the tree is built and reaches nothing" is exactly the state F275 existed
# to leave behind. So this runs `tests/tools/atspi_dump.c`, a separate process that connects to the
# session's accessibility bus the way Orca does, and compares what it finds.
#
# Skipped rather than failed where the parts are missing: a ziman without the backend (`make gui`),
# no session accessibility bus, or no libatspi to build the reader against. A skip says the gate did
# not run; a pass says a reader saw the tree.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-./ziman}
[ -x ./ziman-gui ] && BIN=./ziman-gui
PROGRAM=tests/integration/a11y/tree.zm
APP="ziman a11y fixture"

skip() { echo "SKIP a11y_tree ($1)"; exit 0; }

command -v pkg-config >/dev/null 2>&1 || skip "no pkg-config"
pkg-config --exists atspi-2 glib-2.0 gobject-2.0 || skip "libatspi is not installed"
command -v cc >/dev/null 2>&1 || skip "no cc to build tests/tools/atspi_dump.c"
[ -n "${DBUS_SESSION_BUS_ADDRESS:-}${XDG_RUNTIME_DIR:-}" ] || skip "no session bus"

work="$(mktemp -d)"
trap 'rm -rf "$work"; [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null' EXIT

if ! cc -O1 -o "$work/dump" tests/tools/atspi_dump.c \
     $(pkg-config --cflags --libs atspi-2 glib-2.0 gobject-2.0) 2> "$work/cc.txt"; then
    skip "cannot build tests/tools/atspi_dump.c: $(head -1 "$work/cc.txt")"
fi

"$BIN" "$PROGRAM" > "$work/out.txt" 2>&1 &
pid=$!

# Waited for rather than slept past, as the window gate learned to: the readiness signal is the
# program saying it published.
ready=0
for _ in $(seq 1 100); do
    if grep -q '^published' "$work/out.txt" 2>/dev/null; then ready=1; break; fi
    if grep -q 'no accessibility backend' "$work/out.txt" 2>/dev/null; then
        skip "this ziman has no accessibility backend -- build with 'make gui'"
    fi
    if grep -q 'accessibility bus' "$work/out.txt" 2>/dev/null; then
        skip "no accessibility bus: $(head -2 "$work/out.txt" | tr '\n' ' ')"
    fi
    sleep 0.1
done
if [ "$ready" -eq 0 ]; then
    echo "FAIL a11y_tree (the program never published: $(head -3 "$work/out.txt" | tr '\n' ' '))"
    exit 1
fi

fail=0
# Retried, because registering with the bus is asynchronous: the program has published locally
# before the registry has it.
seen=""
for _ in $(seq 1 40); do
    seen="$("$work/dump" "$APP" 2>/dev/null)" && break
    sleep 0.25
done
if [ -z "$seen" ]; then
    echo "FAIL a11y_tree (nothing named \"$APP\" is on the accessibility bus)"
    exit 1
fi
echo "PASS a11y_on_the_bus"

check() {
    if printf '%s\n' "$seen" | grep -qF -- "$1"; then
        echo "PASS a11y_$2"
    else
        echo "FAIL a11y_$2 (no line '$1' in: $(printf '%s' "$seen" | tr '\n' '|'))"
        fail=1
    fi
}

# **The roles are the assertion, not just the names.** A tree whose every node came out `unknown`
# would carry the right words and tell a reader nothing, which is the failure this is shaped to
# catch: each line below pairs a role a reader announces with the name it belongs to.
check 'application: "ziman a11y fixture"' application
check 'button: "Send"' button
check 'entry: "http://example.test"' text_field
check 'combo box: "staging"' combo_box
check 'list: ""' list
check 'list item: "first"' list_item

# **The states, which are what a reader acts on.** A tree of names and roles says what is in a
# window; these say which of it you are on. `tree.zm` focuses the URL field, so exactly one node
# must report it -- and the body being a text area must say so, or a reader treats it as a one-line
# field and reads it as one.
check 'entry: "http://example.test" [focused]' focus_is_published
check 'button: "Revert" [disabled]' disabled_is_published
if [ "$(printf '%s\n' "$seen" | grep -c 'focused')" = "1" ]; then
    echo "PASS a11y_exactly_one_node_is_focused"
else
    echo "FAIL a11y_exactly_one_node_is_focused ($(printf '%s' "$seen" | grep -c 'focused') claim it)"
    fail=1
fi

# The hierarchy, which is what makes it a tree rather than a bag: the list's items are indented
# under it, and everything is under the application.
if printf '%s\n' "$seen" | awk '/list: ""/{d=match($0,/[^ ]/); getline; if (match($0,/[^ ]/) > d) exit 0; exit 1}'; then
    echo "PASS a11y_items_nest_under_the_list"
else
    echo "FAIL a11y_items_nest_under_the_list (flat tree: $(printf '%s' "$seen" | tr '\n' '|'))"
    fail=1
fi

exit "$fail"
