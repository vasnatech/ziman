#!/usr/bin/env bash
# Runs every fixture in tests/fixtures/.
#
# Single-file fixtures: for each name.zm:
#   - if name.out exists, the program must exit 0 and print exactly that on stdout.
#   - if name.err exists, the program must exit non-zero and that file's content
#     must appear as a substring somewhere in stderr.
#
# Multi-file fixtures: a directory tests/fixtures/name/ with an entry point at
# name/main.zm (and whatever other .zm files it `use`s underneath it) is run
# the same way, checked against sibling name.out / name.err files (outside
# the directory, so the naming convention matches single-file fixtures).
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=./ziman
pass=0
fail=0

check_fixture() {
    local name="$1" entry="$2"
    local out_file="tests/fixtures/$name.out"
    local err_file="tests/fixtures/$name.err"

    local stdout status stderr
    stdout=$("$BIN" "$entry" 2>/tmp/ziman_test_stderr.$$)
    status=$?
    stderr=$(cat /tmp/ziman_test_stderr.$$)
    rm -f /tmp/ziman_test_stderr.$$

    if [ -f "$out_file" ]; then
        local expected
        expected=$(cat "$out_file")
        if [ "$status" -eq 0 ] && [ "$stdout" = "$expected" ]; then
            echo "PASS $name"
            pass=$((pass + 1))
        else
            echo "FAIL $name (expected success with matching stdout)"
            echo "  exit=$status"
            echo "  --- expected stdout ---"; echo "$expected" | sed 's/^/  /'
            echo "  --- actual stdout ---"; echo "$stdout" | sed 's/^/  /'
            echo "  --- actual stderr ---"; echo "$stderr" | sed 's/^/  /'
            fail=$((fail + 1))
        fi
    elif [ -f "$err_file" ]; then
        local needle
        needle=$(cat "$err_file")
        if [ "$status" -ne 0 ] && [[ "$stderr" == *"$needle"* ]]; then
            echo "PASS $name"
            pass=$((pass + 1))
        else
            echo "FAIL $name (expected failure containing: $needle)"
            echo "  exit=$status"
            echo "  --- actual stderr ---"; echo "$stderr" | sed 's/^/  /'
            fail=$((fail + 1))
        fi
    else
        echo "SKIP $name (no .out or .err fixture)"
    fi
}

for zm in tests/fixtures/*.zm; do
    name=$(basename "$zm" .zm)
    check_fixture "$name" "$zm"
done

for dir in tests/fixtures/*/; do
    name=$(basename "$dir")
    entry="tests/fixtures/$name/main.zm"
    if [ -f "$entry" ]; then
        check_fixture "$name" "$entry"
    fi
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
