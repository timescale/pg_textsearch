#!/bin/bash
# pg_textsearch Benchmark Runner
# Runs benchmarks and generates a markdown report
#
# Usage:
#   ./run_benchmark.sh [dataset] [options]
#
# Datasets:
#   msmarco     - MS MARCO Passage Ranking (8.8M passages)
#   wikipedia   - Wikipedia articles
#   cranfield   - Cranfield collection (1,400 docs) - for quick validation
#   all         - Run all benchmarks
#
# Options:
#   --download     - Download dataset if not present
#   --load         - Load data and create index (drops existing)
#   --query        - Run query benchmarks only
#   --skip-validate - Skip validation (not recommended)
#   --report       - Generate markdown report
#   --port PORT    - Postgres port (default: 5433 for release build)
#
# Validation:
#   Validation runs automatically after queries if ground_truth.tsv exists.
#   A benchmark FAILS if score match rate is < 100% (within 5% tolerance).
#   Generate ground truth with: psql -f <dataset>/precompute_ground_truth.sql

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCHMARK_DIR="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="$BENCHMARK_DIR/results"

# Defaults - use release build for benchmarks
PGPORT="${PGPORT:-5433}"
PGHOST="${PGHOST:-localhost}"
PGUSER="${PGUSER:-$(whoami)}"
PGDATABASE="${PGDATABASE:-postgres}"

DATASET=""
DO_DOWNLOAD=false
DO_LOAD=false
DO_QUERY=false
SKIP_VALIDATE=false
DO_REPORT=false
VALIDATION_FAILED=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        msmarco|wikipedia|cranfield|all)
            DATASET="$1"
            shift
            ;;
        --download)
            DO_DOWNLOAD=true
            shift
            ;;
        --load)
            DO_LOAD=true
            shift
            ;;
        --query)
            DO_QUERY=true
            shift
            ;;
        --skip-validate)
            SKIP_VALIDATE=true
            shift
            ;;
        --report)
            DO_REPORT=true
            shift
            ;;
        --port)
            PGPORT="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [dataset] [options]"
            echo ""
            echo "Datasets:"
            echo "  msmarco     - MS MARCO Passage Ranking (8.8M passages)"
            echo "  wikipedia   - Wikipedia articles"
            echo "  cranfield   - Cranfield collection (1,400 docs)"
            echo "  all         - Run all benchmarks"
            echo ""
            echo "Options:"
            echo "  --download       - Download dataset if not present"
            echo "  --load           - Load data and create index"
            echo "  --query          - Run query benchmarks"
            echo "  --skip-validate  - Skip validation (not recommended)"
            echo "  --report         - Generate markdown report"
            echo "  --port PORT      - Postgres port (default: 5433)"
            echo ""
            echo "Validation runs automatically after queries. Fails if score match < 100%."
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Default: run everything if no specific action requested
if ! $DO_DOWNLOAD && ! $DO_LOAD && ! $DO_QUERY && ! $DO_REPORT; then
    DO_DOWNLOAD=true
    DO_LOAD=true
    DO_QUERY=true
    DO_REPORT=true
fi

if [ -z "$DATASET" ]; then
    echo "Error: No dataset specified"
    echo "Usage: $0 [msmarco|wikipedia|cranfield|all] [options]"
    exit 1
fi

export PGPORT PGHOST PGUSER PGDATABASE

mkdir -p "$RESULTS_DIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
REPORT_FILE="$RESULTS_DIR/benchmark_${DATASET}_${TIMESTAMP}.md"

echo "=== pg_textsearch Benchmark Runner ==="
echo "Dataset: $DATASET"
echo "Postgres: $PGHOST:$PGPORT/$PGDATABASE"
echo "Timestamp: $TIMESTAMP"
echo ""

