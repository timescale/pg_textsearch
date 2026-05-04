#!/bin/bash
#
# wal_audit.sh — single-node primary checks of what we put on the
# WAL stream. Three tests:
#
#   3. UNLOGGED bm25 index emits zero pg_textsearch records.
#      Verifies the RelationNeedsWAL(rel) gate at every spill /
#      insert emission site.
#
#   4. LOGGED bm25 inserts and spills produce records that
#      pg_walinspect describes by name (INSERT_TERMS, SPILL).
#      Verifies tp_rmgr_desc / tp_rmgr_identify produce the right
#      output — the only path that exercises them in a normal
#      Postgres install.
#
#   5. Crash + restart: WAL retains our records and the index is
#      queryable post-recovery. Distinct from recovery.sh because
#      we look in pg_walinspect at the LSN range that crossed the
#      crash to confirm the records were on the wire.
#
# Tests use pg_walinspect (contrib in PG15+). All work happens in
# the regression's own database; no replication setup needed.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55462
TEST_DB=wal_audit
DATA_DIR="${SCRIPT_DIR}/../tmp_wal_audit"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m fast >/dev/null 2>&1 || \
            pg_ctl stop -D "${DATA_DIR}" -m immediate >/dev/null 2>&1 || true
    fi
    rm -rf "${DATA_DIR}"
}
trap cleanup EXIT INT TERM

setup_node() {
    log "Initializing primary on port ${TEST_PORT}..."
    rm -rf "${DATA_DIR}"
    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1
    cat >> "${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
unix_socket_directories = '/tmp'
shared_buffers = 128MB
shared_preload_libraries = 'pg_textsearch'
wal_level = replica
logging_collector = on
log_filename = 'postgres.log'
EOF
    pg_ctl start -D "${DATA_DIR}" -l "${DATA_DIR}/postgres.log" -w \
        >/dev/null
    createdb -h /tmp -p "${TEST_PORT}" "${TEST_DB}"
    psql -h /tmp -p "${TEST_PORT}" -d "${TEST_DB}" -c \
        "CREATE EXTENSION pg_textsearch; \
         CREATE EXTENSION pg_walinspect;" >/dev/null
}

sql() {
    psql -h /tmp -p "${TEST_PORT}" -d "${TEST_DB}" -tA -c "$1" 2>&1
}

# Count pg_textsearch WAL records between two LSNs. Argument is an
# optional record_type filter.
count_records() {
    local start_lsn=$1
    local end_lsn=$2
    local filter=${3:-}
    local where="resource_manager = 'pg_textsearch'"
    [ -n "$filter" ] && where="${where} AND record_type = '${filter}'"
    sql "
        SELECT count(*) FROM
            pg_get_wal_records_info('${start_lsn}', '${end_lsn}')
        WHERE ${where};"
}

# ---------------------------------------------------------------
# Test 3: UNLOGGED bm25 index emits zero pg_textsearch records.
# ---------------------------------------------------------------
test_unlogged_emits_no_records() {
    log "=== Test 3: UNLOGGED bm25 emits zero pg_textsearch WAL ==="

    local lsn_before
    lsn_before=$(sql "SELECT pg_current_wal_lsn();")

    sql "
        CREATE UNLOGGED TABLE u_docs (
            id SERIAL PRIMARY KEY, content TEXT NOT NULL);
        CREATE INDEX u_docs_idx ON u_docs USING bm25(content)
            WITH (text_config='english');
        INSERT INTO u_docs (content)
            SELECT 'unlogged ' || md5(g::text)
            FROM generate_series(1, 200) g;
        SELECT bm25_spill_index('u_docs_idx');
    " >/dev/null

    local lsn_after
    lsn_after=$(sql "SELECT pg_current_wal_lsn();")

    local count
    count=$(count_records "${lsn_before}" "${lsn_after}")
    log "  pg_textsearch records emitted for UNLOGGED ops: ${count}"
    if [ "${count}" != "0" ]; then
        error "Test 3: UNLOGGED bm25 emitted ${count} pg_textsearch \
records (expected 0). RelationNeedsWAL guard is broken."
    fi
    log "Test 3 PASSED: UNLOGGED emits zero pg_textsearch records"
}

