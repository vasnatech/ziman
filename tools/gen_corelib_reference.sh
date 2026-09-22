#!/usr/bin/env bash
# Generates docs/CORELIB.md from corelib/*.zm's own `pub` declarations --
# every struct and function signature is extracted straight from source
# (see the awk block below), so the reference can't silently drift from
# the actual API the way a hand-copied list would. Only the one-line
# per-module description in `describe()` is hand-maintained; add one
# there when adding a new core module. Regenerate with
# `make corelib-reference` after touching any corelib/*.zm file.
set -euo pipefail
cd "$(dirname "$0")/.."

describe() {
    case "$1" in
        collections) echo "\`Box<T>\`, \`Option<T>\`, \`HashMap<K, V>\`, \`HashSet<T>\` -- generic containers, written in pure Ziman on top of the language's own \`Eq\`/\`Hash\` bounds." ;;
        io) echo "File I/O. The first native module -- proved the \`native fn\` mechanism end to end." ;;
        random) echo "One native function reading real OS entropy (\`/dev/urandom\`) -- Ziman's first source of genuine non-determinism." ;;
        uuid) echo "RFC 4122 v4 UUIDs, written in pure Ziman on top of \`random\` -- version/variant bits set with plain integer arithmetic, since Ziman has no bitwise operators." ;;
        clock) echo "One native function reading the system clock (\`clock_gettime\`) -- Ziman's other source of genuine non-determinism." ;;
        time) echo "Seven \`java.time\`-shaped types (\`LocalDate\`, \`LocalTime\`, \`LocalDateTime\`, \`Duration\`, \`Instant\`, \`ZoneOffset\`, \`ZoneId\`), written in pure Ziman on top of \`clock\`. \`ZoneOffset\` only -- no IANA-style \`ZoneId\` with DST rules." ;;
        *) echo "" ;;
    esac
}

cat <<'EOF'
# Ziman core module reference

Auto-generated from each `corelib/*.zm` file's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub fn`/
`pub native fn`). This is a signature lookup, not an explanation --
see docs/SYNTAX.md for full semantics and docs/DESIGN.md for why each
module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/SYNTAX.md's "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.
EOF

for f in corelib/*.zm; do
    name=$(basename "$f" .zm)
    desc=$(describe "$name")
    echo
    echo "## \`$name\`"
    echo
    if [ -n "$desc" ]; then
        echo "$desc"
        echo
    fi
    echo '```'
    awk '
        /^pub (struct|enum)/ { print; in_block=1; next }
        in_block && /^}/ { print; print ""; in_block=0; next }
        in_block { print; next }
        /^pub (native )?fn/ {
            line = $0
            sub(/ \{$/, "", line)
            print line
            print ""
        }
    ' "$f"
    echo '```'
done
