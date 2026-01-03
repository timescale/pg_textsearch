#!/bin/bash
# Extract benchmark metrics from log output and create JSON summary
#
# Usage: ./extract_metrics.sh <log> <output_json> [dataset_name] [section]
#
# If section is specified (e.g., "Cranfield", "MS MARCO", "Wikipedia"),
# only extracts metrics from that section of the log file.
#
# Parses benchmark output to extract:
# - Index build time
# - Query latencies
# - Throughput metrics
# - Index/table sizes

set -e

LOG_FILE="${1:-benchmark_results.txt}"
OUTPUT_FILE="${2:-benchmark_metrics.json}"
DATASET_NAME="${3:-unknown}"
SECTION="${4:-}"

# If section specified, extract only that section from log
if [ -n "$SECTION" ]; then
    TEMP_LOG=$(mktemp)
    # Extract lines between "=== $SECTION Benchmark ===" and next "=== * Benchmark ==="
    awk -v section="$SECTION" '
        /^=== .* Benchmark ===/ {
            if (index($0, section) > 0) { capture = 1 }
            else if (capture) { exit }
        }
        capture { print }
    ' "$LOG_FILE" > "$TEMP_LOG"
    LOG_FILE="$TEMP_LOG"
    trap "rm -f $TEMP_LOG" EXIT
fi

# Initialize metrics
INDEX_BUILD_MS=""
LOAD_TIME_MS=""
NUM_DOCUMENTS=""
INDEX_SIZE=""
TABLE_SIZE=""
declare -a QUERY_LATENCIES

# Extract index build time (from CREATE INDEX timing)
INDEX_BUILD_MS=$(grep -E "CREATE INDEX" "$LOG_FILE" -A 1 2>/dev/null | \
    grep -oE "Time: [0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")

# Extract data load time (COPY statements - format: "Time: 11235.512 ms")
LOAD_TIME_MS=$(grep -E "^COPY [0-9]+" "$LOG_FILE" -A 1 2>/dev/null | \
    grep -oE "Time: [0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")

# Extract document count from index build notice (pg_textsearch) or passages loaded (ParadeDB)
NUM_DOCUMENTS=$(grep -E "BM25 index build completed:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ documents" | grep -oE "[0-9]+" || echo "")
# Fallback: ParadeDB/generic format "Passages loaded: | NNNNNNN"
if [ -z "$NUM_DOCUMENTS" ]; then
    NUM_DOCUMENTS=$(grep -E "Passages loaded:" "$LOG_FILE" 2>/dev/null | \
        grep -oE "\| [0-9]+" | grep -oE "[0-9]+" || echo "")
fi

# Extract Execution Time from benchmark query outputs
# Format: "Execution Time: 123.456 ms (min=..., max=...)"
# We extract only the first number (median) from each line
# Support both old format (5 queries) and new format (10 queries: 5 no-score + 5 with-score)
mapfile -t EXEC_TIMES < <(grep -E "Execution Time:" "$LOG_FILE" 2>/dev/null | \
    sed -E 's/.*Execution Time: ([0-9]+\.[0-9]+).*/\1/' || true)

