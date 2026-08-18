#!/bin/bash
#
# Experiment 2: Upper bound on fragmentation impact.
#
# Compares query latency between:
#   A) Normal bulk index build (contiguous pages)
#   B) Bulk index build with randomized page allocation
#      (worst-case scattered pages)
#
# Both builds produce the same segment structure and posting
# lists — only the physical page layout differs.  The delta
# is the maximum possible benefit of defragmentation.
#
# Usage:
#   ./benchmarks/run_fragmentation_upperbound.sh
#
# Prerequisites:
#   - msmarco_v2_passages table loaded (from download.sh)
#   - pg_textsearch installed and in shared_preload_libraries
#
# Environment variables:
#   PGPORT, PGHOST, PGUSER, PGDATABASE — standard PG env
#   TABLE_NAME    — source table (default: msmarco_v2_passages)
#   ROW_LIMIT     — rows to index (default: all)
#   SKIP_NORMAL   — set to 1 to skip normal build
#   SKIP_RANDOM   — set to 1 to skip randomized build

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export PGPORT=${PGPORT:-5432}
if [ "$(uname)" = "Darwin" ]; then
    export PGHOST=${PGHOST:-/tmp}
else
    export PGHOST=${PGHOST:-/var/run/postgresql}
fi
export PGUSER=${PGUSER:-$(whoami)}
export PGDATABASE=${PGDATABASE:-postgres}

TABLE_NAME=${TABLE_NAME:-msmarco_v2_passages}
BENCHMARK_QUERIES="$SCRIPT_DIR/datasets/msmarco-v2/benchmark_queries.tsv"
QUERY_SQL="$SCRIPT_DIR/sql/msmarco_defrag_queries.sql"

RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"
REPORT="$RESULTS_DIR/frag_upperbound_$(date +%Y%m%d_%H%M%S).txt"

now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }

header() {
    echo ""
    echo "================================================================"
    echo "  $1"
    echo "================================================================"
    echo ""
}

log() { echo "[$(date '+%H:%M:%S')] $*"; }

cold_restart() {
    log "Stopping PostgreSQL..."
    if [ "$(uname)" = "Darwin" ]; then
        brew services stop "${PG_BREW_SERVICE:-postgresql@17}" \
            2>/dev/null || true
    else
        sudo pg_ctl stop -D "$PGDATA" -m fast 2>/dev/null \
            || sudo systemctl stop postgresql 2>/dev/null \
            || true
    fi
    sleep 2

    log "Dropping OS page cache..."
    if [ "$(uname)" = "Darwin" ]; then
        sudo purge 2>/dev/null \
            || log "  (purge unavailable)"
    else
        sync
        echo 3 | sudo tee /proc/sys/vm/drop_caches \
            >/dev/null 2>&1 \
            || log "  (drop_caches unavailable)"
    fi

    log "Starting PostgreSQL..."
    if [ "$(uname)" = "Darwin" ]; then
        brew services start "${PG_BREW_SERVICE:-postgresql@17}" \
            2>/dev/null || true
    else
        sudo pg_ctl start -D "$PGDATA" 2>/dev/null \
            || sudo systemctl start postgresql 2>/dev/null \
            || true
    fi
    sleep 3
    until psql -c "SELECT 1" >/dev/null 2>&1; do sleep 1; done
    log "PostgreSQL ready (cold cache)."
}

exec > >(tee "$REPORT") 2>&1

header "Fragmentation Upper-Bound Experiment"
log "PG version:     $(psql -t -A -c 'SHOW server_version;')"
log "shared_buffers:  $(psql -t -A -c 'SHOW shared_buffers;')"
log "Source table:    $TABLE_NAME"
log "Report file:     $REPORT"

psql -q -c "CREATE EXTENSION IF NOT EXISTS pg_textsearch;" \
    2>/dev/null || true

# Verify source table
ROW_CT=$(psql -t -A -c \
    "SELECT COUNT(*) FROM $TABLE_NAME;" 2>/dev/null || echo 0)
log "Source rows:     $ROW_CT"
if [ "$ROW_CT" -le 0 ] 2>/dev/null; then
    log "ERROR: source table $TABLE_NAME is empty or missing."
    exit 1
fi

# Create bench table (copy of source, shared by both builds)
ROW_LIMIT_CLAUSE=""
if [ -n "$ROW_LIMIT" ]; then
    ROW_LIMIT_CLAUSE="LIMIT $ROW_LIMIT"
    log "ROW_LIMIT:       $ROW_LIMIT"
fi

header "Preparing bench table"

psql -q <<SQL
DROP TABLE IF EXISTS msmarco_v2_bench CASCADE;
CREATE TABLE msmarco_v2_bench (
    passage_id TEXT PRIMARY KEY,
    passage_text TEXT NOT NULL
);
INSERT INTO msmarco_v2_bench
    SELECT passage_id, passage_text
    FROM $TABLE_NAME
    $ROW_LIMIT_CLAUSE;
ANALYZE msmarco_v2_bench;
SQL

BENCH_ROWS=$(psql -t -A -c "SELECT COUNT(*) FROM msmarco_v2_bench;")
log "Bench table rows: $BENCH_ROWS"

# Load benchmark queries
psql -q -c "DROP TABLE IF EXISTS defrag_bench_results;"
psql -q -c "DROP TABLE IF EXISTS benchmark_queries;"
psql -q -c "
CREATE TABLE benchmark_queries (
    query_id    INTEGER,
    query_text  TEXT,
    token_bucket INTEGER
);
"
psql -q -c "\copy benchmark_queries FROM '$BENCHMARK_QUERIES' \
    WITH (FORMAT text, DELIMITER E'\t')"
