#!/bin/sh
# F274's gate: does a real input method reach `edit`'s pre-edit model?
#
# **The model's own tests cannot check this.** `tests/stdlib/ibus_test.zm` covers the decoder, which
# is pure, and `edit`'s tests cover the model, which was built before any platform -- but "the
# binding talks to a daemon that talks to an engine, and what comes back fits the model" involves
# three processes and no amount of unit testing sees it. This runs the fixture against the session's
# own ibus daemon.
#
# Two gates, because they need different things:
#
#   * **The plumbing**, which needs only a running daemon: the address file is found, the private
#     bus accepts us, a context is created, keys are offered and answered, and the context closes
#     without leaving a composition open. This is what would catch the trap the binding was written
#     around -- the bus address file is named for the *Wayland* display even for an X11 client -- and
#     it runs on any desktop with ibus, whatever engine is active.
#
#   * **A composition**, which needs an engine that composes. A plain `xkb:*` keyboard layout never
#     produces a pre-edit, so this half is **opt-in**: set `ZIMAN_IBUS_ENGINE` to an installed
#     composing engine (`ZIMAN_IBUS_ENGINE=anthy tests/run_ibus_test.sh`) and the script switches
#     to it, composes, and switches back. **Not done by default on purpose**: the active input
#     method is the user's, a test suite that changed it would change what their other windows do
#     while it ran, and a gate that edits the desktop to pass is not a gate anyone should trust by
#     accident.
#
# Skipped rather than failed where the parts are missing, as the window and a11y gates are.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-./ziman}
[ -x ./ziman-gui ] && BIN=./ziman-gui
PROGRAM=tests/integration/ibus/compose.zm

pass=0
fail=0

note_pass() { echo "PASS ibus_$1"; pass=$((pass + 1)); }
note_fail() { echo "FAIL ibus_$1 ($2)"; fail=$((fail + 1)); }
skip() { echo "SKIP ibus_$1 ($2)"; }

[ -x ./ziman-gui ] || { skip plumbing "no ziman-gui (build with \`make gui\`)"; exit 0; }

