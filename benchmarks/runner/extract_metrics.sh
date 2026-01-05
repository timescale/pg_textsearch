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
# - Query latencies by token bucket (p50/p95/p99)
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

# Helper to output number or null
num_or_null() {
    if [ -n "$1" ] && [[ "$1" =~ ^[0-9]+\.?[0-9]*$ ]]; then
        echo "$1"
    else
        echo "null"
    fi
}

# Extract index build time (from CREATE INDEX timing)
INDEX_BUILD_MS=$(grep -E "CREATE INDEX" "$LOG_FILE" -A 1 2>/dev/null | \
    grep -oE "Time: [0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")

# Extract data load time (COPY statements)
LOAD_TIME_MS=$(grep -E "^COPY [0-9]+" "$LOG_FILE" -A 1 2>/dev/null | \
    grep -oE "Time: [0-9]+\.[0-9]+ ms" | head -1 | grep -oE "[0-9]+\.[0-9]+" || echo "")

# Extract document count
NUM_DOCUMENTS=$(grep -E "BM25 index build completed:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ documents" | grep -oE "[0-9]+" || echo "")
if [ -z "$NUM_DOCUMENTS" ]; then
    NUM_DOCUMENTS=$(grep -E "Passages loaded:" "$LOG_FILE" 2>/dev/null | \
        grep -oE "\| [0-9]+" | grep -oE "[0-9]+" || echo "")
fi

# Extract index and table sizes
INDEX_SIZE=$(grep -E "INDEX_SIZE:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ [kMGT]?B" | head -1 || echo "")
INDEX_SIZE_BYTES=$(grep -E "INDEX_SIZE:" "$LOG_FILE" 2>/dev/null | \
    awk '{print $NF}' | grep -E "^[0-9]+$" | head -1 || echo "")
TABLE_SIZE=$(grep -E "TABLE_SIZE:" "$LOG_FILE" 2>/dev/null | \
    grep -oE "[0-9]+ [kMGT]?B" | head -1 || echo "")
TABLE_SIZE_BYTES=$(grep -E "TABLE_SIZE:" "$LOG_FILE" 2>/dev/null | \
    awk '{print $NF}' | grep -E "^[0-9]+$" | head -1 || echo "")

# Extract latency buckets (new format)
# Format: LATENCY_BUCKET_N: p50=Xms p95=Yms p99=Zms avg=Wms (n=100)
extract_bucket() {
    local bucket=$1
    local line=$(grep -E "LATENCY_BUCKET_${bucket}:" "$LOG_FILE" 2>/dev/null || echo "")
    if [ -n "$line" ]; then
        local p50=$(echo "$line" | grep -oE "p50=[0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
        local p95=$(echo "$line" | grep -oE "p95=[0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
        local p99=$(echo "$line" | grep -oE "p99=[0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
        local avg=$(echo "$line" | grep -oE "avg=[0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
        echo "{\"p50\": $(num_or_null "$p50"), \"p95\": $(num_or_null "$p95"), \"p99\": $(num_or_null "$p99"), \"avg\": $(num_or_null "$avg")}"
    else
        echo "null"
    fi
}

BUCKET_1=$(extract_bucket 1)
BUCKET_2=$(extract_bucket 2)
BUCKET_3=$(extract_bucket 3)
BUCKET_4=$(extract_bucket 4)
BUCKET_5=$(extract_bucket 5)
BUCKET_6=$(extract_bucket 6)
BUCKET_7=$(extract_bucket 7)
BUCKET_8=$(extract_bucket 8)

# Extract throughput result
# Format: "THROUGHPUT_RESULT: N queries in XXXX.XX ms (avg YY.YY ms/query)"
THROUGHPUT_LINE=$(grep -E "THROUGHPUT_RESULT:" "$LOG_FILE" 2>/dev/null | head -1 || echo "")
THROUGHPUT_TOTAL_MS=""
THROUGHPUT_AVG_MS=""
THROUGHPUT_NUM_QUERIES=""
if [ -n "$THROUGHPUT_LINE" ]; then
    THROUGHPUT_NUM_QUERIES=$(echo "$THROUGHPUT_LINE" | grep -oE "[0-9]+ queries" | grep -oE "[0-9]+" || echo "")
    THROUGHPUT_TOTAL_MS=$(echo "$THROUGHPUT_LINE" | grep -oE "in [0-9]+\.[0-9]+ ms" | grep -oE "[0-9]+\.[0-9]+" || echo "")
    THROUGHPUT_AVG_MS=$(echo "$THROUGHPUT_LINE" | grep -oE "avg [0-9]+\.[0-9]+" | grep -oE "[0-9]+\.[0-9]+" || echo "")
fi

# Build JSON output
cat > "$OUTPUT_FILE" << EOJSON
{
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "commit": "${GITHUB_SHA:-unknown}",
  "dataset": "$DATASET_NAME",
  "metrics": {
    "load_time_ms": $(num_or_null "$LOAD_TIME_MS"),
    "index_build_time_ms": $(num_or_null "$INDEX_BUILD_MS"),
    "num_documents": $(num_or_null "$NUM_DOCUMENTS"),
    "index_size": "${INDEX_SIZE:-unknown}",
    "index_size_bytes": $(num_or_null "$INDEX_SIZE_BYTES"),
    "table_size": "${TABLE_SIZE:-unknown}",
    "table_size_bytes": $(num_or_null "$TABLE_SIZE_BYTES"),
    "latency_by_tokens": {
      "bucket_1": $BUCKET_1,
      "bucket_2": $BUCKET_2,
      "bucket_3": $BUCKET_3,
      "bucket_4": $BUCKET_4,
      "bucket_5": $BUCKET_5,
      "bucket_6": $BUCKET_6,
      "bucket_7": $BUCKET_7,
      "bucket_8": $BUCKET_8
    },
    "throughput": {
      "num_queries": $(num_or_null "$THROUGHPUT_NUM_QUERIES"),
      "total_ms": $(num_or_null "$THROUGHPUT_TOTAL_MS"),
      "avg_ms_per_query": $(num_or_null "$THROUGHPUT_AVG_MS")
    }
  }
}
EOJSON

echo "Metrics extracted to $OUTPUT_FILE"
cat "$OUTPUT_FILE"
