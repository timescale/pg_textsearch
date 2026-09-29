#!/bin/bash
#
# Regression test for issue #464: index OIDs are database-local, while the
# pg_textsearch registry is cluster-wide.  Template clones make matching OIDs
# deterministic and let this test verify that registry and cache state remain
# independent across databases.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$(cd "${SCRIPT_DIR}/.." && pwd -P)"
TEST_PORT="${TEST_PORT:-55463}"
TEST_HOST=127.0.0.1
TOP_K=3
SEED_DB=pg_textsearch_registry_seed
DB_A=pg_textsearch_registry_a
DB_B=pg_textsearch_registry_b
DB_DROP=pg_textsearch_registry_drop
DB_FORCE=pg_textsearch_registry_force
DATA_DIR="${TEST_DIR}/tmp_cross_database_registry_${TEST_PORT}_$$"
LOGFILE="${DATA_DIR}/postgres.log"
PRESERVED_LOGFILE="${TEST_DIR}/tmp_cross_database_registry_${TEST_PORT}_$$_postgres.log"
COMMAND_TIMEOUT_SECONDS=20
HOLDER_TIMEOUT_SECONDS=60

PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

PASS_COUNT=0
FAIL_COUNT=0
DATA_DIR_OWNED=0
DATA_DIR_ID=
HOLDER_PID=
HOLDER_APPLICATION_NAME=

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
info() { echo -e "${BLUE}[$(date '+%H:%M:%S')] $1${NC}"; }

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    log "PASS: $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo -e "${RED}[$(date '+%H:%M:%S')] FAIL: $1${NC}"
}

postmaster_pid_is_alive() {
    local pid

    data_dir_is_owned || return 1
    [ -f "${DATA_DIR}/postmaster.pid" ] || return 1
    pid=$(sed -n '1p' "${DATA_DIR}/postmaster.pid")
    [[ "${pid}" =~ ^[0-9]+$ ]] || return 1
    kill -0 "${pid}" 2>/dev/null
}

data_dir_is_owned() {
    local current_id

    [ "${DATA_DIR_OWNED}" -eq 1 ] || return 1
    [ -d "${DATA_DIR}" ] && [ ! -L "${DATA_DIR}" ] || return 1
    current_id=$(stat -c '%d:%i' -- "${DATA_DIR}") || return 1
    [ "${current_id}" = "${DATA_DIR_ID}" ]
}

postmaster_matches_data_dir() {
    local pid_data_dir

    data_dir_is_owned || return 1
    [ -f "${DATA_DIR}/postmaster.pid" ] || return 1
    pid_data_dir=$(sed -n '2p' "${DATA_DIR}/postmaster.pid")
    [ "${pid_data_dir}" = "${DATA_DIR}" ] || return 1
    "${PGBINDIR}/pg_ctl" status -D "${DATA_DIR}" >/dev/null 2>&1
}

preserve_failure_log() {
    data_dir_is_owned || return 1
    ln -T -- "${LOGFILE}" "${PRESERVED_LOGFILE}"
}

verify_test_postmaster() {
    local actual_data_dir

    if ! postmaster_matches_data_dir; then
        fail "PostgreSQL postmaster is not running from ${DATA_DIR}"
        return 1
    fi

    if ! actual_data_dir=$(
        "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
            -d postgres -v ON_ERROR_STOP=1 -tA \
            -c "SHOW data_directory;" 2>&1
    ); then
        fail "Could not verify the test postmaster: ${actual_data_dir}"
        return 1
    fi

    if [ "${actual_data_dir}" != "${DATA_DIR}" ]; then
        fail "Port ${TEST_PORT} belongs to ${actual_data_dir}, not ${DATA_DIR}"
        return 1
    fi
}

