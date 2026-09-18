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
# A deterministic first case pauses force merge after source selection and
# proves the VACUUM backend waits on the exact private index maintenance
# lock.  The stress case then has writers spill many small segments, a merger
# force-merge them, a deleter create dead tuples, and a vacuumer run VACUUM
# concurrently (autovacuum is also aggressive).  Before the fix this fails
# within seconds; after it completes cleanly with no segment-header errors
# and no crash.
#
# It also covers the tombstone-drain-vs-truncate race (issue #465):
# bm25_force_merge truncates the index via tp_truncate_dead_pages
# while VACUUM drains the deferred-free tombstone chain, stranding the
# drain's page frees past EOF ("could not read blocks N..N").
#
# TEST_SIZE_MULTIPLIER scales the loop counts (default 1.0) for
# constrained runners.  Prefer full scale: these are timing races, and
# a shorter run finds them less often.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
export PATH="${PGBINDIR}:${PATH}"
TEST_PORT=55442
TEST_DB=vacuum_concurrent_merge_test
DATA_DIR="${SCRIPT_DIR}/../tmp_vacuum_concurrent_merge"
SOCKET_DIR="${SCRIPT_DIR}/.vcm_sock"
LOGFILE="${DATA_DIR}/postgres.log"
ERR_DIR="${DATA_DIR}/client_logs"
KEEP_DIR="${SCRIPT_DIR}/../tmp_vacuum_concurrent_merge_logs"
TEST_SIZE_MULTIPLIER=${TEST_SIZE_MULTIPLIER:-1.0}
PAUSE_MS=5000
STRESS_TIMEOUT_SECONDS=120

