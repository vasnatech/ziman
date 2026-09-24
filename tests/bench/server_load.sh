#!/usr/bin/env bash
# Load test for the sample backend: memory and request latency per phase.
# usage: tests/bench/server_load.sh <ziman-binary> [requests-per-phase] [port]
# Phases: GET /health, POST /patients, GET /patients (a growing list). Prints the
# server's RSS after each phase and the p50 / p99 / max request time, so it shows
# both the leak (default build) and the collector's pauses (`make gc`).
set -uo pipefail
BIN=${1:?usage: server_load.sh <ziman-binary> [N] [port]}; N=${2:-200}; PORT=${3:-18391}
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT/health-record/health-record-backend-ziman"
case "$BIN" in /*) ;; *) BIN="$ROOT/ziman/$BIN" ;; esac
( exec env SERVER_PORT=$PORT PROFILE_ACTIVE= SERVER_LOG_REQUESTS=false DATASOURCE_URL=sqlite::memory: "$BIN" server.zm > /tmp/server_load_$PORT.log 2>&1 ) &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null' EXIT
for _ in $(seq 1 100); do curl -s -o /dev/null http://127.0.0.1:$PORT/health && break; sleep 0.1; done
rss() { awk '/VmRSS/{print $2}' /proc/$PID/status; }
echo "start rss_kb=$(rss)"
BODY='{"firstName":"Ada","lastName":"Lovelace","email":"ada@example.com","dateOfBirth":"1815-12-10"}'
for phase in health post list; do
  : > /tmp/lat_$$.txt
  for i in $(seq 1 "$N"); do
    case $phase in
      health) curl -s -o /dev/null -w '%{time_total}\n' http://127.0.0.1:$PORT/health ;;
      post) curl -s -o /dev/null -w '%{time_total}\n' -X POST -H 'Content-Type: application/json' -d "$BODY" http://127.0.0.1:$PORT/patients ;;
      list) curl -s -o /dev/null -w '%{time_total}\n' http://127.0.0.1:$PORT/patients ;;
    esac >> /tmp/lat_$$.txt
  done
  sort -n /tmp/lat_$$.txt | awk -v phase=$phase -v rss=$(rss) '
    {a[NR]=$1}
    END{
      i99 = int(NR*0.99) + 1; if (i99 > NR) i99 = NR
      printf "%-7s rss_kb=%-9s p50=%.4fs p99=%.4fs max=%.4fs\n", phase, rss, a[int(NR*0.5)+1], a[i99], a[NR]
    }'
done
rm -f /tmp/lat_$$.txt
