#!/bin/bash
#
# End-to-end CREATE admission test for owner-scoped pg_durable workflows.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DATA_DIR="${REPO_ROOT}/test/tmp_durable_compaction"
KEEP_DIR="${REPO_ROOT}/test/tmp_durable_compaction_logs"
SOCKET_DIR="${REPO_ROOT}"
LOGFILE="${DATA_DIR}/postgres.log"
TEST_PORT=55447
TEST_DB=durable_compaction_test
ROLLBACK_DB=durable_compaction_rollback
PG_CONFIG_BIN="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG_BIN}" --bindir)"
PKGLIBDIR="$("${PG_CONFIG_BIN}" --pkglibdir)"
SHAREDIR="$("${PG_CONFIG_BIN}" --sharedir)"

log() { printf '[durable] %s\n' "$1"; }
error() { printf '[durable] ERROR: %s\n' "$1" >&2; exit 1; }

cleanup() {
    local exit_code=$?

    trap - EXIT INT TERM
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate \
            >/dev/null 2>&1 || true
    fi
    if [ "${exit_code}" -ne 0 ] && [ -d "${DATA_DIR}" ]; then
        rm -rf "${KEEP_DIR}"
        mkdir -p "${KEEP_DIR}"
        cp "${LOGFILE}" "${KEEP_DIR}/" 2>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit "${exit_code}"
}

trap cleanup EXIT INT TERM

sql_as() {
    local role=$1
    shift
    "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U "${role}" -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 "$@"
}

sql_super() {
    sql_as postgres "$@"
}

assert_eq() {
    local description=$1 expected=$2 actual=$3

    if [ "${actual}" != "${expected}" ]; then
        error "${description}: expected '${expected}', got '${actual}'"
    fi
    log "PASS: ${description}"
}

active_jobs_for_index() {
    local oid=$1
    sql_super -c "SELECT count(*)
      FROM df.instances
      WHERE label LIKE 'pg_textsearch:bg:v1:%:${oid}:%'
        AND status IN ('pending', 'running');"
}

wait_for_no_debt() {
    local index_name=$1 timeout=$2
    local waited=0
    while [ "$waited" -lt "$timeout" ]; do
        if [ "$(sql_super -c "SELECT NOT bm25_needs_compaction(
                '${index_name}'::regclass);")" = "t" ]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "index ${index_name} retained compaction debt"
}

wait_for_terminal() {
    local instance_id=$1 timeout=$2
    local waited=0 status=

    while [ "${waited}" -lt "${timeout}" ]; do
        status="$(sql_super -c \
            "SELECT status FROM df.instances WHERE id = '${instance_id}';")"
        case "${status}" in
            completed|failed|cancelled)
                return 0
                ;;
        esac
        sleep 1
        waited=$((waited + 1))
    done
    error "instance ${instance_id} did not become terminal"
}

wait_for_signal_node() {
    local instance_id=$1 timeout=$2
    local waited=0

    while [ "${waited}" -lt "${timeout}" ]; do
        if [ "$(sql_super -c "SELECT EXISTS (
                SELECT 1 FROM df.nodes
                WHERE instance_id = '${instance_id}'
                  AND node_type = 'SIGNAL'
                  AND status = 'running');")" = "t" ]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "instance ${instance_id} did not begin waiting for a signal"
}

wait_for_durable_worker() {
    local attempt id status

    for attempt in $(seq 1 60); do
        id="$(sql_as durable_owner -c \
            "SELECT df.start('SELECT 1', 'probe-${attempt}',
                             current_database(), 'caller');" \
            2>/dev/null || true)"
        if [ -n "${id}" ]; then
            for _ in $(seq 1 20); do
                status="$(sql_super -c \
                    "SELECT status FROM df.instances WHERE id = '${id}';" \
                    2>/dev/null || true)"
                if [ "${status}" = "completed" ]; then
                    return 0
                fi
                if [ "${status}" = "failed" ]; then
                    break
                fi
                sleep 1
            done
        fi
        sleep 1
    done
    error "pg_durable worker did not initialize for ${TEST_DB}"
}

initialize_database() {
    sql_super -c "CREATE EXTENSION pg_durable VERSION '0.2.7';"
    sql_super -c "CREATE EXTENSION pg_textsearch;"
    sql_super -c "GRANT CREATE ON SCHEMA public TO durable_owner;"
    sql_super -c "SELECT df.grant_usage('durable_owner');" >/dev/null
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
                   SET search_path = public, pg_catalog;"
    wait_for_durable_worker
}

