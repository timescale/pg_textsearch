#!/bin/bash
#
# MS-MARCO v2 defragmentation benchmark.
#
# Self-contained script that:
#   1. Downloads the MS-MARCO v2 dataset (~20 GB)
#   2. Loads 138M passages into PostgreSQL
#   3. Builds a *fragmented* BM25 index incrementally
#   4. Runs a multi-token query mix (692 queries, 8 buckets)
#   5. Force-merges with defragmentation
#   6. Reruns the query mix
#   7. Prints a side-by-side summary report
#
# Cold-cache measurements: restarts PostgreSQL between query rounds.
#
# Usage:
#   ./benchmarks/run_msmarco_defrag_bench.sh [BATCH_SIZE]
#
# BATCH_SIZE  — rows per incremental INSERT batch (default 1000000)
#
# Environment variables:
#   PGPORT, PGHOST, PGUSER, PGDATABASE — standard PG env vars
#   PG_BREW_SERVICE — brew service name (default: postgresql@17)
#   SKIP_DOWNLOAD   — set to 1 to skip the download step
#   SKIP_LOAD       — set to 1 to skip data loading (table must exist)
#   SKIP_INDEX      — set to 1 to skip index build (index must exist)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BATCH_SIZE=${1:-1000000}
PG_BREW_SERVICE=${PG_BREW_SERVICE:-postgresql@17}

export PGPORT=${PGPORT:-5432}
if [ "$(uname)" = "Darwin" ]; then
    export PGHOST=${PGHOST:-/tmp}
else
    export PGHOST=${PGHOST:-/var/run/postgresql}
fi
export PGUSER=${PGUSER:-$(whoami)}
export PGDATABASE=${PGDATABASE:-postgres}

DATA_DIR="$SCRIPT_DIR/datasets/msmarco-v2/data"
BENCHMARK_QUERIES="$SCRIPT_DIR/datasets/msmarco-v2/benchmark_queries.tsv"
QUERY_SQL="$SCRIPT_DIR/sql/msmarco_defrag_queries.sql"

RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"
REPORT_FILE="$RESULTS_DIR/msmarco_defrag_$(date +%Y%m%d_%H%M%S).txt"

now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }

header() {
    local msg="$1"
    echo ""
    echo "================================================================"
    echo "  $msg"
    echo "================================================================"
    echo ""
}

log() { echo "[$(date '+%H:%M:%S')] $*"; }

cold_restart() {
    log "Stopping PostgreSQL..."
    if [ "$(uname)" = "Darwin" ]; then
        brew services stop "$PG_BREW_SERVICE" 2>/dev/null \
            || pg_ctl stop -D "$PGDATA" -m fast 2>/dev/null \
            || true
    else
        sudo pg_ctl stop -D "$PGDATA" -m fast 2>/dev/null \
            || sudo systemctl stop postgresql 2>/dev/null \
            || true
    fi
    sleep 2

    log "Dropping OS page cache..."
    if [ "$(uname)" = "Darwin" ]; then
        sudo purge 2>/dev/null \
            || log "  (purge unavailable — OS cache not cleared)"
    else
        sync
        echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 \
            || log "  (drop_caches unavailable — OS cache not cleared)"
    fi

    log "Starting PostgreSQL..."
    if [ "$(uname)" = "Darwin" ]; then
        brew services start "$PG_BREW_SERVICE" 2>/dev/null \
            || pg_ctl start -D "$PGDATA" 2>/dev/null \
            || true
    else
        sudo pg_ctl start -D "$PGDATA" 2>/dev/null \
            || sudo systemctl start postgresql 2>/dev/null \
            || true
    fi
    sleep 3
    until psql -c "SELECT 1" >/dev/null 2>&1; do sleep 1; done
    log "PostgreSQL ready (cold cache: shared_buffers + OS cleared)."
}

# Tee all output to the report file while keeping it on stdout.
exec > >(tee "$REPORT_FILE") 2>&1

