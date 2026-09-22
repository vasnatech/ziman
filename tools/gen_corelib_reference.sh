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
        collections) echo "\`HashMap<K, V>\`, \`HashSet<T>\` -- generic containers, written in pure Ziman on top of the language's own \`Eq\`/\`Hash\` bounds." ;;
        io) echo "File I/O. The first native module -- proved the \`native fn\` mechanism end to end." ;;
        random) echo "One native function reading real OS entropy (\`/dev/urandom\`) -- Ziman's first source of genuine non-determinism." ;;
        uuid) echo "RFC 4122 v4 UUIDs, written in pure Ziman on top of \`random\` -- version/variant bits set with plain integer arithmetic, since Ziman has no bitwise operators." ;;
        clock) echo "One native function reading the system clock (\`clock_gettime\`) -- Ziman's other source of genuine non-determinism." ;;
        time) echo "Seven \`java.time\`-shaped types (\`LocalDate\`, \`LocalTime\`, \`LocalDateTime\`, \`Duration\`, \`Instant\`, \`ZoneOffset\`, \`ZoneId\`), written in pure Ziman on top of \`clock\`. \`ZoneOffset\` only -- no IANA-style \`ZoneId\` with DST rules. Every type has a fixed ISO-8601 \`_to_string\`/\`_parse\` pair; \`LocalDate\`/\`LocalTime\`/\`LocalDateTime\`/\`Instant\` also get a general \`_format\`/\`_parse_pattern\` pair taking a \`DateTimeFormat\` -- a pattern compiled once by \`datetime_format_of\` (see the \`iso_*_format\` vals, each compiled exactly once at program start) rather than a raw pattern string re-scanned on every call." ;;
        json) echo "A JSON parser/serializer (\`Json\`, a genuinely recursive enum), written in pure Ziman -- no native code needed. \`\\uXXXX\` unicode escapes and \`==\`/\`!=\` on \`Json\` are the two known gaps." ;;
        number) echo "Shared formatting/parsing across \`int\`/\`float\`/\`bigint\`/\`decimal\`/\`byte\`, written in pure Ziman on top of each type's own \`str()\`. \`NumberFormat\`/\`parse_pattern\` cover thousands separators and forced decimal places (a small subset of DecimalFormat-style patterns); \`byte_to_hex\`/\`int_to_hex\`/\`bigint_to_hex\` (and their \`_from_hex\` inverses) cover hex for the three integral types." ;;
        *) echo "" ;;
    esac
}

cat <<'EOF'
# Ziman core module reference

Auto-generated from each `corelib/*.zm` file's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub enum`/
`pub fn`/`pub native fn`/`pub val`). This is a signature lookup, not an
explanation -- see docs/SYNTAX.md for full semantics and docs/DESIGN.md
for why each module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/SYNTAX.md's "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.

One exception, not listed below: `corelib/prelude.zm` -- unlike every
module here, it's never `use`-able at all (see its own header comment
and docs/DESIGN.md's "Prelude" entry), since everything it declares
(`Option<T>`, plus a handful of native functions) is already bare and
unqualified in every program, the same way `print`/`str`/`parse_int`
are. See docs/SYNTAX.md's "Modules" section for what it contains.
EOF

for f in corelib/*.zm; do
    name=$(basename "$f" .zm)
    if [ "$name" = "prelude" ]; then continue; fi
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
        /^pub val/ { print; print "" }
    ' "$f"
    echo '```'
done
