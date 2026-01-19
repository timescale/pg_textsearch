#!/bin/bash
# Run parallel build scaling benchmark on MS MARCO
#
# Usage:
#   ./run_parallel_scaling.sh [msmarco_size]
#
# Arguments:
#   msmarco_size: "full" (8.8M) or "sample" (100K). Default: full
#
# Outputs:
#   - Console output with benchmark results
#   - parallel_scaling_results.txt with raw output
#   - parallel_scaling_metrics.json with parsed metrics

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MSMARCO_SIZE="${1:-full}"
DATA_DIR="$SCRIPT_DIR/datasets/msmarco/data"
OUTPUT_FILE="parallel_scaling_results.txt"
METRICS_FILE="parallel_scaling_metrics.json"

echo "=== Parallel Build Scaling Benchmark ==="
echo "MS MARCO size: $MSMARCO_SIZE"
echo ""

# Check if data exists
if [ ! -f "$DATA_DIR/collection.tsv" ]; then
    echo "Downloading MS MARCO dataset..."
    cd "$SCRIPT_DIR/datasets/msmarco"
    ./download.sh "$MSMARCO_SIZE"
    cd "$SCRIPT_DIR"
fi

# Load data (without index)
echo ""
echo "Loading MS MARCO data..."
DATA_DIR="$DATA_DIR" psql -f "$SCRIPT_DIR/datasets/msmarco/load_data_only.sql" 2>&1 | tee load_output.txt

# Run parallel scaling benchmark
echo ""
echo "Running parallel scaling benchmark..."
psql -f "$SCRIPT_DIR/datasets/msmarco/parallel_scaling.sql" 2>&1 | tee "$OUTPUT_FILE"

# Extract metrics from output
echo ""
echo "Extracting metrics..."

# Parse build times from psql timing output
# Format: "Time: 12345.678 ms"
extract_build_time() {
    local workers=$1
    # Find the CREATE INDEX timing after the workers=$workers section
    awk -v w="$workers" '
        /Testing with '"$workers"' worker/ { found=1 }
        found && /CREATE INDEX/ { getline; if (/Time:/) { gsub(/[^0-9.]/, "", $2); print $2; exit } }
    ' "$OUTPUT_FILE"
}

# Parse query results
# Format: "PARALLEL_QUERY_RESULT: workers=N, avg_ms=X, min_ms=Y, max_ms=Z"
extract_query_metrics() {
    local workers=$1
    grep "PARALLEL_QUERY_RESULT: workers=$workers" "$OUTPUT_FILE" | \
        sed 's/.*avg_ms=\([0-9.]*\).*/\1/'
}

# Build JSON output
BUILD_0=$(extract_build_time 0)
BUILD_1=$(extract_build_time 1)
BUILD_2=$(extract_build_time 2)
BUILD_4=$(extract_build_time 4)

QUERY_0=$(extract_query_metrics 0)
QUERY_1=$(extract_query_metrics 1)
QUERY_2=$(extract_query_metrics 2)
QUERY_4=$(extract_query_metrics 4)

# Generate JSON metrics file
cat > "$METRICS_FILE" << EOF
{
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "dataset": "msmarco-$MSMARCO_SIZE",
  "parallel_scaling": {
    "workers_0": {
      "build_time_ms": ${BUILD_0:-null},
      "query_avg_ms": ${QUERY_0:-null}
    },
    "workers_1": {
      "build_time_ms": ${BUILD_1:-null},
      "query_avg_ms": ${QUERY_1:-null}
    },
    "workers_2": {
      "build_time_ms": ${BUILD_2:-null},
      "query_avg_ms": ${QUERY_2:-null}
    },
    "workers_4": {
      "build_time_ms": ${BUILD_4:-null},
      "query_avg_ms": ${QUERY_4:-null}
    }
  }
}
EOF

echo ""
echo "=== Parallel Scaling Results ==="
echo ""
printf "%-10s %15s %15s\n" "Workers" "Build Time (s)" "Query Avg (ms)"
printf "%-10s %15s %15s\n" "-------" "--------------" "--------------"
printf "%-10s %15.1f %15.2f\n" "0 (serial)" "${BUILD_0:-0}" "${QUERY_0:-0}"
printf "%-10s %15.1f %15.2f\n" "1" "${BUILD_1:-0}" "${QUERY_1:-0}"
printf "%-10s %15.1f %15.2f\n" "2" "${BUILD_2:-0}" "${QUERY_2:-0}"
printf "%-10s %15.1f %15.2f\n" "4" "${BUILD_4:-0}" "${QUERY_4:-0}"
echo ""

# Calculate speedups if we have valid build times
if [ -n "$BUILD_0" ] && [ -n "$BUILD_4" ]; then
    SPEEDUP=$(echo "scale=2; $BUILD_0 / $BUILD_4" | bc 2>/dev/null || echo "N/A")
    echo "Speedup (4 workers vs serial): ${SPEEDUP}x"
fi

echo ""
echo "Results written to:"
echo "  - $OUTPUT_FILE (raw output)"
echo "  - $METRICS_FILE (parsed metrics)"