cleanup() {
    local exit_code=$?
    local preserve_data_dir=0

    trap - EXIT
    trap '' INT TERM
    set +e

    if ! stop_database_holder; then
        fail "Could not stop the active database-holder process"
        exit_code=1
    fi

    if [ "${DATA_DIR_OWNED}" -eq 0 ]; then
        exit "${exit_code}"
    fi

    if ! data_dir_is_owned; then
        fail "Owned PGDATA identity changed; skipping cleanup"
        exit 1
    fi

    log "Cleaning up cross-database registry test (exit code: ${exit_code})..."

    if postmaster_matches_data_dir; then
        if ! timeout --signal=TERM --kill-after=2s \
            "${COMMAND_TIMEOUT_SECONDS}s" \
            "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m fast -w; then
            info "Fast shutdown failed; trying immediate shutdown"
            if ! timeout --signal=TERM --kill-after=2s \
                "${COMMAND_TIMEOUT_SECONDS}s" \
                "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" \
                -m immediate -w; then
                fail "PostgreSQL shutdown failed"
                preserve_data_dir=1
                exit_code=1
            fi
        fi
    elif postmaster_pid_is_alive; then
        fail "Cannot confirm ownership of the live postmaster in ${DATA_DIR}"
        preserve_data_dir=1
        exit_code=1
    fi

    if [ "${preserve_data_dir}" -eq 0 ] &&
        { postmaster_matches_data_dir || postmaster_pid_is_alive; }; then
        fail "PostgreSQL postmaster is still running after shutdown"
        preserve_data_dir=1
        exit_code=1
    fi

    if [ "${preserve_data_dir}" -eq 0 ] &&
        [ "${exit_code}" -ne 0 ] && [ -f "${LOGFILE}" ]; then
        if preserve_failure_log; then
            info "Preserved server log: ${PRESERVED_LOGFILE}"
        else
            fail "Could not safely preserve the server log; preserving ${DATA_DIR}"
            preserve_data_dir=1
            exit_code=1
        fi
    fi

    if [ "${preserve_data_dir}" -ne 0 ]; then
        info "Preserved PGDATA and server log: ${DATA_DIR}"
    elif ! data_dir_is_owned; then
        fail "Owned PGDATA identity changed; skipping removal"
        exit_code=1
    else
        rm -rf "${DATA_DIR}"
    fi

    exit "${exit_code}"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

run_sql_quiet() {
    local database="$1"
    local sql="$2"

    timeout --signal=TERM --kill-after=2s "${COMMAND_TIMEOUT_SECONDS}s" \
        "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
        -d "${database}" -v ON_ERROR_STOP=1 -q -c "${sql}" >/dev/null
}

run_sql_value() {
    local database="$1"
    local sql="$2"

    timeout --signal=TERM --kill-after=2s "${COMMAND_TIMEOUT_SECONDS}s" \
        "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
        -d "${database}" -v ON_ERROR_STOP=1 -tA -c "${sql}" 2>&1
}

run_createdb() {
    timeout --signal=TERM --kill-after=2s "${COMMAND_TIMEOUT_SECONDS}s" \
        "${PGBINDIR}/createdb" -h "${TEST_HOST}" -p "${TEST_PORT}" "$@"
}

run_dropdb() {
    timeout --signal=TERM --kill-after=2s "${COMMAND_TIMEOUT_SECONDS}s" \
        "${PGBINDIR}/dropdb" -h "${TEST_HOST}" -p "${TEST_PORT}" "$@"
}

holder_process_is_running() {
    local state

    [ -n "${HOLDER_PID}" ] || return 1
    state=$(ps -o stat= -p "${HOLDER_PID}" 2>/dev/null) || return 1
    [[ "${state}" != Z* ]]
}

wait_for_holder_exit() {
    local deadline=$((SECONDS + 10))
    local holder_pid="${HOLDER_PID}"

    [ -n "${holder_pid}" ] || return 0

    while holder_process_is_running; do
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            return 1
        fi
        sleep 0.1
    done

    wait "${holder_pid}" 2>/dev/null || true
    HOLDER_PID=
    HOLDER_APPLICATION_NAME=
}

start_database_holder() {
    local database="$1"
    local application_name="$2"
    local connected

    if [ -n "${HOLDER_PID}" ]; then
        fail "Refusing to replace an active database-holder process"
        return 1
    fi

    PGAPPNAME="${application_name}" \
        timeout --signal=TERM --kill-after=2s \
        "${HOLDER_TIMEOUT_SECONDS}s" \
        "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
        -d "${database}" -v ON_ERROR_STOP=1 -q \
        -c "SELECT pg_sleep(300);" >/dev/null 2>&1 &
    HOLDER_PID=$!
    HOLDER_APPLICATION_NAME="${application_name}"

    for _ in $(seq 1 100); do
        connected=$(run_sql_value postgres "
            SELECT count(*)
            FROM pg_stat_activity
            WHERE datname = '${database}'
              AND application_name = '${application_name}';
        ")
        if [ "${connected}" = "1" ]; then
            pass "${database} has an active test connection"
            return 0
        fi
        if ! holder_process_is_running; then
            wait_for_holder_exit || true
            fail "${database} test connection exited before registering"
            return 1
        fi
        sleep 0.05
    done

    fail "${database} test connection did not register"
    return 1
}

stop_database_holder() {
    local holder_pid="${HOLDER_PID}"

    [ -n "${holder_pid}" ] || return 0

    if postmaster_matches_data_dir &&
        [ -n "${HOLDER_APPLICATION_NAME}" ]; then
        run_sql_quiet postgres "
            SELECT pg_terminate_backend(pid)
            FROM pg_stat_activity
            WHERE application_name = '${HOLDER_APPLICATION_NAME}';
        " || true
    fi

    if wait_for_holder_exit; then
        return 0
    fi

    kill "${holder_pid}" 2>/dev/null || true
    if wait_for_holder_exit; then
        return 0
    fi

    kill -KILL "${holder_pid}" 2>/dev/null || true
    wait "${holder_pid}" 2>/dev/null || true
    HOLDER_PID=
    HOLDER_APPLICATION_NAME=
    return 1
}

assert_equals() {
    local label="$1"
    local expected="$2"
    local actual="$3"

    if [ "${actual}" = "${expected}" ]; then
        pass "${label}"
    else
        fail "${label}: expected '${expected}', got '${actual}'"
    fi
}

cache_cold_build() {
    local database="$1"

    cache_cold_build_index "${database}" docs_idx
}

cache_cold_build_index() {
    local database="$1"
    local index_name="$2"

    run_sql_value "${database}" "
        SELECT result || '|' || records_applied || '|' ||
               cursor_seq || '|' || estimated_bytes
        FROM bm25_cache_cold_build('${index_name}');
    "
}

cache_apply() {
    local database="$1"

    cache_apply_index "${database}" docs_idx
}

cache_apply_index() {
    local database="$1"
    local index_name="$2"

    run_sql_value "${database}" "
        SELECT result || '|' || records_applied || '|' ||
               cursor_seq || '|' || estimated_bytes
        FROM bm25_cache_apply_to_tail('${index_name}');
    "
}

top_hits() {
    local database="$1"
    local term="$2"

    run_sql_value "${database}" "
        SELECT count(*) || '|' ||
               bool_and(body LIKE '${term} marker %') || '|' ||
               string_agg(id::text, ',' ORDER BY score, id)
        FROM (
            SELECT id, body,
                   body <@> to_bm25query('${term}', 'docs_idx') AS score
            FROM docs
            ORDER BY body <@> to_bm25query('${term}', 'docs_idx')
            LIMIT ${TOP_K}
        ) hits
    "
}

setup_cluster() {
    log "Setting up isolated PostgreSQL cluster..."

    if ! mkdir -m 700 -- "${DATA_DIR}" 2>/dev/null; then
        fail "Refusing to replace existing PGDATA: ${DATA_DIR}"
        return 1
    fi
    if ! DATA_DIR_ID=$(stat -c '%d:%i' -- "${DATA_DIR}"); then
        fail "Could not record ownership of PGDATA: ${DATA_DIR}"
        return 1
    fi
    DATA_DIR_OWNED=1

    if ! "${PGBINDIR}/initdb" -D "${DATA_DIR}" \
        --auth-local=trust --auth-host=trust >/dev/null 2>&1; then
        fail "initdb failed"
        return 1
    fi

    cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
max_connections = 20
shared_buffers = 128MB
unix_socket_directories = ''
listen_addresses = '${TEST_HOST}'
log_min_messages = warning
shared_preload_libraries = 'pg_textsearch'
pg_textsearch.bulk_load_threshold = 0
pg_textsearch.memtable_pages_threshold = 0
pg_textsearch.memtable_cache_enabled = on
EOF

    if ! timeout --signal=TERM --kill-after=2s \
        "${COMMAND_TIMEOUT_SECONDS}s" \
        "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w; then
        fail "PostgreSQL failed to start"
        return 1
    fi
    verify_test_postmaster || return 1

    run_createdb "${SEED_DB}"
    run_sql_quiet "${SEED_DB}" "
        CREATE EXTENSION pg_textsearch VERSION '1.5.0-dev';
        CREATE TABLE docs (id bigserial PRIMARY KEY, body text NOT NULL);
        CREATE INDEX docs_idx ON docs USING bm25 (body)
            WITH (text_config = 'english');
        INSERT INTO docs(body) VALUES ('shared seed document');
    "

    run_createdb --template="${SEED_DB}" "${DB_A}"
    run_createdb --template="${SEED_DB}" "${DB_B}"
    run_dropdb "${SEED_DB}"

    # Clear the registry entry created while building the template.  The
    # cloned databases retain identical catalogs and relation files, but the
    # first post-restart access must rebuild each database's registry state.
    if ! timeout --signal=TERM --kill-after=2s \
        "${COMMAND_TIMEOUT_SECONDS}s" \
        "${PGBINDIR}/pg_ctl" restart -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w; then
        fail "PostgreSQL failed to restart"
        return 1
    fi
    verify_test_postmaster || return 1
}

main() {
    local oid_a oid_b
    local cold_a cold_b
    local a_cursor a_bytes b_cursor b_bytes
    local a_after_b_cache a_before_reindex a_after_reindex
    local global_a global_b global_after_evict expected_global
    local evict_result a_after_evict b_after_evict
    local a_top_hits b_top_hits
    local baseline_global b_before_drop b_after_drop
    local drop_cache drop_extra_cache drop_bytes drop_extra_bytes
    local drop_before_failed drop_after_failed
    local drop_extra_before_failed drop_extra_after_failed
    local global_with_drop global_after_drop drop_status
    local force_cache force_extra_cache force_bytes force_extra_bytes
    local global_with_force global_after_force force_status

    command -v "${PGBINDIR}/pg_ctl" >/dev/null 2>&1 ||
        {
            fail "pg_ctl not found"
            return 1
        }
    command -v timeout >/dev/null 2>&1 ||
        {
            fail "timeout not found"
            return 1
        }

    setup_cluster || return 1

    oid_a=$(run_sql_value "${DB_A}" "SELECT 'docs_idx'::regclass::oid;")
    oid_b=$(run_sql_value "${DB_B}" "SELECT 'docs_idx'::regclass::oid;")
    if [[ "${oid_a}" =~ ^[0-9]+$ ]] && [ "${oid_a}" = "${oid_b}" ]; then
        pass "template clones have matching BM25 index OIDs (${oid_a})"
    else
        fail "matching-OID setup failed: db_a=${oid_a}, db_b=${oid_b}"
    fi

    run_sql_quiet "${DB_A}" "
        INSERT INTO docs(body)
        SELECT 'alpha marker ' || lpad(g::text, 2, '0') ||
               repeat(' alpha', 10 - g)
        FROM generate_series(1, 4) g;
    "
    run_sql_quiet "${DB_B}" "
        INSERT INTO docs(body)
        SELECT 'bravo marker ' || lpad(g::text, 2, '0') ||
               repeat(' bravo', 10 - g)
        FROM generate_series(1, 9) g;
    "

    cold_a=$(cache_cold_build "${DB_A}")
    IFS='|' read -r result records a_cursor a_bytes <<<"${cold_a}"
    assert_equals "database A cold-build result" "OK" "${result}"
    assert_equals "database A cache record count" "5" "${records}"
    if [[ "${a_bytes}" =~ ^[0-9]+$ ]] && [ "${a_bytes}" -gt 0 ]; then
        pass "database A cache has independent nonzero accounting"
    else
        fail "database A cache accounting is invalid: '${a_bytes}'"
    fi

    cold_b=$(cache_cold_build "${DB_B}")
    IFS='|' read -r result records b_cursor b_bytes <<<"${cold_b}"
    assert_equals "database B cold-build is independent" "OK" "${result}"
    assert_equals "database B cache record count" "10" "${records}"
    if [[ "${b_bytes}" =~ ^[0-9]+$ ]] && [ "${b_bytes}" -gt 0 ]; then
        pass "database B cache has independent nonzero accounting"
    else
        fail "database B cache accounting is invalid: '${b_bytes}'"
    fi

    a_after_b_cache=$(cache_apply "${DB_A}")
    assert_equals "database B cache build leaves A cache unchanged" \
        "OK|0|${a_cursor}|${a_bytes}" "${a_after_b_cache}"

    global_a=$(run_sql_value "${DB_A}" \
        "SELECT bm25_cache_global_estimated_bytes();")
    global_b=$(run_sql_value "${DB_B}" \
        "SELECT bm25_cache_global_estimated_bytes();")
    assert_equals "global cache accounting is cluster-wide" \
        "${global_a}" "${global_b}"
    if [[ "${a_bytes}" =~ ^[0-9]+$ ]] &&
        [[ "${b_bytes}" =~ ^[0-9]+$ ]]; then
        expected_global=$((a_bytes + b_bytes))
        assert_equals "global accounting includes both databases" \
            "${expected_global}" "${global_a}"
    else
        fail "cannot validate global accounting: A=${a_bytes}, B=${b_bytes}"
    fi

    a_top_hits=$(top_hits "${DB_A}" "alpha")
    b_top_hits=$(top_hits "${DB_B}" "bravo")
    assert_equals "database A returns its ranked top ${TOP_K}" \
        "3|true|2,3,4" "${a_top_hits}"
    assert_equals "database B returns its ranked top ${TOP_K}" \
        "3|true|2,3,4" "${b_top_hits}"

    # The eviction budget remains cluster-wide.  With B as the caller, A is
    # the only eligible victim even though both indexes have the same OID.
    evict_result=$(run_sql_value "${DB_B}" \
        "SELECT bm25_cache_evict_largest('docs_idx');")
    assert_equals "cross-database eviction distinguishes the caller key" \
        "evicted" "${evict_result}"
    a_after_evict=$(cache_apply "${DB_A}")
    b_after_evict=$(cache_apply "${DB_B}")
    assert_equals "cross-database eviction selected database A" \
        "NOT_INITIALIZED|0|0|0" "${a_after_evict}"
    assert_equals "cross-database eviction preserved caller B" \
        "OK|0|${b_cursor}|${b_bytes}" "${b_after_evict}"
    global_after_evict=$(run_sql_value "${DB_A}" \
        "SELECT bm25_cache_global_estimated_bytes();")
    assert_equals "cross-database eviction drains global accounting" \
        "${b_bytes}" "${global_after_evict}"

    cold_a=$(cache_cold_build "${DB_A}")
    IFS='|' read -r result records a_cursor a_bytes <<<"${cold_a}"
    assert_equals "database A cache rebuild after eviction" "OK" "${result}"
    assert_equals "database A rebuilt cache record count" "5" "${records}"

    a_before_reindex=$(cache_apply "${DB_A}")
    if run_sql_quiet "${DB_B}" "REINDEX INDEX docs_idx;"; then
        pass "database B REINDEX completed"
    else
        fail "database B REINDEX failed"
    fi
    a_after_reindex=$(cache_apply "${DB_A}")
    assert_equals "database B REINDEX leaves A registry/cache state unchanged" \
        "${a_before_reindex}" "${a_after_reindex}"

    a_top_hits=$(top_hits "${DB_A}" "alpha")
    b_top_hits=$(top_hits "${DB_B}" "bravo")
    assert_equals "database A top ${TOP_K} survives B REINDEX" \
        "3|true|2,3,4" "${a_top_hits}"
    assert_equals "database B top ${TOP_K} survives its REINDEX" \
        "3|true|2,3,4" "${b_top_hits}"

    baseline_global=$(run_sql_value "${DB_B}" \
        "SELECT bm25_cache_global_estimated_bytes();")
    b_before_drop=$(cache_apply "${DB_B}")

    run_createdb --template="${DB_A}" "${DB_DROP}"
    run_sql_quiet "${DB_DROP}" "
        INSERT INTO docs(body)
        SELECT 'charlie marker ' || lpad(g::text, 2, '0') ||
               repeat(' charlie', 10 - g)
        FROM generate_series(1, 6) g;
        CREATE TABLE extra_docs (
            id bigserial PRIMARY KEY,
            body text NOT NULL
        );
        CREATE INDEX extra_docs_idx ON extra_docs USING bm25 (body)
            WITH (text_config = 'english');
        INSERT INTO extra_docs(body)
        SELECT 'delta marker ' || lpad(g::text, 2, '0') ||
               repeat(' delta', 8 - g)
        FROM generate_series(1, 4) g;
    "
    drop_cache=$(cache_cold_build "${DB_DROP}")
    IFS='|' read -r result records _ drop_bytes <<<"${drop_cache}"
    assert_equals "drop-test database cold-build result" "OK" "${result}"
    assert_equals "drop-test database cache record count" "11" "${records}"
    if [[ "${drop_bytes}" =~ ^[0-9]+$ ]] && [ "${drop_bytes}" -gt 0 ]; then
        pass "drop-test database cache has nonzero accounting"
    else
        fail "drop-test database cache accounting is invalid: '${drop_bytes}'"
    fi
    drop_extra_cache=$(cache_cold_build_index "${DB_DROP}" extra_docs_idx)
    IFS='|' read -r result records _ drop_extra_bytes <<<"${drop_extra_cache}"
    assert_equals "drop-test second index cold-build result" "OK" "${result}"
    assert_equals "drop-test second index cache record count" "4" "${records}"
    if [[ "${drop_extra_bytes}" =~ ^[0-9]+$ ]] &&
        [ "${drop_extra_bytes}" -gt 0 ]; then
        pass "drop-test second index has nonzero accounting"
    else
        fail "drop-test second index accounting is invalid: '${drop_extra_bytes}'"
    fi
    global_with_drop=$(run_sql_value "${DB_B}" \
        "SELECT bm25_cache_global_estimated_bytes();")
    assert_equals "drop-test database contributes both registry entries" \
        "$((baseline_global + drop_bytes + drop_extra_bytes))" \
        "${global_with_drop}"

    drop_before_failed=$(cache_apply "${DB_DROP}")
    drop_extra_before_failed=$(cache_apply_index "${DB_DROP}" extra_docs_idx)
    start_database_holder "${DB_DROP}" pgts_drop_holder
    set +e
    run_dropdb "${DB_DROP}" >/dev/null 2>&1
    drop_status=$?
    set -e
    if [ "${drop_status}" -eq 0 ]; then
        fail "ordinary DROP DATABASE unexpectedly succeeded with an active connection"
    elif [ "${drop_status}" -eq 124 ] || [ "${drop_status}" -eq 137 ]; then
        fail "ordinary DROP DATABASE timed out instead of failing promptly"
    else
        pass "failed DROP DATABASE preserves the populated database"
    fi
    assert_equals "failed DROP DATABASE preserves global accounting" \
        "${global_with_drop}" \
        "$(run_sql_value "${DB_B}" \
            "SELECT bm25_cache_global_estimated_bytes();")"
    drop_after_failed=$(cache_apply "${DB_DROP}")
    assert_equals "failed DROP DATABASE preserves registry/cache state" \
        "${drop_before_failed}" "${drop_after_failed}"
    drop_extra_after_failed=$(cache_apply_index "${DB_DROP}" extra_docs_idx)
    assert_equals "failed DROP DATABASE preserves every registry entry" \
        "${drop_extra_before_failed}" "${drop_extra_after_failed}"
    stop_database_holder ||
        fail "failed DROP DATABASE holder did not stop within 10 seconds"

    if run_dropdb "${DB_DROP}"; then
        pass "successful DROP DATABASE completed"
    else
        fail "successful DROP DATABASE failed"
    fi
    global_after_drop=$(run_sql_value "${DB_B}" \
        "SELECT bm25_cache_global_estimated_bytes();")
    assert_equals "successful DROP DATABASE releases only its accounting" \
        "${baseline_global}" "${global_after_drop}"
    assert_equals "successful DROP DATABASE removes both cache charges" \
        "$((drop_bytes + drop_extra_bytes))" \
        "$((global_with_drop - global_after_drop))"
    b_after_drop=$(cache_apply "${DB_B}")
    assert_equals "successful DROP DATABASE preserves database B cache" \
        "${b_before_drop}" "${b_after_drop}"
    assert_equals "successful DROP DATABASE preserves database B queries" \
        "3|true|2,3,4" "$(top_hits "${DB_B}" "bravo")"

    run_createdb --template="${DB_A}" "${DB_FORCE}"
    run_sql_quiet "${DB_FORCE}" "
        INSERT INTO docs(body)
        SELECT 'foxtrot marker ' || lpad(g::text, 2, '0') ||
               repeat(' foxtrot', 12 - g)
        FROM generate_series(1, 7) g;
        CREATE TABLE force_extra_docs (
            id bigserial PRIMARY KEY,
            body text NOT NULL
        );
        CREATE INDEX force_extra_docs_idx
            ON force_extra_docs USING bm25 (body)
            WITH (text_config = 'english');
        INSERT INTO force_extra_docs(body)
        SELECT 'golf marker ' || lpad(g::text, 2, '0') ||
               repeat(' golf', 8 - g)
        FROM generate_series(1, 4) g;
    "
    force_cache=$(cache_cold_build "${DB_FORCE}")
    IFS='|' read -r result records _ force_bytes <<<"${force_cache}"
    assert_equals "force-drop database cold-build result" "OK" "${result}"
    assert_equals "force-drop database cache record count" "12" "${records}"
    force_extra_cache=$(
        cache_cold_build_index "${DB_FORCE}" force_extra_docs_idx
    )
    IFS='|' read -r result records _ force_extra_bytes <<<"${force_extra_cache}"
    assert_equals "force-drop second index cold-build result" "OK" "${result}"
    assert_equals "force-drop second index cache record count" "4" "${records}"
    if [[ "${force_bytes}" =~ ^[0-9]+$ ]] &&
        [[ "${force_extra_bytes}" =~ ^[0-9]+$ ]] &&
        [ "${force_bytes}" -gt 0 ] && [ "${force_extra_bytes}" -gt 0 ]; then
        pass "force-drop database has two nonzero cache charges"
    else
        fail "force-drop cache accounting is invalid: ${force_bytes}, ${force_extra_bytes}"
    fi
    global_with_force=$(run_sql_value "${DB_B}" \
        "SELECT bm25_cache_global_estimated_bytes();")
    assert_equals "force-drop database contributes both registry entries" \
        "$((baseline_global + force_bytes + force_extra_bytes))" \
        "${global_with_force}"

    start_database_holder "${DB_FORCE}" pgts_force_holder
    set +e
    run_dropdb --force "${DB_FORCE}" >/dev/null 2>&1
    force_status=$?
    set -e
    if [ "${force_status}" -eq 0 ]; then
        pass "DROP DATABASE WITH (FORCE) completed"
    elif [ "${force_status}" -eq 124 ] || [ "${force_status}" -eq 137 ]; then
        fail "DROP DATABASE WITH (FORCE) timed out"
    else
        fail "DROP DATABASE WITH (FORCE) failed with status ${force_status}"
    fi
    if wait_for_holder_exit; then
        pass "forced DROP DATABASE reaped its terminated client"
    else
        fail "forced DROP DATABASE client did not exit within 10 seconds"
        stop_database_holder || true
    fi
    global_after_force=$(run_sql_value "${DB_B}" \
        "SELECT bm25_cache_global_estimated_bytes();")
    assert_equals "DROP DATABASE WITH (FORCE) releases only its accounting" \
        "${baseline_global}" "${global_after_force}"
    assert_equals "DROP DATABASE WITH (FORCE) removes both cache charges" \
        "$((force_bytes + force_extra_bytes))" \
        "$((global_with_force - global_after_force))"
    assert_equals "DROP DATABASE WITH (FORCE) preserves database B cache" \
        "${b_before_drop}" "$(cache_apply "${DB_B}")"
    assert_equals "DROP DATABASE WITH (FORCE) preserves database B queries" \
        "3|true|2,3,4" "$(top_hits "${DB_B}" "bravo")"

    info "Assertions: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
    if [ "${FAIL_COUNT}" -ne 0 ]; then
        return 1
    fi

    log "All cross-database registry tests passed"
}

main "$@"
