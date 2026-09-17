#!/bin/bash
#
# Deterministic coverage for the unlocked compaction build phase.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
export PATH="${PGBINDIR}:${PATH}"
TEST_PORT=55466
TEST_DB=nonblocking_compaction_test
DATA_DIR="${SCRIPT_DIR}/../tmp_nonblocking_compaction"
SOCKET_DIR="${SCRIPT_DIR}/.nbc_sock"
LOGFILE="${DATA_DIR}/postgres.log"
CLIENT_DIR="${DATA_DIR}/client_logs"
KEEP_DIR="${SCRIPT_DIR}/../tmp_nonblocking_compaction_logs"
PAUSE_MS=5000

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }

sql() {
    psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 "$@"
}

diagnose() {
    warn "pg_stat_activity:"
    sql -F '|' -c "
        SELECT pid, application_name, state,
               coalesce(wait_event_type, ''), coalesce(wait_event, ''),
               left(query, 100)
          FROM pg_stat_activity
         WHERE datname = current_database()
         ORDER BY pid;" 2>&1 || true
    warn "server log tail:"
    tail -n 80 "${LOGFILE}" 2>/dev/null || true
    warn "client log tails:"
    for file in "${CLIENT_DIR}"/*.log; do
        [ -e "${file}" ] || continue
        echo "==> ${file} <=="
        tail -n 20 "${file}" || true
    done
}

fail() {
    echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"
    diagnose
    exit 1
}

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
        cp -r "${CLIENT_DIR}" "${KEEP_DIR}/" 2>/dev/null || true
        warn "Preserved failure logs in ${KEEP_DIR}"
    fi
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    exit "${exit_code}"
}

trap cleanup EXIT INT TERM

setup_cluster() {
    log "Setting up dedicated PostgreSQL cluster..."
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}" "${KEEP_DIR}"
    mkdir -p "${DATA_DIR}" "${SOCKET_DIR}"
    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1 || fail "initdb failed"
    mkdir -p "${CLIENT_DIR}"

    cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
unix_socket_directories = '${SOCKET_DIR}'
listen_addresses = ''
shared_preload_libraries = 'pg_textsearch'
logging_collector = off
log_min_messages = log
log_line_prefix = '%m [%p] %a '
autovacuum = off
pg_textsearch.segments_per_level = 4
pg_textsearch.memtable_pages_threshold = 0
pg_textsearch.bulk_load_threshold = 0
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w ||
        fail "PostgreSQL startup failed"
    createdb -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    sql -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

verify_guc_contract() {
    local defaults
    local output

    log "Checking compaction pause GUC contract..."
    if ! defaults=$(sql -F '|' -c "
        SELECT current_setting(
                   'pg_textsearch.debug_compaction_pause_after_select_ms'),
               current_setting(
                   'pg_textsearch.debug_compaction_pause_before_publish_ms');" \
        2>&1); then
        fail "compaction pause GUCs are unavailable: ${defaults}"
    fi
    [ "${defaults}" = "0|0" ] ||
        fail "compaction pause GUC defaults are not 0|0: ${defaults}"

    if output=$(sql -c "
        SET pg_textsearch.debug_compaction_pause_after_select_ms = 60001;" \
        2>&1); then
        fail "after-select pause accepted a value above 60000"
    fi
    [[ "${output}" == *"outside the valid range"* ]] ||
        fail "after-select range rejection was unexpected: ${output}"

    sql -c "CREATE ROLE pgts_pause_user;" >/dev/null
    if output=$(sql -c "
        SET ROLE pgts_pause_user;
        SET pg_textsearch.debug_compaction_pause_before_publish_ms = 1;" \
        2>&1); then
        fail "non-superuser changed a compaction pause GUC"
    fi
    [[ "${output}" == *"permission denied"* ]] ||
        fail "non-superuser GUC rejection was unexpected: ${output}"
}

seed_index() {
    local table_name=$1
    local index_name=$2
    local token=$3
    local batch

    sql -c "
        CREATE TABLE ${table_name} (
            id bigserial PRIMARY KEY,
            body text NOT NULL
        );
        CREATE INDEX ${index_name} ON ${table_name} USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');" >/dev/null

    for batch in 1 2 3 4; do
        sql -c "
            INSERT INTO ${table_name}(body)
            SELECT 'common ${token} batch${batch} document ' || gs
              FROM generate_series(1, 8) gs;
            SELECT bm25_spill_index('${index_name}');" >/dev/null
    done

    assert_graph "${index_name}" "{4,0,0,0,0,0,0,0}"
}

seed_all_indexes() {
    log "Seeding independent indexes with deterministic compaction debt..."
    seed_index scan_docs scan_idx scancase
    seed_index insert_docs insert_idx insertcase
    seed_index spill_docs spill_idx spillcase
    seed_index serial_docs serial_idx serialcase
    seed_index parallel_a_docs parallel_a_idx parallelacase
    seed_index parallel_b_docs parallel_b_idx parallelbcase
    seed_index cancel_docs cancel_idx cancelcase
}

index_oid() {
    sql -c "SELECT '${1}'::regclass::oid;"
}

graph() {
    sql -c "SELECT bm25_level_counts('${1}'::regclass)::text;"
}

assert_graph() {
    local index_name=$1
    local expected=$2
    local actual

    actual=$(graph "${index_name}")
    [ "${actual}" = "${expected}" ] ||
        fail "${index_name} graph is ${actual}, expected ${expected}"
}

ranked_count() {
    local table_name=$1
    local index_name=$2
    local token=$3

    sql -c "
        SELECT count(*)
          FROM (
                SELECT id
                  FROM ${table_name}
                 ORDER BY body <@> to_bm25query(
                              '${token}', '${index_name}')
                 LIMIT 1000
               ) ranked;"
}

assert_all_documents() {
    local table_name=$1
    local index_name=$2
    local token=$3
    local heap_count
    local index_count
    local mismatch_count

    heap_count=$(sql -c "SELECT count(*) FROM ${table_name};")
    index_count=$(ranked_count "${table_name}" "${index_name}" "${token}")
    [ "${index_count}" = "${heap_count}" ] ||
        fail "${index_name} returned ${index_count}/${heap_count} documents"
    mismatch_count=$(sql -c "
        WITH ranked AS MATERIALIZED (
            SELECT id
              FROM ${table_name}
             ORDER BY body <@> to_bm25query('${token}', '${index_name}')
             LIMIT 1000
        ),
        missing AS (
            SELECT id FROM ${table_name}
            EXCEPT
            SELECT id FROM ranked
        ),
        extra AS (
            SELECT id FROM ranked
            EXCEPT
            SELECT id FROM ${table_name}
        )
        SELECT (SELECT count(*) FROM missing) +
               (SELECT count(*) FROM extra);")
    [ "${mismatch_count}" = "0" ] ||
        fail "${index_name} ranked results differ from heap document IDs"
}

STARTED_PID=
start_compaction() {
    local app_name=$1
    local index_name=$2
    local guc_name=$3
    local pause_ms=$4
    local output_file=$5

    PGAPPNAME="${app_name}" \
        psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 \
        -c "SET statement_timeout = '90s';
            SET ${guc_name} = ${pause_ms};
            SELECT bm25_compact_step('${index_name}'::regclass);" \
        >"${output_file}" 2>&1 &
    STARTED_PID=$!
}

start_sql() {
    local app_name=$1
    local statement=$2
    local output_file=$3

    PGAPPNAME="${app_name}" \
        psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 -c "${statement}" \
        >"${output_file}" 2>&1 &
    STARTED_PID=$!
}

backend_pid() {
    local app_name=$1
    local deadline=$((SECONDS + 10))
    local pid

    while ((SECONDS < deadline)); do
        pid=$(sql -c "
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

wait_for_marker() {
    local phase=$1
    local oid=$2
    local backend=$3
    local deadline=$((SECONDS + 10))
    local marker="pg_textsearch compaction pause at ${phase} for index ${oid} backend ${backend}"

    while ((SECONDS < deadline)); do
        if grep -Fq "${marker}" "${LOGFILE}" 2>/dev/null; then
            log "Observed ${phase} marker for index ${oid}, backend ${backend}"
            return
        fi
        sleep 0.05
    done
    fail "did not observe log marker: ${marker}"
}

assert_still_paused() {
    local phase=$1
    local oid=$2
    local backend=$3
    local label=$4
    local marker="pg_textsearch compaction resume after ${phase} for index ${oid} backend ${backend}"

    if grep -Fq "${marker}" "${LOGFILE}" 2>/dev/null; then
        fail "${label} did not overlap the ${phase} pause"
    fi
}

wait_for_exit() {
    local pid=$1
    local timeout_seconds=$2
    local label=$3
    local deadline=$((SECONDS + timeout_seconds))

    while kill -0 "${pid}" 2>/dev/null; do
        ((SECONDS < deadline)) ||
            fail "${label} did not finish within ${timeout_seconds} seconds"
        sleep 0.05
    done
}

wait_success() {
    local pid=$1
    local timeout_seconds=$2
    local label=$3
    local output_file=$4

    wait_for_exit "${pid}" "${timeout_seconds}" "${label}"
    if ! wait "${pid}"; then
        tail -n 40 "${output_file}" 2>/dev/null || true
        fail "${label} failed"
    fi
}

require_completion_during_pause() {
    local operation_pid=$1
    local compactor_pid=$2
    local label=$3
    local output_file=$4
    local phase=$5
    local oid=$6
    local backend=$7

    wait_success "${operation_pid}" 3 "${label}" "${output_file}"
    kill -0 "${compactor_pid}" 2>/dev/null ||
        fail "${label} completed only after compaction left its pause"
    assert_still_paused "${phase}" "${oid}" "${backend}" "${label}"
    log "${label} completed while compaction was still paused"
}

test_scan_progress() {
    local compactor_output="${CLIENT_DIR}/scan_compactor.log"
    local scan_output="${CLIENT_DIR}/scan_reader.log"
    local compactor_pid
    local reader_pid
    local backend
    local oid

    log "Case: ranked scan progresses during unlocked build..."
    oid=$(index_oid scan_idx)
    start_compaction pgts-scan-compactor scan_idx \
        pg_textsearch.debug_compaction_pause_after_select_ms \
        "${PAUSE_MS}" "${compactor_output}"
    compactor_pid=${STARTED_PID}
    backend=$(backend_pid pgts-scan-compactor)
    wait_for_marker after-select "${oid}" "${backend}"

    start_sql pgts-scan-reader "
        SELECT count(*) FROM (
            SELECT id FROM scan_docs
             ORDER BY body <@> to_bm25query('scancase', 'scan_idx')
             LIMIT 1000
        ) ranked;" "${scan_output}"
    reader_pid=${STARTED_PID}
    require_completion_during_pause \
        "${reader_pid}" "${compactor_pid}" "ranked scan" "${scan_output}" \
        after-select "${oid}" "${backend}"
    [ "$(tail -n 1 "${scan_output}")" = "32" ] ||
        fail "ranked scan did not return all 32 documents"

    wait_success "${compactor_pid}" 10 "scan compactor" "${compactor_output}"
    assert_all_documents scan_docs scan_idx scancase
}

test_insert_progress() {
    local compactor_output="${CLIENT_DIR}/insert_compactor.log"
    local insert_output="${CLIENT_DIR}/insert_writer.log"
    local compactor_pid
    local writer_pid
    local backend
    local oid

    log "Case: insert progresses during unlocked build..."
    oid=$(index_oid insert_idx)
    start_compaction pgts-insert-compactor insert_idx \
        pg_textsearch.debug_compaction_pause_after_select_ms \
        "${PAUSE_MS}" "${compactor_output}"
    compactor_pid=${STARTED_PID}
    backend=$(backend_pid pgts-insert-compactor)
    wait_for_marker after-select "${oid}" "${backend}"

    start_sql pgts-insert-writer "
        INSERT INTO insert_docs(body)
        VALUES ('common insertcase live insert')
        RETURNING id;" "${insert_output}"
    writer_pid=${STARTED_PID}
    require_completion_during_pause \
        "${writer_pid}" "${compactor_pid}" "insert" "${insert_output}" \
        after-select "${oid}" "${backend}"

    wait_success "${compactor_pid}" 10 \
        "insert compactor" "${compactor_output}"
    assert_all_documents insert_docs insert_idx insertcase
}

test_spill_prefix_progress() {
    local compactor_output="${CLIENT_DIR}/spill_compactor.log"
    local spill_output="${CLIENT_DIR}/spill_writer.log"
    local compactor_pid
    local spiller_pid
    local backend
    local oid

    log "Case: concurrent spill prefix survives publication..."
    oid=$(index_oid spill_idx)
    start_compaction pgts-spill-compactor spill_idx \
        pg_textsearch.debug_compaction_pause_after_select_ms \
        "${PAUSE_MS}" "${compactor_output}"
    compactor_pid=${STARTED_PID}
    backend=$(backend_pid pgts-spill-compactor)
    wait_for_marker after-select "${oid}" "${backend}"

    start_sql pgts-spill-writer "
        INSERT INTO spill_docs(body)
        SELECT 'common spillcase concurrent prefix ' || gs
          FROM generate_series(1, 5) gs;
        SELECT bm25_spill_index('spill_idx');" "${spill_output}"
    spiller_pid=${STARTED_PID}
    require_completion_during_pause \
        "${spiller_pid}" "${compactor_pid}" "spill" "${spill_output}" \
        after-select "${oid}" "${backend}"

    wait_success "${compactor_pid}" 10 \
        "spill compactor" "${compactor_output}"
    assert_graph spill_idx "{1,1,0,0,0,0,0,0}"
    assert_all_documents spill_docs spill_idx spillcase
}

test_same_index_serialization() {
    local first_output="${CLIENT_DIR}/serial_first.log"
    local second_output="${CLIENT_DIR}/serial_second.log"
    local first_pid
    local second_pid
    local first_backend
    local second_backend
    local oid
    local wait_state
    local deadline

    log "Case: same-index maintenance serializes..."
    oid=$(index_oid serial_idx)
    start_compaction pgts-serial-first serial_idx \
        pg_textsearch.debug_compaction_pause_after_select_ms \
        "${PAUSE_MS}" "${first_output}"
    first_pid=${STARTED_PID}
    first_backend=$(backend_pid pgts-serial-first)
    wait_for_marker after-select "${oid}" "${first_backend}"

    start_compaction pgts-serial-second serial_idx \
        pg_textsearch.debug_compaction_pause_after_select_ms \
        0 "${second_output}"
    second_pid=${STARTED_PID}
    second_backend=$(backend_pid pgts-serial-second)
    deadline=$((SECONDS + 3))
    wait_state=
    while ((SECONDS < deadline)); do
        wait_state=$(sql -F '|' -c "
            SELECT state, coalesce(wait_event_type, ''),
                   coalesce(wait_event, '')
              FROM pg_stat_activity
             WHERE pid = ${second_backend};" 2>/dev/null || true)
        if [[ "${wait_state}" == "active|Lock|"* ]]; then
            break
        fi
        sleep 0.05
    done
    [[ "${wait_state}" == "active|Lock|"* ]] ||
        fail "second same-index compactor did not wait on a lock: ${wait_state}"
    kill -0 "${first_pid}" 2>/dev/null ||
        fail "first compactor left its pause before serialization proof"
    assert_still_paused \
        after-select "${oid}" "${first_backend}" "same-index compactor"
    kill -0 "${second_pid}" 2>/dev/null ||
        fail "second same-index compactor completed during the first pause"

    wait_success "${first_pid}" 10 "first serial compactor" "${first_output}"
    wait_success "${second_pid}" 10 "second serial compactor" "${second_output}"
    assert_all_documents serial_docs serial_idx serialcase
}

test_different_index_overlap() {
    local a_output="${CLIENT_DIR}/parallel_a.log"
    local b_output="${CLIENT_DIR}/parallel_b.log"
    local a_pid
    local b_pid
    local a_backend
    local b_backend
    local a_oid
    local b_oid

    log "Case: different indexes compact concurrently..."
    a_oid=$(index_oid parallel_a_idx)
    b_oid=$(index_oid parallel_b_idx)

    start_compaction pgts-parallel-a parallel_a_idx \
        pg_textsearch.debug_compaction_pause_after_select_ms \
        "${PAUSE_MS}" "${a_output}"
    a_pid=${STARTED_PID}
    a_backend=$(backend_pid pgts-parallel-a)
    wait_for_marker after-select "${a_oid}" "${a_backend}"

    start_compaction pgts-parallel-b parallel_b_idx \
        pg_textsearch.debug_compaction_pause_after_select_ms \
        "${PAUSE_MS}" "${b_output}"
    b_pid=${STARTED_PID}
    b_backend=$(backend_pid pgts-parallel-b)
    wait_for_marker after-select "${b_oid}" "${b_backend}"
    kill -0 "${a_pid}" 2>/dev/null ||
        fail "index A finished before index B reached the same build phase"
    assert_still_paused \
        after-select "${a_oid}" "${a_backend}" "different-index compactor"

    wait_success "${a_pid}" 10 "parallel compactor A" "${a_output}"
    wait_success "${b_pid}" 10 "parallel compactor B" "${b_output}"
    assert_all_documents parallel_a_docs parallel_a_idx parallelacase
    assert_all_documents parallel_b_docs parallel_b_idx parallelbcase
}

test_cancel_before_publish() {
    local output="${CLIENT_DIR}/cancel_compactor.log"
    local compactor_pid
    local backend
    local oid
    local before_graph
    local cancel_result

    log "Case: cancellation before publication preserves the old graph..."
    oid=$(index_oid cancel_idx)
    before_graph=$(graph cancel_idx)
    start_compaction pgts-cancel-compactor cancel_idx \
        pg_textsearch.debug_compaction_pause_before_publish_ms \
        60000 "${output}"
    compactor_pid=${STARTED_PID}
    backend=$(backend_pid pgts-cancel-compactor)
    wait_for_marker before-publish "${oid}" "${backend}"

    cancel_result=$(sql -c "SELECT pg_cancel_backend(${backend});")
    [ "${cancel_result}" = "t" ] ||
        fail "pg_cancel_backend(${backend}) returned ${cancel_result}"
    wait_for_exit "${compactor_pid}" 10 "cancelled compactor"
    if wait "${compactor_pid}"; then
        fail "cancelled compactor unexpectedly succeeded"
    fi
    grep -Fq "canceling statement due to user request" "${output}" ||
        fail "compactor did not report query cancellation"

    [ "$(graph cancel_idx)" = "${before_graph}" ] ||
        fail "cancellation changed the published segment graph"
    assert_all_documents cancel_docs cancel_idx cancelcase
}

main() {
    setup_cluster
    verify_guc_contract
    seed_all_indexes
    test_scan_progress
    test_insert_progress
    test_spill_prefix_progress
    test_same_index_serialization
    test_different_index_overlap
    test_cancel_before_publish
    log "All deterministic non-blocking compaction cases passed"
}

main "$@"
