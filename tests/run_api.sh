#!/usr/bin/env bash
# `ziman --api`, which answers "what does this module offer" from the parsed declarations.
#
# A script rather than a fixture because tests/run_tests.sh runs a .zm file, and this is a command
# that takes no program at all. The cases are the ones the 2026-10-09 generation benchmark showed
# cost real time: the names under `fs` (eight candidates brute-forced before `write_text`), the
# accessor under a map, and whether `array_sort` needs its type argument spelled out.
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
has "and points at the builtins"         "ziman --api builtins" "$listing"

# The gap wave three of the benchmark found: --api listed modules, so a run hunting a string's
# characters never saw `chars` (a builtin) and used split(word, "") instead, which type-checks and
# fails at runtime. Another concluded "there is no keys function" when map_keys is a builtin.
builtins=$("$BIN" --api builtins 2>&1)
has "builtins lists chars"               "chars" "$builtins"
has "builtins lists map_keys"            "map_keys" "$builtins"
has "builtins names the lowercase types" "int  float  bool  string" "$builtins"

fs=$("$BIN" --api fs 2>&1)
has "fs names write_text"                "fn write_text(" "$fs"
has "fs names read_text with its Result" "-> Result<string, FsError>" "$fs"
has "fs declares its error enum"         "enum FsError {" "$fs"

tst=$("$BIN" --api test 2>&1)
has "a generic signature carries its type parameter and bound" "fn assert_eq<T: Eq>(expected: T, actual: T)" "$tst"

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

# F172: `internal` in the prelude means reachable but not advertised. The five natives behind the
# sorts are the case it was added for -- callable (tests/lang/prelude_internal_still_callable.zm
# pins that) but not vocabulary, because their only callers are the six sort functions above them.
# Asserted by name rather than by count so that adding a builtin does not look like a regression.
for internal_name in is_sortable_scalars sort_scalar_order sort_scalars permute sort_positions; do
    if printf '%s' "$builtins" | grep -qE "^(builtin )?fn $internal_name\b"; then
        fail=$((fail + 1)); echo "FAIL an internal prelude native is advertised: $internal_name" >&2
    else
        pass=$((pass + 1)); echo "ok   an internal prelude native stays out: $internal_name"
    fi
done
has "and the sort it is the inside of is still listed" "fn sort<T: Ord>(a: [T]) -> [T]" "$builtins"

# F173: the same rule over the dispatcher's prefix layer. `map(b, f)` resolves to the literal name
# "mutable_array_" + "map", so these have to exist -- but 51 of the 64 `array_`/`mutable_array_`
# declarations were already invisible for being plain `fn`s, and only these stood out for being
# native. Hidden, not deleted: tests/lang/prelude_internal_still_callable.zm calls them.
for internal_name in array_map array_filter array_flat_map array_reverse mutable_array_push \
                     mutable_array_insert mutable_array_remove mutable_array_filter \
                     mutable_array_sort_with; do
    if printf '%s' "$builtins" | grep -qE "^(builtin )?fn $internal_name\b"; then
        fail=$((fail + 1)); echo "FAIL a prefix-layer native is advertised: $internal_name" >&2
    else
        pass=$((pass + 1)); echo "ok   a prefix-layer native stays out: $internal_name"
    fi
done
# The four that stay: two real constructors with no bare form, and the two `b[i]`/`b[i] = v`
# rewrite to -- those are named in diagnostics, so hiding them would point an error message at a
# name this listing denies exists.
for kept in mutable_array_new mutable_array_filled mutable_array_get mutable_array_set; do
    has "the prefixed name with no bare form is still listed: $kept" "fn $kept" "$builtins"
done

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

# F89: a declaration's doc comment travels with its signature. Round 3 of the 2026-10-12 benchmark
# measured what leaving it out costs: 46-fold-average answered wrongly after picking
# `number.float_to_text` off its signature, not knowing a whole-number float prints as `18` -- which
# that function's own comment says, giving `150` as an example -- and 15-sha256-hex spent 14 extra
# compiles hand-rolling hex rather than trust `bytes_to_hex`'s unstated output format, which its
# comment states exactly. Both are asserted here because both are the reason this exists.
number=$("$BIN" --api number 2>&1)
has "a doc comment travels with its signature"  "The shortest text that reads back as exactly" "$number"
has "and the one that decided 15-sha256-hex"    "Lowercase hex text of a byte array" "$number"

# The contiguity rule: a `// ---- section` banner is separated from the next declaration by a blank
# line, so it must not be read as that declaration's documentation.
cat > "$work/doc.zm" <<'ZM'
// ---------------- a banner

// The real doc for one.
pub fn one() -> int {
    return 1
}
ZM
docs=$("$BIN" --api "$work/doc.zm" 2>&1)
has "a doc comment above a declaration"  "The real doc for one." "$docs"
if printf '%s' "$docs" | grep -qF "a banner"; then
    fail=$((fail + 1)); echo "FAIL a section banner leaked into a declaration's docs" >&2
else
    pass=$((pass + 1)); echo "ok   a blank line keeps a banner out of the docs"
fi

"$BIN" --api nosuchthing >/dev/null 2>&1
if [ $? -eq 1 ]; then pass=$((pass + 1)); echo "ok   an unknown name exits 1"
else fail=$((fail + 1)); echo "FAIL an unknown name should exit 1" >&2; fi

echo "--- $pass passed, $fail failed"
[ "$fail" -eq 0 ]
