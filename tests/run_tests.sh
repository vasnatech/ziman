#!/usr/bin/env bash
# Runs every fixture in tests/fixtures/.
#
# Single-file fixtures: for each name.zm:
#   - if name.out exists, the program must exit 0 (or the code in a sibling name.exit,
#     for a fixture whose successful, exact-stdout-checked output -- `ziman test`'s
#     PASS/FAIL report, say -- comes with a non-zero exit, such as one or more failed
#     tests) and print exactly that on stdout.
#   - if name.err exists, the program must exit non-zero and that file's content
#     must appear as a substring somewhere in stderr.
#
# Either kind may have a sibling name.env (KEY=VALUE lines) exported for its run.
#
# Multi-file fixtures: a directory tests/fixtures/name/ with an entry point at
# name/main.zm (and whatever other .zm files it `use`s underneath it) is run
# the same way, checked against sibling name.out / name.err files (outside
# the directory, so the naming convention matches single-file fixtures).
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./ziman}
pass=0
fail=0

check_fixture() {
    local name="$1" entry="$2"
    local out_file="tests/fixtures/$name.out"
    local err_file="tests/fixtures/$name.err"

    # An optional tests/fixtures/<name>.env holds KEY=VALUE lines (blank lines and
    # #-comments ignored) exported for just this fixture's run -- how a fixture
    # exercises os.getenv / environment-variable overrides, since a Ziman program
    # can't set its own environment.
    local env_args=()
    local env_file="tests/fixtures/$name.env"
    if [ -f "$env_file" ]; then
        while IFS= read -r line; do
            case "$line" in ""|\#*) continue ;; esac
            env_args+=("$line")
        done < "$env_file"
    fi

    # An optional tests/fixtures/<name>.args holds command-line options (space
    # separated) passed to ziman before the file -- how a fixture exercises
    # --check, --json and --emit-derived.
    local cli_args=()
    local args_file="tests/fixtures/$name.args"
    if [ -f "$args_file" ]; then
        read -r -a cli_args < "$args_file"
    fi

    local stdout status stderr
    # `timeout` guards against a genuine deadlock in a spawn/Channel fixture
    # (a mistake in the interpreter's channel/thread handling) hanging the
    # whole suite forever instead of just failing that one fixture.
    # An optional <name>.progargs holds the program's own arguments (after `--`).
    local prog_args=()
    if [ -f "tests/fixtures/$name.progargs" ]; then
        read -r -a prog_args < "tests/fixtures/$name.progargs"
        prog_args=(-- "${prog_args[@]}")
    fi
    stdout=$(env "${env_args[@]}" timeout 5 "$BIN" "${cli_args[@]+"${cli_args[@]}"}" "$entry" "${prog_args[@]+"${prog_args[@]}"}" 2>/tmp/ziman_test_stderr.$$)
    status=$?
    stderr=$(cat /tmp/ziman_test_stderr.$$)
    rm -f /tmp/ziman_test_stderr.$$
    if [ "$status" -eq 124 ]; then
        stderr="$stderr"$'\n'"(timed out after 5s -- possible deadlock)"
    fi

    if [ -f "$out_file" ]; then
        local expected expected_status
        expected=$(cat "$out_file")
        expected_status=0
        [ -f "tests/fixtures/$name.exit" ] && expected_status=$(cat "tests/fixtures/$name.exit")
        if [ "$status" -eq "$expected_status" ] && [ "$stdout" = "$expected" ]; then
            echo "PASS $name"
            pass=$((pass + 1))
        else
            echo "FAIL $name (expected exit $expected_status with matching stdout)"
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

# The worked examples printed in the library manual (docs/lib/examples/<module>_example.zm) are real
# programs: each must print exactly its <module>.out, which the manual shows beside it.
for zm in docs/lib/examples/*_example.zm; do
    [ -f "$zm" ] || continue
    name=$(basename "$zm" .zm)
    if [ "$(timeout 5 "$BIN" "$zm" 2>&1)" = "$(cat "${zm%.zm}.out")" ]; then
        echo "PASS $name"; pass=$((pass + 1))
    else
        echo "FAIL $name (docs/lib/examples/$(basename "$zm") does not print its .out)"; fail=$((fail + 1))
    fi
done

# Real-signal tests (a fixture can't signal its own process).
while IFS= read -r line; do
    echo "$line"
    case "$line" in
        PASS*) pass=$((pass + 1)) ;;
        FAIL*) fail=$((fail + 1)) ;;
    esac
done < <(BIN="$BIN" tests/run_shutdown_test.sh 2>&1)

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
