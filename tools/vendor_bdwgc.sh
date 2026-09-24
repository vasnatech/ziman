#!/usr/bin/env bash
# Refreshes vendor/bdwgc from a bdwgc commit (default: the pinned one).
# usage: tools/vendor_bdwgc.sh [commit]
set -euo pipefail
cd "$(dirname "$0")/.."
COMMIT=${1:-2547e33}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
git clone --quiet https://github.com/ivmai/bdwgc.git "$TMP/bdwgc"
git -C "$TMP/bdwgc" checkout --quiet "$COMMIT"
rm -rf vendor/bdwgc/*.c vendor/bdwgc/include vendor/bdwgc/extra vendor/bdwgc/LICENSE
mkdir -p vendor/bdwgc/extra
for f in $(grep -o '"\.\./[a-z_]*\.c"' "$TMP/bdwgc/extra/gc.c" | tr -d '"' | sed 's|\.\./||' | sort -u); do
    cp "$TMP/bdwgc/$f" vendor/bdwgc/
done
cp -r "$TMP/bdwgc/include" vendor/bdwgc/
cp "$TMP/bdwgc/extra/gc.c" vendor/bdwgc/extra/
cp "$TMP/bdwgc/LICENSE" vendor/bdwgc/
sed -i "s/commit \`[0-9a-f]*\`/commit \`$COMMIT\`/" vendor/bdwgc/README.md
echo "vendored bdwgc $COMMIT into vendor/bdwgc"