# Function to run a single dataset benchmark
run_benchmark() {
    local dataset="$1"
    local dataset_dir="$BENCHMARK_DIR/datasets/$dataset"
    local data_dir="$dataset_dir/data"

    echo ""
    echo "=========================================="
    echo "Running benchmark: $dataset"
    echo "=========================================="

    # Download
    if $DO_DOWNLOAD; then
        if [ -f "$dataset_dir/download.sh" ]; then
            echo ""
            echo "--- Downloading $dataset dataset ---"
            chmod +x "$dataset_dir/download.sh"
            (cd "$dataset_dir" && ./download.sh)
        fi
    fi

    # Load
    if $DO_LOAD; then
        if [ -f "$dataset_dir/load.sql" ]; then
            echo ""
            echo "--- Loading $dataset dataset ---"
            local load_log="$RESULTS_DIR/${dataset}_load_${TIMESTAMP}.log"

            # Capture timing
            local start_time=$(date +%s.%N)

            # Export DATA_DIR for load.sql scripts that use it
            export DATA_DIR="$data_dir"
            psql -f "$dataset_dir/load.sql" 2>&1 | tee "$load_log"

            local end_time=$(date +%s.%N)
            local load_time=$(echo "$end_time - $start_time" | bc)

            echo ""
            echo "Load time: ${load_time}s"
            echo "LOAD_TIME_${dataset}=${load_time}" >> "$RESULTS_DIR/metrics_${TIMESTAMP}.env"
        fi
    fi

    # Query benchmarks
    if $DO_QUERY; then
        if [ -f "$dataset_dir/queries.sql" ]; then
            echo ""
            echo "--- Running $dataset query benchmarks ---"
            local query_log="$RESULTS_DIR/${dataset}_queries_${TIMESTAMP}.log"

            local start_time=$(date +%s.%N)

            psql -f "$dataset_dir/queries.sql" 2>&1 | tee "$query_log"

            local end_time=$(date +%s.%N)
            local query_time=$(echo "$end_time - $start_time" | bc)

            echo ""
            echo "Query benchmark time: ${query_time}s"
            echo "QUERY_TIME_${dataset}=${query_time}" >> "$RESULTS_DIR/metrics_${TIMESTAMP}.env"
        fi
    fi

    # Validation (runs automatically after queries unless --skip-validate)
    if $DO_QUERY && ! $SKIP_VALIDATE; then
        local ground_truth_file="$dataset_dir/ground_truth.tsv"
        local validate_script="$dataset_dir/validate_queries.sql"

        if [ -f "$validate_script" ]; then
            if [ -f "$ground_truth_file" ]; then
                echo ""
                echo "--- Validating $dataset query results ---"
                local validate_log="$RESULTS_DIR/${dataset}_validation_${TIMESTAMP}.log"

                psql -f "$validate_script" 2>&1 | tee "$validate_log"

                # Check for validation failures (score match rate must be 100%)
                if grep -q "VALIDATION FAILED" "$validate_log"; then
                    echo ""
                    echo "ERROR: Validation FAILED for $dataset!"
                    echo "Score match rate < 100% indicates BM25 scoring bugs."
                    echo "VALIDATION_${dataset}=FAILED" >> "$RESULTS_DIR/metrics_${TIMESTAMP}.env"
                    VALIDATION_FAILED=true
                else
                    echo ""
                    echo "Validation PASSED for $dataset"
                    echo "VALIDATION_${dataset}=PASSED" >> "$RESULTS_DIR/metrics_${TIMESTAMP}.env"
                fi
            else
                echo ""
                echo "--- Skipping validation: ground_truth.tsv not found ---"
                echo "Generate ground truth with: psql -f $dataset_dir/precompute_ground_truth.sql"
                echo "VALIDATION_${dataset}=SKIPPED" >> "$RESULTS_DIR/metrics_${TIMESTAMP}.env"
            fi
        else
            echo ""
            echo "--- Skipping validation: validate_queries.sql not found ---"
            echo "VALIDATION_${dataset}=SKIPPED" >> "$RESULTS_DIR/metrics_${TIMESTAMP}.env"
        fi
    fi
}

# Function to generate report
generate_report() {
    echo ""
    echo "--- Generating Report ---"

    cat > "$REPORT_FILE" << EOF
# pg_textsearch Benchmark Report

**Date:** $(date -u +"%Y-%m-%d %H:%M:%S UTC")
**Dataset:** $DATASET
**Postgres:** $PGHOST:$PGPORT

## System Information

- **OS:** $(uname -s) $(uname -r)
- **CPU:** $(sysctl -n machdep.cpu.brand_string 2>/dev/null || grep "model name" /proc/cpuinfo 2>/dev/null | head -1 | cut -d: -f2 || echo "Unknown")
- **Memory:** $(sysctl -n hw.memsize 2>/dev/null | awk '{print $1/1024/1024/1024 " GB"}' || free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo "Unknown")

## Results Summary

EOF

    # Add metrics from env file if it exists
    local metrics_file="$RESULTS_DIR/metrics_${TIMESTAMP}.env"
    if [ -f "$metrics_file" ]; then
        echo "### Timing Results" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
        echo "| Dataset | Load Time | Query Time | Validation |" >> "$REPORT_FILE"
        echo "|---------|-----------|------------|------------|" >> "$REPORT_FILE"

        source "$metrics_file"

        for ds in msmarco wikipedia cranfield; do
            load_var="LOAD_TIME_${ds}"
            query_var="QUERY_TIME_${ds}"
            validation_var="VALIDATION_${ds}"
            if [ -n "${!load_var}" ] || [ -n "${!query_var}" ] || [ -n "${!validation_var}" ]; then
                echo "| $ds | ${!load_var:-N/A}s | ${!query_var:-N/A}s | ${!validation_var:-N/A} |" >> "$REPORT_FILE"
            fi
        done

        echo "" >> "$REPORT_FILE"
    fi

    # Add log excerpts
    echo "## Detailed Logs" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    for log_file in "$RESULTS_DIR"/*_${TIMESTAMP}.log; do
        if [ -f "$log_file" ]; then
            local log_name=$(basename "$log_file" .log)
            echo "### $log_name" >> "$REPORT_FILE"
            echo "" >> "$REPORT_FILE"
            echo '```' >> "$REPORT_FILE"
            # Include last 100 lines of each log
            tail -100 "$log_file" >> "$REPORT_FILE"
            echo '```' >> "$REPORT_FILE"
            echo "" >> "$REPORT_FILE"
        fi
    done

    echo ""
    echo "Report generated: $REPORT_FILE"
}

# Run benchmarks
if [ "$DATASET" = "all" ]; then
    for ds in cranfield msmarco wikipedia; do
        run_benchmark "$ds"
    done
else
    run_benchmark "$DATASET"
fi

# Generate report
if $DO_REPORT; then
    generate_report
fi

echo ""
echo "=== Benchmark Complete ==="
echo "Results in: $RESULTS_DIR"

# Exit with failure if any validation failed
if $VALIDATION_FAILED; then
    echo ""
    echo "ERROR: One or more validations FAILED!"
    echo "This indicates BM25 scoring bugs that need investigation."
    exit 1
fi
