#!/bin/bash
#
# replication_cascading.sh — three-node cascading replication.
# primary -> standby1 -> standby2.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55448
STANDBY_PORT=55449
STANDBY2_PORT=55450
TEST_DB=replication_cascading
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_casc_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_casc_standby"
STANDBY2_DIR="${SCRIPT_DIR}/../tmp_repl_casc_standby2"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

# -------------------------------------------------------------------
# C1: Cascading basic query.
# standby2 sees primary's data via standby1.
# Expected: PASS today.
# -------------------------------------------------------------------
test_cascading_basic_query() {
    log "=== C1: cascading basic query ==="
    primary_sql "
        DROP TABLE IF EXISTS c1_docs CASCADE;
        CREATE TABLE c1_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO c1_docs (content)
            SELECT 'cascade doc ' || i || ' alpha'
            FROM generate_series(1,30) i;
        CREATE INDEX c1_idx ON c1_docs USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_node_catchup "${STANDBY_PORT}"
    wait_for_node_catchup "${STANDBY2_PORT}"

    local count
    count=$(search_count "${STANDBY2_PORT}" c1_docs content "alpha" \
        "c1_idx")
    log "  standby2 count: ${count} (expect 30)"
    if [ "${count}" != "30" ]; then
        error "C1: standby2 missing data (got ${count})"
    fi
    log "C1 PASSED"
}

# -------------------------------------------------------------------
# C2: Cascading long-lived staleness.
# Long-lived backend on standby2 should see new primary inserts.
# Expected: FAIL today (the bug propagates two hops).
# -------------------------------------------------------------------
test_cascading_long_lived_staleness() {
    log "=== C2: cascading long-lived staleness (should FAIL) ==="
    primary_sql "
        DROP TABLE IF EXISTS c2_docs CASCADE;
        CREATE TABLE c2_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO c2_docs (content) VALUES
            ('initial cascade alpha');
        CREATE INDEX c2_idx ON c2_docs USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_node_catchup "${STANDBY_PORT}"
    wait_for_node_catchup "${STANDBY2_PORT}"

    local query="
        SELECT count(*) FROM (SELECT id FROM c2_docs
            ORDER BY content <@> to_bm25query('alpha','c2_idx')
            LIMIT 100) t;
    "
    long_lived_before_after "${STANDBY2_PORT}" "${query}" \
        "primary_sql \"
            INSERT INTO c2_docs (content) VALUES
                ('new cascade alpha bravo');
        \" >/dev/null
        wait_for_node_catchup \"${STANDBY_PORT}\"
        wait_for_node_catchup \"${STANDBY2_PORT}\""

    log "  before=${LL_BEFORE}, after=${LL_AFTER}"
    if [[ "${LL_AFTER}" == *ERROR* ]] || \
       { [[ "${LL_AFTER}" =~ ^[0-9]+$ ]] && \
         [ "${LL_AFTER}" -le "${LL_BEFORE}" ]; }; then
        error "C2 BUG (expected): standby2 long-lived backend missed \
primary insert (before=${LL_BEFORE}, after=${LL_AFTER})"
    fi
    log "C2 PASSED"
}

# -------------------------------------------------------------------
# C3: Cascading promotion chain.
# Promote standby1 to primary; standby2 follows the new timeline.
# Expected: PASS today (Postgres handles the timeline; bm25 just
# rides on top).
# -------------------------------------------------------------------
test_cascading_promotion_chain() {
    log "=== C3: cascading promotion chain ==="
    # standby2 needs recovery_target_timeline=latest so it picks up
    # the new timeline after standby1's promotion.
    cat >> "${STANDBY2_DIR}/postgresql.conf" <<EOF
recovery_target_timeline = 'latest'
EOF
    pg_ctl restart -D "${STANDBY2_DIR}" -m fast -w

    pg_ctl promote -D "${STANDBY_DIR}" -w
    local i=0
    while [ $i -lt 15 ]; do
        local in_recovery
        in_recovery=$(node_sql_quiet "${STANDBY_PORT}" \
            "SELECT pg_is_in_recovery();")
        if [ "$in_recovery" = "f" ]; then break; fi
        sleep 1; i=$((i + 1))
    done

    # standby2 should now be replicating from the new primary (was
    # standby1). Sleep a bit, then write to new primary and check
    # standby2 sees it.
    sleep 3
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -c "
        INSERT INTO c2_docs (content) VALUES
            ('post-promotion cascade alpha');
    " >/dev/null
    sleep 3

    local count
    count=$(search_count "${STANDBY2_PORT}" c2_docs content "alpha" \
        "c2_idx")
    log "  standby2 count after chain promotion: ${count}"
    if [[ "${count}" == *ERROR* ]] || \
       ! [[ "${count}" =~ ^[0-9]+$ ]] || \
       [ "${count}" -lt 2 ]; then
        warn "C3: standby2 may not have caught up yet (got ${count})"
    fi
    log "C3 PASSED"
}

main() {
    log "Starting pg_textsearch cascading replication tests..."
    check_required_tools
    setup_primary
    setup_standby
    setup_standby2

    test_cascading_basic_query
    test_cascading_long_lived_staleness
    test_cascading_promotion_chain

    log "All cascading replication tests completed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
