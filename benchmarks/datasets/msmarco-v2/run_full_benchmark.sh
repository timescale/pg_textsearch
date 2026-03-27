#!/bin/bash
# MS MARCO v2 Full Benchmark — pg_textsearch vs ParadeDB (ParadeDB)
#
# Reproducible benchmark for blog-post results on 138M passages.
#
# Prerequisites:
#   - download.sh has been run (collection.tsv exists)
#   - generate_benchmark_queries.sh has been run (benchmark_queries.tsv)
#   - pg_textsearch is built and installed (make && sudo make install)
#   - ParadeDB .deb is installed
#   - msmarco_v2_passages table is already loaded (load.sql or
#     load_data_only.sql)
#
# Usage: ./run_full_benchmark.sh <step>
#   Steps:
#     env            - capture machine specs, PG config, extensions
#     build-tapir    - build pg_textsearch BM25 index
#     build-paradedb  - copy data + build ParadeDB BM25 index
#     query-tapir    - single-client latency benchmarks
#     query-paradedb  - single-client latency benchmarks
#     power-tapir    - concurrent throughput (pgbench)
#     power-paradedb  - concurrent throughput (pgbench)
#     summary        - side-by-side comparison table
#     all            - run everything in sequence

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────
PARALLEL_WORKERS=14
MAINT_WORK_MEM='2GB'
MIN_PARALLEL_SCAN='64kB'
POWER_CLIENTS=16
POWER_DURATION=60
POWER_WARMUP=5
# ─────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="$SCRIPT_DIR/results/$TIMESTAMP"
STEP="${1:-all}"

PGPORT="${PGPORT:-5432}"
PGDATABASE="${PGDATABASE:-postgres}"
export PGPORT PGDATABASE DATA_DIR

mkdir -p "$RESULTS_DIR"

log() { echo "$(date +%H:%M:%S) $*"; }

# ── Prerequisites ────────────────────────────────────────────────
check_prereqs() {
    if [ ! -f "$DATA_DIR/collection.tsv" ]; then
        echo "ERROR: collection.tsv not found. Run ./download.sh first."
        exit 1
    fi
    if [ ! -f "$SCRIPT_DIR/benchmark_queries.tsv" ]; then
        echo "ERROR: benchmark_queries.tsv not found."
        exit 1
    fi
    log "Prerequisites OK"
    log "  Collection: $(wc -l < "$DATA_DIR/collection.tsv") passages"
    log "  Benchmark queries: $(wc -l < "$SCRIPT_DIR/benchmark_queries.tsv")"
}

# ── step: env ────────────────────────────────────────────────────
step_env() {
    local out="$RESULTS_DIR/env.log"
    log "Capturing environment → $out"
    {
        echo "=== Machine ==="
        echo "Date:     $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "Hostname: $(hostname)"
        echo "CPU:      $(nproc) cores"
        lscpu | grep -E 'Model name|Socket|Core|Thread|MHz' || true
        echo "RAM:      $(free -h | awk '/^Mem:/{print $2}')"
        echo "Disk:"
        df -h /nvme 2>/dev/null || df -h /
        echo ""

        echo "=== Postgres ==="
        psql -c "SELECT version();"
        psql -c "SHOW data_directory;"
        psql -c "SHOW shared_buffers;"
        psql -c "SHOW work_mem;"
        psql -c "SHOW maintenance_work_mem;"
        psql -c "SHOW max_parallel_maintenance_workers;"
        psql -c "SHOW max_parallel_workers_per_gather;"
        psql -c "SHOW max_parallel_workers;"
        psql -c "SHOW shared_preload_libraries;"
        echo ""

        echo "=== Extensions ==="
        psql -c "SELECT name, default_version, installed_version
                  FROM pg_available_extensions
                  WHERE name IN ('pg_textsearch','pg_search')
                  ORDER BY name;"
        echo ""

        echo "=== Dataset ==="
        psql -t -c "SELECT 'Passages: ' || COUNT(*) FROM msmarco_v2_passages;"
        psql -t -c "SELECT 'Table size: '
            || pg_size_pretty(pg_total_relation_size('msmarco_v2_passages'));"
        echo ""

        echo "=== Benchmark Config ==="
        echo "PARALLEL_WORKERS=$PARALLEL_WORKERS"
        echo "MAINT_WORK_MEM=$MAINT_WORK_MEM"
        echo "MIN_PARALLEL_SCAN=$MIN_PARALLEL_SCAN"
        echo "POWER_CLIENTS=$POWER_CLIENTS"
        echo "POWER_DURATION=${POWER_DURATION}s"
    } 2>&1 | tee "$out"
}

