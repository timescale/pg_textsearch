#!/bin/bash
#
# Regression test for issue #404:
#   "Partial BM25 reads can fail with invalid segment header during
#    concurrent matching writes and spill/merge"
#
# The standalone <@> scoring operator (used when the planner picks a
# sequential scan instead of an index scan) opened segment pages by block
# number WITHOUT holding the per-index LWLock. A concurrent spill/merge
# (LW_EXCLUSIVE) frees old segment pages and the memtable recycles those
# blocks, so the unlocked reader could read a recycled memtable page where
# a segment header was expected:
#
#   ERROR: invalid segment header at block N
#   DETAIL: magic=0x5450544D, expected 0x54505347
#
# The reader below forces the standalone path (enable_indexscan=off) on a
# selective partial index while writers and spill/merge run concurrently.
# Before the fix this fails within a few seconds; after the fix it must
# complete cleanly.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55441
TEST_DB=partial_concurrent_read_test
DATA_DIR="${SCRIPT_DIR}/../tmp_partial_concurrent_read"
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
    log "Creating selective partial index and seeding data..."
    $PSQL <<'SQL' >/dev/null
SET client_min_messages=warning;
CREATE TABLE docs (id bigserial PRIMARY KEY, content text NOT NULL, lang text NOT NULL);
CREATE INDEX docs_de_bm25 ON docs USING bm25(content)
    WITH (text_config='german') WHERE lang = 'de';
INSERT INTO docs (content, lang)
SELECT repeat('postgres bm25 search ', 50) || gs,
       CASE WHEN gs % 2 = 0 THEN 'de' ELSE 'en' END
FROM generate_series(1, 4000) gs;
SELECT bm25_spill_index('docs_de_bm25');
SELECT bm25_force_merge('docs_de_bm25');
SQL
}

writer() {
    for i in $(seq 1 120); do
        $PSQL -c "INSERT INTO docs (content, lang)
          SELECT 'writer postgres bm25 ' || gs, 'de'
          FROM generate_series(1, 30) gs" \
          >>"${ERR_DIR}/writer.log" 2>&1 || return 10
    done
}

merger() {
    for i in $(seq 1 200); do
        $PSQL -c "SELECT bm25_spill_index('docs_de_bm25');
                  SELECT bm25_force_merge('docs_de_bm25')" \
          >>"${ERR_DIR}/merger.log" 2>&1 || return 30
    done
}

# Forces the standalone (<@> operator) scoring path via enable_indexscan=off.
reader() {
    local tag=$1
    for i in $(seq 1 300); do
        $PSQL -c "SET enable_indexscan=off; SET enable_bitmapscan=off;
          SELECT count(*) FROM (
            SELECT id FROM docs WHERE lang='de'
            ORDER BY content <@> to_bm25query('postgres bm25', 'docs_de_bm25')
            LIMIT 20
          ) s" >>"${ERR_DIR}/reader_${tag}.log" 2>&1 || return 40
    done
}

run_test() {
    log "Running concurrent writer + merger + readers (standalone path)..."

    writer & w_pid=$!
    merger & m_pid=$!
    reader a & ra_pid=$!
    reader b & rb_pid=$!

    local failed=0
    wait $w_pid || { warn "writer failed"; failed=1; }
    wait $m_pid || { warn "merger failed"; failed=1; }
    wait $ra_pid || { warn "reader a failed"; failed=1; }
    wait $rb_pid || { warn "reader b failed"; failed=1; }

    # Surface the actual error for diagnosis.
    if grep -RIl "invalid segment header" "${ERR_DIR}" >/dev/null 2>&1; then
        warn "Found 'invalid segment header' in client logs:"
        grep -RIn "invalid segment header" "${ERR_DIR}" | head -5
        error "TEST FAILED: issue #404 reproduced (invalid segment header)"
    fi

    if [ "$failed" -ne 0 ]; then
        warn "Client logs:"
        tail -n 20 "${ERR_DIR}"/*.log 2>/dev/null || true
        error "TEST FAILED: a concurrent client exited with an error"
    fi

    log "TEST PASSED: partial-index standalone reads survived concurrent spill/merge"
}

# Main
setup_test_db
seed_data
run_test

log "All tests passed!"
exit 0
