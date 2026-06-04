#!/bin/bash
#
# Regression test for issue #404: the standalone <@> scoring operator
# (used on a seqscan) opened segment pages without the per-index LWLock,
# so a concurrent spill/merge could free and recycle those blocks under
# the reader, yielding "invalid segment header".
#
# The reader forces the standalone path (enable_indexscan=off) on a
# partial index while writers and spill/merge run concurrently. Before the
# fix this fails within seconds; after it, it completes cleanly.
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

    # The test only covers #404 if the reader runs as a Seq Scan
    # (standalone <@> scoring); fail loudly if the planner picks otherwise.
    local plan
    plan=$($PSQL -c "SET enable_indexscan=off; SET enable_bitmapscan=off;
        EXPLAIN (COSTS off)
        SELECT id FROM docs WHERE lang='de'
        ORDER BY content <@> to_bm25query('postgres bm25', 'docs_de_bm25')
        LIMIT 20")
    if ! echo "$plan" | grep -qi "Seq Scan"; then
        warn "Reader plan was not a Seq Scan:"
        echo "$plan"
        error "TEST SETUP FAILED: reader no longer exercises the standalone path"
    fi
}

writer() {
    for i in $(seq 1 120); do
        $PSQL -c "SET statement_timeout='60s'; SET lock_timeout='30s';
          INSERT INTO docs (content, lang)
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

# "Partial" is incidental: the trigger is the standalone <@> seqscan path
# (forced by enable_indexscan=off), not any partial-index-specific code.
reader() {
    local tag=$1
    for i in $(seq 1 300); do
        $PSQL -c "SET enable_indexscan=off; SET enable_bitmapscan=off;
          SET statement_timeout='60s'; SET lock_timeout='30s';
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
