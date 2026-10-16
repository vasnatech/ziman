#!/usr/bin/env bash
# ab against portable_server.zm for one binary -- the fixed workload for comparing interpreter
# binaries across versions (see that file for why the real backend cannot serve that purpose).
# usage: portable_server.sh <binary> <requests> "<levels>"
BIN=$1; N=$2; LEVELS=$3
S="$(dirname "$0")"
# The server's own log goes to a temp dir, not beside the script: this lives in the repo.
LOG="$(mktemp -d)/server.log"
"$BIN" "$S/portable_server.zm" > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null' EXIT
for _ in $(seq 1 60); do curl -s -o /dev/null -m 1 http://127.0.0.1:18600/ && break; sleep 0.2; done
curl -s -o /dev/null -m 2 http://127.0.0.1:18600/ || { echo "did not start"; cat "$LOG"; exit 1; }
OUT=""
for C in $LEVELS; do
  R="$(ab -n "$N" -c "$C" http://127.0.0.1:18600/ 2>&1 | awk '/Requests per second/{print $4}')"
  OUT="$OUT c=$C:$R"
done
echo "$OUT"
