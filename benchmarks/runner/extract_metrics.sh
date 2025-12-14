#!/bin/bash
# Extract benchmark metrics from log output and create JSON summary
#
# Usage: ./extract_metrics.sh <benchmark_log> <output_json>
#
# Parses benchmark output to extract:
# - Index build time
# - Query latencies (from EXPLAIN ANALYZE)
# - Throughput metrics
# - Index/table sizes

set -e

LOG_FILE="${1:-benchmark_results.txt}"
OUTPUT_FILE="${2:-benchmark_metrics.json}"

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

# Build JSON output
cat > "$OUTPUT_FILE" << EOF
{
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "commit": "${GITHUB_SHA:-unknown}",
  "dataset": "msmarco",
  "metrics": {
    "load_time_ms": ${LOAD_TIME_MS:-null},
    "index_build_time_ms": ${INDEX_BUILD_MS:-null},
    "num_documents": ${NUM_DOCUMENTS:-null},
    "index_size": "${INDEX_SIZE:-unknown}",
    "table_size": "${TABLE_SIZE:-unknown}",
    "query_latencies_ms": {
      "short_query": ${EXEC_TIMES[0]:-null},
      "medium_query": ${EXEC_TIMES[1]:-null},
      "long_query": ${EXEC_TIMES[2]:-null},
      "common_term": ${EXEC_TIMES[3]:-null},
      "rare_term": ${EXEC_TIMES[4]:-null}
    },
    "throughput": {
      "num_queries": 20,
      "total_ms": ${THROUGHPUT_TOTAL_MS:-null},
      "avg_ms_per_query": ${THROUGHPUT_AVG_MS:-null}
    }
  }
}
EOF

echo "Metrics extracted to $OUTPUT_FILE"
cat "$OUTPUT_FILE"