# The address the binding looks for, computed here the same way, so that a skip says "no daemon"
# rather than letting the fixture report a failure that is really an absent desktop.
address_file() {
    machine=$(cat /etc/machine-id 2>/dev/null) || return 1
    config=${XDG_CONFIG_HOME:-$HOME/.config}
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then
        echo "$config/ibus/bus/$machine-unix-$WAYLAND_DISPLAY"
    else
        number=${DISPLAY#*:}
        echo "$config/ibus/bus/$machine-unix-${number%%.*}"
    fi
}

if [ -z "${IBUS_ADDRESS:-}" ]; then
    file=$(address_file) || { skip plumbing "no /etc/machine-id"; exit 0; }
    [ -r "$file" ] || { skip plumbing "no ibus daemon (no $file)"; exit 0; }
fi

work="$(mktemp -d)"
engine_was=""
restore() {
    [ -n "$engine_was" ] && ibus engine "$engine_was" >/dev/null 2>&1
    rm -rf "$work"
}
trap restore EXIT INT TERM

composing=0
if [ -n "${ZIMAN_IBUS_ENGINE:-}" ]; then
    if ! command -v ibus >/dev/null 2>&1; then
        skip composition "no \`ibus\` command to switch the engine with"
    elif ! ibus list-engine 2>/dev/null | grep -q "^  ${ZIMAN_IBUS_ENGINE} "; then
        skip composition "the engine '$ZIMAN_IBUS_ENGINE' is not installed"
    else
        engine_was=$(ibus engine 2>/dev/null)
        ibus engine "$ZIMAN_IBUS_ENGINE" >/dev/null 2>&1
        # **Checked by reading the engine back, not by that command's exit status, which lies**:
        # `ibus engine anthy` switches the engine and exits 1 anyway, while switching to an
        # `xkb:*` layout exits 0. Trusting it skipped this whole half of the gate on a machine
        # where it had in fact worked.
        if [ "$(ibus engine 2>/dev/null)" = "$ZIMAN_IBUS_ENGINE" ]; then
            composing=1
        else
            skip composition "could not switch to '$ZIMAN_IBUS_ENGINE' (still $(ibus engine 2>/dev/null))"
        fi
    fi
fi

"$BIN" "$PROGRAM" > "$work/out.txt" 2>&1
status=$?

if [ "$status" -ne 0 ]; then
    note_fail plumbing "the fixture exited $status: $(head -2 "$work/out.txt" | tr '\n' ' ')"
    exit 1
fi

check() {
    if grep -qF "$2" "$work/out.txt"; then
        note_pass "$1"
    else
        note_fail "$1" "expected '$2' in the transcript: $(tr '\n' '|' < "$work/out.txt")"
    fi
}

check plumbing_opens "OPEN ok"
check plumbing_closes "DONE"
# **A context must not close while a composition is open**, which is the one way this binding could
# leave a user's desktop worse than it found it: a candidate list over a window that has gone.
check plumbing_leaves_nothing_composing "STILL_COMPOSING no"

if [ "$composing" -eq 0 ]; then
    skip composition "set ZIMAN_IBUS_ENGINE to an installed composing engine (e.g. anthy)"
    echo "----"
    echo "$pass passed, $fail failed"
    [ "$fail" -eq 0 ]
    exit $?
fi

# The method took keys. Not "took all of them": a method is allowed to hand a key back, and which
# ones depend on the engine.
taken=$(sed -n 's/^TAKEN \([0-9]*\)$/\1/p' "$work/out.txt")
if [ "${taken:-0}" -gt 0 ]; then
    note_pass composition_takes_keys
else
    note_fail composition_takes_keys "the engine took no keys at all"
fi

# A pre-edit arrived, with text in it. The *existence* of a non-empty pre-edit is the engine-
# independent fact; which characters are in it is anthy's or libpinyin's business.
if grep -q '^PREEDIT "..*"' "$work/out.txt"; then
    note_pass composition_shows_a_preedit
else
    note_fail composition_shows_a_preedit "no pre-edit with any text in it"
fi

# **The pre-edit is spliced where the caret is, not at the start.** The fixture composes into
# "name: ", so a `shown` that begins with the document is the `at` arithmetic holding.
if grep -q '^PREEDIT .* shown="name: ..*"' "$work/out.txt"; then
    note_pass composition_splices_at_the_caret
else
    note_fail composition_splices_at_the_caret "a pre-edit was not spliced after the document"
fi

# **The marked run, and conditionally, because marking is the engine's choice.** A method that
# converts marks the segment it is converting -- anthy sends it as an `IBUS_ATTR_TYPE_HINT` of
# value 2, which is the one finding in this binding that no documentation would have given -- but
# an engine is allowed never to mark anything. So: *if* a pre-edit reported a non-empty marked
# run, `edit.shown_highlight` must have translated it into the field's own coordinates. The
# alternative, asserting a highlight unconditionally, would gate the engine; asserting nothing
# leaves the attribute decoding unchecked by anything but a unit test of numbers I chose myself.
# **The marked run, asserted rather than asked about -- and the first version of this check was
# vacuous.** It looked for a non-empty `marked=[a,b)` in the transcript and only then required a
# highlight, which reads reasonably and tests nothing: the transcript is the output of the code
# under test, so a binding that decoded no marked run made the guard false and skipped the check
# that would have caught it. Mutation found it -- removing the hint decoding left the gate green at
# 8 passed, 0 failed.
#
# So it is unconditional. The fixture presses space, which is "convert" to every method that
# converts, and a conversion marks the segment being converted -- anthy sends it as an
# `IBUS_ATTR_TYPE_HINT` of value 2, which is the one finding here that no documentation would have
# given. An engine that composes but never converts is the wrong engine for this half of the gate,
# and failing is the right answer rather than skipping, because the alternative is what was just
# removed: a check that disappears exactly when it matters.
if grep -q '^HIGHLIGHT \[' "$work/out.txt"; then
    note_pass composition_marks_the_converted_run
else
    note_fail composition_marks_the_converted_run \
        "no marked run reached shown_highlight (is '$ZIMAN_IBUS_ENGINE' an engine that converts?)"
fi

if grep -q '^COMMIT "..*"' "$work/out.txt"; then
    note_pass composition_commits
else
    note_fail composition_commits "nothing was ever committed"
fi

# The committed text landed in the document and the composition is gone -- which is the whole
# instalment in one line.
if grep -q '^FINAL "name: ..*"' "$work/out.txt" && grep -q '^STILL_COMPOSING no$' "$work/out.txt"; then
    note_pass composition_lands_in_the_document
else
    note_fail composition_lands_in_the_document "the commit did not reach the document: $(grep '^FINAL' "$work/out.txt")"
fi

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
