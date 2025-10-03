#!/bin/bash

# Memory stress benchmark runner for Tapir extension
# This script runs the memory stress test and captures output

set -e

echo "=== Tapir Memory Stress Benchmark ==="
echo "Starting at: $(date)"
echo ""

# Set up environment
export PGPORT=${PGPORT:-5432}
export PGHOST=${PGHOST:-localhost}
export PGUSER=${PGUSER:-$(whoami)}
export PGDATABASE=${PGDATABASE:-postgres}

# Check if pgtextsearch extension is available
echo "Checking Tapir extension availability..."
psql -c "CREATE EXTENSION IF NOT EXISTS pgtextsearch;" || {
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

# Choose benchmark size based on argument
BENCHMARK_SIZE="${1:-micro}"
BENCHMARK_FILE=""

case "$BENCHMARK_SIZE" in
    "micro")
        BENCHMARK_FILE="sql/memory_stress_micro.sql"
        echo "Running MICRO memory stress benchmark (1K documents, ~1 minute)"
        ;;
    "small")
        BENCHMARK_FILE="sql/memory_stress_small.sql"
        echo "Running SMALL memory stress benchmark (25K documents, ~5-10 minutes)"
        ;;
    "large"|"full")
        BENCHMARK_FILE="sql/memory_stress.sql"
        echo "Running FULL memory stress benchmark (100K documents, may timeout)"
        ;;
    *)
        echo "Usage: $0 [micro|small|large]"
        echo "  micro: 1K documents (default, quick test)"
        echo "  small: 25K documents (moderate stress)"
        echo "  large: 100K documents (high stress, may fail)"
        exit 1
        ;;
esac

# Run the memory stress benchmark
echo "=== Running Memory Stress Benchmark ($BENCHMARK_SIZE) ==="
echo "This demonstrates extension behavior under different dataset sizes..."
echo ""

# Run the benchmark and capture both stdout and stderr
if psql -f "$(dirname "$0")/$BENCHMARK_FILE" 2>&1; then
    echo ""
    echo "=== Benchmark Results ==="
    echo "Benchmark completed successfully at: $(date)"
    echo ""
    case "$BENCHMARK_SIZE" in
        "micro")
            echo "Micro benchmark establishes baseline functionality."
            echo "Try 'small' or 'large' sizes for stress testing."
            ;;
        "small"|"large")
            echo "If benchmark completed without memory errors, try:"
            echo "- Reducing tapir.shared_memory_size in postgresql.conf"
            echo "- Running 'large' size for maximum stress"
            ;;
    esac
else
    echo ""
    echo "=== Benchmark Results ==="
    echo "Benchmark encountered expected memory limitations at: $(date)"
    echo ""
    echo "This demonstrates the extension's behavior under memory pressure."
    echo "Check the error messages above for specific memory-related failures."
fi

echo ""
echo "=== Memory Stress Benchmark Complete ==="
