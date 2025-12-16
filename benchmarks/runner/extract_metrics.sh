#!/bin/bash
# Extract benchmark metrics from log output and create JSON summary
#
# Usage: ./extract_metrics.sh <benchmark_log> <output_json> [dataset_name]
#
# Parses benchmark output to extract:
# - Index build time
# - Query latencies (from EXPLAIN ANALYZE)
# - Throughput metrics
# - Index/table sizes

set -e

LOG_FILE="${1:-benchmark_results.txt}"
OUTPUT_FILE="${2:-benchmark_metrics.json}"
DATASET_NAME="${3:-unknown}"

# Initialize metrics
INDEX_BUILD_MS=""
LOAD_TIME_MS=""
NUM_DOCUMENTS=""
INDEX_SIZE=""
TABLE_SIZE=""
declare -a QUERY_LATENCIES

# Extract index build time (from CREATE INDEX timing - format: "Time: 63073.468 ms")
INDEX_BUILD_MS=$(grep -E "CREATE INDEX" "$LOG_FILE" -A 1 2>/dev/null | \
    grep -oE "Time: [0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")

# Extract data load time (COPY statements - format: "Time: 11235.512 ms")
LOAD_TIME_MS=$(grep -E "^COPY [0-9]+" "$LOG_FILE" -A 1 2>/dev/null | \
    grep -oE "Time: [0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")

# Extract document count from index build notice
NUM_DOCUMENTS=$(grep -E "BM25 index build completed:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ documents" | grep -oE "[0-9]+" || echo "")

# Extract Execution Time from EXPLAIN ANALYZE outputs
# Format: "Execution Time: 123.456 ms"
mapfile -t EXEC_TIMES < <(grep -E "Execution Time:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+\.[0-9]+" || true)

# Extract throughput result
THROUGHPUT_LINE=$(grep -E "THROUGHPUT_RESULT:" "$LOG_FILE" 2>/dev/null || echo "")
THROUGHPUT_TOTAL_MS=""
THROUGHPUT_AVG_MS=""
if [ -n "$THROUGHPUT_LINE" ]; then
    THROUGHPUT_TOTAL_MS=$(echo "$THROUGHPUT_LINE" | \
        grep -oE "[0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")
    THROUGHPUT_AVG_MS=$(echo "$THROUGHPUT_LINE" | \
        grep -oE "avg [0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
fi

# Extract index and table sizes
INDEX_SIZE=$(grep -E "msmarco_bm25_idx.*[0-9]+ [MG]B" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ [MG]B" | head -1 || echo "")
TABLE_SIZE=$(grep -E "table_size.*[0-9]+ [MG]B" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ [MG]B" | head -1 || echo "")

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
  --arg table_size "${TABLE_SIZE:-unknown}" \
  --argjson short_query "$(num_or_null "${EXEC_TIMES[0]}")" \
  --argjson medium_query "$(num_or_null "${EXEC_TIMES[1]}")" \
  --argjson long_query "$(num_or_null "${EXEC_TIMES[2]}")" \
  --argjson common_term "$(num_or_null "${EXEC_TIMES[3]}")" \
  --argjson rare_term "$(num_or_null "${EXEC_TIMES[4]}")" \
  --argjson throughput_total "$(num_or_null "$THROUGHPUT_TOTAL_MS")" \
  --argjson throughput_avg "$(num_or_null "$THROUGHPUT_AVG_MS")" \
  '{
    timestamp: $timestamp,
    commit: $commit,
    dataset: $dataset,
    metrics: {
      load_time_ms: $load_time,
      index_build_time_ms: $index_build,
      num_documents: $num_docs,
      index_size: $index_size,
      table_size: $table_size,
      query_latencies_ms: {
        short_query: $short_query,
        medium_query: $medium_query,
        long_query: $long_query,
        common_term: $common_term,
        rare_term: $rare_term
      },
      throughput: {
        num_queries: 20,
        total_ms: $throughput_total,
        avg_ms_per_query: $throughput_avg
      }
    }
  }' > "$OUTPUT_FILE"

echo "Metrics extracted to $OUTPUT_FILE"
cat "$OUTPUT_FILE"
