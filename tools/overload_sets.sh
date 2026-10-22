#!/usr/bin/env bash
# The declaration-site check from docs/design/full-overloading.md §4.
#
# Two populations share the shape "one name, several declarations", and only one is an overload
# set. A set of ordinary declarations is the resolver's -- `sort` is two real functions and a call
# picks one. A set of `builtin fn` declarations is a single polymorphic implementation with its
# type rule written out: `len` has nine signatures over one C function, and there is no `array_len`
# for a call to resolve to. This gate governs the first population only.
#
# Until F181 it asserted "exactly `concat` and `mutable` are unresolvable", which was true of the
# two populations mixed together -- and both offenders turned out to be in the second, where the
# resolver never looks. Against the population that matters the bar is simply **zero**.
#
# A third population appeared when `concat` stopped being a builtin: `#[compiler_resolved]`, four
# real declarations that a rule in check_call chooses between because no argument list can. Those
# are counted apart and checked **inversely** -- the attribute is a defect on a set that does not
# need it -- so the "unresolvable" count still has to be zero and the exemption cannot spread.
#
# usage: tools/overload_sets.sh [ziman-binary]
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${1:-./ziman}

out=$(ZIMAN_OVERLOAD_SETS=1 "$BIN" --check tests/lang/array_extras.zm 2>&1 >/dev/null)
summary=$(printf '%s\n' "$out" | grep -o '[0-9]* resolver-owned set(s), [0-9]* unresolvable; [0-9]* polymorphic builtin(s) not checked; [0-9]* compiler-resolved')

if [ -z "$summary" ]; then
    echo "FAIL the declaration-site check produced no summary -- it did not run" >&2
    exit 1
fi
sets=$(printf '%s' "$summary" | awk '{print $1}')
bad=$(printf '%s' "$summary" | awk '{print $4}')

echo "overload sets: $summary"

# A positive invariant: zero sets found means the check is looking at the wrong module, which
# would otherwise pass silently for the same reason an empty shadow run did (F176).
if [ "$sets" -eq 0 ]; then
    echo "FAIL no resolver-owned overload sets were found at all -- the check is misdirected" >&2
    exit 1
fi
if [ "$bad" -ne 0 ]; then
    echo "FAIL $bad resolver-owned set(s) cannot be resolved by any call:" >&2
    printf '%s\n' "$out" | grep '^overload-set:' >&2
    echo "    a set whose members no argument list can tell apart is a defect in the" >&2
    echo "    declarations -- give one its own name, or narrow a parameter" >&2
    echo "    (a set the compiler resolves by a rule says so with #[compiler_resolved], and" >&2
    echo "     carrying it without needing it is reported here too)" >&2
    exit 1
fi
echo "ok   every resolver-owned overload set can be told apart by some call"
