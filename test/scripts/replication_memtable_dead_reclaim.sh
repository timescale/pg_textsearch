#!/bin/bash
#
# replication_memtable_dead_reclaim.sh — physical replication coverage
# for memtable spill DEAD stamping (issue #386).
#
#   1. GenericXLog DEAD + dead_fxid stamps replicate to the standby.
#   2. Standby can query the index after spill (dead orphans are not
#      reachable from the metapage).
#   3. Primary VACUUM reclaims DEAD pages locally (FSM not WAL-logged).
#   4. Standby still lists the same dead pages until it runs local
#      reclaim; after promote + VACUUM, reclaim works there too.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55462
STANDBY_PORT=55463
TEST_DB=replication_memtable_dead
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_memtable_dead_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_memtable_dead_standby"
IDX=dead_spill_idx
SPILL_DEAD_COUNT=0
SPILL_MAIN_SIZE_PRIMARY=0
SPILL_MAIN_SIZE_STANDBY=0

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

dead_page_count() {
    node_sql_quiet "$1" \
        "SELECT count(*)::int FROM bm25_memtable_dead_pages('${IDX}');"
}

dead_flags_ok() {
    node_sql_quiet "$1" "
        SELECT bool_and((flags & 1) <> 0)
               AND bool_and(dead_fxid > 0)
        FROM bm25_memtable_dead_pages('${IDX}');"
}

chain_page_count() {
    node_sql_quiet "$1" \
        "SELECT count(*)::int FROM bm25_memtable_chain('${IDX}');"
}

index_main_size() {
    node_sql_quiet "$1" "
        SELECT pg_relation_size('${IDX}'::regclass, 'main')::bigint;"
}

# DEAD flags stay on disk until a block is reallocated; prove reclaim
# via main-fork growth on a second chain build (memtable_reclaim.sql).
verify_fsm_reuse() {
    local port=$1
    local dead=$2
    local size_after_spill=$3

    node_sql_quiet "${port}" \
        "SET pg_textsearch.memtable_pages_threshold = 0;" >/dev/null
    node_sql_quiet "${port}" "
        INSERT INTO dead_spill_t (body)
        SELECT 'reuse ' || i || ' ' || repeat('pad ', 6)
        FROM generate_series(1, 200) i;" >/dev/null

    local size_after
    size_after=$(index_main_size "${port}")
    local growth=$((size_after - size_after_spill))
    local bs budget
    bs=$(node_sql_quiet "${port}" \
        "SELECT current_setting('block_size')::int;")
    budget=$((dead * bs))

    log "  main fork growth after re-insert: ${growth} bytes \
(budget ${budget} = ${dead} dead pages)"
    if [ "${growth}" -ge "${budget}" ]; then
        return 1
    fi
    return 0
}

test_dead_stamp_replicates_to_standby() {
    log "=== Test 1: DEAD stamps replicate via GenericXLog ==="

    primary_sql "
        DROP TABLE IF EXISTS dead_spill_t CASCADE;
        CREATE TABLE dead_spill_t (id serial PRIMARY KEY, body text);
        CREATE INDEX ${IDX} ON dead_spill_t USING bm25(body)
            WITH (text_config = 'english');
    " >/dev/null

    wait_for_standby_catchup

    primary_sql "
        INSERT INTO dead_spill_t (body)
        SELECT 'dead repl ' || i || ' ' || repeat('pad ', 6)
        FROM generate_series(1, 200) i;
    " >/dev/null
    wait_for_standby_catchup

    local live_pages
    live_pages=$(chain_page_count "${PRIMARY_PORT}")
    log "  primary live chain pages before spill: ${live_pages}"
    if [ "${live_pages:-0}" -le 1 ]; then
        error "Test 1: expected multi-page memtable chain, got ${live_pages}"
    fi

    primary_sql "SELECT bm25_spill_index('${IDX}');" >/dev/null
    primary_sql "CHECKPOINT;" >/dev/null
    wait_for_standby_catchup

    local primary_dead standby_dead
    primary_dead=$(dead_page_count "${PRIMARY_PORT}")
    standby_dead=$(dead_page_count "${STANDBY_PORT}")
    log "  primary dead pages: ${primary_dead} (expect ${live_pages})"
    log "  standby dead pages: ${standby_dead} (expect ${live_pages})"

    if [ "${primary_dead}" != "${live_pages}" ]; then
        error "Test 1: primary dead count ${primary_dead}, expected ${live_pages}"
    fi
    if [ "${standby_dead}" != "${live_pages}" ]; then
        error "Test 1: standby dead count ${standby_dead}, expected ${live_pages}. \
DEAD GenericXLog did not replicate."
    fi

    local primary_flags standby_flags
    primary_flags=$(dead_flags_ok "${PRIMARY_PORT}")
    standby_flags=$(dead_flags_ok "${STANDBY_PORT}")
    if [ "${primary_flags}" != "t" ] || [ "${standby_flags}" != "t" ]; then
        error "Test 1: DEAD flag or dead_fxid missing (primary=${primary_flags}, \
standby=${standby_flags})"
    fi

    local live_after
    live_after=$(chain_page_count "${STANDBY_PORT}")
    if [ "${live_after}" != "0" ]; then
        error "Test 1: standby live chain after spill should be 0, got ${live_after}"
    fi

    SPILL_DEAD_COUNT="${primary_dead}"
    SPILL_MAIN_SIZE_PRIMARY=$(index_main_size "${PRIMARY_PORT}")
    SPILL_MAIN_SIZE_STANDBY=$(index_main_size "${STANDBY_PORT}")
    export SPILL_DEAD_COUNT SPILL_MAIN_SIZE_PRIMARY SPILL_MAIN_SIZE_STANDBY

    if [ "${SPILL_MAIN_SIZE_PRIMARY}" != "${SPILL_MAIN_SIZE_STANDBY}" ]; then
        error "Test 1: primary/standby main fork size mismatch after spill \
(${SPILL_MAIN_SIZE_PRIMARY} vs ${SPILL_MAIN_SIZE_STANDBY})"
    fi

    log "Test 1 PASSED: DEAD stamps match on primary and standby"
}

