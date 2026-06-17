#!/bin/bash
#
# Regression test for the VACUUM-vs-merge race (issue #411): the index
# bulk-delete (tp_bulkdelete) and cleanup (tp_vacuumcleanup) paths walked
# segments by block number while reading the metapage snapshot outside the
# per-index LWLock, so a concurrent spill/merge could free and recycle those
# blocks between identifying a segment and reopening it.  This produced
# "invalid segment header" errors ("while cleaning up index" / "while
# vacuuming index") and, when a recycled block still passed the header magic
# check, an out-of-bounds alive-bitset write (TRAP: failed
# Assert("doc_id < bitset->num_docs"), or a SIGSEGV in release builds).
#
# Writers spill many small segments, a merger force-merges them, a deleter
# creates dead tuples, and a vacuumer runs VACUUM concurrently (autovacuum
# is also aggressive).  Before the fix this fails within seconds; after it,
# it completes cleanly with no segment-header errors and no crash.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55442
TEST_DB=vacuum_concurrent_merge_test
DATA_DIR="${SCRIPT_DIR}/../tmp_vacuum_concurrent_merge"
LOGFILE="${DATA_DIR}/postgres.log"
ERR_DIR="${DATA_DIR}/client_logs"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    local exit_code=$?
    log "Cleaning up (exit code: $exit_code)..."
    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up PostgreSQL instance..."
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1
    mkdir -p "${ERR_DIR}"

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_textsearch'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
autovacuum = on
autovacuum_naptime = 1s
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w -o "-p ${TEST_PORT}" || \
        error "Failed to start PostgreSQL"

    createdb -h "${DATA_DIR}" -p ${TEST_PORT} ${TEST_DB}
    psql -h "${DATA_DIR}" -p ${TEST_PORT} -d ${TEST_DB} \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
    log "Test database ready"
}

PSQL="psql -h ${DATA_DIR} -p ${TEST_PORT} -d ${TEST_DB} -qAt -v ON_ERROR_STOP=1"

seed_data() {
    log "Creating bm25 index and seeding data..."
    $PSQL <<'SQL' >/dev/null
SET client_min_messages=warning;
CREATE TABLE docs (id bigserial PRIMARY KEY, body text NOT NULL);
CREATE INDEX docs_bm25 ON docs USING bm25(body) WITH (text_config='english');
-- Force frequent autovacuum bulk-delete on this table.
ALTER TABLE docs SET (autovacuum_vacuum_scale_factor=0,
                      autovacuum_vacuum_threshold=200);
INSERT INTO docs (body)
SELECT 'seed postgres bm25 ' || md5(gs::text)
FROM generate_series(1, 2000) gs;
SELECT bm25_spill_index('docs_bm25');
SQL
}

# Spill the memtable aggressively so many small segments accrue for the
# merger to compact.
writer() {
    for i in $(seq 1 120); do
        $PSQL -c "SET pg_textsearch.memtable_pages_threshold=4;
          SET statement_timeout='60s'; SET lock_timeout='30s';
          INSERT INTO docs (body)
          SELECT 'w postgres bm25 ' || md5(random()::text)
          FROM generate_series(1, 200) gs;
          SELECT bm25_spill_index('docs_bm25')" \
          >>"${ERR_DIR}/writer.log" 2>&1 || return 10
    done
}

# Concurrent spill + force-merge: the LW_EXCLUSIVE segment recycler that
# races VACUUM.
merger() {
    for i in $(seq 1 250); do
        $PSQL -c "SELECT bm25_spill_index('docs_bm25');
                  SELECT bm25_force_merge('docs_bm25')" \
          >>"${ERR_DIR}/merger.log" 2>&1 || return 30
    done
}

# Delete the oldest rows by id (monotonic -> no row-lock deadlocks) to
# create dead tuples for VACUUM bulk-delete to reclaim.
deleter() {
    for i in $(seq 1 250); do
        $PSQL -c "DELETE FROM docs
                  WHERE id IN (SELECT id FROM docs ORDER BY id ASC LIMIT 120)" \
          >>"${ERR_DIR}/deleter.log" 2>&1 || return 40
        sleep 0.1
    done
}

# Explicit VACUUM in a loop deterministically exercises the bulk-delete and
# cleanup paths concurrently with the merger.
vacuumer() {
    for i in $(seq 1 200); do
        $PSQL -c "VACUUM docs" >>"${ERR_DIR}/vacuumer.log" 2>&1 || return 50
        sleep 0.05
    done
}

run_test() {
    log "Running concurrent writer + merger + deleter + vacuumer..."

    writer & w_pid=$!
    merger & m_pid=$!
    deleter & d_pid=$!
    vacuumer & v_pid=$!

    local failed=0
    wait $w_pid || { warn "writer failed"; failed=1; }
    wait $m_pid || { warn "merger failed"; failed=1; }
    wait $d_pid || { warn "deleter failed"; failed=1; }
    wait $v_pid || { warn "vacuumer failed"; failed=1; }

    # The bug surfaces both server-side (autovacuum) and client-side
    # (explicit VACUUM).  Check both the server log and client logs.
    if grep -RIl "invalid segment header" "${ERR_DIR}" "${LOGFILE}" \
        >/dev/null 2>&1; then
        warn "Found 'invalid segment header':"
        grep -RIn "invalid segment header" "${ERR_DIR}" "${LOGFILE}" | head -5
        error "TEST FAILED: issue #411 reproduced (invalid segment header)"
    fi

    # An out-of-bounds alive-bitset write crashes the backend.
    if grep -qE "TRAP: failed Assert|was terminated by signal|server closed the connection unexpectedly" \
        "${LOGFILE}" "${ERR_DIR}"/*.log 2>/dev/null; then
        warn "Found a crash signature:"
        grep -hE "TRAP: failed Assert|was terminated by signal|alive_bitset" \
            "${LOGFILE}" 2>/dev/null | head -5
        error "TEST FAILED: issue #411 reproduced (backend crash)"
    fi

    if [ "$failed" -ne 0 ]; then
        warn "Client logs:"
        tail -n 20 "${ERR_DIR}"/*.log 2>/dev/null || true
        error "TEST FAILED: a concurrent client exited with an error"
    fi

    log "TEST PASSED: VACUUM survived concurrent spill/merge"
}

# Main
setup_test_db
seed_data
run_test

log "All tests passed!"
exit 0
