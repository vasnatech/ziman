#!/usr/bin/env bash
# Runs every fixture in tests/fixtures/.
#
# For each name.zm:
#   - if name.out exists, the program must exit 0 and print exactly that on stdout.
#   - if name.err exists, the program must exit non-zero and that file's content
#     must appear as a substring somewhere in stderr.
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=./ziman
pass=0
fail=0

for zm in tests/fixtures/*.zm; do
    name=$(basename "$zm" .zm)
    out_file="tests/fixtures/$name.out"
    err_file="tests/fixtures/$name.err"

    stdout=$("$BIN" "$zm" 2>/tmp/ziman_test_stderr.$$)
    status=$?
    stderr=$(cat /tmp/ziman_test_stderr.$$)
    rm -f /tmp/ziman_test_stderr.$$

    if [ -f "$out_file" ]; then
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
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
