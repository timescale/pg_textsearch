#!/bin/bash
# MS MARCO v2 Full Benchmark Runner
# Runs pg_textsearch and ParadeDB benchmarks sequentially on 138M passages
#
# Prerequisites:
#   - download.sh has been run (collection.tsv exists)
#   - generate_benchmark_queries.sh has been run (benchmark_queries.tsv exists)
#   - pg_textsearch is built and installed (make && sudo make install)
#   - ParadeDB .deb is installed
#
# Usage: ./run_v2_benchmark.sh [step]
#   Steps:
#     all          - Run everything (default)
#     load-tapir   - Load data + build pg_textsearch index
#     query-tapir  - Run pg_textsearch query benchmarks
#     load-systemx - Load data + build ParadeDB index
#     query-systemx - Run ParadeDB query benchmarks

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
LOG_DIR="/tmp/msmarco-v2-benchmark"
STEP="${1:-all}"

# Postgres connection
PGPORT="${PGPORT:-5432}"
PGDATABASE="${PGDATABASE:-postgres}"

export DATA_DIR PGPORT PGDATABASE

mkdir -p "$LOG_DIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)

echo "=== MS MARCO v2 Benchmark ==="
echo "Timestamp: $TIMESTAMP"
echo "Step: $STEP"
echo "Data dir: $DATA_DIR"
echo "PG port: $PGPORT"
echo "Log dir: $LOG_DIR"
echo ""

# Verify prerequisites
check_prereqs() {
    if [ ! -f "$DATA_DIR/collection.tsv" ]; then
        echo "ERROR: collection.tsv not found. Run ./download.sh first."
        exit 1
    fi
    if [ ! -f "$SCRIPT_DIR/benchmark_queries.tsv" ]; then
        echo "ERROR: benchmark_queries.tsv not found. Run ./generate_benchmark_queries.sh first."
        exit 1
    fi
    echo "Prerequisites OK"
    echo "  Collection: $(wc -l < "$DATA_DIR/collection.tsv") passages"
    echo "  Benchmark queries: $(wc -l < "$SCRIPT_DIR/benchmark_queries.tsv") queries"
    echo ""
}

# Verify pg_textsearch version
check_tapir_version() {
    local ver
    ver=$(psql -t -c "SELECT default_version FROM pg_available_extensions WHERE name = 'pg_textsearch';" 2>/dev/null | tr -d ' ')
    echo "pg_textsearch version: $ver"
    if [ "$ver" != "1.0.0-dev" ]; then
        echo "WARNING: Expected 1.0.0-dev, got '$ver'"
    fi
}

# Show system info
show_system_info() {
    echo "=== System Info ==="
    echo "CPU: $(nproc) cores"
    echo "RAM: $(free -h | awk '/^Mem:/{print $2}')"
    echo "Disk (NVMe): $(df -h /nvme | tail -1 | awk '{print $4 " avail of " $2}')"
    echo "PG data dir: $(psql -t -c 'SHOW data_directory;' | tr -d ' ')"
    echo "PG version: $(psql -t -c 'SELECT version();' | head -1)"
    echo ""
}

