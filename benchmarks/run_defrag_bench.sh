#!/bin/bash
#
# Defragmentation benchmark: measures force_merge overhead and query
# latency improvement from contiguous segment layout.
#
# Measures:
#   1. Force merge wall time WITH vs WITHOUT defragmentation
#   2. Query latency: scattered segments → merged (scattered) → merged (contiguous)
#   3. Cold-cache query latency comparison
#
# Usage:
#   ./benchmarks/run_defrag_bench.sh [NUM_DOCS]
#
# NUM_DOCS defaults to 500000.

set -e

NUM_DOCS=${1:-500000}
BATCH_SIZE=10000
QUERY_RUNS=5

export PGPORT=${PGPORT:-5432}
export PGHOST=${PGHOST:-localhost}
export PGUSER=${PGUSER:-$(whoami)}
export PGDATABASE=${PGDATABASE:-postgres}

now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }

header() {
    echo ""
    echo "================================================================"
    echo "  $1"
    echo "================================================================"
}

run_query_bench() {
    local label="$1"
    echo ""
    echo "--- $label: query latency ($QUERY_RUNS runs each) ---"
    echo ""
    printf "%-12s" "Term"
    for r in $(seq 1 $QUERY_RUNS); do printf "%10s" "Run$r"; done
    echo ""
    echo "---------------------------------------------------------------"

    for TERM in word1 term50 doc100 word999; do
        printf "%-12s" "$TERM"
        for r in $(seq 1 $QUERY_RUNS); do
            local t
            t=$(psql -t -A -c "
                EXPLAIN (ANALYZE, COSTS OFF, TIMING ON)
                SELECT id FROM defrag_bench
                ORDER BY body <@> to_bm25query('$TERM',
                    'defrag_bench_idx')
                LIMIT 10;
            " 2>&1 | grep "Execution Time" | \
                sed 's/.*: \([0-9.]*\) ms/\1/')
            printf "%10s" "${t}ms"
        done
        echo ""
    done
}

build_scattered_index() {
    psql -q <<'SQL'
DROP TABLE IF EXISTS defrag_bench CASCADE;
CREATE TABLE defrag_bench (
    id   SERIAL PRIMARY KEY,
    body TEXT
);
SQL

    psql -q -c "
    CREATE INDEX defrag_bench_idx ON defrag_bench USING bm25(body)
      WITH (text_config='english');
    "

    local INSERTED=0
    local SPILL_COUNT=0
    while [ "$INSERTED" -lt "$NUM_DOCS" ]; do
        local REMAINING=$((NUM_DOCS - INSERTED))
        local THIS_BATCH=$((REMAINING < BATCH_SIZE ? REMAINING : BATCH_SIZE))

        psql -q -c "
        INSERT INTO defrag_bench (body)
        SELECT 'word' || (g % 1000)::text || ' '
            || 'term' || (g % 500)::text || ' '
            || 'doc'  || g::text || ' '
            || 'extra' || (g % 200)::text || ' '
            || 'fill'  || (g % 100)::text
        FROM generate_series(1, $THIS_BATCH) g;
        "

        psql -q -c "SELECT bm25_spill_index('defrag_bench_idx');" >/dev/null
        SPILL_COUNT=$((SPILL_COUNT + 1))
        INSERTED=$((INSERTED + THIS_BATCH))
        printf "\r  %d / %d docs inserted (%d spills)" \
            "$INSERTED" "$NUM_DOCS" "$SPILL_COUNT"
    done
    echo ""
    echo "Insert complete: $INSERTED docs, $SPILL_COUNT spills."
}

header "Defrag Benchmark"
echo "Documents:      $NUM_DOCS"
echo "Batch size:     $BATCH_SIZE"
echo "Query runs:     $QUERY_RUNS"
echo "shared_buffers: $(psql -t -A -c 'SHOW shared_buffers;')"
echo "PG version:     $(psql -t -A -c 'SHOW server_version;')"
echo "Started:        $(date)"

psql -q -c "CREATE EXTENSION IF NOT EXISTS pg_textsearch;" 2>/dev/null

# =================================================================
# Part A: Force merge time — WITHOUT defrag (baseline)
# =================================================================

header "Part A: Force merge WITHOUT defrag"

echo "Building scattered index..."
build_scattered_index

echo ""
echo "--- Pre-merge index summary ---"
psql -t -A -c "
SELECT pg_size_pretty(pg_relation_size('defrag_bench_idx'));
"

echo ""
echo "Running bm25_force_merge (defrag OFF)..."
START_MS=$(now_ms)
psql -q -c "
SET pg_textsearch.defrag_on_merge = false;
SELECT bm25_force_merge('defrag_bench_idx');
"
END_MS=$(now_ms)
MERGE_NO_DEFRAG=$((END_MS - START_MS))
echo "Force merge (no defrag): ${MERGE_NO_DEFRAG} ms"

echo ""
echo "--- Post-merge index summary (scattered single segment) ---"
psql -c "SELECT bm25_summarize_index('defrag_bench_idx');"

run_query_bench "Merged, scattered pages"

# Record index size after non-defrag merge
SIZE_NO_DEFRAG=$(psql -t -A -c "
SELECT pg_size_pretty(pg_relation_size('defrag_bench_idx'));
")

psql -q -c "DROP TABLE defrag_bench CASCADE;"

# =================================================================
# Part B: Force merge time — WITH defrag
# =================================================================

header "Part B: Force merge WITH defrag"

echo "Building scattered index (identical workload)..."
build_scattered_index

echo ""
echo "--- Pre-merge index summary ---"
psql -t -A -c "
SELECT pg_size_pretty(pg_relation_size('defrag_bench_idx'));
"

echo ""
echo "Running bm25_force_merge (defrag ON)..."
START_MS=$(now_ms)
psql -q -c "
SET pg_textsearch.defrag_on_merge = true;
SELECT bm25_force_merge('defrag_bench_idx');
"
END_MS=$(now_ms)
MERGE_DEFRAG=$((END_MS - START_MS))
echo "Force merge (defrag): ${MERGE_DEFRAG} ms"

echo ""
echo "--- Post-merge index summary (contiguous single segment) ---"
psql -c "SELECT bm25_summarize_index('defrag_bench_idx');"

run_query_bench "Merged, contiguous pages"

SIZE_DEFRAG=$(psql -t -A -c "
SELECT pg_size_pretty(pg_relation_size('defrag_bench_idx'));
")

# =================================================================
# Part C: Cold-cache query comparison
# =================================================================

header "Part C: Cold-cache queries (contiguous segment)"

echo "Restarting PostgreSQL to clear shared_buffers..."
brew services restart postgresql@17 2>/dev/null || pg_ctl restart -D "$PGDATA" 2>/dev/null || true
sleep 3

echo ""
echo "--- Cold-cache: contiguous segment ---"
for TERM in word1 term50 doc100 word999; do
    t=$(psql -t -A -c "
        EXPLAIN (ANALYZE, COSTS OFF, TIMING ON)
        SELECT id FROM defrag_bench
        ORDER BY body <@> to_bm25query('$TERM', 'defrag_bench_idx')
        LIMIT 10;
    " 2>&1 | grep "Execution Time" | \
        sed 's/.*: \([0-9.]*\) ms/\1/')
    printf "  %-12s %sms\n" "$TERM" "$t"
done

# =================================================================
# Summary
# =================================================================

header "Summary"

echo ""
echo "Force merge time:"
echo "  Without defrag:  ${MERGE_NO_DEFRAG} ms"
echo "  With defrag:     ${MERGE_DEFRAG} ms"
if [ "$MERGE_NO_DEFRAG" -gt 0 ]; then
    OVERHEAD=$((MERGE_DEFRAG - MERGE_NO_DEFRAG))
    PCT=$(python3 -c "print(f'{($MERGE_DEFRAG - $MERGE_NO_DEFRAG) / $MERGE_NO_DEFRAG * 100:.1f}')")
    echo "  Overhead:        ${OVERHEAD} ms (+${PCT}%)"
fi
echo ""
echo "Index size after merge:"
echo "  Without defrag:  ${SIZE_NO_DEFRAG}"
echo "  With defrag:     ${SIZE_DEFRAG}"
echo ""

# Cleanup
psql -q -c "DROP TABLE defrag_bench CASCADE;"
echo "Benchmark complete at $(date)"
