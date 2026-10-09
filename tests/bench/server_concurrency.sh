#!/usr/bin/env bash
# Throughput vs. concurrency for the sample backend -- isolates whether a bottleneck scales with
# client threads or is flat regardless of them (F2/P2-1: "500-650 req/s ... does not scale with
# clients"). Starts the real backend (serve.zm) on in-memory SQLite, registers one account, then
# hits an authenticated GET at each concurrency level with `ab`.
#
# usage: tests/bench/server_concurrency.sh <ziman-binary> [port] [requests-per-run] [concurrency-levels]
#   ziman-binary          e.g. ./ziman or ./ziman-nogc (a bare name is resolved under this repo's root)
#   port                  default 18391
#   requests-per-run      default 3000
#   concurrency-levels    space-separated, quoted, default "1 4 8 16"
#
# Needs `ab` (apache2-utils / httpd-tools) on PATH.
set -uo pipefail
BIN=${1:?usage: server_concurrency.sh <ziman-binary> [port] [N] ["c1 c2 ..."]}
PORT=${2:-18391}
N=${3:-3000}
LEVELS=${4:-"1 4 8 16"}

command -v ab >/dev/null 2>&1 || { echo "Error: 'ab' not found (apache2-utils / httpd-tools)." >&2; exit 1; }

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BACKEND_DIR="$ROOT/health-record/health-record-backend-ziman"
case "$BIN" in /*) ;; *) BIN="$ROOT/ziman/$BIN" ;; esac
[ -x "$BIN" ] || { echo "Error: ziman binary not found or not executable: $BIN" >&2; exit 1; }

WORK="$(mktemp -d)"
( exec env ZIMAN_RESOURCE_PATH="$BACKEND_DIR/src/test/resources:$BACKEND_DIR/src/main/resources" \
    JWT_SECRET="$(head -c 32 /dev/urandom | base64)" MFA_SECRET_KEY="$(head -c 32 /dev/urandom | base64)" \
    SERVER_PORT="$PORT" PROFILE_ACTIVE= \
    "$BIN" "$BACKEND_DIR/src/main/ziman/serve.zm" > "$WORK/server.log" 2>&1 ) &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; rm -rf "$WORK"' EXIT

BASE_URL="http://127.0.0.1:$PORT"
for _ in $(seq 1 100); do curl -s -o /dev/null "$BASE_URL/health" && break; sleep 0.1; done
if ! kill -0 "$PID" 2>/dev/null || ! curl -s -o /dev/null "$BASE_URL/health"; then
  echo "Error: the server did not start:" >&2; cat "$WORK/server.log" >&2; exit 1
fi

# Register, then log in for the token. Registration deliberately returns no token (health-record
# 36aa350, task B11: it answers identically whether or not the email is taken, so it cannot be used
# to find out which addresses exist), which broke this script silently -- it extracted an empty
# token and exited before running a single benchmark. Found on 2026-10-08 by a review that was
# asked to measure with it; see docs/reviews/tasks.md F20.
EMAIL="bench-$$@example.com"
PASSWORD="bench runner password"
curl -s -o /dev/null -X POST "$BASE_URL/api/auth/register" -H 'Content-Type: application/json' \
    -d "{\"name\":\"Bench Runner\",\"email\":\"$EMAIL\",\"password\":\"$PASSWORD\"}"
TOKEN="$(curl -s -X POST "$BASE_URL/api/auth/login" -H 'Content-Type: application/json' \
    -d "{\"email\":\"$EMAIL\",\"password\":\"$PASSWORD\"}" \
    | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
[ -n "$TOKEN" ] || { echo "Error: could not register and log in the runner's account:" >&2; cat "$WORK/server.log" >&2; exit 1; }

echo "binary=$(basename "$BIN") port=$PORT requests=$N"
for C in $LEVELS; do
  OUT="$(ab -n "$N" -c "$C" -H "Authorization: Bearer $TOKEN" "$BASE_URL/api/me/allergies" 2>&1)"
  RPS="$(echo "$OUT" | awk '/Requests per second/{print $4}')"
  FAILED="$(echo "$OUT" | awk '/Failed requests/{print $3}')"
  printf 'c=%-3s rps=%-10s failed=%s\n' "$C" "$RPS" "$FAILED"
done
