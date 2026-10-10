#!/usr/bin/env bash
# `ziman --api`, which answers "what does this module offer" from the parsed declarations.
#
# A script rather than a fixture because tests/run_tests.sh runs a .zm file, and this is a command
# that takes no program at all. The cases are the ones the 2026-10-09 generation benchmark showed
# cost real time: the names under `fs` (eight candidates brute-forced before `write_text`), the
# accessor under a map, and whether `collections.sort` needs its type argument spelled out.
#
# usage: tests/run_api.sh [ziman-binary]   (default ./ziman)
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${1:-./ziman}

pass=0 fail=0
has() { # has <name> <pattern> <text>
    if printf '%s' "$3" | grep -qF -- "$2"; then pass=$((pass + 1)); echo "ok   $1"
    else fail=$((fail + 1)); echo "FAIL $1: no '$2' in the output" >&2; fi
}

listing=$("$BIN" --api 2>&1)
has "a bare --api lists bundled modules" "fs" "$listing"
has "and says builtins need no use"      "need no 'use' at all" "$listing"

fs=$("$BIN" --api fs 2>&1)
has "fs names write_text"                "fn write_text(" "$fs"
has "fs names read_text with its Result" "-> Result<string, FsError>" "$fs"
has "fs declares its error enum"         "enum FsError {" "$fs"

coll=$("$BIN" --api collections 2>&1)
has "a generic signature carries its type parameter and bound" "fn sort<T: Ord>(items: [T]) -> [T]" "$coll"

net=$("$BIN" --api net 2>&1)
has "an enum variant carries its payload type" "Io(string)" "$net"
has "the unix-socket entry point is listed"    "fn connect_unix(path: string)" "$net"

# A private declaration is not API, so it must not appear. `net` has raw natives behind its pub
# wrappers, which is exactly that case.
if printf '%s' "$net" | grep -qF "connect_unix_raw"; then
    fail=$((fail + 1)); echo "FAIL a non-pub declaration leaked into --api" >&2
else
    pass=$((pass + 1)); echo "ok   a non-pub declaration stays out"
fi

# Works on a file, not only a bundled name.
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
cat > "$work/mine.zm" <<'ZM'
struct Point {
    x: int
    y: int
}

pub fn shift(p: Point, by: int) -> Point {
    return Point { x: p.x + by, y: p.y }
}

fn hidden() -> int {
    return 1
}
ZM
mine=$("$BIN" --api "$work/mine.zm" 2>&1)
has "a project file's own pub function"  "fn shift(p: Point, by: int) -> Point" "$mine"
if printf '%s' "$mine" | grep -qF "hidden"; then
    fail=$((fail + 1)); echo "FAIL a private function leaked from a file" >&2
else
    pass=$((pass + 1)); echo "ok   a private function stays out of a file's api"
fi

"$BIN" --api nosuchthing >/dev/null 2>&1
if [ $? -eq 1 ]; then pass=$((pass + 1)); echo "ok   an unknown name exits 1"
else fail=$((fail + 1)); echo "FAIL an unknown name should exit 1" >&2; fi

echo "--- $pass passed, $fail failed"
[ "$fail" -eq 0 ]
