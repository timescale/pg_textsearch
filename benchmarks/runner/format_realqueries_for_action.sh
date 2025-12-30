#!/bin/bash
# Convert real-query metrics JSON to github-action-benchmark format
#
# Usage: ./format_realqueries_for_action.sh <input_json> <output_json>
#
# Input: realqueries_metrics.json (from extract_realqueries_metrics.sh)
# Output: Array of {name, unit, value} for github-action-benchmark

set -e

INPUT_FILE="${1:-realqueries_metrics.json}"
OUTPUT_FILE="${2:-realqueries_action.json}"

if [ ! -f "$INPUT_FILE" ]; then
    echo "Error: Input file $INPUT_FILE not found"
    exit 1
fi

# Extract dataset name and document count
DATASET=$(jq -r '.dataset // "MS MARCO Real Queries"' "$INPUT_FILE")
NUM_DOCS=$(jq -r '.metrics.num_documents // empty' "$INPUT_FILE")
NUM_QUERIES=$(jq -r '.metrics.throughput.num_queries // empty' "$INPUT_FILE")

# Format counts for labels
format_count() {
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

DOCS_LABEL=$(format_count "$NUM_DOCS")
QUERIES_LABEL=$(format_count "$NUM_QUERIES")
if [ -n "$DOCS_LABEL" ]; then
    DATASET_LABEL="$DATASET ($DOCS_LABEL docs, $QUERIES_LABEL queries)"
else
    DATASET_LABEL="$DATASET"
fi

# Build output array
jq --arg dataset "$DATASET_LABEL" '[
    # Index build time
    (if .metrics.index_build_time_ms != null then
        {
            name: "\($dataset) - Index Build Time",
            unit: "ms",
            value: .metrics.index_build_time_ms
        }
    else empty end),

    # Throughput metrics
    (if .metrics.throughput.avg_ms_per_query != null then
        {
            name: "\($dataset) - Avg Query Latency (all queries)",
            unit: "ms",
            value: .metrics.throughput.avg_ms_per_query
        }
    else empty end),

    (if .metrics.throughput.queries_per_sec != null then
        {
            name: "\($dataset) - Queries Per Second",
            unit: "QPS",
            value: .metrics.throughput.queries_per_sec
        }
    else empty end),

    # Query length metrics
    (if .metrics.query_length_avg_ms["1-2_words"] != null then
        {
            name: "\($dataset) - Short Query (1-2 words)",
            unit: "ms",
            value: .metrics.query_length_avg_ms["1-2_words"]
        }
    else empty end),

    (if .metrics.query_length_avg_ms["3-5_words"] != null then
        {
            name: "\($dataset) - Medium Query (3-5 words)",
            unit: "ms",
            value: .metrics.query_length_avg_ms["3-5_words"]
        }
    else empty end),

    (if .metrics.query_length_avg_ms["6-8_words"] != null then
        {
            name: "\($dataset) - Long Query (6-8 words)",
            unit: "ms",
            value: .metrics.query_length_avg_ms["6-8_words"]
        }
    else empty end),

    (if .metrics.query_length_avg_ms["9+_words"] != null then
        {
            name: "\($dataset) - Very Long Query (9+ words)",
            unit: "ms",
            value: .metrics.query_length_avg_ms["9+_words"]
        }
    else empty end),

    # Latency percentiles
    (if .metrics.latency_percentiles_ms.p50 != null then
        {
            name: "\($dataset) - P50 Latency",
            unit: "ms",
            value: .metrics.latency_percentiles_ms.p50
        }
    else empty end),

    (if .metrics.latency_percentiles_ms.p95 != null then
        {
            name: "\($dataset) - P95 Latency",
            unit: "ms",
            value: .metrics.latency_percentiles_ms.p95
        }
    else empty end),

    (if .metrics.latency_percentiles_ms.p99 != null then
        {
            name: "\($dataset) - P99 Latency",
            unit: "ms",
            value: .metrics.latency_percentiles_ms.p99
        }
    else empty end),

    # Term selectivity
    (if .metrics.term_selectivity_avg_ms.high_selectivity != null then
        {
            name: "\($dataset) - High Selectivity (rare terms)",
            unit: "ms",
            value: .metrics.term_selectivity_avg_ms.high_selectivity
        }
    else empty end),

    (if .metrics.term_selectivity_avg_ms.low_selectivity != null then
        {
            name: "\($dataset) - Low Selectivity (common terms)",
            unit: "ms",
            value: .metrics.term_selectivity_avg_ms.low_selectivity
        }
    else empty end),

    # Index size
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
