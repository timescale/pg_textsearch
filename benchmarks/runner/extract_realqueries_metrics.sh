#!/bin/bash
# Extract benchmark metrics from real-query benchmark output
#
# Usage: ./extract_realqueries_metrics.sh <log> <output_json> [dataset_name]
#
# Parses the new real-query benchmark output format which includes:
# - Full throughput (all queries)
# - Query length analysis
# - Latency percentiles (P50, P90, P95, P99)
# - Term selectivity analysis

set -e

LOG_FILE="${1:-benchmark_results.txt}"
OUTPUT_FILE="${2:-realqueries_metrics.json}"
DATASET_NAME="${3:-msmarco-realqueries}"

# Extract throughput metrics
# Format: THROUGHPUT_ALL: 6980 queries in 12345.67 ms
THROUGHPUT_LINE=$(grep -E "THROUGHPUT_ALL:" "$LOG_FILE" 2>/dev/null || echo "")
THROUGHPUT_QUERIES=""
THROUGHPUT_TOTAL_MS=""
if [ -n "$THROUGHPUT_LINE" ]; then
    THROUGHPUT_QUERIES=$(echo "$THROUGHPUT_LINE" | \
        grep -oE "[0-9]+ queries" | grep -oE "[0-9]+" || echo "")
    THROUGHPUT_TOTAL_MS=$(echo "$THROUGHPUT_LINE" | \
        grep -oE "in [0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
fi

# Format: THROUGHPUT_AVG: 1.234 ms/query
THROUGHPUT_AVG=$(grep -E "THROUGHPUT_AVG:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "")

# Format: THROUGHPUT_QPS: 123.45 queries/sec
THROUGHPUT_QPS=$(grep -E "THROUGHPUT_QPS:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "")

# Extract index build time (from CREATE INDEX timing)
INDEX_BUILD_MS=$(grep -E "CREATE INDEX" "$LOG_FILE" -A 1 2>/dev/null | \
    grep -oE "Time: [0-9]+\.[0-9]+ ms" | head -1 | \
    grep -oE "[0-9]+\.[0-9]+" || echo "")

# Extract document count
NUM_DOCUMENTS=$(grep -E "BM25 index build completed:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ documents" | grep -oE "[0-9]+" || echo "")

# Extract index and table sizes
INDEX_SIZE=$(grep -E "INDEX_SIZE:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ [kMGT]?B" | head -1 || echo "")
INDEX_SIZE_BYTES=$(grep -E "INDEX_SIZE:" "$LOG_FILE" 2>/dev/null | \
    awk '{print $NF}' | grep -E "^[0-9]+$" | head -1 || echo "")
TABLE_SIZE=$(grep -E "TABLE_SIZE:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ [kMGT]?B" | head -1 || echo "")
TABLE_SIZE_BYTES=$(grep -E "TABLE_SIZE:" "$LOG_FILE" 2>/dev/null | \
    awk '{print $NF}' | grep -E "^[0-9]+$" | head -1 || echo "")

# Extract query length performance
# The output table format:
#  Length   | Count |  Total Time  |  Avg Time
# ----------+-------+--------------+-----------
#  1-2 words|   234 | 1234.56 ms   | 5.277 ms
extract_length_metric() {
    local pattern="$1"
    grep -E "^\s*$pattern" "$LOG_FILE" 2>/dev/null | \
        awk -F'|' '{gsub(/[^0-9.]/, "", $4); print $4}' | head -1 || echo ""
}

LENGTH_1_2_AVG=$(extract_length_metric "1-2 words")
LENGTH_3_5_AVG=$(extract_length_metric "3-5 words")
LENGTH_6_8_AVG=$(extract_length_metric "6-8 words")
LENGTH_9_PLUS_AVG=$(extract_length_metric "9\\+ words")

# Extract latency percentiles from table output
# Format:
#  Min (ms) | P50 (ms) | P90 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Avg (ms)
# ----------+----------+----------+----------+----------+----------+---------
#     1.234 |    2.345 |    5.678 |    7.890 |   12.345 |   45.678 |   3.456
LATENCY_LINE=$(grep -E "^\s*[0-9]+\.[0-9]+\s*\|\s*[0-9]+\.[0-9]+\s*\|" \
    "$LOG_FILE" 2>/dev/null | head -1 || echo "")

if [ -n "$LATENCY_LINE" ]; then
    LATENCY_MIN=$(echo "$LATENCY_LINE" | awk -F'|' '{gsub(/[^0-9.]/, "", $1); print $1}')
    LATENCY_P50=$(echo "$LATENCY_LINE" | awk -F'|' '{gsub(/[^0-9.]/, "", $2); print $2}')
    LATENCY_P90=$(echo "$LATENCY_LINE" | awk -F'|' '{gsub(/[^0-9.]/, "", $3); print $3}')
    LATENCY_P95=$(echo "$LATENCY_LINE" | awk -F'|' '{gsub(/[^0-9.]/, "", $4); print $4}')
    LATENCY_P99=$(echo "$LATENCY_LINE" | awk -F'|' '{gsub(/[^0-9.]/, "", $5); print $5}')
    LATENCY_MAX=$(echo "$LATENCY_LINE" | awk -F'|' '{gsub(/[^0-9.]/, "", $6); print $6}')
    LATENCY_AVG=$(echo "$LATENCY_LINE" | awk -F'|' '{gsub(/[^0-9.]/, "", $7); print $7}')
fi

# Extract term selectivity metrics
# Format:
#    Selectivity     | Count |  Avg Time   | Avg Results
# -------------------+-------+-------------+------------
#  Low (common terms)|    15 | 12.345 ms   |      45678
#  High (rare terms) |    42 |  3.456 ms   |        234
extract_selectivity_metric() {
    local pattern="$1"
    grep -E "^\s*$pattern" "$LOG_FILE" 2>/dev/null | \
        awk -F'|' '{gsub(/[^0-9.]/, "", $3); print $3}' | head -1 || echo ""
}

SELECTIVITY_HIGH_AVG=$(extract_selectivity_metric "High")
SELECTIVITY_LOW_AVG=$(extract_selectivity_metric "Low")

# Helper function
num_or_null() {
    if [ -n "$1" ] && [[ "$1" =~ ^[0-9]+\.?[0-9]*$ ]]; then
        echo "$1"
    else
        echo "null"
    fi
}

# Build JSON output
jq -n \
  --arg timestamp "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  --arg commit "${GITHUB_SHA:-unknown}" \
  --arg dataset "$DATASET_NAME" \
  --argjson index_build "$(num_or_null "$INDEX_BUILD_MS")" \
  --argjson num_docs "$(num_or_null "$NUM_DOCUMENTS")" \
  --arg index_size "${INDEX_SIZE:-unknown}" \
  --argjson index_size_bytes "$(num_or_null "$INDEX_SIZE_BYTES")" \
  --arg table_size "${TABLE_SIZE:-unknown}" \
  --argjson table_size_bytes "$(num_or_null "$TABLE_SIZE_BYTES")" \
  --argjson throughput_queries "$(num_or_null "$THROUGHPUT_QUERIES")" \
  --argjson throughput_total "$(num_or_null "$THROUGHPUT_TOTAL_MS")" \
  --argjson throughput_avg "$(num_or_null "$THROUGHPUT_AVG")" \
  --argjson throughput_qps "$(num_or_null "$THROUGHPUT_QPS")" \
  --argjson length_1_2 "$(num_or_null "$LENGTH_1_2_AVG")" \
  --argjson length_3_5 "$(num_or_null "$LENGTH_3_5_AVG")" \
  --argjson length_6_8 "$(num_or_null "$LENGTH_6_8_AVG")" \
  --argjson length_9_plus "$(num_or_null "$LENGTH_9_PLUS_AVG")" \
  --argjson latency_min "$(num_or_null "$LATENCY_MIN")" \
  --argjson latency_p50 "$(num_or_null "$LATENCY_P50")" \
  --argjson latency_p90 "$(num_or_null "$LATENCY_P90")" \
  --argjson latency_p95 "$(num_or_null "$LATENCY_P95")" \
  --argjson latency_p99 "$(num_or_null "$LATENCY_P99")" \
  --argjson latency_max "$(num_or_null "$LATENCY_MAX")" \
  --argjson latency_avg "$(num_or_null "$LATENCY_AVG")" \
  --argjson selectivity_high "$(num_or_null "$SELECTIVITY_HIGH_AVG")" \
  --argjson selectivity_low "$(num_or_null "$SELECTIVITY_LOW_AVG")" \
  '{
    timestamp: $timestamp,
    commit: $commit,
    dataset: $dataset,
    metrics: {
      index_build_time_ms: $index_build,
      num_documents: $num_docs,
      index_size: $index_size,
      index_size_bytes: $index_size_bytes,
      table_size: $table_size,
      table_size_bytes: $table_size_bytes,
      throughput: {
        num_queries: $throughput_queries,
        total_ms: $throughput_total,
        avg_ms_per_query: $throughput_avg,
        queries_per_sec: $throughput_qps
      },
      query_length_avg_ms: {
        "1-2_words": $length_1_2,
        "3-5_words": $length_3_5,
        "6-8_words": $length_6_8,
        "9+_words": $length_9_plus
      },
      latency_percentiles_ms: {
        min: $latency_min,
        p50: $latency_p50,
        p90: $latency_p90,
        p95: $latency_p95,
        p99: $latency_p99,
        max: $latency_max,
        avg: $latency_avg
      },
      term_selectivity_avg_ms: {
        high_selectivity: $selectivity_high,
        low_selectivity: $selectivity_low
      }
    }
  }' > "$OUTPUT_FILE"

echo "Real-query metrics extracted to $OUTPUT_FILE"
cat "$OUTPUT_FILE"
