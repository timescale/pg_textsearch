#!/bin/bash
#
# replication_concurrency.sh — concurrency tests for physical
# replication. Multiple standby readers under primary write load.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55442
STANDBY_PORT=55443
TEST_DB=replication_concurrency
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_conc_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_conc_standby"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

# -------------------------------------------------------------------
# B1: N concurrent long-lived backends on standby.
# Eight long-lived backends each query before+after a primary
# insert. All should see the new row.
# Expected: FAIL today (every long-lived backend goes stale).
# -------------------------------------------------------------------
test_concurrent_standby_readers() {
    log "=== B1: 8 concurrent long-lived standby readers ==="

    primary_sql "
        DROP TABLE IF EXISTS b1_docs CASCADE;
        CREATE TABLE b1_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO b1_docs (content) VALUES
            ('initial doc alpha bravo');
        CREATE INDEX b1_idx ON b1_docs USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    # Spawn 8 reader subprocesses. Each opens its own long-lived
    # session, takes a 'before' reading, waits for a sync file,
    # takes an 'after' reading, and writes both to a result file.
    local sync_file
    sync_file=$(mktemp)
    rm "${sync_file}"  # parent recreates after primary insert
    local results_dir
    results_dir=$(mktemp -d)

    local pids=()
    for i in $(seq 1 8); do
        (
            # shellcheck source=replication_lib.sh
            source "${SCRIPT_DIR}/replication_lib.sh"
            long_lived_open "${STANDBY_PORT}"
            local before
            before=$(long_lived_query "
                SELECT count(*) FROM (SELECT id FROM b1_docs
                    ORDER BY content <@> to_bm25query('alpha', 'b1_idx')
                    LIMIT 100) t;")
            # Wait until parent flips sync file.
            while [ ! -f "${sync_file}" ]; do sleep 0.1; done
            local after
            after=$(long_lived_query "
                SELECT count(*) FROM (SELECT id FROM b1_docs
                    ORDER BY content <@> to_bm25query('alpha', 'b1_idx')
                    LIMIT 100) t;")
            long_lived_close
            echo "${before} ${after}" > "${results_dir}/r_${i}"
        ) &
        pids+=($!)
    done

    # Give readers time to take 'before' readings.
    sleep 2

    # Insert on primary, wait for replication, signal readers.
    primary_sql "
        INSERT INTO b1_docs (content) VALUES
            ('newly added alpha bravo charlie');
    " >/dev/null
    wait_for_standby_catchup
    touch "${sync_file}"

    for p in "${pids[@]}"; do wait "$p"; done

    local fail_count=0
    for i in $(seq 1 8); do
        local before after
        read -r before after < "${results_dir}/r_${i}"
        log "  reader ${i}: before=${before}, after=${after}"
        if [[ "${after}" == *ERROR* ]] || \
           { [[ "${after}" =~ ^[0-9]+$ ]] && \
             [ "${after}" -le "${before}" ]; }; then
            fail_count=$((fail_count + 1))
        fi
    done

    rm -rf "${results_dir}" "${sync_file}"

    if [ "${fail_count}" -gt 0 ]; then
        error "B1 BUG (expected): ${fail_count}/8 standby readers \
missed the new primary insert"
    fi
    log "B1 PASSED"
}

# -------------------------------------------------------------------
# B2: Drain under insert burst.
# Primary inserts 5000 docs; 4 standby long-lived readers
# repeatedly query during and after. All should converge to seeing
# all 5000.
# Expected: FAIL today.
# -------------------------------------------------------------------
test_drain_under_insert_burst() {
    log "=== B2: drain under insert burst ==="

    primary_sql "
        DROP TABLE IF EXISTS b2_docs CASCADE;
        CREATE TABLE b2_docs (id SERIAL PRIMARY KEY, content TEXT);
        CREATE INDEX b2_idx ON b2_docs USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    local results_dir
    results_dir=$(mktemp -d)
    local stop_file
    stop_file=$(mktemp); rm "${stop_file}"

    local pids=()
    for i in 1 2 3 4; do
        (
            # shellcheck source=replication_lib.sh
            source "${SCRIPT_DIR}/replication_lib.sh"
            long_lived_open "${STANDBY_PORT}"
            local last=0
            while [ ! -f "${stop_file}" ]; do
                last=$(long_lived_query "
                    SELECT count(*) FROM (SELECT id FROM b2_docs
                        ORDER BY content <@> to_bm25query('burst','b2_idx')
                        LIMIT 10000) t;")
                sleep 0.2
            done
            # Final reading after stop signal.
            local final
            final=$(long_lived_query "
                SELECT count(*) FROM (SELECT id FROM b2_docs
                    ORDER BY content <@> to_bm25query('burst','b2_idx')
                    LIMIT 10000) t;")
            long_lived_close
            echo "${last} ${final}" > "${results_dir}/r_${i}"
        ) &
        pids+=($!)
    done

    # Insert burst on primary in batches.
    for batch in $(seq 1 50); do
        primary_sql "
            INSERT INTO b2_docs (content)
                SELECT 'burst batch ${batch} doc ' || j
                FROM generate_series(1, 100) j;
        " >/dev/null
    done
    wait_for_standby_catchup
    sleep 1
    touch "${stop_file}"

    for p in "${pids[@]}"; do wait "$p"; done

    local fail_count=0
    for i in 1 2 3 4; do
        local last final
        read -r last final < "${results_dir}/r_${i}"
        log "  reader ${i}: last_during=${last}, final_after=${final}"
        if [[ "${final}" == *ERROR* ]] || \
           ! [[ "${final}" =~ ^[0-9]+$ ]] || \
           [ "${final}" -lt "5000" ]; then
            fail_count=$((fail_count + 1))
        fi
    done
    rm -rf "${results_dir}" "${stop_file}"

    if [ "${fail_count}" -gt 0 ]; then
        error "B2 BUG (expected): ${fail_count}/4 standby readers \
did not converge to 5000 after burst"
    fi
    log "B2 PASSED"
}

# -------------------------------------------------------------------
# B3: No deadlock under spam load.
# Primary spam-inserts and spam-spills; many standby readers
# repeatedly query. No process crashes, no deadlocks, no error
# messages in standby log.
# Expected: PASS today (passes vacuously — there's no drain code
# to deadlock yet). After implementation, this guards the LWLock.
# -------------------------------------------------------------------
test_no_deadlock_under_spam_load() {
    log "=== B3: no deadlock under spam load ==="

    primary_sql "
        DROP TABLE IF EXISTS b3_docs CASCADE;
        CREATE TABLE b3_docs (id SERIAL PRIMARY KEY, content TEXT);
        CREATE INDEX b3_idx ON b3_docs USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    local stop_file
    stop_file=$(mktemp); rm "${stop_file}"

    # 4 standby readers
    local rd_pids=()
    for i in 1 2 3 4; do
        (
            while [ ! -f "${stop_file}" ]; do
                psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA -c "
                    SELECT count(*) FROM (SELECT id FROM b3_docs
                        ORDER BY content <@>
                            to_bm25query('spam','b3_idx')
                        LIMIT 1000) t;
                " >/dev/null 2>&1 || true
            done
        ) &
        rd_pids+=($!)
    done

    # Primary writer: insert + occasional spill.
    for batch in $(seq 1 30); do
        primary_sql "
            INSERT INTO b3_docs (content)
                SELECT 'spam batch ${batch} doc ' || j
                FROM generate_series(1, 200) j;
        " >/dev/null
        if [ $((batch % 5)) -eq 0 ]; then
            primary_sql "SELECT bm25_spill_index('b3_idx');" >/dev/null
        fi
    done

    wait_for_standby_catchup
    sleep 2
    touch "${stop_file}"
    for p in "${rd_pids[@]}"; do wait "$p" 2>/dev/null; done
    rm -f "${stop_file}"

    # Verify standby is still alive and logs clean of fatal errors.
    local alive
    alive=$(node_sql_quiet "${STANDBY_PORT}" "SELECT 1;")
    if [ "${alive}" != "1" ]; then
        error "B3: standby is dead after spam load"
    fi

    if grep -E 'PANIC|FATAL|deadlock' \
       "${STANDBY_DIR}/log/postgres.log" >/dev/null 2>&1; then
        error "B3: standby log shows PANIC/FATAL/deadlock entries"
    fi

    log "B3 PASSED"
}

main() {
    log "Starting pg_textsearch replication concurrency tests..."
    check_required_tools
    setup_primary
    setup_standby

    test_concurrent_standby_readers
    test_drain_under_insert_burst
    test_no_deadlock_under_spam_load

    log "All replication concurrency tests completed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