header "MS-MARCO v2 Defrag Benchmark"
log "Batch size:        $BATCH_SIZE"
log "PG version:        $(psql -t -A -c 'SHOW server_version;')"
log "shared_buffers:    $(psql -t -A -c 'SHOW shared_buffers;')"
log "Report file:       $REPORT_FILE"

psql -q -c "CREATE EXTENSION IF NOT EXISTS pg_textsearch;" 2>/dev/null

# ==============================================================
# Step 1: Download MS-MARCO v2
# ==============================================================

header "Step 1: Download MS-MARCO v2 dataset"

if [ "${SKIP_DOWNLOAD:-0}" = "1" ]; then
    log "SKIP_DOWNLOAD=1 — skipping."
elif [ -f "$DATA_DIR/collection.tsv" ] \
     && [ "$(wc -c < "$DATA_DIR/collection.tsv")" -gt 1000 ]; then
    log "collection.tsv already present — skipping download."
else
    rm -f "$DATA_DIR/collection.tsv" 2>/dev/null || true
    log "Running download script (this downloads ~20 GB)..."
    log "If this is interrupted, re-run and it will resume."
    bash "$SCRIPT_DIR/datasets/msmarco-v2/download.sh"
fi

log "Dataset files:"
ls -lh "$DATA_DIR"/*.tsv 2>/dev/null || log "(no files yet)"

# Validate collection.tsv is not empty after download
if [ ! -f "$DATA_DIR/collection.tsv" ]; then
    COLLECTION_SIZE=0
else
    COLLECTION_SIZE=$(wc -c < "$DATA_DIR/collection.tsv")
fi
if [ "$COLLECTION_SIZE" -lt 1000 ]; then
    log "ERROR: collection.tsv is empty or missing."
    log "       The 20 GB download may have failed."
    log "       Run the download manually to see errors:"
    log "         cd $DATA_DIR && bash ../download.sh"
    exit 1
fi

# ==============================================================
# Step 2: Load data (no index)
# ==============================================================

header "Step 2: Load passages into PostgreSQL"

if [ "${SKIP_LOAD:-0}" = "1" ]; then
    log "SKIP_LOAD=1 — skipping."
else
    ROW_COUNT=$(psql -t -A -c \
        "SELECT COUNT(*) FROM msmarco_v2_passages;" 2>/dev/null || echo 0)
    if [ "$ROW_COUNT" -gt 100000000 ]; then
        log "msmarco_v2_passages already has $ROW_COUNT rows — skipping."
    else
        log "Loading data via load_data_only.sql..."
        DATA_DIR="$DATA_DIR" psql -f \
            "$SCRIPT_DIR/datasets/msmarco-v2/load_data_only.sql"
    fi
fi

TOTAL_ROWS=$(psql -t -A -c "SELECT COUNT(*) FROM msmarco_v2_passages;")
log "Total passages: $TOTAL_ROWS"
if [ "$TOTAL_ROWS" -le 0 ] 2>/dev/null; then
    log "ERROR: 0 passages loaded. Check that collection.tsv"
    log "       is not empty and re-run the download step."
    exit 1
fi

# ==============================================================
# Step 3: Build fragmented index incrementally
# ==============================================================

header "Step 3: Build fragmented BM25 index (incremental)"

if [ "${SKIP_INDEX:-0}" = "1" ]; then
    log "SKIP_INDEX=1 — skipping."
else
    log "Creating bench table + empty index..."
    psql -q <<'SQL'
DROP TABLE IF EXISTS msmarco_v2_bench CASCADE;
CREATE TABLE msmarco_v2_bench (
    passage_id TEXT PRIMARY KEY,
    passage_text TEXT NOT NULL
);
CREATE INDEX msmarco_v2_bm25_idx ON msmarco_v2_bench
    USING bm25(passage_text) WITH (text_config='english');
SQL

    # Use ctid page ranges for efficient batch slicing (PG 14+
    # TID Range Scan). No extra columns or tables needed.
    psql -q -c "ANALYZE msmarco_v2_passages;"
    TOTAL_PAGES=$(psql -t -A -c "
        SELECT COALESCE(NULLIF(relpages, 0),
               pg_relation_size('msmarco_v2_passages')::bigint
                   / current_setting('block_size')::bigint)
        FROM pg_class WHERE relname = 'msmarco_v2_passages';
    ")
    TOTAL_PAGES=${TOTAL_PAGES:-1}
    if [ "$TOTAL_PAGES" -le 0 ] 2>/dev/null; then
        TOTAL_PAGES=1
    fi

    ROWS_PER_PAGE=$(python3 -c \
        "print(max(1, $TOTAL_ROWS // max(1, $TOTAL_PAGES)))")
    PAGES_PER_BATCH=$(python3 -c \
        "print(max(1, $BATCH_SIZE // max(1, $ROWS_PER_PAGE)))")
    log "Source table: $TOTAL_PAGES pages, ~$ROWS_PER_PAGE rows/page"
    log "Batch size: ~$PAGES_PER_BATCH pages ($BATCH_SIZE rows)"

    log "Incrementally inserting in batches..."
    INSERTED=0
    SPILL_COUNT=0
    PAGE_START=0

    while [ "$PAGE_START" -lt "$TOTAL_PAGES" ]; do
        PAGE_END=$((PAGE_START + PAGES_PER_BATCH))
        if [ "$PAGE_END" -gt "$TOTAL_PAGES" ]; then
            PAGE_END=$((TOTAL_PAGES + 1))
        fi

        BATCH_CT=$(psql -t -A -c "
            WITH ins AS (
                INSERT INTO msmarco_v2_bench
                SELECT passage_id, passage_text
                FROM msmarco_v2_passages
                WHERE ctid >= '($PAGE_START,0)'::tid
                  AND ctid <  '($PAGE_END,0)'::tid
                RETURNING 1
            )
            SELECT COUNT(*) FROM ins;
        ")
        INSERTED=$((INSERTED + BATCH_CT))
        PAGE_START=$PAGE_END

        psql -q -c \
            "SELECT bm25_spill_index('msmarco_v2_bm25_idx');" \
            >/dev/null 2>&1 || true
        SPILL_COUNT=$((SPILL_COUNT + 1))

        log "  $INSERTED / $TOTAL_ROWS rows ($SPILL_COUNT spills)"
    done

    log "Insert complete: $INSERTED rows, $SPILL_COUNT spills."
    log "Analyzing table..."
    psql -q -c "ANALYZE msmarco_v2_bench;"
fi

echo ""
log "Pre-merge index summary:"
psql -c "SELECT bm25_summarize_index('msmarco_v2_bm25_idx');"

IDX_SIZE_BEFORE=$(psql -t -A -c \
    "SELECT pg_size_pretty(pg_relation_size('msmarco_v2_bm25_idx'));")
log "Index size (scattered): $IDX_SIZE_BEFORE"

# ==============================================================
# Step 3b: Load benchmark queries
# ==============================================================

log "Loading benchmark queries and cleaning prior results..."
psql -q -c "DROP TABLE IF EXISTS defrag_bench_results;"
psql -q -c "DROP TABLE IF EXISTS benchmark_queries;"
psql -q -c "
CREATE TABLE benchmark_queries (
    query_id    INTEGER,
    query_text  TEXT,
    token_bucket INTEGER
);
"
psql -q -c "\copy benchmark_queries FROM '$BENCHMARK_QUERIES' WITH (FORMAT text, DELIMITER E'\t')"
BQ_COUNT=$(psql -t -A -c "SELECT COUNT(*) FROM benchmark_queries;")
log "Loaded $BQ_COUNT benchmark queries."

# ==============================================================
# Step 4: Query benchmark — scattered segments (cold cache)
# ==============================================================

header "Step 4: Query benchmark — SCATTERED segments"

cold_restart

log "Running query benchmark (scattered)..."
psql -v run_label='scattered' -f "$QUERY_SQL"

# ==============================================================
# Step 5: Force merge with defragmentation
# ==============================================================

header "Step 5: Force merge WITH defragmentation"

log "Running bm25_force_merge..."
START_MS=$(now_ms)
psql -q -c "
SET pg_textsearch.defrag_on_merge = true;
SELECT bm25_force_merge('msmarco_v2_bm25_idx');
"
END_MS=$(now_ms)
MERGE_TIME_MS=$((END_MS - START_MS))
log "Force merge completed in ${MERGE_TIME_MS} ms"

echo ""
log "Post-merge index summary:"
psql -c "SELECT bm25_summarize_index('msmarco_v2_bm25_idx');"

IDX_SIZE_AFTER=$(psql -t -A -c \
    "SELECT pg_size_pretty(pg_relation_size('msmarco_v2_bm25_idx'));")
log "Index size (contiguous): $IDX_SIZE_AFTER"

# ==============================================================
# Step 6: Query benchmark — contiguous segment (cold cache)
# ==============================================================

header "Step 6: Query benchmark — CONTIGUOUS segment"

cold_restart

log "Running query benchmark (contiguous)..."
psql -v run_label='contiguous' -f "$QUERY_SQL"

# ==============================================================
# Step 7: Summary report
# ==============================================================

header "SUMMARY REPORT"

echo "Force merge time:    ${MERGE_TIME_MS} ms"
echo "Index size before:   ${IDX_SIZE_BEFORE}"
echo "Index size after:    ${IDX_SIZE_AFTER}"
echo ""

echo "--- Per-bucket latency comparison (cold cache, ms) ---"
echo ""
psql -t -A --pset="format=aligned" -c "
SELECT
    s.bucket,
    s.num_queries AS n,
    round(s.p50_ms, 1)  AS scattered_p50,
    round(c.p50_ms, 1)  AS contiguous_p50,
    CASE WHEN s.p50_ms > 0
         THEN round((s.p50_ms - c.p50_ms) / s.p50_ms * 100, 1)
         ELSE 0 END     AS p50_pct_change,
    round(s.p95_ms, 1)  AS scattered_p95,
    round(c.p95_ms, 1)  AS contiguous_p95,
    round(s.avg_ms, 1)  AS scattered_avg,
    round(c.avg_ms, 1)  AS contiguous_avg,
    CASE WHEN s.avg_ms > 0
         THEN round((s.avg_ms - c.avg_ms) / s.avg_ms * 100, 1)
         ELSE 0 END     AS avg_pct_change
FROM defrag_bench_results s
JOIN defrag_bench_results c
    ON s.bucket = c.bucket
WHERE s.run_label = 'scattered'
  AND c.run_label = 'contiguous'
ORDER BY s.bucket;
"

echo ""
echo "--- Weighted-average latency ---"
psql -t -A -c "
WITH w(bucket, weight) AS (
    VALUES (1, 35638), (2, 165033), (3, 304887), (4, 264177),
           (5, 143765), (6, 59558), (7, 22595), (8, 15252)
)
SELECT
    'Scattered  weighted avg: '
        || round(SUM(s.avg_ms * w.weight) / SUM(w.weight), 2)
        || ' ms'
FROM defrag_bench_results s
JOIN w ON s.bucket = w.bucket
WHERE s.run_label = 'scattered'
UNION ALL
SELECT
    'Contiguous weighted avg: '
        || round(SUM(c.avg_ms * w.weight) / SUM(w.weight), 2)
        || ' ms'
FROM defrag_bench_results c
JOIN w ON c.bucket = w.bucket
WHERE c.run_label = 'contiguous';
"

echo ""
log "Full report saved to: $REPORT_FILE"
log "Benchmark complete."
