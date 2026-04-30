#!/bin/bash
#
# replication_issue_342.sh — targeted reproducers for issue #342:
#   "BM25 index queries fail on streaming replicas with
#    'ERROR: invalid page index at block N'"
#
# Root cause per issue: page-index pages are written via
# MarkBufferDirty + FlushRelationBuffers without GenericXLog, so they
# never reach the standby. Three code paths are listed in the issue:
#
#   1. Serial CREATE INDEX     — tp_write_segment_from_build_ctx
#                                (src/access/build_context.c)
#   2. Parallel CREATE INDEX   — leader path
#                                (src/access/build_parallel.c)
#   3. Memtable spill / merge  — write_page_index_internal
#                                (src/segment/segment.c)
#
# At small data sizes (< ~150K docs) the serial-build path doesn't
# trigger a visible failure because the segment's page-index fits in
# a single block whose extension WAL covers the page implicitly. The
# spill path triggers reliably because additional segment pages are
# allocated post-build, exposing the missing GenericXLog coverage on
# the page-index page.
#
# Broader coverage (UPDATE, partitioned, expression, partial, array,
# multi-index) is in `replication_correctness.sh`. This file is the
# minimal, focused reproducer pinned to the issue.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55454
STANDBY_PORT=55455
TEST_DB=replication_issue_342
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_342_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_342_standby"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

# Count matching docs without the LIMIT 100 cap that search_count uses.
search_count_full() {
    local port=$1
    local table=$2
    local column=$3
    local query=$4
    local index=$5
    local limit=${6:-100000}
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
        error "${label} BUG (expected via #342): standby returned \
'${got}', expected '${expected}'"
    fi
}

# -------------------------------------------------------------------
# Test: Memtable spill with standby streaming.
#
# Reproduces issue #342's "spill / level merge" path. Empty index is
# built (replicates trivially via initial relation extension WAL),
# then the primary inserts data and spills the memtable. The spill
# exercises write_page_index_internal in segment.c, which today
# writes the page-index page via MarkBufferDirty without GenericXLog.
# The standby cannot read it.
#
# Symptom on un-fixed code: standby query errors with
#   ERROR: could not read blocks N..N in file "base/X/Y": read only 0
# (or the magic-byte variant if FSM recycled a stale page).
# -------------------------------------------------------------------
test_memtable_spill_with_standby() {
    log "=== #342: memtable spill with standby streaming ==="

    primary_sql "
        DROP TABLE IF EXISTS chunks_contracts CASCADE;
        CREATE TABLE chunks_contracts (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
        CREATE INDEX idx_chunks_contracts_content_bm25
            ON chunks_contracts USING bm25(content)
            WITH (text_config='english');
        INSERT INTO chunks_contracts (content)
            SELECT 'document ' || i || ' alpha bravo charlie '
                || repeat(md5(i::text), 8)
            FROM generate_series(1, 3000) i;
        SELECT bm25_spill_index('idx_chunks_contracts_content_bm25');
    " >/dev/null

    wait_for_standby_catchup

    local primary_count
    primary_count=$(search_count_full "${PRIMARY_PORT}" \
        chunks_contracts content "alpha" \
        "idx_chunks_contracts_content_bm25" 10000)
    log "  primary count: ${primary_count}"
    if [ "${primary_count}" != "3000" ]; then
        error "#342: primary returned ${primary_count}/3000 — \
test setup is broken"
    fi

    local standby_count
    standby_count=$(search_count_full "${STANDBY_PORT}" \
        chunks_contracts content "alpha" \
        "idx_chunks_contracts_content_bm25" 10000)
    log "  standby count: ${standby_count} (expect 3000)"
    expect_count "#342" "${standby_count}" "3000"
    log "#342 PASSED"
}

main() {
    log "Starting issue #342 reproducer..."
    check_required_tools
    setup_primary
    setup_standby
    wait_for_standby_catchup

    test_memtable_spill_with_standby

    log "Issue #342 reproducer PASSED — bug is fixed."
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