# ── step: build-tapir ────────────────────────────────────────────
step_build_tapir() {
    local out="$RESULTS_DIR/build_tapir.log"
    log "Building pg_textsearch index → $out"

    # Drop pg_search to avoid bm25 AM name collision
    psql -c "DROP EXTENSION IF EXISTS pg_search CASCADE;" 2>/dev/null || true

    # Drop page cache for cold-start fairness
    if [ -w /proc/sys/vm/drop_caches ]; then
        sync
        echo 3 > /proc/sys/vm/drop_caches
        log "Page cache dropped"
    else
        log "WARNING: cannot drop page cache (not root)"
    fi

    psql -v ON_ERROR_STOP=1 <<EOSQL 2>&1 | tee "$out"
\timing on

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET max_parallel_maintenance_workers = $PARALLEL_WORKERS;
SET maintenance_work_mem = '$MAINT_WORK_MEM';
SET min_parallel_table_scan_size = '$MIN_PARALLEL_SCAN';

DROP INDEX IF EXISTS msmarco_v2_bm25_idx;

\echo ''
\echo '=== Building pg_textsearch BM25 Index ==='
\echo 'parallel_workers=$PARALLEL_WORKERS  maint_work_mem=$MAINT_WORK_MEM  min_parallel_scan=$MIN_PARALLEL_SCAN'
CREATE INDEX msmarco_v2_bm25_idx ON msmarco_v2_passages
    USING bm25(passage_text) WITH (text_config='english');

\echo ''
\echo '=== Sizes ==='
SELECT 'INDEX_SIZE_TAPIR: '
    || pg_size_pretty(pg_relation_size('msmarco_v2_bm25_idx'))
    || ' (' || pg_relation_size('msmarco_v2_bm25_idx') || ' bytes)'
    as result;
SELECT 'TABLE_SIZE: '
    || pg_size_pretty(pg_total_relation_size('msmarco_v2_passages'))
    as result;

\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('msmarco_v2_bm25_idx');

\echo '=== build-tapir complete ==='
EOSQL
    log "build-tapir done"
}

# ── step: build-paradedb ─────────────────────────────────────────
step_build_paradedb() {
    local out="$RESULTS_DIR/build_paradedb.log"
    log "Building ParadeDB index → $out"

    # Free disk: drop tapir index before copying data
    log "Dropping tapir index to free disk space..."
    psql -c "DROP INDEX IF EXISTS msmarco_v2_bm25_idx;" 2>/dev/null || true

    # Drop pg_textsearch to avoid bm25 AM name collision with pg_search
    log "Dropping pg_textsearch extension (bm25 AM conflict)..."
    psql -c "DROP EXTENSION IF EXISTS pg_textsearch CASCADE;" 2>/dev/null || true

    # Drop page cache for cold-start fairness
    if [ -w /proc/sys/vm/drop_caches ]; then
        sync
        echo 3 > /proc/sys/vm/drop_caches
        log "Page cache dropped"
    fi

    psql -v ON_ERROR_STOP=1 <<EOSQL 2>&1 | tee "$out"
\timing on

-- Ensure pg_search extension is available (pg_textsearch already dropped)
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Apply same parallel GUCs for fairness
SET max_parallel_maintenance_workers = $PARALLEL_WORKERS;
SET maintenance_work_mem = '$MAINT_WORK_MEM';
SET min_parallel_table_scan_size = '$MIN_PARALLEL_SCAN';

DROP INDEX IF EXISTS msmarco_v2_paradedb_idx;

\echo ''
\echo '=== Building ParadeDB BM25 Index ==='
\echo 'parallel_workers=$PARALLEL_WORKERS  maint_work_mem=$MAINT_WORK_MEM  min_parallel_scan=$MIN_PARALLEL_SCAN'
CREATE INDEX msmarco_v2_paradedb_idx ON msmarco_v2_passages
    USING bm25 (passage_id, passage_text)
    WITH (
        key_field = 'passage_id',
        text_fields = '{
            "passage_text": {
                "tokenizer": {
                    "type": "default",
                    "stopwords_language": "English",
                    "stemmer": "English"
                }
            }
        }'
    );

\echo 'Running VACUUM to compact segments...'
VACUUM msmarco_v2_passages;

