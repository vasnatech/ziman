#!/usr/bin/env bash
# memcheck over the few things it is worth running on, with tools/valgrind.supp.
#
# Narrow on purpose. valgrind runs a fixture that takes 10 ms natively in about 1.5 s, so sweeping
# all 889 of them is tens of minutes against `make asan`'s 42 seconds, and it collides with
# run_tests.sh's 5-second per-fixture timeout. What it buys that the sanitizer targets cannot is
# memcheck's uninitialised-value checking -- ASan does not do it and MSan is not in use -- plus a
# leak checker, which is what found A26 (docs/reviews/tasks.md F23).
#
# The three fixtures here are the ones that exercise the conservative stack scanner hardest: each
# builds a stack trace, so the scan runs over a deep stack full of live roots. The unit tests are
# the arena/scanner/lookup layer underneath it.
#
# usage: tests/run_valgrind.sh [ziman-binary]   (default ./ziman-vg, which `make valgrind` builds
# with -fno-omit-frame-pointer -- the suppressions do not match without it, see valgrind.supp)
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${1:-./ziman-vg}
SUPP=tools/valgrind.supp
pass=0
fail=0

if ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind: skipped -- valgrind is not on PATH"
    exit 0
fi

# `--error-exitcode` is no use here: these fixtures exit non-zero by design (they end in a runtime
# error, which is the point), so the verdict comes from memcheck's own summary line instead.
# $1 = name, $2 = "leaks" or "noleaks", rest = the command.
#
# Leak checking is on for the unit tests and off for a whole `ziman` run, and that is not timidity:
# this interpreter never frees an AST, a Value or a module graph, by design (docs/DESIGN.md's
# "Memory reclamation"), so a leak check over a program run reports the design -- 555 loss records
# on a three-line fixture -- and drowns the thing worth looking at. The same conclusion F24 reached
# for LeakSanitizer. Uninitialised-value checking, which is why this target exists, is on for both.
check() {
    local name="$1" leaks="$2"; shift 2
    local log="/tmp/ziman_vg_$name.log"
    local leakargs=(--leak-check=no)
    [ "$leaks" = "leaks" ] && leakargs=(--leak-check=full --show-leak-kinds=definite --errors-for-leak-kinds=definite)
    valgrind "${leakargs[@]}" \
             --suppressions="$SUPP" --log-file="$log" "$@" >/dev/null 2>&1
    local summary
    summary=$(grep -oE 'ERROR SUMMARY: [0-9]+ errors' "$log" | tail -1 | grep -oE '[0-9]+' | head -1)
    if [ "${summary:-1}" = "0" ]; then
        echo "PASS $name ($(grep -oE 'suppressed: [0-9]+ from [0-9]+' "$log" | tail -1))"
        pass=$((pass + 1))
    else
        echo "FAIL $name -- $(grep -E 'ERROR SUMMARY' "$log" | tail -1)"
        sed -n '/^==[0-9]*== [A-Z]/,+6p' "$log" | head -14 | sed 's/^/  /'
        fail=$((fail + 1))
    fi
}

for name in error_stack_trace error_stack_trace_recursive stack_depth_trace_bounded; do
    args=()
    [ -f "tests/fixtures/$name.args" ] && read -r -a args < "tests/fixtures/$name.args"
    check "$name" noleaks "$BIN" "${args[@]}" "tests/fixtures/$name.zm"
done

for t in /tmp/ziman_gc_scan_test /tmp/ziman_gc_lookup_test /tmp/ziman_arena_test /tmp/ziman_gc_object_test; do
    [ -x "$t" ] || continue
    check "$(basename "$t")" leaks "$t"
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
