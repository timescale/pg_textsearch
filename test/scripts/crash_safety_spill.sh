#!/bin/bash
#
# Crash safety test for memtable spill ordering
#
# Verifies that the finalize-before-mark-dead ordering prevents FSM
# corruption when a crash occurs during spill. After recovery, VACUUM
# should not free pages still in the live chain, and subsequent
# inserts/spills should not corrupt the index.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55437
TEST_DB=crash_safety_spill_test
DATA_DIR="${SCRIPT_DIR}/../tmp_crash_safety_spill"
LOGFILE="${DATA_DIR}/postgres.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    log "Cleaning up..."
    pkill -f "postgres.*-D.*tmp_crash_safety_spill" 2>/dev/null || true
    sleep 1
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up PostgreSQL instance..."
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
shared_preload_libraries = 'pg_textsearch'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
fsync = on
full_page_writes = on
wal_level = replica
max_wal_senders = 2
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w -o "-p ${TEST_PORT}" || \
        error "Failed to start PostgreSQL"

    createdb -p ${TEST_PORT} ${TEST_DB}

    psql -p ${TEST_PORT} -d ${TEST_DB} -c "CREATE EXTENSION pg_textsearch;" >/dev/null
    log "Test database ready"
}

run_test() {
    log "Creating test table and index..."
    psql -p ${TEST_PORT} -d ${TEST_DB} <<EOF >/dev/null
CREATE TABLE docs (id serial PRIMARY KEY, content text);
CREATE INDEX docs_bm25 ON docs USING bm25(content) WITH (text_config = 'english');
EOF

    log "Inserting initial data..."
    psql -p ${TEST_PORT} -d ${TEST_DB} <<EOF >/dev/null
INSERT INTO docs (content)
SELECT 'document number ' || i || ' with some searchable content words'
FROM generate_series(1, 1000) i;
EOF

    log "Forcing spill to create segment..."
    psql -p ${TEST_PORT} -d ${TEST_DB} -c "SELECT bm25_spill_index('docs_bm25');" >/dev/null

    log "Verifying initial data..."
    local count1=$(psql -p ${TEST_PORT} -d ${TEST_DB} -t -c \
        "SELECT count(*) FROM (SELECT 1 FROM docs ORDER BY content <@> to_bm25query('document', 'docs_bm25') LIMIT 1000) sub;")
    count1=$(echo $count1 | tr -d ' ')
    [ "$count1" -eq "1000" ] || error "Initial count mismatch: expected 1000, got $count1"
    log "Initial verification passed: $count1 documents"

    log "Inserting data and triggering crash during spill (same transaction)..."
    psql -p ${TEST_PORT} -d ${TEST_DB} <<EOF 2>&1 | grep -q "PANIC" || true
BEGIN;
INSERT INTO docs (content)
SELECT 'batch two document ' || i || ' more searchable text here'
FROM generate_series(1, 500) i;
SET pg_textsearch.debug_panic_after_spill_finalize = true;
SELECT bm25_spill_index('docs_bm25');
COMMIT;
EOF

    # Wait for PANIC to propagate
    sleep 1

    # Force stop any remaining server processes after PANIC
    log "Stopping server after PANIC..."
    pg_ctl stop -D "${DATA_DIR}" -m immediate 2>/dev/null || true
    sleep 1

    # Clean up stale pid file if still present
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        local old_pid=$(head -1 "${DATA_DIR}/postmaster.pid")
        if ! kill -0 $old_pid 2>/dev/null; then
            rm -f "${DATA_DIR}/postmaster.pid"
        else
            # Force kill if still running
            kill -9 $old_pid 2>/dev/null || true
            sleep 1
            rm -f "${DATA_DIR}/postmaster.pid"
        fi
    fi

    log "Restarting after crash..."
    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w -o "-p ${TEST_PORT}" || \
        error "Failed to restart PostgreSQL after crash"

    log "Running VACUUM to trigger FSM reclaim..."
    psql -p ${TEST_PORT} -d ${TEST_DB} -c "VACUUM docs;" >/dev/null

    log "Inserting more data after recovery..."
    psql -p ${TEST_PORT} -d ${TEST_DB} <<EOF >/dev/null
INSERT INTO docs (content)
SELECT 'post recovery document ' || i || ' additional searchable text'
FROM generate_series(1, 500) i;
EOF

    log "Forcing another spill..."
    psql -p ${TEST_PORT} -d ${TEST_DB} -c "SELECT bm25_spill_index('docs_bm25');" >/dev/null

    log "Verifying data integrity..."
    # 1000 initial + 500 post-recovery = 1500
    # The 500 pre-crash docs are rolled back (same transaction as PANIC)
    local count2=$(psql -p ${TEST_PORT} -d ${TEST_DB} -t -c \
        "SELECT count(*) FROM (SELECT 1 FROM docs ORDER BY content <@> to_bm25query('document', 'docs_bm25') LIMIT 2000) sub;")
    count2=$(echo $count2 | tr -d ' ')
    [ "$count2" -eq "1500" ] || error "Final count mismatch: expected 1500, got $count2"

    log "Final verification passed: $count2 documents"
    log "TEST PASSED: Crash safety spill ordering verified"
}

# Main
pkill -f "postgres.*-D.*tmp_crash_safety_spill" 2>/dev/null || true
sleep 1

setup_test_db
run_test

log "All tests passed!"
exit 0
