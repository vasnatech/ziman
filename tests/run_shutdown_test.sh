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

# server.serve: serves a request through the handler function value and context,
# then a SIGTERM drains it and it exits 0.
check_serve() {
    local out
    out=$(mktemp)
    "$BIN" tests/shutdown/serve.zm > "$out" 2>&1 &
    local pid=$!
    for _ in $(seq 1 50); do grep -q "^listening" "$out" 2>/dev/null && break; sleep 0.1; done
    local port
    port=$(sed -n 's/^listening //p' "$out" | head -1)
    local body status_hello status_missing
    body=$(curl -s --max-time 3 "http://127.0.0.1:$port/hello")
    status_missing=$(curl -s --max-time 3 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/nope")
    kill -TERM "$pid" 2>/dev/null
    local waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 40 ]; do sleep 0.1; waited=$((waited + 1)); done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        echo "FAIL http_serve (still running 4s after SIGTERM)"; rm -f "$out"; return 1
    fi
    wait "$pid"; local status=$?
    if [ "$body" = "hi GET" ] && [ "$status_missing" = "404" ] && [ "$status" -eq 0 ] && grep -q stopped "$out"; then
        echo "PASS http_serve"; rm -f "$out"; return 0
    fi
    echo "FAIL http_serve (body='$body' missing=$status_missing exit=$status)"; sed 's/^/  /' "$out"; rm -f "$out"; return 1
}

rc=0
check_signal TERM || rc=1
check_signal INT || rc=1
check_serve || rc=1
exit $rc
