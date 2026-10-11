#!/usr/bin/env bash
# How `use` finds a module that is not beside the importing file: ZIMAN_MODULE_PATH, and a
# `ziman.toml` marking the project root (ROADMAP.md's `ziman test` entry).
#
# Why this is a script and not a fixture: tests/run_tests.sh runs a directory fixture's entry at
# `name/main.zm`, which is the top of that directory -- so the marker's directory and the entry's
# directory are the same one, and the marker can never change an answer. The whole point here is an
# entry *below* the root, which only a script can set up.
#
# usage: tests/run_module_path.sh [ziman-binary]   (default ./ziman)
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=$(cd "$(dirname "${1:-./ziman}")" && pwd)/$(basename "${1:-./ziman}")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src/util" "$work/test"
cat > "$work/src/util/math.zm" <<'ZM'
pub fn double(n: int) -> int {
    return n * 2
}
ZM
cat > "$work/test/double_test.zm" <<'ZM'
use src/util/math

fn main() {
    print(math.double(21))
}
ZM
cat > "$work/test/beside_test.zm" <<'ZM'
use math

fn main() {
    print(math.double(4))
}
ZM

pass=0 fail=0
check() { # check <name> <expected> <actual>
    if [ "$2" = "$3" ]; then pass=$((pass + 1)); echo "ok   $1"
    else fail=$((fail + 1)); echo "FAIL $1: expected '$2', got '$3'" >&2; fi
}

# Without a marker the project-relative path cannot resolve, and the error says where it looked.
out=$("$BIN" "$work/test/double_test.zm" 2>&1 | head -1)
case "$out" in
    *"cannot find module 'src/util/math'"*) check "no marker: refused, and names the path" yes yes ;;
    *) check "no marker: refused, and names the path" yes "no ($out)" ;;
esac

# With one, the same `use` resolves from the marker's directory.
touch "$work/ziman.toml"
check "marker: a test under test/ uses src/util/math" "42" "$("$BIN" "$work/test/double_test.zm" 2>&1 | head -1)"

# The marker is a search path, not a new root: a module beside the importing file still wins, which
# is what keeps every program that resolved before the marker existed resolving the same way.
cp "$work/src/util/math.zm" "$work/test/math.zm"
check "beside the importing file still resolves" "8" "$("$BIN" "$work/test/beside_test.zm" 2>&1 | head -1)"
rm "$work/test/math.zm"

# ZIMAN_MODULE_PATH keeps working, with and without a marker present.
check "ZIMAN_MODULE_PATH, marker present" "8" \
    "$(ZIMAN_MODULE_PATH=$work/src/util "$BIN" "$work/test/beside_test.zm" 2>&1 | head -1)"
rm "$work/ziman.toml"
check "ZIMAN_MODULE_PATH, no marker" "8" \
    "$(ZIMAN_MODULE_PATH=$work/src/util "$BIN" "$work/test/beside_test.zm" 2>&1 | head -1)"

# A *relative* entry path, which is how the README and every handwritten invocation spell it. Every
# case above names the entry file absolutely, which is the only reason F33 could ship with the
# marker doing nothing in the common invocation: the upward walk ran out of slashes at the first
# path component and never probed the current directory, so `ziman src/test/x_test.zm` from a
# project root found no marker while `ziman /abs/src/test/x_test.zm` found one (tasks.md F52).
touch "$work/ziman.toml"
check "marker: relative entry path from the project root" "42" \
    "$(cd "$work" && "$BIN" test/double_test.zm 2>&1 | head -1)"
check "marker: relative entry path spelled './...'" "42" \
    "$(cd "$work" && "$BIN" ./test/double_test.zm 2>&1 | head -1)"

# The limit of a relative walk, stated so it is a decision and not a surprise: it cannot rise above
# the directory it starts in, because a relative path has no way to spell the parent. Run from the
# project root, or name the entry file absolutely.
out="$(cd "$work/test" && "$BIN" double_test.zm 2>&1 | head -1)"
case "$out" in
    *"cannot find module 'src/util/math'"*) check "relative walk does not rise above the cwd" yes yes ;;
    *) check "relative walk does not rise above the cwd" yes "no ($out)" ;;
esac
rm "$work/ziman.toml"

echo "--- $pass passed, $fail failed"
[ "$fail" -eq 0 ]
