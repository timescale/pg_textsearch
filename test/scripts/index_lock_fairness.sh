#!/bin/bash
#
# Deterministic regression test for writer starvation at the per-index LWLock.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
export PATH="${PGBINDIR}:${PATH}"
TEST_PORT=55460
TEST_DB=index_lock_fairness_test
DATA_DIR="${SCRIPT_DIR}/../tmp_index_lock_fairness"
SOCKET_DIR="${SCRIPT_DIR}/.fair_sock"
LOGFILE="${DATA_DIR}/postgres.log"
ERR_DIR="${DATA_DIR}/client_logs"
KEEP_DIR="${SCRIPT_DIR}/../tmp_index_lock_fairness_logs"
PAUSE_MS=5000

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
fail() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    local exit_code=$?
    local pid

    trap - EXIT INT TERM
    for pid in $(jobs -p); do
        kill "${pid}" 2>/dev/null || true
    done
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate >/dev/null 2>&1 || true
    fi
    if [ "${exit_code}" -ne 0 ] && [ -d "${DATA_DIR}" ]; then
        rm -rf "${KEEP_DIR}"
        mkdir -p "${KEEP_DIR}"
        cp "${LOGFILE}" "${KEEP_DIR}/" 2>/dev/null || true
        cp -r "${ERR_DIR}" "${KEEP_DIR}/" 2>/dev/null || true
        warn "Preserved logs in ${KEEP_DIR}"
    fi
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    exit "${exit_code}"
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up PostgreSQL instance..."
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}" "${KEEP_DIR}"
    mkdir -p "${DATA_DIR}" "${SOCKET_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1
    mkdir -p "${ERR_DIR}"

    cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
unix_socket_directories = '${SOCKET_DIR}'
listen_addresses = ''
shared_preload_libraries = 'pg_textsearch'
logging_collector = off
log_min_messages = log
log_line_prefix = '%m [%p] %a '
autovacuum = off
pg_textsearch.memtable_pages_threshold = 0
pg_textsearch.bulk_load_threshold = 0
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w ||
        fail "PostgreSQL startup failed"

    createdb -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

PSQL="psql -h ${SOCKET_DIR} -p ${TEST_PORT} -d ${TEST_DB} -qAt -v ON_ERROR_STOP=1"

seed_data() {
    log "Creating BM25 index and seeding ranked-scan data..."
    $PSQL <<'SQL' >/dev/null
CREATE TABLE docs (id bigserial PRIMARY KEY, content text NOT NULL);
CREATE INDEX docs_bm25 ON docs USING bm25(content)
    WITH (text_config='english');
INSERT INTO docs (content)
SELECT 'alpha beta gamma document ' || gs || ' ' || repeat(md5(gs::text), 4)
FROM generate_series(1, 30000) gs;
SELECT bm25_spill_index('docs_bm25');
INSERT INTO docs (content)
SELECT 'alpha beta pending spill document ' || gs
FROM generate_series(1, 300) gs;
SQL

    local plan
    plan=$($PSQL -c "EXPLAIN (COSTS off)
        SELECT id FROM docs
        ORDER BY content <@> to_bm25query('alpha beta', 'docs_bm25')
        LIMIT 20000")
    if ! grep -qi "Index Scan" <<<"${plan}"; then
        warn "Reader plan was not an Index Scan:"
        echo "${plan}"
        fail "test setup did not exercise ranked index scans"
    fi
}

backend_pid() {
    local app_name=$1
    local deadline=$((SECONDS + 10))
    local pid

    while ((SECONDS < deadline)); do
        pid=$($PSQL -c "
            SELECT pid
              FROM pg_stat_activity
             WHERE application_name = '${app_name}'
             ORDER BY backend_start DESC
             LIMIT 1;" 2>/dev/null || true)
        if [[ "${pid}" =~ ^[0-9]+$ ]]; then
            echo "${pid}"
            return
        fi
        sleep 0.05
    done
    fail "backend ${app_name} did not appear within 10 seconds"
}

wait_for_writer_marker() {
    local oid=$1
    local backend=$2
    local marker="pg_textsearch index lock exclusive waiter registered for index ${oid} backend ${backend}"
    local deadline=$((SECONDS + 10))

    while ((SECONDS < deadline)); do
        if grep -Fq "${marker}" "${LOGFILE}" 2>/dev/null; then
            log "Observed queued exclusive-waiter marker"
            return
        fi
        sleep 0.05
    done
    fail "did not observe log marker: ${marker}"
}

wait_for_reader_block() {
    local backend=$1
    local deadline=$((SECONDS + 10))
    local blocked

    while ((SECONDS < deadline)); do
        blocked=$($PSQL -c "
            SELECT state = 'active' AND wait_event_type = 'Extension'
              FROM pg_stat_activity
             WHERE pid = ${backend};" 2>/dev/null || true)
        if [ "${blocked}" = "t" ]; then
            log "Observed later shared reader blocked by writer preference"
            return
        fi
        sleep 0.05
    done
    fail "later shared reader did not block behind the queued writer"
}

writer() {
    PGAPPNAME=pgts-fair-writer $PSQL <<SQL
SET statement_timeout = '60s';
SET pg_textsearch.debug_index_lock_pause_exclusive_waiter_ms = ${PAUSE_MS};
SELECT bm25_spill_index('docs_bm25');
SQL
}

reader() {
    PGAPPNAME=pgts-fair-reader $PSQL -c "
        SELECT count(*) FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('alpha beta', 'docs_bm25')
            LIMIT 20000
        ) ranked;"
}

run_test() {
    local writer_pid
    local writer_backend
    local reader_pid
    local reader_backend
    local completed_pid
    local first_status=0
    local oid
    local spill_root
    local ranked_count

    oid=$($PSQL -c "SELECT 'docs_bm25'::regclass::oid;")

    log "Starting an exclusive spill and pausing after waiter registration..."
    writer >"${ERR_DIR}/writer.log" 2>&1 &
    writer_pid=$!
    writer_backend=$(backend_pid pgts-fair-writer)
    wait_for_writer_marker "${oid}" "${writer_backend}"

    log "Starting a later shared ranked reader..."
    reader >"${ERR_DIR}/reader.log" 2>&1 &
    reader_pid=$!
    reader_backend=$(backend_pid pgts-fair-reader)
    wait_for_reader_block "${reader_backend}"
    kill -0 "${writer_pid}" 2>/dev/null ||
        fail "writer left its registration pause before blocking was proved"
    [ ! -s "${ERR_DIR}/reader.log" ] ||
        fail "later reader overtook queued exclusive spill"

    wait -n -p completed_pid "${writer_pid}" "${reader_pid}" ||
        first_status=$?
    [ "${first_status}" -eq 0 ] || fail "first completed process failed"
    [ "${completed_pid}" = "${writer_pid}" ] ||
        fail "later reader completed before the queued writer"
    wait "${reader_pid}" || fail "later ranked reader failed"

    spill_root=$(grep -E '^[0-9]+$' "${ERR_DIR}/writer.log" | tail -1)
    [[ "${spill_root}" =~ ^[0-9]+$ ]] ||
        fail "writer did not return a spill root"
    ranked_count=$(grep -E '^[0-9]+$' "${ERR_DIR}/reader.log" | tail -1)
    [ "${ranked_count:-0}" = "20000" ] ||
        fail "reader returned ${ranked_count:-0} rows, expected 20000"

    log "TEST PASSED: queued writer completed before the later reader"
}

setup_test_db
seed_data
run_test
