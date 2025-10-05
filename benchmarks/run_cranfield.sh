#!/bin/bash

# Cranfield Collection BM25 benchmark runner for Tapir extension
# This script runs the complete Cranfield IR benchmark (1400 documents, 225 queries)

set -e

echo "=== Tapir Cranfield Collection Benchmark ==="
echo "Standard Information Retrieval benchmark with 1400 aerodynamics abstracts"
echo "Starting at: $(date)"
echo ""

# Set up environment
export PGPORT=${PGPORT:-5432}
export PGHOST=${PGHOST:-localhost}
export PGUSER=${PGUSER:-$(whoami)}
export PGDATABASE=${PGDATABASE:-postgres}

# Navigate to Cranfield benchmark directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRANFIELD_DIR="$SCRIPT_DIR/sql/cranfield"

if [ ! -d "$CRANFIELD_DIR" ]; then
    echo "ERROR: Cranfield benchmark directory not found at $CRANFIELD_DIR"
    exit 1
fi

cd "$CRANFIELD_DIR"

# Check if pg_textsearch extension is available
echo "Checking Tapir extension availability..."
psql -c "CREATE EXTENSION IF NOT EXISTS pg_textsearch;" || {
    echo "ERROR: Tapir extension not available. Please install it first with 'make install'"
    exit 1
}

echo "Tapir extension loaded successfully."
echo ""

# Show initial configuration (skip if parameters not available)
echo "=== Initial Configuration ==="
psql -c "SHOW tapir.index_memory_limit;" 2>/dev/null || echo "tapir.index_memory_limit: (parameter not visible)"
psql -c "SHOW tapir.default_limit;" 2>/dev/null || echo "tapir.default_limit: (parameter not visible)"
echo ""

# Phase 1: Load Cranfield dataset
echo "=== Phase 1: Data Loading ==="
echo "Loading 1400 Cranfield collection documents..."
echo ""

START_TIME=$(date +%s)
psql -f 01-load.sql || {
    echo "ERROR: Failed to load Cranfield dataset"
    exit 1
}
LOAD_TIME=$(($(date +%s) - START_TIME))

echo ""
echo "Data loading completed in ${LOAD_TIME} seconds"
echo ""

# Phase 2: Run query benchmarks
echo "=== Phase 2: Query Benchmarks ==="
echo "Running 225 Cranfield queries with BM25 scoring..."
echo ""

START_TIME=$(date +%s)
psql -f 02-queries.sql || {
    echo "ERROR: Failed to run query benchmarks"
    exit 1
}
QUERY_TIME=$(($(date +%s) - START_TIME))

echo ""
echo "Query benchmarks completed in ${QUERY_TIME} seconds"
echo ""

# Summary
TOTAL_TIME=$((LOAD_TIME + QUERY_TIME))
echo "=== Benchmark Summary ==="
echo "Data Loading Time:  ${LOAD_TIME} seconds"
echo "Query Execution:    ${QUERY_TIME} seconds"
echo "Total Runtime:      ${TOTAL_TIME} seconds"
echo "Dataset Size:       1400 documents, 225 queries"
echo "Completed at:       $(date)"
echo ""
echo "Benchmark completed successfully!"
