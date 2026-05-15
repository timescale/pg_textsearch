#!/bin/bash
#
# wal_audit.sh — single-node primary checks of what we put on the
# WAL stream. Three tests:
#
#   3. UNLOGGED bm25 index emits zero pg_textsearch records.
#      Vestigial check: after issue #374 deleted the custom rmgr,
#      there is no pg_textsearch resource manager registered, so
#      this is trivially true. Kept to catch regressions if a
#      future change reintroduces a custom rmgr without honoring
#      RelationNeedsWAL(rel).
#
#   4. LOGGED bm25 inserts and spills emit ONLY Generic (page-delta)
#      WAL records — no custom rmgr 149. Verifies the on-disk
#      memtable + GenericXLog design (issue #374): every page
#      mutation goes through GenericXLog so stock PostgreSQL replay
#      (including the single-page WAL-redo helper) can reconstruct
#      every page without loading pg_textsearch.so.
#
#   5. Crash + restart: the index is queryable post-recovery. The
#      on-disk memtable chain pages are themselves the durable
#      record — no docid-page bootstrap is needed.
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
# Test 4: LOGGED inserts/spill emit ONLY Generic records — no
# custom rmgr 149. This is the core walredo-compatibility check.
# ---------------------------------------------------------------
test_logged_records_described_by_name() {
    log "=== Test 4: logged ops use only Generic WAL records ==="

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

    # After issue #374 no custom resource manager is registered.
    # We expect ZERO pg_textsearch-named records and a positive
    # count of Generic records covering the metapage, chain
    # pages, and segment header.
    local custom_count
    custom_count=$(count_records "${lsn_before}" "${lsn_after}")
    log "  custom pg_textsearch records: ${custom_count} (expect 0)"
    if [ "${custom_count}" != "0" ]; then
        error "Test 4: expected 0 pg_textsearch records (custom \
rmgr was deleted in issue #374), got '${custom_count}'. A custom \
WAL record path was reintroduced."
    fi

    local generic_count
    generic_count=$(sql "
        SELECT count(*) FROM
            pg_get_wal_records_info('${lsn_before}', '${lsn_after}')
        WHERE resource_manager = 'Generic';")
    log "  Generic records: ${generic_count} (expect >0)"
    if [ "${generic_count}" -lt 1 ]; then
        error "Test 4: expected ≥1 Generic record for the on-disk \
memtable mutations, got '${generic_count}'. The GenericXLog write \
path is silent."
    fi
    log "Test 4 PASSED: logged ops emit only Generic records"
}

# ---------------------------------------------------------------
# Test 5: Crash + restart — the index is queryable post-recovery.
# The on-disk memtable chain pages + segment pages are themselves
# the durable record; stock PostgreSQL replay reconstructs them.
# ---------------------------------------------------------------
test_crash_replay() {
    log "=== Test 5: crash + restart preserves the index ==="

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

    # Insert again; these records ride the WAL only (no checkpoint
    # has flushed the dirty buffers), so on crash the post-CP docs
    # are recoverable only via WAL replay.
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

    # Recovery completed. The post-CP records were Generic
    # (page-delta) records. Verify there are no pg_textsearch
    # custom records in that LSN window — they must not exist
    # after issue #374.
    local custom_count
    custom_count=$(count_records "${lsn_after_checkpoint}" \
        "${lsn_pre_crash}")
    log "  custom pg_textsearch records across crash window: ${custom_count} (expect 0)"
    if [ "${custom_count}" != "0" ]; then
        error "Test 5: expected 0 custom records across the \
crash-recovery LSN window (issue #374 deleted the custom rmgr), \
got '${custom_count}'"
    fi

    # Index must be functional post-recovery and contain all 60
    # docs (30 precrash + 30 postcheckpoint).
    local total_docs
    total_docs=$(sql "
        SELECT count(*) FROM (
            SELECT 1 FROM c_docs
            ORDER BY content <@> to_bm25query('precrash OR postcheckpoint', 'c_docs_idx')
        ) sub;")
    log "  post-recovery doc count: ${total_docs:-<none>} (expect 60)"
    if [ "${total_docs:-0}" -lt 60 ]; then
        error "Test 5: expected ≥60 docs queryable post-recovery, \
got '${total_docs}'"
    fi
    log "Test 5 PASSED: WAL recovery preserves the index"
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
