#!/usr/bin/env bash
# Type-checks and runs the North Star application against this build of the interpreter.
#
# docs/ROADMAP.md's first section names a slice of the health-record backend as the target this
# language exists to reach, and that application lives in a sibling checkout -- so nothing in this
# repository was watching it. It had been broken for some time when this script was written
# (docs/reviews/tasks.md F25): two type errors from a signature change, and, underneath them, a
# runtime failure from B11, which deliberately stopped registration from issuing a token and left
# the demo unwrapping a `None`.
#
# That second failure is why this script *runs* the application instead of only checking it. A type
# check would have reported the first pair and gone green on the second, which is the more
# interesting kind of breakage: the language accepted the program and the program could not work.
#
# Nothing is asserted about the output. Every line of it carries a fresh UUID and timestamp, so
# pinning the text would be a test of `date` -- what is checked is that the program type-checks,
# runs to completion and prints something. The secrets below are dummies that only have to satisfy
# the config validation (two distinct base64 keys, since it refuses a reused one).
#
# usage: tests/run_northstar.sh [ziman-binary]
#   ZIMAN_NORTHSTAR=path   where the backend checkout is (default ../health-record/health-record-backend-ziman)
#
# Checks both entry points and runs the one that terminates. See the loop below for why both.
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=$(cd "$(dirname "${1:-./ziman}")" && pwd)/$(basename "${1:-./ziman}")
APP=${ZIMAN_NORTHSTAR:-../health-record/health-record-backend-ziman}

if [ ! -f "$APP/src/main/ziman/main.zm" ]; then
    echo "run_northstar.sh: skipped -- no North Star checkout at $APP (set ZIMAN_NORTHSTAR to point at one)"
    exit 0
fi
APP=$(cd "$APP" && pwd)

echo "run_northstar.sh: $APP"
# Both entry points, because there are two and they share no code: main.zm walks the controllers the
# way the routes call them, and serve.zm is the actual HTTP server -- the thing that runs in
# production. Neither references the other (checked: zero references each way), so checking main.zm
# alone leaves the server unguarded, which is what the first version of this script did. That is the
# same mistake F25 was filed for, one level down: a guard that covers what its author happened to
# look at rather than what matters.
for entry in main.zm serve.zm; do
    if [ ! -f "$APP/src/main/ziman/$entry" ]; then
        echo "run_northstar.sh: FAIL -- $entry is missing from the North Star checkout" >&2
        exit 1
    fi
    if ! (cd "$APP/src/main/ziman" && "$BIN" --check "$entry"); then
        echo "run_northstar.sh: FAIL -- the North Star application's $entry does not type-check against this build" >&2
        exit 1
    fi
    echo "ok   --check $entry"
done

# Only main.zm is run. serve.zm binds a port and serves until it is told to stop, so running it
# here would hang the suite; type-checking it is the half that can be done unattended, and the TLS
# and bench suites are where the server is actually exercised.
# Run from the resources directory, which is where application.properties is read from.
out=$(cd "$APP/src/main/resources" && \
    JWT_SECRET=$(head -c 32 /dev/urandom | base64 | tr -d '\n') \
    MFA_SECRET_KEY=$(head -c 32 /dev/urandom | base64 | tr -d '\n') \
    DB_URL=unused DB_USER=unused DB_PASSWORD=unused SMTP_PASSWORD=unused \
    "$BIN" ../ziman/main.zm 2>/dev/null)
status=$?
if [ $status -ne 0 ]; then
    echo "run_northstar.sh: FAIL -- the North Star application type-checks but exited $status when run" >&2
    exit 1
fi
if [ -z "$out" ]; then
    echo "run_northstar.sh: FAIL -- the North Star application ran but printed nothing" >&2
    exit 1
fi
echo "ok   run ($(printf '%s\n' "$out" | wc -l) lines of output)"
echo "run_northstar.sh: the North Star application type-checks and runs"