# ---------------------------------------------------------------
# Test 4: LOGGED inserts/spill produce records pg_walinspect can
# describe by name.
# ---------------------------------------------------------------
test_logged_records_described_by_name() {
    log "=== Test 4: pg_walinspect describes our records by name ==="

    sql "
        CREATE TABLE l_docs (
            id SERIAL PRIMARY KEY, content TEXT NOT NULL);
        CREATE INDEX l_docs_idx ON l_docs USING bm25(content)
            WITH (text_config='english');
    " >/dev/null

    local lsn_before
    lsn_before=$(sql "SELECT pg_current_wal_lsn();")

    sql "
        INSERT INTO l_docs (content)
            SELECT 'logged ' || md5(g::text)
            FROM generate_series(1, 50) g;
        SELECT bm25_spill_index('l_docs_idx');
    " >/dev/null

    local lsn_after
    lsn_after=$(sql "SELECT pg_current_wal_lsn();")

    local insert_terms_count
    local spill_count
    insert_terms_count=$(count_records "${lsn_before}" "${lsn_after}" \
        "INSERT_TERMS")
    spill_count=$(count_records "${lsn_before}" "${lsn_after}" \
        "SPILL")
    log "  INSERT_TERMS records: ${insert_terms_count} (expect 50)"
    log "  SPILL records:        ${spill_count} (expect 1)"

    if [ "${insert_terms_count}" != "50" ]; then
        error "Test 4: expected 50 INSERT_TERMS records, got \
'${insert_terms_count}'. tp_rmgr_identify may be returning the \
wrong name for XLOG_TP_INSERT_TERMS."
    fi
    if [ "${spill_count}" != "1" ]; then
        error "Test 4: expected 1 SPILL record, got '${spill_count}'."
    fi
    log "Test 4 PASSED: rm_desc / rm_identify produce expected names"
}

# ---------------------------------------------------------------
# Test 5: Crash + restart — our WAL records persist across the
# crash and the index is queryable after recovery.
# ---------------------------------------------------------------
test_crash_replay() {
    log "=== Test 5: crash + restart preserves WAL records ==="

    sql "
        CREATE TABLE c_docs (
            id SERIAL PRIMARY KEY, content TEXT NOT NULL);
        CREATE INDEX c_docs_idx ON c_docs USING bm25(content)
            WITH (text_config='english');
        INSERT INTO c_docs (content)
            SELECT 'precrash ' || md5(g::text)
            FROM generate_series(1, 30) g;
        CHECKPOINT;
    " >/dev/null

    local lsn_after_checkpoint
    lsn_after_checkpoint=$(sql "SELECT pg_current_wal_lsn();")

    # Insert again; these records ride the WAL only — no checkpoint
    # has flushed the memtable, so on crash the docs are recoverable
    # only via WAL replay or docid pages.
    sql "
        INSERT INTO c_docs (content)
            SELECT 'postcheckpoint ' || md5(g::text)
            FROM generate_series(31, 60) g;
    " >/dev/null

    local lsn_pre_crash
    lsn_pre_crash=$(sql "SELECT pg_current_wal_lsn();")

    # Crash the postmaster — kill -9 the process group leader.
    local pid
    pid=$(head -n 1 "${DATA_DIR}/postmaster.pid")
    log "  killing postmaster (pid ${pid})"
    kill -9 "${pid}" 2>/dev/null || true
    # Make sure all postmaster children are gone before restart.
    sleep 2
    pkill -9 -f "postgres .*${DATA_DIR}" 2>/dev/null || true
    sleep 1

    log "  restarting..."
    pg_ctl start -D "${DATA_DIR}" -l "${DATA_DIR}/postgres.log" -w \
        >/dev/null

    # Recovery completed. Verify our INSERT_TERMS records were on
    # the WAL stream that crossed the crash (i.e., the LSN window
    # between the last checkpoint and the pre-crash LSN).
    local crashed_count
    crashed_count=$(count_records "${lsn_after_checkpoint}" \
        "${lsn_pre_crash}" "INSERT_TERMS")
    log "  INSERT_TERMS records replayed across crash: ${crashed_count}"
    if [ "${crashed_count}" -lt 30 ]; then
        error "Test 5: expected ≥30 INSERT_TERMS records in the \
post-checkpoint pre-crash WAL window, got '${crashed_count}'"
    fi

    # Index must be queryable post-recovery, with all 60 docs.
    local count
    count=$(sql "
        SELECT count(*) FROM (
            SELECT id FROM c_docs
            ORDER BY content <@> to_bm25query('precrash',
                'c_docs_idx')
            LIMIT 100000
        ) t;")
    log "  post-recovery doc count: ${count}"
    if [ "${count}" != "60" ]; then
        error "Test 5: expected 60 docs queryable post-recovery, \
got '${count}'"
    fi
    log "Test 5 PASSED: WAL records survive crash, index recovers"
}

main() {
    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"
    command -v initdb >/dev/null 2>&1 || error "initdb not found"

    setup_node

    # Subshell each test so one failure doesn't short-circuit the
    # rest — verify-coverage workflow needs every test to report.
    local failures=""
    ( test_unlogged_emits_no_records ) || \
        failures="${failures} test_unlogged_emits_no_records"
    ( test_logged_records_described_by_name ) || \
        failures="${failures} test_logged_records_described_by_name"
    ( test_crash_replay ) || \
        failures="${failures} test_crash_replay"

    if [ -n "${failures}" ]; then
        error "wal_audit failures:${failures}"
    fi
    log "All wal-audit tests passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
