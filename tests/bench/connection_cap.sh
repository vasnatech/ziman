#!/usr/bin/env bash
# Throughput at one concurrency level with server.max-connections overridden, optional CPU pinning
# for server and client separately, and optional `ab` flags (AB_FLAGS=-k for keep-alive). Written
# for F139 and for MEMORY.md item 3, where three things turned out to matter more than the effect
# being chased: co-locating `ab` with the server costs 16-19%, keep-alive is worth 1.8-2.3x, and
# crossing max_connections with keep-alive clients stalls the server rather than throttling it.
#
# Peak thread count is sampled DURING the run: without keep-alive `ab` opens a connection per
# request, so the per-connection threads do not survive until a before/after sample.
#
# usage: connection_cap.sh <binary> <port> <requests> <level> <cap> [srv_cpus] [ab_cpus]
#        AB_FLAGS=-k connection_cap.sh ./ziman 18600 2000 256 256 0-11 12-15
set -uo pipefail
BIN=$1; PORT=$2; N=$3; C=$4; CAP=$5; SRVSET=${6:-}; ABSET=${7:-}
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BACKEND_DIR="$ROOT/health-record/health-record-backend-ziman"
case "$BIN" in /*) ;; *) BIN="$ROOT/ziman/$BIN" ;; esac
WORK="$(mktemp -d)"
( exec env ZIMAN_RESOURCE_PATH="$BACKEND_DIR/src/test/resources:$BACKEND_DIR/src/main/resources" \
    JWT_SECRET="$(head -c 32 /dev/urandom | base64)" MFA_SECRET_KEY="$(head -c 32 /dev/urandom | base64)" \
    SERVER_PORT="$PORT" PROFILE_ACTIVE= SERVER_MAX_CONNECTIONS="$CAP" \
    ${SRVSET:+taskset -c $SRVSET} "$BIN" "$BACKEND_DIR/src/main/ziman/serve.zm" > "$WORK/server.log" 2>&1 ) &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; rm -rf "$WORK"' EXIT
BASE_URL="http://127.0.0.1:$PORT"
for _ in $(seq 1 150); do curl -s -o /dev/null "$BASE_URL/health" && break; sleep 0.1; done
curl -s -o /dev/null "$BASE_URL/health" || { echo "  cap=$CAP c=$C NO START: $(tail -2 "$WORK/server.log")"; exit 1; }
EMAIL="bench-$$@example.com"; PASSWORD="bench runner password"
curl -s -o /dev/null -X POST "$BASE_URL/api/auth/register" -H 'Content-Type: application/json' \
    -d "{\"name\":\"B\",\"email\":\"$EMAIL\",\"password\":\"$PASSWORD\"}"
TOKEN="$(curl -s -X POST "$BASE_URL/api/auth/login" -H 'Content-Type: application/json' \
    -d "{\"email\":\"$EMAIL\",\"password\":\"$PASSWORD\"}" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
[ -n "$TOKEN" ] || { echo "  cap=$CAP c=$C NO TOKEN"; exit 1; }
PEAK=0
( while :; do t=$(awk '/^Threads:/{print $2}' "/proc/$PID/status" 2>/dev/null); [ -n "$t" ] && echo "$t"; sleep 0.02; done > "$WORK/threads" ) &
SAMP=$!
AB="$(${ABSET:+taskset -c $ABSET} ab ${AB_FLAGS:-} -n "$N" -c "$C" -H "Authorization: Bearer $TOKEN" "$BASE_URL/api/me/allergies" 2>&1)"
kill $SAMP 2>/dev/null
PEAK=$(sort -n "$WORK/threads" 2>/dev/null | tail -1)
RPS="$(echo "$AB" | awk '/Requests per second/{print $4}')"
FAIL="$(echo "$AB" | awk '/Failed requests/{print $3}')"
printf '  cap=%-4s c=%-4s rps=%-9s failed=%-3s peak_threads=%-4s srv_cpus=%s ab_cpus=%-5s flags=%s\n' "$CAP" "$C" "$RPS" "$FAIL" "${PEAK:-?}" "${SRVSET:-all}" "${ABSET:-all}" "${AB_FLAGS:-none}"