dependency_count() {
    sql_super -c "SELECT count(*)
      FROM pg_catalog.pg_depend AS dep
      JOIN pg_catalog.pg_am AS am
        ON dep.classid = 'pg_catalog.pg_am'::regclass
       AND dep.objid = am.oid
      JOIN pg_catalog.pg_extension AS ext
        ON dep.refclassid = 'pg_catalog.pg_extension'::regclass
       AND dep.refobjid = ext.oid
      WHERE am.amname = 'bm25'
        AND ext.extname = 'pg_durable'
        AND dep.deptype = 'n';"
}

active_job_id() {
    local index_oid=$1

    sql_super -c "SELECT id
      FROM df.instances
      WHERE label LIKE 'pg_textsearch:bg:v1:%:${index_oid}:%'
        AND status IN ('pending', 'running')
      ORDER BY created_at DESC, id DESC
      LIMIT 1;"
}

create_compaction_debt() {
    sql_as durable_owner <<'SQL' >/dev/null
BEGIN;
DO $body$
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO documents (id, body)
        SELECT 10000 + n * 100 + i,
               format('signal round %s document %s filler', n, i)
        FROM generate_series(1, 20) AS i;
        PERFORM bm25_spill_index('documents_idx');
    END LOOP;
END
$body$;
COMMIT;
SQL
}

setup_cluster() {
    for required in \
        "${PKGLIBDIR}/pg_durable.so" \
        "${PKGLIBDIR}/pg_textsearch.so" \
        "${SHAREDIR}/extension/pg_durable--0.2.7.sql"; do
        [ -f "${required}" ] || error "required install artifact missing: ${required}"
    done

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
    "${PGBINDIR}/initdb" -D "${DATA_DIR}" -U postgres \
        --auth-local=trust --auth-host=reject >/dev/null
    {
        printf "port = %s\n" "${TEST_PORT}"
        printf "unix_socket_directories = '%s'\n" "${SOCKET_DIR}"
        printf "listen_addresses = ''\n"
        if [ -n "${PG_DURABLE_LIBDIR:-}" ]; then
            printf "dynamic_library_path = '%s:\$libdir'\n" \
                "${PG_DURABLE_LIBDIR}"
        fi
        printf "shared_preload_libraries = 'pg_durable,pg_textsearch'\n"
        printf "logging_collector = on\n"
        printf "log_directory = '.'\n"
        printf "log_filename = 'postgres.log'\n"
        printf "pg_durable.database = '%s'\n" "${TEST_DB}"
        printf "pg_durable.worker_role = 'postgres'\n"
        printf "pg_durable.max_user_connections = 4\n"
        printf "pg_textsearch.segments_per_level = 2\n"
    } >>"${DATA_DIR}/postgresql.conf"

    PGHOST="${SOCKET_DIR}" "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w >/dev/null
    "${PGBINDIR}/createdb" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres "${TEST_DB}"
    sql_super -c "CREATE ROLE durable_owner LOGIN;"
    sql_super -c "CREATE ROLE durable_nologin NOLOGIN;"
}

test_create_activation() {
    local index_oid create_output canonical_label

    sql_super -c "CREATE TABLE documents (id integer, body text);
                   ALTER TABLE documents OWNER TO durable_owner;"
    sql_as durable_owner -c "INSERT INTO documents
        SELECT i, 'document ' || i || ' filler filler filler'
        FROM generate_series(1, 10000) AS i;"

    create_output="$(sql_as durable_owner -c "
        SET client_min_messages = warning;
        SET maintenance_work_mem = '1MB';
        CREATE INDEX documents_idx ON documents USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" 2>&1)"
    if ! grep -Fq \
        "pg_textsearch background compaction is a preview feature" \
        <<<"${create_output}"; then
        error "successful activation did not emit the preview warning"
    fi

    index_oid="$(sql_super -c \
        "SELECT 'documents_idx'::regclass::oid;")"
    assert_eq "CREATE admits one active owner job" "1" \
        "$(active_jobs_for_index "${index_oid}")"
    canonical_label="$(sql_super -c "SELECT pg_catalog.format(
        'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%s',
        database.oid,
        relation.oid,
        coalesce(
            nullif(relation.reltablespace, 0),
            database.dattablespace),
        pg_catalog.pg_relation_filenode(relation.oid),
        relation.relowner,
        pg_catalog.encode(pg_catalog.convert_to(
            pg_catalog.current_setting(
                'pg_textsearch.background_compaction_schedule'),
            'UTF8'), 'hex'))
      FROM pg_catalog.pg_class AS relation
      JOIN pg_catalog.pg_database AS database
        ON database.datname = pg_catalog.current_database()
      WHERE relation.oid = 'documents_idx'::regclass;")"
    assert_eq "label captures physical identity and full schedule" \
        "${canonical_label}" \
        "$(sql_super -c "SELECT label FROM df.instances
                          WHERE id = '$(active_job_id "${index_oid}")';")"
    assert_eq "workflow is submitted as the index owner" "durable_owner" \
        "$(sql_super -c "SELECT submitted_by::pg_catalog.text
                          FROM df.instances
                          WHERE id = '$(active_job_id "${index_oid}")';")"
    wait_for_no_debt documents_idx 60
    assert_eq "job remains active after the initial cascade" "1" \
        "$(active_jobs_for_index "${index_oid}")"
    assert_eq "activation pins pg_durable with a normal dependency" "1" \
        "$(dependency_count)"

    local first_instance recovered_instance
    first_instance="$(active_job_id "${index_oid}")"
    wait_for_signal_node "${first_instance}" 30
    create_compaction_debt
    wait_for_no_debt documents_idx 30
    assert_eq "spill signaling reuses the owner workflow" \
        "${first_instance}" "$(active_job_id "${index_oid}")"

    sql_as durable_owner -c \
        "SELECT df.cancel('${first_instance}', 'test recovery');" >/dev/null
    wait_for_terminal "${first_instance}" 30
    create_compaction_debt
    wait_for_no_debt documents_idx 30
    recovered_instance="$(active_job_id "${index_oid}")"
    if [ -z "${recovered_instance}" ] ||
        [ "${recovered_instance}" = "${first_instance}" ]; then
        error "spill did not recover the terminal managed generation"
    fi
    assert_eq "recovery admits one replacement owner workflow" "1" \
        "$(active_jobs_for_index "${index_oid}")"
}