load_tapir() {
    echo "=== Loading pg_textsearch (Tapir) ==="
    local log="$LOG_DIR/tapir_load_${TIMESTAMP}.log"
    echo "Log: $log"

    check_tapir_version

    # Use max parallelism for index build
    local ncpu
    ncpu=$(nproc)
    local workers=$((ncpu - 2))
    if [ "$workers" -lt 2 ]; then
        workers=2
    fi
    echo "Using $workers parallel maintenance workers"

    psql -v ON_ERROR_STOP=1 <<EOSQL 2>&1 | tee "$log"
\timing on

-- Ensure extension is loaded
DROP EXTENSION IF EXISTS pg_textsearch CASCADE;
CREATE EXTENSION pg_textsearch;

-- Max out parallelism for this session
SET max_parallel_maintenance_workers = $workers;
SET maintenance_work_mem = '8GB';

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_v2_passages CASCADE;
DROP TABLE IF EXISTS msmarco_v2_queries CASCADE;
DROP TABLE IF EXISTS msmarco_v2_qrels CASCADE;

-- Create tables
\echo 'Creating tables...'
CREATE TABLE msmarco_v2_passages (
    passage_id TEXT PRIMARY KEY,
    passage_text TEXT NOT NULL
);
CREATE TABLE msmarco_v2_queries (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);
CREATE TABLE msmarco_v2_qrels (
    query_id INTEGER NOT NULL,
    passage_id TEXT NOT NULL,
    relevance INTEGER NOT NULL,
    PRIMARY KEY (query_id, passage_id)
);

-- Load passages
\echo 'Loading 138M passages...'
\copy msmarco_v2_passages(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \"\\\"\" \$1 \"\\\"\", \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Load queries
\echo 'Loading queries...'
\copy msmarco_v2_queries(query_id, query_text) FROM PROGRAM 'cat "$DATA_DIR/passv2_dev_queries.tsv"' WITH (FORMAT text, DELIMITER E'\t');

-- Load qrels
\echo 'Loading qrels...'
CREATE TEMP TABLE qrels_raw (
    query_id INTEGER, zero INTEGER, passage_id TEXT, relevance INTEGER
);
\copy qrels_raw FROM PROGRAM 'cat "$DATA_DIR/passv2_dev_qrels.tsv"' WITH (FORMAT text, DELIMITER E'\t');
INSERT INTO msmarco_v2_qrels (query_id, passage_id, relevance)
SELECT query_id, passage_id, relevance FROM qrels_raw;
DROP TABLE qrels_raw;

-- Verify
\echo ''
\echo '=== Data Verification ==='
SELECT 'Passages:' as what, COUNT(*)::text as n FROM msmarco_v2_passages
UNION ALL SELECT 'Queries:', COUNT(*)::text FROM msmarco_v2_queries
UNION ALL SELECT 'Qrels:', COUNT(*)::text FROM msmarco_v2_qrels
ORDER BY what;

-- Build BM25 index with max parallelism
\echo ''
\echo '=== Building BM25 Index (parallel workers: $workers) ==='
CREATE INDEX msmarco_v2_bm25_idx ON msmarco_v2_passages
    USING bm25(passage_text) WITH (text_config='english');

-- Report sizes
\echo ''
\echo '=== Sizes ==='
SELECT 'INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_v2_bm25_idx')) as size,
    pg_relation_size('msmarco_v2_bm25_idx') as bytes;
SELECT 'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('msmarco_v2_passages')) as size,
    pg_total_relation_size('msmarco_v2_passages') as bytes;

\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('msmarco_v2_bm25_idx');

\echo '=== pg_textsearch Load Complete ==='
EOSQL

    echo ""
    echo "Load log: $log"
}

load_tapir_index_only() {
    echo "=== Building pg_textsearch Index Only ==="
    local log="$LOG_DIR/tapir_index_${TIMESTAMP}.log"
    echo "Log: $log"

    check_tapir_version

    local ncpu
    ncpu=$(nproc)
    local workers=$((ncpu - 2))
    if [ "$workers" -lt 2 ]; then
        workers=2
    fi
    echo "Using $workers parallel maintenance workers"

    psql -v ON_ERROR_STOP=1 <<EOSQL 2>&1 | tee "$log"
\timing on

SET max_parallel_maintenance_workers = $workers;
SET maintenance_work_mem = '8GB';

-- Drop old index if exists
DROP INDEX IF EXISTS msmarco_v2_bm25_idx;

\echo '=== Building BM25 Index (parallel workers: $workers) ==='
CREATE INDEX msmarco_v2_bm25_idx ON msmarco_v2_passages
    USING bm25(passage_text) WITH (text_config='english');

\echo ''
\echo '=== Sizes ==='
SELECT 'INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_v2_bm25_idx')) as size,
    pg_relation_size('msmarco_v2_bm25_idx') as bytes;