\echo ''
\echo '=== Sizes ==='
SELECT 'INDEX_SIZE_SYSTEMX: '
    || pg_size_pretty(pg_relation_size('msmarco_v2_paradedb_idx'))
    || ' (' || pg_relation_size('msmarco_v2_paradedb_idx') || ' bytes)'
    as result;
SELECT 'TABLE_SIZE_SYSTEMX: '
    || pg_size_pretty(pg_total_relation_size('msmarco_v2_passages'))
    as result;

\echo '=== build-paradedb complete ==='
EOSQL
    log "build-paradedb done"
}

# ── step: query-tapir ────────────────────────────────────────────
step_query_tapir() {
    local out="$RESULTS_DIR/query_tapir.log"
    log "Running pg_textsearch query benchmarks → $out"

    cd "$REPO_DIR"
    psql -f benchmarks/datasets/msmarco-v2/queries.sql 2>&1 | tee "$out"

    log "query-tapir done"
    echo ""
    echo "=== Key Results ==="
    grep -E "LATENCY_BUCKET|WEIGHTED_|THROUGHPUT_RESULT" "$out" || true
}

# ── step: query-paradedb ─────────────────────────────────────────
step_query_paradedb() {
    local out="$RESULTS_DIR/query_paradedb.log"
    log "Running ParadeDB query benchmarks → $out"

    cd "$REPO_DIR"
    # Rewrite paradedb queries to use msmarco_v2_passages (same table)
    sed 's/msmarco_v2_passages_paradedb/msmarco_v2_passages/g' \
        benchmarks/datasets/msmarco-v2/paradedb/queries.sql \
        | psql 2>&1 | tee "$out"

    log "query-paradedb done"
    echo ""
    echo "=== Key Results ==="
    grep -E "LATENCY_BUCKET|WEIGHTED_|THROUGHPUT_RESULT" "$out" || true
}

# ── step: power-tapir ────────────────────────────────────────────
step_power_tapir() {
    local out="$RESULTS_DIR/power_tapir.log"
    log "Running pg_textsearch power test → $out"
    log "  clients=$POWER_CLIENTS  duration=${POWER_DURATION}s"

    # Set up the power-test query table
    psql -v ON_ERROR_STOP=1 \
        -f "$SCRIPT_DIR/setup_power_test.sql" 2>&1 \
        | tee "$RESULTS_DIR/setup_power_test.log"

    pgbench -n \
        -c "$POWER_CLIENTS" \
        -j "$POWER_CLIENTS" \
        -T "$POWER_DURATION" \
        -M prepared \
        -f "$SCRIPT_DIR/power_tapir.sql" \
        2>&1 | tee "$out"

    local tps
    tps=$(grep -oP 'tps = \K[0-9.]+' "$out" | tail -1)
    echo "POWER_TPS_TAPIR: $tps" | tee -a "$out"

    log "power-tapir done"
}

# ── step: power-paradedb ─────────────────────────────────────────
step_power_paradedb() {
    local out="$RESULTS_DIR/power_paradedb.log"
    log "Running ParadeDB power test → $out"
    log "  clients=$POWER_CLIENTS  duration=${POWER_DURATION}s"

    # Set up the power-test query table (idempotent)
    psql -v ON_ERROR_STOP=1 \
        -f "$SCRIPT_DIR/setup_power_test.sql" 2>&1 \
        | tee "$RESULTS_DIR/setup_power_test.log"

    pgbench -n \
        -c "$POWER_CLIENTS" \
        -j "$POWER_CLIENTS" \
        -T "$POWER_DURATION" \
        -M prepared \
        -f "$SCRIPT_DIR/power_paradedb.sql" \
        2>&1 | tee "$out"

    local tps
    tps=$(grep -oP 'tps = \K[0-9.]+' "$out" | tail -1)
    echo "POWER_TPS_SYSTEMX: $tps" | tee -a "$out"

    log "power-paradedb done"
}

