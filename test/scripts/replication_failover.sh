#!/bin/bash
#
# replication_failover.sh — standby-promotion tests under various
# pre-promotion states.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55444
STANDBY_PORT=55445
TEST_DB=replication_failover
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_fail_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_fail_standby"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

wait_for_promotion() {
    local port=$1
    local i=0
    while [ $i -lt 15 ]; do
        local in_recovery
        in_recovery=$(node_sql_quiet "${port}" \
            "SELECT pg_is_in_recovery();")
        if [ "$in_recovery" = "f" ]; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    error "Promotion did not complete on port ${port}"
}

# -------------------------------------------------------------------
# D1: Promotion under steady write load.
# Primary actively inserting; promote standby; verify the promoted
# standby (a) sees pre-promotion data, (b) accepts new writes.
# Expected: FAIL today — segments created by primary spill while
# the standby is already running (WAL-replayed) are not enumerable
# on the standby even after promotion. bm25_summarize_index shows
# corpus stats (total_docs=100) but no segments. Same bug class as
# replication_correctness.sh A2/A5. Will pass once the rmgr-based
# replication implementation lands.
# -------------------------------------------------------------------
test_promotion_under_write_load() {
    log "=== D1: promotion under write load ==="
    primary_sql "
        DROP TABLE IF EXISTS d1_docs CASCADE;
        CREATE TABLE d1_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO d1_docs (content)
            SELECT 'doc ' || i || ' alpha'
            FROM generate_series(1,100) i;
        CREATE INDEX d1_idx ON d1_docs USING bm25(content)
            WITH (text_config='simple');
        SELECT bm25_spill_index('d1_idx');
    " >/dev/null
    wait_for_standby_catchup

    pg_ctl promote -D "${STANDBY_DIR}" -w
    wait_for_promotion "${STANDBY_PORT}"

    # Spilled docs should be visible (segment-on-disk).
    local count
    count=$(search_count "${STANDBY_PORT}" d1_docs content "alpha" \
        "d1_idx")
    log "  promoted-standby alpha count: ${count}"
    if [[ "${count}" == *ERROR* ]] || [ "${count}" != "100" ]; then
        error "D1 BUG (expected): WAL-replayed spilled segment not \
enumerable on promoted standby (got ${count}/100)"
    fi

    # Writes work.
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -c "
        INSERT INTO d1_docs (content) VALUES
            ('post-promotion alpha doc');
    " >/dev/null
    local count2
    count2=$(search_count "${STANDBY_PORT}" d1_docs content "alpha" \
        "d1_idx")
    if [[ "${count2}" == *ERROR* ]] || \
       ! [[ "${count2}" =~ ^[0-9]+$ ]] || \
       [ "${count2}" -le "${count}" ]; then
        error "D1: writes on promoted standby not visible \
(${count} -> ${count2})"
    fi
    log "D1 PASSED"
}

# -------------------------------------------------------------------
# D2: Promotion with unspilled data.
# Primary has un-spilled data in memtable; standby is replicated
# but its memtable was rebuilt from docid pages (so it has the
# data). Promote standby; verify all data is visible after promotion.
# Expected: PASS today — the long-lived backend opened before any
# inserts has its first query trigger memtable rebuild, and the
# promotion + subsequent query sees post-rebuild state. The memtable
# staleness bug does not trigger in this sequence (compare with
# replication.sh test_long_lived_backend_staleness, which DOES open
# the long-lived backend before the first insert and sees staleness).
# -------------------------------------------------------------------
test_promotion_with_unspilled_memtable() {
    log "=== D2: promotion with unspilled memtable ==="

    # Reset: fresh primary+standby pair so this test is independent
    # of D1's promoted standby (which is no longer in recovery).
    pg_ctl stop -D "${STANDBY_DIR}" -m fast 2>/dev/null || true
    pg_ctl stop -D "${PRIMARY_DIR}" -m fast 2>/dev/null || true
    rm -rf "${PRIMARY_DIR}" "${STANDBY_DIR}"
    setup_primary

    primary_sql "
        DROP TABLE IF EXISTS d2_docs CASCADE;
        CREATE TABLE d2_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO d2_docs (content)
            SELECT 'doc ' || i || ' alpha'
            FROM generate_series(1,50) i;
        CREATE INDEX d2_idx ON d2_docs USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    setup_standby
    wait_for_standby_catchup

    long_lived_open "${STANDBY_PORT}"
    local before
    before=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM d2_docs
            ORDER BY content <@> to_bm25query('alpha','d2_idx')
            LIMIT 200) t;")
    log "  pre-promotion long-lived count: ${before} (expect 50)"

    # Insert more on primary — these stay in memtable, not spilled.
    primary_sql "
        INSERT INTO d2_docs (content)
            SELECT 'late doc ' || i || ' alpha'
            FROM generate_series(1,30) i;
    " >/dev/null
    wait_for_standby_catchup

    pg_ctl promote -D "${STANDBY_DIR}" -w
    wait_for_promotion "${STANDBY_PORT}"

    local after
    after=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM d2_docs
            ORDER BY content <@> to_bm25query('alpha','d2_idx')
            LIMIT 200) t;")
    long_lived_close

    log "  post-promotion long-lived count: ${after} (expect 80)"
    if [ "${after}" != "80" ]; then
        error "D2 BUG (likely): long-lived backend on promoted standby \
missed pre-promotion memtable inserts (${after}/80)"
    fi
    log "D2 PASSED"
}

# -------------------------------------------------------------------
# D3: Post-promotion writes + search via fresh backends.
# Verify the standard "promoted standby is a normal primary" path
# works for fresh psql connections.
# Expected: PASS today.
# -------------------------------------------------------------------
test_post_promotion_writes_and_search() {
    log "=== D3: post-promotion writes + search ==="
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -c "
        INSERT INTO d2_docs (content) VALUES
            ('promoted-mode doc alpha bravo'),
            ('another promoted doc alpha');
    " >/dev/null
    local count
    count=$(search_count "${STANDBY_PORT}" d2_docs content "alpha" \
        "d2_idx")
    log "  search count after promotion-writes: ${count}"
    if [[ "${count}" == *ERROR* ]] || \
       ! [[ "${count}" =~ ^[0-9]+$ ]] || \
       [ "${count}" -lt 82 ]; then
        error "D3: post-promotion writes not visible (got ${count})"
    fi
    log "D3 PASSED"
}

main() {
    log "Starting pg_textsearch replication failover tests..."
    check_required_tools
    setup_primary

    primary_sql "
        DROP TABLE IF EXISTS d_warmup CASCADE;
        CREATE TABLE d_warmup (id INT, content TEXT);
        INSERT INTO d_warmup VALUES (1, 'warmup');
    " >/dev/null
    setup_standby
    wait_for_standby_catchup

    test_promotion_under_write_load
    test_promotion_with_unspilled_memtable
    test_post_promotion_writes_and_search

    log "All replication failover tests completed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