# If we have 10 results, split into no-score (first 5) and with-score (last 5)
if [ ${#EXEC_TIMES[@]} -ge 10 ]; then
    EXEC_TIMES_NO_SCORE=("${EXEC_TIMES[@]:0:5}")
    EXEC_TIMES_WITH_SCORE=("${EXEC_TIMES[@]:5:5}")
else
    # Old format - just use what we have
    EXEC_TIMES_NO_SCORE=("${EXEC_TIMES[@]}")
    EXEC_TIMES_WITH_SCORE=()
fi

# Extract throughput result (support both old and new format)
THROUGHPUT_LINE=$(grep -E "THROUGHPUT_RESULT:" "$LOG_FILE" 2>/dev/null || \
    grep -E "THROUGHPUT_RESULT_NO_SCORE:" "$LOG_FILE" 2>/dev/null || echo "")
THROUGHPUT_TOTAL_MS=""
THROUGHPUT_AVG_MS=""
if [ -n "$THROUGHPUT_LINE" ]; then
    THROUGHPUT_TOTAL_MS=$(echo "$THROUGHPUT_LINE" | \
        grep -oE "[0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")
    THROUGHPUT_AVG_MS=$(echo "$THROUGHPUT_LINE" | \
        grep -oE "avg [0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
fi

# Extract with-score throughput if available
THROUGHPUT_SCORE_LINE=$(grep -E "THROUGHPUT_RESULT_WITH_SCORE:" "$LOG_FILE" 2>/dev/null || echo "")
THROUGHPUT_SCORE_TOTAL_MS=""
THROUGHPUT_SCORE_AVG_MS=""
if [ -n "$THROUGHPUT_SCORE_LINE" ]; then
    THROUGHPUT_SCORE_TOTAL_MS=$(echo "$THROUGHPUT_SCORE_LINE" | \
        grep -oE "[0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")
    THROUGHPUT_SCORE_AVG_MS=$(echo "$THROUGHPUT_SCORE_LINE" | \
        grep -oE "avg [0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
fi

# Extract index and table sizes from standardized output format
# Format: INDEX_SIZE: | 123 MB | 128974848
INDEX_SIZE=$(grep -E "INDEX_SIZE:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ [kMGT]?B" | head -1 || echo "")
INDEX_SIZE_BYTES=$(grep -E "INDEX_SIZE:" "$LOG_FILE" 2>/dev/null | \
    awk '{print $NF}' | grep -E "^[0-9]+$" | head -1 || echo "")
TABLE_SIZE=$(grep -E "TABLE_SIZE:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ [kMGT]?B" | head -1 || echo "")
TABLE_SIZE_BYTES=$(grep -E "TABLE_SIZE:" "$LOG_FILE" 2>/dev/null | \
    awk '{print $NF}' | grep -E "^[0-9]+$" | head -1 || echo "")

# Helper to output number or null
num_or_null() {
    if [ -n "$1" ] && [[ "$1" =~ ^[0-9]+\.?[0-9]*$ ]]; then
        echo "$1"
    else
        echo "null"
    fi
}

# Build JSON output using jq for proper formatting
jq -n \
  --arg timestamp "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  --arg commit "${GITHUB_SHA:-unknown}" \
  --arg dataset "$DATASET_NAME" \
  --argjson load_time "$(num_or_null "$LOAD_TIME_MS")" \
  --argjson index_build "$(num_or_null "$INDEX_BUILD_MS")" \
  --argjson num_docs "$(num_or_null "$NUM_DOCUMENTS")" \
  --arg index_size "${INDEX_SIZE:-unknown}" \
  --argjson index_size_bytes "$(num_or_null "$INDEX_SIZE_BYTES")" \
  --arg table_size "${TABLE_SIZE:-unknown}" \
  --argjson table_size_bytes "$(num_or_null "$TABLE_SIZE_BYTES")" \
  --argjson short_query "$(num_or_null "${EXEC_TIMES_NO_SCORE[0]}")" \
  --argjson medium_query "$(num_or_null "${EXEC_TIMES_NO_SCORE[1]}")" \
  --argjson long_query "$(num_or_null "${EXEC_TIMES_NO_SCORE[2]}")" \
  --argjson common_term "$(num_or_null "${EXEC_TIMES_NO_SCORE[3]}")" \
  --argjson rare_term "$(num_or_null "${EXEC_TIMES_NO_SCORE[4]}")" \
  --argjson short_query_score "$(num_or_null "${EXEC_TIMES_WITH_SCORE[0]}")" \
  --argjson medium_query_score "$(num_or_null "${EXEC_TIMES_WITH_SCORE[1]}")" \
  --argjson long_query_score "$(num_or_null "${EXEC_TIMES_WITH_SCORE[2]}")" \
  --argjson common_term_score "$(num_or_null "${EXEC_TIMES_WITH_SCORE[3]}")" \
  --argjson rare_term_score "$(num_or_null "${EXEC_TIMES_WITH_SCORE[4]}")" \
  --argjson throughput_total "$(num_or_null "$THROUGHPUT_TOTAL_MS")" \
  --argjson throughput_avg "$(num_or_null "$THROUGHPUT_AVG_MS")" \
  --argjson throughput_score_total "$(num_or_null "$THROUGHPUT_SCORE_TOTAL_MS")" \
  --argjson throughput_score_avg "$(num_or_null "$THROUGHPUT_SCORE_AVG_MS")" \
  '{
    timestamp: $timestamp,
    commit: $commit,
    dataset: $dataset,
    metrics: {
      load_time_ms: $load_time,
      index_build_time_ms: $index_build,
      num_documents: $num_docs,
      index_size: $index_size,
      index_size_bytes: $index_size_bytes,
      table_size: $table_size,
      table_size_bytes: $table_size_bytes,
      query_latencies_ms: {
        short_query: $short_query,
        medium_query: $medium_query,
        long_query: $long_query,
        common_term: $common_term,
        rare_term: $rare_term
      },
      query_latencies_with_score_ms: {
        short_query: $short_query_score,
        medium_query: $medium_query_score,
        long_query: $long_query_score,
        common_term: $common_term_score,
        rare_term: $rare_term_score
      },
      throughput: {
        num_queries: 20,
        total_ms: $throughput_total,
        avg_ms_per_query: $throughput_avg
      },
      throughput_with_score: {
        num_queries: 20,
        total_ms: $throughput_score_total,
        avg_ms_per_query: $throughput_score_avg
      }
    }
  }' > "$OUTPUT_FILE"

echo "Metrics extracted to $OUTPUT_FILE"
cat "$OUTPUT_FILE"
