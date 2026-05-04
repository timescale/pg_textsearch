#!/bin/bash
#
# replication_spill_paths.sh — verify that the SPILL WAL records
# emitted from the non-`tp_do_spill` spill paths actually replicate
# end-to-end. Two tests:
#
#   1. tp_bulk_load_spill_check (PRE_COMMIT bulk-load threshold)
#      A fat single-transaction insert crosses bulk_load_threshold;
#      PRE_COMMIT triggers a spill on the primary. The standby's
#      memtable must be empty afterwards (cleared by SPILL redo);
#      the segment must hold all the docs.
#
#   2. tp_spill_memtable_if_needed (VACUUM bulkdelete)
#      INSERT then DELETE then VACUUM. VACUUM's bulkdelete phase
#      always spills any non-empty memtable. The standby's memtable
#      must be empty afterwards; the segment holds the docs (with
#      half marked dead by the alive-bitset update).
#
# Both tests assert directly against bm25_summarize_index on the
# standby (BMW query results turn out to mask memtable+segment
# overlap in some cases). The bug they catch is "primary spill
# replicates segment data + metapage but not the memtable-clear
# signal" — visible only as a memtable+segment doc-count
# divergence on the standby.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55460
STANDBY_PORT=55461
TEST_DB=replication_spill_paths
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_spill_paths_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_spill_paths_standby"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