# ── step: summary ────────────────────────────────────────────────
step_summary() {
    local out="$RESULTS_DIR/summary.log"
    log "Generating summary → $out"

    # Find the latest result files (use explicit dir or latest)
    local rdir="$RESULTS_DIR"

    # If query logs don't exist in this run's dir, find the latest
    if [ ! -f "$rdir/query_tapir.log" ]; then
        rdir=$(ls -dt "$SCRIPT_DIR/results"/*/ 2>/dev/null | head -1)
        if [ -z "$rdir" ]; then
            echo "ERROR: no result logs found"
            exit 1
        fi
        log "Using results from: $rdir"
    fi

    {
        echo "=============================================="
        echo "  MS-MARCO v2 Benchmark Summary"
        echo "  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "  Results dir: $rdir"
        echo "=============================================="
        echo ""

        # --- Build Times ---
        echo "=== Index Build Time ==="
        local bt_tapir bt_paradedb
        bt_tapir=$(grep -oP 'CREATE INDEX.*Time: \K[0-9.]+' \
            "$rdir/build_tapir.log" 2>/dev/null || echo "N/A")
        bt_paradedb=$(grep -oP 'CREATE INDEX.*Time: \K[0-9.]+' \
            "$rdir/build_paradedb.log" 2>/dev/null || echo "N/A")
        printf "  %-20s %s\n" "pg_textsearch:" "${bt_tapir} ms"
        printf "  %-20s %s\n" "ParadeDB:" "${bt_paradedb} ms"
        echo ""

        # --- Index Sizes ---
        echo "=== Index Size ==="
        grep "INDEX_SIZE_TAPIR:" "$rdir/build_tapir.log" 2>/dev/null \
            | sed 's/^/  /' || true
        grep "INDEX_SIZE_SYSTEMX:" "$rdir/build_paradedb.log" 2>/dev/null \
            | sed 's/^/  /' || true
        echo ""

        # --- Single-Client Latency ---
        echo "=== Single-Client Query Latency ==="
        echo "  --- pg_textsearch ---"
        grep -E "LATENCY_BUCKET" "$rdir/query_tapir.log" 2>/dev/null \
            | sed 's/^/  /' || echo "  (not available)"
        echo ""
        grep -E "WEIGHTED_LATENCY" "$rdir/query_tapir.log" 2>/dev/null \
            | sed 's/^/  /' || true
        echo ""
        echo "  --- ParadeDB ---"
        grep -E "LATENCY_BUCKET" "$rdir/query_paradedb.log" 2>/dev/null \
            | sed 's/^/  /' || echo "  (not available)"
        echo ""
        grep -E "WEIGHTED_LATENCY" "$rdir/query_paradedb.log" 2>/dev/null \
            | sed 's/^/  /' || true
        echo ""

        # --- Throughput (sequential) ---
        echo "=== Sequential Throughput ==="
        grep "THROUGHPUT_RESULT:" "$rdir/query_tapir.log" 2>/dev/null \
            | sed 's/^/  pg_textsearch  /' || true
        grep "THROUGHPUT_RESULT:" "$rdir/query_paradedb.log" 2>/dev/null \
            | sed 's/^/  ParadeDB       /' || true
        echo ""

        # --- Power Test (concurrent) ---
        echo "=== Concurrent Throughput (pgbench, ${POWER_CLIENTS} clients) ==="
        grep "POWER_TPS_TAPIR:" "$rdir/power_tapir.log" 2>/dev/null \
            | sed 's/^/  /' || echo "  (not available)"
        grep "POWER_TPS_SYSTEMX:" "$rdir/power_paradedb.log" 2>/dev/null \
            | sed 's/^/  /' || echo "  (not available)"
        echo ""

        echo "=============================================="
    } 2>&1 | tee "$out"
}

# ── Main ─────────────────────────────────────────────────────────
echo "=== MS-MARCO v2 Full Benchmark ==="
echo "Timestamp: $TIMESTAMP"
echo "Step:      $STEP"
echo "Results:   $RESULTS_DIR"
echo ""

check_prereqs

case "$STEP" in
    env)            step_env ;;
    build-tapir)    step_env; step_build_tapir ;;
    build-paradedb)  step_env; step_build_paradedb ;;
    query-tapir)    step_query_tapir ;;
    query-paradedb)  step_query_paradedb ;;
    power-tapir)    step_power_tapir ;;
    power-paradedb)  step_power_paradedb ;;
    summary)        step_summary ;;
    all)
        step_env
        step_build_tapir
        step_query_tapir
        step_power_tapir
        step_build_paradedb
        step_query_paradedb
        step_power_paradedb
        step_summary
        ;;
    *)
        echo "Unknown step: $STEP"
        echo "Steps: env build-tapir build-paradedb query-tapir" \
             "query-paradedb power-tapir power-paradedb summary all"
        exit 1
        ;;
esac

echo ""
log "Done. Results in $RESULTS_DIR"
