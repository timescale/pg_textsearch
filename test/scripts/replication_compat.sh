#!/bin/bash
#
# replication_compat.sh — compatibility tests:
#   F1: physical + logical replication coexistence
#   F2: TimescaleDB hypertable replication
#   F3: pg_basebackup with bm25 index files

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55446
STANDBY_PORT=55447
LOGICAL_SUB_PORT=55448
TEST_DB=replication_compat
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_compat_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_compat_standby"
LOGICAL_SUB_DIR="${SCRIPT_DIR}/../tmp_repl_compat_subscriber"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

# Custom cleanup that also handles the logical-subscriber instance.
# Capture $? on entry: the wrapper runs commands before delegating
# to repl_cleanup, which destroys the original exit code. Restore it
# (via a subshell `exit`) so repl_cleanup's `local exit_code=$?`
# captures the script's real exit status, not `rm -rf`'s.
compat_cleanup() {
    local rc=$?
    if [ -n "${LOGICAL_SUB_DIR:-}" ] && \
       [ -f "${LOGICAL_SUB_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${LOGICAL_SUB_DIR}" -m fast 2>/dev/null \
            || true
    fi
    rm -rf "${LOGICAL_SUB_DIR:-}"
    (exit "$rc")
    repl_cleanup
}
trap compat_cleanup EXIT INT TERM

# Bring up a third standalone instance to act as a logical
# subscriber.
setup_logical_subscriber() {
    log "Setting up logical subscriber on port ${LOGICAL_SUB_PORT}..."
    rm -rf "${LOGICAL_SUB_DIR}"
    mkdir -p "${LOGICAL_SUB_DIR}"
    initdb -D "${LOGICAL_SUB_DIR}" --auth-local=trust \
        --auth-host=trust >/dev/null 2>&1
    cat >> "${LOGICAL_SUB_DIR}/postgresql.conf" <<EOF
port = ${LOGICAL_SUB_PORT}
shared_preload_libraries = 'pg_textsearch'
wal_level = logical
log_min_messages = notice
logging_collector = on
log_filename = 'postgres.log'
EOF
    pg_ctl start -D "${LOGICAL_SUB_DIR}" \
        -l "${LOGICAL_SUB_DIR}/postgres.log" -w
    createdb -p "${LOGICAL_SUB_PORT}" "${TEST_DB}"
    psql -p "${LOGICAL_SUB_PORT}" -d "${TEST_DB}" -c "
        CREATE EXTENSION pg_textsearch;
    " >/dev/null
}

# -------------------------------------------------------------------
# F1: physical + logical replication coexistence.
# Primary publishes a logical pub for one table, has a physical
# standby, AND has a logical subscriber on a third instance.
# All three views agree.
# Expected: PASS today.
# -------------------------------------------------------------------
test_logical_and_physical_coexist() {
    log "=== F1: logical + physical coexistence ==="

    # Need wal_level=logical on primary.
    psql -p "${PRIMARY_PORT}" -d postgres -c "
        ALTER SYSTEM SET wal_level = logical;
    " >/dev/null
    pg_ctl restart -D "${PRIMARY_DIR}" -m fast -w
    # Recreate standby with logical-level WAL.
    pg_ctl stop -D "${STANDBY_DIR}" -m fast 2>/dev/null || true
    rm -rf "${STANDBY_DIR}"

    primary_sql "
        DROP TABLE IF EXISTS f1_docs CASCADE;
        CREATE TABLE f1_docs (id INT PRIMARY KEY, content TEXT);
        INSERT INTO f1_docs VALUES
            (1, 'initial alpha bravo'),
            (2, 'second alpha charlie');
        CREATE INDEX f1_idx ON f1_docs USING bm25(content)
            WITH (text_config='simple');
        CREATE PUBLICATION f1_pub FOR TABLE f1_docs;
    " >/dev/null

    setup_standby
    setup_logical_subscriber

    psql -p "${LOGICAL_SUB_PORT}" -d "${TEST_DB}" -c "
        CREATE TABLE f1_docs (id INT PRIMARY KEY, content TEXT);
    " >/dev/null
    # CREATE SUBSCRIPTION with create_slot=true (the default) cannot
    # run inside a transaction block, so issue it on its own.
    psql -p "${LOGICAL_SUB_PORT}" -d "${TEST_DB}" -c \
        "CREATE SUBSCRIPTION f1_sub CONNECTION 'host=127.0.0.1 port=${PRIMARY_PORT} dbname=${TEST_DB}' PUBLICATION f1_pub;" \
        >/dev/null
    sleep 3  # initial sync

    # Insert on primary.
    primary_sql "
        INSERT INTO f1_docs VALUES (3, 'third alpha delta');
    " >/dev/null
    wait_for_standby_catchup
    sleep 2  # let logical replication catch up

    # Subscriber: build the bm25 index now (logical only replicates
    # the heap).
    psql -p "${LOGICAL_SUB_PORT}" -d "${TEST_DB}" -c "
        CREATE INDEX IF NOT EXISTS f1_sub_idx ON f1_docs
            USING bm25(content) WITH (text_config='simple');
    " >/dev/null

    local primary_count standby_count subscriber_count
    primary_count=$(search_count "${PRIMARY_PORT}" f1_docs content \
        "alpha" "f1_idx")
    standby_count=$(search_count "${STANDBY_PORT}" f1_docs content \
        "alpha" "f1_idx")
    subscriber_count=$(search_count "${LOGICAL_SUB_PORT}" f1_docs \
        content "alpha" "f1_sub_idx")

    log "  primary=${primary_count} standby=${standby_count} \
subscriber=${subscriber_count}"

    if [ "${primary_count}" != "3" ] || \
       [ "${standby_count}" != "3" ] || \
       [ "${subscriber_count}" != "3" ]; then
        error "F1: counts disagree across nodes"
    fi
    log "F1 PASSED"
}

# -------------------------------------------------------------------
# F2: TimescaleDB hypertable replication.
# Hypertable on primary has a bm25 index. Long-lived standby backend
# should see new inserts to recent chunks.
# Expected: FAIL today when actually run (memtable staleness on
# hypertable inserts).
# Skipped if TimescaleDB is not available OR not loaded via
# shared_preload_libraries. Today the test is typically SKIPPED on
# pg18-release because setup_primary in replication_lib.sh sets
# SPL='pg_textsearch' only.
# -------------------------------------------------------------------
test_timescaledb_hypertable_replication_long_lived() {
    log "=== F2: TimescaleDB hypertable (should FAIL today) ==="

    local has_ts
    has_ts=$(psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c "
        SELECT count(*) FROM pg_available_extensions
        WHERE name='timescaledb';" 2>/dev/null)
    if [ "${has_ts}" != "1" ]; then
        warn "F2: TimescaleDB not available, skipping"
        return 0
    fi

    # Try to actually load it. Fails if timescaledb is not in
    # shared_preload_libraries on the primary.
    if ! psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" \
         -c "CREATE EXTENSION IF NOT EXISTS timescaledb;" \
         >/dev/null 2>&1; then
        warn "F2: TimescaleDB present but not in \
shared_preload_libraries — skipping. To actually exercise this \
test, add timescaledb to shared_preload_libraries in setup_primary \
(replication_lib.sh)."
        return 0
    fi

    primary_sql "
        DROP TABLE IF EXISTS f2_events CASCADE;
        CREATE TABLE f2_events (
            ts TIMESTAMPTZ NOT NULL,
            content TEXT
        );
        SELECT create_hypertable('f2_events', 'ts',
            chunk_time_interval => INTERVAL '1 day');
        INSERT INTO f2_events VALUES
            (now() - INTERVAL '12 hours', 'old event alpha'),
            (now() - INTERVAL '1 hour',   'recent event alpha');
        CREATE INDEX f2_idx ON f2_events USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    local query="
        SELECT count(*) FROM (SELECT ts FROM f2_events
            ORDER BY content <@> to_bm25query('alpha','f2_idx')
            LIMIT 100) t;
    "
    long_lived_before_after "${STANDBY_PORT}" "${query}" \
        "primary_sql \"
            INSERT INTO f2_events VALUES
                (now(), 'brand new event alpha bravo');
        \" >/dev/null
        wait_for_standby_catchup"

    log "  before=${LL_BEFORE}, after=${LL_AFTER}"
    if [[ "${LL_AFTER}" == *ERROR* ]] || \
       ! [[ "${LL_AFTER}" =~ ^[0-9]+$ ]] || \
       [ "${LL_AFTER}" -le "${LL_BEFORE}" ]; then
        error "F2 BUG (expected): hypertable standby long-lived \
backend missed primary insert (before=${LL_BEFORE}, after=${LL_AFTER})"
    fi
    log "F2 PASSED"
}

# -------------------------------------------------------------------
# F3: pg_basebackup with bm25 index files.
# Standby was already created via pg_basebackup. Verify the bm25
# index file made it into the backup and is queryable on the
# standby. (This is a sanity check that segment files are not
# excluded from basebackup.)
# Expected: PASS today.
# -------------------------------------------------------------------
test_pg_basebackup_with_bm25_index() {
    log "=== F3: pg_basebackup includes bm25 index files ==="

    # Use the F1 table+index that's already on primary+standby.
    # Spill primary so segment files exist on disk.
    primary_sql "SELECT bm25_spill_index('f1_idx');" >/dev/null
    wait_for_standby_catchup

    # Wipe standby and rebuild from basebackup.
    pg_ctl stop -D "${STANDBY_DIR}" -m fast 2>/dev/null || true
    rm -rf "${STANDBY_DIR}"
    setup_standby
    wait_for_standby_catchup

    local count
    count=$(search_count "${STANDBY_PORT}" f1_docs content "alpha" \
        "f1_idx")
    log "  re-basebackuped standby count: ${count}"
    if [ "${count}" != "3" ]; then
        error "F3: re-basebackuped standby missing data (got ${count})"
    fi
    log "F3 PASSED"
}

main() {
    log "Starting pg_textsearch replication compat tests..."
    check_required_tools
    setup_primary
    setup_standby
    test_logical_and_physical_coexist
    test_timescaledb_hypertable_replication_long_lived
    test_pg_basebackup_with_bm25_index

    log "All replication compat tests completed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
