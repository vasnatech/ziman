#!/usr/bin/env bash
# Runs every fixture in tests/lang/.
#
# Single-file fixtures: for each name.zm:
#   - if name.out exists, the program must exit 0 (or the code in a sibling name.exit,
#     for a fixture whose successful, exact-stdout-checked output -- `ziman test`'s
#     PASS/FAIL report, say -- comes with a non-zero exit, such as one or more failed
#     tests) and print exactly that on stdout.
#   - if name.err exists, the program must exit non-zero and that file's content
#     must appear as a substring somewhere in stderr.
#   - if name.diag exists, the program must exit non-zero and its `--json`
#     diagnostics must match that file line for line: `<phase> <line>:<col>`
#     and an optional message substring, one line per diagnostic in order
#     (`...` as a last line allows more after those). `--json` is added to the
#     run automatically, since that is what a .diag fixture is for.
#
#     This is the one kind that can assert *where* an error was reported. A .err
#     fixture says a message appeared somewhere in stderr and nothing about its
#     position, so a diagnostic that names the right problem at the wrong place
#     passes it -- which is exactly what tasks.md F50 and F53 turned out to be.
#     Leaving the substring off asserts position alone, so a fixture about where
#     an error lands does not break when someone rewords the message.
#
# Either kind may have a sibling name.env (KEY=VALUE lines) exported for its run.
#
# SKIP="name1 name2" leaves those fixtures out, reporting each as skipped. For a fixture that
# cannot hold under a particular build rather than one that is broken: `make asan` uses it for
# stack_depth_deep_recursion_works, whose frames are several times larger under the sanitizer than
# the stack its own --stack-mb=256 asks for (docs/reviews/tasks.md F14).
#
# Multi-file fixtures: a directory tests/lang/name/ with an entry point at
# name/main.zm (and whatever other .zm files it `use`s underneath it) is run
# the same way, checked against sibling name.out / name.err files (outside
# the directory, so the naming convention matches single-file fixtures).
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./ziman}
SKIP=${SKIP:-}
# Seconds each fixture gets before it is called a deadlock. Five is right for an ordinary
# build and far too tight under a sanitizer: db_cursor_finalizer_no_self_deadlock, a
# 1500-round finalizer stress fixture, runs in well under a second normally and 5.1-5.2s
# under ziman-tsan-gc. Raising it for every build would weaken the guard; the sanitizer
# targets pass a larger one instead.
TIMEOUT=${TIMEOUT:-5}
pass=0
fail=0
skipped=0

