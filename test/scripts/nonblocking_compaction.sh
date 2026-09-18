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
    local pause_guc

    log "Checking compaction pause GUC contract..."
    if ! defaults=$(sql -F '|' -c "
        SELECT current_setting(
                   'pg_textsearch.debug_compaction_pause_after_select_ms'),
               current_setting(
                   'pg_textsearch.debug_compaction_pause_source_estimate_ms'),
               current_setting(
                   'pg_textsearch.debug_compaction_pause_before_publish_ms'),
               current_setting(
                   'pg_textsearch.debug_compaction_pause_after_restamp_ms'),
               current_setting(
                   'pg_textsearch.debug_compaction_pause_after_allocation');" \
        2>&1); then
        fail "compaction pause GUCs are unavailable: ${defaults}"
    fi
    [ "${defaults}" = "0|0|0|0|none" ] ||
        fail "compaction pause GUC defaults are not 0|0|0|0|none: ${defaults}"

    if output=$(sql -c "
        SET pg_textsearch.debug_compaction_pause_after_select_ms = 60001;" \
        2>&1); then
        fail "after-select pause accepted a value above 60000"
    fi
    [[ "${output}" == *"outside the valid range"* ]] ||
        fail "after-select range rejection was unexpected: ${output}"

    if output=$(sql -c "
        SET pg_textsearch.debug_compaction_pause_source_estimate_ms = 60001;" \
        2>&1); then
        fail "source-estimate pause accepted a value above 60000"
    fi
    [[ "${output}" == *"outside the valid range"* ]] ||
        fail "source-estimate range rejection was unexpected: ${output}"

    if output=$(sql -c "
        SET pg_textsearch.debug_compaction_pause_after_restamp_ms = 60001;" \
        2>&1); then
        fail "after-restamp pause accepted a value above 60000"
    fi
    [[ "${output}" == *"outside the valid range"* ]] ||
        fail "after-restamp range rejection was unexpected: ${output}"

    sql -c "CREATE ROLE pgts_pause_user;" >/dev/null
    for pause_guc in \
        pg_textsearch.debug_compaction_pause_source_estimate_ms \
        pg_textsearch.debug_compaction_pause_before_publish_ms; do
        if output=$(sql -c "
            SET ROLE pgts_pause_user;
            SET ${pause_guc} = 1;" 2>&1); then
            fail "non-superuser changed ${pause_guc}"
        fi
        [[ "${output}" == *"permission denied"* ]] ||
            fail "non-superuser ${pause_guc} rejection was unexpected: ${output}"
    done
    if output=$(sql -c "
        SET ROLE pgts_pause_user;
        SET pg_textsearch.debug_compaction_pause_after_allocation =
            'output-data';" 2>&1); then
        fail "non-superuser changed allocation pause selector"
    fi
    [[ "${output}" == *"permission denied"* ]] ||
        fail "non-superuser allocation pause rejection was unexpected: ${output}"
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
    assert_index_scan_plan "${table_name}" "${index_name}" "${token}"
}

seed_all_indexes() {
    log "Seeding independent indexes with deterministic compaction debt..."
    seed_index scan_docs scan_idx scancase
    seed_index planning_docs planning_idx planningcase
    seed_index restamp_docs restamp_idx restampcase
    seed_index insert_docs insert_idx insertcase
    seed_index spill_docs spill_idx spillcase
    seed_index serial_docs serial_idx serialcase
    seed_index parallel_a_docs parallel_a_idx parallelacase
    seed_index parallel_b_docs parallel_b_idx parallelbcase
    seed_index publication_docs publication_idx publicationcase
    seed_index cancel_docs cancel_idx cancelcase
    for phase in output_data page_index tombstone; do
        seed_index "${phase}_docs" "${phase}_idx" "${phase}case"
        seed_index "${phase}_control_docs" "${phase}_control_idx" \
            "${phase}case"
    done
}

test_inline_policy_runs_one_pass() {
    local batch

    log "Case: visible inline compaction runs one bounded pass..."
    sql -c "
        CREATE TABLE inline_once_docs (
            id bigserial PRIMARY KEY,
            body text NOT NULL
        );
        CREATE INDEX inline_once_idx ON inline_once_docs USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');" >/dev/null

    for batch in 1 2 3 4 5 6 7; do
        sql -c "
            INSERT INTO inline_once_docs(body)
            SELECT 'inlineonce batch${batch} document ' || gs
              FROM generate_series(1, 8) gs;
            SELECT bm25_spill_index('inline_once_idx');" >/dev/null
    done

    sql -c "
        ALTER INDEX inline_once_idx SET (compaction = 'inline');
        INSERT INTO inline_once_docs(body)
        SELECT 'inlineonce batch8 document ' || gs
          FROM generate_series(1, 8) gs;
        SELECT bm25_spill_index('inline_once_idx');" >/dev/null

    assert_graph inline_once_idx "{4,1,0,0,0,0,0,0}"
}

index_oid() {
    sql -c "SELECT '${1}'::regclass::oid;"
}

graph() {
    sql -c "SELECT bm25_level_counts('${1}'::regclass)::text;"
}

pending_free() {
    sql -c "SELECT bm25_pending_free_pages('${1}');"
}

relation_blocks() {
    sql -c "
        SELECT pg_relation_size('${1}'::regclass, 'main') /
               current_setting('block_size')::bigint;"
}

assert_graph() {
    local index_name=$1
    local expected=$2
    local actual

    actual=$(graph "${index_name}")
    [ "${actual}" = "${expected}" ] ||
        fail "${index_name} graph is ${actual}, expected ${expected}"
}

assert_index_scan_plan() {
    local table_name=$1
    local index_name=$2
    local token=$3
    local plan
    local expected="Index Scan using ${index_name} on ${table_name}"

    plan=$(sql -c "
        SET enable_seqscan = off;
        EXPLAIN (COSTS off)
        SELECT id
          FROM ${table_name}
         ORDER BY body <@> to_bm25query('${token}', '${index_name}')
         LIMIT 1000;")
    if ! grep -Fq "${expected}" <<<"${plan}"; then
        warn "Unexpected ranked-query plan for ${index_name}:"
        echo "${plan}"
        fail "ranked assertions would not use ${index_name}"
    fi
}

ranked_count() {
    local table_name=$1
    local index_name=$2
    local token=$3

    sql -c "
        SET enable_seqscan = off;
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
        SET enable_seqscan = off;
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
start_compaction_with_settings() {
    local app_name=$1
    local index_name=$2
    local settings=$3
    local output_file=$4

    PGAPPNAME="${app_name}" \
        psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 \
        -c "SET statement_timeout = '90s';
            ${settings}
            SELECT bm25_compact_step('${index_name}'::regclass);" \
        >"${output_file}" 2>&1 &
    STARTED_PID=$!
}

start_compaction() {
    local app_name=$1
    local index_name=$2
    local guc_name=$3
    local pause_ms=$4
    local output_file=$5

    start_compaction_with_settings \
        "${app_name}" "${index_name}" \
        "SET ${guc_name} = ${pause_ms};" "${output_file}"
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

wait_for_allocation_marker() {
    local phase=$1
    local oid=$2
    local backend=$3
    local deadline=$((SECONDS + 10))
    local marker="pg_textsearch compaction pause at ${phase} for index ${oid} backend ${backend}"

    while ((SECONDS < deadline)); do
        if grep -Fq "${marker}" "${LOGFILE}" 2>/dev/null; then
            log "Observed ${phase} allocation marker for index ${oid}, backend ${backend}"
            return
        fi
        sleep 0.05
    done
    fail "did not observe allocation log marker: ${marker}"
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

wait_for_spill_queue_or_completion() {
    local operation_pid=$1
    local backend=$2
    local blocker=$3
    local deadline=$((SECONDS + 3))
    local blocked

    while ((SECONDS < deadline)); do
        blocked=$(sql -c "
            SELECT wait_event_type = 'LWLock' AND
                   wait_event = 'tapir_index_lock'
              FROM pg_stat_activity
             WHERE pid = ${backend};" \
            2>/dev/null || true)
        if [ "${blocked}" = "t" ]; then
            log "Observed spill backend ${backend} queued behind ${blocker}"
            return
        fi
        if ! kill -0 "${operation_pid}" 2>/dev/null; then
            log "Spill completed before it needed to queue"
            return
        fi
        sleep 0.05
    done

    fail "spill neither queued nor completed within 3 seconds"
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
        SET enable_seqscan = off;
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

test_source_estimation_progress() {
    local compactor_output="${CLIENT_DIR}/planning_compactor.log"
    local spill_output="${CLIENT_DIR}/planning_spill.log"
    local scan_output="${CLIENT_DIR}/planning_reader.log"
    local compactor_pid
    local spiller_pid
    local spiller_backend
    local reader_pid
    local backend
    local oid
    local reader_count

    log "Case: ranked scan passes a spill queued during source estimation..."
    oid=$(index_oid planning_idx)
    start_compaction_with_settings \
        pgts-planning-compactor planning_idx \
        "SET pg_textsearch.debug_compaction_pause_source_estimate_ms = ${PAUSE_MS};
         SET pg_textsearch.debug_compaction_pause_after_select_ms = ${PAUSE_MS};" \
        "${compactor_output}"
    compactor_pid=${STARTED_PID}
    backend=$(backend_pid pgts-planning-compactor)
    wait_for_marker source-estimate "${oid}" "${backend}"

    start_sql pgts-planning-spill "
        SELECT pg_sleep(0.25);
        INSERT INTO planning_docs(body)
        SELECT 'common planningcase concurrent prefix ' || gs
          FROM generate_series(1, 8) gs;
        SELECT bm25_spill_index('planning_idx');" "${spill_output}"
    spiller_pid=${STARTED_PID}
    spiller_backend=$(backend_pid pgts-planning-spill)
    wait_for_spill_queue_or_completion \
        "${spiller_pid}" "${spiller_backend}" "${backend}"

    start_sql pgts-planning-reader "
        SET enable_seqscan = off;
        SELECT count(*) FROM (
            SELECT id FROM planning_docs
             ORDER BY body <@> to_bm25query(
                          'planningcase', 'planning_idx')
             LIMIT 1000
        ) ranked;" "${scan_output}"
    reader_pid=${STARTED_PID}
    require_completion_during_pause \
        "${reader_pid}" "${compactor_pid}" "source-estimation ranked scan" \
        "${scan_output}" source-estimate "${oid}" "${backend}"
    reader_count=$(tail -n 1 "${scan_output}")
    [[ "${reader_count}" =~ ^[0-9]+$ ]] ||
        fail "source-estimation ranked scan returned '${reader_count}'"

    require_completion_during_pause \
        "${spiller_pid}" "${compactor_pid}" "source-estimation spill" \
        "${spill_output}" source-estimate "${oid}" "${backend}"
    wait_success "${compactor_pid}" 15 \
        "planning compactor" "${compactor_output}"
    assert_graph planning_idx "{1,1,0,0,0,0,0,0}"
    assert_all_documents planning_docs planning_idx planningcase
}

test_scan_progress_after_restamp() {
    local compactor_output="${CLIENT_DIR}/restamp_compactor.log"
    local scan_output="${CLIENT_DIR}/restamp_reader.log"
    local compactor_pid
    local reader_pid
    local backend
    local oid

    log "Case: ranked scan progresses after detached reclaim restamping..."
    oid=$(index_oid restamp_idx)
    start_compaction pgts-restamp-compactor restamp_idx \
        pg_textsearch.debug_compaction_pause_after_restamp_ms \
        "${PAUSE_MS}" "${compactor_output}"
    compactor_pid=${STARTED_PID}
    backend=$(backend_pid pgts-restamp-compactor)
    wait_for_marker after-restamp "${oid}" "${backend}"

    start_sql pgts-restamp-reader "
        SET enable_seqscan = off;
        SELECT count(*) FROM (
            SELECT id FROM restamp_docs
             ORDER BY body <@> to_bm25query('restampcase', 'restamp_idx')
             LIMIT 1000
        ) ranked;" "${scan_output}"
    reader_pid=${STARTED_PID}
    require_completion_during_pause \
        "${reader_pid}" "${compactor_pid}" "post-restamp ranked scan" \
        "${scan_output}" after-restamp "${oid}" "${backend}"
    [ "$(tail -n 1 "${scan_output}")" = "32" ] ||
        fail "post-restamp ranked scan did not return all 32 documents"

    wait_success "${compactor_pid}" 10 \
        "restamp compactor" "${compactor_output}"
    assert_all_documents restamp_docs restamp_idx restampcase
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
    local lock_proof
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
    lock_proof=f
    while ((SECONDS < deadline)); do
        lock_proof=$(sql -c "
            SELECT EXISTS (
                SELECT 1
                  FROM pg_stat_activity activity
                  JOIN pg_locks pending
                    ON pending.pid = activity.pid
                 WHERE activity.pid = ${second_backend}
                   AND activity.state = 'active'
                   AND ${first_backend} =
                       ANY (pg_blocking_pids(activity.pid))
                   AND pending.locktype = 'object'
                   AND pending.classid = 'pg_am'::regclass
                   AND pending.objid = ${oid}
                   AND pending.objsubid = 3
                   AND pending.mode = 'ExclusiveLock'
                   AND NOT pending.granted
            );" 2>/dev/null || true)
        if [ "${lock_proof}" = "t" ]; then
            break
        fi
        sleep 0.05
    done
    [ "${lock_proof}" = "t" ] ||
        fail "second compactor was not blocked by backend ${first_backend} on the serial_idx maintenance lock"
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

test_publication_preparation_retry() {
    local compactor_output="${CLIENT_DIR}/publication_compactor.log"
    local first_spill_output="${CLIENT_DIR}/publication_first_spill.log"
    local second_spill_output="${CLIENT_DIR}/publication_second_spill.log"
    local compactor_pid
    local spiller_pid
    local backend
    local oid
    local retry_marker
    local retry_count

    log "Case: publication retries after a spill races prepared identity..."
    oid=$(index_oid publication_idx)
    start_compaction_with_settings \
        pgts-publication-compactor publication_idx \
        "SET pg_textsearch.debug_compaction_pause_after_select_ms = ${PAUSE_MS};
         SET pg_textsearch.debug_compaction_pause_before_publish_ms = ${PAUSE_MS};" \
        "${compactor_output}"
    compactor_pid=${STARTED_PID}
    backend=$(backend_pid pgts-publication-compactor)
    wait_for_marker after-select "${oid}" "${backend}"

    start_sql pgts-publication-first-spill "
        INSERT INTO publication_docs(body)
        SELECT 'common publicationcase first prefix ' || gs
          FROM generate_series(1, 5) gs;
        SELECT bm25_spill_index('publication_idx');" "${first_spill_output}"
    spiller_pid=${STARTED_PID}
    require_completion_during_pause \
        "${spiller_pid}" "${compactor_pid}" "first publication spill" \
        "${first_spill_output}" after-select "${oid}" "${backend}"

    wait_for_marker before-publish "${oid}" "${backend}"
    start_sql pgts-publication-second-spill "
        INSERT INTO publication_docs(body)
        SELECT 'common publicationcase second prefix ' || gs
          FROM generate_series(1, 7) gs;
        SELECT bm25_spill_index('publication_idx');" "${second_spill_output}"
    spiller_pid=${STARTED_PID}
    require_completion_during_pause \
        "${spiller_pid}" "${compactor_pid}" "second publication spill" \
        "${second_spill_output}" before-publish "${oid}" "${backend}"

    wait_success "${compactor_pid}" 15 \
        "publication compactor" "${compactor_output}"
    retry_marker="pg_textsearch compaction publication retry for index ${oid} backend ${backend}"
    retry_count=$(grep -Fc "${retry_marker}" "${LOGFILE}" || true)
    [ "${retry_count}" = "1" ] ||
        fail "publication retried ${retry_count} times after one identity race"
    assert_graph publication_idx "{2,1,0,0,0,0,0,0}"
    assert_all_documents \
        publication_docs publication_idx publicationcase
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

test_cancel_after_allocation() {
    local phase=$1
    local table_name=$2
    local index_name=$3
    local control_table=$4
    local control_index=$5
    local token=$6
    local app_name="pgts-${phase}-cancel"
    local output="${CLIENT_DIR}/${phase}_cancel.log"
    local compactor_pid
    local backend
    local oid
    local before_graph
    local before_pending
    local before_blocks
    local cancelled_blocks
    local final_blocks
    local control_before_blocks
    local control_final_blocks
    local cancel_result

    log "Case: cancellation after ${phase} allocation discards owned pages..."
    oid=$(index_oid "${index_name}")
    before_graph=$(graph "${index_name}")
    before_pending=$(pending_free "${index_name}")
    before_blocks=$(relation_blocks "${index_name}")
    control_before_blocks=$(relation_blocks "${control_index}")

    start_compaction_with_settings \
        "${app_name}" "${index_name}" \
        "SET pg_textsearch.debug_compaction_pause_after_allocation = '${phase}';" \
        "${output}"
    compactor_pid=${STARTED_PID}
    backend=$(backend_pid "${app_name}")
    wait_for_allocation_marker "${phase}" "${oid}" "${backend}"

    cancel_result=$(sql -c "SELECT pg_cancel_backend(${backend});")
    [ "${cancel_result}" = "t" ] ||
        fail "pg_cancel_backend(${backend}) returned ${cancel_result}"
    wait_for_exit "${compactor_pid}" 10 "${phase} cancelled compactor"
    if wait "${compactor_pid}"; then
        fail "${phase} cancelled compactor unexpectedly succeeded"
    fi
    grep -Fq "canceling statement due to user request" "${output}" ||
        fail "${phase} compactor did not report query cancellation"

    [ "$(graph "${index_name}")" = "${before_graph}" ] ||
        fail "${phase} cancellation changed the published segment graph"
    [ "$(pending_free "${index_name}")" = "${before_pending}" ] ||
        fail "${phase} cancellation changed the pending-free count"
    assert_all_documents "${table_name}" "${index_name}" "${token}"
    cancelled_blocks=$(relation_blocks "${index_name}")
    [ "${cancelled_blocks}" -ge "${before_blocks}" ] ||
        fail "${phase} cancellation unexpectedly shrank the index"

    sql -c "SELECT bm25_compact_step('${index_name}'::regclass);" >/dev/null
    sql -c "SELECT bm25_compact_step('${control_index}'::regclass);" >/dev/null
    assert_graph "${index_name}" "{0,1,0,0,0,0,0,0}"
    assert_graph "${control_index}" "{0,1,0,0,0,0,0,0}"
    assert_all_documents "${table_name}" "${index_name}" "${token}"
    assert_all_documents "${control_table}" "${control_index}" "${token}"

    final_blocks=$(relation_blocks "${index_name}")
    control_final_blocks=$(relation_blocks "${control_index}")
    [ $((final_blocks - before_blocks)) -eq \
      $((control_final_blocks - control_before_blocks)) ] ||
        fail "${phase} discarded pages were not reused by later compaction"

    sql -c "
        INSERT INTO ${table_name}(body)
        VALUES ('common ${token} post-cancel spill');
        SELECT bm25_spill_index('${index_name}');" >/dev/null
    assert_graph "${index_name}" "{1,1,0,0,0,0,0,0}"
    assert_all_documents "${table_name}" "${index_name}" "${token}"
}

test_partial_allocation_cancellation() {
    test_cancel_after_allocation \
        output-data output_data_docs output_data_idx \
        output_data_control_docs output_data_control_idx output_datacase
    test_cancel_after_allocation \
        page-index page_index_docs page_index_idx \
        page_index_control_docs page_index_control_idx page_indexcase
    test_cancel_after_allocation \
        tombstone tombstone_docs tombstone_idx \
        tombstone_control_docs tombstone_control_idx tombstonecase
}

main() {
    setup_cluster
    verify_guc_contract
    seed_all_indexes
    test_inline_policy_runs_one_pass
    test_source_estimation_progress
    test_scan_progress
    test_scan_progress_after_restamp
    test_insert_progress
    test_spill_prefix_progress
    test_same_index_serialization
    test_different_index_overlap
    test_publication_preparation_retry
    test_cancel_before_publish
    test_partial_allocation_cancellation
    log "All deterministic non-blocking compaction cases passed"
}

main "$@"
