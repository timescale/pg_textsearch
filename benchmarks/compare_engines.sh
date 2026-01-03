#!/bin/bash
# Competitive Engine Benchmark Runner
#
# Compares pg_textsearch against ParadeDB, Tantivy, and OpenSearch (Lucene)
# on the MS-MARCO passage retrieval dataset.
#
# Usage:
#   ./compare_engines.sh [options]
#
# Options:
#   --data-dir DIR      MS-MARCO data directory (default: ./datasets/msmarco)
#   --output-dir DIR    Output directory for results (default: ./results)
#   --engines LIST      Comma-separated engines to run (default: all)
#                       Options: pg_textsearch,paradedb,tantivy,opensearch
#   --skip-index        Skip index building (use existing indexes)
#   --help              Show this help message

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="${SCRIPT_DIR}/datasets/msmarco"
OUTPUT_DIR="${SCRIPT_DIR}/results/competitive"
ENGINES="pg_textsearch,paradedb,tantivy,opensearch"
SKIP_INDEX=""
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

usage() {
    head -17 "$0" | tail -14
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --data-dir) DATA_DIR="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --engines) ENGINES="$2"; shift 2 ;;
        --skip-index) SKIP_INDEX="--skip-index"; shift ;;
        --help) usage ;;
        *) log_error "Unknown option: $1"; usage ;;
    esac
done

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "============================================================"
echo "Competitive Engine Benchmark"
echo "============================================================"
echo "Timestamp: $TIMESTAMP"
echo "Data directory: $DATA_DIR"
echo "Output directory: $OUTPUT_DIR"
echo "Engines: $ENGINES"
echo ""

# Check prerequisites
check_prerequisites() {
    log_info "Checking prerequisites..."

    local missing=()

    # Check Python
    if ! command -v python3 &> /dev/null; then
        missing+=("python3")
    fi

    # Check psql
    if ! command -v psql &> /dev/null; then
        missing+=("psql")
    fi

    # Check Docker (for OpenSearch)
    if [[ "$ENGINES" == *"opensearch"* ]]; then
        if ! command -v docker &> /dev/null; then
            missing+=("docker")
        fi
        if ! command -v docker-compose &> /dev/null && \
           ! docker compose version &> /dev/null 2>&1; then
            missing+=("docker-compose")
        fi
    fi

    # Check MS-MARCO data
    if [[ ! -f "$DATA_DIR/data/collection.tsv" ]]; then
        log_error "MS-MARCO collection.tsv not found in $DATA_DIR/data/"
        log_error "Run: ./datasets/msmarco/download.sh"
        exit 1
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing prerequisites: ${missing[*]}"
        exit 1
    fi

    log_info "All prerequisites satisfied"
}

# Run pg_textsearch benchmark
run_pg_textsearch() {
    log_info "Running pg_textsearch benchmark..."

    local output_file="$OUTPUT_DIR/pg_textsearch_results.json"
    local port="${PGPORT:-5433}"

    # Check if extension is available
    if ! psql -p "$port" -c "SELECT 1 FROM pg_available_extensions \
        WHERE name = 'pg_textsearch'" -t 2>/dev/null | grep -q 1; then
        log_warn "pg_textsearch extension not available, skipping"
        return 1
    fi

    # Run existing benchmark and capture output
    log_info "  Running queries..."
    psql -p "$port" -f "${SCRIPT_DIR}/datasets/msmarco/queries.sql" \
        > "$OUTPUT_DIR/pg_textsearch_output.txt" 2>&1 || true

    # Extract metrics from output and create JSON
    python3 - "$OUTPUT_DIR/pg_textsearch_output.txt" "$output_file" << 'PYTHON'
import sys
import json
import re

input_file = sys.argv[1]
output_file = sys.argv[2]

with open(input_file, 'r') as f:
    content = f.read()

results = {
    "engine": "pg_textsearch",
    "queries": {}
}

# Extract throughput
m = re.search(r'THROUGHPUT_ALL: (\d+) queries in ([\d.]+) ms', content)
if m:
    total_queries = int(m.group(1))
    total_ms = float(m.group(2))
    results["queries"]["throughput"] = {
        "total_queries": total_queries,
        "total_ms": total_ms,
        "avg_ms": total_ms / total_queries if total_queries > 0 else 0,
        "qps": total_queries / (total_ms / 1000) if total_ms > 0 else 0
    }

# Extract percentiles
for metric, pattern in [
    ("p50", r'P50.*?:\s*([\d.]+)\s*ms'),
    ("p95", r'P95.*?:\s*([\d.]+)\s*ms'),
    ("p99", r'P99.*?:\s*([\d.]+)\s*ms'),
]:
    m = re.search(pattern, content, re.IGNORECASE)
    if m:
        if "percentiles" not in results["queries"]:
            results["queries"]["percentiles"] = {}
        results["queries"]["percentiles"][metric] = float(m.group(1))

with open(output_file, 'w') as f:
    json.dump(results, f, indent=2)

print(f"Results saved to {output_file}")
PYTHON

    log_info "pg_textsearch benchmark complete"
}

