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
GATE_KEY=472007

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

verify_gate_guc() {
    local output

    [ "$($PSQL -c "
        SELECT current_setting(
            'pg_textsearch.debug_index_lock_exclusive_waiter_gate');")" = "0" ] ||
        fail "exclusive-waiter gate is not disabled by default"

    $PSQL -c "CREATE ROLE pgts_fairness_user;" >/dev/null
    if output=$($PSQL -c "
        SET ROLE pgts_fairness_user;
        SET pg_textsearch.debug_index_lock_exclusive_waiter_gate =
            ${GATE_KEY};" 2>&1); then
        fail "non-superuser changed the exclusive-waiter gate"
    fi
    [[ "${output}" == *"permission denied"* ]] ||
        fail "non-superuser gate rejection was unexpected: ${output}"
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

wait_for_log_marker() {
    local marker=$1
    local deadline=$((SECONDS + 10))

    while ((SECONDS < deadline)); do
        if grep -Fq "${marker}" "${LOGFILE}" 2>/dev/null; then
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

wait_for_writer_gate_block() {
    local backend=$1
    local deadline=$((SECONDS + 10))
    local blocked

    while ((SECONDS < deadline)); do
        blocked=$($PSQL -c "
            SELECT EXISTS (
                SELECT 1
                  FROM pg_locks pending
                 WHERE pending.pid = ${backend}
                   AND pending.locktype = 'advisory'
                   AND pending.mode = 'ShareLock'
                   AND NOT pending.granted
                   AND cardinality(pg_blocking_pids(${backend})) > 0
            );" 2>/dev/null || true)
        if [ "${blocked}" = "t" ]; then
            log "Exclusive waiter is blocked on the explicit gate"
            return
        fi
        sleep 0.05
    done
    fail "exclusive waiter did not block on the explicit gate"
}

wait_for_gate_lock() {
    local deadline=$((SECONDS + 10))
    local held

    while ((SECONDS < deadline)); do
        held=$($PSQL -c "
            SELECT EXISTS (
                SELECT 1
                  FROM pg_locks locks
                  JOIN pg_stat_activity activity
                    ON activity.pid = locks.pid
                 WHERE activity.application_name = 'pgts-fair-gate'
                   AND locks.locktype = 'advisory'
                   AND locks.mode = 'ExclusiveLock'
                   AND locks.granted
            );" 2>/dev/null || true)
        if [ "${held}" = "t" ]; then
            log "Explicit fairness gate is held"
            return
        fi
        sleep 0.05
    done
    fail "fairness gate backend did not acquire its advisory lock"
}

writer() {
    PGAPPNAME=pgts-fair-writer $PSQL <<SQL
SET statement_timeout = '60s';
SET pg_textsearch.debug_index_lock_exclusive_waiter_gate = ${GATE_KEY};
SELECT bm25_spill_index('docs_bm25');
SQL
}

reader() {
    PGAPPNAME=pgts-fair-reader $PSQL -c "
        SET pg_textsearch.debug_index_lock_exclusive_waiter_gate = ${GATE_KEY};
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
    local gate_pid
    local oid
    local waiter_marker
    local release_marker
    local acquire_marker
    local release_line
    local acquire_line
    local spill_root
    local ranked_count

    oid=$($PSQL -c "SELECT 'docs_bm25'::regclass::oid;")

    mkfifo "${DATA_DIR}/fairness_gate.fifo"
    exec 9<>"${DATA_DIR}/fairness_gate.fifo"
    PGAPPNAME=pgts-fair-gate $PSQL \
        <"${DATA_DIR}/fairness_gate.fifo" \
        >"${ERR_DIR}/gate.log" 2>&1 &
    gate_pid=$!
    printf "SELECT pg_advisory_lock(%d, 0);\n" "${GATE_KEY}" >&9
    wait_for_gate_lock

    log "Starting an exclusive spill behind an explicit test gate..."
    writer >"${ERR_DIR}/writer.log" 2>&1 &
    writer_pid=$!
    writer_backend=$(backend_pid pgts-fair-writer)
    waiter_marker="pg_textsearch index lock exclusive waiter registered for index ${oid} backend ${writer_backend}"
    wait_for_log_marker "${waiter_marker}"
    wait_for_writer_gate_block "${writer_backend}"
    log "Observed queued exclusive-waiter marker"

    log "Starting a later shared ranked reader..."
    reader >"${ERR_DIR}/reader.log" 2>&1 &
    reader_pid=$!
    reader_backend=$(backend_pid pgts-fair-reader)
    wait_for_reader_block "${reader_backend}"
    release_marker="pg_textsearch index lock exclusive release for index ${oid} backend ${writer_backend}"
    acquire_marker="pg_textsearch index lock shared acquired for index ${oid} backend ${reader_backend}"
    ! grep -Fq "${release_marker}" "${LOGFILE}" ||
        fail "writer passed the explicit gate before release"
    ! grep -Fq "${acquire_marker}" "${LOGFILE}" ||
        fail "later reader acquired the shared lock before gate release"

    log "Releasing the queued writer..."
    printf "SELECT pg_advisory_unlock(%d, 0);\n" "${GATE_KEY}" >&9
    wait_for_log_marker "${release_marker}"
    wait_for_log_marker "${acquire_marker}"

    release_line=$(grep -nF "${release_marker}" "${LOGFILE}" |
        head -1 | cut -d: -f1)
    acquire_line=$(grep -nF "${acquire_marker}" "${LOGFILE}" |
        head -1 | cut -d: -f1)
    [ "${release_line}" -lt "${acquire_line}" ] ||
        fail "shared lock was acquired before the exclusive lock was released"

    wait "${writer_pid}" || fail "queued writer failed"
    wait "${reader_pid}" || fail "later ranked reader failed"
    printf "\\q\n" >&9
    exec 9>&-
    wait "${gate_pid}" || fail "fairness gate backend failed"

    spill_root=$(grep -E '^[0-9]+$' "${ERR_DIR}/writer.log" | tail -1)
    [[ "${spill_root}" =~ ^[0-9]+$ ]] ||
        fail "writer did not return a spill root"
    ranked_count=$(grep -E '^[0-9]+$' "${ERR_DIR}/reader.log" | tail -1)
    [ "${ranked_count:-0}" = "20000" ] ||
        fail "reader returned ${ranked_count:-0} rows, expected 20000"

    log "TEST PASSED: exclusive release preceded shared acquisition"
}

run_spill_threshold_recheck_test() {
    local gate_pid
    local vacuum_pid
    local vacuum_backend
    local oid
    local marker
    local segment_count
    local spill_was_empty

    log "Case: threshold spill rechecks after waiting for exclusive..."
    $PSQL <<'SQL' >/dev/null
CREATE TABLE threshold_docs (id bigserial PRIMARY KEY, content text NOT NULL);
CREATE INDEX threshold_bm25 ON threshold_docs USING bm25(content)
    WITH (text_config='english');
INSERT INTO threshold_docs (content)
SELECT 'threshold pending document ' || gs || ' ' || md5(gs::text)
FROM generate_series(1, 1000) gs;
SQL
    oid=$($PSQL -c "SELECT 'threshold_bm25'::regclass::oid;")

    mkfifo "${DATA_DIR}/threshold_gate.fifo"
    exec 9<>"${DATA_DIR}/threshold_gate.fifo"
    PGAPPNAME=pgts-fair-gate $PSQL \
        <"${DATA_DIR}/threshold_gate.fifo" \
        >"${ERR_DIR}/threshold_gate.log" 2>&1 &
    gate_pid=$!
    printf "SELECT pg_advisory_lock(%d, 0);\n" "${GATE_KEY}" >&9
    wait_for_gate_lock

    PGAPPNAME=pgts-threshold-vacuum \
        PGOPTIONS="-c statement_timeout=60000 -c pg_textsearch.debug_index_lock_exclusive_waiter_gate=${GATE_KEY}" \
        $PSQL -c "VACUUM threshold_docs;" \
        >"${ERR_DIR}/threshold_vacuum.log" 2>&1 &
    vacuum_pid=$!
    vacuum_backend=$(backend_pid pgts-threshold-vacuum)
    marker="pg_textsearch spill threshold check passed for index ${oid} backend ${vacuum_backend}"
    wait_for_log_marker "${marker}"

    $PSQL <<'SQL' >/dev/null
SELECT bm25_spill_index('threshold_bm25');
INSERT INTO threshold_docs (content)
VALUES ('fresh below threshold');
SQL

    printf "SELECT pg_advisory_unlock(%d, 0);\n" "${GATE_KEY}" >&9
    wait "${vacuum_pid}" || fail "threshold VACUUM failed"
    printf "\\q\n" >&9
    exec 9>&-
    wait "${gate_pid}" || fail "threshold gate backend failed"

    segment_count=$($PSQL -c "
        SELECT regexp_count(
            bm25_dump_index('threshold_bm25'),
            '========== Segment at block ');")
    [ "${segment_count}" = "1" ] ||
        fail "queued VACUUM spilled a fresh below-threshold chain"

    spill_was_empty=$($PSQL -c "
        SELECT bm25_spill_index('threshold_bm25') IS NULL;")
    [ "${spill_was_empty}" = "f" ] ||
        fail "fresh below-threshold chain was not left in the memtable"

    log "TEST PASSED: threshold spill skipped the fresh runt chain"
}

setup_test_db
seed_data
verify_gate_guc
run_test
run_spill_threshold_recheck_test