BQ=$(psql -t -A -c "SELECT COUNT(*) FROM benchmark_queries;")
log "Benchmark queries: $BQ"

# ==============================================================
# Run A: Normal (contiguous) bulk build
# ==============================================================

if [ "${SKIP_NORMAL:-0}" != "1" ]; then
    header "Run A: Normal bulk index build"

    psql -q -c "DROP INDEX IF EXISTS msmarco_v2_bm25_idx;"
    log "Building index (normal page allocation)..."
    START=$(now_ms)
    psql -q -c "
SET pg_textsearch.debug_randomize_pages = false;
CREATE INDEX msmarco_v2_bm25_idx ON msmarco_v2_bench
    USING bm25(passage_text) WITH (text_config='english');
"
    END=$(now_ms)
    BUILD_NORMAL=$((END - START))
    log "Normal build: ${BUILD_NORMAL} ms"

    IDX_SIZE_NORMAL=$(psql -t -A -c \
        "SELECT pg_size_pretty(pg_relation_size('msmarco_v2_bm25_idx'));")
    log "Index size (normal): $IDX_SIZE_NORMAL"

    psql -c "SELECT bm25_summarize_index('msmarco_v2_bm25_idx');"

    cold_restart

    log "Running query benchmark (normal)..."
    psql -v run_label='normal' -f "$QUERY_SQL"
else
    log "SKIP_NORMAL=1 — skipping."
fi

# ==============================================================
# Run B: Randomized (worst-case fragmented) bulk build
# ==============================================================

if [ "${SKIP_RANDOM:-0}" != "1" ]; then
    header "Run B: Randomized page allocation build"

    psql -q -c "DROP INDEX IF EXISTS msmarco_v2_bm25_idx;"
    log "Building index (randomized page allocation)..."
    START=$(now_ms)
    psql -q -c "
SET pg_textsearch.debug_randomize_pages = true;
CREATE INDEX msmarco_v2_bm25_idx ON msmarco_v2_bench
    USING bm25(passage_text) WITH (text_config='english');
SET pg_textsearch.debug_randomize_pages = false;
"
    END=$(now_ms)
    BUILD_RANDOM=$((END - START))
    log "Randomized build: ${BUILD_RANDOM} ms"

    IDX_SIZE_RANDOM=$(psql -t -A -c \
        "SELECT pg_size_pretty(pg_relation_size('msmarco_v2_bm25_idx'));")
    log "Index size (randomized): $IDX_SIZE_RANDOM"

    psql -c "SELECT bm25_summarize_index('msmarco_v2_bm25_idx');"

    cold_restart

    log "Running query benchmark (randomized)..."
    psql -v run_label='randomized' -f "$QUERY_SQL"
else
    log "SKIP_RANDOM=1 — skipping."
fi

# ==============================================================
# Summary
# ==============================================================

header "SUMMARY"

echo "Build time (normal):      ${BUILD_NORMAL:-skipped} ms"
echo "Build time (randomized):  ${BUILD_RANDOM:-skipped} ms"
echo "Index size (normal):      ${IDX_SIZE_NORMAL:-skipped}"
echo "Index size (randomized):  ${IDX_SIZE_RANDOM:-skipped}"
echo ""

echo "--- Per-bucket latency comparison (warm cache, ms) ---"
echo ""
psql -t -A --pset="format=aligned" -c "
SELECT
    n.bucket,
    n.num_queries AS n,
    round(n.p50_ms, 1)  AS normal_p50,
    round(r.p50_ms, 1)  AS random_p50,
    CASE WHEN r.p50_ms > 0
         THEN round((r.p50_ms - n.p50_ms) / r.p50_ms * 100, 1)
         ELSE 0 END     AS p50_defrag_gain_pct,
    round(n.avg_ms, 1)  AS normal_avg,
    round(r.avg_ms, 1)  AS random_avg,
    CASE WHEN r.avg_ms > 0
         THEN round((r.avg_ms - n.avg_ms) / r.avg_ms * 100, 1)
         ELSE 0 END     AS avg_defrag_gain_pct
FROM defrag_bench_results n
JOIN defrag_bench_results r
    ON n.bucket = r.bucket
WHERE n.run_label = 'normal'
  AND r.run_label = 'randomized'
ORDER BY n.bucket;
" 2>/dev/null || echo "(comparison requires both runs)"

echo ""
echo "--- Weighted-average latency ---"
psql -t -A -c "
WITH w(bucket, weight) AS (
    VALUES (1, 35638), (2, 165033), (3, 304887), (4, 264177),
           (5, 143765), (6, 59558), (7, 22595), (8, 15252)
)
SELECT
    'Normal     weighted avg: '
        || round(SUM(s.avg_ms * w.weight) / SUM(w.weight), 2)
        || ' ms'
FROM defrag_bench_results s
JOIN w ON s.bucket = w.bucket
WHERE s.run_label = 'normal'
UNION ALL
SELECT
    'Randomized weighted avg: '
        || round(SUM(c.avg_ms * w.weight) / SUM(w.weight), 2)
        || ' ms'
FROM defrag_bench_results c
JOIN w ON c.bucket = w.bucket
WHERE c.run_label = 'randomized';
" 2>/dev/null || echo "(comparison requires both runs)"

echo ""
log "Full report saved to: $REPORT"
log "Benchmark complete."