# Run ParadeDB benchmark
run_paradedb() {
    log_info "Running ParadeDB benchmark..."

    local output_file="$OUTPUT_DIR/paradedb_results.json"
    local port="${PGPORT:-5433}"

    # Check if pg_search extension is available
    if ! psql -p "$port" -c "SELECT 1 FROM pg_available_extensions \
        WHERE name = 'pg_search'" -t 2>/dev/null | grep -q 1; then
        log_warn "ParadeDB pg_search extension not available, skipping"
        return 1
    fi

    # Run setup if not skipping index
    if [[ -z "$SKIP_INDEX" ]]; then
        log_info "  Setting up ParadeDB index..."
        psql -p "$port" -f "${SCRIPT_DIR}/engines/paradedb/setup.sql" \
            > "$OUTPUT_DIR/paradedb_setup.txt" 2>&1 || true
    fi

    # Run queries
    log_info "  Running ParadeDB queries..."
    psql -p "$port" -f "${SCRIPT_DIR}/engines/paradedb/queries.sql" \
        > "$OUTPUT_DIR/paradedb_output.txt" 2>&1 || true

    # Extract metrics and create JSON
    python3 - "$OUTPUT_DIR/paradedb_output.txt" "$output_file" << 'PYTHON'
import sys
import json
import re

input_file = sys.argv[1]
output_file = sys.argv[2]

with open(input_file, 'r') as f:
    content = f.read()

results = {
    "engine": "paradedb",
    "queries": {}
}

# Extract throughput
m = re.search(r'PARADEDB_THROUGHPUT_ALL: (\d+) queries in ([\d.]+) ms', content)
if m:
    total_queries = int(m.group(1))
    total_ms = float(m.group(2))
    results["queries"]["throughput"] = {
        "total_queries": total_queries,
        "total_ms": total_ms,
        "avg_ms": total_ms / total_queries if total_queries > 0 else 0,
        "qps": total_queries / (total_ms / 1000) if total_ms > 0 else 0
    }

# Extract percentiles from table output
for metric, pattern in [
    ("p50", r'P50.*?\|\s*([\d.]+)'),
    ("p95", r'P95.*?\|\s*([\d.]+)'),
    ("p99", r'P99.*?\|\s*([\d.]+)'),
]:
    m = re.search(pattern, content, re.IGNORECASE)
    if m:
        if "percentiles" not in results["queries"]:
            results["queries"]["percentiles"] = {}
        results["queries"]["percentiles"][metric] = float(m.group(1))

with open(output_file, 'w') as f:
    json.dump(results, f, indent=2)

print(f"Results saved to {output_file}")
PYTHON

    log_info "ParadeDB benchmark complete"
}

# Run Tantivy benchmark
run_tantivy() {
    log_info "Running Tantivy benchmark..."

    local output_file="$OUTPUT_DIR/tantivy_results.json"
    local index_dir="$OUTPUT_DIR/tantivy_index"

    # Check if tantivy is installed
    if ! python3 -c "import tantivy" 2>/dev/null; then
        log_warn "tantivy-py not installed, skipping"
        log_warn "Install with: pip install tantivy"
        return 1
    fi

    # Build args
    local args="--data-dir $DATA_DIR/data --output $output_file"
    args="$args --index-dir $index_dir"
    if [[ -n "$SKIP_INDEX" ]]; then
        args="$args --skip-index"
    fi

    python3 "${SCRIPT_DIR}/engines/tantivy/benchmark.py" $args

    log_info "Tantivy benchmark complete: $output_file"
}

# Run OpenSearch benchmark
run_opensearch() {
    log_info "Running OpenSearch benchmark..."

    local output_file="$OUTPUT_DIR/opensearch_results.json"
    local compose_file="${SCRIPT_DIR}/engines/opensearch/docker-compose.yml"

    # Check if opensearch-py is installed
    if ! python3 -c "from opensearchpy import OpenSearch" 2>/dev/null; then
        log_warn "opensearch-py not installed, skipping"
        log_warn "Install with: pip install opensearch-py"
        return 1
    fi

    # Start OpenSearch container
    log_info "  Starting OpenSearch container..."
    if docker compose version &> /dev/null 2>&1; then
        docker compose -f "$compose_file" up -d
    else
        docker-compose -f "$compose_file" up -d
    fi

    # Wait for OpenSearch to be ready
    log_info "  Waiting for OpenSearch to be ready..."
    local max_wait=60
    local waited=0
    while ! curl -s http://localhost:9200 > /dev/null 2>&1; do
        sleep 2
        waited=$((waited + 2))
        if [[ $waited -ge $max_wait ]]; then
            log_error "OpenSearch failed to start within ${max_wait}s"
            return 1
        fi
    done

    # Build args
    local args="--data-dir $DATA_DIR/data --output $output_file"
    if [[ -n "$SKIP_INDEX" ]]; then
        args="$args --skip-index"
    fi

    python3 "${SCRIPT_DIR}/engines/opensearch/benchmark.py" $args

    # Stop OpenSearch container
    log_info "  Stopping OpenSearch container..."
    if docker compose version &> /dev/null 2>&1; then
        docker compose -f "$compose_file" down
    else
        docker-compose -f "$compose_file" down
    fi

    log_info "OpenSearch benchmark complete: $output_file"
}

# Main execution
check_prerequisites

# Run each engine
IFS=',' read -ra ENGINE_LIST <<< "$ENGINES"
for engine in "${ENGINE_LIST[@]}"; do
    engine=$(echo "$engine" | tr -d ' ')
    echo ""
    echo "------------------------------------------------------------"

    case $engine in
        pg_textsearch)
            run_pg_textsearch || true
            ;;
        paradedb)
            run_paradedb || true
            ;;
        tantivy)
            run_tantivy || true
            ;;
        opensearch)
            run_opensearch || true
            ;;
        *)
            log_warn "Unknown engine: $engine"
            ;;
    esac
done

# Aggregate results
echo ""
echo "------------------------------------------------------------"
log_info "Aggregating results..."

python3 "${SCRIPT_DIR}/compare_results.py" \
    --output-dir "$OUTPUT_DIR" \
    --timestamp "$TIMESTAMP" || log_warn "Results aggregation skipped"

echo ""
echo "============================================================"
echo "Benchmark Complete"
echo "============================================================"
echo "Results saved to: $OUTPUT_DIR"
echo "  - Individual engine results: *_results.json"
echo "  - Comparison summary: comparison_results.json"