# Scale a loop count by TEST_SIZE_MULTIPLIER (minimum 1).
scaled_count() {
    awk "BEGIN {n = int($1 * ${TEST_SIZE_MULTIPLIER} + 0.5);
                print (n < 1 ? 1 : n)}"
}

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
    warn "pg_locks for test indexes:"
    sql -F '|' -c "
        SELECT pid, locktype,
               CASE
                   WHEN locktype = 'relation' THEN relation::regclass::text
                   WHEN locktype = 'object' THEN
                       classid::regclass::text || ':' || objid || ':' || objsubid
               END,
               mode, granted
          FROM pg_locks
         WHERE (locktype = 'relation'
                AND relation IN ('docs_bm25'::regclass,
                                 'coord_bm25'::regclass,
                                 'identify_bm25'::regclass))
            OR (locktype = 'object'
                AND classid = 'pg_am'::regclass
                AND objid IN ('docs_bm25'::regclass,
                              'coord_bm25'::regclass,
                              'identify_bm25'::regclass))
         ORDER BY pid, locktype, mode;" 2>&1 || true
    warn "server log tail:"
    tail -n 80 "${LOGFILE}" 2>/dev/null || true
    warn "client log tails:"
    for file in "${ERR_DIR}"/*.log; do
        [ -e "${file}" ] || continue
        echo "==> ${file} <=="
        tail -n 20 "${file}" || true
    done
}

error() {
    echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"
    diagnose
    exit 1
}

cleanup() {
    local exit_code=$?
    local pid

    trap - EXIT INT TERM
    log "Cleaning up (exit code: $exit_code)..."
    for pid in $(jobs -p); do
        kill "${pid}" 2>/dev/null || true
    done
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    # Preserve the logs on failure: the data dir is too large to
    # upload, and without the server log a CI failure is undebuggable.
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
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    mkdir -p "${DATA_DIR}" "${SOCKET_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1
    mkdir -p "${ERR_DIR}"

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
unix_socket_directories = '${SOCKET_DIR}'
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

    createdb -h "${SOCKET_DIR}" -p ${TEST_PORT} ${TEST_DB}
    sql -c "CREATE EXTENSION pg_textsearch;" >/dev/null
    log "Test database ready"
}

PSQL="psql -h ${SOCKET_DIR} -p ${TEST_PORT} -d ${TEST_DB} -qAt -v ON_ERROR_STOP=1"

seed_data() {
    log "Creating bm25 indexes and seeding data..."
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

CREATE TABLE coord_docs (id bigserial PRIMARY KEY, body text NOT NULL);
CREATE INDEX coord_bm25 ON coord_docs USING bm25(body)
    WITH (text_config='english', compaction='off');
ALTER TABLE coord_docs SET (autovacuum_enabled=false);
INSERT INTO coord_docs(body)
SELECT 'coordcase batch1 document ' || gs FROM generate_series(1, 8) gs;
SELECT bm25_spill_index('coord_bm25');
INSERT INTO coord_docs(body)
SELECT 'coordcase batch2 document ' || gs FROM generate_series(1, 8) gs;
SELECT bm25_spill_index('coord_bm25');
INSERT INTO coord_docs(body)
SELECT 'coordcase batch3 document ' || gs FROM generate_series(1, 8) gs;
SELECT bm25_spill_index('coord_bm25');
INSERT INTO coord_docs(body)
SELECT 'coordcase batch4 document ' || gs FROM generate_series(1, 8) gs;
SELECT bm25_spill_index('coord_bm25');

CREATE TABLE identify_docs (
    id bigserial PRIMARY KEY,
    body text NOT NULL
);
CREATE INDEX identify_bm25 ON identify_docs USING bm25(body)
    WITH (text_config='english', compaction='off');
ALTER TABLE identify_docs SET (autovacuum_enabled=false);
INSERT INTO identify_docs(body)
SELECT 'identify vacuum document ' || gs || ' ' || repeat(md5(gs::text), 8)
FROM generate_series(1, 20000) gs;
SELECT bm25_spill_index('identify_bm25');
DELETE FROM identify_docs WHERE id <= 1000;
SQL
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
    error "backend ${app_name} did not appear within 10 seconds"
}

client_backend_pid() {
    local output_file=$1
    local label=$2
    local deadline=$((SECONDS + 10))
    local pid

    while ((SECONDS < deadline)); do
        pid=$(sed -n '1p' "${output_file}" 2>/dev/null || true)
        if [[ "${pid}" =~ ^[0-9]+$ ]]; then
            echo "${pid}"
            return
        fi
        sleep 0.05
    done
    error "${label} did not report its backend PID within 10 seconds"
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
    error "did not observe log marker: ${marker}"
}

wait_for_vacuum_marker() {
    local oid=$1
    local backend=$2
    local deadline=$((SECONDS + 10))
    local marker="pg_textsearch VACUUM pause during identification for index ${oid} backend ${backend}"

    while ((SECONDS < deadline)); do
        if grep -Fq "${marker}" "${LOGFILE}" 2>/dev/null; then
            log "Observed VACUUM identification marker for index ${oid}, backend ${backend}"
            return
        fi
        sleep 0.05
    done
    error "did not observe log marker: ${marker}"
}

assert_still_paused() {
    local phase=$1
    local oid=$2
    local backend=$3
    local marker="pg_textsearch compaction resume after ${phase} for index ${oid} backend ${backend}"

    if grep -Fq "${marker}" "${LOGFILE}" 2>/dev/null; then
        error "VACUUM did not overlap the ${phase} compaction pause"
    fi
}

wait_for_exit() {
    local pid=$1
    local timeout_seconds=$2
    local label=$3
    local deadline=$((SECONDS + timeout_seconds))

    while kill -0 "${pid}" 2>/dev/null; do
        ((SECONDS < deadline)) ||
            error "${label} did not finish within ${timeout_seconds} seconds"
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
        error "${label} failed"
    fi
}

assert_no_segment_errors() {
    if grep -REIl \
        "invalid segment header|could not read blocks?.*read only [0-9]+ of|not a valid memtable page|invalid magic|magic mismatch" \
        "${ERR_DIR}" "${LOGFILE}" >/dev/null 2>&1; then
        warn "Found an index storage error:"
        grep -REIn \
            "invalid segment header|could not read blocks?.*read only [0-9]+ of|not a valid memtable page|invalid magic|magic mismatch" \
            "${ERR_DIR}" "${LOGFILE}" | sed -n '1,5p'
        error "TEST FAILED: concurrent maintenance reported an index storage error"
    fi
}

test_vacuum_identification_does_not_gate_readers() {
    local vacuum_output="${ERR_DIR}/identify_vacuum.log"
    local spill_output="${ERR_DIR}/identify_spill.log"
    local reader_output="${ERR_DIR}/identify_reader.log"
    local vacuum_pid
    local spill_pid
    local reader_pid
    local vacuum_backend
    local oid
    local reader_result

    log "Case: VACUUM identification does not gate later readers..."
    oid=$(sql -c "SELECT 'identify_bm25'::regclass::oid;")

    PGAPPNAME=pgts-identify-vacuum \
        PGOPTIONS="-c statement_timeout=90000 -c lock_timeout=30000 -c pg_textsearch.debug_compaction_pause_source_estimate_ms=${PAUSE_MS}" \
        psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 \
        -c "SELECT pg_backend_pid();" \
        -c "VACUUM identify_docs;" \
        >"${vacuum_output}" 2>&1 &
    vacuum_pid=$!
    vacuum_backend=$(client_backend_pid \
        "${vacuum_output}" "identification VACUUM client")
    wait_for_vacuum_marker "${oid}" "${vacuum_backend}"

    PGAPPNAME=pgts-identify-spill \
        PGOPTIONS="-c statement_timeout=90000 -c lock_timeout=30000" \
        sql -c "
            INSERT INTO identify_docs(body)
            VALUES ('queued spill reader visibility');
            SELECT bm25_spill_index('identify_bm25');" \
        >"${spill_output}" 2>&1 &
    spill_pid=$!
    sleep 0.25

    PGAPPNAME=pgts-identify-reader \
        PGOPTIONS="-c statement_timeout=2000 -c lock_timeout=1500" \
        sql -c "
            SELECT count(*)
              FROM (
                    SELECT id
                      FROM identify_docs
                     ORDER BY body <@> to_bm25query(
                                  'identify', 'identify_bm25')
                     LIMIT 1
                   ) ranked;" \
        >"${reader_output}" 2>&1 &
    reader_pid=$!
    wait_success "${reader_pid}" 3 "later identification reader" \
        "${reader_output}"
    reader_result=$(tail -n 1 "${reader_output}")
    [ "${reader_result}" = "1" ] ||
        error "later identification reader returned ${reader_result}"

    kill -0 "${vacuum_pid}" 2>/dev/null ||
        error "VACUUM identification ended before reader proof"
    wait_success "${vacuum_pid}" 15 "identification VACUUM" \
        "${vacuum_output}"
    wait_success "${spill_pid}" 15 "identification spill" "${spill_output}"

    log "Later reader completed while VACUUM identification was paused"
}

test_vacuum_waits_for_force_merge() {
    local compactor_output="${ERR_DIR}/deterministic_compactor.log"
    local vacuum_output="${ERR_DIR}/deterministic_vacuum.log"
    local compactor_pid
    local vacuum_pid
    local compactor_backend
    local vacuum_backend
    local oid
    local lock_proof=f
    local deadline
    local plan
    local ranked_ids
    local replacement_ids
    local resurrected_ids
    local reused_count
    local expected_ids="9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32"

    log "Case: VACUUM waits for a force merge with selected sources..."
    oid=$(sql -c "SELECT 'coord_bm25'::regclass::oid;")

    # Leave one committed, removable dead set so VACUUM must call
    # ambulkdelete even though the force merge's snapshot will keep the
    # post-selection deletes below from becoming removable immediately.
    sql -c "
        CREATE TABLE coord_deleted_slots (
            old_id bigint PRIMARY KEY,
            old_ctid tid NOT NULL
        );
        INSERT INTO coord_deleted_slots
        SELECT id, ctid
          FROM coord_docs
         WHERE id BETWEEN 1 AND 4;" >/dev/null
    sql -c "DELETE FROM coord_docs WHERE id BETWEEN 1 AND 4;" >/dev/null

    PGAPPNAME=pgts-vacuum-compactor \
        sql -c "
            SET statement_timeout = '90s';
            SET pg_textsearch.debug_compaction_pause_after_select_ms = ${PAUSE_MS};
            SELECT bm25_force_merge('coord_bm25');" \
        >"${compactor_output}" 2>&1 &
    compactor_pid=$!
    compactor_backend=$(backend_pid pgts-vacuum-compactor)
    wait_for_marker after-select "${oid}" "${compactor_backend}"

    sql -c "DELETE FROM coord_docs WHERE id BETWEEN 5 AND 8;" >/dev/null

    PGAPPNAME=pgts-blocked-vacuum \
        PGOPTIONS="-c statement_timeout=90000 -c lock_timeout=15000" \
        psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 \
        -c "SELECT pg_backend_pid();" -c "VACUUM coord_docs;" \
        >"${vacuum_output}" 2>&1 &
    vacuum_pid=$!
    vacuum_backend=$(client_backend_pid \
        "${vacuum_output}" "VACUUM client")

    deadline=$((SECONDS + 3))
    while ((SECONDS < deadline)); do
        lock_proof=$(sql -c "
            SELECT EXISTS (
                SELECT 1
                  FROM pg_stat_activity activity
                  JOIN pg_locks pending
                    ON pending.pid = activity.pid
                 WHERE activity.pid = ${vacuum_backend}
                   AND activity.state = 'active'
                   AND ${compactor_backend} =
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
        error "VACUUM was not blocked by backend ${compactor_backend} on the coord_bm25 maintenance lock"
    kill -0 "${vacuum_pid}" 2>/dev/null ||
        error "VACUUM exited before the selected-source compaction resumed"
    kill -0 "${compactor_pid}" 2>/dev/null ||
        error "force merge exited before VACUUM blocking was proved"
    assert_still_paused after-select "${oid}" "${compactor_backend}"

    wait_success "${compactor_pid}" 15 "force merge" "${compactor_output}"
    wait_success "${vacuum_pid}" 15 "VACUUM" "${vacuum_output}"

    sql -c "
        INSERT INTO coord_docs(body)
        SELECT 'replacement unrelated row ' || gs
          FROM generate_series(1, 64) gs;" >/dev/null
    reused_count=$(sql -c "
        SELECT count(*)
          FROM coord_deleted_slots slots
          JOIN coord_docs replacements
            ON replacements.ctid = slots.old_ctid
         WHERE replacements.body LIKE 'replacement unrelated row %';")
    [ "${reused_count}" = "4" ] ||
        error "replacement rows reused ${reused_count}/4 deleted CTID slots"
    replacement_ids=$(sql -c "
        SELECT string_agg(replacements.id::text, ',' ORDER BY replacements.id)
          FROM coord_deleted_slots slots
          JOIN coord_docs replacements
            ON replacements.ctid = slots.old_ctid
         WHERE replacements.body LIKE 'replacement unrelated row %';")
    [ -n "${replacement_ids}" ] ||
        error "could not identify replacement IDs in reused CTID slots"

    plan=$(sql -c "
        SET enable_seqscan = off;
        EXPLAIN (COSTS off)
        SELECT id
          FROM coord_docs
         ORDER BY body <@> to_bm25query('coordcase', 'coord_bm25')
         LIMIT 10000;")
    grep -Fq "Index Scan using coord_bm25 on coord_docs" <<<"${plan}" ||
        error "post-VACUUM assertion did not use coord_bm25"

    ranked_ids=$(sql -c "
        SET enable_seqscan = off;
        SELECT string_agg(id::text, ',' ORDER BY id)
          FROM (
            SELECT id
              FROM coord_docs
             ORDER BY body <@> to_bm25query('coordcase', 'coord_bm25')
             LIMIT 10000
          ) ranked;")
    [ "${ranked_ids}" = "${expected_ids}" ] ||
        error "forced BM25 Index Scan returned IDs ${ranked_ids}, expected ${expected_ids}"
    resurrected_ids=$(sql -c "
        SET enable_seqscan = off;
        WITH ranked AS MATERIALIZED (
            SELECT id
              FROM coord_docs
             ORDER BY body <@> to_bm25query('coordcase', 'coord_bm25')
             LIMIT 10000
        )
        SELECT coalesce(string_agg(ranked.id::text, ',' ORDER BY ranked.id), '')
          FROM ranked
          JOIN coord_docs replacements USING (id)
          JOIN coord_deleted_slots slots
            ON slots.old_ctid = replacements.ctid
         WHERE replacements.body LIKE 'replacement unrelated row %';")
    [ -z "${resurrected_ids}" ] ||
        error "old-term scan resurrected replacement IDs ${resurrected_ids} from deleted CTIDs ${replacement_ids}"
    assert_no_segment_errors

    log "VACUUM waited for force merge and removed all known deleted documents"
}

# Spill the memtable aggressively so many small segments accrue for the
# merger to compact.
writer() {
    for i in $(seq 1 $(scaled_count 120)); do
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
    local output

    for i in $(seq 1 $(scaled_count 250)); do
        if ! output=$($PSQL -c \
            "SET statement_timeout='60s'; SET lock_timeout='30s';
             SELECT bm25_spill_index('docs_bm25');
             SELECT bm25_force_merge('docs_bm25')" 2>&1); then
            printf '%s\n' "$output" >>"${ERR_DIR}/merger.log"
            return 30
        fi
        printf '%s\n' "$output" >>"${ERR_DIR}/merger.log"
    done
}

# Delete the oldest rows by id (monotonic -> no row-lock deadlocks) to
# create dead tuples for VACUUM bulk-delete to reclaim.
deleter() {
    for i in $(seq 1 $(scaled_count 250)); do
        $PSQL -c "SET statement_timeout='60s'; SET lock_timeout='30s';
                  DELETE FROM docs
                  WHERE id IN (SELECT id FROM docs ORDER BY id ASC LIMIT 120)" \
          >>"${ERR_DIR}/deleter.log" 2>&1 || return 40
        sleep 0.1
    done
}

# Explicit VACUUM in a loop deterministically exercises the bulk-delete and
# cleanup paths concurrently with the merger.
vacuumer() {
    for i in $(seq 1 $(scaled_count 200)); do
        PGOPTIONS="-c statement_timeout=60000 -c lock_timeout=30000" \
          $PSQL -c "VACUUM docs" \
          >>"${ERR_DIR}/vacuumer.log" 2>&1 || return 50
        sleep 0.05
    done
}

run_test() {
    log "Running concurrent writer + merger + deleter + vacuumer..."

    writer & w_pid=$!
    merger & m_pid=$!
    deleter & d_pid=$!
    vacuumer & v_pid=$!

    wait_success "${w_pid}" "${STRESS_TIMEOUT_SECONDS}" \
        "stress writer" "${ERR_DIR}/writer.log"
    wait_success "${m_pid}" "${STRESS_TIMEOUT_SECONDS}" \
        "stress merger" "${ERR_DIR}/merger.log"
    wait_success "${d_pid}" "${STRESS_TIMEOUT_SECONDS}" \
        "stress deleter" "${ERR_DIR}/deleter.log"
    wait_success "${v_pid}" "${STRESS_TIMEOUT_SECONDS}" \
        "stress vacuumer" "${ERR_DIR}/vacuumer.log"

    # The bug surfaces both server-side (autovacuum) and client-side
    # (explicit VACUUM).  Check both the server log and client logs.
    assert_no_segment_errors

    # An out-of-bounds alive-bitset write crashes the backend.
    if grep -qE "TRAP: failed Assert|was terminated by signal|server closed the connection unexpectedly" \
        "${LOGFILE}" "${ERR_DIR}"/*.log 2>/dev/null; then
        warn "Found a crash signature:"
        grep -hE "TRAP: failed Assert|was terminated by signal|alive_bitset" \
            "${LOGFILE}" 2>/dev/null | sed -n '1,5p'
        error "TEST FAILED: issue #411 reproduced (backend crash)"
    fi

    log "TEST PASSED: VACUUM survived concurrent spill/merge"
}

# Main
setup_test_db
seed_data
test_vacuum_identification_does_not_gate_readers
test_vacuum_waits_for_force_merge
run_test

log "All tests passed!"
exit 0
