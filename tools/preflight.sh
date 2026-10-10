#!/usr/bin/env bash
# Everything that is worth running before a release, and that `make check` deliberately leaves out.
#
# Why this exists: there is no CI in this repository, so every heavy target here -- the three
# sanitizers, memcheck, the static analyzer, the two-engine behavioural suite, the two database
# suites -- was a command with no occasion that ran it. On 2026-10-09 that class of gap was found
# three times in one day: the North Star application had stopped compiling with nothing watching it,
# the guard then written for it checked the demo and not the server, and the application's own
# 981-check suite had never been run from here. The suite's own header records a schema divergence
# that survived two commits because nothing ran `--postgres`. A target nobody runs guards nothing;
# this script is the occasion.
#
# It is still a command a person runs, not automation. What it adds over running each target by hand
# is that it probes for each prerequisite and says plainly what it skipped and why, so "I ran
# preflight" means something checkable rather than "I ran the parts that happened to work".
#
# usage: tools/preflight.sh [--list | --only STEP]
#   --list        probe everything and print what would run, without running it
#   --only STEP   run just that step (check, asan, ubsan, tsan, valgrind, analyze,
#                 northstar-both, test-pg, test-mysql) -- what you want after one of them fails
#
# Environment:
#   LDFLAGS            passed to the sanitizer builds, for a runtime unpacked outside /usr/lib64
#                      (libasan/libubsan/libtsan are packaged separately on some distributions)
#   ZIMAN_NORTHSTAR    the North Star checkout (default ../health-record/health-record-backend-ziman)
#
# Cost: roughly 15-25 minutes with everything available, most of it the sanitizers and the
# two-engine suite. Exit 0 only if every step that ran passed.
set -uo pipefail
cd "$(dirname "$0")/.."

LIST_ONLY=0
ONLY=""
case "${1:-}" in
    --list) LIST_ONLY=1 ;;
    --only) ONLY=${2:?--only needs a step name} ;;
    "")     ;;
    *)      echo "preflight: unknown option '$1' (try --list or --only STEP)" >&2; exit 2 ;;
esac

ran=0 failed=0 skipped=0
declare -a failures=() skips=()

# can_link <sanitizer> -- whether this machine can link against that sanitizer's runtime, honouring
# LDFLAGS so an unpacked copy elsewhere counts.
can_link() {
    local probe
    probe=$(mktemp /tmp/ziman-preflight-XXXXXX.c)
    printf 'int main(void){return 0;}\n' > "$probe"
    cc "-fsanitize=$1" ${LDFLAGS:-} -o "${probe%.c}.bin" "$probe" >/dev/null 2>&1
    local ok=$?
    rm -f "$probe" "${probe%.c}.bin"
    return $ok
}

step() {
    local name=$1 reason=$2 cmd=$3
    if [ -n "$ONLY" ] && [ "$ONLY" != "$name" ]; then return; fi
    if [ -n "$reason" ]; then
        skipped=$((skipped + 1)); skips+=("$name -- $reason")
        printf 'SKIP %-18s %s\n' "$name" "$reason"
        return
    fi
    if [ "$LIST_ONLY" = 1 ]; then printf 'would run %s\n' "$name"; return; fi
    printf '==== %s\n' "$name"
    if eval "$cmd" > ".preflight-$name.log" 2>&1; then
        ran=$((ran + 1)); printf 'PASS %s\n' "$name"
    else
        ran=$((ran + 1)); failed=$((failed + 1)); failures+=("$name (see .preflight-$name.log)")
        printf 'FAIL %-18s see .preflight-%s.log\n' "$name" "$name"
        tail -15 ".preflight-$name.log" | sed 's/^/     /'
    fi
}

runtime=$(command -v docker || command -v podman || true)
app=${ZIMAN_NORTHSTAR:-../health-record/health-record-backend-ziman}

step check "" "make check"
for san in address undefined thread; do
    case $san in
        address)   target=asan ;;
        undefined) target=ubsan ;;
        thread)    target=tsan ;;
    esac
    if can_link "$san"; then
        step "$target" "" "make $target"
    else
        step "$target" "no lib$( [ "$san" = address ] && echo asan || { [ "$san" = thread ] && echo tsan || echo ubsan; } ) runtime this cc can link (set LDFLAGS to one)" ""
    fi
done
step valgrind "$(command -v valgrind >/dev/null || echo 'valgrind is not on PATH')" "make valgrind"
step analyze "$(cc -fanalyzer -E - </dev/null >/dev/null 2>&1 || echo 'this cc has no -fanalyzer')" "make analyze"
step northstar-both \
    "$( [ -x "$app/src/test/other/run-all.sh" ] || echo "no North Star checkout at $app"; [ -n "$runtime" ] || echo 'no container runtime for the postgres engine')" \
    "make northstar-both"
step test-pg "$( [ -n "$runtime" ] || echo 'no container runtime')" "make test-pg"
step test-mysql "$( [ -n "$runtime" ] || echo 'no container runtime')" "make test-mysql"

if [ -n "$ONLY" ] && [ "$ran" -eq 0 ] && [ "$skipped" -eq 0 ]; then
    echo "preflight: no step named '$ONLY'" >&2
    exit 2
fi

echo
echo "preflight: $ran ran, $failed failed, $skipped skipped"
for s in ${skips+"${skips[@]}"}; do echo "  skipped: $s"; done
for f in ${failures+"${failures[@]}"}; do echo "  FAILED:  $f"; done
[ "$failed" -eq 0 ] || exit 1
[ "$skipped" -eq 0 ] || echo "preflight: not a full pass -- $skipped step(s) could not run here"
