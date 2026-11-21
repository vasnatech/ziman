#!/usr/bin/env bash
# `fs.write_text_private` writes a file only its owner can read -- and the mode is the whole point,
# so the mode is what this checks.
#
# Why this is a script and not a fixture: `fs` can write a file's permissions now but cannot read
# them, so a ziman program cannot see the thing being asserted. `stat` can. The same reasoning as
# tests/run_clipboard_test.sh, which is about a property that lives outside the process.
#
# usage: tests/run_private_file_test.sh [ziman-binary]   (default ./ziman)
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=$(cd "$(dirname "${1:-./ziman}")" && pwd)/$(basename "${1:-./ziman}")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0

cat > "$work/write.zm" <<'ZM'
use fs
use os

fn main() {
    val dir = switch os.get_env("WORK") { Some(d) { d } None { "." } }
    val secret = fs.path_join(dir, "token")
    print(unwrap(fs.write_text_private(secret, "a-bearer-token")))
    // Written a second time: a token is replaced when it is refreshed, so this has to overwrite
    // rather than refuse, and has to stay private when it does.
    print(unwrap(fs.write_text_private(secret, "a-longer-bearer-token")))
    print(unwrap(fs.read_text(secret)))
    // Beside it, the ordinary writer -- the comparison that makes the mode meaningful.
    val open_ = fs.path_join(dir, "public")
    print(unwrap(fs.write_text_atomic(open_, "not a secret")))
}
ZM

WORK="$work" "$BIN" "$work/write.zm" > "$work/out.txt" 2>&1
if [ "$(cat "$work/out.txt")" != "14
21
a-longer-bearer-token
12" ]; then
    echo "FAIL private_file_writes_and_overwrites (got: $(tr '\n' '|' < "$work/out.txt"))"
    fail=1
else
    echo "PASS private_file_writes_and_overwrites"
fi

mode=$(stat -c '%a' "$work/token" 2>/dev/null)
if [ "$mode" = "600" ]; then
    echo "PASS private_file_is_readable_only_by_its_owner"
else
    echo "FAIL private_file_is_readable_only_by_its_owner (mode $mode, wanted 600)"
    fail=1
fi

# **The comparison that gives the number meaning.** 600 on its own could be the umask rather than
# the call; the ordinary writer in the same directory in the same run is not 600, so it is the
# call.
open_mode=$(stat -c '%a' "$work/public" 2>/dev/null)
if [ "$open_mode" != "600" ]; then
    echo "PASS an_ordinary_write_is_not_private (mode $open_mode)"
else
    echo "FAIL an_ordinary_write_is_not_private (mode $open_mode -- the umask, not the call, may be what this suite is measuring)"
    fail=1
fi

# No temporary file is left behind on the happy path.
if [ -z "$(find "$work" -name 'token.tmp*' -print -quit)" ]; then
    echo "PASS private_file_leaves_no_temporary"
else
    echo "FAIL private_file_leaves_no_temporary"
    fail=1
fi

exit "$fail"