# ---------------------------------------------------------------
# Test 1: bulk-load PRE_COMMIT spill replication
#
# Tunes the GUCs so the only spill trigger that fires is the
# bulk-load PRE_COMMIT path:
#   - bulk_load_threshold = 500 terms (low, easy to cross)
#   - memtable_spill_threshold = 0 (disabled)
#   - memory_limit very high (per-row check won't fire)
# Then runs a 2000-row INSERT in a single transaction. Each row
# carries enough unique terms that terms_added_this_xact crosses
# the threshold by COMMIT time, so tp_bulk_load_spill_check fires
# at PRE_COMMIT.
# ---------------------------------------------------------------
test_bulk_load_spill_replication() {
    log "=== Test 1: bulk-load PRE_COMMIT spill replication ==="

    primary_sql "
        DROP TABLE IF EXISTS bulk_docs CASCADE;
        CREATE TABLE bulk_docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
        CREATE INDEX bulk_docs_idx ON bulk_docs USING bm25(content)
            WITH (text_config='english');
    " >/dev/null

    wait_for_standby_catchup

    local query="
        SELECT count(*) FROM (
            SELECT id FROM bulk_docs
            ORDER BY content <@> to_bm25query('alpha',
                'bulk_docs_idx')
            LIMIT 100000
        ) t;
    "

    # bulk_load_threshold and memtable_spill_threshold are SUSET —
    # set per-session in the same psql -c that runs the INSERT so
    # PRE_COMMIT sees them. memory_limit is SIGHUP and stays at
    # its default 2 GB, which is well above the ~140 KB this
    # transaction needs.
    long_lived_before_after "${STANDBY_PORT}" "${query}" \
        "primary_sql \"
            SET pg_textsearch.bulk_load_threshold = 500;
            SET pg_textsearch.memtable_spill_threshold = 0;
            BEGIN;
            INSERT INTO bulk_docs (content)
                SELECT 'alpha bravo charlie delta echo ' ||
                       'foxtrot golf hotel india juliet ' ||
                       md5(g::text) || ' doc_' || g
                FROM generate_series(1, 2000) g;
            COMMIT;
        \" >/dev/null
        wait_for_standby_catchup"

    log "  initial standby count: ${LL_BEFORE}"
    log "  post-insert count:     ${LL_AFTER}"

    if [ "${LL_BEFORE}" != "0" ]; then
        error "Test 1: expected initial 0, got '${LL_BEFORE}'"
    fi
    # Long-lived backend must see all 2000 docs (this fails before
    # PR 2 — INSERT_TERMS not replicated to the cached memtable).
    if [ "${LL_AFTER}" != "2000" ]; then
        error "Test 1: expected post-insert count 2000, \
got '${LL_AFTER}'"
    fi

    # Direct check on standby state via bm25_summarize_index. The
    # bulk-load PRE_COMMIT spill on the primary should have written
    # the SPILL WAL record, whose redo on the standby clears the
    # standby's memtable. Replay independently lands the segment on
    # the standby's disk via GenericXLog. Final state on the
    # standby must be: memtable empty (cleared by SPILL redo),
    # segment populated (received via segment-data WAL).
    #
    # Without the SPILL WAL emit (the bug this test catches),
    # nothing would clear the standby memtable; it would retain
    # all 2000 docs from INSERT_TERMS replay, AND the segment
    # would also have 2000 — visible as a memtable+segment overlap
    # in bm25_summarize_index even though the BMW query happens to
    # mask the duplication elsewhere.
    local standby_summary
    standby_summary=$(standby_sql_quiet "
        SELECT bm25_summarize_index('bulk_docs_idx');")
    local standby_memtable_docs
    local standby_segment_docs
    standby_memtable_docs=$(echo "${standby_summary}" \
        | awk '/^Memtable:/{f=1;next} f && /documents:/{print $2; exit}')
    standby_segment_docs=$(echo "${standby_summary}" \
        | awk '/^Segments:/{f=1;next} \
               f && /Total:/{ \
                 for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/ && $(i+1) ~ /docs/){print $i; exit} \
               }')
    log "  standby memtable docs: ${standby_memtable_docs:-<none>} (expect 0)"
    log "  standby segment docs:  ${standby_segment_docs:-<none>} (expect 2000)"

    if [ "${standby_memtable_docs:-x}" != "0" ]; then
        log "  --- standby summary (for diagnosis) ---"
        echo "${standby_summary}" | while IFS= read -r line; do
            log "    S| ${line}"
        done
        error "Test 1: standby memtable should be empty after \
PRE_COMMIT spill replicates (got '${standby_memtable_docs}'). \
SPILL WAL is not being emitted from tp_bulk_load_spill_check, \
or its redo is not clearing the standby memtable."
    fi
    if [ "${standby_segment_docs:-0}" != "2000" ]; then
        error "Test 1: standby segment should hold all 2000 docs \
post-spill, got '${standby_segment_docs}'"
    fi

    log "Test 1 PASSED: bulk-load PRE_COMMIT spill replicates"
}

# ---------------------------------------------------------------
# Test 2: VACUUM-triggered spill replication
#
# VACUUM's bulkdelete phase always calls tp_spill_memtable_if_needed
# with min_postings=1 (vacuum.c:766) — i.e., spills any non-empty
# memtable so the bulkdelete pass operates on segment storage only.
# This is one of the spill paths PR 2 added tp_xlog_spill to.
#
# The test inserts a small number of docs (well under the bulk-load
# threshold), DELETEs some, then VACUUMs. The VACUUM's bulkdelete
# spill fires deterministically. With SPILL WAL emitted, redo
# clears the standby's memtable; the standby's bm25_summarize_index
# memtable count drops to 0. Without the WAL emit, the standby's
# memtable retains all the inserted docs.
# ---------------------------------------------------------------
test_vacuum_spill_replication() {
    log "=== Test 2: VACUUM-triggered spill replication ==="

    primary_sql "
        DROP TABLE IF EXISTS vac_docs CASCADE;
        CREATE TABLE vac_docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
        CREATE INDEX vac_docs_idx ON vac_docs USING bm25(content)
            WITH (text_config='english');
        INSERT INTO vac_docs (content)
            SELECT 'gamma delta epsilon zeta eta ' || md5(g::text)
            FROM generate_series(1, 200) g;
    " >/dev/null

    wait_for_standby_catchup

    # Sanity check — primary memtable should hold the docs at
    # this point (no spill yet because we're well under bulk-load
    # and per-index limits).
    local primary_memtable_pre
    primary_memtable_pre=$(primary_sql_quiet "
        SELECT bm25_summarize_index('vac_docs_idx');" \
        | awk '/^Memtable:/{f=1;next} f && /documents:/{print $2; exit}')
    log "  primary memtable docs before VACUUM: ${primary_memtable_pre} (expect 200)"

    # VACUUM can't run in a transaction block; psql -c with multiple
    # statements wraps in one — split.
    primary_sql "DELETE FROM vac_docs WHERE id <= 100;" >/dev/null
    primary_sql "VACUUM vac_docs;" >/dev/null
    # Force a checkpoint after VACUUM so all VACUUM-generated WAL
    # is flushed to disk on the primary. Without this, the
    # following wait_for_standby_catchup can return before the
    # metapage GenericXLog records reach the standby's buffer
    # pool, causing the standby to read the pre-VACUUM metapage.
    primary_sql "CHECKPOINT;" >/dev/null

    wait_for_standby_catchup

    # Diagnostic: primary state right after VACUUM. If the bulkdelete
    # spill fired, primary memtable will be empty and a segment will
    # exist.
    log "  --- primary summary after VACUUM ---"
    primary_sql_quiet "SELECT bm25_summarize_index('vac_docs_idx');" \
        | while IFS= read -r line; do log "    P| ${line}"; done

    local standby_summary
    standby_summary=$(standby_sql_quiet "
        SELECT bm25_summarize_index('vac_docs_idx');")
    local standby_memtable_docs
    local standby_segment_docs
    standby_memtable_docs=$(echo "${standby_summary}" \
        | awk '/^Memtable:/{f=1;next} f && /documents:/{print $2; exit}')
    standby_segment_docs=$(echo "${standby_summary}" \
        | awk '/^Segments:/{f=1;next} \
               f && /Total:/{ \
                 for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/ && $(i+1) ~ /docs/){print $i; exit} \
               }')
    log "  standby memtable docs after VACUUM: ${standby_memtable_docs:-<none>} (expect 0)"
    log "  standby segment docs after VACUUM:  ${standby_segment_docs:-<none>} (expect 200)"

    if [ "${standby_memtable_docs:-x}" != "0" ]; then
        log "  --- standby summary (for diagnosis) ---"
        echo "${standby_summary}" | while IFS= read -r line; do
            log "    S| ${line}"
        done
        error "Test 2: standby memtable should be empty after \
VACUUM-triggered spill replicates (got '${standby_memtable_docs}'). \
SPILL WAL is not being emitted from tp_spill_memtable_if_needed, \
or its redo is not clearing the standby memtable."
    fi
    if [ "${standby_segment_docs:-0}" != "200" ]; then
        error "Test 2: standby segment should hold all 200 docs \
post-VACUUM, got '${standby_segment_docs}'"
    fi

    log "Test 2 PASSED: VACUUM-triggered spill replicates"
}

main() {
    log "Starting spill-paths replication test..."
    check_required_tools

    setup_primary
    setup_standby
    wait_for_standby_catchup

    # Run each subtest in a subshell so a failure in one doesn't
    # short-circuit the others. Useful for the verify-coverage
    # workflow (deliberate bug-reverts) — every test should still
    # report independently. Track failures and exit non-zero at
    # the end if any failed.
    local failures=""
    ( test_bulk_load_spill_replication ) || \
        failures="${failures} test_bulk_load_spill_replication"
    ( test_vacuum_spill_replication ) || \
        failures="${failures} test_vacuum_spill_replication"

    if [ -n "${failures}" ]; then
        error "Spill-paths failures:${failures}"
    fi
    log "All spill-paths replication tests passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
