#!/usr/bin/env bash
# Generates docs/CORELIB.md from the lib/core and lib/db modules' own `pub` declarations --
# every struct and function signature is extracted straight from source
# (see the awk block below), so the reference can't silently drift from
# the actual API the way a hand-copied list would. Only the one-line
# per-module description in `describe()` is hand-maintained; add one
# there when adding a new core module. Regenerate with
# `make corelib-reference` after touching any lib/core or lib/db module.
set -euo pipefail
cd "$(dirname "$0")/.."

describe() {
    case "$1" in
        io) echo "File I/O. The first native module -- proved the \`native fn\` mechanism end to end." ;;
        random) echo "One native function reading real OS entropy (\`/dev/urandom\`) -- Ziman's first source of genuine non-determinism." ;;
        uuid) echo "RFC 4122 v4 UUIDs, written in pure Ziman on top of \`random\` -- version/variant bits set with plain integer arithmetic, since Ziman has no bitwise operators." ;;
        clock) echo "One native function reading the system clock (\`clock_gettime\`) -- Ziman's other source of genuine non-determinism." ;;
        time) echo "Seven \`java.time\`-shaped types (\`LocalDate\`, \`LocalTime\`, \`LocalDateTime\`, \`Duration\`, \`Instant\`, \`ZoneOffset\`, \`ZoneId\`), written in pure Ziman on top of \`clock\`. \`ZoneId\` reads the IANA zone files (see \`tzdata\`). Every type has a fixed ISO-8601 \`_to_string\`/\`_parse\` pair; \`LocalDate\`/\`LocalTime\`/\`LocalDateTime\`/\`Instant\` also get a general \`_format\`/\`_parse_pattern\` pair taking a \`DateTimeFormat\` -- a pattern compiled once by \`datetime_format_of\` (see the \`iso_*_format\` vals, each compiled exactly once at program start) rather than a raw pattern string re-scanned on every call." ;;
        zone) echo "The IANA time zone database, read from the operating system's zone files, for \`time\`'s named zones (\`zoneid_of\`)." ;;
        markdown) echo "Markdown as a document tree: a reader over a declared subset, a writer, and the escaping that makes a generated document safe. The writer is the point -- \`|\` in a table cell, the \`*_[]\` family in text, a leading \`#\` or \`1.\`, a code span fenced longer than whatever is inside it -- so nothing a caller puts in a string can change the structure of the output. Markdown has no syntax errors, so a reader cannot refuse what it does not handle: \`parse_markdown\` reads anything unsupported as text, and \`parse_markdown_strict\` returns an Err naming the line instead." ;;
        xml) echo "XML 1.0 as a document tree: a reader, a writer and a small path finder, in pure Ziman. Not a \`Json\` tree -- XML interleaves text with elements, separates attributes from children and identifies a name by its namespace URI, none of which survive a map. There is no DTD at all, so \`<!DOCTYPE>\` is a parse error and entity-expansion and external-entity attacks have nowhere to stand; size, depth and attribute-count limits are in \`XmlLimits\`." ;;
        mime) echo "Media types (\`type/subtype; name=value\`): parse, write, match with wildcards, and the type of a file by its extension." ;;
        json) echo "A JSON parser/serializer (\`Json\`, a genuinely recursive enum), written in pure Ziman -- no native code needed. \`\\uXXXX\` unicode escapes are read; \`==\`/\`!=\` on \`Json\` is the one known gap." ;;
        number) echo "Shared formatting/parsing across \`int\`/\`float\`/\`bigint\`/\`decimal\`/\`byte\`, written in pure Ziman on top of each type's own \`str()\`. \`NumberFormat\`/\`parse_pattern\` cover thousands separators and forced decimal places (a small subset of DecimalFormat-style patterns); \`byte_to_hex\`/\`int_to_hex\`/\`bigint_to_hex\` (and their \`_from_hex\` inverses) cover hex for the three integral types." ;;
        crypto) echo "Hashes (SHA-1/256/384/512), HMAC, PBKDF2, password hashing (bcrypt, Argon2id), a constant-time byte comparison, base64 and base64url, on OpenSSL. No ciphers or public-key operations." ;;
        otp) echo "Time-based one-time passwords (RFC 6238): the six-digit codes of an authenticator app -- generate a secret, a provisioning URI, and check a code." ;;
        migrate) echo "Versioned schema migrations: a directory of V<n>__<name>.sql files applied in order, once each, in one transaction, with checksums to catch an edited migration." ;;
        jwt) echo "JSON Web Tokens signed with a shared secret (HS256/384/512): sign, and verify with expiry checks." ;;
        ndjson) echo "Newline-delimited JSON (one value per line) on top of \`json\`." ;;
        csv) echo "RFC 4180 CSV as a \`Json\` array of objects keyed by the header row (typed or string cells; TSV via the \`_with\` variants); a table writer that flattens one level." ;;
        toml) echo "TOML 1.0 to and from a \`Json\` tree. Dates stay strings; \`inf\`/\`nan\` are rejected." ;;
        yaml) echo "A strict YAML 1.2 subset to and from a \`Json\` tree: block and flow collections, quoted/plain/block scalars. Anchors, tags, merge keys and multiple documents are errors." ;;
        pg) echo "A PostgreSQL client, the second engine behind \`datasource\`: the simple and extended query protocols, trust/cleartext/SCRAM-SHA-256 authentication, TLS. No MD5 auth, Kerberos, client certificates, COPY or LISTEN/NOTIFY." ;;
        mysql) echo "A MySQL/MariaDB client, the third engine behind \`datasource\`: the text and binary protocols, \`mysql_native_password\`/\`caching_sha2_password\` authentication, TLS. No compressed protocol, LOCAL INFILE, the RSA key exchange, or prepared-statement reuse." ;;
        *) echo "" ;;
    esac
}

