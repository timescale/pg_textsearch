#!/bin/bash
#
# replication_parallel_build.sh — parallel CREATE INDEX replication
# test for issue #342.
#
# PR #343 fixed three WAL-silent paths. The
# tp_write_segment_from_build_ctx (serial build) and
# write_page_index_internal (spill / merge) paths are exercised by
# replication_issue_342.sh and replication_correctness.sh at small
# data scales. The parallel-build leader path
# (src/access/build_parallel.c, write_merged_segment_to_sink + new
# FULL_IMAGE pass) only runs for tables of >= TP_MIN_PARALLEL_TUPLES
# (100K) rows when max_parallel_maintenance_workers > 0.
#
# This test inserts 150K rows on the primary, runs parallel CREATE
# INDEX, waits for the standby to catch up, and asserts the standby
# returns the same row count as the primary.
#
# On the un-fixed code path, the standby would either ERROR with
# "could not read blocks" / "invalid page index" or return 0 results
# from the un-WAL-logged merged segment.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55456
STANDBY_PORT=55457
TEST_DB=replication_parallel_build
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_pbuild_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_pbuild_standby"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

# Count matching docs without the LIMIT 100 cap that search_count
# uses (we have 150K rows).
search_count_full() {
    local port=$1
    local table=$2
    local column=$3
    local query=$4
    local index=$5
    local limit=${6:-200000}
    psql -p "${port}" -d "${TEST_DB}" -tA -c "
        SELECT count(*) FROM (
            SELECT id FROM ${table}
            ORDER BY ${column} <@> to_bm25query('${query}',
                '${index}')
            LIMIT ${limit}
        ) t;" 2>/dev/null
}

expect_count() {
    local label=$1
    local got=$2
    local expected=$3
    if [[ "${got}" == *ERROR* ]] || \
       [[ "${got}" == *"invalid page"* ]] || \
       [[ "${got}" == *"invalid segment"* ]] || \
       [[ "${got}" == *"could not read blocks"* ]] || \
       ! [[ "${got}" =~ ^[0-9]+$ ]] || \
       [ "${got}" != "${expected}" ]; then
        error "${label} BUG (expected via #343): standby returned \
'${got}', expected '${expected}'"
    fi
}

# -------------------------------------------------------------------
# Test: parallel CREATE INDEX with standby streaming.
#
# Reproduces the parallel-build leg of issue #342. >=100K rows
# triggers parallel CREATE INDEX (TP_MIN_PARALLEL_TUPLES). The leader
# merges N worker segments via write_merged_segment_to_sink; before
# PR #343 the merged data pages reached disk on the primary but
# never made it into WAL.
# -------------------------------------------------------------------
test_parallel_create_index_with_standby() {
    log "=== parallel CREATE INDEX with standby streaming ==="

    primary_sql "
        DROP TABLE IF EXISTS pbuild_docs CASCADE;
        CREATE TABLE pbuild_docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
        INSERT INTO pbuild_docs (content)
            SELECT 'document ' || i || ' alpha bravo charlie '
                || repeat(md5(i::text), 4)
            FROM generate_series(1, 150000) i;
        ANALYZE pbuild_docs;
    " >/dev/null

    # Force parallel build. pg_textsearch's heuristic is gated on
    # reltuples >= TP_MIN_PARALLEL_TUPLES (100K) and on the planner
    # assigning workers via plan_create_index_workers. ANALYZE above
    # ensures reltuples reflects reality; min_parallel_table_scan_size
    # = 0 keeps the planner from declining parallelism for the small
    # heap.
    primary_sql "
        SET max_parallel_maintenance_workers = 4;
        SET maintenance_work_mem = '256MB';
        SET min_parallel_table_scan_size = 0;
        CREATE INDEX pbuild_idx ON pbuild_docs
            USING bm25(content)
            WITH (text_config='english');
    " >/dev/null

    # Sanity: the index build must have launched parallel workers,
    # otherwise we are testing the serial path (already covered by
    # other replication tests) and not the build_parallel.c fix.
    # pg_textsearch emits a NOTICE "parallel index build: launched
    # N of M requested workers" when tp_build_parallel runs; the
    # NOTICE goes to the server log at log_min_messages = notice.
    local launched
    launched=$(grep -oE "parallel index build: launched [0-9]+" \
            "${PRIMARY_DIR}/log/postgres.log" 2>/dev/null | tail -1)
    if [ -z "${launched}" ] || \
       [[ "${launched}" =~ launched\ 0$ ]]; then
        error "parallel build: primary did not launch parallel \
workers (log line: '${launched:-<missing>}'); test is not \
exercising build_parallel.c. Check max_parallel_maintenance_workers \
and reltuples on pbuild_docs."
    fi
    log "  ${launched}"

    wait_for_standby_catchup 30

    local primary_count
    primary_count=$(search_count_full "${PRIMARY_PORT}" \
        pbuild_docs content "alpha" pbuild_idx 200000)
    log "  primary count: ${primary_count}"
    if [ "${primary_count}" != "150000" ]; then
        error "parallel build: primary returned \
${primary_count}/150000 — test setup is broken"
    fi

    local standby_count
    standby_count=$(search_count_full "${STANDBY_PORT}" \
        pbuild_docs content "alpha" pbuild_idx 200000)
    log "  standby count: ${standby_count} (expect 150000)"
    expect_count "parallel-build" "${standby_count}" "150000"
    log "parallel CREATE INDEX replication PASSED"
}

main() {
    log "Starting parallel-build replication test..."
    check_required_tools
    setup_primary
    setup_standby
    wait_for_standby_catchup

    test_parallel_create_index_with_standby

    log "Parallel-build replication test PASSED — \
build_parallel.c WAL fix verified."
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
