#!/bin/bash
#
# replication.sh — physical (streaming) replication smoke test for
# pg_textsearch. Sets up a primary + hot-standby pair via
# pg_basebackup, exercises the index across the basic write/replay
# cycle, and asserts the standby sees the same results as the
# primary.
#
# Tests:
#   1. Standby starts up with replicated bm25 index (basebackup
#      includes the index file).
#   2. BM25 search queries work on the hot standby (read-side
#      smoke test).
#   3. Primary inserts replicate via WAL and a primary-side spill
#      reaches the standby (exercises the #342 fix in this PR via
#      the memtable-spill path).
#   4. Standby can be promoted; index still works on the new
#      primary.
#   5. Long-lived standby backend sees primary inserts that arrive
#      via WAL replay during the backend's lifetime. Currently
#      FAILS — see #345 (per-backend cache not invalidated by WAL
#      replay).
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55436
STANDBY_PORT=55437
TEST_DB=replication_test
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_standby"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

# ---------------------------------------------------------------
# Test 1: Build index on primary, then create standby, query it
# ---------------------------------------------------------------
test_basic_standby_queries() {
    log "=== Test 1: Basic standby queries ==="

    log "Creating table and index on primary..."
    primary_sql "
        CREATE TABLE docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
    " >/dev/null

    primary_sql "
        INSERT INTO docs (content) VALUES
            ('database systems and query optimization'),
            ('full text search algorithms'),
            ('postgresql extension development guide'),
            ('bm25 ranking algorithm implementation'),
            ('distributed systems architecture'),
            ('machine learning algorithms overview'),
            ('natural language processing methods'),
            ('search engine optimization techniques'),
            ('cloud computing infrastructure'),
            ('microservices design patterns');
    " >/dev/null

    primary_sql "
        CREATE INDEX docs_bm25_idx ON docs USING bm25(content)
            WITH (text_config='english');
    "

    # Verify search works on primary
    local primary_count
    primary_count=$(search_count "${PRIMARY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Primary search for 'algorithms': ${primary_count} results"
    if [ -z "$primary_count" ] || [ "$primary_count" -lt 1 ]; then
        error "Primary search returned no results!"
    fi

    # Now create the standby (after data is loaded, avoids slow
    # checkpoint during basebackup of empty cluster)
    setup_standby
    wait_for_standby_catchup

    log "Running search on standby..."
    local standby_result
    standby_result=$(standby_sql "
        SELECT id, content <@> to_bm25query('algorithms',
            'docs_bm25_idx') AS score
        FROM docs
        WHERE content <@> to_bm25query('algorithms',
            'docs_bm25_idx') < 0
        ORDER BY content <@> to_bm25query('algorithms',
            'docs_bm25_idx')
        LIMIT 10;
    ")
    echo "$standby_result"

    local standby_count
    standby_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Standby search for 'algorithms': ${standby_count} results"

    if [ -z "$standby_count" ] || [ "$standby_count" -lt 1 ]; then
        error "Standby search returned no results!"
    fi

    if [ "$standby_count" != "$primary_count" ]; then
        error "Result count mismatch: primary=${primary_count} \
standby=${standby_count}"
    fi

    log "Test 1 PASSED: Standby queries match primary"
}

# ---------------------------------------------------------------
# Test 2: Insert on primary, verify replication to standby
# ---------------------------------------------------------------
test_ongoing_replication() {
    log "=== Test 2: Ongoing replication ==="

    log "Inserting new documents on primary..."
    primary_sql "
        INSERT INTO docs (content) VALUES
            ('advanced algorithms for sorting and searching'),
            ('graph algorithms and network analysis'),
            ('parallel algorithms on modern hardware');
    " >/dev/null

    wait_for_standby_catchup

    log "Searching on standby after new inserts..."
    local standby_count
    standby_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Standby search for 'algorithms' after inserts: \
${standby_count} results"

    # Should find more than before (original 2 + 3 new = 5)
    if [ "$standby_count" -lt 4 ]; then
        error "Expected at least 4 results, got ${standby_count}"
    fi

    # Verify counts match primary
    local primary_count
    primary_count=$(search_count "${PRIMARY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")

    if [ "$standby_count" != "$primary_count" ]; then
        error "Result count mismatch after inserts: \
primary=${primary_count} standby=${standby_count}"
    fi

    log "Test 2 PASSED: Ongoing replication works"
}

# ---------------------------------------------------------------
# Test 3: Spill to segments on primary, verify on standby
# ---------------------------------------------------------------
test_segment_replication() {
    log "=== Test 3: Segment replication ==="

    log "Forcing memtable spill on primary..."
    primary_sql "SELECT bm25_spill_index('docs_bm25_idx');"

    wait_for_standby_catchup

    log "Searching on standby after segment spill..."
    local standby_count
    standby_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Standby search after spill: ${standby_count} results"

    local primary_count
    primary_count=$(search_count "${PRIMARY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")

    if [ "$standby_count" != "$primary_count" ]; then
        error "Result mismatch after spill: \
primary=${primary_count} standby=${standby_count}"
    fi

    log "Test 3 PASSED: Segment replication works"
}

# ---------------------------------------------------------------
# Test 4: Promote standby and verify read/write functionality
# ---------------------------------------------------------------
test_standby_promotion() {
    log "=== Test 4: Standby promotion ==="

    # Capture pre-promotion search state
    local pre_count
    pre_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Pre-promotion search count: ${pre_count}"

    log "Promoting standby to primary..."
    pg_ctl promote -D "${STANDBY_DIR}" -w

    # Wait for promotion to complete
    local i=0
    while [ $i -lt 10 ]; do
        local in_recovery
        in_recovery=$(psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA \
            -c "SELECT pg_is_in_recovery();" 2>/dev/null)
        if [ "$in_recovery" = "f" ]; then
            break
        fi
        sleep 1
        i=$((i + 1))
    done

    local in_recovery
    in_recovery=$(psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA \
        -c "SELECT pg_is_in_recovery();" 2>/dev/null)
    if [ "$in_recovery" != "f" ]; then
        error "Standby did not complete promotion!"
    fi
    log "Standby promoted successfully"

    # Test read queries on promoted standby
    log "Testing search on promoted standby..."
    local promoted_count
    promoted_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Search results on promoted standby: ${promoted_count}"

    if [ -z "$promoted_count" ] || [ "$promoted_count" -lt 1 ]; then
        error "Search failed on promoted standby!"
    fi

    # Test write on promoted standby
    log "Testing insert on promoted standby..."
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -c "
        INSERT INTO docs (content) VALUES
            ('post-promotion test document about algorithms');
    " >/dev/null 2>&1

    local post_insert_count
    post_insert_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Search after insert on promoted standby: \
${post_insert_count}"

    if [ "$post_insert_count" -le "$promoted_count" ]; then
        error "Insert on promoted standby not reflected in search \
(before: ${promoted_count}, after: ${post_insert_count})"
    fi

    log "Test 4 PASSED: Standby promotion works"
}

# ---------------------------------------------------------------
# Test 5: Long-lived standby backend sees new primary inserts
#
# This test exposes the memtable-staleness bug: a backend that
# stays alive across primary inserts rebuilds its memtable once
# (on first scan) and then never updates it because WAL replay
# does not call into our aminsert path.
# ---------------------------------------------------------------
test_long_lived_backend_staleness() {
    log "=== Test 5: Long-lived backend staleness ==="

    primary_sql "
        DROP TABLE IF EXISTS staleness_docs CASCADE;
        CREATE TABLE staleness_docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
        INSERT INTO staleness_docs (content) VALUES
            ('initial document about quantum mechanics'),
            ('initial document about general relativity');
        CREATE INDEX staleness_idx ON staleness_docs USING bm25(content)
            WITH (text_config='english');
    " >/dev/null

    wait_for_standby_catchup

    local query="
        SELECT count(*) FROM (
            SELECT id FROM staleness_docs
            ORDER BY content <@> to_bm25query('quantum',
                'staleness_idx')
            LIMIT 100
        ) t;
    "

    long_lived_before_after "${STANDBY_PORT}" "${query}" \
        "primary_sql \"
            INSERT INTO staleness_docs (content) VALUES
                ('quantum entanglement and bell inequalities');
        \" >/dev/null
        wait_for_standby_catchup"

    log "Initial standby count: ${LL_BEFORE}"
    log "Standby count after primary insert: ${LL_AFTER}"

    if [ "${LL_BEFORE}" != "2" ]; then
        error "Expected initial count=2, got '${LL_BEFORE}'"
    fi

    # The bug manifests in two ways:
    #   1. Silent staleness: LL_AFTER == LL_BEFORE (memtable not updated).
    #   2. Segment-block invalidation: LL_AFTER contains 'ERROR'
    #      (long-lived backend has a stale view of segment storage).
    # Once fixed, LL_AFTER should be LL_BEFORE + 1 (the new doc is visible).
    if [[ "${LL_AFTER}" == *ERROR* ]] || \
       ! [[ "${LL_AFTER}" =~ ^[0-9]+$ ]] || \
       [ "${LL_AFTER}" -le "${LL_BEFORE}" ]; then
        error "BUG (expected): long-lived standby backend did not see \
new primary insert (before=${LL_BEFORE}, after=${LL_AFTER})"
    fi

    log "Test 5 PASSED: Long-lived backend sees new primary inserts"
}

# ---------------------------------------------------------------
# Test 6: Segment graph snapshots vs concurrent WAL replay
# ---------------------------------------------------------------
# Standalone <@> scoring captures the metapage, the memtable chain
# endpoint and every published segment root as one snapshot, then walks
# only the captured block numbers.  On a primary the reader's per-index
# LWLock already excludes spill publication, so the recovery-mode
# capture path is unreachable there.  A standby has no such protection:
# there is no custom rmgr, so WAL replay republishes the graph without
# taking the per-index lock.
#
# This exercises that path -- roughly two thousand recovery-mode
# captures -- against a primary that is spilling and merging, and
# asserts no reader error and no torn or corrupt segment read.  The
# debug pause GUCs widen the capture windows so replay interleaves.
#
# It does not prove the root walk itself is atomic: the pause hooks
# bracket the capture, not the individual level chains, so replay
# cannot be forced to land between two levels from a shell test.
test_standby_snapshot_race() {
    log "=== Test 6: Standby snapshot vs concurrent replay ==="

    primary_sql "
        DROP TABLE IF EXISTS race_docs CASCADE;
        CREATE TABLE race_docs (
            id bigserial PRIMARY KEY,
            content text NOT NULL
        );
        CREATE INDEX race_idx ON race_docs USING bm25(content)
            WITH (text_config='english');
        INSERT INTO race_docs (content)
        SELECT 'postgres bm25 snapshot race ' || gs
        FROM generate_series(1, 30) gs;
        SELECT bm25_spill_index('race_idx');
        INSERT INTO race_docs (content)
        SELECT 'postgres bm25 second batch ' || gs
        FROM generate_series(1, 30) gs;
        SELECT bm25_spill_index('race_idx');
    " >/dev/null

    wait_for_standby_catchup

    local plan
    plan=$(standby_sql "EXPLAIN (COSTS off)
        SELECT content <@> to_bm25query('postgres bm25', 'race_idx')
        FROM race_docs")
    if ! echo "${plan}" | grep -qi 'Seq Scan'; then
        echo "${plan}"
        error "Test 6 SETUP FAILED: reader does not use standalone scoring"
    fi

    local readerlog="${STANDBY_DIR}/snapshot_race_reader.log"
    local standbylog="${STANDBY_DIR}/log/postgres.log"
    : > "${readerlog}"

    (
        for _ in $(seq 1 10); do
            psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -qAt \
                -v ON_ERROR_STOP=1 -c "
                SET enable_indexscan=off;
                SET enable_bitmapscan=off;
                SET statement_timeout='120s';
                SET pg_textsearch.\
debug_segment_graph_snapshot_pause_before_lock_ms=1;
                SET pg_textsearch.\
debug_segment_graph_snapshot_pause_before_unlock_ms=8;
                SET pg_textsearch.debug_segment_graph_snapshot_pause_ms=8;
                SELECT count(score) FROM (
                    SELECT content <@> to_bm25query('postgres bm25',
                        'race_idx') AS score
                    FROM race_docs
                ) s" >>"${readerlog}" 2>&1 || exit 40
        done
    ) &
    local reader_pid=$!

    (
        for _ in $(seq 1 15); do
            psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -qAt -c "
                INSERT INTO race_docs (content)
                SELECT 'racer postgres bm25 ' || gs
                FROM generate_series(1, 10) gs;
                SELECT bm25_spill_index('race_idx');
                SELECT bm25_force_merge('race_idx');
            " >/dev/null 2>&1 || true
        done
    ) &
    local writer_pid=$!

    local failed=0
    wait ${reader_pid} || failed=1
    wait ${writer_pid} || true

    local torn='shorter than its recorded count'
    torn="${torn}|longer than its recorded count"
    torn="${torn}|could not read BM25 segment root"
    torn="${torn}|segment-root count overflow"
    torn="${torn}|invalid segment header"

    if grep -IEq "${torn}" "${readerlog}" || \
       grep -IEq "${torn}" "${standbylog}"; then
        grep -IEn "${torn}" "${readerlog}" "${standbylog}" | head -5
        error "Test 6 FAILED: standby snapshot torn by concurrent replay"
    fi
    if [ "${failed}" -ne 0 ]; then
        tail -n 10 "${readerlog}"
        error "Test 6 FAILED: standby reader exited with an error"
    fi

    # Prove the run actually opened the window it claims to test.
    if ! grep -q "segment graph snapshot pause at before-unlock" \
         "${standbylog}"; then
        error "Test 6 FAILED: the capture window never opened on the standby"
    fi

    log "Test 6 PASSED: standby snapshots survived concurrent replay"
}

main() {
    log "Starting pg_textsearch physical replication test..."

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"
    command -v pg_basebackup >/dev/null 2>&1 || \
        error "pg_basebackup not found"
    command -v initdb >/dev/null 2>&1 || error "initdb not found"

    setup_primary
    test_basic_standby_queries
    test_ongoing_replication
    test_segment_replication
    test_long_lived_backend_staleness
    test_standby_snapshot_race
    test_standby_promotion

    log "All physical replication tests passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