test_standby_query_after_spill() {
    log "=== Test 2: standby search after spill ==="

    local count
    count=$(search_count "${STANDBY_PORT}" dead_spill_t body \
        "dead" "${IDX}")
    log "  standby search hits: ${count} (expect > 0)"
    if [ -z "${count}" ] || [ "${count}" -lt 1 ]; then
        error "Test 2: standby query returned no rows after spill"
    fi

    log "Test 2 PASSED: standby queries work with replicated DEAD chain"
}

test_primary_reclaim_and_standby_still_dead() {
    log "=== Test 3: primary reclaim; standby keeps DEAD until local VACUUM ==="

    primary_sql "VACUUM ANALYZE dead_spill_t;" >/dev/null
    primary_sql "VACUUM ANALYZE dead_spill_t;" >/dev/null
    wait_for_standby_catchup

    local standby_dead
    standby_dead=$(dead_page_count "${STANDBY_PORT}")
    log "  standby dead pages after primary VACUUM only: ${standby_dead} \
(expect ${SPILL_DEAD_COUNT}; FSM not WAL-logged)"
    if [ "${standby_dead}" != "${SPILL_DEAD_COUNT}" ]; then
        error "Test 3: standby dead count ${standby_dead}, \
expected ${SPILL_DEAD_COUNT}"
    fi

    local standby_size_after_primary_vacuum
    standby_size_after_primary_vacuum=$(index_main_size "${STANDBY_PORT}")
    log "  standby main size after primary VACUUM only: \
${standby_size_after_primary_vacuum} (expect ${SPILL_MAIN_SIZE_STANDBY})"
    if [ "${standby_size_after_primary_vacuum}" != \
        "${SPILL_MAIN_SIZE_STANDBY}" ]; then
        error "Test 3: standby main fork grew/shrank after primary VACUUM \
only (${standby_size_after_primary_vacuum} vs ${SPILL_MAIN_SIZE_STANDBY}); \
FSM reclaim is not WAL-logged"
    fi

    if ! verify_fsm_reuse "${PRIMARY_PORT}" "${SPILL_DEAD_COUNT}" \
        "${SPILL_MAIN_SIZE_PRIMARY}"; then
        error "Test 3: primary FSM reclaim did not reuse dead pages"
    fi

    primary_sql "CHECKPOINT;" >/dev/null
    wait_for_standby_catchup

    local primary_size standby_size_after_reuse
    primary_size=$(index_main_size "${PRIMARY_PORT}")
    standby_size_after_reuse=$(index_main_size "${STANDBY_PORT}")
    log "  main size after primary re-insert: primary=${primary_size} \
standby=${standby_size_after_reuse}"
    if [ "${primary_size}" != "${standby_size_after_reuse}" ]; then
        error "Test 3: primary/standby main fork diverged after reuse WAL \
(${primary_size} vs ${standby_size_after_reuse})"
    fi

    log "Test 3 PASSED: primary FSM reclaim is local (not streamed)"
}

test_standby_reclaim_after_promote() {
    log "=== Test 4: standby reclaim after promote ==="

    pg_ctl promote -D "${STANDBY_DIR}" -w

    local in_recovery
    in_recovery=$(node_sql_quiet "${STANDBY_PORT}" \
        "SELECT pg_is_in_recovery();")
    if [ "${in_recovery}" = "t" ]; then
        error "Test 4: standby still in recovery after promote"
    fi

    node_sql_quiet "${STANDBY_PORT}" \
        "VACUUM ANALYZE dead_spill_t;" >/dev/null
    node_sql_quiet "${STANDBY_PORT}" \
        "VACUUM ANALYZE dead_spill_t;" >/dev/null

    if ! verify_fsm_reuse "${STANDBY_PORT}" "${SPILL_DEAD_COUNT}" \
        "${SPILL_MAIN_SIZE_STANDBY}"; then
        error "Test 4: promoted node FSM reclaim did not reuse dead pages"
    fi

    local count
    count=$(search_count "${STANDBY_PORT}" dead_spill_t body \
        "dead" "${IDX}")
    if [ -z "${count}" ] || [ "${count}" -lt 1 ]; then
        error "Test 4: search failed on promoted node after reclaim"
    fi

    log "Test 4 PASSED: replica-local reclaim after promote"
}

main() {
    log "Starting memtable DEAD/reclaim replication tests..."
    check_required_tools

    setup_primary
    setup_standby
    wait_for_standby_catchup

    local failures=""
    test_dead_stamp_replicates_to_standby || \
        failures="${failures} test_dead_stamp_replicates_to_standby"
    test_standby_query_after_spill || \
        failures="${failures} test_standby_query_after_spill"
    test_primary_reclaim_and_standby_still_dead || \
        failures="${failures} test_primary_reclaim_and_standby_still_dead"
    test_standby_reclaim_after_promote || \
        failures="${failures} test_standby_reclaim_after_promote"

    if [ -n "${failures}" ]; then
        error "Memtable DEAD replication failures:${failures}"
    fi
    log "All memtable DEAD/reclaim replication tests passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