cat <<'EOF'
# Ziman core module reference

Auto-generated from each `lib/core/*.zm` or `lib/db/*.zm` file's own `pub` declarations by
`tools/gen_corelib_reference.sh` -- **don't hand-edit the signature
blocks below**; regenerate with `make corelib-reference` after changing
any core module (adding, removing, or renaming a `pub struct`/`pub enum`/
`pub fn`/`pub native fn`/`pub val`). This is a signature lookup, not an
explanation -- see the manual in docs/lib/ for how to use each module, docs/SYNTAX.md for full semantics and docs/DESIGN.md
for why each module is shaped the way it is.

`use <module>` resolves any of these with no local file needed (see
docs/SYNTAX.md's "Modules" -- "Bundled core module"); a per-name import
(`use <module>.Name`) binds one item bare instead of qualified.

One exception, not listed below: `lib/prelude/` -- unlike every
module here, it's never `use`-able at all (see its own header comment
and docs/DESIGN.md's "Prelude" entry), since everything it declares
(`Option<T>`, plus a handful of native functions) is already bare and
unqualified in every program, the same way `print`/`str`/`parse_int`
are. See docs/SYNTAX.md's "Modules" section for what it contains.
EOF

for f in lib/core/*.zm lib/db/*.zm lib/http/*.zm lib/time/*.zm; do
    name=$(basename "$f" .zm)
    if [ "$name" = "prelude" ]; then continue; fi
    desc=$(describe "$name")
    # The "Experimental" label comes from the module's own `#![experimental]` attribute, not from a
    # hand-written prefix in `describe` above -- one source of truth, so the reference, the manual
    # (tools/gen_lib_manual.py) and the module itself cannot drift apart. See docs/ROADMAP.md.
    if grep -q '^#!\[experimental\]' "$f"; then
        desc="**Experimental** (see docs/DESIGN.md \"Experimental modules\"). $desc"
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
