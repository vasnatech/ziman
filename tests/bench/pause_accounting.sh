#!/usr/bin/env bash
# Pause accounting for the collector build, reproducing the methodology behind docs/MEMORY.md's
# founding table (collections / avg pause / total pause / % of wall-clock paused, at c=1 vs c=16).
#
# One server per concurrency level, so pauses attribute to one level cleanly. GC_PRINT_STATS output
# from startup/register/login is excluded by taking a baseline line count after the token is in
# hand and only counting collections printed after it.
#
# usage: pause_accounting.sh <ziman-binary> <port> <requests> <level>
set -uo pipefail
BIN=$1; PORT=$2; N=$3; C=$4
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BACKEND_DIR="$ROOT/health-record/health-record-backend-ziman"
case "$BIN" in /*) ;; *) BIN="$ROOT/ziman/$BIN" ;; esac

WORK="$(mktemp -d)"
( exec env GC_PRINT_STATS=1 \
    ZIMAN_RESOURCE_PATH="$BACKEND_DIR/src/test/resources:$BACKEND_DIR/src/main/resources" \
    JWT_SECRET="$(head -c 32 /dev/urandom | base64)" MFA_SECRET_KEY="$(head -c 32 /dev/urandom | base64)" \
    SERVER_PORT="$PORT" PROFILE_ACTIVE= \
    "$BIN" "$BACKEND_DIR/src/main/ziman/serve.zm" > "$WORK/server.log" 2>&1 ) &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; rm -rf "$WORK"' EXIT

BASE_URL="http://127.0.0.1:$PORT"
for _ in $(seq 1 100); do curl -s -o /dev/null "$BASE_URL/health" && break; sleep 0.1; done
curl -s -o /dev/null "$BASE_URL/health" || { echo "server did not start" >&2; cat "$WORK/server.log" >&2; exit 1; }

EMAIL="bench-$$@example.com"; PASSWORD="bench runner password"
curl -s -o /dev/null -X POST "$BASE_URL/api/auth/register" -H 'Content-Type: application/json' \
    -d "{\"name\":\"Bench Runner\",\"email\":\"$EMAIL\",\"password\":\"$PASSWORD\"}"
TOKEN="$(curl -s -X POST "$BASE_URL/api/auth/login" -H 'Content-Type: application/json' \
    -d "{\"email\":\"$EMAIL\",\"password\":\"$PASSWORD\"}" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
[ -n "$TOKEN" ] || { echo "no token" >&2; exit 1; }

BASELINE=$(wc -l < "$WORK/server.log")
AB="$(ab -n "$N" -c "$C" -H "Authorization: Bearer $TOKEN" "$BASE_URL/api/me/allergies" 2>&1)"
sleep 0.3
tail -n +$((BASELINE + 1)) "$WORK/server.log" > "$WORK/bench.log"

RPS="$(echo "$AB" | awk '/Requests per second/{print $4}')"
SECS="$(echo "$AB" | awk '/Time taken for tests/{print $5}')"
python3 - "$WORK/bench.log" "$C" "$RPS" "$SECS" "$(basename "$BIN")" <<'PY'
import re, sys
log, c, rps, secs, binname = sys.argv[1:6]
text = open(log, errors="replace").read()
# "World-stopped marking took 12 ms 345678 ns" -- the stop-the-world pause itself.
stw = [int(m[0]) * 1e6 + int(m[1]) for m in re.findall(r'World-stopped marking took (\d+) ms (\d+) ns', text)]
full = [int(m[0]) * 1e6 + int(m[1]) for m in re.findall(r'Complete collection took (\d+) ms (\d+) ns', text)]
secs = float(secs)
tot_stw = sum(stw) / 1e6
tot_full = sum(full) / 1e6
print(f"  c={c:<3} rps={rps:<9} wall={secs:.2f}s  collections={len(full):<4}"
      f" stw_total={tot_stw:.0f}ms avg={tot_stw/len(stw) if stw else 0:.1f}ms"
      f" paused={100*tot_stw/(secs*1000) if secs else 0:.1f}%"
      f"  (full_total={tot_full:.0f}ms)")
PY
