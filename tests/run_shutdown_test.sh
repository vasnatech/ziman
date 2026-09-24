#!/usr/bin/env bash
# The signal tests: a real server process gets a real SIGTERM (then SIGINT) and
# must leave its accept loop, run its shutdown code and exit 0 -- something a
# fixture can't do, because a Ziman program can't signal itself. Run by
# tests/run_tests.sh; prints PASS/FAIL lines in the same format.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-./ziman}

check_signal() {
    local sig="$1" out
    out=$(mktemp)
    "$BIN" tests/shutdown/accept_loop.zm > "$out" 2>&1 &
    local pid=$!
    for _ in $(seq 1 50); do grep -q listening "$out" 2>/dev/null && break; sleep 0.1; done
    kill "-$sig" "$pid" 2>/dev/null
    local waited=0 status=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 40 ]; do sleep 0.1; waited=$((waited + 1)); done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        echo "FAIL shutdown_$sig (still running 4s after the signal)"; rm -f "$out"; return 1
    fi
    wait "$pid"; status=$?
    if [ "$status" -eq 0 ] && grep -q drained "$out"; then
        echo "PASS shutdown_$sig"; rm -f "$out"; return 0
    fi
    echo "FAIL shutdown_$sig (exit=$status)"; sed 's/^/  /' "$out"; rm -f "$out"; return 1
}

rc=0
check_signal TERM || rc=1
check_signal INT || rc=1
exit $rc
