#!/bin/bash
#
# Regression test for writer starvation at the per-index LWLock.
# PostgreSQL LWLocks allow new shared holders to bypass an already queued
# exclusive waiter.  Continuous ranked scans therefore used to prevent an
# automatic spill from making bounded progress.
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
N_READERS=12

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
    log "Creating BM25 index and seeding ranked-scan data..."
    $PSQL <<'SQL' >/dev/null
CREATE TABLE docs (id bigserial PRIMARY KEY, content text NOT NULL);
CREATE INDEX docs_bm25 ON docs USING bm25(content)
    WITH (text_config='english');
INSERT INTO docs (content)
SELECT 'alpha beta gamma document ' || gs || ' ' || repeat(md5(gs::text), 4)
FROM generate_series(1, 30000) gs;
SELECT bm25_spill_index('docs_bm25');

CREATE TABLE reader_control (stop boolean NOT NULL);
INSERT INTO reader_control VALUES (false);
CREATE PROCEDURE run_ranked_reader(tag integer) LANGUAGE plpgsql AS $$
DECLARE
    ranked_count integer;
BEGIN
    LOOP
        SELECT count(*) INTO ranked_count FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('alpha beta', 'docs_bm25')
            LIMIT 20000
        ) ranked;
        COMMIT;
        EXIT WHEN (SELECT stop FROM reader_control);
    END LOOP;
END
$$;
SQL

    local plan
    plan=$($PSQL -c "EXPLAIN (COSTS off)
        SELECT id FROM docs
        ORDER BY content <@> to_bm25query('alpha beta', 'docs_bm25')
        LIMIT 20000")
    if ! echo "$plan" | grep -qi "Index Scan"; then
        warn "Reader plan was not an Index Scan:"
        echo "$plan"
        fail "test setup did not exercise ranked index scans"
    fi
}

reader() {
    local tag=$1

    PGAPPNAME=pgts-fair-reader $PSQL \
        -c "CALL run_ranked_reader(${tag});" \
        >>"${ERR_DIR}/reader_${tag}.log" 2>&1 || return 10
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

wait_for_readers() {
    local deadline=$((SECONDS + 10))
    local active=0

    while ((SECONDS < deadline)); do
        active=$($PSQL -c "
            SELECT count(*)
              FROM pg_stat_activity
             WHERE application_name = 'pgts-fair-reader'
               AND state = 'active';")
        if [ "${active:-0}" -ge 8 ]; then
            log "${active} ranked readers are active"
            return
        fi
        sleep 0.1
    done

    fail "fewer than eight ranked readers became active"
}

run_test() {
    local reader_pids=()
    local writer_pid
    local writer_status=0
    local update_count
    local ranked_count

    log "Starting ${N_READERS} continuous ranked readers..."
    for i in $(seq 1 "${N_READERS}"); do
        reader "$i" &
        reader_pids+=("$!")
    done
    wait_for_readers

    log "Starting updates that trigger an exclusive automatic spill..."
    writer >"${ERR_DIR}/writer.log" 2>&1 &
    writer_pid=$!
    deadline=$((SECONDS + 20))
    while kill -0 "$writer_pid" 2>/dev/null && ((SECONDS < deadline)); do
        sleep 1
    done
    if kill -0 "$writer_pid" 2>/dev/null; then
        kill "$writer_pid" 2>/dev/null || true
        fail "exclusive spill request was starved by ranked scans"
    fi

    wait "$writer_pid" || writer_status=$?
    if [ "$writer_status" -ne 0 ]; then
        tail -n 20 "${ERR_DIR}/writer.log" || true
        fail "writer exited with status ${writer_status}"
    fi

    update_count=$(grep -E '^[0-9]+$' "${ERR_DIR}/writer.log" | tail -1)
    if [ "${update_count:-0}" -le 0 ]; then
        fail "writer did not update any rows"
    fi

    $PSQL -c "UPDATE reader_control SET stop = true;" >/dev/null
    for pid in "${reader_pids[@]}"; do
        wait "$pid" || {
            tail -n 20 "${ERR_DIR}"/reader_*.log || true
            fail "a ranked reader exited with an error"
        }
    done

    ranked_count=$($PSQL -c "
        SELECT count(*) FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('alpha beta', 'docs_bm25')
            LIMIT 10
        ) ranked;")
    if [ "${ranked_count:-0}" -le 0 ]; then
        fail "ranked query returned no rows after the spill"
    fi

    log "TEST PASSED: exclusive spill completed after ${update_count} updates"
}

setup_test_db
seed_data
run_test
