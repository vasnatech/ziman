#!/usr/bin/env bash
# Stage 1 of docs/design/full-overloading.md: run the shadow resolver over every Ziman file we
# have and report whether it ever disagrees with the hand-written dispatch arms.
#
# The invariant is positive, not an absence: a run that observes **zero** calls prints nothing and
# would otherwise pass. So this reads the observed count out of the summary line and fails when it
# is zero, which is what a mis-wired hook looks like.
#
# usage: tools/overload_shadow.sh [ziman-binary]
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${1:-./ziman}
NORTHSTAR=${ZIMAN_NORTHSTAR:-../health-record/health-record-backend-ziman}

files=$(ls tests/lang/*.zm docs/reviews/benchmark/reference/*.zm 2>/dev/null)
files="$files $(find lib examples -name '*.zm' 2>/dev/null)"
[ -d "$NORTHSTAR" ] && files="$files $(find "$NORTHSTAR" -name '*.zm' 2>/dev/null)"

observed=0 agreed=0 disagreed=0 scanned=0
report=$(mktemp); trap 'rm -f "$report"' EXIT

for f in $files; do
    out=$(ZIMAN_OVERLOAD_SHADOW=1 "$BIN" --check "$f" 2>&1 >/dev/null)
    scanned=$((scanned + 1))
    printf '%s\n' "$out" | grep '^overload-shadow: [0-9]*:' | sed "s|^|$f: |" >> "$report"
    line=$(printf '%s\n' "$out" | grep -o '[0-9]* call(s) observed, [0-9]* agreed, [0-9]* disagreed' | tail -1)
    [ -z "$line" ] && continue
    observed=$((observed + $(printf '%s' "$line" | awk '{print $1}')))
    agreed=$((agreed   + $(printf '%s' "$line" | awk '{print $4}')))
    disagreed=$((disagreed + $(printf '%s' "$line" | awk '{print $6}')))
done

echo "overload shadow: $scanned file(s), $observed dispatched call(s), $agreed agreed, $disagreed disagreed"

if [ "$observed" -eq 0 ]; then
    echo "FAIL the shadow resolver observed no calls at all -- the hook is not wired" >&2
    exit 1
fi
if [ "$disagreed" -ne 0 ]; then
    echo "--- disagreements (these are the specification, not necessarily bugs) ---" >&2
    cat "$report" >&2
    exit 1
fi
echo "ok   the resolver agrees with every hand-written dispatch arm it saw"
