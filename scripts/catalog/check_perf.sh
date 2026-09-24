#!/usr/bin/env bash
# scripts/catalog/check_perf.sh
#
# Catalog Phase-0 performance budgets (prd-studio-sample-catalog §9 Phase 0,
# memory-tests convention: budgets centralized here, never silently raised):
#
#   COLD_SCAN_FILES_PER_MIN   cold scan of the synthetic 100k corpus
#   RESCAN_MAX_SECONDS        unchanged-tree re-scan of the same corpus
#
# Generates the corpus with `nkido-catalog gen-corpus` in a temp dir (~100 MB
# for the default 100k files), scans into a temp DB, and asserts both budgets.
#
# Env overrides:
#   NKIDO_BIN_DIR             Directory holding nkido-catalog.
#                             Default: first existing of build/release/bin, build/bin.
#   CATALOG_PERF_FILES        Corpus size (default 100000).

set -euo pipefail

# --- budgets (do not raise casually; see PRD §9 Phase 0) ---
COLD_SCAN_FILES_PER_MIN=2000
RESCAN_MAX_SECONDS=5

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FILES="${CATALOG_PERF_FILES:-100000}"

if [[ -n "${NKIDO_BIN_DIR:-}" ]]; then
    BIN_DIR="$NKIDO_BIN_DIR"
elif [[ -x "$REPO_ROOT/build/release/bin/nkido-catalog" ]]; then
    BIN_DIR="$REPO_ROOT/build/release/bin"
elif [[ -x "$REPO_ROOT/build/bin/nkido-catalog" ]]; then
    BIN_DIR="$REPO_ROOT/build/bin"
else
    echo "check_perf: nkido-catalog binary not found (set NKIDO_BIN_DIR)" >&2
    exit 2
fi
CLI="$BIN_DIR/nkido-catalog"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/catalog_perf.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
CORPUS="$WORK/corpus"
DB="$WORK/catalog.db"

echo "== corpus: generating $FILES files =="
"$CLI" gen-corpus "$FILES" "$CORPUS" >/dev/null

echo "== cold scan =="
start=$(date +%s.%N)
"$CLI" scan "$CORPUS" --db "$DB" 2>/dev/null
end=$(date +%s.%N)
cold_s=$(echo "$end $start" | awk '{print $1 - $2}')
rate=$(echo "$FILES $cold_s" | awk '{printf "%d", $1 / $2 * 60}')
echo "cold scan: ${cold_s}s => ${rate} files/min (budget: >= ${COLD_SCAN_FILES_PER_MIN})"

echo "== unchanged re-scan =="
start=$(date +%s.%N)
"$CLI" scan "$CORPUS" --db "$DB" 2>/dev/null
end=$(date +%s.%N)
rescan_s=$(echo "$end $start" | awk '{print $1 - $2}')
echo "re-scan: ${rescan_s}s (budget: <= ${RESCAN_MAX_SECONDS}s)"

fail=0
if [[ "$(echo "$rate $COLD_SCAN_FILES_PER_MIN" | awk '{print ($1 >= $2)}')" != "1" ]]; then
    echo "FAIL: cold scan rate ${rate} files/min under budget ${COLD_SCAN_FILES_PER_MIN}" >&2
    fail=1
fi
if [[ "$(echo "$rescan_s $RESCAN_MAX_SECONDS" | awk '{print ($1 <= $2)}')" != "1" ]]; then
    echo "FAIL: re-scan ${rescan_s}s over budget ${RESCAN_MAX_SECONDS}s" >&2
    fail=1
fi

if [[ "$fail" == "0" ]]; then
    echo "catalog perf: ALL BUDGETS PASS"
fi
exit "$fail"
