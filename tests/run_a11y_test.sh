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
check 'combo box: "Environment" = "staging" (7 chars)' combo_box
# Read back exactly, which is only possible if the offsets are characters: `café ☕` is 6 characters
# and 10 bytes, so a byte count walks past the end of the string.
# `café ☕` is 6 characters and 9 bytes, so the count is where a byte/character mix-up shows: the
# text comes back right either way, because the copy stops at the NUL.
check 'entry: "Note" = "café ☕" (6 chars)' non_ascii_value_round_trips
check 'password text: "Password" = "•••••••" (7 chars)' password_is_masked_on_the_bus
# **Three facts a row of a tree carries that no other control here does**: that it is a tree item,
# that it can be opened, and whether it is open. A reader with only the first cannot tell a folder
# from a request, and one with only the first two cannot tell an open folder from a shut one.
check 'tree: "Collections"' tree
check 'tree item: "api" [selected, expandable, expanded]' tree_branch_says_it_is_open
check 'tree item: "archive" [expandable]' tree_branch_says_it_is_shut
# A leaf must publish **neither** state: `expandable` on one offers a reader a way to open
# something that cannot be opened.
check 'tree item: "login"' tree_leaf_claims_nothing
# ...and that line would also match a leaf that published both, since the states print after it --
# so the absence is asserted on its own below.
check 'list: "Rows"' list
check 'list item: "first"' list_item
# **Which row is selected, which is the only thing that changes as a user arrows through a list.**
# A reader that cannot hear it reads the same four names however far down you go.
check 'list item: "second" [selected]' selection_is_published

# **The states, which are what a reader acts on.** A tree of names and roles says what is in a
# window; these say which of it you are on. `tree.zm` focuses the URL field, so exactly one node
# must report it -- and the body being a text area must say so, or a reader treats it as a one-line
# field and reads it as one.
check 'entry: "http://example.test" [focused]' focus_is_published
check 'button: "Revert" [disabled]' disabled_is_published
check 'entry: "Body" = "a' multiline_value_is_readable
check 'b" (3 chars) [multi line]' multiline_is_published

# **The value is a different fact from the name**, and a reader says both: the name to tell you what
# a control is, the text to tell you what it holds. A tree that published only the first would
# announce "URL, entry" and stop. These two lines are the pair that catches it -- a node whose
# `AtkText` answered nothing would keep its name and lose its contents.
if printf '%s\n' "$seen" | grep -q 'combo box: "Environment" = "staging"'; then
    echo "PASS a11y_a_choice_announces_what_is_chosen"
else
    echo "FAIL a11y_a_choice_announces_what_is_chosen (got: $(printf '%s' "$seen" | grep 'combo box' | tr '\n' '|'))"
    fail=1
fi
# The leaf's states, asserted as an absence because a prefix match cannot see one: `tree item:
# "login"` matches whatever follows it.
if printf '%s\n' "$seen" | grep -qE 'tree item: "login" \['; then
    echo "FAIL a11y_a_leaf_publishes_no_expansion_state ($(printf '%s' "$seen" | grep -F 'tree item: "login"' | tr '\n' '|'))"
    fail=1
else
    echo "PASS a11y_a_leaf_publishes_no_expansion_state"
fi

# **The only other check here that asserts an absence, and the reason the password field is in the
# fixture.** Everything else asks whether a fact arrived; this asks whether a secret did not. A
# masked field that drew bullets and published its contents would pass every other line on this
# page and read the password out loud to the first person using a screen reader.
if printf '%s\n' "$seen" | grep -qF 'hunter2'; then
    echo "FAIL a11y_a_password_never_reaches_the_bus ($(printf '%s' "$seen" | grep -F 'hunter2' | tr '\n' '|'))"
    fail=1
else
    echo "PASS a11y_a_password_never_reaches_the_bus"
fi
if [ "$(printf '%s\n' "$seen" | grep -c 'focused')" = "1" ]; then
    echo "PASS a11y_exactly_one_node_is_focused"
else
    echo "FAIL a11y_exactly_one_node_is_focused ($(printf '%s' "$seen" | grep -c 'focused') claim it)"
    fail=1
fi

# **The tree's own hierarchy, which is the half a role and a state cannot carry.** A reader asks a
# node for its children; a branch whose rows were siblings of it would say that three things exist
# and nothing about which is inside which -- an indented list, from the one side that cannot see
# the indent. The first version of `tree_widget` published exactly that and every other line on
# this page passed.
if printf '%s\n' "$seen" | awk '/tree item: "api"/{d=match($0,/[^ ]/); getline; if (match($0,/[^ ]/) > d) exit 0; exit 1}'; then
    echo "PASS a11y_a_branch_is_the_parent_of_its_rows"
else
    echo "FAIL a11y_a_branch_is_the_parent_of_its_rows (flat: $(printf '%s' "$seen" | grep 'tree item' | tr '\n' '|'))"
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
