#!/bin/bash
#
# Verify that a queued exclusive spill precedes later shared acquisitions.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55460
TEST_DB=index_lock_fairness_test
DATA_DIR="${SCRIPT_DIR}/../tmp_index_lock_fairness"
SOCKET_DIR="${SCRIPT_DIR}/.fair_sock"
LOGFILE="${DATA_DIR}/postgres.log"
ERR_DIR="${DATA_DIR}/client_logs"
KEEP_DIR="${SCRIPT_DIR}/../tmp_index_lock_fairness_logs"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
fail() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    local exit_code=$?

    log "Cleaning up (exit code: $exit_code)..."
    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    if [ "$exit_code" -ne 0 ] && [ -d "${DATA_DIR}" ]; then
        rm -rf "${KEEP_DIR}"
        mkdir -p "${KEEP_DIR}"
        cp "${LOGFILE}" "${KEEP_DIR}/" 2>/dev/null || true
        cp -r "${ERR_DIR}" "${KEEP_DIR}/" 2>/dev/null || true
        warn "Preserved logs in ${KEEP_DIR}"
    fi
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    exit $exit_code
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
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_textsearch'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
autovacuum = off
pg_textsearch.memtable_pages_threshold = 0
pg_textsearch.bulk_load_threshold = 0
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w -o "-p ${TEST_PORT}" ||
        fail "PostgreSQL startup failed"

    createdb -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

PSQL="psql -h ${SOCKET_DIR} -p ${TEST_PORT} -d ${TEST_DB} -qAt -v ON_ERROR_STOP=1"

seed_data() {
    log "Creating BM25 index and seeding lock-test data..."
    $PSQL <<'SQL' >/dev/null
CREATE TABLE docs (id bigserial PRIMARY KEY, content text NOT NULL);
CREATE INDEX docs_bm25 ON docs USING bm25(content)
    WITH (text_config='english');
INSERT INTO docs (content)
SELECT 'alpha beta gamma document ' || gs || ' ' || repeat(md5(gs::text), 4)
FROM generate_series(1, 1000) gs;
SELECT bm25_spill_index('docs_bm25');
SQL
}

hold_reader() {
    PGAPPNAME=pgts-fair-holder $PSQL \
        -c "SELECT bm25_test_hold_index_lock(
            'docs_bm25', false, 5000);" \
        >"${ERR_DIR}/holder.log" 2>&1
}

late_reader() {
    PGAPPNAME=pgts-fair-late-reader $PSQL \
        -c "SELECT bm25_test_hold_index_lock(
            'docs_bm25', false, 0);" \
        >"${ERR_DIR}/late_reader.log" 2>&1
}

writer() {
    PGAPPNAME=pgts-fair-writer $PSQL <<'SQL'
SET statement_timeout = '60s';
SET pg_textsearch.memtable_pages_threshold = 1;
WITH updated AS (
    UPDATE docs
       SET content = content || ' refreshed'
     WHERE id <= 300
     RETURNING 1
)
SELECT count(*) FROM updated;
SQL
}

wait_for_holder() {
    local deadline=$((SECONDS + 10))
    local active

    while ((SECONDS < deadline)); do
        active=$($PSQL -c "
            SELECT count(*) FROM pg_stat_activity
            WHERE application_name = 'pgts-fair-holder'
              AND state = 'active';")
        if [ "${active:-0}" -eq 1 ]; then
            return
        fi
        sleep 0.1
    done

    fail "shared lock holder did not become active"
}

wait_for_writer_queue() {
    local deadline=$((SECONDS + 10))
    local waiters

    while ((SECONDS < deadline)); do
        waiters=$($PSQL -c "
            SELECT bm25_test_exclusive_waiters('docs_bm25');")
        if [ "${waiters:-0}" -ge 1 ]; then
            log "exclusive writer is queued"
            return
        fi
        sleep 0.05
    done

    fail "exclusive writer never registered as waiting"
}

wait_for_late_reader_admission() {
    local late_reader_pid="$1"
    local deadline=$((SECONDS + 10))
    local waiting

    while ((SECONDS < deadline)); do
        if ! kill -0 "${late_reader_pid}" 2>/dev/null; then
            cat "${ERR_DIR}/late_reader.log" || true
            fail "late shared reader exited before reaching admission"
        fi
        waiting=$($PSQL -c "
            SELECT count(*)
            FROM pg_stat_activity
            WHERE application_name = 'pgts-fair-late-reader'
              AND state = 'active'
              AND wait_event_type = 'Extension'
              AND wait_event = 'Extension'
              AND bm25_test_exclusive_waiters('docs_bm25') >= 1;")
        if [ "${waiting:-0}" -eq 1 ]; then
            return
        fi
        sleep 0.05
    done

    fail "late shared reader did not reach the admission wait"
}

run_test() {
    local holder_pid
    local late_reader_pid
    local writer_pid
    local writer_status=0
    local update_count
    local ranked_count

    log "Holding the per-index lock in shared mode..."
    hold_reader &
    holder_pid=$!
    wait_for_holder

    log "Starting updates that trigger an exclusive automatic spill..."
    writer >"${ERR_DIR}/writer.log" 2>&1 &
    writer_pid=$!
    wait_for_writer_queue

    log "Starting a reader after the writer has queued..."
    late_reader &
    late_reader_pid=$!
    wait_for_late_reader_admission "${late_reader_pid}"

    wait "${holder_pid}" || {
        cat "${ERR_DIR}/holder.log" || true
        fail "shared lock holder exited with an error"
    }
    wait "$writer_pid" || writer_status=$?
    if [ "$writer_status" -ne 0 ]; then
        tail -n 20 "${ERR_DIR}/writer.log" || true
        fail "writer exited with status ${writer_status}"
    fi
    wait "${late_reader_pid}" || {
        cat "${ERR_DIR}/late_reader.log" || true
        fail "late shared reader exited with an error"
    }

    update_count=$(grep -E '^[0-9]+$' "${ERR_DIR}/writer.log" | tail -1)
    if [ "${update_count:-0}" -le 0 ]; then
        fail "writer did not update any rows"
    fi

    ranked_count=$($PSQL -c "
        SELECT count(*) FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('alpha beta', 'docs_bm25')
            LIMIT 10
        ) ranked;")
    if [ "${ranked_count:-0}" -le 0 ]; then
        fail "ranked query returned no rows after the spill"
    fi

    log "TEST PASSED: queued writer preceded the late reader"
}

setup_test_db
seed_data
run_test