SELECT 'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('msmarco_v2_passages')) as size,
    pg_total_relation_size('msmarco_v2_passages') as bytes;

\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('msmarco_v2_bm25_idx');

\echo '=== Index Build Complete ==='
EOSQL

    echo ""
    echo "Index build log: $log"
}

query_tapir() {
    echo "=== Running pg_textsearch Query Benchmarks ==="
    local log="$LOG_DIR/tapir_queries_${TIMESTAMP}.log"
    echo "Log: $log"

    cd "$REPO_DIR"
    psql -f benchmarks/datasets/msmarco-v2/queries.sql 2>&1 | tee "$log"

    echo ""
    echo "Query log: $log"
    echo ""
    echo "=== Key Results ==="
    grep -E "LATENCY_BUCKET|THROUGHPUT_RESULT|INDEX_SIZE" "$log" || true
}

load_systemx() {
    echo "=== Loading ParadeDB (System X) ==="
    local log="$LOG_DIR/systemx_load_${TIMESTAMP}.log"
    echo "Log: $log"

    # Drop pg_textsearch first to avoid conflicts
    psql -c "DROP EXTENSION IF EXISTS pg_textsearch CASCADE;" 2>/dev/null || true

    cd "$REPO_DIR"
    psql -f benchmarks/datasets/msmarco-v2/systemx/load.sql 2>&1 | tee "$log"

    echo ""
    echo "Load log: $log"
}

query_systemx() {
    echo "=== Running ParadeDB Query Benchmarks ==="
    local log="$LOG_DIR/systemx_queries_${TIMESTAMP}.log"
    echo "Log: $log"

    cd "$REPO_DIR"
    psql -f benchmarks/datasets/msmarco-v2/systemx/queries.sql 2>&1 | tee "$log"

    echo ""
    echo "Query log: $log"
    echo ""
    echo "=== Key Results ==="
    grep -E "LATENCY_BUCKET|THROUGHPUT_RESULT|INDEX_SIZE" "$log" || true
}

cleanup_tapir() {
    echo "=== Cleaning up pg_textsearch data ==="
    psql -c "DROP TABLE IF EXISTS msmarco_v2_passages CASCADE;"
    psql -c "DROP TABLE IF EXISTS msmarco_v2_queries CASCADE;"
    psql -c "DROP TABLE IF EXISTS msmarco_v2_qrels CASCADE;"
    psql -c "DROP EXTENSION IF EXISTS pg_textsearch CASCADE;"
    echo "Cleanup complete"
}

cleanup_systemx() {
    echo "=== Cleaning up ParadeDB data ==="
    psql -c "DROP TABLE IF EXISTS msmarco_v2_passages_systemx CASCADE;"
    psql -c "DROP TABLE IF EXISTS msmarco_v2_queries_systemx CASCADE;"
    psql -c "DROP TABLE IF EXISTS msmarco_v2_qrels_systemx CASCADE;"
    psql -c "DROP EXTENSION IF EXISTS pg_search CASCADE;"
    echo "Cleanup complete"
}

# Main
check_prereqs
show_system_info

case "$STEP" in
    all)
        load_tapir
        query_tapir
        cleanup_tapir
        load_systemx
        query_systemx
        cleanup_systemx
        ;;
    load-tapir)
        load_tapir
        ;;
    index-tapir)
        load_tapir_index_only
        ;;
    query-tapir)
        query_tapir
        ;;
    load-systemx)
        load_systemx
        ;;
    query-systemx)
        query_systemx
        ;;
    cleanup-tapir)
        cleanup_tapir
        ;;
    cleanup-systemx)
        cleanup_systemx
        ;;
    *)
        echo "Unknown step: $STEP"
        echo "Valid steps: all, load-tapir, index-tapir, query-tapir, load-systemx, query-systemx, cleanup-tapir, cleanup-systemx"
        exit 1
        ;;
esac

echo ""
echo "=== Done ==="
echo "Logs in: $LOG_DIR"
