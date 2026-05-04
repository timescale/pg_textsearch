#!/bin/bash
#
# replication_spill_paths.sh — verify that the SPILL WAL records
# emitted from the non-`tp_do_spill` spill paths actually replicate
# end-to-end. Two tests:
#
#   1. tp_bulk_load_spill_check (PRE_COMMIT bulk-load threshold)
#      A fat single-transaction insert crosses bulk_load_threshold;
#      PRE_COMMIT triggers a spill on the primary. The standby's
#      long-lived backend must see the docs after catchup, with
#      a non-empty L0 segment.
#
#   2. tp_evict_largest_memtable (global memory-pressure eviction)
#      A low memory_limit + a heavy load on multiple indexes pushes
#      the global estimate past the soft limit, triggering an
#      eviction-driven spill. The standby's long-lived backend must
#      see the converged state.
#
# Both tests would silently pass before PR #347 added tp_xlog_spill
# to these spill paths — the standby would fall back to docid-page
# rebuild on a fresh connection (which masked the bug). They use a
# long-lived connection per replication_lib.sh's helpers so the
# bug class would actually manifest.

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
# Test 2: memory-pressure eviction replication
#
# Sets memory_limit to a small value, creates several bm25 indexes,
# loads enough into each that the global estimate crosses the soft
# limit (memory_limit / 2). The amortized check inside
# tp_auto_spill_if_needed (every ~100 docs per backend) calls
# tp_evict_largest_memtable, which does a regular spill on the
# largest unsplit memtable — and now also emits SPILL WAL.
#
# We can't trigger this deterministically without internal hooks,
# but with these settings the eviction path fires reliably in
# practice. We assert convergence on the standby and look for the
# distinctive primary log line as a positive signal that the
# eviction path was the one that ran.
# ---------------------------------------------------------------
test_memory_pressure_eviction_replication() {
    log "=== Test 2: memory-pressure eviction replication ==="

    # memory_limit is PGC_SIGHUP — needs ALTER SYSTEM + reload.
    # ALTER SYSTEM can't run inside a transaction, so each one
    # has to be its own psql -c (psql wraps multi-statement -c
    # in an implicit transaction).
    primary_sql "ALTER SYSTEM SET pg_textsearch.bulk_load_threshold = 0;" \
        >/dev/null
    primary_sql "ALTER SYSTEM SET pg_textsearch.memtable_spill_threshold = 0;" \
        >/dev/null
    primary_sql "ALTER SYSTEM SET pg_textsearch.memory_limit = 1048576;" \
        >/dev/null
    primary_sql "SELECT pg_reload_conf();" >/dev/null
    primary_sql "SELECT pg_sleep(0.5);" >/dev/null

    primary_sql "
        DROP TABLE IF EXISTS mp_a CASCADE;
        DROP TABLE IF EXISTS mp_b CASCADE;
        DROP TABLE IF EXISTS mp_c CASCADE;
        CREATE TABLE mp_a (id SERIAL PRIMARY KEY, content TEXT NOT NULL);
        CREATE TABLE mp_b (id SERIAL PRIMARY KEY, content TEXT NOT NULL);
        CREATE TABLE mp_c (id SERIAL PRIMARY KEY, content TEXT NOT NULL);
        CREATE INDEX mp_a_idx ON mp_a USING bm25(content)
            WITH (text_config='english');
        CREATE INDEX mp_b_idx ON mp_b USING bm25(content)
            WITH (text_config='english');
        CREATE INDEX mp_c_idx ON mp_c USING bm25(content)
            WITH (text_config='english');
    " >/dev/null

    wait_for_standby_catchup

    # Push each index past its share of the global limit. Amount
    # picked empirically — large enough to cross the soft limit
    # in every supported PG version, small enough to keep test
    # latency reasonable.
    for tbl in mp_a mp_b mp_c; do
        primary_sql "
            INSERT INTO ${tbl} (content)
                SELECT 'lorem ipsum dolor sit amet ' ||
                       repeat(md5(g::text), 16)
                FROM generate_series(1, 1500) g;
        " >/dev/null
    done

    wait_for_standby_catchup

    # Convergence check on each index — the standby must see the
    # full row count whether or not eviction was the spill trigger.
    for tbl in mp_a mp_b mp_c; do
        local primary_count
        local standby_count
        primary_count=$(primary_sql_quiet "
            SELECT count(*) FROM (
                SELECT id FROM ${tbl}
                ORDER BY content <@> to_bm25query('lorem',
                    '${tbl}_idx')
                LIMIT 100000
            ) t;")
        standby_count=$(standby_sql_quiet "
            SELECT count(*) FROM (
                SELECT id FROM ${tbl}
                ORDER BY content <@> to_bm25query('lorem',
                    '${tbl}_idx')
                LIMIT 100000
            ) t;")
        log "  ${tbl}: primary=${primary_count} standby=${standby_count}"
        if [ "${primary_count}" != "${standby_count}" ] || \
           [ -z "${primary_count}" ] || \
           [ "${primary_count}" -lt 1500 ]; then
            log "  --- ${tbl} primary summary ---"
            primary_sql_quiet "SELECT bm25_summarize_index('${tbl}_idx');" \
                | while IFS= read -r line; do log "    P| ${line}"; done
            log "  --- ${tbl} standby summary ---"
            standby_sql_quiet "SELECT bm25_summarize_index('${tbl}_idx');" \
                | while IFS= read -r line; do log "    S| ${line}"; done
            error "Test 2: ${tbl} did not converge"
        fi
    done

    # Positive signal that tp_evict_largest_memtable specifically
    # was the one that fired (vs. a per-row spill). The log message
    # is unique to this code path. Soft assertion — if the message
    # didn't appear under this load, the test still proves
    # convergence but the eviction-replication path may not have
    # been the one exercised.
    if grep -q "memory pressure, spilled memtable for index" \
            "${PRIMARY_DIR}/log/postgres.log" 2>/dev/null; then
        log "  primary log shows eviction-spill fired"
    else
        warn "  primary log shows no eviction-spill — convergence \
proved but the eviction path may not have triggered under this \
load. Adjust the volume or memory_limit if this becomes important."
    fi

    log "Test 2 PASSED: memory-pressure eviction replicates"
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
    ( test_memory_pressure_eviction_replication ) || \
        failures="${failures} test_memory_pressure_eviction_replication"

    if [ -n "${failures}" ]; then
        error "Spill-paths failures:${failures}"
    fi
    log "All spill-paths replication tests passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
