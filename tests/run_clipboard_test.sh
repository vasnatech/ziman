#!/usr/bin/env bash
# The clipboard, which can only be tested between two processes.
#
# X11 has no clipboard: it has selections, and a selection is a live promise by one window to hand
# its text to another when asked. So there is nothing here that one process can check about itself
# -- a single program that put text somewhere and read it back would pass with the whole protocol
# missing. **Every case below is two programs on one display**, which is the only arrangement in
# which the thing being claimed is the thing being measured.
#
# The three cases, and what each one would catch:
#
#   * **copy, then paste from another program.** The ordinary case, and the one that fails if
#     `SelectionRequest` is not answered from the poll loop. A program that takes the selection and
#     then goes quiet looks to every other window like a clipboard holding nothing.
#   * **paste from ourselves.** The owner's answer comes from the owner's event loop, so when owner
#     and asker are the same process that loop is the one blocked asking. Without the short-circuit
#     in `native_window_clipboard_get_raw` this hangs for the whole timeout on every copy-and-paste
#     inside one window, which is the common case rather than an edge one.
#   * **a program that is not ziman reads what ziman copied, and ziman reads what it wrote.** This
#     is the task's done condition, and it is the case two ziman programs cannot stand in for:
#     both ends agreeing on a mistake looks exactly like both ends being right. Needs `xclip`, and
#     skips without it.
#
# **There is no case here asserting that the clipboard is empty, or that a selection dies with the
# program that owned it.** Both are true of the bare X11 protocol and both were written, and both
# failed on a GNOME desktop -- because a clipboard manager takes ownership when a program exits,
# which is the whole reason clipboard managers exist. The test was wrong about the world rather
# than the code being wrong about the protocol. A gate asserting either would pass on a bare
# display and fail on every real one.
#
# Run by tests/run_tests.sh; prints PASS/FAIL/SKIP lines in its format. Skips rather than fails
# when it cannot run: a gate that failed on a headless machine would be turned off within a week.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-./ziman}
[ -x ./ziman-gui ] && BIN=./ziman-gui

skip() { echo "SKIP clipboard ($1)"; exit 0; }

[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || skip "no DISPLAY or WAYLAND_DISPLAY"
"$BIN" tests/integration/window/clip_get.zm 2>&1 | grep -q "no window backend" && \
    skip "this ziman has no window backend (make gui)"

work="$(mktemp -d)"
owner_pid=""
trap 'rm -rf "$work"; [ -n "$owner_pid" ] && kill "$owner_pid" 2>/dev/null' EXIT

fails=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1: expected [$2], got [$3]"
        fails=$((fails + 1))
    fi
}

said="ziman clipboard $$"

# --- one program owns it, another asks
"$BIN" tests/integration/window/clip_put.zm -- "$said" > "$work/owner.txt" 2>&1 &
owner_pid=$!
for _ in $(seq 1 100); do
    grep -q '^put ' "$work/owner.txt" 2>/dev/null && break
    sleep 0.05
done
if ! grep -q '^put ' "$work/owner.txt" 2>/dev/null; then
    echo "FAIL clipboard_the_owner_started: owner never reported (see below)"
    cat "$work/owner.txt"
    exit 1
fi

got="$("$BIN" tests/integration/window/clip_get.zm 2>&1 | sed -n 's/^got \[\(.*\)\]$/\1/p')"
check clipboard_text_crosses_between_two_programs "$said" "$got"

# **Twice, because a selection is a promise and not a handover.** A first paste that worked and a
# second that came back empty would mean the owner answered once and then lost what it promised,
# which is exactly what happens if the text is freed after the first request.
got="$("$BIN" tests/integration/window/clip_get.zm 2>&1 | sed -n 's/^got \[\(.*\)\]$/\1/p')"
check clipboard_the_owner_can_answer_more_than_once "$said" "$got"

# --- a program that is not ziman, which is what the task is actually for
if command -v xclip >/dev/null; then
    got="$(timeout 5 xclip -o -selection clipboard 2>/dev/null)"
    check clipboard_another_application_can_read_what_ziman_copied "$said" "$got"
else
    echo "SKIP clipboard_another_application_can_read_what_ziman_copied (no xclip)"
fi

# --- a program pasting from itself
#
# **This case takes the clipboard away from the owner**, because it has to: the thing being checked
# is what happens when owner and asker are one process. So it runs after every case that wants the
# owner's text, and the first ordering of this file had it running before them -- which failed as a
# protocol bug and was a test writing over its own fixture.
mine="itself $$"
started=$(date +%s%N)
got="$("$BIN" tests/integration/window/clip_get.zm -- "ignored" "$mine" 2>&1 | sed -n 's/^got \[\(.*\)\]$/\1/p')"
spent=$(( ($(date +%s%N) - started) / 1000000 ))
check clipboard_a_program_can_paste_what_it_copied "$mine" "$got"
# The short-circuit, measured rather than asserted: without it this waits out the whole timeout.
if [ "$spent" -lt 400 ]; then
    echo "PASS clipboard_pasting_from_ourselves_does_not_wait"
else
    echo "FAIL clipboard_pasting_from_ourselves_does_not_wait: took ${spent}ms"
    fails=$((fails + 1))
fi

kill "$owner_pid" 2>/dev/null
owner_pid=""

# --- and the other direction
if command -v xclip >/dev/null; then
    theirs="xclip wrote this $$"
    # **The redirection is load-bearing and is not tidiness.** `xclip -i` forks and stays alive to
    # own the selection -- it has to, because on X11 an owner is a live process -- and the copy it
    # leaves behind inherits this script's stdout. That is the pipe `tests/run_tests.sh` reads
    # from, so the suite's `while read` never sees EOF and the whole gate hangs *having printed
    # every PASS*: a green run that never ends. It sat for nineteen minutes before anyone looked.
    #
    # `-l 1`, which exits after serving one paste, looks like the tidier answer and is not: the
    # desktop's clipboard manager asks for the content the moment xclip takes it, so the one
    # serving is spent before our reader asks. Hence a lingering owner, killed by pid below --
    # and only the ones this script started, because the user may have an xclip of their own.
    xclip_before="$(pgrep -x xclip 2>/dev/null | sort)"
    printf '%s' "$theirs" | timeout 5 xclip -i -selection clipboard >/dev/null 2>&1
    got="$("$BIN" tests/integration/window/clip_get.zm 2>&1 | sed -n 's/^got \[\(.*\)\]$/\1/p')"
    check clipboard_ziman_can_read_what_another_application_wrote "$theirs" "$got"
    for p in $(pgrep -x xclip 2>/dev/null); do
        printf '%s\n' "$xclip_before" | grep -qx "$p" || kill "$p" 2>/dev/null
    done
else
    echo "SKIP clipboard_ziman_can_read_what_another_application_wrote (no xclip)"
fi

[ "$fails" -eq 0 ] || exit 1