check_fixture() {
    local name="$1" entry="$2"
    for s in $SKIP; do
        if [ "$s" = "$name" ]; then
            echo "SKIP $name"
            skipped=$((skipped + 1))
            return
        fi
    done
    local out_file="tests/lang/$name.out"
    local err_file="tests/lang/$name.err"

    # An optional tests/lang/<name>.env holds KEY=VALUE lines (blank lines and
    # #-comments ignored) exported for just this fixture's run -- how a fixture
    # exercises os.getenv / environment-variable overrides, since a Ziman program
    # can't set its own environment.
    local env_args=()
    local env_file="tests/lang/$name.env"
    if [ -f "$env_file" ]; then
        while IFS= read -r line; do
            case "$line" in ""|\#*) continue ;; esac
            env_args+=("$line")
        done < "$env_file"
    fi

    # An optional tests/lang/<name>.args holds command-line options (space
    # separated) passed to ziman before the file -- how a fixture exercises
    # --check, --json and --emit-derived.
    local cli_args=()
    local args_file="tests/lang/$name.args"
    if [ -f "$args_file" ]; then
        read -r -a cli_args < "$args_file"
    fi
    # A .diag fixture asserts the structured diagnostics, so it gets --json whether it asked or
    # not: forgetting it would make the fixture compare an empty JSON stream against expectations
    # and fail for a reason that has nothing to do with what it is testing.
    local diag_file="tests/lang/$name.diag"
    if [ -f "$diag_file" ]; then
        cli_args=("--json" "${cli_args[@]+"${cli_args[@]}"}")
    fi

    local stdout status stderr
    # `timeout` guards against a genuine deadlock in a spawn/Channel fixture
    # (a mistake in the interpreter's channel/thread handling) hanging the
    # whole suite forever instead of just failing that one fixture.
    # An optional <name>.progargs holds the program's own arguments (after `--`).
    local prog_args=()
    if [ -f "tests/lang/$name.progargs" ]; then
        read -r -a prog_args < "tests/lang/$name.progargs"
        prog_args=(-- "${prog_args[@]}")
    fi
    stdout=$(env "${env_args[@]}" timeout "$TIMEOUT" "$BIN" "${cli_args[@]+"${cli_args[@]}"}" "$entry" "${prog_args[@]+"${prog_args[@]}"}" 2>/tmp/ziman_test_stderr.$$)
    status=$?
    stderr=$(cat /tmp/ziman_test_stderr.$$)
    rm -f /tmp/ziman_test_stderr.$$
    if [ "$status" -eq 124 ]; then
        stderr="$stderr"$'\n'"(timed out after ${TIMEOUT}s -- possible deadlock)"
    fi

    if [ -f "$out_file" ]; then
        local expected expected_status
        expected=$(cat "$out_file")
        expected_status=0
        [ -f "tests/lang/$name.exit" ] && expected_status=$(cat "tests/lang/$name.exit")
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
    elif [ -f "$diag_file" ]; then
        local report
        printf '%s\n' "$stderr" > "/tmp/ziman_diag_actual.$$"
        report=$(python3 tools/check_diag.py "$diag_file" "/tmp/ziman_diag_actual.$$" 2>&1)
        local diag_status=$?
        rm -f "/tmp/ziman_diag_actual.$$"
        # Diagnostics do not always mean failure. A `#[deprecated]` use is a warning on an ordinary
        # compile, so the program type-checks and exits 0 while still reporting something -- which
        # this branch used to call a failure, because when .diag was added (F57) every diagnostic was
        # an error. `name.exit` names the status to require, exactly as it does for a .out fixture;
        # with no such file the rule is the original "must have failed" (F68).
        local diag_expected_status=""
        [ -f "tests/lang/$name.exit" ] && diag_expected_status=$(cat "tests/lang/$name.exit")
        local status_ok=0
        if [ -n "$diag_expected_status" ]; then
            [ "$status" -eq "$diag_expected_status" ] && status_ok=1
        else
            [ "$status" -ne 0 ] && status_ok=1
        fi
        if [ "$status_ok" -eq 1 ] && [ "$diag_status" -eq 0 ]; then
            echo "PASS $name"
            pass=$((pass + 1))
        else
            echo "FAIL $name (diagnostics do not match $diag_file)"
            echo "  exit=$status${diag_expected_status:+, wanted $diag_expected_status}"
            [ -n "$report" ] && echo "$report"
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
        echo "SKIP $name (no .out, .diag or .err fixture)"
    fi
}

for zm in tests/lang/*.zm; do
    name=$(basename "$zm" .zm)
    check_fixture "$name" "$zm"
done

for dir in tests/lang/*/; do
    name=$(basename "$dir")
    entry="tests/lang/$name/main.zm"
    if [ -f "$entry" ]; then
        check_fixture "$name" "$entry"
    fi
done

# The worked examples printed in the library manual (docs/lib/examples/<folder>/<module>_example.zm) are real
# programs: each must print exactly its <module>.out, which the manual shows beside it.
for zm in docs/lib/examples/*/*_example.zm; do
    [ -f "$zm" ] || continue
    name=$(basename "$zm" .zm)
    if [ "$(timeout "$TIMEOUT" "$BIN" "$zm" 2>&1)" = "$(cat "${zm%.zm}.out")" ]; then
        echo "PASS $name"; pass=$((pass + 1))
    else
        echo "FAIL $name (${zm} does not print its .out)"; fail=$((fail + 1))
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
if [ "$skipped" -gt 0 ]; then echo "$pass passed, $fail failed, $skipped skipped"; else echo "$pass passed, $fail failed"; fi
[ "$fail" -eq 0 ]
