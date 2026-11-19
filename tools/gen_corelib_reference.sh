#!/usr/bin/env bash
# Generates docs/CORELIB.md from every bundled module's own `pub` declarations --
# every struct and function signature is extracted straight from source
# (see the awk block below), so the reference can't silently drift from
# the actual API the way a hand-copied list would. Only the one-line
# per-module description in `describe()` is hand-maintained; add one
# there when adding a new core module. Regenerate with
# `make corelib-reference` after touching any lib/core or lib/db module.
set -euo pipefail
cd "$(dirname "$0")/.."

# One line per module, from the module's own header comment -- see tools/gen_module_summaries.py,
# which carries the argument. This was a hand-written `case` with one arm per module and
# `*) echo "" ;;` for anything it had not been told about, so **42 of the 92 bundled modules
# shipped here with a bare signature dump and no description**, and the `git diff` gate was happy
# because the generator had produced the blanks itself.
summaries=$(python3 tools/gen_module_summaries.py)

describe() {
    printf '%s\n' "$summaries" | awk -F'\t' -v want="$1" '$1 == want { print $2; found = 1 } END { exit !found }'
}


cat <<'EOF'
# Ziman core module reference

Auto-generated from every bundled module's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub enum`/
`pub fn`/`pub native fn`/`pub val`). This is a signature lookup, not an
explanation -- see the manual in docs/lib/ for how to use each module, docs/syntax/ for full semantics and docs/design/journal/
for why each module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/syntax/'s "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.

One exception, not listed below: `lib/prelude/` -- unlike every
module here, it's never `use`-able at all (see its own header comment
and docs/design/journal/'s "Prelude" entry), since everything it declares
(`Option<T>`, plus a handful of native functions) is already bare and
unqualified in every program, the same way `print`/`str`/`parse_int`
are. See docs/syntax/'s "Modules" section for what it contains.
EOF

# **Every directory a bundled module can live in, and the five that live one deeper.**
#
# This globbed `lib/core` and `lib/db` when it was written, and `lib/http` and `lib/time` were
# added to the glob as those appeared. `lib/graphics` and `lib/ui` never were -- so **29 of the 92
# bundled modules had no entry here**, among them `widget`, `controls`, `paint`, `raster`,
# `window` and the whole HTTP server, while README.md called this file "every bundled module's
# `pub` signatures" and CHEATSHEET.md called it "every module and every function in it".
#
# The `git diff` gate could not see it: the generator never produced those sections, so there was
# nothing for a diff against the last run to differ from. Same shape as the VS Code grammar whose
# two generated rules matched nothing, and as the stale `charset_source.h` that hid a broken clean
# build -- a check that compares is not a check that verifies.
#
# A multi-part bundled module (`charset`, `theme`, `server`, `client`, `rest`) keeps its parts in a
# folder and registers them all under the folder's name, so the folder is the module and its
# first part carries the header comment.
modules=$(
    for f in lib/*.zm lib/core/*.zm lib/db/*.zm lib/graphics/*.zm lib/http/*.zm lib/time/*.zm \
             lib/ui/*.zm lib/ui/desktop/*.zm lib/ui/terminal/*.zm; do
        [ -e "$f" ] || continue
        echo "$(basename "$f" .zm) $f"
    done
    for d in lib/core/charset lib/ui/theme lib/http/server lib/http/client lib/http/rest; do
        [ -d "$d" ] || continue
        name=$(basename "$d")
        echo "$name $d/$name.zm"
    done
)

echo "$modules" | sort -u | while read -r name f; do
    if [ -z "$name" ]; then continue; fi
    if [ "$name" = "prelude" ]; then continue; fi
    desc=$(describe "$name")
    # The "Experimental" label comes from the module's own `#![experimental]` attribute, not from a
    # hand-written prefix in `describe` above -- one source of truth, so the reference, the manual
    # (tools/gen_lib_manual.py) and the module itself cannot drift apart. See docs/roadmap/.
    if grep -q '^#!\[experimental\]' "$f"; then
        desc="**Experimental** (see docs/design/journal/ \"Experimental modules\"). $desc"
    fi
    echo
    echo "## \`$name\`"
    echo
    if [ -n "$desc" ]; then
        echo "$desc"
        echo
    fi
    echo '```'
    awk '
        # A text block holds Ziman *source a generator writes*, not Ziman this module declares.
        # `lib/core/derive_*.zm` keep their templates in one, and every `pub fn @{snake}_to_json`
        # in there was scraped into this reference as though the corelib exported it -- eleven
        # functions that do not exist, named with holes. `tools/gen_lib_manual.py` had the same
        # bug and skips these the same way.
        /"""/ {
            if (gsub(/"""/, "&") % 2 == 1) in_text = !in_text
            next
        }
        in_text { next }
        /^pub (struct|enum)/ { print; in_block=1; next }
        in_block && /^}/ { print; print ""; in_block=0; next }
        in_block { print; next }
        /^pub (native )?fn/ {
            line = $0
            sub(/ \{$/, "", line)
            print line
            print ""
        }
        /^pub val/ { print; print "" }
    ' "$f"
    echo '```'
done
