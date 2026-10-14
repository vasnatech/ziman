#!/usr/bin/env bash
# Load test for the sample backend: memory and request latency per phase.
# usage: tests/bench/server_load.sh <ziman-binary> [requests-per-phase] [port]
# Phases: GET /health, POST /api/me/allergies, GET /api/me/allergies (a growing
# list). Prints the server's RSS after each phase and the p50 / p99 / max request
# time, so it shows both the leak (default build) and the collector's pauses
# (`make gc`).
#
# Every phase asserts its own HTTP status and the script exits non-zero on the
# first failure. It used to do neither: it launched `server.zm`, a path the
# backend has not had since the entry point became `src/main/ziman/serve.zm`, so
# the server died at once, `rss()` read a `/proc` entry that was already gone,
# and the latency columns reported `curl`'s round trips to a closed port as
# `p50=0.0002s`. Three phases of plausible sub-millisecond numbers and exit 0,
# measuring nothing -- `set -uo pipefail` has no `-e`, and awk over an empty file
# prints zeroes rather than complaining. A benchmark that cannot fail is worse
# than no benchmark, because DESIGN.md quoted it.
set -uo pipefail
BIN=${1:?usage: server_load.sh <ziman-binary> [N] [port]}; N=${2:-200}; PORT=${3:-18391}
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BACKEND="$ROOT/health-record/health-record-backend-ziman"
cd "$BACKEND"
case "$BIN" in /*) ;; *) BIN="$ROOT/ziman/$BIN" ;; esac
LOG=/tmp/server_load_$PORT.log
# ZIMAN_RESOURCE_PATH is how the app finds application.properties by bare name, and
# the two secrets are required at startup -- the same environment src/test/other/*_test.py
# sets, which is the arrangement that is actually known to boot.
( exec env ZIMAN_RESOURCE_PATH="$BACKEND/src/test/resources:$BACKEND/src/main/resources" \
    SERVER_PORT=$PORT PROFILE_ACTIVE= SERVER_LOG_REQUESTS=false DATASOURCE_URL=sqlite::memory: \
    JWT_SECRET=AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA= \
    MFA_SECRET_KEY=ISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+PwA= \
    "$BIN" src/main/ziman/serve.zm > "$LOG" 2>&1 ) &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null' EXIT
up=0
for _ in $(seq 1 100); do
  curl -s -o /dev/null http://127.0.0.1:$PORT/health && { up=1; break; }
  kill -0 $PID 2>/dev/null || break
  sleep 0.1
done
[ "$up" = 1 ] || { echo "server did not start; $LOG:" >&2; sed 's/^/  /' "$LOG" >&2; exit 1; }

# A phase needs a user: every measured endpoint but /health is authenticated, and the
# list it grows is that user's own.
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
json='Content-Type: application/json'
reg=$(code -X POST -H "$json" -d '{"name":"Load","email":"load@example.com","password":"correct horse"}' \
  http://127.0.0.1:$PORT/api/auth/register)
[ "$reg" = 200 ] || { echo "register failed: HTTP $reg" >&2; exit 1; }
TOKEN=$(curl -s -X POST -H "$json" -d '{"email":"load@example.com","password":"correct horse"}' \
  http://127.0.0.1:$PORT/api/auth/login | sed -n 's/.*"token"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
[ -n "$TOKEN" ] || { echo "login returned no token" >&2; exit 1; }
AUTH="Authorization: Bearer $TOKEN"

rss() { awk '/VmRSS/{print $2}' /proc/$PID/status 2>/dev/null; }
echo "start rss_kb=$(rss)"
BODY='{"substance":"Peanut","severity":"MODERATE","reaction":"hives"}'
LAT=$(mktemp)
trap 'rm -f "$LAT"; kill $PID 2>/dev/null; wait $PID 2>/dev/null' EXIT
for phase in health post list; do
  : > "$LAT"
  bad=0 badcode=
  for _ in $(seq 1 "$N"); do
    case $phase in
      health) out=$(curl -s -o /dev/null -w '%{time_total} %{http_code}\n' http://127.0.0.1:$PORT/health) ;;
      post)   out=$(curl -s -o /dev/null -w '%{time_total} %{http_code}\n' -X POST -H "$json" -H "$AUTH" -d "$BODY" http://127.0.0.1:$PORT/api/me/allergies) ;;
      list)   out=$(curl -s -o /dev/null -w '%{time_total} %{http_code}\n' -H "$AUTH" http://127.0.0.1:$PORT/api/me/allergies) ;;
    esac
    case "${out##* }" in 2??) ;; *) bad=$((bad + 1)); badcode=${out##* } ;; esac
    echo "${out%% *}" >> "$LAT"
  done
  [ "$bad" = 0 ] || { echo "$phase: $bad/$N requests failed (last HTTP $badcode)" >&2; exit 1; }
  sort -n "$LAT" | awk -v phase=$phase -v rss="$(rss)" '
    {a[NR]=$1}
    END{
      if (NR == 0) { printf "%s: no timings\n", phase > "/dev/stderr"; exit 1 }
      i99 = int(NR*0.99) + 1; if (i99 > NR) i99 = NR
      printf "%-7s rss_kb=%-9s p50=%.4fs p99=%.4fs max=%.4fs\n", phase, rss, a[int(NR*0.5)+1], a[i99], a[NR]
    }' || exit 1
done