test_rollback_in_fresh_database() {
    local nologin_error

    sql_super -c "CREATE DATABASE ${ROLLBACK_DB};"
    sql_super -c "ALTER SYSTEM SET pg_durable.database = '${ROLLBACK_DB}';"
    PGHOST="${SOCKET_DIR}" "${PGBINDIR}/pg_ctl" restart -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w >/dev/null
    TEST_DB="${ROLLBACK_DB}"

    initialize_database
    sql_super -c "GRANT CREATE ON SCHEMA public TO durable_nologin;
                   CREATE TABLE nologin_documents (id integer, body text);
                   ALTER TABLE nologin_documents OWNER TO durable_nologin;"
    if nologin_error="$(sql_super -c "
        SET ROLE durable_nologin;
        CREATE INDEX nologin_documents_idx ON nologin_documents
          USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "background CREATE accepted a NOLOGIN index owner"
    fi
    if ! grep -Fq \
        "index owner must have LOGIN for background compaction" \
        <<<"${nologin_error}"; then
        error "NOLOGIN rejection did not use the stable admission message"
    fi
    log "PASS: background admission rejects a NOLOGIN index owner"
    assert_eq "failed NOLOGIN admission leaves no sticky dependency" "0" \
        "$(dependency_count)"

    sql_super -c "CREATE TABLE rollback_documents (id integer, body text);
                   ALTER TABLE rollback_documents OWNER TO durable_owner;"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
BEGIN;
CREATE INDEX rollback_documents_idx ON rollback_documents USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
ROLLBACK;
SQL

    assert_eq "rolled-back CREATE leaves no managed job" "0" \
        "$(sql_super -c "SELECT count(*) FROM df.instances
                          WHERE label LIKE 'pg_textsearch:bg:v1:%';")"
    assert_eq "rolled-back CREATE leaves no sticky dependency" "0" \
        "$(dependency_count)"
    assert_eq "rolled-back CREATE leaves no private-helper grant" "f" \
        "$(sql_super -c "SELECT
            pg_catalog.has_function_privilege(
                'durable_owner',
                'bm25_compact_step_if_current(oid,oid,oid,oid,oid)',
                'EXECUTE')
            OR pg_catalog.has_function_privilege(
                'durable_owner',
                'bm25_background_target_is_current(oid,oid,oid,oid,oid)',
                'EXECUTE');")"
}

test_sticky_dependency() {
    if sql_super -c "DROP EXTENSION pg_durable;" >/dev/null 2>&1; then
        error "sticky dependency allowed DROP EXTENSION pg_durable"
    fi
    log "PASS: sticky dependency blocks DROP EXTENSION pg_durable"

    sql_super -c "DROP EXTENSION pg_durable CASCADE;" >/dev/null
    assert_eq "CASCADE follows the access-method dependency to pg_textsearch" \
        "0" "$(sql_super -c "SELECT count(*)
                              FROM pg_catalog.pg_extension
                              WHERE extname = 'pg_textsearch';")"
}

setup_cluster
initialize_database
test_create_activation
test_sticky_dependency
test_rollback_in_fresh_database
log "Managed pg_durable CREATE activation tests passed"
