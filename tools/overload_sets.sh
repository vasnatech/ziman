#!/usr/bin/env bash
# Stage 2 of docs/design/full-overloading.md: the declaration-site check.
#
# Two members of an overload set are indistinguishable when their arities overlap and, at every
# position within that overlap, their parameter types accept some common type. Such a set cannot
# be resolved by any call and is a defect in the declarations, not in the call site.
#
# The design predicts exactly two unresolvable sets in lib/prelude.zm -- `concat` and `mutable` --
# and this gate holds that prediction. A *third* name appearing here means the prelude has a shape
# the design did not anticipate, and docs/design/full-overloading.md is what needs fixing, before
# any later stage leans on it. A name *disappearing* means one of the two got fixed and the
# document should say so.
#
# usage: tools/overload_sets.sh [ziman-binary]
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${1:-./ziman}
EXPECTED="concat mutable"

out=$(ZIMAN_OVERLOAD_SETS=1 "$BIN" --check tests/lang/array_extras.zm 2>&1 >/dev/null)
summary=$(printf '%s\n' "$out" | grep -o '[0-9]* set(s) with more than one declaration, [0-9]* unresolvable')

if [ -z "$summary" ]; then
    echo "FAIL the declaration-site check produced no summary -- it did not run" >&2
    exit 1
fi
sets=$(printf '%s' "$summary" | awk '{print $1}')
if [ "$sets" -eq 0 ]; then
    echo "FAIL no overload sets were found at all -- the check is looking at the wrong module" >&2
    exit 1
fi

found=$(printf '%s\n' "$out" | sed -n "s/^overload-set: '\([a-z_]*\)' is ambiguous.*/\1/p" | sort -u | tr '\n' ' ')
found=${found% }

echo "overload sets: $summary"
if [ "$found" != "$EXPECTED" ]; then
    echo "FAIL unresolvable sets are '$found', the design predicts '$EXPECTED'" >&2
    printf '%s\n' "$out" | grep '^overload-set' >&2
    echo "    a third name means docs/design/full-overloading.md is wrong, not the declaration" >&2
    exit 1
fi
echo "ok   exactly the two sets the design predicts are unresolvable: $found"
