#!/usr/bin/env bash
# The terminal tests that need a real terminal: raw mode actually entered, a real escape sequence
# assembled into a key, and -- the one that matters -- the settings put back however the program
# ends. A .zm fixture cannot do any of it, because the suite's stdin is not a terminal and a Ziman
# program cannot signal itself. Run by tests/run_tests.sh; prints PASS/FAIL lines in its format.
#
# tests/stdlib/terminal_test.zm covers the escape-sequence table, which is pure. This covers the
# half that is not. See docs/design/terminal.md §3.1 and §10.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-./ziman}
fail=0

run_case() {
    local name="$1" mode="$2" program="$3"
    if ! BIN="$BIN" timeout 30 python3 -I tests/terminal_pty.py "$name" "$mode" "$program"; then
        fail=1
    fi
}

run_case terminal_keys           keys        tests/integration/terminal/keys.zm
run_case terminal_scope_restores after_scope tests/integration/terminal/after_scope.zm
run_case terminal_crash_restores crash       tests/integration/terminal/crash.zm
run_case terminal_sigterm_restores sigterm   tests/integration/terminal/keys.zm
run_case terminal_screen_sigterm   screen_sigterm tests/integration/terminal/screen.zm
run_case terminal_screen_crash     screen_crash   tests/integration/terminal/screen_crash.zm

exit "$fail"
