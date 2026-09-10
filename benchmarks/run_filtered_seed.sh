#!/bin/bash
# Run the filtered-seed top-K benchmark (issues #434, #435)
#
# Measures what pg_textsearch.filtered_seed is worth on filtered top-k
# queries -- WHERE <filter> ORDER BY <score> LIMIT k -- by toggling the
# GUC over identical data and queries.  Results are identical either
# way, so the metrics are latency and scoring passes.
#
# The union2/union3 shapes are the #435 case: several BM25 scans of one
# index in one statement.  To see what #435 bought, run this on main and
# again on the branch: the on_passes column drops to one pass per arm
# only when the seed is bound per scan.
#
# Usage:
#   ./run_filtered_seed.sh [ndocs] [-- psql args...]
#
# Arguments:
#   ndocs: corpus size. Default 200000.  Below ~100k the planner may
#          prefer a seq scan and sort, and cells with no BM25 scan are
#          dropped with a warning.
#
# Examples:
#   ./run_filtered_seed.sh
#   ./run_filtered_seed.sh 1000000
#   ./run_filtered_seed.sh 200000 -- -p 55433 -d contrib_regression
#
# Outputs:
#   - Console output with the results table
#   - results/filtered_seed_<timestamp>.txt with raw output
#   - results/filtered_seed_<timestamp>.json with parsed metrics

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NDOCS="${1:-200000}"
shift || true
if [ "${1:-}" = "--" ]; then shift; fi

TS="$(date +%Y%m%d_%H%M%S)"
RESULTS_DIR="$SCRIPT_DIR/results"
OUTPUT_FILE="$RESULTS_DIR/filtered_seed_${TS}.txt"
METRICS_FILE="$RESULTS_DIR/filtered_seed_${TS}.json"

mkdir -p "$RESULTS_DIR"

echo "=== Filtered-Seed Top-K Benchmark ==="
echo "Corpus size: $NDOCS documents"
echo ""

psql -v ndocs="$NDOCS" "$@" \
    -f "$SCRIPT_DIR/sql/filtered_seed.sql" 2>&1 | tee "$OUTPUT_FILE"

if ! grep -q '^FSB_RESULT:' "$OUTPUT_FILE"; then
    echo ""
    echo "ERROR: no results produced. See $OUTPUT_FILE" >&2
    exit 1
fi

# Warn loudly rather than let a dropped cell pass unnoticed.
if grep -q 'no BM25 scan for shape=' "$OUTPUT_FILE"; then
    echo ""
    echo "WARNING: some cells were dropped because the planner chose a"
    echo "         seq scan instead of the BM25 index scan. Try a larger"
    echo "         corpus size."
fi

# FSB_RESULT: shape=X sel=Y limit=Z off_ms=.. off_passes=.. on_ms=..
#             on_passes=.. speedup=..
field() { sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<< "$1"; }

{
    echo "{"
    echo "  \"timestamp\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
    echo "  \"ndocs\": $NDOCS,"
    echo "  \"cells\": ["
    first=1
    while IFS= read -r line; do
        [ $first -eq 1 ] || echo ","
        first=0
        printf '    {"shape": "%s", "selectivity": "%s", "limit": %s, ' \
            "$(field "$line" shape)" \
            "$(field "$line" sel)" \
            "$(field "$line" limit)"
        printf '"bm25_scans": %s, ' "$(field "$line" scans)"
        printf '"seed_off_ms": %s, "seed_off_passes": %s, ' \
            "$(field "$line" off_ms)" \
            "$(field "$line" off_passes)"
        printf '"seed_on_ms": %s, "seed_on_passes": %s, "speedup": %s}' \
            "$(field "$line" on_ms)" \
            "$(field "$line" on_passes)" \
            "$(field "$line" speedup)"
    done < <(grep '^FSB_RESULT:' "$OUTPUT_FILE")
    echo ""
    echo "  ]"
    echo "}"
} > "$METRICS_FILE"

echo ""
echo "Results written to:"
echo "  - $OUTPUT_FILE (raw output)"
echo "  - $METRICS_FILE (parsed metrics)"
