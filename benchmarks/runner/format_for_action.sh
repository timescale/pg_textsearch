#!/bin/bash
# Convert benchmark_metrics.json to github-action-benchmark format
#
# Usage: ./format_for_action.sh <input_json> <output_json>
#
# Input format (our benchmark_metrics.json):
#   { "metrics": { "index_build_time_ms": 547000, ... } }
#
# Output format (github-action-benchmark customSmallerIsBetter):
#   [ {"name": "...", "unit": "ms", "value": 547000}, ... ]

set -e

INPUT_FILE="${1:-benchmark_metrics.json}"
OUTPUT_FILE="${2:-benchmark_action.json}"

if [ ! -f "$INPUT_FILE" ]; then
    echo "Error: Input file $INPUT_FILE not found"
    exit 1
fi

# Extract dataset name and document count from input
DATASET=$(jq -r '.dataset // "Benchmark"' "$INPUT_FILE")
NUM_DOCS=$(jq -r '.metrics.num_documents // empty' "$INPUT_FILE")

# Format document count (e.g., 1400 -> "1.4K", 8841823 -> "8.8M")
format_docs() {
    local n=$1
    if [ -z "$n" ] || [ "$n" = "null" ]; then
        echo ""
        return
    fi
    if [ "$n" -ge 1000000 ]; then
        printf "%.1fM" "$(echo "scale=1; $n / 1000000" | bc)"
    elif [ "$n" -ge 1000 ]; then
        printf "%.1fK" "$(echo "scale=1; $n / 1000" | bc)"
    else
        echo "$n"
    fi
}

DOCS_LABEL=$(format_docs "$NUM_DOCS")
if [ -n "$DOCS_LABEL" ]; then
    DATASET_LABEL="$DATASET ($DOCS_LABEL docs)"
else
    DATASET_LABEL="$DATASET"
fi

# Build the output array using jq
jq --arg dataset "$DATASET_LABEL" '[
    # Index build time
    (if .metrics.index_build_time_ms != null then
        {
            name: "\($dataset) - Index Build Time",
            unit: "ms",
            value: .metrics.index_build_time_ms
        }
    else empty end),

    # Query latencies
    (if .metrics.query_latencies_ms.short_query != null then
        {
            name: "\($dataset) - Short Query (1 word)",
            unit: "ms",
            value: .metrics.query_latencies_ms.short_query
        }
    else empty end),

    (if .metrics.query_latencies_ms.medium_query != null then
        {
            name: "\($dataset) - Medium Query (3 words)",
            unit: "ms",
            value: .metrics.query_latencies_ms.medium_query
        }
    else empty end),

    (if .metrics.query_latencies_ms.long_query != null then
        {
            name: "\($dataset) - Long Query (question)",
            unit: "ms",
            value: .metrics.query_latencies_ms.long_query
        }
    else empty end),

    (if .metrics.query_latencies_ms.common_term != null then
        {
            name: "\($dataset) - Common Term Query",
            unit: "ms",
            value: .metrics.query_latencies_ms.common_term
        }
    else empty end),

    (if .metrics.query_latencies_ms.rare_term != null then
        {
            name: "\($dataset) - Rare Term Query",
            unit: "ms",
            value: .metrics.query_latencies_ms.rare_term
        }
    else empty end),

    # Throughput (average latency)
    (if .metrics.throughput.avg_ms_per_query != null then
        {
            name: "\($dataset) - Avg Query Latency (20 queries)",
            unit: "ms",
            value: .metrics.throughput.avg_ms_per_query
        }
    else empty end),

    # Index size (in MB for readability)
    (if .metrics.index_size_bytes != null then
        {
            name: "\($dataset) - Index Size",
            unit: "MB",
            value: ((.metrics.index_size_bytes / 1048576 * 100 | floor) / 100)
        }
    else empty end)
]' "$INPUT_FILE" > "$OUTPUT_FILE"

echo "Converted $INPUT_FILE to github-action-benchmark format:"
cat "$OUTPUT_FILE"
