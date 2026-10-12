#!/usr/bin/env bash
# Contamination screen for a cold round. Detection, not prevention: an attempt that spells a
# Ziman-only library name did not guess it from training distribution, so the run is not cold.
# The patterns are library spellings that exist nowhere else -- deliberately NOT generic keywords
# like `val`, `switch` or `try`, which a cold model can and does reach for by accident.
set -uo pipefail
dir="${1:?usage: screen.sh <attempts-dir>}"
pat='fs\.read_text|fs\.write_text|mapb_|sb_to_string|ab_to_array|sb_append|ab_push|set_items|map_keys|map_get_or|collections\.sort_with|number\.format_number|parse_pattern|unwrap_Some|net\.listen_tls|process\.run\(program:|--api|ziman --check'
flagged=0 total=0
for f in "$dir"/*.zm; do
  [ -f "$f" ] || continue
  total=$((total + 1))
  if hits=$(grep -oE "$pat" "$f" | sort -u | tr '\n' ' '); [ -n "$hits" ]; then
    printf 'FLAGGED  %-28s %s\n' "$(basename "$f")" "$hits"
    flagged=$((flagged + 1))
  fi
done
printf -- '----\n%d attempts screened, %d flagged\n' "$total" "$flagged"
