#!/bin/bash
#
# End-to-end permission and lifecycle tests for owner-scoped workflows.
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
PG_DURABLE_VERSION="${PG_DURABLE_VERSION:-0.2.8}"
PG_DURABLE_PACKAGE_DIR="${PG_DURABLE_PACKAGE_DIR:-}"
PACKAGE_BACKUP_DIR="${REPO_ROOT}/test/tmp_durable_package_backup"
DURABLE_PACKAGE_LIBDIR=
STAGED_SHARE_FILES=()

log() { printf '[durable] %s\n' "$1"; }
error() { printf '[durable] ERROR: %s\n' "$1" >&2; exit 1; }

restore_durable_package() {
    local staged source

    for staged in "${STAGED_SHARE_FILES[@]}"; do
        rm -f "${staged}"
    done
    if [ -d "${PACKAGE_BACKUP_DIR}" ]; then
        while IFS= read -r source; do
            cp -p "${source}" "${SHAREDIR}/extension/"
        done < <(find "${PACKAGE_BACKUP_DIR}" -maxdepth 1 -type f -print)
        rm -rf "${PACKAGE_BACKUP_DIR}"
    fi
}

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
    restore_durable_package
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

reindex_database() {
    local command=$1 output

    for _ in $(seq 1 5); do
        if output="$(sql_super -c "${command}" 2>&1)"; then
            printf '%s' "${output}"
            return 0
        fi
        if ! grep -Fq "deadlock detected" <<<"${output}"; then
            printf '%s\n' "${output}" >&2
            return 1
        fi
        sleep 0.2
    done

    printf '%s\n' "${output}" >&2
    return 1
}

assert_eq() {
    local description=$1 expected=$2 actual=$3

    if [ "${actual}" != "${expected}" ]; then
        error "${description}: expected '${expected}', got '${actual}'"
    fi
    log "PASS: ${description}"
}

assert_ne() {
    local description=$1 unexpected=$2 actual=$3

    if [ "${actual}" = "${unexpected}" ]; then
        error "${description}: did not expect '${unexpected}'"
    fi
    log "PASS: ${description}"
}

helper_privileges_for_role() {
    local role=$1

    sql_super -c "SELECT pg_catalog.concat_ws(
        ':',
        pg_catalog.has_function_privilege(
            '${role}',
            'bm25_compact_step_if_current(oid,oid,oid,oid,oid)',
            'EXECUTE'),
        pg_catalog.has_function_privilege(
            '${role}',
            'bm25_background_target_is_current(oid,oid,oid,oid,oid)',
            'EXECUTE'));"
}

assert_rejected_background_alter() {
    local description=$1 owner=$2 executor=$3 prefix=$4 expected_error=$5
    local alter_error dependencies_before jobs_before privileges_before
    local table_name="${prefix}_docs"
    local index_name="${prefix}_idx"

    sql_super -c "CREATE TABLE ${table_name} (body text);
                   ALTER TABLE ${table_name} OWNER TO ${owner};
                   CREATE INDEX ${index_name}
                     ON ${table_name} USING bm25(body)
                     WITH (text_config = 'english',
                           compaction = 'manual');"
    assert_eq "${description} test index has the expected owner" \
        "${owner}" \
        "$(sql_super -c "SELECT pg_catalog.pg_get_userbyid(relowner)
                          FROM pg_catalog.pg_class
                          WHERE oid = '${index_name}'::regclass;")"

    jobs_before="$(managed_job_count)"
    dependencies_before="$(dependency_count)"
    privileges_before="$(helper_privileges_for_role "${owner}")"
    if [ "${executor}" = "set-role" ]; then
        if alter_error="$(sql_super -c "SET ROLE ${owner};
            ALTER INDEX public.${index_name}
              SET (compaction = 'background');" 2>&1)"; then
            error "${description} ALTER unexpectedly succeeded"
        fi
    elif alter_error="$(sql_as "${executor}" -c "
        ALTER INDEX public.${index_name}
          SET (compaction = 'background');" 2>&1)"; then
        error "${description} ALTER unexpectedly succeeded"
    fi
    if ! grep -Fq "${expected_error}" <<<"${alter_error}"; then
        error "${description} ALTER did not fail admission: ${alter_error}"
    fi

    assert_eq "${description} ALTER preserves manual reloption" "t" \
        "$(sql_super -c "SELECT reloptions @> ARRAY['compaction=manual']
                          FROM pg_catalog.pg_class
                          WHERE oid = '${index_name}'::regclass;")"
    assert_eq "${description} ALTER creates no managed job" \
        "${jobs_before}" "$(managed_job_count)"
    assert_eq "${description} ALTER leaves dependencies unchanged" \
        "${dependencies_before}" "$(dependency_count)"
    assert_eq "${description} ALTER leaves helper privileges unchanged" \
        "${privileges_before}" "$(helper_privileges_for_role "${owner}")"

    sql_super -c "DROP TABLE ${table_name};"
}

test_alter_preflight_rejections() {
    assert_rejected_background_alter \
        "NOLOGIN owner" durable_nologin set-role alter_nologin \
        "index owner must have LOGIN for background compaction"
    assert_rejected_background_alter \
        "disallowed superuser owner" postgres postgres alter_superuser \
        "pg_durable superuser instances are disabled"
    assert_rejected_background_alter \
        "owner without pg_durable table privileges" durable_usage_only \
        durable_usage_only alter_usage_only \
        "index owner lacks required pg_durable privileges"
    assert_rejected_background_alter \
        "owner without database CONNECT" durable_no_connect durable_actor \
        alter_no_connect \
        "index owner cannot connect for background compaction"
    assert_rejected_background_alter \
        "owner without pg_textsearch schema USAGE" \
        durable_no_textsearch_schema durable_writer \
        alter_no_textsearch_schema \
        "index owner lacks required pg_textsearch schema privilege"
}

test_cic_owner_privilege_preflight() {
    local create_error jobs_before schema_error

    schema_error='Role "durable_no_textsearch_schema" lacks USAGE privilege'
    schema_error+=' on schema public.'

    sql_super -c "REVOKE CONNECT ON DATABASE durable_compaction_test
                   FROM PUBLIC;
                   GRANT CONNECT ON DATABASE durable_compaction_test
                   TO postgres, durable_owner, durable_owner_two,
                      durable_usage_only, durable_read_only, durable_bypass,
                      durable_actor, durable_writer, durable_maintainer;"

    jobs_before="$(managed_job_count)"
    sql_super -c "CREATE TABLE no_connect_docs (body text);
                   ALTER TABLE no_connect_docs OWNER TO durable_no_connect;"
    if create_error="$(sql_as durable_actor -c "
        CREATE INDEX CONCURRENTLY no_connect_docs_idx
          ON no_connect_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "background CIC accepted an owner without database CONNECT"
    fi
    assert_eq "rejected no-CONNECT CIC leaves no relation" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.to_regclass('no_connect_docs_idx') IS NULL;")"
    if ! grep -Fq \
        "index owner cannot connect for background compaction" \
        <<<"${create_error}"; then
        error "no-CONNECT owner did not fail deterministic CIC preflight: \
${create_error}"
    fi
    assert_eq "no-CONNECT CIC creates no probe workflow" "${jobs_before}" \
        "$(managed_job_count)"
    sql_super -c "DROP TABLE no_connect_docs;"

    sql_super -c "GRANT CONNECT ON DATABASE durable_compaction_test
                   TO durable_no_textsearch_schema;
                   REVOKE USAGE ON SCHEMA public FROM PUBLIC;
                   GRANT USAGE ON SCHEMA public
                   TO postgres, durable_owner, durable_owner_two,
                      durable_nologin, durable_usage_only, durable_read_only,
                      durable_bypass, durable_no_connect, durable_actor,
                      durable_writer, durable_maintainer;"

    jobs_before="$(managed_job_count)"
    sql_super -c "CREATE TABLE no_textsearch_schema_docs (body text);
                   ALTER TABLE no_textsearch_schema_docs
                     OWNER TO durable_no_textsearch_schema;"
    if create_error="$(sql_as durable_writer -c "
        CREATE INDEX CONCURRENTLY no_textsearch_schema_docs_idx
          ON no_textsearch_schema_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "background CIC accepted an owner without pg_textsearch \
schema USAGE"
    fi
    assert_eq "rejected no-schema-USAGE CIC leaves no relation" "t" \
        "$(sql_super -c "SELECT pg_catalog.to_regclass(
            'no_textsearch_schema_docs_idx') IS NULL;")"
    if ! grep -Fq \
        "index owner lacks required pg_textsearch schema privilege" \
        <<<"${create_error}"; then
        error "no-schema-USAGE owner did not fail stable privilege check: \
${create_error}"
    fi
    if ! grep -Fq "${schema_error}" <<<"${create_error}"; then
        error "no-schema-USAGE owner error did not identify public: \
${create_error}"
    fi
    if ! grep -Fq \
        'Grant access with GRANT USAGE ON SCHEMA public TO' \
        <<<"${create_error}"; then
        error "no-schema-USAGE owner error did not provide an actionable \
hint: ${create_error}"
    fi
    assert_eq "no-schema-USAGE CIC creates no probe workflow" \
        "${jobs_before}" "$(managed_job_count)"
    assert_eq "no-schema-USAGE CIC leaves no sticky dependency" "0" \
        "$(dependency_count)"
    assert_eq "no-schema-USAGE CIC leaves no private-helper grant" "f" \
        "$(sql_super -c "SELECT
            pg_catalog.has_function_privilege(
                'durable_no_textsearch_schema',
                'bm25_compact_step_if_current(oid,oid,oid,oid,oid)',
                'EXECUTE')
            OR pg_catalog.has_function_privilege(
                'durable_no_textsearch_schema',
                'bm25_background_target_is_current(oid,oid,oid,oid,oid)',
                'EXECUTE');")"
    sql_super -c "DROP TABLE no_textsearch_schema_docs;"

    assert_eq "usage-only owner can resolve df.start" "t" \
        "$(sql_super -c "SELECT pg_catalog.has_function_privilege(
            'durable_usage_only', procedure.oid, 'EXECUTE')
          FROM pg_catalog.pg_proc AS procedure
          JOIN pg_catalog.pg_namespace AS namespace
            ON namespace.oid = procedure.pronamespace
          JOIN pg_catalog.pg_depend AS dependency
            ON dependency.classid = 'pg_catalog.pg_proc'::regclass
           AND dependency.objid = procedure.oid
           AND dependency.deptype = 'e'
          JOIN pg_catalog.pg_extension AS extension
            ON extension.oid = dependency.refobjid
          WHERE namespace.nspname = 'df'
            AND procedure.proname = 'start'
            AND extension.extname = 'pg_durable';")"
    assert_eq "usage-only owner can resolve df.explain" "t" \
        "$(sql_super -c "SELECT pg_catalog.has_function_privilege(
            'durable_usage_only', 'df.explain(text)', 'EXECUTE');")"
    assert_eq "usage-only owner cannot read df.instances" "f" \
        "$(sql_super -c "SELECT pg_catalog.has_table_privilege(
            'durable_usage_only', 'df.instances', 'SELECT');")"

    jobs_before="$(managed_job_count)"
    sql_super -c "CREATE TABLE usage_only_docs (body text);
                   ALTER TABLE usage_only_docs OWNER TO durable_usage_only;"
    if create_error="$(sql_as durable_usage_only -c "
        CREATE INDEX CONCURRENTLY usage_only_docs_idx
          ON usage_only_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "background CIC accepted an owner without durable table access"
    fi
    assert_eq "rejected usage-only CIC leaves no relation" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.to_regclass('usage_only_docs_idx') IS NULL;")"
    if ! grep -Fq "index owner lacks required pg_durable privileges" \
        <<<"${create_error}"; then
        error "usage-only owner did not fail deterministic CIC preflight: \
${create_error}"
    fi
    assert_eq "usage-only CIC creates no probe workflow" "${jobs_before}" \
        "$(managed_job_count)"
    sql_super -c "DROP TABLE usage_only_docs;"

    assert_eq "read-only owner cannot insert df.instances" "f" \
        "$(sql_super -c "SELECT pg_catalog.has_table_privilege(
            'durable_read_only', 'df.instances', 'INSERT');")"
    jobs_before="$(managed_job_count)"
    sql_super -c "CREATE TABLE read_only_docs (body text);
                   ALTER TABLE read_only_docs OWNER TO durable_read_only;"
    if create_error="$(sql_as durable_read_only -c "
        CREATE INDEX CONCURRENTLY read_only_docs_idx
          ON read_only_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "background CIC accepted an owner without durable INSERT"
    fi
    assert_eq "rejected read-only CIC leaves no relation" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.to_regclass('read_only_docs_idx') IS NULL;")"
    if ! grep -Fq "index owner lacks required pg_durable privileges" \
        <<<"${create_error}"; then
        error "read-only owner did not fail deterministic CIC preflight: \
${create_error}"
    fi
    assert_eq "read-only CIC creates no probe workflow" "${jobs_before}" \
        "$(managed_job_count)"
    sql_super -c "DROP TABLE read_only_docs;"
}

test_defaulted_start_arity() {
    local create_output index_oid instance_id start_shape
    local shimmed=false

    start_shape="$(sql_super -c "SELECT
        procedure.pronargs || ':' || procedure.pronargdefaults
      FROM pg_catalog.pg_proc AS procedure
      JOIN pg_catalog.pg_namespace AS namespace
        ON namespace.oid = procedure.pronamespace
      JOIN pg_catalog.pg_depend AS dependency
        ON dependency.classid = 'pg_catalog.pg_proc'::regclass
       AND dependency.objid = procedure.oid
       AND dependency.deptype = 'e'
      JOIN pg_catalog.pg_extension AS extension
        ON extension.oid = dependency.refobjid
      WHERE namespace.nspname = 'df'
        AND procedure.proname = 'start'
        AND extension.extname = 'pg_durable';")"

    case "${start_shape}" in
        4:*)
            sql_super <<'SQL' >/dev/null
ALTER FUNCTION df.start(text, text, text, text) RENAME TO start_v027;
CREATE FUNCTION df.start(
    fut text,
    label text,
    database text,
    transaction_mode text,
    max_attempts integer DEFAULT 1,
    max_backoff interval DEFAULT interval '16 seconds',
    on_failure text DEFAULT 'fail')
RETURNS text
LANGUAGE sql
SET search_path = pg_catalog, pg_temp
AS $body$
    SELECT df.start_v027(fut, label, database, transaction_mode)
$body$;
ALTER EXTENSION pg_durable ADD FUNCTION
    df.start(text, text, text, text, integer, interval, text);
SQL
            shimmed=true
            ;;
        7:*)
            ;;
        *)
            error "unexpected extension-owned df.start shape: ${start_shape}"
            ;;
    esac

    assert_eq "defaulted start probe is callable with four arguments" \
        "7:true" \
        "$(sql_super -c "SELECT pronargs || ':' ||
                                (pronargdefaults >= pronargs - 4)
          FROM pg_catalog.pg_proc
          WHERE oid = 'df.start(text,text,text,text,integer,interval,text)'
                      ::pg_catalog.regprocedure;")"

    sql_super -c "CREATE TABLE arity_docs (body text);
                   ALTER TABLE arity_docs OWNER TO durable_owner;"
    if ! create_output="$(sql_as durable_owner -c "
        CREATE INDEX CONCURRENTLY arity_docs_idx
          ON arity_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "four-argument call rejected defaulted df.start: \
${create_output}"
    fi
    index_oid="$(sql_super -c "SELECT 'arity_docs_idx'::regclass::oid;")"
    instance_id="$(active_job_id_for_owner \
        "${index_oid}" durable_owner)"
    [ -n "${instance_id}" ] ||
        error "defaulted df.start probe created no owner workflow"
    sql_as durable_owner -c \
        "SELECT df.cancel('${instance_id}', 'arity probe complete');" \
        >/dev/null
    wait_for_terminal "${instance_id}" 30
    sql_super -c "DROP TABLE arity_docs;"
    if "${shimmed}"; then
        sql_super -c "ALTER EXTENSION pg_durable DROP FUNCTION
                     df.start(text, text, text, text, integer, interval, text);
                   DROP FUNCTION
                     df.start(text, text, text, text, integer, interval, text);
                   ALTER FUNCTION df.start_v027(text, text, text, text)
                     RENAME TO start;"
    fi
    log "PASS: four-argument adapter accepts trailing defaulted arguments"
}

stage_durable_package() {
    local package_library package_control package_sql source destination
    local -a package_libraries package_controls package_sql_files

    if [ -z "${PG_DURABLE_PACKAGE_DIR}" ]; then
        error "PG_DURABLE_PACKAGE_DIR must name one pg_durable package"
    fi

    mapfile -t package_libraries < <(
        find "${PG_DURABLE_PACKAGE_DIR}" -type f -name pg_durable.so -print
    )
    mapfile -t package_controls < <(
        find "${PG_DURABLE_PACKAGE_DIR}" -type f \
            -name pg_durable.control -print
    )
    mapfile -t package_sql_files < <(
        find "${PG_DURABLE_PACKAGE_DIR}" -type f \
            -name "pg_durable--${PG_DURABLE_VERSION}.sql" -print
    )

    [ "${#package_libraries[@]}" -eq 1 ] ||
        error "package must contain exactly one pg_durable.so"
    [ "${#package_controls[@]}" -eq 1 ] ||
        error "package must contain exactly one pg_durable.control"
    [ "${#package_sql_files[@]}" -eq 1 ] ||
        error "package must contain exactly one pg_durable version SQL file"

    package_library="${package_libraries[0]}"
    package_control="${package_controls[0]}"
    package_sql="${package_sql_files[0]}"
    if ! grep -Eq \
        "^default_version[[:space:]]*=[[:space:]]*'${PG_DURABLE_VERSION}'" \
        "${package_control}"; then
        error "pg_durable package control/version SQL do not match"
    fi
    DURABLE_PACKAGE_LIBDIR="$(dirname "${package_library}")"

    rm -rf "${PACKAGE_BACKUP_DIR}"
    mkdir -p "${PACKAGE_BACKUP_DIR}"
    for source in "${package_control}" "${package_sql}"; do
        destination="${SHAREDIR}/extension/$(basename "${source}")"
        if [ -f "${destination}" ]; then
            cp -p "${destination}" "${PACKAGE_BACKUP_DIR}/"
        fi
        cp -p "${source}" "${destination}"
        STAGED_SHARE_FILES+=("${destination}")
        cmp -s "${source}" "${destination}" ||
            error "failed to stage matching pg_durable package SQL"
    done

    log "Using pg_durable ${PG_DURABLE_VERSION} package library: \
$(sha256sum "${package_library}" | cut -d' ' -f1)"
    log "Using pg_durable ${PG_DURABLE_VERSION} package SQL: \
$(sha256sum "${package_sql}" | cut -d' ' -f1)"
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

quiesce_durable_worker() {
    local instance_id

    while IFS= read -r instance_id; do
        [ -n "${instance_id}" ] || continue
        sql_super -c "SELECT df.cancel(
            id, 'extension dependency test teardown')
          FROM df.instances
          WHERE id = '${instance_id}'
            AND status IN ('pending', 'running');" >/dev/null
        wait_for_terminal "${instance_id}" 30
    done < <(sql_super -c "SELECT id
      FROM df.instances
      WHERE status IN ('pending', 'running')
      ORDER BY id;")

    assert_eq "extension dependency teardown has no active workflows" "0" \
        "$(sql_super -c "SELECT count(*)
          FROM df.instances
          WHERE status IN ('pending', 'running');")"

    PGHOST="${SOCKET_DIR}" "${PGBINDIR}/pg_ctl" restart -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w \
        -o "-c shared_preload_libraries=pg_textsearch" >/dev/null
    assert_eq "extension dependency teardown stops the durable worker" \
        "pg_textsearch" \
        "$(sql_super -c "SHOW shared_preload_libraries;")"
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

wait_for_log_message() {
    local message=$1 timeout=$2
    local waited=0

    while [ "${waited}" -lt "${timeout}" ]; do
        if grep -Fq "${message}" "${LOGFILE}"; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "server log did not contain: ${message}"
}

wait_for_audit_rows() {
    local expected=$1 timeout=$2
    local waited=0 count

    while [ "${waited}" -lt "${timeout}" ]; do
        count="$(sql_super -c \
            "SELECT count(*) FROM public.worker_identity_audit;")"
        if [ "${count}" -ge "${expected}" ]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "worker identity audit did not reach ${expected} rows"
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
    sql_super -c \
        "CREATE EXTENSION pg_durable VERSION '${PG_DURABLE_VERSION}';"
    assert_eq "loaded pg_durable package version" "${PG_DURABLE_VERSION}" \
        "$(sql_super -c "SELECT extversion
                          FROM pg_catalog.pg_extension
                          WHERE extname = 'pg_durable';")"
    assert_eq "df.instances submitted_by type" "regrole" \
        "$(sql_super -c "SELECT pg_catalog.format_type(
                              attribute.atttypid, attribute.atttypmod)
                          FROM pg_catalog.pg_attribute AS attribute
                          WHERE attribute.attrelid = 'df.instances'::regclass
                            AND attribute.attname = 'submitted_by'
                            AND NOT attribute.attisdropped;")"
    sql_super -c "CREATE EXTENSION pg_textsearch;"
    sql_super -c "GRANT CREATE ON SCHEMA public
                   TO durable_owner, durable_owner_two, durable_usage_only,
                      durable_read_only, durable_bypass, durable_actor,
                      durable_writer, durable_maintainer;"
    sql_super -c "GRANT pg_maintain TO durable_maintainer;"
    sql_super -c "SELECT df.grant_usage('durable_owner');" >/dev/null
    sql_super -c "SELECT df.grant_usage('durable_owner_two');" >/dev/null
    sql_super -c "SELECT df.grant_usage('durable_bypass');" >/dev/null
    sql_super -c "SELECT df.grant_usage('durable_no_connect');" >/dev/null
    sql_super -c \
        "SELECT df.grant_usage('durable_no_textsearch_schema');" >/dev/null
    sql_super -c "GRANT USAGE ON SCHEMA df
                   TO durable_usage_only, durable_read_only;
                   GRANT SELECT ON df.instances, df.vars
                   TO durable_read_only;"
    sql_super -c "CREATE SCHEMA owner_shadow AUTHORIZATION durable_owner;"
    sql_as durable_owner <<'SQL'
CREATE FUNCTION owner_shadow.reject_text_equality(left_value text,
                                                   right_value text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE
AS $body$
BEGIN
    IF pg_catalog.left(left_value, 20)
           OPERATOR(pg_catalog.=) 'pg_textsearch:bg:v1:'
       OR left_value OPERATOR(pg_catalog.=)
           ANY (ARRAY['pending', 'running', 'completed', 'failed',
                      'cancelled']::pg_catalog.text[]) THEN
        RAISE EXCEPTION 'owner search_path resolved shadow text equality';
    END IF;
    RETURN left_value OPERATOR(pg_catalog.=) right_value;
END
$body$;

CREATE FUNCTION owner_shadow.reject_text_like(text, text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE
AS $body$
BEGIN
    RAISE EXCEPTION 'owner search_path resolved shadow text LIKE';
END
$body$;

CREATE OPERATOR owner_shadow.= (
    LEFTARG = pg_catalog.text,
    RIGHTARG = pg_catalog.text,
    FUNCTION = owner_shadow.reject_text_equality
);

CREATE OPERATOR owner_shadow.~~ (
    LEFTARG = pg_catalog.text,
    RIGHTARG = pg_catalog.text,
    FUNCTION = owner_shadow.reject_text_like
);
SQL
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
                   SET search_path = owner_shadow, public, pg_catalog;
                   ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
                   SET maintenance_work_mem = '1MB';"
    wait_for_durable_worker
    assert_eq "hostile owner search_path is active" \
        "owner_shadow, public, pg_catalog" \
        "$(sql_as durable_owner -c "SHOW search_path;")"
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

latest_job_id() {
    local index_oid=$1

    sql_super -c "SELECT id
      FROM df.instances
      WHERE label LIKE 'pg_textsearch:bg:v1:%:${index_oid}:%'
      ORDER BY created_at DESC, id DESC
      LIMIT 1;"
}

active_job_id_for_owner() {
    local index_oid=$1 owner=$2

    sql_super -c "SELECT id
      FROM df.instances
      WHERE label LIKE 'pg_textsearch:bg:v1:%:${index_oid}:%'
        AND submitted_by = '${owner}'::regrole
        AND status IN ('pending', 'running')
      ORDER BY created_at DESC, id DESC
      LIMIT 1;"
}

current_generation_job_id() {
    local index_oid=$1

    sql_super -c "SELECT instance.id
      FROM df.instances AS instance
      JOIN pg_catalog.pg_class AS relation
        ON relation.oid = ${index_oid}
      JOIN pg_catalog.pg_database AS database
        ON database.datname = pg_catalog.current_database()
      WHERE instance.label LIKE pg_catalog.format(
                'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%%',
                database.oid,
                relation.oid,
                coalesce(
                    nullif(relation.reltablespace, 0),
                    database.dattablespace),
                pg_catalog.pg_relation_filenode(relation.oid),
                relation.relowner)
        AND instance.submitted_by::pg_catalog.oid = relation.relowner
        AND instance.status IN ('pending', 'running')
      ORDER BY instance.created_at DESC, instance.id DESC
      LIMIT 1;"
}

current_generation_job_count() {
    local index_oid=$1

    sql_super -c "SELECT count(*)
      FROM df.instances AS instance
      JOIN pg_catalog.pg_class AS relation
        ON relation.oid = ${index_oid}
      JOIN pg_catalog.pg_database AS database
        ON database.datname = pg_catalog.current_database()
      WHERE instance.label LIKE pg_catalog.format(
                'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%%',
                database.oid,
                relation.oid,
                coalesce(
                    nullif(relation.reltablespace, 0),
                    database.dattablespace),
                pg_catalog.pg_relation_filenode(relation.oid),
                relation.relowner)
        AND instance.submitted_by::pg_catalog.oid = relation.relowner
        AND instance.status IN ('pending', 'running');"
}

current_generation_schedule_job_count() {
    local index_oid=$1 schedule=$2

    sql_super -c "SELECT count(*)
      FROM df.instances AS instance
      JOIN pg_catalog.pg_class AS relation
        ON relation.oid = ${index_oid}
      JOIN pg_catalog.pg_database AS database
        ON database.datname = pg_catalog.current_database()
      WHERE instance.label LIKE pg_catalog.format(
                'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%%',
                database.oid,
                relation.oid,
                coalesce(nullif(relation.reltablespace, 0),
                         database.dattablespace),
                pg_catalog.pg_relation_filenode(relation.oid),
                relation.relowner)
        AND instance.label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '${schedule}', 'UTF8'), 'hex'))
        AND instance.submitted_by::pg_catalog.oid = relation.relowner
        AND instance.status IN ('pending', 'running');"
}

background_target_is_current() {
    local index_oid=$1 relfilenumber=$2 owner=$3

    sql_as "${owner}" -c "SELECT
          bm25_background_target_is_current(
            relation.oid,
            database.oid,
            coalesce(nullif(relation.reltablespace, 0),
                     database.dattablespace),
            ${relfilenumber}::pg_catalog.oid,
            '${owner}'::pg_catalog.regrole::pg_catalog.oid)
        FROM pg_catalog.pg_class AS relation
        JOIN pg_catalog.pg_database AS database
          ON database.datname = pg_catalog.current_database()
        WHERE relation.oid = ${index_oid};"
}

managed_job_count() {
    sql_super -c "SELECT count(*) FROM df.instances
      WHERE label LIKE 'pg_textsearch:bg:v1:%';"
}

index_lineage() {
    local index_name=$1

    sql_super -c "SELECT pg_catalog.substr(
        option, pg_catalog.length('compaction_lineage=') + 1)
      FROM pg_catalog.pg_class AS relation,
           LATERAL pg_catalog.unnest(relation.reloptions) AS option
      WHERE relation.oid = '${index_name}'::regclass
        AND option OPERATOR(pg_catalog.~~) 'compaction_lineage=%';"
}

remove_index_lineage() {
    local index_name=$1

    sql_super -c "UPDATE pg_catalog.pg_class
      SET reloptions = ARRAY(
        SELECT option
        FROM pg_catalog.unnest(reloptions) AS option
        WHERE option OPERATOR(pg_catalog.!~~) 'compaction_lineage=%')
      WHERE oid = '${index_name}'::regclass;"
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

create_bypass_compaction_debt() {
    sql_as durable_bypass <<'SQL' >/dev/null
BEGIN;
DO $body$
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO bypass_documents (id, body)
        SELECT 10000 + n * 100 + i,
               format('bypass round %s document %s filler', n, i)
        FROM generate_series(1, 20) AS i;
        PERFORM bm25_spill_index('bypass_documents_idx');
    END LOOP;
END
$body$;
COMMIT;
SQL
}

setup_cluster() {
    for required in \
        "${PKGLIBDIR}/pg_textsearch.so" \
        "${DURABLE_PACKAGE_LIBDIR}/pg_durable.so" \
        "${SHAREDIR}/extension/pg_durable--${PG_DURABLE_VERSION}.sql"; do
        [ -f "${required}" ] ||
            error "required install artifact missing: ${required}"
    done

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
    "${PGBINDIR}/initdb" -D "${DATA_DIR}" -U postgres \
        --auth-local=trust --auth-host=reject >/dev/null
    {
        printf "port = %s\n" "${TEST_PORT}"
        printf "unix_socket_directories = '%s'\n" "${SOCKET_DIR}"
        printf "listen_addresses = ''\n"
        printf "dynamic_library_path = '%s:\$libdir'\n" \
            "${DURABLE_PACKAGE_LIBDIR}"
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
    sql_super -c "CREATE ROLE durable_owner_two LOGIN;"
    sql_super -c "CREATE ROLE durable_nologin NOLOGIN;"
    sql_super -c "CREATE ROLE durable_usage_only LOGIN;"
    sql_super -c "CREATE ROLE durable_read_only LOGIN;"
    sql_super -c "CREATE ROLE durable_bypass LOGIN BYPASSRLS;"
    sql_super -c "CREATE ROLE durable_no_connect LOGIN;"
    sql_super -c "CREATE ROLE durable_no_textsearch_schema LOGIN;"
    sql_super -c "CREATE ROLE durable_actor LOGIN;"
    sql_super -c "CREATE ROLE durable_writer LOGIN;"
    sql_super -c "CREATE ROLE durable_maintainer LOGIN;"
    sql_super -c "GRANT durable_no_connect TO durable_actor;
                   GRANT durable_no_textsearch_schema TO durable_writer;"
}

test_missing_durable_cic() {
    local create_error

    sql_super -c "CREATE EXTENSION pg_textsearch;
                   CREATE TABLE missing_durable_docs (body text);"
    if create_error="$(sql_super -c "
        CREATE INDEX CONCURRENTLY missing_durable_idx
          ON missing_durable_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "background CIC succeeded without pg_durable"
    fi
    if ! grep -Fq \
        "background compaction requires pg_durable 0.2.8 or newer" \
        <<<"${create_error}"; then
        error "missing-pg_durable CIC did not use the stable admission message"
    fi
    assert_eq "rejected missing-pg_durable CIC leaves no relation" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.to_regclass('missing_durable_idx') IS NULL;")"
    sql_super -c "DROP TABLE missing_durable_docs;
                   DROP EXTENSION pg_textsearch;"
}

test_cic_preflight_rejections() {
    local create_error

    sql_super -c "CREATE TABLE superuser_docs (body text);"
    if create_error="$(sql_super -c "
        CREATE INDEX CONCURRENTLY superuser_docs_idx
          ON superuser_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "background CIC accepted a disallowed superuser owner"
    fi
    if ! grep -Fq "pg_durable superuser instances are disabled" \
        <<<"${create_error}"; then
        error "superuser rejection did not happen during CIC preflight"
    fi
    assert_eq "rejected superuser CIC leaves no relation" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.to_regclass('superuser_docs_idx') IS NULL;")"
    sql_super -c "DROP TABLE superuser_docs;"

    sql_super -c "CREATE TABLE invalid_schedule_docs (body text);
                   ALTER TABLE invalid_schedule_docs OWNER TO durable_owner;"
    if create_error="$(sql_as durable_owner -c "
        CREATE INDEX CONCURRENTLY invalid_schedule_docs_idx
          ON invalid_schedule_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'background',
                compaction_schedule = 'not a cron');" 2>&1)"; then
        error "background CIC accepted an invalid compaction schedule"
    fi
    if ! grep -Fq "Invalid cron expression" <<<"${create_error}"; then
        error "invalid schedule was not rejected during CIC preflight"
    fi
    assert_eq "rejected invalid-schedule CIC leaves no relation" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.to_regclass('invalid_schedule_docs_idx') IS NULL;")"
    sql_super -c "DROP TABLE invalid_schedule_docs;"
}

test_create_activation() {
    local index_oid create_output canonical_label

    sql_super -c "CREATE TABLE documents (id integer, body text);
                   ALTER TABLE documents OWNER TO durable_owner;"
    sql_as durable_owner -c "INSERT INTO documents
        SELECT i, 'document ' || i || ' filler filler filler'
        FROM generate_series(1, 10000) AS i;"

    if ! create_output="$(sql_as durable_owner -c "
        CREATE INDEX CONCURRENTLY documents_idx
          ON documents USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        2>&1)"; then
        error "background CIC failed under hostile owner search_path: \
${create_output}"
    fi
    if ! grep -Fq \
        "pg_textsearch background compaction is a preview feature" \
        <<<"${create_output}"; then
        error "successful activation did not emit the preview warning"
    fi

    index_oid="$(sql_super -c \
        "SELECT 'documents_idx'::regclass::oid;")"
    assert_eq "successful background CIC is valid and ready" "t" \
        "$(sql_super -c "SELECT index.indisvalid AND index.indisready
                          FROM pg_catalog.pg_index AS index
                          WHERE index.indexrelid = ${index_oid};")"
    assert_eq "hostile-path activation admits one active owner job" "1" \
        "$(active_jobs_for_index "${index_oid}")"
    canonical_label="$(sql_super -c "SELECT pg_catalog.format(
        'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%s:%s:%s',
        database.oid,
        relation.oid,
        coalesce(
            nullif(relation.reltablespace, 0),
            database.dattablespace),
        pg_catalog.pg_relation_filenode(relation.oid),
        relation.relowner,
        index_catalog.indrelid,
        pg_catalog.substr(
            lineage.option,
            pg_catalog.length('compaction_lineage=') + 1),
        pg_catalog.encode(pg_catalog.convert_to(
            pg_catalog.current_setting(
                'pg_textsearch.background_compaction_schedule'),
            'UTF8'), 'hex'))
      FROM pg_catalog.pg_class AS relation
      JOIN pg_catalog.pg_index AS index_catalog
        ON index_catalog.indexrelid = relation.oid
      JOIN pg_catalog.pg_database AS database
        ON database.datname = pg_catalog.current_database()
      CROSS JOIN LATERAL (
        SELECT option
        FROM pg_catalog.unnest(relation.reloptions) AS option
        WHERE option OPERATOR(pg_catalog.~~) 'compaction_lineage=%'
      ) AS lineage
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
    assert_eq "hostile-path current selection keeps the job active" "1" \
        "$(active_jobs_for_index "${index_oid}")"
    assert_eq "activation pins pg_durable with a normal dependency" "1" \
        "$(dependency_count)"

    local first_instance recovered_instance
    first_instance="$(active_job_id "${index_oid}")"
    wait_for_signal_node "${first_instance}" 30
    create_compaction_debt
    wait_for_no_debt documents_idx 30
    assert_eq "hostile-path current selection survives spill signaling" \
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

test_reindex_nonrelation_passthrough() {
    local database_job_after database_job_before index_oid
    local reindex_output schema_job_after schema_job_before

    sql_super -c "
        CREATE SCHEMA lifecycle_reindex_scope
          AUTHORIZATION durable_owner;
        CREATE TABLE lifecycle_reindex_scope.documents
          (id integer PRIMARY KEY, body text);
        INSERT INTO lifecycle_reindex_scope.documents
        VALUES (1, 'one'), (2, 'two');
        ALTER TABLE lifecycle_reindex_scope.documents
          OWNER TO durable_owner;" >/dev/null
    sql_as durable_owner -c "
        CREATE INDEX documents_bm25_idx
          ON lifecycle_reindex_scope.documents USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'lifecycle_reindex_scope.documents_bm25_idx'::regclass::oid;")"
    schema_job_before="$(current_generation_job_id "${index_oid}")"

    reindex_output="$(sql_as durable_owner -c \
        "REINDEX SCHEMA lifecycle_reindex_scope;" 2>&1)"
    if grep -Fq "still active" <<<"${reindex_output}"; then
        error "REINDEX SCHEMA leaked an active snapshot across commit"
    fi
    schema_job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${schema_job_after}" ] ||
        [ "${schema_job_after}" = "${schema_job_before}" ]; then
        error "REINDEX SCHEMA did not reconcile the managed generation"
    fi
    log "PASS: REINDEX SCHEMA reconciles managed indexes"

    database_job_before="${schema_job_after}"
    reindex_output="$(reindex_database \
        "REINDEX DATABASE ${TEST_DB};")"
    if grep -Fq "still active" <<<"${reindex_output}"; then
        error "REINDEX DATABASE leaked an active snapshot across commit"
    fi
    database_job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${database_job_after}" ] ||
        [ "${database_job_after}" = "${database_job_before}" ]; then
        error "REINDEX DATABASE did not reconcile the managed generation"
    fi
    log "PASS: REINDEX DATABASE reconciles managed indexes"
    wait_for_signal_node "${database_job_after}" 30

    database_job_before="${database_job_after}"
    reindex_output="$(reindex_database "REINDEX DATABASE;")"
    database_job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${database_job_after}" ] ||
        [ "${database_job_after}" = "${database_job_before}" ]; then
        error "bare REINDEX DATABASE did not reconcile the managed generation"
    fi
    log "PASS: bare REINDEX DATABASE reconciles managed indexes"

    sql_as durable_owner -c \
        "DROP SCHEMA lifecycle_reindex_scope CASCADE;" >/dev/null
}

test_late_bulk_reindex_target() {
    local file_after file_before index_oid job_after reindex_output

    sql_super <<'SQL' >/dev/null
CREATE SCHEMA lifecycle_reindex_late AUTHORIZATION durable_owner;
CREATE TABLE lifecycle_reindex_late.documents
    (id integer PRIMARY KEY, body text);
INSERT INTO lifecycle_reindex_late.documents
VALUES (1, 'one'), (2, 'two');
ALTER TABLE lifecycle_reindex_late.documents OWNER TO durable_owner;
CREATE INDEX documents_seed_bm25_idx
    ON lifecycle_reindex_late.documents USING bm25(body)
    WITH (text_config = 'english', compaction = 'inline');

CREATE TABLE public.lifecycle_reindex_late_capture
    (index_oid oid, filenumber oid);

CREATE FUNCTION public.lifecycle_reindex_late_create()
RETURNS event_trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting(
           'lifecycle.reindex_late', true)
           OPERATOR(pg_catalog.=) 'armed' THEN
        PERFORM pg_catalog.set_config(
            'lifecycle.reindex_late', 'created', false);
        EXECUTE $command$
            REINDEX INDEX
              lifecycle_reindex_late.documents_seed_bm25_idx
        $command$;
        EXECUTE $command$
            CREATE INDEX documents_bm25_idx
              ON lifecycle_reindex_late.documents USING bm25(body)
              WITH (text_config = 'english',
                    compaction = 'background',
                    compaction_schedule = '0 0 1 1 *')
        $command$;
        INSERT INTO public.lifecycle_reindex_late_capture
        SELECT relation.oid, pg_catalog.pg_relation_filenode(relation.oid)
        FROM pg_catalog.pg_class AS relation
        WHERE relation.oid =
              'lifecycle_reindex_late.documents_bm25_idx'::regclass;
    END IF;
END
$body$;

CREATE EVENT TRIGGER lifecycle_reindex_late_create
    ON ddl_command_start
    WHEN TAG IN ('REINDEX')
    EXECUTE FUNCTION public.lifecycle_reindex_late_create();
SQL
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
        SET lifecycle.reindex_late = 'armed';" >/dev/null

    reindex_output="$(sql_as durable_owner -c \
        "REINDEX SCHEMA lifecycle_reindex_late;" 2>&1)"
    if grep -Fq "still active" <<<"${reindex_output}"; then
        error "late-target REINDEX leaked an active snapshot across commit"
    fi

    index_oid="$(sql_super -c "SELECT index_oid
        FROM public.lifecycle_reindex_late_capture;")"
    file_before="$(sql_super -c "SELECT filenumber
        FROM public.lifecycle_reindex_late_capture;")"
    index_oid="$(sql_super -c "SELECT
        'lifecycle_reindex_late.documents_bm25_idx'::regclass::oid;")"
    file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${index_oid});")"
    if [ "${file_after}" = "${file_before}" ]; then
        error "REINDEX SCHEMA did not rebuild its late managed index"
    fi
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ]; then
        error "REINDEX SCHEMA did not reconcile its late managed index"
    fi
    log "PASS: REINDEX SCHEMA reconciles a late managed index"

    sql_super -c "
        DROP INDEX lifecycle_reindex_late.documents_bm25_idx;
        TRUNCATE public.lifecycle_reindex_late_capture;" >/dev/null
    reindex_output="$(sql_as durable_owner -c \
        "REINDEX (CONCURRENTLY) SCHEMA lifecycle_reindex_late;" 2>&1)"
    index_oid="$(sql_super -c "SELECT index_oid
        FROM public.lifecycle_reindex_late_capture;")"
    file_before="$(sql_super -c "SELECT filenumber
        FROM public.lifecycle_reindex_late_capture;")"
    index_oid="$(sql_super -c "SELECT
        'lifecycle_reindex_late.documents_bm25_idx'::regclass::oid;")"
    file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${index_oid});")"
    if [ "${file_after}" = "${file_before}" ]; then
        error "concurrent REINDEX SCHEMA did not rebuild its late managed index"
    fi
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ]; then
        error "concurrent REINDEX SCHEMA did not reconcile its late managed index"
    fi
    log "PASS: concurrent REINDEX SCHEMA reconciles a late managed index"

    sql_super -c "
        ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
          RESET lifecycle.reindex_late;
        DROP EVENT TRIGGER lifecycle_reindex_late_create;
        DROP FUNCTION public.lifecycle_reindex_late_create();
        DROP TABLE public.lifecycle_reindex_late_capture;
        DROP SCHEMA lifecycle_reindex_late CASCADE;" >/dev/null
}

test_bulk_reindex_concurrent_mode_change() {
    local blocker_pid file_after file_before index_oid job_after
    local reindex_output reindex_pid reindex_status=0

    sql_super -c "CREATE SCHEMA lifecycle_reindex_mode
                   AUTHORIZATION durable_owner;" >/dev/null
    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE lifecycle_reindex_mode.blocker_docs
    (id integer PRIMARY KEY);
CREATE TABLE lifecycle_reindex_mode.late_docs
    (id integer, body text);
INSERT INTO lifecycle_reindex_mode.late_docs VALUES (1, 'one');
CREATE INDEX late_docs_idx
    ON lifecycle_reindex_mode.late_docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'inline');
SQL

    PGAPPNAME=lifecycle-reindex-mode-blocker \
        sql_as durable_owner -c "
        BEGIN;
        LOCK TABLE lifecycle_reindex_mode.blocker_docs
          IN ACCESS EXCLUSIVE MODE;
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/reindex-mode-blocker.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS relation_lock
                ON relation_lock.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-reindex-mode-blocker'
                AND relation_lock.relation =
                    'lifecycle_reindex_mode.blocker_docs'::regclass
                AND relation_lock.mode = 'AccessExclusiveLock'
                AND relation_lock.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    index_oid="$(sql_super -c "SELECT
        'lifecycle_reindex_mode.late_docs_idx'::regclass::oid;")"
    file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${index_oid});")"
    reindex_output="${DATA_DIR}/reindex-mode.out"
    PGAPPNAME=lifecycle-reindex-mode \
        PGOPTIONS="-c statement_timeout=30s" \
        sql_as durable_owner -c \
        "REINDEX SCHEMA lifecycle_reindex_mode;" \
        >"${reindex_output}" 2>&1 &
    reindex_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS relation_lock
                ON relation_lock.pid = activity.pid
              WHERE activity.application_name = 'lifecycle-reindex-mode'
                AND relation_lock.relation =
                    'lifecycle_reindex_mode.blocker_docs'::regclass
                AND NOT relation_lock.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "broad REINDEX reached its post-initial-commit relation wait" \
        "1" "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS relation_lock
            ON relation_lock.pid = activity.pid
          WHERE activity.application_name = 'lifecycle-reindex-mode'
            AND relation_lock.relation =
                'lifecycle_reindex_mode.blocker_docs'::regclass
            AND NOT relation_lock.granted;")"

    sql_as durable_owner -c "
        ALTER INDEX lifecycle_reindex_mode.late_docs_idx
          SET (compaction = 'background',
               compaction_schedule = '1 2 3 4 *');" >/dev/null
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-reindex-mode-blocker';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${reindex_pid}" || reindex_status=$?
    if [ "${reindex_status}" -ne 0 ]; then
        error "broad REINDEX with a concurrent mode change failed:
$(cat "${reindex_output}")"
    fi

    file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${index_oid});")"
    assert_ne "broad REINDEX rebuilt the newly managed index" \
        "${file_before}" "${file_after}"
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ]; then
        error "broad REINDEX lost a concurrent background mode change"
    fi
    log "PASS: broad REINDEX tracks a concurrent background mode change"

    sql_super -c "DROP SCHEMA lifecycle_reindex_mode CASCADE;" >/dev/null
}

test_bulk_reindex_concurrent_schedule_change() {
    local blocker_pid blocker_table file_before index_name index_oid
    local reindex_output reindex_pid reindex_status=0 schedule_matches
    local target_table

    sql_super -c "CREATE SCHEMA lifecycle_reindex_schedule_change
                   AUTHORIZATION durable_owner;" >/dev/null
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE lifecycle_reindex_schedule_change.a_docs
    (id integer, body text);
INSERT INTO lifecycle_reindex_schedule_change.a_docs
VALUES (1, 'a');
CREATE INDEX a_docs_idx
    ON lifecycle_reindex_schedule_change.a_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '1 0 1 1 *');
CREATE TABLE lifecycle_reindex_schedule_change.b_docs
    (id integer, body text);
INSERT INTO lifecycle_reindex_schedule_change.b_docs
VALUES (1, 'b');
CREATE INDEX b_docs_idx
    ON lifecycle_reindex_schedule_change.b_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '1 0 1 1 *');
SQL
    target_table="$(sql_super -c "SELECT relation.relname
      FROM pg_catalog.pg_class AS relation
      JOIN pg_catalog.pg_namespace AS namespace
        ON namespace.oid = relation.relnamespace
      WHERE namespace.nspname = 'lifecycle_reindex_schedule_change'
        AND relation.relname IN ('a_docs', 'b_docs')
      ORDER BY relation.ctid
      LIMIT 1;")"
    blocker_table="$(sql_super -c "SELECT relation.relname
      FROM pg_catalog.pg_class AS relation
      JOIN pg_catalog.pg_namespace AS namespace
        ON namespace.oid = relation.relnamespace
      WHERE namespace.nspname = 'lifecycle_reindex_schedule_change'
        AND relation.relname IN ('a_docs', 'b_docs')
      ORDER BY relation.ctid DESC
      LIMIT 1;")"
    index_name="${target_table}_idx"
    index_oid="$(sql_super -c "SELECT
        'lifecycle_reindex_schedule_change.${index_name}'
          ::regclass::oid;")"
    file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${index_oid});")"

    PGAPPNAME=lifecycle-reindex-schedule-blocker \
        sql_as durable_owner -c "
        BEGIN;
        LOCK TABLE lifecycle_reindex_schedule_change.${blocker_table}
          IN ACCESS EXCLUSIVE MODE;
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/reindex-schedule-blocker.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS relation_lock
                ON relation_lock.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-reindex-schedule-blocker'
                AND relation_lock.relation =
                    'lifecycle_reindex_schedule_change.${blocker_table}'
                      ::regclass
                AND relation_lock.mode = 'AccessExclusiveLock'
                AND relation_lock.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    reindex_output="${DATA_DIR}/reindex-schedule-change.out"
    PGAPPNAME=lifecycle-reindex-schedule-change \
        PGOPTIONS="-c statement_timeout=30s" \
        sql_as durable_owner -c \
        "REINDEX SCHEMA lifecycle_reindex_schedule_change;" \
        >"${reindex_output}" 2>&1 &
    reindex_pid=$!
    schedule_matches=0
    for _ in $(seq 1 200); do
        if [ "$(sql_super -c "SELECT
              pg_catalog.pg_relation_filenode(${index_oid});")" != \
             "${file_before}" ] &&
            [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS relation_lock
                ON relation_lock.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-reindex-schedule-change'
                AND relation_lock.relation =
                    'lifecycle_reindex_schedule_change.${blocker_table}'
                      ::regclass
                AND NOT relation_lock.granted;")" = "1" ]; then
            schedule_matches="$(
                current_generation_schedule_job_count \
                    "${index_oid}" "1 0 1 1 *"
            )"
            if [ "${schedule_matches}" = "1" ]; then
                break
            fi
        fi
        sleep 0.1
    done
    assert_eq "broad REINDEX reconciled before its later relation wait" \
        "1" "${schedule_matches}"

    sql_as durable_owner -c "
        ALTER INDEX lifecycle_reindex_schedule_change.${index_name}
          SET (compaction_schedule = '2 0 1 1 *');" >/dev/null
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-reindex-schedule-blocker';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${reindex_pid}" || reindex_status=$?
    if [ "${reindex_status}" -ne 0 ]; then
        error "broad REINDEX with a concurrent schedule change failed:
$(cat "${reindex_output}")"
    fi

    schedule_matches="$(
        current_generation_schedule_job_count "${index_oid}" "2 0 1 1 *"
    )"
    assert_eq "broad REINDEX preserves a later committed schedule" \
        "1" "${schedule_matches}"

    sql_super -c "
        DROP SCHEMA lifecycle_reindex_schedule_change CASCADE;" >/dev/null
}

test_failed_bulk_reindex_reconciliation() {
    local failure_value index_oid job_after job_before reindex_error

    sql_super -c "CREATE SCHEMA lifecycle_reindex_failure
                   AUTHORIZATION durable_owner;" >/dev/null
    sql_as durable_owner <<'SQL'
CREATE TABLE lifecycle_reindex_failure.first_docs
    (id integer, body text);
INSERT INTO lifecycle_reindex_failure.first_docs VALUES (1, 'one');
CREATE INDEX first_docs_idx
    ON lifecycle_reindex_failure.first_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');

CREATE FUNCTION lifecycle_reindex_failure.fail_when_enabled(value text)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
AS $body$
BEGIN
    IF pg_catalog.current_setting(
            'lifecycle.fail_reindex', true) = value THEN
        RAISE EXCEPTION 'intentional later REINDEX failure';
    END IF;
    RETURN value;
END
$body$;

CREATE TABLE lifecycle_reindex_failure.second_docs (body text);
INSERT INTO lifecycle_reindex_failure.second_docs VALUES ('two');
CREATE INDEX second_docs_idx
    ON lifecycle_reindex_failure.second_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE INDEX first_docs_fail_idx
    ON lifecycle_reindex_failure.first_docs
    (lifecycle_reindex_failure.fail_when_enabled(body));
CREATE INDEX second_docs_fail_idx
    ON lifecycle_reindex_failure.second_docs
    (lifecycle_reindex_failure.fail_when_enabled(body));
SQL
    index_oid="$(sql_super -c "
        SELECT pg_catalog.format(
                   '%I.%I', namespace.nspname, relation.relname || '_idx'
               )::regclass::oid
        FROM pg_catalog.pg_class AS relation
        JOIN pg_catalog.pg_namespace AS namespace
          ON namespace.oid = relation.relnamespace
        WHERE namespace.nspname = 'lifecycle_reindex_failure'
          AND relation.relname IN ('first_docs', 'second_docs')
        ORDER BY relation.ctid
        LIMIT 1;")"
    failure_value="$(sql_super -c "
        SELECT CASE relation.relname
                 WHEN 'first_docs' THEN 'one'
                 ELSE 'two'
               END
        FROM pg_catalog.pg_class AS relation
        JOIN pg_catalog.pg_namespace AS namespace
          ON namespace.oid = relation.relnamespace
        WHERE namespace.nspname = 'lifecycle_reindex_failure'
          AND relation.relname IN ('first_docs', 'second_docs')
        ORDER BY relation.ctid DESC
        LIMIT 1;")"
    job_before="$(current_generation_job_id "${index_oid}")"

    if reindex_error="$(PGOPTIONS="-c lifecycle.fail_reindex=${failure_value}" \
        sql_as durable_owner -c \
        "REINDEX SCHEMA lifecycle_reindex_failure;" 2>&1)"; then
        error "bulk REINDEX failure test unexpectedly succeeded"
    fi
    if ! grep -Fq "intentional later REINDEX failure" \
        <<<"${reindex_error}"; then
        error "bulk REINDEX failed for the wrong reason: ${reindex_error}"
    fi

    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "failed bulk REINDEX stranded an already committed generation"
    fi
    log "PASS: failed bulk REINDEX reconciles committed generations"

    sql_super -c \
        "DROP SCHEMA lifecycle_reindex_failure CASCADE;" >/dev/null
}

test_partitioned_create_activation() {
    local create_output leaf_jobs parent_oid

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_partitioned_docs
          (id integer, body text)
          PARTITION BY RANGE (id);
        CREATE TABLE public.lifecycle_partitioned_docs_low
          PARTITION OF public.lifecycle_partitioned_docs
          FOR VALUES FROM (0) TO (100);
        CREATE TABLE public.lifecycle_partitioned_docs_high
          PARTITION OF public.lifecycle_partitioned_docs
          FOR VALUES FROM (100) TO (200);"
    if ! create_output="$(sql_as durable_owner -c "
        CREATE INDEX lifecycle_partitioned_idx
          ON public.lifecycle_partitioned_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" 2>&1)"; then
        error "partitioned background CREATE failed: ${create_output}"
    fi

    parent_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_partitioned_idx'::regclass::oid;")"
    assert_eq "partitioned parent has two physical leaf indexes" "2" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_inherits AS inheritance
          JOIN pg_catalog.pg_class AS child
            ON child.oid = inheritance.inhrelid
          WHERE inheritance.inhparent = ${parent_oid}
            AND child.relkind = 'i';")"
    assert_eq "partitioned parent has no managed workflow" "0" \
        "$(active_jobs_for_index "${parent_oid}")"
    leaf_jobs="$(sql_super -c "SELECT count(*)
      FROM pg_catalog.pg_inherits AS inheritance
      JOIN pg_catalog.pg_class AS relation
        ON relation.oid = inheritance.inhrelid
      JOIN pg_catalog.pg_database AS database
        ON database.datname = pg_catalog.current_database()
      JOIN df.instances AS instance
        ON instance.label LIKE pg_catalog.format(
             'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%%',
             database.oid,
             relation.oid,
             coalesce(nullif(relation.reltablespace, 0),
                      database.dattablespace),
             pg_catalog.pg_relation_filenode(relation.oid),
             relation.relowner)
       AND instance.submitted_by::pg_catalog.oid = relation.relowner
       AND instance.status IN ('pending', 'running')
      WHERE inheritance.inhparent = ${parent_oid}
        AND relation.relkind = 'i';")"
    assert_eq "partitioned CREATE activates every physical leaf" "2" \
        "${leaf_jobs}"

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_partitioned_docs_later
          PARTITION OF public.lifecycle_partitioned_docs
          FOR VALUES FROM (200) TO (300);" >/dev/null 2>&1
    assert_eq "new partition inherits one managed workflow" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_inherits AS inheritance
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = inheritance.inhrelid
          JOIN pg_catalog.pg_index AS index_catalog
            ON index_catalog.indexrelid = relation.oid
          WHERE inheritance.inhparent = ${parent_oid}
            AND relation.relkind = 'i'
            AND index_catalog.indrelid =
                'public.lifecycle_partitioned_docs_later'::regclass
            AND EXISTS (
              SELECT 1 FROM df.instances AS instance
              WHERE instance.label OPERATOR(pg_catalog.~~)
                    ('pg_textsearch:bg:v1:%:' ||
                     relation.oid::pg_catalog.text || ':%')
                AND instance.status OPERATOR(pg_catalog.=)
                    ANY (ARRAY['pending', 'running']::pg_catalog.text[]));")"

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_partitioned_attach
          (id integer, body text);
        CREATE INDEX lifecycle_partitioned_attach_idx
          ON public.lifecycle_partitioned_attach USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'manual');
        ALTER TABLE public.lifecycle_partitioned_docs
          ATTACH PARTITION public.lifecycle_partitioned_attach
          FOR VALUES FROM (300) TO (400);" >/dev/null 2>&1
    assert_eq "attached index inherits managed parent options" "t:t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
            ':',
            relation.reloptions @> ARRAY['compaction=background'],
            relation.reloptions @>
              ARRAY['compaction_schedule=0 0 1 1 *'],
            relation.reloptions @>
              ARRAY['compaction_lineage=' || pg_catalog.substr(
                parent_option,
                pg_catalog.length('compaction_lineage=') + 1)])
          FROM pg_catalog.pg_class AS relation
          CROSS JOIN LATERAL (
            SELECT option AS parent_option
            FROM pg_catalog.pg_class AS parent,
                 LATERAL pg_catalog.unnest(parent.reloptions) AS option
            WHERE parent.oid = ${parent_oid}
              AND option OPERATOR(pg_catalog.~~)
                  'compaction_lineage=%'
          ) AS lineage
          WHERE relation.oid =
                'public.lifecycle_partitioned_attach_idx'::regclass;")"
    assert_eq "attached existing index receives a managed workflow" "1" \
        "$(active_jobs_for_index \
          "$(sql_super -c "SELECT
            'public.lifecycle_partitioned_attach_idx'::regclass::oid;")")"

    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'partitioned lifecycle test complete')
      FROM df.instances AS instance
      JOIN pg_catalog.pg_inherits AS inheritance
        ON instance.label OPERATOR(pg_catalog.~~)
           ('pg_textsearch:bg:v1:%:' ||
            inheritance.inhrelid::pg_catalog.text || ':%')
      WHERE inheritance.inhparent = ${parent_oid}
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    sql_as durable_owner -c \
        "DROP TABLE public.lifecycle_partitioned_docs;"
}

test_direct_index_partition_attach() {
    local child_oid parent_oid

    sql_as durable_owner <<'SQL'
CREATE TABLE public.lifecycle_direct_attach_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_direct_attach_leaf
    PARTITION OF public.lifecycle_direct_attach_docs
    FOR VALUES FROM (0) TO (100);
CREATE INDEX lifecycle_direct_attach_parent_idx
    ON ONLY public.lifecycle_direct_attach_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE INDEX lifecycle_direct_attach_child_idx
    ON public.lifecycle_direct_attach_leaf USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
ALTER INDEX public.lifecycle_direct_attach_parent_idx
    ATTACH PARTITION public.lifecycle_direct_attach_child_idx;
SQL
    parent_oid="$(sql_super -c "SELECT
        'public.lifecycle_direct_attach_parent_idx'::regclass::oid;")"
    child_oid="$(sql_super -c "SELECT
        'public.lifecycle_direct_attach_child_idx'::regclass::oid;")"

    assert_eq "directly attached index inherits managed parent options" \
        "t:t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
            ':',
            child.reloptions @> ARRAY['compaction=background'],
            child.reloptions @>
              ARRAY['compaction_schedule=0 0 1 1 *'],
            child.reloptions @>
              ARRAY['compaction_lineage=' || pg_catalog.substr(
                parent_option,
                pg_catalog.length('compaction_lineage=') + 1)])
          FROM pg_catalog.pg_class AS child
          CROSS JOIN LATERAL (
            SELECT option AS parent_option
            FROM pg_catalog.pg_class AS parent,
                 LATERAL pg_catalog.unnest(parent.reloptions) AS option
            WHERE parent.oid = ${parent_oid}
              AND option OPERATOR(pg_catalog.~~)
                  'compaction_lineage=%'
          ) AS lineage
          WHERE child.oid = ${child_oid};")"
    assert_eq "directly attached index receives a managed workflow" "1" \
        "$(current_generation_job_count "${child_oid}")"

    sql_super -c \
        "DROP TABLE public.lifecycle_direct_attach_docs;" >/dev/null
}

test_reused_intermediate_partition_options() {
    local direct_existing_oid direct_intermediate_oid direct_late_oid
    local direct_lineage table_existing_oid table_intermediate_oid
    local table_late_oid table_lineage

    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_intermediate_table_root
    (id integer, subid integer, body text)
    PARTITION BY RANGE (id);
CREATE INDEX lifecycle_intermediate_table_root_idx
    ON ONLY public.lifecycle_intermediate_table_root USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');

CREATE TABLE public.lifecycle_intermediate_table_branch
    (id integer, subid integer, body text)
    PARTITION BY RANGE (subid);
CREATE TABLE public.lifecycle_intermediate_table_low
    PARTITION OF public.lifecycle_intermediate_table_branch
    FOR VALUES FROM (0) TO (100);
CREATE INDEX lifecycle_intermediate_table_branch_idx
    ON public.lifecycle_intermediate_table_branch USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
ALTER TABLE public.lifecycle_intermediate_table_root
    ATTACH PARTITION public.lifecycle_intermediate_table_branch
    FOR VALUES FROM (0) TO (100);

CREATE TABLE public.lifecycle_intermediate_direct_root
    (id integer, subid integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_intermediate_direct_branch
    (id integer, subid integer, body text)
    PARTITION BY RANGE (subid);
ALTER TABLE public.lifecycle_intermediate_direct_root
    ATTACH PARTITION public.lifecycle_intermediate_direct_branch
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_intermediate_direct_low
    PARTITION OF public.lifecycle_intermediate_direct_branch
    FOR VALUES FROM (0) TO (100);
CREATE INDEX lifecycle_intermediate_direct_root_idx
    ON ONLY public.lifecycle_intermediate_direct_root USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE INDEX lifecycle_intermediate_direct_branch_idx
    ON public.lifecycle_intermediate_direct_branch USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
ALTER INDEX public.lifecycle_intermediate_direct_root_idx
    ATTACH PARTITION public.lifecycle_intermediate_direct_branch_idx;
SQL

    table_intermediate_oid="$(sql_super -c "SELECT
        'public.lifecycle_intermediate_table_branch_idx'::regclass::oid;")"
    table_existing_oid="$(sql_super -c "SELECT
        'public.lifecycle_intermediate_table_low_body_idx'::regclass::oid;")"
    table_lineage="$(
        index_lineage public.lifecycle_intermediate_table_root_idx
    )"
    assert_eq "table attach aligns reused intermediate options" "t:t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
            ':',
            relation.reloptions @> ARRAY['compaction=background'],
            relation.reloptions @>
              ARRAY['compaction_schedule=0 0 1 1 *'],
            relation.reloptions @>
              ARRAY['compaction_lineage=${table_lineage}'])
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid = ${table_intermediate_oid};")"
    assert_eq "table attach activates existing physical leaves" "1" \
        "$(current_generation_job_count "${table_existing_oid}")"
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_intermediate_table_late
          PARTITION OF public.lifecycle_intermediate_table_branch
          FOR VALUES FROM (100) TO (200);" >/dev/null 2>&1
    table_late_oid="$(sql_super -c "SELECT indexrelid
      FROM pg_catalog.pg_index
      WHERE indrelid =
            'public.lifecycle_intermediate_table_late'::regclass;")"
    assert_eq "later table-attached leaf inherits aligned options" "t:t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
            ':',
            relation.reloptions @> ARRAY['compaction=background'],
            relation.reloptions @>
              ARRAY['compaction_schedule=0 0 1 1 *'],
            relation.reloptions @>
              ARRAY['compaction_lineage=${table_lineage}'])
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid = ${table_late_oid};")"
    assert_eq "later table-attached leaf has one workflow" "1" \
        "$(current_generation_job_count "${table_late_oid}")"

    direct_intermediate_oid="$(sql_super -c "SELECT
        'public.lifecycle_intermediate_direct_branch_idx'::regclass::oid;")"
    direct_existing_oid="$(sql_super -c "SELECT
        'public.lifecycle_intermediate_direct_low_body_idx'::regclass::oid;")"
    direct_lineage="$(
        index_lineage public.lifecycle_intermediate_direct_root_idx
    )"
    assert_eq "direct attach aligns reused intermediate options" "t:t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
            ':',
            relation.reloptions @> ARRAY['compaction=background'],
            relation.reloptions @>
              ARRAY['compaction_schedule=0 0 1 1 *'],
            relation.reloptions @>
              ARRAY['compaction_lineage=${direct_lineage}'])
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid = ${direct_intermediate_oid};")"
    assert_eq "direct attach activates existing physical leaves" "1" \
        "$(current_generation_job_count "${direct_existing_oid}")"
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_intermediate_direct_late
          PARTITION OF public.lifecycle_intermediate_direct_branch
          FOR VALUES FROM (100) TO (200);" >/dev/null 2>&1
    direct_late_oid="$(sql_super -c "SELECT indexrelid
      FROM pg_catalog.pg_index
      WHERE indrelid =
            'public.lifecycle_intermediate_direct_late'::regclass;")"
    assert_eq "later directly attached leaf inherits aligned options" \
        "t:t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
            ':',
            relation.reloptions @> ARRAY['compaction=background'],
            relation.reloptions @>
              ARRAY['compaction_schedule=0 0 1 1 *'],
            relation.reloptions @>
              ARRAY['compaction_lineage=${direct_lineage}'])
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid = ${direct_late_oid};")"
    assert_eq "later directly attached leaf has one workflow" "1" \
        "$(current_generation_job_count "${direct_late_oid}")"

    sql_super -c "
        DROP TABLE public.lifecycle_intermediate_table_root,
                   public.lifecycle_intermediate_direct_root;" >/dev/null
}

test_partition_detach_lineage() {
    local branch_index_oid concurrent_child_index_oid concurrent_detach_output
    local concurrent_detach_pid concurrent_new_lineage concurrent_old_job
    local concurrent_old_lineage concurrent_reader_pid detach_backend_pid
    local detach_dump detached_job detached_lineage detached_low_index_oid
    local old_job old_lineage restore_output root_index_oid sibling_index_oid
    local restored_detached_lineage restored_parent_lineage

    detach_dump="${DATA_DIR}/partition-detach-lineage.sql"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_detach_root
    (id integer, subid integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_detach_branch
    (id integer, subid integer, body text)
    PARTITION BY RANGE (subid);
ALTER TABLE public.lifecycle_detach_root
    ATTACH PARTITION public.lifecycle_detach_branch
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_detach_branch_low
    PARTITION OF public.lifecycle_detach_branch
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_detach_sibling
    PARTITION OF public.lifecycle_detach_root
    FOR VALUES FROM (100) TO (200);
CREATE INDEX lifecycle_detach_root_idx
    ON public.lifecycle_detach_root USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    root_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_detach_root_idx'::regclass::oid;")"
    branch_index_oid="$(sql_super -c "SELECT indexrelid
      FROM pg_catalog.pg_index
      WHERE indrelid = 'public.lifecycle_detach_branch'::regclass;")"
    detached_low_index_oid="$(sql_super -c "SELECT indexrelid
      FROM pg_catalog.pg_index
      WHERE indrelid = 'public.lifecycle_detach_branch_low'::regclass;")"
    sibling_index_oid="$(sql_super -c "SELECT indexrelid
      FROM pg_catalog.pg_index
      WHERE indrelid = 'public.lifecycle_detach_sibling'::regclass;")"
    old_lineage="$(index_lineage public.lifecycle_detach_root_idx)"
    old_job="$(current_generation_job_id "${detached_low_index_oid}")"

    sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_detach_root
          DETACH PARTITION public.lifecycle_detach_branch;" >/dev/null 2>&1
    detached_lineage="$(sql_super -c "SELECT
        pg_catalog.substr(
          option, pg_catalog.length('compaction_lineage=') + 1)
      FROM pg_catalog.pg_class AS relation
      CROSS JOIN LATERAL
        pg_catalog.unnest(relation.reloptions) AS option
      WHERE relation.oid = ${branch_index_oid}
        AND option OPERATOR(pg_catalog.~~) 'compaction_lineage=%';")"
    assert_ne "plain detach assigns a fresh hierarchy lineage" \
        "${old_lineage}" "${detached_lineage}"
    assert_eq "plain detach lineage remains 128-bit" "32" \
        "${#detached_lineage}"
    assert_eq "detached physical leaves share the fresh lineage" \
        "${detached_lineage}" \
        "$(sql_super -c "SELECT pg_catalog.substr(
            option, pg_catalog.length('compaction_lineage=') + 1)
          FROM pg_catalog.pg_class AS relation
          CROSS JOIN LATERAL
            pg_catalog.unnest(relation.reloptions) AS option
          WHERE relation.oid = ${detached_low_index_oid}
            AND option OPERATOR(pg_catalog.~~)
                'compaction_lineage=%';")"
    assert_eq "former parent hierarchy retains its lineage" \
        "${old_lineage}:${old_lineage}" \
        "$(index_lineage public.lifecycle_detach_root_idx):$(
            sql_super -c "SELECT pg_catalog.substr(
                option, pg_catalog.length('compaction_lineage=') + 1)
              FROM pg_catalog.pg_class AS relation
              CROSS JOIN LATERAL
                pg_catalog.unnest(relation.reloptions) AS option
              WHERE relation.oid = ${sibling_index_oid}
                AND option OPERATOR(pg_catalog.~~)
                    'compaction_lineage=%';"
        )"
    detached_job="$(current_generation_job_id "${detached_low_index_oid}")"
    assert_ne "plain detach replaces the detached workflow" \
        "${old_job}" "${detached_job}"
    assert_eq "plain detach preserves the workflow schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '0 0 1 1 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${detached_job}';")"

    "${PGBINDIR}/pg_dump" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" --schema-only --no-owner \
        --table=public.lifecycle_detach_root \
        --table=public.lifecycle_detach_branch \
        --table=public.lifecycle_detach_branch_low \
        --table=public.lifecycle_detach_sibling >"${detach_dump}"
    sql_as durable_owner -c "
        DROP TABLE public.lifecycle_detach_root,
                   public.lifecycle_detach_branch;" >/dev/null
    if ! restore_output="$(sql_as durable_owner -f "${detach_dump}" 2>&1)"; then
        error "detached hierarchies failed schema-only restore: \
${restore_output}"
    fi
    restored_parent_lineage="$(
        index_lineage public.lifecycle_detach_root_idx
    )"
    restored_detached_lineage="$(sql_super -c "SELECT pg_catalog.substr(
        option, pg_catalog.length('compaction_lineage=') + 1)
      FROM pg_catalog.pg_class AS relation
      CROSS JOIN LATERAL
        pg_catalog.unnest(relation.reloptions) AS option
      WHERE relation.oid = (
          SELECT indexrelid FROM pg_catalog.pg_index
          WHERE indrelid = 'public.lifecycle_detach_branch'::regclass)
        AND option OPERATOR(pg_catalog.~~) 'compaction_lineage=%';")"
    assert_eq "restored parent lineage remains 128-bit" "32" \
        "${#restored_parent_lineage}"
    assert_eq "restored detached lineage remains 128-bit" "32" \
        "${#restored_detached_lineage}"
    assert_ne "restored detached hierarchies retain distinct lineages" \
        "${restored_parent_lineage}" "${restored_detached_lineage}"
    sql_as durable_owner -c "
        DROP TABLE public.lifecycle_detach_root,
                   public.lifecycle_detach_branch;" >/dev/null

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_detach_concurrent_root
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_detach_concurrent_low
    PARTITION OF public.lifecycle_detach_concurrent_root
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_detach_concurrent_high
    PARTITION OF public.lifecycle_detach_concurrent_root
    FOR VALUES FROM (100) TO (200);
CREATE INDEX lifecycle_detach_concurrent_idx
    ON public.lifecycle_detach_concurrent_root USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    concurrent_child_index_oid="$(sql_super -c "SELECT indexrelid
      FROM pg_catalog.pg_index
      WHERE indrelid =
            'public.lifecycle_detach_concurrent_high'::regclass;")"
    concurrent_old_lineage="$(
        index_lineage public.lifecycle_detach_concurrent_idx
    )"
    concurrent_old_job="$(
        current_generation_job_id "${concurrent_child_index_oid}"
    )"

    PGAPPNAME=lifecycle-detach-reader sql_as durable_owner <<'SQL' \
        >/dev/null 2>&1 &
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM public.lifecycle_detach_concurrent_root;
SELECT pg_catalog.pg_sleep(30);
COMMIT;
SQL
    concurrent_reader_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-detach-reader'
                AND query OPERATOR(pg_catalog.~~) '%pg_sleep%';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "concurrent detach reader holds an old snapshot" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-detach-reader'
            AND query OPERATOR(pg_catalog.~~) '%pg_sleep%';")"

    concurrent_detach_output="${DATA_DIR}/partition-detach-concurrent.out"
    PGAPPNAME=lifecycle-detach-concurrent sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_detach_concurrent_root
          DETACH PARTITION public.lifecycle_detach_concurrent_high
          CONCURRENTLY;" >"${concurrent_detach_output}" 2>&1 &
    concurrent_detach_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_inherits
              WHERE inhrelid =
                    'public.lifecycle_detach_concurrent_high'::regclass
                AND inhdetachpending;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "concurrent detach reaches pending phase" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_inherits
          WHERE inhrelid =
                'public.lifecycle_detach_concurrent_high'::regclass
            AND inhdetachpending;")"
    assert_eq "concurrent detach phase one preserves lineage" \
        "${concurrent_old_lineage}" \
        "$(sql_super -c "SELECT pg_catalog.substr(
            option, pg_catalog.length('compaction_lineage=') + 1)
          FROM pg_catalog.pg_class AS relation
          CROSS JOIN LATERAL
            pg_catalog.unnest(relation.reloptions) AS option
          WHERE relation.oid = ${concurrent_child_index_oid}
            AND option OPERATOR(pg_catalog.~~)
                'compaction_lineage=%';")"

    detach_backend_pid="$(sql_super -c "SELECT pid
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-detach-concurrent';")"
    sql_super -c \
        "SELECT pg_catalog.pg_terminate_backend(${detach_backend_pid});" \
        >/dev/null
    if wait "${concurrent_detach_pid}"; then
        error "concurrent detach completed before forced finalization"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-detach-reader';" >/dev/null
    wait "${concurrent_reader_pid}" 2>/dev/null || true
    assert_eq "interrupted concurrent detach remains pending" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_inherits
          WHERE inhrelid =
                'public.lifecycle_detach_concurrent_high'::regclass
            AND inhdetachpending;")"

    sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_detach_concurrent_root
          DETACH PARTITION public.lifecycle_detach_concurrent_high
          FINALIZE;" >/dev/null 2>&1
    concurrent_new_lineage="$(sql_super -c "SELECT pg_catalog.substr(
        option, pg_catalog.length('compaction_lineage=') + 1)
      FROM pg_catalog.pg_class AS relation
      CROSS JOIN LATERAL
        pg_catalog.unnest(relation.reloptions) AS option
      WHERE relation.oid = ${concurrent_child_index_oid}
        AND option OPERATOR(pg_catalog.~~) 'compaction_lineage=%';")"
    assert_ne "concurrent finalize assigns a fresh lineage" \
        "${concurrent_old_lineage}" "${concurrent_new_lineage}"
    assert_eq "concurrent finalize leaves parent lineage unchanged" \
        "${concurrent_old_lineage}" \
        "$(index_lineage public.lifecycle_detach_concurrent_idx)"
    assert_ne "concurrent finalize replaces the detached workflow" \
        "${concurrent_old_job}" \
        "$(current_generation_job_id "${concurrent_child_index_oid}")"

    sql_super -c "
        DROP TABLE public.lifecycle_detach_concurrent_root,
                   public.lifecycle_detach_concurrent_high;" >/dev/null
}

test_partitioned_existing_leaf_reconciliation() {
    local high_oid low_oid parent_lineage parent_oid

    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_existing_leaf_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_existing_leaf_low
    PARTITION OF public.lifecycle_existing_leaf_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_existing_leaf_high
    PARTITION OF public.lifecycle_existing_leaf_docs
    FOR VALUES FROM (100) TO (200);
CREATE INDEX lifecycle_existing_leaf_low_idx
    ON public.lifecycle_existing_leaf_low USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
SQL
    sql_super -c "ALTER TABLE public.lifecycle_existing_leaf_high
                   OWNER TO durable_owner_two;"
    sql_as durable_owner_two -c "
        CREATE INDEX lifecycle_existing_leaf_high_idx
          ON public.lifecycle_existing_leaf_high USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '5 4 3 2 *');" >/dev/null 2>&1

    low_oid="$(sql_super -c "SELECT
        'public.lifecycle_existing_leaf_low_idx'::regclass::oid;")"
    high_oid="$(sql_super -c "SELECT
        'public.lifecycle_existing_leaf_high_idx'::regclass::oid;")"
    sql_as durable_owner -c "
        CREATE INDEX lifecycle_existing_leaf_parent_idx
          ON public.lifecycle_existing_leaf_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1

    parent_oid="$(sql_super -c "SELECT
        'public.lifecycle_existing_leaf_parent_idx'::regclass::oid;")"
    parent_lineage="$(
        index_lineage public.lifecycle_existing_leaf_parent_idx
    )"
    assert_eq "partitioned CREATE attaches both existing leaf indexes" \
        "2" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_partition_tree(${parent_oid}) AS tree
          WHERE tree.isleaf
            AND tree.relid = ANY (
              ARRAY[${low_oid}, ${high_oid}]::pg_catalog.oid[]);")"
    assert_eq "attached leaves inherit parent background options" "2:2:2" \
        "$(sql_super -c "SELECT
            count(*) FILTER (
              WHERE relation.reloptions @>
                    ARRAY['compaction=background']) || ':' ||
            count(*) FILTER (
              WHERE relation.reloptions @>
                    ARRAY['compaction_schedule=0 0 1 1 *']) || ':' ||
            count(*) FILTER (
              WHERE relation.reloptions @>
                    ARRAY['compaction_lineage=${parent_lineage}'])
          FROM pg_catalog.pg_partition_tree(${parent_oid}) AS tree
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = tree.relid
          WHERE tree.isleaf
            AND relation.relkind = 'i';")"
    assert_eq "attached leaves have one current owner workflow each" "2:2" \
        "$(sql_super -c "SELECT
            count(*) || ':' ||
            count(*) FILTER (
              WHERE EXISTS (
                SELECT 1
                FROM df.instances AS instance
                JOIN pg_catalog.pg_database AS database
                  ON database.datname = pg_catalog.current_database()
                WHERE instance.label OPERATOR(pg_catalog.~~)
                      pg_catalog.format(
                        'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%s:%s:%%',
                        database.oid,
                        relation.oid,
                        coalesce(nullif(relation.reltablespace, 0),
                                 database.dattablespace),
                        pg_catalog.pg_relation_filenode(relation.oid),
                        relation.relowner,
                        index_catalog.indrelid,
                        '${parent_lineage}')
                  AND instance.submitted_by::pg_catalog.oid =
                      relation.relowner
                  AND instance.status OPERATOR(pg_catalog.=)
                      ANY (ARRAY['pending', 'running']
                           ::pg_catalog.text[])))
          FROM pg_catalog.pg_partition_tree(${parent_oid}) AS tree
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = tree.relid
          JOIN pg_catalog.pg_index AS index_catalog
            ON index_catalog.indexrelid = relation.oid
          WHERE tree.isleaf
            AND relation.relkind = 'i';")"
    assert_eq "attached leaves select the parent workflow as current" "2" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_partition_tree(${parent_oid}) AS tree
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = tree.relid
          JOIN pg_catalog.pg_database AS database
            ON database.datname = pg_catalog.current_database()
          WHERE tree.isleaf
            AND relation.relkind = 'i'
            AND (
              SELECT instance.label
              FROM df.instances AS instance
              WHERE instance.label OPERATOR(pg_catalog.~~)
                    pg_catalog.format(
                      'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%%',
                      database.oid,
                      relation.oid,
                      coalesce(nullif(relation.reltablespace, 0),
                               database.dattablespace),
                      pg_catalog.pg_relation_filenode(relation.oid),
                      relation.relowner)
                AND instance.submitted_by::pg_catalog.oid =
                    relation.relowner
                AND instance.status OPERATOR(pg_catalog.=)
                    ANY (ARRAY['pending', 'running']
                         ::pg_catalog.text[])
              ORDER BY instance.created_at DESC, instance.id DESC
              LIMIT 1
            ) OPERATOR(pg_catalog.~~)
              ('%:' || '${parent_lineage}' || ':%');")"

    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'existing leaf reconciliation complete')
      FROM df.instances AS instance
      WHERE instance.submitted_by = 'durable_owner'::regrole
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[])
        AND (instance.label OPERATOR(pg_catalog.~~)
               'pg_textsearch:bg:v1:%:${low_oid}:%');" >/dev/null
    sql_as durable_owner_two -c "SELECT df.cancel(
        instance.id, 'existing leaf reconciliation complete')
      FROM df.instances AS instance
      WHERE instance.submitted_by = 'durable_owner_two'::regrole
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[])
        AND (instance.label OPERATOR(pg_catalog.~~)
               'pg_textsearch:bg:v1:%:${high_oid}:%');" >/dev/null
    sql_super -c \
        "DROP TABLE public.lifecycle_existing_leaf_docs;"
}

test_create_tracking_reentry() {
    local nested_oid outer_oid

    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_create_reentry_docs
    (body_a text, body_b text);
CREATE FUNCTION public.lifecycle_create_reenter()
RETURNS event_trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-create-reentry'
       AND pg_catalog.to_regclass(
             'public.lifecycle_create_reentry_nested_idx') IS NULL THEN
        EXECUTE
            'CREATE INDEX lifecycle_create_reentry_nested_idx '
            'ON public.lifecycle_create_reentry_docs USING bm25(body_b) '
            'WITH (text_config = ''english'', compaction = ''manual'')';
    END IF;
END
$body$;
SQL
    sql_super -c "
        CREATE EVENT TRIGGER lifecycle_create_reenter
          ON ddl_command_end
          WHEN TAG IN ('CREATE INDEX')
          EXECUTE FUNCTION public.lifecycle_create_reenter();" >/dev/null

    PGAPPNAME=lifecycle-create-reentry sql_as durable_owner -c "
        CREATE INDEX lifecycle_create_reentry_outer_idx
          ON public.lifecycle_create_reentry_docs USING bm25(body_a)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1

    sql_super -c "DROP EVENT TRIGGER lifecycle_create_reenter;
                   DROP FUNCTION public.lifecycle_create_reenter();" \
        >/dev/null
    outer_oid="$(sql_super -c "SELECT
        'public.lifecycle_create_reentry_outer_idx'::regclass::oid;")"
    nested_oid="$(sql_super -c "SELECT
        'public.lifecycle_create_reentry_nested_idx'::regclass::oid;")"
    assert_eq "nested CREATE retains its own manual configuration" "t" \
        "$(sql_super -c "SELECT reloptions @> ARRAY['compaction=manual']
          FROM pg_catalog.pg_class WHERE oid = ${nested_oid};")"
    assert_eq "outer CREATE activates only its own index" "1:0" \
        "$(active_jobs_for_index "${outer_oid}"):$(
            active_jobs_for_index "${nested_oid}")"

    sql_super -c "DROP TABLE public.lifecycle_create_reentry_docs;"
}

test_cached_create_uses_fresh_lineage() {
    local first_lineage second_lineage

    sql_super -c "
CREATE SCHEMA lifecycle_cached_create_a AUTHORIZATION durable_owner;
CREATE SCHEMA lifecycle_cached_create_b AUTHORIZATION durable_owner;" \
        >/dev/null
    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE lifecycle_cached_create_a.docs (body text);
CREATE TABLE lifecycle_cached_create_b.docs (body text);
CREATE FUNCTION public.lifecycle_cached_create()
RETURNS void
LANGUAGE plpgsql
AS $body$
DECLARE
    schema_name text;
BEGIN
    FOREACH schema_name IN ARRAY ARRAY[
        'lifecycle_cached_create_a',
        'lifecycle_cached_create_b'
    ] LOOP
        PERFORM pg_catalog.set_config(
            'search_path',
            pg_catalog.format('%I, public, pg_catalog', schema_name),
            true);
        CREATE INDEX lifecycle_cached_create_idx
          ON docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
    END LOOP;
END
$body$;

SELECT public.lifecycle_cached_create();
SQL
    first_lineage="$(
        index_lineage lifecycle_cached_create_a.lifecycle_cached_create_idx
    )"
    second_lineage="$(
        index_lineage lifecycle_cached_create_b.lifecycle_cached_create_idx
    )"

    if [ "${second_lineage}" = "${first_lineage}" ]; then
        error "cached CREATE INDEX reused its prior compaction lineage"
    fi
    assert_eq "cached CREATE INDEX creates one workflow per heap" "2" \
        "$(( $(active_jobs_for_index "$(sql_super -c "SELECT
          'lifecycle_cached_create_a.lifecycle_cached_create_idx'
             ::regclass::oid;")") +
             $(active_jobs_for_index "$(sql_super -c "SELECT
          'lifecycle_cached_create_b.lifecycle_cached_create_idx'
             ::regclass::oid;")") ))"

    sql_super -c "DROP SCHEMA lifecycle_cached_create_a CASCADE;
                   DROP SCHEMA lifecycle_cached_create_b CASCADE;
                   DROP FUNCTION public.lifecycle_cached_create();"
}

test_create_tracking_concurrent() {
    local blocker_pid concurrent_oid outer_oid outer_output outer_pid
    local outer_status=0

    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_create_concurrent_docs
    (body_a text, body_b text);
INSERT INTO public.lifecycle_create_concurrent_docs
VALUES ('pause', 'other'), ('continue', 'other');
CREATE FUNCTION public.lifecycle_create_concurrent_pause(value text)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF value OPERATOR(pg_catalog.=) 'pause' THEN
        PERFORM pg_catalog.pg_advisory_xact_lock(478, 8);
    END IF;
    RETURN value;
END
$body$;
SQL

    PGAPPNAME=lifecycle-create-concurrent-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 8);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/create-concurrent-gate.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 8
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    outer_output="${DATA_DIR}/create-concurrent-outer.out"
    PGAPPNAME=lifecycle-create-concurrent-outer \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "CREATE INDEX lifecycle_create_concurrent_outer_idx
            ON public.lifecycle_create_concurrent_docs
            USING bm25(public.lifecycle_create_concurrent_pause(body_a))
            WITH (text_config = 'english',
                  compaction = 'background',
                  compaction_schedule = '0 0 1 1 *');" \
        >"${outer_output}" 2>&1 &
    outer_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-create-concurrent-outer'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "first CREATE pauses during its index build" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-create-concurrent-outer'
            AND wait_event = 'advisory';")"

    sql_as durable_owner -c "
        SET statement_timeout = '10s';
        CREATE INDEX lifecycle_create_concurrent_other_idx
          ON public.lifecycle_create_concurrent_docs USING bm25(body_b)
          WITH (text_config = 'english', compaction = 'manual');" \
        >/dev/null
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-create-concurrent-gate';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${outer_pid}" || outer_status=$?
    if [ "${outer_status}" -ne 0 ]; then
        error "tracked concurrent CREATE failed: $(cat "${outer_output}")"
    fi

    outer_oid="$(sql_super -c "SELECT
        'public.lifecycle_create_concurrent_outer_idx'::regclass::oid;")"
    concurrent_oid="$(sql_super -c "SELECT
        'public.lifecycle_create_concurrent_other_idx'::regclass::oid;")"
    assert_eq "concurrent CREATE retains its manual configuration" "t" \
        "$(sql_super -c "SELECT reloptions @> ARRAY['compaction=manual']
          FROM pg_catalog.pg_class WHERE oid = ${concurrent_oid};")"
    assert_eq "tracked CREATE does not absorb a concurrent index" "1:0" \
        "$(active_jobs_for_index "${outer_oid}"):$(
            active_jobs_for_index "${concurrent_oid}")"

    sql_super -c "DROP TABLE public.lifecycle_create_concurrent_docs;
                   DROP FUNCTION
                     public.lifecycle_create_concurrent_pause(text);"
}

test_create_tracking_table_rename() {
    local heap_oid outer_oid outer_output outer_pid outer_status=0
    local rename_output rename_pid rename_status=0 snapshot_pid

    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_create_rename_docs (body text);
INSERT INTO public.lifecycle_create_rename_docs
SELECT pg_catalog.format('document %s filler', value)
FROM pg_catalog.generate_series(1, 1000) AS value;
SQL
    heap_oid="$(sql_super -c "SELECT
        'public.lifecycle_create_rename_docs'::regclass::oid;")"

    PGAPPNAME=lifecycle-create-rename-snapshot sql_super -c \
        "BEGIN ISOLATION LEVEL REPEATABLE READ;
         SELECT count(*) FROM public.lifecycle_create_rename_docs;
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/create-rename-snapshot.out" 2>&1 &
    snapshot_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-create-rename-snapshot'
                AND wait_event = 'PgSleep';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    outer_output="${DATA_DIR}/create-rename-outer.out"
    PGAPPNAME=lifecycle-create-rename-outer \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "CREATE INDEX CONCURRENTLY lifecycle_create_rename_idx
            ON public.lifecycle_create_rename_docs USING bm25(body)
            WITH (text_config = 'english',
                  compaction = 'background',
                  compaction_schedule = '0 0 1 1 *');" \
        >"${outer_output}" 2>&1 &
    outer_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_progress_create_index AS progress
              JOIN pg_catalog.pg_stat_activity AS activity
                ON activity.pid = progress.pid
              WHERE activity.application_name =
                    'lifecycle-create-rename-outer'
                AND progress.phase = 'waiting for old snapshots';")" = "1" ];
        then
            break
        fi
        sleep 0.1
    done
    assert_eq "concurrent CREATE waits for the old snapshot" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_progress_create_index AS progress
          JOIN pg_catalog.pg_stat_activity AS activity
            ON activity.pid = progress.pid
          WHERE activity.application_name =
                'lifecycle-create-rename-outer'
            AND progress.phase = 'waiting for old snapshots';")"

    rename_output="${DATA_DIR}/create-rename-other.out"
    PGAPPNAME=lifecycle-create-rename-other \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "ALTER TABLE public.lifecycle_create_rename_docs
              RENAME TO lifecycle_create_renamed_docs;" \
        >"${rename_output}" 2>&1 &
    rename_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-create-rename-other'
                AND wait_event_type = 'Lock';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "table rename queues behind concurrent CREATE" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-create-rename-other'
            AND wait_event_type = 'Lock';")"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-create-rename-snapshot';" >/dev/null
    wait "${snapshot_pid}" || true
    wait "${outer_pid}" || outer_status=$?
    wait "${rename_pid}" || rename_status=$?
    if [ "${rename_status}" -ne 0 ]; then
        error "queued table rename failed: $(cat "${rename_output}")"
    fi
    if [ "${outer_status}" -ne 0 ]; then
        error "CREATE lost its original heap after rename: \
$(cat "${outer_output}")"
    fi

    outer_oid="$(sql_super -c "SELECT
        'public.lifecycle_create_rename_idx'::regclass::oid;")"
    assert_eq "tracked CREATE retains the original heap OID" \
        "${heap_oid}" \
        "$(sql_super -c "SELECT indrelid
          FROM pg_catalog.pg_index WHERE indexrelid = ${outer_oid};")"
    assert_eq "renamed-table CREATE activates its created index" "1" \
        "$(active_jobs_for_index "${outer_oid}")"

    sql_super -c "DROP TABLE public.lifecycle_create_renamed_docs;"
}

test_create_like_regenerates_lineage() {
    local clone_index clone_lineage create_output source_lineage

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_like_source (body text);
        CREATE INDEX lifecycle_like_source_idx
          ON public.lifecycle_like_source USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    source_lineage="$(index_lineage public.lifecycle_like_source_idx)"

    if ! create_output="$(sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_like_clone
          (LIKE public.lifecycle_like_source INCLUDING INDEXES);" 2>&1)"; then
        if ! grep -Fq \
            "background compaction lineage is already in use" \
            <<<"${create_output}"; then
            error "CREATE TABLE LIKE failed unexpectedly: ${create_output}"
        fi
        error "CREATE TABLE LIKE reused the source managed lineage"
    fi
    clone_index="$(sql_super -c "SELECT indexrelid::regclass::text
      FROM pg_catalog.pg_index
      WHERE indrelid = 'public.lifecycle_like_clone'::regclass;")"
    clone_lineage="$(index_lineage "${clone_index}")"
    if [ -z "${clone_lineage}" ] ||
        [ "${clone_lineage}" = "${source_lineage}" ]; then
        error "CREATE TABLE LIKE did not assign a fresh managed lineage"
    fi
    assert_eq "CREATE TABLE LIKE activates the cloned managed index" "1" \
        "$(active_jobs_for_index \
            "$(sql_super -c "SELECT '${clone_index}'::regclass::oid;")")"

    sql_super -c "DROP TABLE public.lifecycle_like_clone,
                             public.lifecycle_like_source;"
}

test_owner_reconciliation() {
    local alter_error alter_output dependencies_before index_oid job_before
    local job_after jobs_before filenumber_before filenumber_after
    local privileges_before

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_owner_docs (body text);
        CREATE INDEX lifecycle_owner_idx
          ON public.lifecycle_owner_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_owner_idx'::regclass::oid;")"
    filenumber_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${index_oid});")"
    job_before="$(current_generation_job_id "${index_oid}")"

    jobs_before="$(managed_job_count)"
    if ! alter_output="$(sql_super -c "
        ALTER INDEX public.lifecycle_owner_idx
          OWNER TO durable_nologin;" 2>&1)"; then
        error "ALTER INDEX OWNER no-op failed: ${alter_output}"
    fi
    if ! grep -Fq "cannot change owner of index" <<<"${alter_output}"; then
        error "ALTER INDEX OWNER did not preserve the core warning: \
${alter_output}"
    fi
    assert_eq "ALTER INDEX OWNER remains a no-op" "durable_owner" \
        "$(sql_super -c "SELECT pg_catalog.pg_get_userbyid(relowner)
                          FROM pg_catalog.pg_class
                          WHERE oid = ${index_oid};")"
    assert_eq "ALTER INDEX OWNER no-op creates no workflow" \
        "${jobs_before}" "$(managed_job_count)"

    if alter_error="$(sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_owner_docs
          OWNER TO durable_usage_only;" 2>&1)"; then
        error "ALTER TABLE OWNER bypassed role membership"
    fi
    if ! grep -Fq "must be able to SET ROLE \"durable_usage_only\"" \
        <<<"${alter_error}"; then
        error "ALTER TABLE OWNER did not preserve core role-membership \
ordering: ${alter_error}"
    fi

    sql_super -c \
        "ALTER TABLE public.lifecycle_owner_docs
           OWNER TO durable_owner_two;" \
        >/dev/null 2>&1
    filenumber_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${index_oid});")"
    job_after="$(current_generation_job_id "${index_oid}")"
    assert_eq "ALTER OWNER preserves the physical generation" \
        "${filenumber_before}" "${filenumber_after}"
    assert_eq "ALTER TABLE OWNER changes the physical index owner" \
        "durable_owner_two" \
        "$(sql_super -c "SELECT pg_catalog.pg_get_userbyid(relowner)
                          FROM pg_catalog.pg_class
                          WHERE oid = ${index_oid};")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "ALTER TABLE OWNER did not create a new-owner workflow"
    fi
    assert_eq "ALTER TABLE OWNER submits the replacement as the new owner" \
        "durable_owner_two" \
        "$(sql_super -c "SELECT submitted_by::pg_catalog.text
                          FROM df.instances
                          WHERE id = '${job_after}';")"

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_owner_reject_docs (body text);
        CREATE INDEX lifecycle_owner_reject_idx
          ON public.lifecycle_owner_reject_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    jobs_before="$(managed_job_count)"
    dependencies_before="$(dependency_count)"
    privileges_before="$(helper_privileges_for_role durable_nologin)"
    if alter_error="$(sql_as durable_writer -c "
        ALTER TABLE public.lifecycle_owner_reject_docs
          OWNER TO durable_nologin;" 2>&1)"; then
        error "unrelated writer changed background index ownership"
    fi
    if ! grep -Fq "must be owner of table lifecycle_owner_reject_docs" \
        <<<"${alter_error}"; then
        error "unauthorized ALTER OWNER bypassed core ownership checks: \
${alter_error}"
    fi
    assert_eq "unauthorized ALTER OWNER creates no workflow" \
        "${jobs_before}" "$(managed_job_count)"

    if alter_error="$(sql_super -c "
        ALTER TABLE public.lifecycle_owner_reject_docs
          OWNER TO durable_nologin;" 2>&1)"; then
        error "ALTER TABLE OWNER accepted an ineligible NOLOGIN owner"
    fi
    if ! grep -Fq \
        "index owner must have LOGIN for background compaction" \
        <<<"${alter_error}"; then
        error "ALTER TABLE OWNER did not preflight the new owner: \
${alter_error}"
    fi
    assert_eq "rejected ALTER TABLE OWNER preserves the old owner" \
        "durable_owner" \
        "$(sql_super -c "SELECT pg_catalog.pg_get_userbyid(relowner)
          FROM pg_catalog.pg_class
          WHERE oid = 'public.lifecycle_owner_reject_idx'::regclass;")"
    assert_eq "rejected ALTER TABLE OWNER creates no workflow" \
        "${jobs_before}" "$(managed_job_count)"
    assert_eq "rejected ALTER TABLE OWNER changes no dependency" \
        "${dependencies_before}" "$(dependency_count)"
    assert_eq "rejected ALTER TABLE OWNER changes no helper grants" \
        "${privileges_before}" \
        "$(helper_privileges_for_role durable_nologin)"

    sql_super -c "
        CREATE SCHEMA lifecycle_owner_private
          AUTHORIZATION durable_owner;
        GRANT durable_owner_two TO durable_owner;"
    sql_as durable_owner -c "
        CREATE TABLE lifecycle_owner_private.docs (body text);
        CREATE INDEX docs_idx
          ON lifecycle_owner_private.docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    if alter_error="$(sql_as durable_owner -c "
        ALTER TABLE lifecycle_owner_private.docs
          OWNER TO durable_owner_two;" 2>&1)"; then
        error "ALTER TABLE OWNER bypassed target schema CREATE"
    fi
    if ! grep -Fq "permission denied for schema lifecycle_owner_private" \
        <<<"${alter_error}"; then
        error "ALTER TABLE OWNER did not preserve core schema CREATE \
ordering: ${alter_error}"
    fi
    assert_eq "schema-rejected ALTER TABLE OWNER preserves the old owner" \
        "durable_owner" \
        "$(sql_super -c "SELECT pg_catalog.pg_get_userbyid(relowner)
          FROM pg_catalog.pg_class
          WHERE oid = 'lifecycle_owner_private.docs_idx'::regclass;")"
    sql_super -c "
        REVOKE durable_owner_two FROM durable_owner;
        DROP SCHEMA lifecycle_owner_private CASCADE;" >/dev/null

    sql_as durable_owner_two -c "SELECT df.cancel(
        '${job_after}', 'owner lifecycle test complete');" >/dev/null
    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'owner lifecycle test complete')
      FROM df.instances AS instance
      WHERE instance.label OPERATOR(pg_catalog.~~)
            'pg_textsearch:bg:v1:%:${index_oid}:%'
        AND instance.submitted_by OPERATOR(pg_catalog.=)
            'durable_owner'::regrole
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'owner lifecycle test complete')
      FROM df.instances AS instance
      WHERE instance.label OPERATOR(pg_catalog.~~)
            ('pg_textsearch:bg:v1:%:' ||
             'public.lifecycle_owner_reject_idx'::regclass::oid::text ||
             ':%')
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    sql_super -c \
        "DROP TABLE public.lifecycle_owner_docs,
                    public.lifecycle_owner_reject_docs;"
}

test_owner_change_preserves_captured_schedule() {
    local index_oid job_after

    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      SET pg_textsearch.background_compaction_schedule = '1 2 3 4 *';"
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_owner_schedule_docs (body text);
        CREATE INDEX lifecycle_owner_schedule_idx
          ON public.lifecycle_owner_schedule_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background');" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_owner_schedule_idx'::regclass::oid;")"

    sql_super -c "
        SET pg_textsearch.background_compaction_schedule = '5 6 7 8 *';
        ALTER TABLE public.lifecycle_owner_schedule_docs
          OWNER TO durable_owner_two;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    assert_eq "ALTER OWNER preserves the captured default schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${job_after}';")"

    sql_as durable_owner_two -c \
        "SELECT df.cancel('${job_after}', 'owner schedule test complete');" \
        >/dev/null
    sql_super -c "DROP TABLE public.lifecycle_owner_schedule_docs;"
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      RESET pg_textsearch.background_compaction_schedule;"
}

test_reassign_owned_reconciliation() {
    local create_output index_oid job_after job_before reassign_output

    sql_super -c "
        CREATE ROLE lifecycle_reassign_old LOGIN;
        CREATE ROLE lifecycle_reassign_new LOGIN;
        GRANT CONNECT ON DATABASE ${TEST_DB}
          TO lifecycle_reassign_old, lifecycle_reassign_new;
        GRANT USAGE, CREATE ON SCHEMA public TO lifecycle_reassign_old;
        GRANT USAGE ON SCHEMA public TO lifecycle_reassign_new;
        ALTER ROLE lifecycle_reassign_old IN DATABASE ${TEST_DB}
          SET pg_textsearch.background_compaction_schedule = '1 2 3 4 *';
        SELECT df.grant_usage('lifecycle_reassign_old');
        SELECT df.grant_usage('lifecycle_reassign_new');" >/dev/null
    if ! create_output="$(sql_as lifecycle_reassign_old -c "
        CREATE TABLE public.lifecycle_reassign_docs (body text);
        CREATE INDEX lifecycle_reassign_idx
          ON public.lifecycle_reassign_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background');" 2>&1)"; then
        error "REASSIGN OWNED setup failed: ${create_output}"
    fi
    index_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_reassign_idx'::regclass::oid;")"
    job_before="$(current_generation_job_id "${index_oid}")"

    if ! reassign_output="$(sql_super -c "
        SET pg_textsearch.background_compaction_schedule = '5 6 7 8 *';
        REASSIGN OWNED BY lifecycle_reassign_old
          TO lifecycle_reassign_new;" 2>&1)"; then
        error "REASSIGN OWNED failed: ${reassign_output}"
    fi
    job_after="$(current_generation_job_id "${index_oid}")"
    assert_eq "REASSIGN OWNED changes the physical index owner" \
        "lifecycle_reassign_new" \
        "$(sql_super -c "SELECT pg_catalog.pg_get_userbyid(relowner)
          FROM pg_catalog.pg_class WHERE oid = ${index_oid};")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "REASSIGN OWNED did not create a new-owner workflow"
    fi
    assert_eq "REASSIGN OWNED submits the replacement as the new owner" \
        "lifecycle_reassign_new" \
        "$(sql_super -c "SELECT submitted_by::pg_catalog.text
          FROM df.instances WHERE id = '${job_after}';")"
    assert_eq "REASSIGN OWNED preserves the captured default schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${job_after}';")"

    sql_as lifecycle_reassign_new -c "
        INSERT INTO public.lifecycle_reassign_docs
        SELECT pg_catalog.format('reassigned document %s', value)
        FROM pg_catalog.generate_series(1, 20) AS value;
        SELECT bm25_spill_index('public.lifecycle_reassign_idx');" \
        >/dev/null 2>&1
    assert_eq "reassigned owner can signal the current workflow" "1" \
        "$(current_generation_job_count "${index_oid}")"

    sql_as lifecycle_reassign_new -c \
        "SELECT df.cancel('${job_after}', 'reassign lifecycle complete');" \
        >/dev/null
    sql_super -c "
        DROP OWNED BY lifecycle_reassign_old;
        DROP OWNED BY lifecycle_reassign_new CASCADE;
        DROP ROLE lifecycle_reassign_old;
        DROP ROLE lifecycle_reassign_new;" >/dev/null
}

test_reindex_reconciliation() {
    local a_before a_after a_table_after b_before b_after
    local a_file_before a_file_after a_table_file_before a_table_file_after
    local b_file_before b_file_after a_oid b_oid

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_reindex_docs
          (body_a text, body_b text);
        CREATE INDEX lifecycle_reindex_a_idx
          ON public.lifecycle_reindex_docs USING bm25(body_a)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    sql_as durable_owner -c "
        CREATE INDEX lifecycle_reindex_b_idx
          ON public.lifecycle_reindex_docs USING bm25(body_b)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    a_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_reindex_a_idx'::regclass::oid;")"
    b_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_reindex_b_idx'::regclass::oid;")"
    a_before="$(current_generation_job_id "${a_oid}")"
    b_before="$(current_generation_job_id "${b_oid}")"
    a_file_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${a_oid});")"

    sql_as durable_owner -c \
        "REINDEX INDEX public.lifecycle_reindex_a_idx;" >/dev/null 2>&1
    a_file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${a_oid});")"
    a_after="$(current_generation_job_id "${a_oid}")"
    if [ "${a_file_after}" = "${a_file_before}" ]; then
        error "REINDEX INDEX did not replace the physical generation"
    fi
    if [ -z "${a_after}" ] || [ "${a_after}" = "${a_before}" ]; then
        error "REINDEX INDEX did not reconcile a replacement workflow"
    fi

    a_table_after="${a_after}"
    a_table_file_before="${a_file_after}"
    b_file_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${b_oid});")"
    sql_as durable_owner -c \
        "REINDEX TABLE public.lifecycle_reindex_docs;" >/dev/null 2>&1
    a_table_file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${a_oid});")"
    b_file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${b_oid});")"
    a_table_after="$(current_generation_job_id "${a_oid}")"
    b_after="$(current_generation_job_id "${b_oid}")"
    if [ "${a_table_file_after}" = "${a_table_file_before}" ] ||
        [ "${b_file_after}" = "${b_file_before}" ]; then
        error "REINDEX TABLE did not replace every physical generation"
    fi
    if [ -z "${a_table_after}" ] ||
        [ "${a_table_after}" = "${a_after}" ] ||
        [ -z "${b_after}" ] || [ "${b_after}" = "${b_before}" ]; then
        error "REINDEX TABLE did not reconcile every background index"
    fi
    assert_eq "REINDEX TABLE selects one current workflow per index" "2" \
        "$(( $(current_generation_job_count "${a_oid}") +
             $(current_generation_job_count "${b_oid}") ))"

    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'reindex lifecycle test complete')
      FROM df.instances AS instance
      WHERE (instance.label OPERATOR(pg_catalog.~~)
               'pg_textsearch:bg:v1:%:${a_oid}:%'
             OR instance.label OPERATOR(pg_catalog.~~)
               'pg_textsearch:bg:v1:%:${b_oid}:%')
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    sql_as durable_owner -c "DROP TABLE public.lifecycle_reindex_docs;"
}

test_plain_reindex_preserves_captured_schedule() {
    local current_after current_before current_oid
    local legacy_after legacy_before legacy_lineage legacy_oid

    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      SET pg_textsearch.background_compaction_schedule = '1 2 3 4 *';"
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_reindex_schedule_index_docs
          (body text);
        INSERT INTO public.lifecycle_reindex_schedule_index_docs
          VALUES ('current');
        CREATE INDEX lifecycle_reindex_schedule_index_idx
          ON public.lifecycle_reindex_schedule_index_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background');

        CREATE TABLE public.lifecycle_reindex_schedule_table_docs
          (body text);
        INSERT INTO public.lifecycle_reindex_schedule_table_docs
          VALUES ('legacy');
        CREATE INDEX lifecycle_reindex_schedule_table_idx
          ON public.lifecycle_reindex_schedule_table_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background');" >/dev/null 2>&1
    current_oid="$(sql_super -c "SELECT
        'public.lifecycle_reindex_schedule_index_idx'::regclass::oid;")"
    legacy_oid="$(sql_super -c "SELECT
        'public.lifecycle_reindex_schedule_table_idx'::regclass::oid;")"
    current_before="$(current_generation_job_id "${current_oid}")"
    legacy_before="$(current_generation_job_id "${legacy_oid}")"

    sql_super -c "UPDATE df.instances AS instance
      SET label = pg_catalog.format(
        'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%s',
        database.oid,
        relation.oid,
        coalesce(nullif(relation.reltablespace, 0),
                 database.dattablespace),
        pg_catalog.pg_relation_filenode(relation.oid),
        relation.relowner,
        pg_catalog.encode(
          pg_catalog.convert_to('1 2 3 4 *', 'UTF8'), 'hex'))
      FROM pg_catalog.pg_class AS relation,
           pg_catalog.pg_database AS database
      WHERE instance.id = '${legacy_before}'
        AND relation.oid = ${legacy_oid}
        AND database.datname = pg_catalog.current_database();"
    remove_index_lineage public.lifecycle_reindex_schedule_table_idx

    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      SET pg_textsearch.background_compaction_schedule = '5 6 7 8 *';"
    sql_as durable_owner -c "
        REINDEX INDEX public.lifecycle_reindex_schedule_index_idx;" \
        >/dev/null 2>&1
    current_after="$(current_generation_job_id "${current_oid}")"
    if [ -z "${current_after}" ] ||
       [ "${current_after}" = "${current_before}" ]; then
        error "plain REINDEX INDEX created no replacement workflow"
    fi
    assert_eq "plain REINDEX INDEX preserves the captured schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${current_after}';")"

    sql_as durable_owner -c "
        REINDEX TABLE public.lifecycle_reindex_schedule_table_docs;" \
        >/dev/null 2>&1
    legacy_after="$(current_generation_job_id "${legacy_oid}")"
    if [ -z "${legacy_after}" ] ||
       [ "${legacy_after}" = "${legacy_before}" ]; then
        error "plain REINDEX TABLE created no replacement workflow"
    fi
    legacy_lineage="$(
        index_lineage public.lifecycle_reindex_schedule_table_idx
    )"
    assert_eq "plain REINDEX TABLE backfills legacy lineage" "32" \
        "${#legacy_lineage}"
    assert_eq "plain REINDEX TABLE preserves the legacy schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${legacy_after}';")"

    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      RESET pg_textsearch.background_compaction_schedule;"
    sql_super -c "
        DROP TABLE public.lifecycle_reindex_schedule_index_docs,
                   public.lifecycle_reindex_schedule_table_docs;"
}

test_physical_rewrite_reconciliation() {
    local excluded_oid external_oid file_after file_before index_oid
    local job_after job_before
    local lock_error lock_pid tablespace_dir

    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      SET pg_textsearch.background_compaction_schedule = '1 2 3 4 *';"
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_rewrite_docs
          (id integer, body text);
        INSERT INTO public.lifecycle_rewrite_docs
          SELECT value, pg_catalog.format('document %s', value)
          FROM pg_catalog.generate_series(1, 100) AS value;
        CREATE INDEX lifecycle_rewrite_cluster_idx
          ON public.lifecycle_rewrite_docs(id);
        CREATE INDEX lifecycle_rewrite_idx
          ON public.lifecycle_rewrite_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background');" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_rewrite_idx'::regclass::oid;")"
    file_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${index_oid});")"
    job_before="$(current_generation_job_id "${index_oid}")"
    sql_super -c "ALTER ROLE postgres IN DATABASE ${TEST_DB}
      SET pg_textsearch.background_compaction_schedule = '5 6 7 8 *';"

    sql_super -c \
        "VACUUM (FULL) public.lifecycle_rewrite_docs;" >/dev/null 2>&1
    file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${index_oid});")"
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ "${file_after}" = "${file_before}" ]; then
        error "VACUUM FULL did not replace the BM25 physical generation"
    fi
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "VACUUM FULL did not reconcile the replacement workflow"
    fi

    file_before="${file_after}"
    job_before="${job_after}"
    sql_super -c "CLUSTER public.lifecycle_rewrite_docs
      USING lifecycle_rewrite_cluster_idx;" >/dev/null 2>&1
    file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${index_oid});")"
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ "${file_after}" = "${file_before}" ]; then
        error "CLUSTER did not replace the BM25 physical generation"
    fi
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "CLUSTER did not reconcile the replacement workflow"
    fi

    tablespace_dir="${DATA_DIR}-lifecycle-rewrite-tablespace"
    mkdir -p "${tablespace_dir}"
    sql_super -c "
        CREATE TABLESPACE lifecycle_rewrite_tablespace
          OWNER durable_owner LOCATION '${tablespace_dir}';" >/dev/null
    job_before="${job_after}"
    sql_super -c "
        ALTER INDEX public.lifecycle_rewrite_idx
          SET TABLESPACE lifecycle_rewrite_tablespace;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "ALTER INDEX SET TABLESPACE did not reconcile the workflow"
    fi

    job_before="${job_after}"
    sql_super -c "
        ALTER INDEX ALL IN TABLESPACE lifecycle_rewrite_tablespace
          OWNED BY durable_owner SET TABLESPACE pg_default;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "ALTER INDEX ALL IN TABLESPACE did not reconcile the workflow"
    fi

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_rewrite_external (body text);
        CREATE INDEX lifecycle_rewrite_external_idx
          ON public.lifecycle_rewrite_external USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *')
          TABLESPACE lifecycle_rewrite_tablespace;" \
        >/dev/null 2>&1
    external_oid="$(sql_super -c "SELECT
        'public.lifecycle_rewrite_external_idx'::regclass::oid;")"
    remove_index_lineage public.lifecycle_rewrite_external_idx

    sql_as durable_owner_two -c "
        CREATE TABLE public.lifecycle_rewrite_excluded (body text);
        CREATE INDEX lifecycle_rewrite_excluded_idx
          ON public.lifecycle_rewrite_excluded USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    excluded_oid="$(sql_super -c "SELECT
        'public.lifecycle_rewrite_excluded_idx'::regclass::oid;")"
    remove_index_lineage public.lifecycle_rewrite_excluded_idx

    PGAPPNAME=lifecycle-tablespace-nowait \
        sql_as durable_owner -c "
        BEGIN;
        ALTER INDEX public.lifecycle_rewrite_idx
          SET (compaction_schedule = '1 2 3 4 *');
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/tablespace-nowait-lock.out" 2>&1 &
    lock_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_locks
              WHERE relation = ${index_oid}
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    if lock_error="$(sql_super -c "
        SET statement_timeout = '2s';
        ALTER INDEX ALL IN TABLESPACE pg_default
          OWNED BY durable_owner SET TABLESPACE
          lifecycle_rewrite_tablespace NOWAIT;" 2>&1)"; then
        error "ALTER INDEX ALL NOWAIT unexpectedly moved a locked index"
    fi
    if grep -Fq "canceling statement due to statement timeout" \
        <<<"${lock_error}"; then
        error "ALTER INDEX ALL NOWAIT blocked behind an extension prelock"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-tablespace-nowait';" >/dev/null
    wait "${lock_pid}" || true

    job_before="${job_after}"
    sql_super -c "
        ALTER INDEX ALL IN TABLESPACE pg_default
          OWNED BY durable_owner SET TABLESPACE
          lifecycle_rewrite_tablespace;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "pg_default bulk move did not reconcile the workflow"
    fi
    assert_eq "OWNED BY leaves excluded legacy lineage untouched" "" \
        "$(index_lineage "${excluded_oid}")"
    assert_eq "pg_default move leaves another tablespace untouched" "" \
        "$(index_lineage "${external_oid}")"
    assert_eq "physical rewrites preserve the captured default schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${job_after}';")"

    sql_as durable_owner -c \
        "SELECT df.cancel('${job_after}', 'rewrite lifecycle test complete');" \
        >/dev/null
    sql_super -c "DROP TABLE public.lifecycle_rewrite_docs,
                             public.lifecycle_rewrite_excluded,
                             public.lifecycle_rewrite_external;"
    sql_super -c "DROP TABLESPACE lifecycle_rewrite_tablespace;"
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      RESET pg_textsearch.background_compaction_schedule;"
    sql_super -c "ALTER ROLE postgres IN DATABASE ${TEST_DB}
      RESET pg_textsearch.background_compaction_schedule;"
}

test_database_owner_vacuum_full() {
    local file_after file_before index_oid job_after job_before
    local vacuum_output

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_database_vacuum_docs (body text);
INSERT INTO public.lifecycle_database_vacuum_docs VALUES ('alpha');
CREATE INDEX lifecycle_database_vacuum_idx
    ON public.lifecycle_database_vacuum_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_database_vacuum_idx'::regclass::oid;")"
    file_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${index_oid});")"
    job_before="$(current_generation_job_id "${index_oid}")"

    sql_super -c "GRANT USAGE ON SCHEMA public TO durable_actor;
                   ALTER DATABASE ${TEST_DB} OWNER TO durable_actor;" \
        >/dev/null
    if ! vacuum_output="$(sql_as durable_actor -c \
        "VACUUM (FULL) public.lifecycle_database_vacuum_docs;" 2>&1)"; then
        sql_super -c "ALTER DATABASE ${TEST_DB} OWNER TO postgres;" \
            >/dev/null
        error "database-owner VACUUM FULL failed: ${vacuum_output}"
    fi
    sql_super -c "ALTER DATABASE ${TEST_DB} OWNER TO postgres;" >/dev/null

    file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${index_oid});")"
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ "${file_after}" = "${file_before}" ]; then
        error "database-owner VACUUM FULL did not rewrite the index"
    fi
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "database-owner VACUUM FULL did not reconcile the workflow"
    fi
    log "PASS: database-owner VACUUM FULL reconciles managed indexes"

    sql_super -c \
        "DROP TABLE public.lifecycle_database_vacuum_docs;" >/dev/null
}

test_vacuum_full_skip_locked() {
    local blocker_pid index_oid job_before vacuum_output

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_skip_locked_docs (body text);
INSERT INTO public.lifecycle_skip_locked_docs VALUES ('alpha');
CREATE INDEX lifecycle_skip_locked_idx
    ON public.lifecycle_skip_locked_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_skip_locked_idx'::regclass::oid;")"
    job_before="$(current_generation_job_id "${index_oid}")"

    PGAPPNAME=lifecycle-vacuum-skip-lock \
        sql_as durable_owner -c "
        BEGIN;
        LOCK TABLE public.lifecycle_skip_locked_docs
          IN ACCESS EXCLUSIVE MODE;
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/vacuum-skip-lock.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_skip_locked_docs'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    if ! vacuum_output="$(PGOPTIONS='-c statement_timeout=2s' \
        sql_as durable_owner -c \
        "VACUUM (FULL, SKIP_LOCKED)
           public.lifecycle_skip_locked_docs;" 2>&1)"; then
        kill "${blocker_pid}" 2>/dev/null || true
        wait "${blocker_pid}" 2>/dev/null || true
        error "VACUUM FULL SKIP_LOCKED blocked or failed: ${vacuum_output}"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-vacuum-skip-lock';" >/dev/null
    wait "${blocker_pid}" 2>/dev/null || true
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_skip_locked_docs'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "0" ]; then
            break
        fi
        sleep 0.1
    done

    assert_eq "VACUUM FULL SKIP_LOCKED leaves the workflow unchanged" \
        "${job_before}" "$(current_generation_job_id "${index_oid}")"
    log "PASS: VACUUM FULL SKIP_LOCKED preserves core lock skipping"

    if ! vacuum_output="$(sql_as durable_owner -c \
        "VACUUM (FULL, SKIP_LOCKED)
           public.lifecycle_skip_locked_docs;" 2>&1)"; then
        error "unlocked VACUUM FULL SKIP_LOCKED failed: ${vacuum_output}"
    fi
    if [ "$(current_generation_job_id "${index_oid}")" = "${job_before}" ]; then
        error "unlocked VACUUM FULL SKIP_LOCKED did not reconcile the workflow"
    fi
    log "PASS: unlocked VACUUM FULL SKIP_LOCKED reconciles the workflow"

    sql_super -c \
        "DROP TABLE public.lifecycle_skip_locked_docs;" >/dev/null
}

test_partition_vacuum_child_authorization() {
    local blocker_pid child_index child_job_before low_index low_job_after
    local low_job_before only_output server_version vacuum_output vacuum_pid
    local vacuum_status=0

    server_version="$(sql_super -c \
        "SELECT pg_catalog.current_setting('server_version_num')::integer;")"
    if [ "${server_version}" -lt 180000 ]; then
        log "PASS: partition-child VACUUM authorization is PG18-only"
        return
    fi

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE FUNCTION public.lifecycle_vacuum_auth_pause(value text)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
AS $body$
BEGIN
    PERFORM pg_catalog.pg_advisory_lock(478, 11);
    PERFORM pg_catalog.pg_advisory_unlock(478, 11);
    RETURN value;
END
$body$;
CREATE TABLE public.lifecycle_vacuum_auth_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_vacuum_auth_low
    PARTITION OF public.lifecycle_vacuum_auth_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_vacuum_auth_high
    PARTITION OF public.lifecycle_vacuum_auth_docs
    FOR VALUES FROM (100) TO (200);
CREATE INDEX lifecycle_vacuum_auth_idx
    ON public.lifecycle_vacuum_auth_docs
    USING bm25(public.lifecycle_vacuum_auth_pause(body))
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
INSERT INTO public.lifecycle_vacuum_auth_low VALUES (1, 'alpha');
SQL
    sql_super -c "
        ALTER TABLE public.lifecycle_vacuum_auth_high
          OWNER TO durable_owner_two;
        GRANT MAINTAIN ON public.lifecycle_vacuum_auth_low
          TO durable_writer;" >/dev/null
    child_index="$(sql_super -c "
        SELECT child_index.oid::regclass::text
        FROM pg_catalog.pg_inherits AS inheritance
        JOIN pg_catalog.pg_class AS child_index
          ON child_index.oid = inheritance.inhrelid
        JOIN pg_catalog.pg_index AS index_info
          ON index_info.indexrelid = child_index.oid
        WHERE inheritance.inhparent =
              'public.lifecycle_vacuum_auth_idx'::regclass
          AND index_info.indrelid =
              'public.lifecycle_vacuum_auth_high'::regclass;")"
    low_index="$(sql_super -c "
        SELECT child_index.oid::regclass::text
        FROM pg_catalog.pg_inherits AS inheritance
        JOIN pg_catalog.pg_class AS child_index
          ON child_index.oid = inheritance.inhrelid
        JOIN pg_catalog.pg_index AS index_info
          ON index_info.indexrelid = child_index.oid
        WHERE inheritance.inhparent =
              'public.lifecycle_vacuum_auth_idx'::regclass
          AND index_info.indrelid =
              'public.lifecycle_vacuum_auth_low'::regclass;")"
    child_job_before="$(current_generation_job_id \
        "$(sql_super -c "SELECT '${child_index}'::regclass::oid;")")"
    low_job_before="$(current_generation_job_id \
        "$(sql_super -c "SELECT '${low_index}'::regclass::oid;")")"

    PGAPPNAME=lifecycle-vacuum-auth-gate \
        sql_super -c "
        SELECT pg_catalog.pg_advisory_lock(478, 11);
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/vacuum-auth-gate.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 11
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    vacuum_output="${DATA_DIR}/vacuum-auth-child.out"
    PGAPPNAME=lifecycle-vacuum-auth-child \
        sql_as durable_writer -c "
        VACUUM (FULL) public.lifecycle_vacuum_auth_docs;" \
        >"${vacuum_output}" 2>&1 &
    vacuum_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-vacuum-auth-child'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "authorized child VACUUM reaches the rewrite gate" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-vacuum-auth-child'
            AND wait_event = 'advisory';")"
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-vacuum-auth-gate';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${vacuum_pid}" || vacuum_status=$?
    if [ "${vacuum_status}" -ne 0 ]; then
        error "authorized child VACUUM failed: $(cat "${vacuum_output}")"
    fi
    low_job_after="$(current_generation_job_id \
        "$(sql_super -c "SELECT '${low_index}'::regclass::oid;")")"
    if [ -z "${low_job_after}" ] ||
        [ "${low_job_after}" = "${low_job_before}" ]; then
        error "VACUUM did not reconcile an authorized child of an \
unauthorized root"
    fi
    assert_eq "VACUUM leaves an unauthorized child workflow unchanged" \
        "${child_job_before}" \
        "$(current_generation_job_id \
          "$(sql_super -c "SELECT '${child_index}'::regclass::oid;")")"

    sql_super -c "GRANT MAINTAIN ON public.lifecycle_vacuum_auth_docs
                   TO durable_writer;" >/dev/null
    PGAPPNAME=lifecycle-vacuum-auth-only-lock \
        sql_as durable_owner_two -c "
        BEGIN;
        ALTER INDEX ${child_index}
          SET (compaction_schedule = '2 3 4 5 *');
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/vacuum-auth-only-lock.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE relation = '${child_index}'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    if ! only_output="$(PGOPTIONS='-c statement_timeout=2s' \
        sql_as durable_writer -c "
        VACUUM (FULL) ONLY public.lifecycle_vacuum_auth_docs;" 2>&1)"; then
        error "VACUUM ONLY prelocked a child index: ${only_output}"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-vacuum-auth-only-lock';" \
        >/dev/null
    wait "${blocker_pid}" || true

    sql_super -c "DROP TABLE public.lifecycle_vacuum_auth_docs;
                   DROP FUNCTION public.lifecycle_vacuum_auth_pause(text);" \
        >/dev/null
}

test_global_cluster_scope() {
    local blocker_pid clustered_file_before clustered_index_oid
    local clustered_job_before cluster_output unclustered_index_oid
    local unclustered_job_before

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_global_cluster_docs
    (id integer, body text);
INSERT INTO public.lifecycle_global_cluster_docs VALUES (1, 'alpha');
CREATE INDEX lifecycle_global_cluster_order_idx
    ON public.lifecycle_global_cluster_docs(id);
CREATE INDEX lifecycle_global_cluster_idx
    ON public.lifecycle_global_cluster_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CLUSTER public.lifecycle_global_cluster_docs
    USING lifecycle_global_cluster_order_idx;

CREATE TABLE public.lifecycle_global_unclustered_docs (body text);
CREATE INDEX lifecycle_global_unclustered_idx
    ON public.lifecycle_global_unclustered_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    clustered_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_global_cluster_idx'::regclass::oid;")"
    unclustered_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_global_unclustered_idx'::regclass::oid;")"
    clustered_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${clustered_index_oid});")"
    clustered_job_before="$(
        current_generation_job_id "${clustered_index_oid}"
    )"
    unclustered_job_before="$(
        current_generation_job_id "${unclustered_index_oid}"
    )"

    PGAPPNAME=lifecycle-global-cluster-lock \
        sql_as durable_owner -c "
        BEGIN;
        ALTER INDEX public.lifecycle_global_unclustered_idx
          RENAME TO lifecycle_global_unclustered_locked_idx;
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/global-cluster-lock.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_locks
              WHERE relation = ${unclustered_index_oid}
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    if ! cluster_output="$(PGOPTIONS='-c statement_timeout=2s' \
        sql_as durable_owner -c "CLUSTER;" 2>&1)"; then
        sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-global-cluster-lock';" \
            >/dev/null
        wait "${blocker_pid}" || true
        error "global CLUSTER touched an unclustered heap: ${cluster_output}"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-global-cluster-lock';" >/dev/null
    wait "${blocker_pid}" || true

    if [ "$(sql_super -c "SELECT
              pg_catalog.pg_relation_filenode(${clustered_index_oid});")" = \
         "${clustered_file_before}" ]; then
        error "global CLUSTER did not rewrite the clustered managed index"
    fi
    if [ "$(current_generation_job_id "${clustered_index_oid}")" = \
         "${clustered_job_before}" ]; then
        error "global CLUSTER did not reconcile the clustered workflow"
    fi
    assert_eq "global CLUSTER ignores unclustered managed indexes" \
        "${unclustered_job_before}" \
        "$(current_generation_job_id "${unclustered_index_oid}")"

    sql_super -c "DROP TABLE public.lifecycle_global_cluster_docs,
                             public.lifecycle_global_unclustered_docs;" \
        >/dev/null
}

test_global_cluster_concurrent_mode_change() {
    local blocker_pid cluster_output cluster_pid cluster_status=0
    local file_after file_before index_oid job_after

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_global_cluster_mode_blocker
    (id integer, body text);
INSERT INTO public.lifecycle_global_cluster_mode_blocker
VALUES (1, 'blocker');
CREATE INDEX lifecycle_global_cluster_mode_blocker_order_idx
    ON public.lifecycle_global_cluster_mode_blocker(id);
CREATE INDEX lifecycle_global_cluster_mode_blocker_idx
    ON public.lifecycle_global_cluster_mode_blocker USING bm25(body)
    WITH (text_config = 'english', compaction = 'inline');
CLUSTER public.lifecycle_global_cluster_mode_blocker
    USING lifecycle_global_cluster_mode_blocker_order_idx;

CREATE TABLE public.lifecycle_global_cluster_mode_target
    (id integer, body text);
INSERT INTO public.lifecycle_global_cluster_mode_target
VALUES (1, 'target');
CREATE INDEX lifecycle_global_cluster_mode_target_order_idx
    ON public.lifecycle_global_cluster_mode_target(id);
CREATE INDEX lifecycle_global_cluster_mode_target_idx
    ON public.lifecycle_global_cluster_mode_target USING bm25(body)
    WITH (text_config = 'english', compaction = 'inline');
CLUSTER public.lifecycle_global_cluster_mode_target
    USING lifecycle_global_cluster_mode_target_order_idx;
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_global_cluster_mode_target_idx'::regclass::oid;")"
    file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${index_oid});")"

    PGAPPNAME=lifecycle-global-cluster-mode-blocker \
        sql_as durable_owner -c "
        BEGIN;
        ALTER INDEX public.lifecycle_global_cluster_mode_blocker_idx
          SET (compaction = 'inline');
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/global-cluster-mode-blocker.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_global_cluster_mode_blocker_idx'
                      ::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    cluster_output="${DATA_DIR}/global-cluster-mode.out"
    PGAPPNAME=lifecycle-global-cluster-mode \
        PGOPTIONS="-c statement_timeout=30s" \
        sql_as durable_owner -c "CLUSTER;" \
        >"${cluster_output}" 2>&1 &
    cluster_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS relation_lock
                ON relation_lock.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-global-cluster-mode'
                AND relation_lock.relation =
                    'public.lifecycle_global_cluster_mode_blocker_idx'
                      ::regclass
                AND NOT relation_lock.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "global CLUSTER reached its candidate-discovery wait" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS relation_lock
            ON relation_lock.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-global-cluster-mode'
            AND relation_lock.relation =
                'public.lifecycle_global_cluster_mode_blocker_idx'
                  ::regclass
            AND NOT relation_lock.granted;")"

    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_global_cluster_mode_target_idx
          SET (compaction = 'background',
               compaction_schedule = '1 2 3 4 *');" >/dev/null
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-global-cluster-mode-blocker';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${cluster_pid}" || cluster_status=$?
    if [ "${cluster_status}" -ne 0 ]; then
        error "global CLUSTER with a concurrent mode change failed:
$(cat "${cluster_output}")"
    fi

    file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${index_oid});")"
    assert_ne "global CLUSTER rebuilt the newly managed index" \
        "${file_before}" "${file_after}"
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ]; then
        error "global CLUSTER lost a concurrent background mode change"
    fi
    log "PASS: global CLUSTER tracks a concurrent background mode change"

    sql_super -c "
        DROP TABLE public.lifecycle_global_cluster_mode_target,
                   public.lifecycle_global_cluster_mode_blocker;" >/dev/null
}

test_inheritance_vacuum_full_scope() {
    local child_file_after child_file_before child_index_oid
    local child_job_after child_job_before parent_file_after
    local parent_file_before parent_index_oid parent_job_after
    local parent_job_before
    local server_version_num

    server_version_num="$(sql_super -c \
        "SHOW server_version_num;")"
    if [ "${server_version_num}" -lt 180000 ]; then
        log "PASS: recursive ordinary-inheritance VACUUM is PostgreSQL 18+"
        return
    fi

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_vacuum_parent (body text);
CREATE TABLE public.lifecycle_vacuum_child ()
    INHERITS (public.lifecycle_vacuum_parent);
INSERT INTO public.lifecycle_vacuum_parent VALUES ('parent');
INSERT INTO public.lifecycle_vacuum_child VALUES ('child');
CREATE INDEX lifecycle_vacuum_parent_idx
    ON public.lifecycle_vacuum_parent USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE INDEX lifecycle_vacuum_child_idx
    ON public.lifecycle_vacuum_child USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    parent_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_vacuum_parent_idx'::regclass::oid;")"
    child_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_vacuum_child_idx'::regclass::oid;")"
    parent_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    child_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${child_index_oid});")"
    parent_job_before="$(current_generation_job_id "${parent_index_oid}")"
    child_job_before="$(current_generation_job_id "${child_index_oid}")"

    sql_as durable_owner -c \
        "VACUUM (FULL) public.lifecycle_vacuum_parent;" >/dev/null 2>&1
    parent_file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    child_file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${child_index_oid});")"
    parent_job_after="$(current_generation_job_id "${parent_index_oid}")"
    child_job_after="$(current_generation_job_id "${child_index_oid}")"
    if [ "${parent_file_after}" = "${parent_file_before}" ] ||
       [ "${child_file_after}" = "${child_file_before}" ]; then
        error "recursive VACUUM FULL did not rewrite parent and child indexes"
    fi
    if [ "${parent_job_after}" = "${parent_job_before}" ] ||
       [ "${child_job_after}" = "${child_job_before}" ]; then
        error "recursive VACUUM FULL did not reconcile parent and child"
    fi

    parent_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    child_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${child_index_oid});")"
    child_job_before="$(current_generation_job_id "${child_index_oid}")"
    sql_as durable_owner -c \
        "VACUUM (FULL) ONLY public.lifecycle_vacuum_parent;" >/dev/null 2>&1
    parent_file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    if [ "${parent_file_after}" = "${parent_file_before}" ]; then
        error "VACUUM FULL ONLY did not rewrite the parent index"
    fi
    assert_eq "VACUUM FULL ONLY leaves child storage unchanged" \
        "${child_file_before}" \
        "$(sql_super -c "SELECT
          pg_catalog.pg_relation_filenode(${child_index_oid});")"
    assert_eq "VACUUM FULL ONLY leaves child workflow unchanged" \
        "${child_job_before}" \
        "$(current_generation_job_id "${child_index_oid}")"

    sql_super -c "DROP TABLE public.lifecycle_vacuum_child,
                             public.lifecycle_vacuum_parent;" >/dev/null
}

test_inheritance_alter_rewrite_scope() {
    local child_file_after child_file_before child_index_oid
    local child_job_after child_job_before parent_file_after
    local parent_file_before parent_index_oid parent_job_after
    local parent_job_before

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_alter_parent
    (marker integer, body text);
CREATE TABLE public.lifecycle_alter_child ()
    INHERITS (public.lifecycle_alter_parent);
INSERT INTO public.lifecycle_alter_parent VALUES (1, 'parent');
INSERT INTO public.lifecycle_alter_child VALUES (2, 'child');
CREATE INDEX lifecycle_alter_parent_idx
    ON public.lifecycle_alter_parent USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE INDEX lifecycle_alter_child_idx
    ON public.lifecycle_alter_child USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    parent_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_alter_parent_idx'::regclass::oid;")"
    child_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_alter_child_idx'::regclass::oid;")"
    parent_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    child_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${child_index_oid});")"
    parent_job_before="$(current_generation_job_id "${parent_index_oid}")"
    child_job_before="$(current_generation_job_id "${child_index_oid}")"

    sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_alter_parent
          ALTER COLUMN marker TYPE bigint
          USING marker::pg_catalog.int8;" >/dev/null 2>&1
    parent_file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    child_file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${child_index_oid});")"
    parent_job_after="$(current_generation_job_id "${parent_index_oid}")"
    child_job_after="$(current_generation_job_id "${child_index_oid}")"
    if [ "${parent_file_after}" = "${parent_file_before}" ] ||
       [ "${child_file_after}" = "${child_file_before}" ]; then
        error "recursive ALTER TABLE did not rewrite parent and child indexes"
    fi
    if [ -z "${parent_job_after}" ] ||
       [ "${parent_job_after}" = "${parent_job_before}" ]; then
        error "recursive ALTER TABLE did not reconcile parent"
    fi
    if [ -z "${child_job_after}" ] ||
       [ "${child_job_after}" = "${child_job_before}" ]; then
        error "recursive ALTER TABLE did not reconcile child"
    fi

    parent_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    child_file_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${child_index_oid});")"
    child_job_before="$(current_generation_job_id "${child_index_oid}")"
    sql_as durable_owner -c "
        ALTER TABLE ONLY public.lifecycle_alter_parent
          SET UNLOGGED;" >/dev/null 2>&1
    parent_file_after="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    if [ "${parent_file_after}" = "${parent_file_before}" ]; then
        error "ALTER TABLE ONLY did not rewrite the parent index"
    fi
    assert_eq "ALTER TABLE ONLY leaves child storage unchanged" \
        "${child_file_before}" \
        "$(sql_super -c "SELECT
          pg_catalog.pg_relation_filenode(${child_index_oid});")"
    assert_eq "ALTER TABLE ONLY leaves child workflow unchanged" \
        "${child_job_before}" \
        "$(current_generation_job_id "${child_index_oid}")"

    sql_super -c "DROP TABLE public.lifecycle_alter_child,
                             public.lifecycle_alter_parent;" >/dev/null
}

test_additional_rewrite_reconciliation() {
    local index_oid job_after job_before mv_index_oid mv_job_after
    local mv_job_before

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_additional_rewrite_docs
          (id integer, body text);
        INSERT INTO public.lifecycle_additional_rewrite_docs
          VALUES (1, 'one');
        CREATE INDEX lifecycle_additional_rewrite_idx
          ON public.lifecycle_additional_rewrite_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_additional_rewrite_idx'::regclass::oid;")"

    job_before="$(current_generation_job_id "${index_oid}")"
    sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_additional_rewrite_docs
          ALTER COLUMN id TYPE bigint;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "ALTER COLUMN TYPE did not reconcile the workflow"
    fi

    sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_additional_rewrite_docs
          ADD COLUMN generated_body text
          GENERATED ALWAYS AS (body) STORED;" >/dev/null 2>&1
    job_before="${job_after}"
    sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_additional_rewrite_docs
          ALTER COLUMN generated_body
          SET EXPRESSION AS (body OPERATOR(pg_catalog.||) ' changed');" \
        >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "ALTER COLUMN SET EXPRESSION did not reconcile the workflow"
    fi

    job_before="${job_after}"
    sql_as durable_owner -c \
        "TRUNCATE public.lifecycle_additional_rewrite_docs;" \
        >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "TRUNCATE did not reconcile the workflow"
    fi

    sql_as durable_owner -c "
        CREATE MATERIALIZED VIEW public.lifecycle_rewrite_mv
          AS SELECT body FROM public.lifecycle_additional_rewrite_docs;
        CREATE INDEX lifecycle_rewrite_mv_idx
          ON public.lifecycle_rewrite_mv USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    mv_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_rewrite_mv_idx'::regclass::oid;")"
    mv_job_before="$(current_generation_job_id "${mv_index_oid}")"
    sql_as durable_owner -c \
        "REFRESH MATERIALIZED VIEW public.lifecycle_rewrite_mv;" \
        >/dev/null 2>&1
    mv_job_after="$(current_generation_job_id "${mv_index_oid}")"
    if [ -z "${mv_job_after}" ] ||
        [ "${mv_job_after}" = "${mv_job_before}" ]; then
        error "REFRESH MATERIALIZED VIEW did not reconcile the workflow"
    fi

    sql_super -c "DROP MATERIALIZED VIEW public.lifecycle_rewrite_mv;
                   DROP TABLE public.lifecycle_additional_rewrite_docs;"
}

test_truncate_cascade_reconciliation() {
    local child_oid child_job_after child_job_before

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_truncate_parent (
    id integer PRIMARY KEY);
CREATE TABLE public.lifecycle_truncate_child (
    parent_id integer REFERENCES public.lifecycle_truncate_parent(id),
    body text);
INSERT INTO public.lifecycle_truncate_parent VALUES (1);
INSERT INTO public.lifecycle_truncate_child VALUES (1, 'one');
CREATE INDEX lifecycle_truncate_child_idx
    ON public.lifecycle_truncate_child USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    child_oid="$(sql_super -c "SELECT
        'public.lifecycle_truncate_child_idx'::regclass::oid;")"
    child_job_before="$(current_generation_job_id "${child_oid}")"

    sql_as durable_owner -c \
        "TRUNCATE public.lifecycle_truncate_parent CASCADE;" \
        >/dev/null 2>&1
    child_job_after="$(current_generation_job_id "${child_oid}")"
    if [ -z "${child_job_after}" ] ||
        [ "${child_job_after}" = "${child_job_before}" ]; then
        error "TRUNCATE CASCADE did not reconcile the cascaded workflow"
    fi

    sql_super -c "DROP TABLE public.lifecycle_truncate_child,
                             public.lifecycle_truncate_parent;" >/dev/null
}

test_maintenance_privilege_compatibility() {
    local cluster_oid cluster_job_after cluster_job_before
    local global_job_after matview_oid matview_job_after matview_job_before
    local schema_job_after schema_job_before

    sql_super -c "CREATE SCHEMA lifecycle_maintain
                   AUTHORIZATION durable_owner;" >/dev/null
    sql_as durable_owner <<'SQL'
CREATE TABLE lifecycle_maintain.cluster_docs
    (id integer, body text);
INSERT INTO lifecycle_maintain.cluster_docs
VALUES (2, 'two'), (1, 'one');
CREATE INDEX lifecycle_cluster_order_idx
    ON lifecycle_maintain.cluster_docs(id);
CREATE INDEX lifecycle_cluster_bm25_idx
    ON lifecycle_maintain.cluster_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
ALTER TABLE lifecycle_maintain.cluster_docs
    CLUSTER ON lifecycle_cluster_order_idx;
CREATE MATERIALIZED VIEW lifecycle_maintain.docs_mv
    AS SELECT body FROM lifecycle_maintain.cluster_docs;
CREATE INDEX lifecycle_docs_mv_idx
    ON lifecycle_maintain.docs_mv USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    sql_super -c "GRANT USAGE ON SCHEMA lifecycle_maintain
                   TO durable_maintainer;
                   GRANT MAINTAIN ON
                     lifecycle_maintain.cluster_docs,
                     lifecycle_maintain.docs_mv
                   TO durable_maintainer;" >/dev/null

    cluster_oid="$(sql_super -c "SELECT
        'lifecycle_maintain.lifecycle_cluster_bm25_idx'::regclass::oid;")"
    cluster_job_before="$(current_generation_job_id "${cluster_oid}")"
    sql_as durable_maintainer -c "
        BEGIN;
        CLUSTER lifecycle_maintain.cluster_docs
          USING lifecycle_cluster_order_idx;
        COMMIT;" >/dev/null
    cluster_job_after="$(current_generation_job_id "${cluster_oid}")"
    if [ -z "${cluster_job_after}" ] ||
        [ "${cluster_job_after}" = "${cluster_job_before}" ]; then
        error "transactional MAINTAIN CLUSTER did not reconcile the workflow"
    fi

    matview_oid="$(sql_super -c "SELECT
        'lifecycle_maintain.lifecycle_docs_mv_idx'::regclass::oid;")"
    matview_job_before="$(current_generation_job_id "${matview_oid}")"
    sql_as durable_maintainer -c \
        "REFRESH MATERIALIZED VIEW lifecycle_maintain.docs_mv;" \
        >/dev/null
    matview_job_after="$(current_generation_job_id "${matview_oid}")"
    if [ -z "${matview_job_after}" ] ||
        [ "${matview_job_after}" = "${matview_job_before}" ]; then
        error "MAINTAIN REFRESH did not reconcile the workflow"
    fi

    schema_job_before="${cluster_job_after}"
    sql_as durable_maintainer -c \
        "REINDEX SCHEMA lifecycle_maintain;" >/dev/null
    schema_job_after="$(current_generation_job_id "${cluster_oid}")"
    if [ -z "${schema_job_after}" ] ||
        [ "${schema_job_after}" = "${schema_job_before}" ]; then
        error "pg_maintain REINDEX SCHEMA did not reconcile the workflow"
    fi

    sql_as durable_maintainer -c "CLUSTER;" >/dev/null
    global_job_after="$(current_generation_job_id "${cluster_oid}")"
    if [ -z "${global_job_after}" ] ||
        [ "${global_job_after}" = "${schema_job_after}" ]; then
        error "database-wide MAINTAIN CLUSTER did not reconcile the workflow"
    fi

    sql_super -c "DROP SCHEMA lifecycle_maintain CASCADE;" >/dev/null
}

test_tablespace_move_without_owned_by() {
    local blocker_pid error_output error_tablespace_dir index_oid
    local job_after job_before tablespace_dir

    tablespace_dir="${DATA_DIR}-lifecycle-owner-tablespace"
    mkdir -p "${tablespace_dir}"
    sql_super -c "CREATE TABLESPACE lifecycle_owner_tablespace
                   OWNER durable_owner LOCATION '${tablespace_dir}';" \
        >/dev/null
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_owner_move_docs (body text);
        CREATE INDEX lifecycle_owner_move_idx
          ON public.lifecycle_owner_move_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *')
          TABLESPACE lifecycle_owner_tablespace;" \
        >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_owner_move_idx'::regclass::oid;")"
    job_before="$(current_generation_job_id "${index_oid}")"

    sql_as durable_owner -c "
        ALTER INDEX ALL IN TABLESPACE lifecycle_owner_tablespace
          SET TABLESPACE pg_default;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "owner move without OWNED BY did not reconcile the workflow"
    fi

    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_owner_move_idx
          SET TABLESPACE lifecycle_owner_tablespace;" >/dev/null 2>&1
    job_before="$(current_generation_job_id "${index_oid}")"
    sql_as durable_owner -c "
        ALTER INDEX ALL IN TABLESPACE lifecycle_owner_tablespace
          OWNED BY durable_owner, durable_writer
          SET TABLESPACE pg_default;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ] || [ "${job_after}" = "${job_before}" ]; then
        error "mixed OWNED BY move did not reconcile the caller-owned index"
    fi

    error_tablespace_dir="${DATA_DIR}-lifecycle-owner-error-tablespace"
    mkdir -p "${error_tablespace_dir}"
    sql_super -c "
        CREATE TABLESPACE lifecycle_owner_error_tablespace
          OWNER durable_owner LOCATION '${error_tablespace_dir}';" >/dev/null
    sql_super -c "
        CREATE TABLE public.lifecycle_owner_conflict_docs (value integer);
        ALTER TABLE public.lifecycle_owner_conflict_docs
          OWNER TO durable_writer;
        CREATE INDEX lifecycle_owner_conflict_idx
          ON public.lifecycle_owner_conflict_docs(value)
          TABLESPACE lifecycle_owner_error_tablespace;" >/dev/null
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_owner_locked_docs (body text);
        CREATE INDEX lifecycle_owner_locked_idx
          ON public.lifecycle_owner_locked_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *')
          TABLESPACE lifecycle_owner_error_tablespace;" >/dev/null 2>&1

    PGAPPNAME=lifecycle-owner-mixed-lock \
        sql_as durable_owner -c "
        BEGIN;
        ALTER INDEX public.lifecycle_owner_locked_idx
          SET (compaction_schedule = '1 2 3 4 *');
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/owner-mixed-lock.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_owner_locked_idx'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    if error_output="$(sql_as durable_owner -c "
        ALTER INDEX ALL IN TABLESPACE lifecycle_owner_error_tablespace
          OWNED BY durable_owner, durable_writer
          SET TABLESPACE pg_default NOWAIT;" 2>&1)"; then
        error "mixed OWNED BY moved an index owned by another role"
    fi
    if ! grep -Fq "must be owner of index lifecycle_owner_conflict_idx" \
        <<<"${error_output}" &&
        ! grep -Fq \
            'lock on relation "public.lifecycle_owner_locked_idx" is not available' \
            <<<"${error_output}"; then
        error "mixed OWNED BY did not preserve a core failure: \
${error_output}"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-owner-mixed-lock';" >/dev/null
    wait "${blocker_pid}" || true

    sql_super -c "DROP TABLE public.lifecycle_owner_move_docs;" >/dev/null
    sql_super -c "DROP TABLESPACE lifecycle_owner_tablespace;" >/dev/null
    sql_super -c "DROP TABLE public.lifecycle_owner_conflict_docs,
                             public.lifecycle_owner_locked_docs;" >/dev/null
    sql_super -c \
        "DROP TABLESPACE lifecycle_owner_error_tablespace;" >/dev/null
}

test_repeatable_read_direct_activation_snapshot() {
    local index_oid initial_job reader_output reader_pid

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_rr_activation_docs (body text);
        CREATE INDEX lifecycle_rr_activation_idx
          ON public.lifecycle_rr_activation_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_rr_activation_idx'::regclass::oid;")"
    initial_job="$(current_generation_job_id "${index_oid}")"
    sql_as durable_owner -c \
        "SELECT df.cancel('${initial_job}', 'repeatable-read setup');" \
        >/dev/null
    wait_for_terminal "${initial_job}" 30

    reader_output="${DATA_DIR}/repeatable-read-activation.out"
    PGAPPNAME=lifecycle-repeatable-read-activation \
        sql_as durable_owner <<'SQL' >"${reader_output}" 2>&1 &
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM df.instances;
SELECT pg_catalog.pg_sleep(5);
ALTER INDEX public.lifecycle_rr_activation_idx
    SET (compaction_schedule = '1 2 3 4 *');
COMMIT;
SQL
    reader_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-repeatable-read-activation'
                AND query OPERATOR(pg_catalog.~~) '%pg_sleep%';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_rr_activation_idx
          SET (compaction_schedule = '1 2 3 4 *');" >/dev/null 2>&1
    wait "${reader_pid}" || {
        cat "${reader_output}" >&2
        error "repeatable-read activation failed"
    }
    assert_eq "repeatable-read direct activation keeps one workflow" "1" \
        "$(current_generation_job_count "${index_oid}")"

    sql_super -c \
        "DROP TABLE public.lifecycle_rr_activation_docs;" >/dev/null
}

test_owner_repair_without_old_durable_acl() {
    local index_oid job_after

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_revoked_owner_docs (body text);
        CREATE INDEX lifecycle_revoked_owner_idx
          ON public.lifecycle_revoked_owner_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '1 2 3 4 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_revoked_owner_idx'::regclass::oid;")"

    sql_super -c "REVOKE USAGE ON SCHEMA df FROM durable_owner;
                   REVOKE ALL ON df.instances FROM durable_owner;" >/dev/null
    sql_super -c "ALTER TABLE public.lifecycle_revoked_owner_docs
                   OWNER TO durable_owner_two;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    assert_eq "revoked old-owner ACL permits ownership repair" \
        "durable_owner_two:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
            ':',
            submitted_by::pg_catalog.text,
            label OPERATOR(pg_catalog.~~)
              ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex')))
          FROM df.instances WHERE id = '${job_after}';")"

    sql_super -c "SELECT df.grant_usage('durable_owner');" >/dev/null
    sql_super -c \
        "DROP TABLE public.lifecycle_revoked_owner_docs;" >/dev/null
}

test_writer_search_path_isolation() {
    local memtable_threshold_before

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_search_path_docs
          (id integer, body text);
        CREATE INDEX lifecycle_search_path_idx
          ON public.lifecycle_search_path_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    sql_super <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_search_path_audit (
    role_name name NOT NULL
);
ALTER TABLE public.lifecycle_search_path_audit OWNER TO durable_owner;
REVOKE ALL ON public.lifecycle_search_path_audit FROM PUBLIC;

CREATE SCHEMA lifecycle_writer_shadow AUTHORIZATION durable_writer;
GRANT USAGE ON SCHEMA lifecycle_writer_shadow TO durable_owner;
GRANT INSERT ON public.lifecycle_search_path_docs TO durable_writer;
SQL
    sql_as durable_writer <<'SQL' >/dev/null
CREATE FUNCTION lifecycle_writer_shadow.hijack_varchar_text(
    left_value varchar, right_value text)
RETURNS boolean
LANGUAGE plpgsql
VOLATILE
AS $body$
BEGIN
    INSERT INTO public.lifecycle_search_path_audit(role_name)
    VALUES (current_user);
    RETURN left_value::pg_catalog.text
           OPERATOR(pg_catalog.=) right_value;
END
$body$;

CREATE OPERATOR lifecycle_writer_shadow.= (
    LEFTARG = pg_catalog.varchar,
    RIGHTARG = pg_catalog.text,
    FUNCTION = lifecycle_writer_shadow.hijack_varchar_text
);
SQL

    memtable_threshold_before="$(sql_super -c \
        "SHOW pg_textsearch.memtable_pages_threshold;")"
    sql_super -c "ALTER SYSTEM SET
                     pg_textsearch.memtable_pages_threshold = 1;" >/dev/null
    sql_super -c "SELECT pg_catalog.pg_reload_conf();" >/dev/null
    sql_as durable_writer <<'SQL' >/dev/null
SET search_path = lifecycle_writer_shadow, pg_catalog, public;
INSERT INTO public.lifecycle_search_path_docs
SELECT document_number,
       (SELECT pg_catalog.string_agg(
                   pg_catalog.format(
                       'searchpath%sterm%s', document_number, term_number),
                   ' ')
        FROM pg_catalog.generate_series(1, 200) AS term_number)
FROM pg_catalog.generate_series(1, 6) AS document_number;
SQL
    sql_super -c "ALTER SYSTEM RESET
                     pg_textsearch.memtable_pages_threshold;" >/dev/null
    sql_super -c "SELECT pg_catalog.pg_reload_conf();" >/dev/null
    assert_eq "writer spill threshold is restored" \
        "${memtable_threshold_before}" \
        "$(sql_super -c \
            "SHOW pg_textsearch.memtable_pages_threshold;")"
    assert_eq "writer search_path cannot execute as the index owner" "0" \
        "$(sql_super -c \
            "SELECT count(*) FROM public.lifecycle_search_path_audit;")"

    sql_super -c "DROP TABLE public.lifecycle_search_path_docs,
                             public.lifecycle_search_path_audit;
                   DROP SCHEMA lifecycle_writer_shadow CASCADE;" >/dev/null
}

test_persisted_helper_oid_guard() {
    local index_oid instance_id

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_helper_oid_docs (body text);
        CREATE INDEX lifecycle_helper_oid_idx
          ON public.lifecycle_helper_oid_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_helper_oid_idx'::regclass::oid;")"
    instance_id="$(current_generation_job_id "${index_oid}")"
    wait_for_signal_node "${instance_id}" 90

    sql_super <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_helper_oid_audit (
    role_name name NOT NULL
);
ALTER TABLE public.lifecycle_helper_oid_audit OWNER TO durable_owner;
REVOKE ALL ON public.lifecycle_helper_oid_audit FROM PUBLIC;
GRANT INSERT ON public.lifecycle_helper_oid_audit TO durable_owner;

ALTER DATABASE durable_compaction_test OWNER TO durable_actor;
SET ROLE durable_actor;
ALTER SCHEMA public RENAME TO lifecycle_original_public;
CREATE SCHEMA public AUTHORIZATION durable_actor;
CREATE FUNCTION public.lifecycle_helper_oid_payload()
RETURNS boolean
LANGUAGE plpgsql
VOLATILE
AS $body$
BEGIN
    INSERT INTO lifecycle_original_public.lifecycle_helper_oid_audit(
        role_name)
    VALUES (current_user);
    RETURN false;
END
$body$;
CREATE FUNCTION public.bm25_background_target_is_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
LANGUAGE sql
IMMUTABLE
AS 'SELECT public.lifecycle_helper_oid_payload()';
GRANT USAGE ON SCHEMA public TO durable_owner;
GRANT EXECUTE ON FUNCTION public.lifecycle_helper_oid_payload()
    TO durable_owner;
GRANT EXECUTE ON FUNCTION
    public.bm25_background_target_is_current(oid, oid, oid, oid, oid)
    TO durable_owner;
RESET ROLE;
SQL

    sql_as durable_owner -c \
        "SELECT df.signal('${instance_id}', 'compact', '{}');" >/dev/null
    wait_for_terminal "${instance_id}" 30
    assert_eq "persisted workflow verifies the helper function OID" "0" \
        "$(sql_super -c "SELECT count(*)
          FROM lifecycle_original_public.lifecycle_helper_oid_audit;")"

    sql_super <<'SQL' >/dev/null
DROP SCHEMA public CASCADE;
ALTER SCHEMA lifecycle_original_public RENAME TO public;
ALTER DATABASE durable_compaction_test OWNER TO postgres;
DROP TABLE public.lifecycle_helper_oid_docs,
           public.lifecycle_helper_oid_audit;
SQL
}

test_reassign_owned_heap_lock_order() {
    local blocker_pid index_oid reassign_output reassign_pid

    sql_super -c "CREATE ROLE lifecycle_reassign_lock_old LOGIN;
                   CREATE ROLE lifecycle_reassign_lock_new LOGIN;
                   SELECT df.grant_usage('lifecycle_reassign_lock_old');
                   SELECT df.grant_usage('lifecycle_reassign_lock_new');
                   GRANT CONNECT ON DATABASE ${TEST_DB}
                     TO lifecycle_reassign_lock_old,
                        lifecycle_reassign_lock_new;
                   GRANT USAGE ON SCHEMA public
                     TO lifecycle_reassign_lock_old,
                        lifecycle_reassign_lock_new;
                   GRANT CREATE ON SCHEMA public
                     TO lifecycle_reassign_lock_old,
                        lifecycle_reassign_lock_new;" >/dev/null
    sql_as lifecycle_reassign_lock_old -c "
        CREATE TABLE public.lifecycle_reassign_lock_docs (body text);
        CREATE INDEX lifecycle_reassign_lock_idx
          ON public.lifecycle_reassign_lock_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_reassign_lock_idx'::regclass::oid;")"

    PGAPPNAME=lifecycle-reassign-lock-blocker \
        sql_super -c "
        BEGIN;
        LOCK TABLE public.lifecycle_reassign_lock_docs
          IN ACCESS EXCLUSIVE MODE;
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/reassign-lock-blocker.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_reassign_lock_docs'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    reassign_output="${DATA_DIR}/reassign-lock-order.out"
    PGAPPNAME=lifecycle-reassign-lock-order \
        sql_super -c "
        REASSIGN OWNED BY lifecycle_reassign_lock_old
          TO lifecycle_reassign_lock_new;" \
        >"${reassign_output}" 2>&1 &
    reassign_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-reassign-lock-order'
                AND wait_event_type = 'Lock';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "REASSIGN waits for the heap before locking its index" "0" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS relation_lock
            ON relation_lock.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-reassign-lock-order'
            AND relation_lock.locktype = 'relation'
            AND relation_lock.relation = ${index_oid}
            AND relation_lock.mode = 'ShareUpdateExclusiveLock'
            AND relation_lock.granted;")"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-reassign-lock-blocker';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${reassign_pid}" || {
        cat "${reassign_output}" >&2
        error "REASSIGN lock-order test failed"
    }

    sql_super -c "DROP TABLE public.lifecycle_reassign_lock_docs;
                   DROP OWNED BY lifecycle_reassign_lock_old,
                                 lifecycle_reassign_lock_new;
                   DROP ROLE lifecycle_reassign_lock_old;
                   DROP ROLE lifecycle_reassign_lock_new;" >/dev/null
}

test_rewrite_preflight_ordering() {
    local alter_error blocker_pid bulk_error private_blocker_pid
    local tablespace_dir vacuum_error

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_preflight_a (body text);
        CREATE INDEX lifecycle_preflight_a_idx
          ON public.lifecycle_preflight_a USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_preflight_b (body text);
        CREATE INDEX lifecycle_preflight_b_idx
          ON public.lifecycle_preflight_b USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    sql_super -c "
        CREATE SCHEMA lifecycle_preflight_private
          AUTHORIZATION durable_owner;" >/dev/null
    sql_as durable_owner -c "
        CREATE TABLE lifecycle_preflight_private.docs (body text);
        CREATE INDEX docs_idx
          ON lifecycle_preflight_private.docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    tablespace_dir="${DATA_DIR}-lifecycle-preflight-tablespace"
    mkdir -p "${tablespace_dir}"
    sql_super -c "
        CREATE TABLESPACE lifecycle_preflight_tablespace
          OWNER durable_writer LOCATION '${tablespace_dir}';" >/dev/null

    PGAPPNAME=lifecycle-vacuum-preflight \
        sql_as durable_owner -c "
        BEGIN;
        ALTER INDEX public.lifecycle_preflight_b_idx
          SET (compaction_schedule = '1 2 3 4 *');
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/vacuum-preflight-lock.out" 2>&1 &
    blocker_pid=$!
    PGAPPNAME=lifecycle-private-preflight \
        sql_as durable_owner -c "
        BEGIN;
        ALTER INDEX lifecycle_preflight_private.docs_idx
          SET (compaction_schedule = '1 2 3 4 *');
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/private-preflight-lock.out" 2>&1 &
    private_blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_locks
              WHERE relation IN (
                    'public.lifecycle_preflight_b_idx'::regclass,
                    'lifecycle_preflight_private.docs_idx'::regclass)
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "2" ]; then
            break
        fi
        sleep 0.1
    done

    if vacuum_error="$(sql_as durable_owner -c "
        BEGIN;
        SET LOCAL statement_timeout = '2s';
        VACUUM (FULL);" 2>&1)"; then
        error "VACUUM FULL unexpectedly ran inside a transaction block"
    fi
    if ! grep -Fq "cannot run inside a transaction block" \
        <<<"${vacuum_error}"; then
        error "VACUUM FULL prelocked indexes before its transaction check: \
${vacuum_error}"
    fi

    if alter_error="$(sql_as durable_owner -c "
        SET statement_timeout = '2s';
        ALTER MATERIALIZED VIEW public.lifecycle_preflight_b
          OWNER TO durable_usage_only;" 2>&1)"; then
        error "ALTER MATERIALIZED VIEW accepted an ordinary table"
    fi
    if ! grep -Fq "is not a materialized view" <<<"${alter_error}"; then
        error "ALTER OWNER target checks preceded relation-kind validation: \
${alter_error}"
    fi

    if alter_error="$(sql_as durable_owner -c "
        SET statement_timeout = '2s';
        ALTER TABLE public.lifecycle_preflight_b
          OWNER TO durable_usage_only;" 2>&1)"; then
        error "ALTER OWNER unexpectedly accepted an unauthorized target role"
    fi
    if ! grep -Fq \
        "must be able to SET ROLE \"durable_usage_only\"" \
        <<<"${alter_error}"; then
        error "ALTER OWNER prelocked indexes before its target-role check: \
${alter_error}"
    fi

    sql_super -c "GRANT durable_owner_two TO durable_owner;" >/dev/null
    if alter_error="$(sql_as durable_owner -c "
        SET statement_timeout = '2s';
        ALTER TABLE lifecycle_preflight_private.docs
          OWNER TO durable_owner_two;" 2>&1)"; then
        error "ALTER OWNER unexpectedly bypassed target schema CREATE"
    fi
    if ! grep -Fq \
        "permission denied for schema lifecycle_preflight_private" \
        <<<"${alter_error}"; then
        error "ALTER OWNER prelocked indexes before its schema check: \
${alter_error}"
    fi
    sql_super -c "REVOKE durable_owner_two FROM durable_owner;" >/dev/null

    if bulk_error="$(sql_as durable_owner -c "
        SET statement_timeout = '2s';
        ALTER INDEX ALL IN TABLESPACE pg_default
          SET TABLESPACE lifecycle_preflight_missing;" 2>&1)"; then
        error "bulk move unexpectedly accepted a missing tablespace"
    fi
    if ! grep -Fq \
        'tablespace "lifecycle_preflight_missing" does not exist' \
        <<<"${bulk_error}"; then
        error "bulk move prelocked indexes before tablespace lookup: \
${bulk_error}"
    fi

    if bulk_error="$(sql_as durable_owner -c "
        SET statement_timeout = '2s';
        ALTER INDEX ALL IN TABLESPACE pg_default
          SET TABLESPACE lifecycle_preflight_tablespace;" 2>&1)"; then
        error "bulk move unexpectedly bypassed tablespace CREATE privilege"
    fi
    if ! grep -Fq \
        "permission denied for tablespace lifecycle_preflight_tablespace" \
        <<<"${bulk_error}"; then
        error "bulk move prelocked indexes before tablespace ACL checks: \
${bulk_error}"
    fi

    if ! bulk_error="$(sql_as durable_owner -c "
        SET statement_timeout = '2s';
        ALTER INDEX ALL IN TABLESPACE pg_default
          SET TABLESPACE pg_default;" 2>&1)"; then
        error "bulk move no-op waited for index locks: ${bulk_error}"
    fi

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name IN (
        'lifecycle-vacuum-preflight',
        'lifecycle-private-preflight');" >/dev/null
    wait "${blocker_pid}" || true
    wait "${private_blocker_pid}" || true
    sql_super -c "DROP TABLE public.lifecycle_preflight_a,
                             public.lifecycle_preflight_b;
                   DROP SCHEMA lifecycle_preflight_private CASCADE;" \
        >/dev/null
    sql_super -c "DROP TABLESPACE lifecycle_preflight_tablespace;" \
        >/dev/null
}

test_concurrent_reindex_reconciliation() {
    local a_before a_after a_oid_before a_oid_after
    local b_before b_after b_oid_before b_oid_after

    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      SET pg_textsearch.background_compaction_schedule = '1 2 3 4 *';"
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_concurrent_docs
          (body_a text, body_b text);
        INSERT INTO public.lifecycle_concurrent_docs
          SELECT pg_catalog.format('document %s', value),
                 pg_catalog.format('other %s', value)
          FROM generate_series(1, 20) AS value;
        CREATE INDEX lifecycle_concurrent_a_idx
          ON public.lifecycle_concurrent_docs USING bm25(body_a)
          WITH (text_config = 'english',
                compaction = 'background');" >/dev/null 2>&1
    sql_as durable_owner -c "
        CREATE INDEX lifecycle_concurrent_b_idx
          ON public.lifecycle_concurrent_docs USING bm25(body_b)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1

    a_oid_before="$(sql_super -c \
        "SELECT 'public.lifecycle_concurrent_a_idx'::regclass::oid;")"
    a_before="$(current_generation_job_id "${a_oid_before}")"
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      SET pg_textsearch.background_compaction_schedule = '5 6 7 8 *';"
    sql_as durable_owner -c \
        "REINDEX INDEX CONCURRENTLY
           public.lifecycle_concurrent_a_idx;" >/dev/null 2>&1
    a_oid_after="$(sql_super -c \
        "SELECT 'public.lifecycle_concurrent_a_idx'::regclass::oid;")"
    a_after="$(current_generation_job_id "${a_oid_after}")"
    if [ "${a_oid_after}" = "${a_oid_before}" ]; then
        error "REINDEX INDEX CONCURRENTLY did not replace the index OID"
    fi
    if [ -z "${a_after}" ] || [ "${a_after}" = "${a_before}" ]; then
        error "REINDEX INDEX CONCURRENTLY did not reconcile the new OID"
    fi
    assert_eq "concurrent index replacement has one current workflow" "1" \
        "$(current_generation_job_count "${a_oid_after}")"
    assert_eq "concurrent index replacement preserves captured schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${a_after}';")"

    a_oid_before="${a_oid_after}"
    b_oid_before="$(sql_super -c \
        "SELECT 'public.lifecycle_concurrent_b_idx'::regclass::oid;")"
    a_before="${a_after}"
    b_before="$(current_generation_job_id "${b_oid_before}")"
    sql_as durable_owner -c \
        "REINDEX TABLE CONCURRENTLY
           public.lifecycle_concurrent_docs;" >/dev/null 2>&1
    a_oid_after="$(sql_super -c \
        "SELECT 'public.lifecycle_concurrent_a_idx'::regclass::oid;")"
    b_oid_after="$(sql_super -c \
        "SELECT 'public.lifecycle_concurrent_b_idx'::regclass::oid;")"
    a_after="$(current_generation_job_id "${a_oid_after}")"
    b_after="$(current_generation_job_id "${b_oid_after}")"
    if [ "${a_oid_after}" = "${a_oid_before}" ] ||
        [ "${b_oid_after}" = "${b_oid_before}" ]; then
        error "REINDEX TABLE CONCURRENTLY did not replace every index OID"
    fi
    if [ -z "${a_after}" ] || [ "${a_after}" = "${a_before}" ] ||
        [ -z "${b_after}" ] || [ "${b_after}" = "${b_before}" ]; then
        error "REINDEX TABLE CONCURRENTLY did not reconcile replacement OIDs"
    fi

    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'concurrent reindex lifecycle test complete')
      FROM df.instances AS instance
      WHERE (instance.label OPERATOR(pg_catalog.~~)
               'pg_textsearch:bg:v1:%:${a_oid_after}:%'
             OR instance.label OPERATOR(pg_catalog.~~)
               'pg_textsearch:bg:v1:%:${b_oid_after}:%')
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    sql_as durable_owner -c \
        "DROP TABLE public.lifecycle_concurrent_docs;"
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      RESET pg_textsearch.background_compaction_schedule;"
}

test_partitioned_reindex_reconciliation() {
    local leaf_count leaf_jobs parent_index_oid

    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_reindex_partitioned_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_reindex_partitioned_low
    PARTITION OF public.lifecycle_reindex_partitioned_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_reindex_partitioned_high
    PARTITION OF public.lifecycle_reindex_partitioned_docs
    FOR VALUES FROM (100) TO (200);
INSERT INTO public.lifecycle_reindex_partitioned_docs
VALUES (1, 'low partition'), (101, 'high partition');
CREATE INDEX lifecycle_reindex_partitioned_idx
    ON public.lifecycle_reindex_partitioned_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    parent_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_reindex_partitioned_idx'::regclass::oid;")"
    sql_super <<SQL
CREATE TABLE public.lifecycle_reindex_partitioned_before AS
SELECT child.relname,
       child.oid AS index_oid,
       pg_catalog.pg_relation_filenode(child.oid) AS filenumber,
       (
         SELECT instance.id
         FROM df.instances AS instance
         WHERE instance.label OPERATOR(pg_catalog.~~)
               ('pg_textsearch:bg:v1:%:' || child.oid::text || ':%')
           AND instance.status OPERATOR(pg_catalog.=)
               ANY (ARRAY['pending', 'running']::pg_catalog.text[])
         ORDER BY instance.created_at DESC, instance.id DESC
         LIMIT 1
       ) AS instance_id
FROM pg_catalog.pg_inherits AS inheritance
JOIN pg_catalog.pg_class AS child
  ON child.oid = inheritance.inhrelid
WHERE inheritance.inhparent = ${parent_index_oid}
  AND child.relkind = 'i';
SQL
    sql_as durable_owner -c \
        "REINDEX INDEX
           public.lifecycle_reindex_partitioned_idx;" >/dev/null 2>&1
    assert_eq "partitioned REINDEX INDEX keeps both leaf OIDs" "2" \
        "$(sql_super -c "SELECT count(*)
          FROM public.lifecycle_reindex_partitioned_before AS before
          JOIN pg_catalog.pg_class AS current
            ON current.oid = before.index_oid
          WHERE pg_catalog.pg_relation_filenode(current.oid)
                OPERATOR(pg_catalog.<>) before.filenumber;")"
    leaf_jobs="$(sql_super -c "SELECT count(*)
      FROM public.lifecycle_reindex_partitioned_before AS before
      JOIN pg_catalog.pg_class AS current
        ON current.oid = before.index_oid
      JOIN df.instances AS instance
        ON instance.label OPERATOR(pg_catalog.~~)
           pg_catalog.format(
             'pg_textsearch:bg:v1:%%:%s:%%:%s:%s:%%',
             current.oid,
             pg_catalog.pg_relation_filenode(current.oid),
             current.relowner)
       AND instance.status OPERATOR(pg_catalog.=)
           ANY (ARRAY['pending', 'running']::pg_catalog.text[])
      WHERE instance.id OPERATOR(pg_catalog.<>) before.instance_id;")"
    assert_eq "partitioned REINDEX INDEX reconciles every leaf" "2" \
        "${leaf_jobs}"

    sql_super -c "TRUNCATE public.lifecycle_reindex_partitioned_before;
      INSERT INTO public.lifecycle_reindex_partitioned_before
      SELECT child.relname,
             child.oid,
             pg_catalog.pg_relation_filenode(child.oid),
             (
               SELECT instance.id
               FROM df.instances AS instance
               WHERE instance.label OPERATOR(pg_catalog.~~)
                     ('pg_textsearch:bg:v1:%:' ||
                      child.oid::text || ':%')
                 AND instance.status OPERATOR(pg_catalog.=)
                     ANY (ARRAY['pending', 'running']::pg_catalog.text[])
               ORDER BY instance.created_at DESC, instance.id DESC
               LIMIT 1
             )
      FROM pg_catalog.pg_inherits AS inheritance
      JOIN pg_catalog.pg_class AS child
        ON child.oid = inheritance.inhrelid
      WHERE inheritance.inhparent = ${parent_index_oid}
        AND child.relkind = 'i';"
    sql_as durable_owner -c \
        "REINDEX TABLE CONCURRENTLY
           public.lifecycle_reindex_partitioned_docs;" >/dev/null 2>&1
    leaf_count="$(sql_super -c "SELECT count(*)
      FROM public.lifecycle_reindex_partitioned_before AS before
      JOIN pg_catalog.pg_class AS current
        ON current.relname = before.relname
       AND current.relnamespace = 'public'::regnamespace
      WHERE current.oid OPERATOR(pg_catalog.<>) before.index_oid;")"
    assert_eq "partitioned concurrent REINDEX resolves replacement OIDs" \
        "2" "${leaf_count}"
    leaf_jobs="$(sql_super -c "SELECT count(*)
      FROM public.lifecycle_reindex_partitioned_before AS before
      JOIN pg_catalog.pg_class AS current
        ON current.relname = before.relname
       AND current.relnamespace = 'public'::regnamespace
      JOIN df.instances AS instance
        ON instance.label OPERATOR(pg_catalog.~~)
           ('pg_textsearch:bg:v1:%:' || current.oid::text || ':%')
       AND instance.status OPERATOR(pg_catalog.=)
           ANY (ARRAY['pending', 'running']::pg_catalog.text[])
      WHERE instance.id OPERATOR(pg_catalog.<>) before.instance_id;")"
    assert_eq "partitioned concurrent REINDEX reconciles every leaf" "2" \
        "${leaf_jobs}"

    sql_as durable_owner -c \
        "DROP TABLE public.lifecycle_reindex_partitioned_docs;"
    sql_super -c "DROP TABLE public.lifecycle_reindex_partitioned_before;"
}

test_partitioned_reindex_failure_reconciliation() {
    local first_file_before first_file_after first_index_oid
    local first_job_before first_job_after reindex_error second_file_before
    local second_file_after second_index_oid

    sql_as durable_owner <<'SQL' >/dev/null
CREATE FUNCTION public.lifecycle_reindex_value(value text)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting(
           'lifecycle.reindex_failure', true)
           OPERATOR(pg_catalog.=) 'on'
       AND value OPERATOR(pg_catalog.=) 'fail' THEN
        RAISE EXCEPTION 'intentional partition reindex failure';
    END IF;
    RETURN value;
END
$body$;

CREATE TABLE public.lifecycle_reindex_failure_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_reindex_failure_low
    PARTITION OF public.lifecycle_reindex_failure_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_reindex_failure_high
    PARTITION OF public.lifecycle_reindex_failure_docs
    FOR VALUES FROM (100) TO (200);
INSERT INTO public.lifecycle_reindex_failure_docs
VALUES (1, 'safe'), (101, 'fail');
CREATE INDEX lifecycle_reindex_failure_idx
    ON public.lifecycle_reindex_failure_docs
    USING bm25 (public.lifecycle_reindex_value(body))
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    first_index_oid="$(sql_super -c "SELECT child.oid
      FROM pg_catalog.pg_inherits AS inheritance
      JOIN pg_catalog.pg_class AS child
        ON child.oid = inheritance.inhrelid
      WHERE inheritance.inhparent =
            'public.lifecycle_reindex_failure_idx'::regclass
      ORDER BY child.oid
      LIMIT 1;")"
    second_index_oid="$(sql_super -c "SELECT child.oid
      FROM pg_catalog.pg_inherits AS inheritance
      JOIN pg_catalog.pg_class AS child
        ON child.oid = inheritance.inhrelid
      WHERE inheritance.inhparent =
            'public.lifecycle_reindex_failure_idx'::regclass
      ORDER BY child.oid DESC
      LIMIT 1;")"
    first_file_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${first_index_oid});")"
    second_file_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${second_index_oid});")"
    first_job_before="$(current_generation_job_id "${first_index_oid}")"

    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
        SET lifecycle.reindex_failure = 'on';"
    if reindex_error="$(sql_as durable_owner -c "
        REINDEX INDEX
          public.lifecycle_reindex_failure_idx;" 2>&1)"; then
        error "partitioned REINDEX failure injection unexpectedly succeeded"
    fi
    if ! grep -Fq "intentional partition reindex failure" \
        <<<"${reindex_error}"; then
        error "partitioned REINDEX failed for the wrong reason: \
${reindex_error}"
    fi
    first_file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${first_index_oid});")"
    second_file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${second_index_oid});")"
    if [ "${first_file_after}" = "${first_file_before}" ]; then
        error "partitioned REINDEX did not commit the earlier leaf"
    fi
    assert_eq "failed partitioned REINDEX rolls back the failing leaf" \
        "${second_file_before}" "${second_file_after}"
    first_job_after="$(current_generation_job_id "${first_index_oid}")"
    if [ -z "${first_job_after}" ] ||
        [ "${first_job_after}" = "${first_job_before}" ]; then
        error "committed partition leaf replacement was not reconciled"
    fi
    assert_eq "failed partitioned REINDEX selects the admitted earlier leaf" \
        "${first_job_after}" \
        "$(current_generation_job_id "${first_index_oid}")"
    assert_eq "earlier leaf reconciliation does not eagerly cancel history" \
        "t" \
        "$(sql_super -c "SELECT status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[])
          FROM df.instances WHERE id = '${first_job_before}';")"
    assert_eq "old physical helper rejects the committed replacement" "f" \
        "$(background_target_is_current \
            "${first_index_oid}" "${first_file_before}" durable_owner)"
    sql_as durable_owner -c "SELECT df.signal(
        '${first_job_before}', 'compact', '{}');" >/dev/null
    wait_for_terminal "${first_job_before}" 30
    log "PASS: stale earlier-leaf workflow retires through its guard"

    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
        RESET lifecycle.reindex_failure;"
    sql_as durable_owner -c "
        DROP TABLE public.lifecycle_reindex_failure_docs;
        DROP FUNCTION public.lifecycle_reindex_value(text);"
}

test_partitioned_reindex_rename_reconciliation() {
    local blocker_pid high_index_oid high_index_name high_file_before
    local high_file_after high_job_before high_job_after reindex_pid
    local reindex_output="${DATA_DIR}/partitioned-reindex-rename.out"

    sql_as durable_owner <<'SQL' >/dev/null
CREATE FUNCTION public.lifecycle_reindex_pause(value text)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF value OPERATOR(pg_catalog.=) 'pause' THEN
        PERFORM pg_catalog.pg_advisory_xact_lock(478, 3);
    END IF;
    RETURN value;
END
$body$;

CREATE TABLE public.lifecycle_reindex_rename_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_reindex_rename_low
    PARTITION OF public.lifecycle_reindex_rename_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_reindex_rename_high
    PARTITION OF public.lifecycle_reindex_rename_docs
    FOR VALUES FROM (100) TO (200);
INSERT INTO public.lifecycle_reindex_rename_docs
VALUES (1, 'pause'), (101, 'continue');
CREATE INDEX lifecycle_reindex_rename_idx
    ON public.lifecycle_reindex_rename_docs
    USING bm25 (public.lifecycle_reindex_pause(body))
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    high_index_oid="$(sql_super -c "SELECT index_class.oid
      FROM pg_catalog.pg_inherits AS inheritance
      JOIN pg_catalog.pg_class AS index_class
        ON index_class.oid = inheritance.inhrelid
      JOIN pg_catalog.pg_index AS index_catalog
        ON index_catalog.indexrelid = index_class.oid
      WHERE inheritance.inhparent =
            'public.lifecycle_reindex_rename_idx'::regclass
        AND index_catalog.indrelid =
            'public.lifecycle_reindex_rename_high'::regclass;")"
    high_index_name="$(sql_super -c "SELECT relname
      FROM pg_catalog.pg_class WHERE oid = ${high_index_oid};")"
    high_file_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${high_index_oid});")"
    high_job_before="$(current_generation_job_id "${high_index_oid}")"

    sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 3);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/partitioned-reindex-lock.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 3
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-reindex-rename \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "REINDEX INDEX public.lifecycle_reindex_rename_idx;" \
        >"${reindex_output}" 2>&1 &
    reindex_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-reindex-rename'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "partitioned REINDEX pauses before the second leaf" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-reindex-rename'
            AND wait_event = 'advisory';")"

    sql_as durable_owner -c "ALTER INDEX public.${high_index_name}
      RENAME TO lifecycle_reindex_rename_high_renamed_idx;"
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE pid <> pg_catalog.pg_backend_pid()
        AND query OPERATOR(pg_catalog.~~)
            'SELECT pg_catalog.pg_advisory_lock(478, 3)%';" >/dev/null
    wait "${blocker_pid}" || true
    if ! wait "${reindex_pid}"; then
        error "partitioned REINDEX with leaf rename failed: \
$(cat "${reindex_output}")"
    fi

    high_file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${high_index_oid});")"
    high_job_after="$(current_generation_job_id "${high_index_oid}")"
    if [ "${high_file_after}" = "${high_file_before}" ]; then
        error "partitioned REINDEX did not rebuild the renamed leaf"
    fi
    if [ -z "${high_job_after}" ] ||
        [ "${high_job_after}" = "${high_job_before}" ]; then
        error "renamed pending leaf was not reconciled by original OID"
    fi

    sql_as durable_owner -c "
        DROP TABLE public.lifecycle_reindex_rename_docs;
        DROP FUNCTION public.lifecycle_reindex_pause(text);"
}

test_reindex_tracking_reentry() {
    local leaf_jobs reindex_output

    sql_as durable_owner <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_reentry_outer_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_reentry_outer_low
    PARTITION OF public.lifecycle_reentry_outer_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_reentry_outer_high
    PARTITION OF public.lifecycle_reentry_outer_docs
    FOR VALUES FROM (100) TO (200);
INSERT INTO public.lifecycle_reentry_outer_docs
VALUES (1, 'low'), (101, 'high');
CREATE INDEX lifecycle_reentry_outer_idx
    ON public.lifecycle_reentry_outer_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');

CREATE TABLE public.lifecycle_reentry_inner_docs (body text);
INSERT INTO public.lifecycle_reentry_inner_docs VALUES ('inner');
CREATE INDEX lifecycle_reentry_inner_idx
    ON public.lifecycle_reentry_inner_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    sql_super <<'SQL' >/dev/null
CREATE TABLE public.lifecycle_reentry_before AS
SELECT child.oid AS index_oid,
       pg_catalog.pg_relation_filenode(child.oid) AS filenumber,
       (
         SELECT instance.id
         FROM df.instances AS instance
         WHERE instance.label OPERATOR(pg_catalog.~~)
               ('pg_textsearch:bg:v1:%:' || child.oid::text || ':%')
           AND instance.status OPERATOR(pg_catalog.=)
               ANY (ARRAY['pending', 'running']::pg_catalog.text[])
         ORDER BY instance.created_at DESC, instance.id DESC
         LIMIT 1
       ) AS instance_id
FROM pg_catalog.pg_inherits AS inheritance
JOIN pg_catalog.pg_class AS child
  ON child.oid = inheritance.inhrelid
WHERE inheritance.inhparent =
      'public.lifecycle_reentry_outer_idx'::regclass;

CREATE FUNCTION public.lifecycle_reindex_reenter()
RETURNS event_trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting(
           'lifecycle.reindex_reentry', true)
           OPERATOR(pg_catalog.=) 'outer' THEN
        PERFORM pg_catalog.set_config(
            'lifecycle.reindex_reentry', 'inner', false);
        EXECUTE 'REINDEX INDEX public.lifecycle_reentry_inner_idx';
    END IF;
END
$body$;

CREATE EVENT TRIGGER lifecycle_reindex_reenter
    ON ddl_command_start
    WHEN TAG IN ('REINDEX')
    EXECUTE FUNCTION public.lifecycle_reindex_reenter();
SQL
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      SET lifecycle.reindex_reentry = 'outer';"
    if ! reindex_output="$(sql_as durable_owner -c "
        REINDEX INDEX public.lifecycle_reentry_outer_idx;" 2>&1)"; then
        error "nested ordinary REINDEX broke outer tracking: ${reindex_output}"
    fi
    sql_super -c "ALTER ROLE durable_owner IN DATABASE ${TEST_DB}
      RESET lifecycle.reindex_reentry;"
    sql_super -c "DROP EVENT TRIGGER lifecycle_reindex_reenter;
      DROP FUNCTION public.lifecycle_reindex_reenter();"

    leaf_jobs="$(sql_super -c "SELECT pg_catalog.count(*)
      FROM public.lifecycle_reentry_before AS before
      JOIN pg_catalog.pg_class AS current
        ON current.oid = before.index_oid
      JOIN df.instances AS instance
        ON instance.label OPERATOR(pg_catalog.~~)
           pg_catalog.format(
             'pg_textsearch:bg:v1:%%:%s:%%:%s:%s:%%',
             current.oid,
             pg_catalog.pg_relation_filenode(current.oid),
             current.relowner)
       AND instance.status OPERATOR(pg_catalog.=)
           ANY (ARRAY['pending', 'running']::pg_catalog.text[])
      WHERE pg_catalog.pg_relation_filenode(current.oid)
            OPERATOR(pg_catalog.<>) before.filenumber
        AND instance.id OPERATOR(pg_catalog.<>) before.instance_id;")"
    assert_eq "nested ordinary REINDEX preserves outer tracking" \
        "2" "${leaf_jobs}"

    sql_super -c "DROP TABLE public.lifecycle_reentry_before;"
    sql_as durable_owner -c "
        DROP TABLE public.lifecycle_reentry_outer_docs,
                   public.lifecycle_reentry_inner_docs;"
}

test_ordinary_inheritance_reindex_scope() {
    local child_file_before child_file_after child_job_before
    local child_job_after parent_file_before parent_file_after
    local parent_index_oid

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_inherit_parent (body text);
        CREATE TABLE public.lifecycle_inherit_child ()
          INHERITS (public.lifecycle_inherit_parent);
        CREATE INDEX lifecycle_inherit_parent_idx
          ON public.lifecycle_inherit_parent USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE INDEX lifecycle_inherit_child_idx
          ON public.lifecycle_inherit_child USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    sql_super -c "ALTER TABLE public.lifecycle_inherit_child
                   OWNER TO durable_owner_two;" >/dev/null 2>&1
    parent_index_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_inherit_parent_idx'::regclass::oid;")"
    parent_file_before="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    child_file_before="$(sql_super -c "SELECT pg_catalog.pg_relation_filenode(
        'public.lifecycle_inherit_child_idx'::regclass);")"
    child_job_before="$(current_generation_job_id \
        "$(sql_super -c "SELECT
          'public.lifecycle_inherit_child_idx'::regclass::oid;")")"

    sql_super -c "ALTER ROLE durable_owner_two NOLOGIN;"
    sql_as durable_owner -c \
        "REINDEX TABLE public.lifecycle_inherit_parent;" >/dev/null 2>&1
    sql_super -c "ALTER ROLE durable_owner_two LOGIN;"

    parent_file_after="$(sql_super -c \
        "SELECT pg_catalog.pg_relation_filenode(${parent_index_oid});")"
    child_file_after="$(sql_super -c "SELECT pg_catalog.pg_relation_filenode(
        'public.lifecycle_inherit_child_idx'::regclass);")"
    child_job_after="$(current_generation_job_id \
        "$(sql_super -c "SELECT
          'public.lifecycle_inherit_child_idx'::regclass::oid;")")"
    if [ "${parent_file_after}" = "${parent_file_before}" ]; then
        error "ordinary parent REINDEX did not rebuild its own index"
    fi
    assert_eq "ordinary child index is not reindexed with its parent" \
        "${child_file_before}" "${child_file_after}"
    assert_eq "ordinary child workflow is not mistaken for parent work" \
        "${child_job_before}" "${child_job_after}"

    sql_super -c "DROP TABLE public.lifecycle_inherit_child,
                             public.lifecycle_inherit_parent;"
}

test_legacy_lineage_backfill() {
    local legacy_instance legacy_lineage legacy_oid owner_job
    local replacement_instance
    local owner_instance owner_lineage reindex_lineage reindex_oid_before
    local reindex_oid_after

    install_signal_probe
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_legacy_signal_docs
    (id integer, body text);
CREATE INDEX lifecycle_legacy_signal_idx
    ON public.lifecycle_legacy_signal_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
GRANT INSERT ON public.lifecycle_legacy_signal_docs TO durable_writer;

CREATE TABLE public.lifecycle_legacy_owner_docs (body text);
CREATE INDEX lifecycle_legacy_owner_idx
    ON public.lifecycle_legacy_owner_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');

CREATE TABLE public.lifecycle_legacy_reindex_docs (body text);
INSERT INTO public.lifecycle_legacy_reindex_docs VALUES ('legacy');
CREATE INDEX lifecycle_legacy_reindex_idx
    ON public.lifecycle_legacy_reindex_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL

    legacy_oid="$(sql_super -c "SELECT
        'public.lifecycle_legacy_signal_idx'::regclass::oid;")"
    legacy_instance="$(current_generation_job_id "${legacy_oid}")"
    wait_for_signal_node "${legacy_instance}" 30
    sql_super -c "UPDATE df.instances AS instance
      SET label = pg_catalog.format(
        'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%s',
        database.oid,
        relation.oid,
        coalesce(nullif(relation.reltablespace, 0),
                 database.dattablespace),
        pg_catalog.pg_relation_filenode(relation.oid),
        relation.relowner,
        pg_catalog.encode(
          pg_catalog.convert_to('0 0 1 1 *', 'UTF8'), 'hex'))
      FROM pg_catalog.pg_class AS relation,
           pg_catalog.pg_database AS database
      WHERE instance.id = '${legacy_instance}'
        AND relation.oid = ${legacy_oid}
        AND database.datname = pg_catalog.current_database();"
    remove_index_lineage public.lifecycle_legacy_signal_idx
    assert_eq "legacy signal setup removes lineage" "" \
        "$(index_lineage public.lifecycle_legacy_signal_idx)"

    reset_signal_probe
    sql_as durable_owner <<'SQL' >/dev/null
BEGIN;
INSERT INTO public.lifecycle_legacy_signal_docs
SELECT 1000 + document_number,
       (SELECT pg_catalog.string_agg(
                   pg_catalog.format(
                     'legacy%sterm%s', document_number, term_number),
                   ' ')
        FROM generate_series(1, 200) AS term_number)
FROM generate_series(1, 6) AS document_number;
SELECT bm25_spill_index('public.lifecycle_legacy_signal_idx');
INSERT INTO public.lifecycle_legacy_signal_docs
SELECT 2000 + document_number,
       (SELECT pg_catalog.string_agg(
                   pg_catalog.format(
                     'legacy%sterm%s', document_number, term_number),
                   ' ')
        FROM generate_series(201, 400) AS term_number)
FROM generate_series(1, 6) AS document_number;
SELECT bm25_spill_index('public.lifecycle_legacy_signal_idx');
COMMIT;
SQL
    legacy_lineage="$(index_lineage public.lifecycle_legacy_signal_idx)"
    assert_eq "legacy signal lazily assigns a 128-bit lineage" "32" \
        "${#legacy_lineage}"
    assert_eq "legacy signal dispatches the replacement workflow" "1" \
        "$(signal_attempt_count)"
    replacement_instance="$(sql_super -c "SELECT instance_id
      FROM public.compaction_signal_audit;")"
    assert_eq "legacy signal selects the replacement workflow" \
        "${replacement_instance}" \
        "$(current_generation_job_id "${legacy_oid}")"
    assert_eq "legacy migration does not eagerly cancel history" "t" \
        "$(sql_super -c "SELECT status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[])
          FROM df.instances WHERE id = '${legacy_instance}';")"
    sql_as durable_owner -c "SELECT df.signal_v028(
        '${legacy_instance}', 'compact', '{}');" >/dev/null
    wait_for_terminal "${legacy_instance}" 30
    log "PASS: stale legacy workflow retires through its current guard"

    owner_instance="$(current_generation_job_id \
        "$(sql_super -c "SELECT
          'public.lifecycle_legacy_owner_idx'::regclass::oid;")")"
    sql_super -c "UPDATE df.instances AS instance
      SET label = pg_catalog.format(
        'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%s',
        database.oid,
        relation.oid,
        coalesce(nullif(relation.reltablespace, 0),
                 database.dattablespace),
        pg_catalog.pg_relation_filenode(relation.oid),
        relation.relowner,
        pg_catalog.encode(
          pg_catalog.convert_to('1 2 3 4 *', 'UTF8'), 'hex'))
      FROM pg_catalog.pg_class AS relation,
           pg_catalog.pg_database AS database
      WHERE instance.id = '${owner_instance}'
        AND relation.oid =
            'public.lifecycle_legacy_owner_idx'::regclass
        AND database.datname = pg_catalog.current_database();"
    remove_index_lineage public.lifecycle_legacy_owner_idx
    sql_super -c "ALTER TABLE public.lifecycle_legacy_owner_docs
                   OWNER TO durable_owner_two;" >/dev/null 2>&1
    owner_lineage="$(index_lineage public.lifecycle_legacy_owner_idx)"
    assert_eq "legacy owner reconciliation backfills lineage" "32" \
        "${#owner_lineage}"
    owner_job="$(current_generation_job_id \
        "$(sql_super -c "SELECT
          'public.lifecycle_legacy_owner_idx'::regclass::oid;")")"
    assert_eq "legacy owner reconciliation uses the new owner" \
        "durable_owner_two" \
        "$(sql_super -c "SELECT submitted_by::pg_catalog.text
                          FROM df.instances
                          WHERE id = '${owner_job}';")"
    assert_eq "legacy owner reconciliation preserves its schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${owner_job}';")"

    reindex_oid_before="$(sql_super -c "SELECT
        'public.lifecycle_legacy_reindex_idx'::regclass::oid;")"
    remove_index_lineage public.lifecycle_legacy_reindex_idx
    sql_as durable_owner -c "REINDEX INDEX CONCURRENTLY
        public.lifecycle_legacy_reindex_idx;" >/dev/null 2>&1
    reindex_oid_after="$(sql_super -c "SELECT
        'public.lifecycle_legacy_reindex_idx'::regclass::oid;")"
    if [ "${reindex_oid_after}" = "${reindex_oid_before}" ]; then
        error "legacy concurrent REINDEX did not replace the index OID"
    fi
    reindex_lineage="$(index_lineage public.lifecycle_legacy_reindex_idx)"
    assert_eq "legacy concurrent REINDEX backfills lineage" "32" \
        "${#reindex_lineage}"
    if [ -z "$(current_generation_job_id "${reindex_oid_after}")" ]; then
        error "legacy concurrent REINDEX created no replacement workflow"
    fi

    restore_signal_probe
    sql_super -c "DROP TABLE public.lifecycle_legacy_signal_docs,
                             public.lifecycle_legacy_owner_docs,
                             public.lifecycle_legacy_reindex_docs;"
}

test_legacy_reconciliation_edges() {
    local index_oid job_after legacy_instance

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_legacy_noop_docs (body text);
        CREATE INDEX lifecycle_legacy_noop_idx
          ON public.lifecycle_legacy_noop_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_legacy_noop_idx'::regclass::oid;")"
    legacy_instance="$(current_generation_job_id "${index_oid}")"
    sql_super -c "UPDATE df.instances AS instance
      SET label = pg_catalog.format(
        'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%s',
        database.oid,
        relation.oid,
        coalesce(nullif(relation.reltablespace, 0),
                 database.dattablespace),
        pg_catalog.pg_relation_filenode(relation.oid),
        relation.relowner,
        pg_catalog.encode(
          pg_catalog.convert_to('1 2 3 4 *', 'UTF8'), 'hex'))
      FROM pg_catalog.pg_class AS relation,
           pg_catalog.pg_database AS database
      WHERE instance.id = '${legacy_instance}'
        AND relation.oid = ${index_oid}
        AND database.datname = pg_catalog.current_database();"
    remove_index_lineage public.lifecycle_legacy_noop_idx

    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_legacy_noop_idx
          SET TABLESPACE pg_default;" >/dev/null 2>&1
    job_after="$(current_generation_job_id "${index_oid}")"
    if [ -z "${job_after}" ]; then
        error "no-op rewrite did not migrate the legacy workflow"
    fi
    assert_eq "legacy migration preserves the captured schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(pg_catalog.convert_to(
                '1 2 3 4 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${job_after}';")"

    sql_super -c "ALTER ROLE durable_owner NOLOGIN;" >/dev/null
    if ! sql_super -c "
        ALTER TABLE public.lifecycle_legacy_noop_docs
          OWNER TO durable_owner_two;" >/dev/null 2>&1; then
        sql_super -c "ALTER ROLE durable_owner LOGIN;" >/dev/null
        error "ineligible old owner prevented ownership repair"
    fi
    sql_super -c "ALTER ROLE durable_owner LOGIN;" >/dev/null
    assert_eq "ineligible old owner can be replaced" "durable_owner_two" \
        "$(sql_super -c "SELECT pg_catalog.pg_get_userbyid(relowner)
          FROM pg_catalog.pg_class WHERE oid = ${index_oid};")"

    sql_super -c "DROP TABLE public.lifecycle_legacy_noop_docs;"
}

test_refresh_selects_requested_workflow() {
    local first_instance index_oid second_instance selected_instance
    local rollback_error

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_refresh_docs (body text);
CREATE INDEX lifecycle_refresh_idx
    ON public.lifecycle_refresh_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_refresh_idx'::regclass::oid;")"
    first_instance="$(current_generation_job_id "${index_oid}")"
    sql_super -c "REVOKE EXECUTE ON FUNCTION df.cancel(text, text)
                   FROM durable_owner;"

    if rollback_error="$(sql_as durable_owner <<'SQL' 2>&1
BEGIN;
ALTER INDEX public.lifecycle_refresh_idx
  SET (compaction_schedule = '5 4 3 2 *');
DO $body$
BEGIN
    RAISE EXCEPTION 'force admitted refresh rollback';
END
$body$;
COMMIT;
SQL
    )"; then
        error "admitted workflow refresh unexpectedly committed"
    fi
    if ! grep -Fq "force admitted refresh rollback" \
        <<<"${rollback_error}"; then
        error "workflow refresh failed before the rollback probe: \
${rollback_error}"
    fi
    assert_eq "rolled-back refresh preserves the prior current workflow" \
        "${first_instance}" \
        "$(current_generation_job_id "${index_oid}")"

    sql_as durable_owner -c "ALTER INDEX public.lifecycle_refresh_idx
      SET (compaction_schedule = '5 4 3 2 *');" >/dev/null 2>&1
    second_instance="$(current_generation_job_id "${index_oid}")"
    if [ -z "${second_instance}" ] ||
        [ "${second_instance}" = "${first_instance}" ]; then
        error "schedule refresh did not publish a new current workflow"
    fi
    assert_eq "schedule refresh keeps prior workflow for guard retirement" \
        "t" \
        "$(sql_super -c "SELECT status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[])
          FROM df.instances WHERE id = '${first_instance}';")"

    sql_as durable_owner -c "ALTER INDEX public.lifecycle_refresh_idx
      SET (compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    selected_instance="$(current_generation_job_id "${index_oid}")"
    if [ -z "${selected_instance}" ] ||
        [ "${selected_instance}" = "${first_instance}" ] ||
        [ "${selected_instance}" = "${second_instance}" ]; then
        error "schedule rollback did not republish the requested workflow"
    fi
    assert_eq "latest workflow carries the requested schedule" "t" \
        "$(sql_super -c "SELECT label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(
              pg_catalog.convert_to('0 0 1 1 *', 'UTF8'), 'hex'))
          FROM df.instances WHERE id = '${selected_instance}';")"

    sql_super -c "GRANT EXECUTE ON FUNCTION df.cancel(text, text)
                   TO durable_owner;"
    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'schedule refresh test complete')
      FROM df.instances AS instance
      WHERE instance.label OPERATOR(pg_catalog.~~)
            'pg_textsearch:bg:v1:%:${index_oid}:%'
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    sql_super -c "DROP TABLE public.lifecycle_refresh_docs;"
}

test_concurrent_legacy_lineage_backfill() {
    local final_lineage first_output first_pid first_status=0
    local gate_pid index_oid legacy_instance
    local second_output second_pid second_status=0

    first_output="${DATA_DIR}/legacy-backfill-first.out"
    second_output="${DATA_DIR}/legacy-backfill-second.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_legacy_race_docs
    (id integer, body text);
CREATE INDEX lifecycle_legacy_race_idx
    ON public.lifecycle_legacy_race_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_legacy_race_idx'::regclass::oid;")"
    legacy_instance="$(current_generation_job_id "${index_oid}")"
    wait_for_signal_node "${legacy_instance}" 30
    sql_super -c "UPDATE df.instances AS instance
      SET label = pg_catalog.format(
        'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%s',
        database.oid,
        relation.oid,
        coalesce(nullif(relation.reltablespace, 0),
                 database.dattablespace),
        pg_catalog.pg_relation_filenode(relation.oid),
        relation.relowner,
        pg_catalog.encode(
          pg_catalog.convert_to('0 0 1 1 *', 'UTF8'), 'hex'))
      FROM pg_catalog.pg_class AS relation,
           pg_catalog.pg_database AS database
      WHERE instance.id = '${legacy_instance}'
        AND relation.oid = ${index_oid}
        AND database.datname = pg_catalog.current_database();"
    remove_index_lineage public.lifecycle_legacy_race_idx

    PGAPPNAME=lifecycle-legacy-race-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 6);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/legacy-backfill-gate.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 6
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-legacy-race-first \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            INSERT INTO public.lifecycle_legacy_race_docs
            SELECT 1000 + document_number,
                   pg_catalog.format(
                     'first writer document %s filler', document_number)
            FROM generate_series(1, 20) AS document_number;
            SELECT bm25_spill_index(
              'public.lifecycle_legacy_race_idx');
            INSERT INTO public.lifecycle_legacy_race_docs
            SELECT 2000 + document_number,
                   pg_catalog.format(
                     'first writer second %s filler', document_number)
            FROM generate_series(1, 20) AS document_number;
            SELECT bm25_spill_index(
              'public.lifecycle_legacy_race_idx');
            SELECT pg_catalog.pg_advisory_xact_lock_shared(478, 6);
            COMMIT;" >"${first_output}" 2>&1 &
    first_pid=$!
    PGAPPNAME=lifecycle-legacy-race-second \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            INSERT INTO public.lifecycle_legacy_race_docs
            SELECT 3000 + document_number,
                   pg_catalog.format(
                     'second writer document %s filler', document_number)
            FROM generate_series(1, 20) AS document_number;
            SELECT bm25_spill_index(
              'public.lifecycle_legacy_race_idx');
            INSERT INTO public.lifecycle_legacy_race_docs
            SELECT 4000 + document_number,
                   pg_catalog.format(
                     'second writer second %s filler', document_number)
            FROM generate_series(1, 20) AS document_number;
            SELECT bm25_spill_index(
              'public.lifecycle_legacy_race_idx');
            SELECT pg_catalog.pg_advisory_xact_lock_shared(478, 6);
            COMMIT;" >"${second_output}" 2>&1 &
    second_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name IN (
                'lifecycle-legacy-race-first',
                'lifecycle-legacy-race-second')
                AND wait_event = 'advisory';")" = "2" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "concurrent legacy writers reach the commit gate" "2" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name IN (
            'lifecycle-legacy-race-first',
            'lifecycle-legacy-race-second')
            AND wait_event = 'advisory';")"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-legacy-race-gate';" >/dev/null
    wait "${gate_pid}" || true
    wait "${first_pid}" || first_status=$?
    wait "${second_pid}" || second_status=$?
    if [ "${first_status}" -ne 0 ] || [ "${second_status}" -ne 0 ]; then
        error "concurrent legacy backfill failed:
first: $(cat "${first_output}")
second: $(cat "${second_output}")"
    fi
    assert_eq "both concurrent legacy writers commit" "80" \
        "$(sql_super -c "SELECT count(*)
          FROM public.lifecycle_legacy_race_docs;")"
    final_lineage="$(index_lineage public.lifecycle_legacy_race_idx)"
    assert_eq "concurrent legacy writers converge on one 128-bit lineage" \
        "32" "${#final_lineage}"
    assert_eq "concurrent legacy writers leave one lineage workflow" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM df.instances AS instance
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = ${index_oid}
          JOIN pg_catalog.pg_database AS database
            ON database.datname = pg_catalog.current_database()
          WHERE instance.label OPERATOR(pg_catalog.~~)
                pg_catalog.format(
                  'pg_textsearch:bg:v1:%s:%s:%s:%s:%s:%%:%s:%%',
                  database.oid,
                  relation.oid,
                  coalesce(nullif(relation.reltablespace, 0),
                           database.dattablespace),
                  pg_catalog.pg_relation_filenode(relation.oid),
                  relation.relowner,
                  '${final_lineage}')
            AND instance.submitted_by::pg_catalog.oid = relation.relowner
            AND instance.status OPERATOR(pg_catalog.=)
                ANY (ARRAY['pending', 'running']::pg_catalog.text[]);")"

    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'legacy race test complete')
      FROM df.instances AS instance
      WHERE instance.label OPERATOR(pg_catalog.~~)
            'pg_textsearch:bg:v1:%:${index_oid}:%'
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    sql_super -c "DROP TABLE public.lifecycle_legacy_race_docs;"
}

test_legacy_reindex_spill_lock_order() {
    local blocker_output blocker_pid final_lineage first_oid
    local reindex_output reindex_pid
    local reindex_status=0 spill_output spill_pid spill_status=0 target_oid
    local target_oid_after

    blocker_output="${DATA_DIR}/legacy-lock-blocker.out"
    reindex_output="${DATA_DIR}/legacy-lock-reindex.out"
    spill_output="${DATA_DIR}/legacy-lock-spill.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_legacy_lock_docs
    (id integer, body text);
CREATE INDEX lifecycle_legacy_lock_first_idx
    ON public.lifecycle_legacy_lock_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE INDEX lifecycle_legacy_lock_target_idx
    ON public.lifecycle_legacy_lock_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    first_oid="$(sql_super -c "SELECT
        'public.lifecycle_legacy_lock_first_idx'::regclass::oid;")"
    target_oid="$(sql_super -c "SELECT
        'public.lifecycle_legacy_lock_target_idx'::regclass::oid;")"
    remove_index_lineage public.lifecycle_legacy_lock_first_idx
    remove_index_lineage public.lifecycle_legacy_lock_target_idx

    PGAPPNAME=lifecycle-legacy-lock-blocker sql_super -c "
        BEGIN;
        UPDATE pg_catalog.pg_class
        SET reloptions = reloptions
        WHERE oid = ${first_oid};
        SELECT pg_catalog.pg_sleep(120);" \
        >"${blocker_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-legacy-lock-blocker'
                AND wait_event = 'PgSleep';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "legacy lock blocker holds the first catalog row" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-legacy-lock-blocker'
            AND wait_event = 'PgSleep';")"

    PGAPPNAME=lifecycle-legacy-lock-reindex \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "REINDEX TABLE CONCURRENTLY
              public.lifecycle_legacy_lock_docs;" \
        >"${reindex_output}" 2>&1 &
    reindex_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              WHERE activity.application_name =
                    'lifecycle-legacy-lock-reindex'
                AND activity.wait_event_type = 'Lock'
                AND EXISTS (
                  SELECT 1
                  FROM pg_catalog.pg_locks AS relation_lock
                  WHERE relation_lock.pid = activity.pid
                    AND relation_lock.locktype = 'relation'
                    AND relation_lock.relation = ${target_oid}
                    AND relation_lock.mode = 'ShareUpdateExclusiveLock'
                    AND relation_lock.granted);")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "tracked REINDEX holds the target before legacy backfill" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          WHERE activity.application_name =
                'lifecycle-legacy-lock-reindex'
            AND activity.wait_event_type = 'Lock'
            AND EXISTS (
              SELECT 1
              FROM pg_catalog.pg_locks AS relation_lock
              WHERE relation_lock.pid = activity.pid
                AND relation_lock.locktype = 'relation'
                AND relation_lock.relation = ${target_oid}
                AND relation_lock.mode = 'ShareUpdateExclusiveLock'
                AND relation_lock.granted);")"

    PGAPPNAME=lifecycle-legacy-lock-spill \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            INSERT INTO public.lifecycle_legacy_lock_docs
            SELECT document_number,
                   pg_catalog.format(
                     'legacy lock first %s filler', document_number)
            FROM generate_series(1, 20) AS document_number;
            SELECT bm25_spill_index(
              'public.lifecycle_legacy_lock_target_idx');
            INSERT INTO public.lifecycle_legacy_lock_docs
            SELECT 100 + document_number,
                   pg_catalog.format(
                     'legacy lock second %s filler', document_number)
            FROM generate_series(1, 20) AS document_number;
            SELECT bm25_spill_index(
              'public.lifecycle_legacy_lock_target_idx');
            COMMIT;" >"${spill_output}" 2>&1 &
    spill_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS relation_lock
                ON relation_lock.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-legacy-lock-spill'
                AND relation_lock.locktype = 'relation'
                AND relation_lock.relation = ${target_oid}
                AND relation_lock.mode = 'ShareUpdateExclusiveLock'
                AND NOT relation_lock.granted;")" = "1" ] ||
            [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-legacy-lock-spill';")" = "0" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "contended legacy spill does not hold the private index lock" \
        "0" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS private_lock
            ON private_lock.pid = activity.pid
          WHERE activity.application_name = 'lifecycle-legacy-lock-spill'
            AND private_lock.locktype = 'object'
            AND private_lock.classid =
                'pg_catalog.pg_am'::pg_catalog.regclass
            AND private_lock.objid = ${target_oid}
            AND private_lock.objsubid = 1
            AND private_lock.mode = 'ExclusiveLock'
            AND private_lock.granted;")"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-legacy-lock-blocker';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${reindex_pid}" || reindex_status=$?
    wait "${spill_pid}" || spill_status=$?
    if [ "${reindex_status}" -ne 0 ] || [ "${spill_status}" -ne 0 ]; then
        error "legacy REINDEX/spill lock race failed:
reindex: $(cat "${reindex_output}")
spill: $(cat "${spill_output}")"
    fi

    assert_eq "legacy REINDEX/spill preserves writer rows" "40" \
        "$(sql_super -c "SELECT count(*)
          FROM public.lifecycle_legacy_lock_docs;")"
    target_oid_after="$(sql_super -c "SELECT
        'public.lifecycle_legacy_lock_target_idx'::regclass::oid;")"
    final_lineage="$(
        index_lineage public.lifecycle_legacy_lock_target_idx
    )"
    assert_eq "legacy REINDEX/spill converges on one lineage" "32" \
        "${#final_lineage}"
    assert_eq "legacy REINDEX/spill leaves one current workflow" "1" \
        "$(current_generation_job_count "${target_oid_after}")"

    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'legacy lock race complete')
      FROM df.instances AS instance
      WHERE instance.submitted_by = 'durable_owner'::regrole
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[])
        AND (instance.label OPERATOR(pg_catalog.~~)
               'pg_textsearch:bg:v1:%:${target_oid_after}:%');" >/dev/null
    sql_super -c "DROP TABLE public.lifecycle_legacy_lock_docs;"
}

test_managed_lock_order() {
    local blocker_output blocker_pid index_oid reconcile_completed=false
    local reconcile_output reconcile_pid reconcile_status=0
    local signal_completed=false signal_output signal_pid signal_status=0

    blocker_output="${DATA_DIR}/managed-lock-blocker.out"
    reconcile_output="${DATA_DIR}/managed-lock-reconcile.out"
    signal_output="${DATA_DIR}/managed-lock-signal.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_managed_lock_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_managed_lock_leaf
    PARTITION OF public.lifecycle_managed_lock_docs
    FOR VALUES FROM (0) TO (1000);
CREATE INDEX lifecycle_managed_lock_leaf_idx
    ON public.lifecycle_managed_lock_leaf USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
INSERT INTO public.lifecycle_managed_lock_leaf
SELECT document_number,
       pg_catalog.format('managed lock first %s filler', document_number)
FROM generate_series(1, 20) AS document_number;
SELECT bm25_spill_index('public.lifecycle_managed_lock_leaf_idx');
INSERT INTO public.lifecycle_managed_lock_leaf
SELECT 100 + document_number,
       pg_catalog.format('managed lock second %s filler', document_number)
FROM generate_series(1, 20) AS document_number;
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_managed_lock_leaf_idx'::regclass::oid;")"

    PGAPPNAME=lifecycle-managed-lock-blocker sql_super -c "
        BEGIN;
        COMMENT ON ACCESS METHOD bm25 IS 'managed lock gate';
        SELECT pg_catalog.pg_sleep(120);" \
        >"${blocker_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-managed-lock-blocker'
                AND wait_event = 'PgSleep';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "managed lock blocker holds the dependency object" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS dependency
            ON dependency.pid = activity.pid
          JOIN pg_catalog.pg_am AS access_method
            ON access_method.oid = dependency.objid
          WHERE activity.application_name =
                'lifecycle-managed-lock-blocker'
            AND activity.wait_event = 'PgSleep'
            AND dependency.locktype = 'object'
            AND dependency.classid =
                'pg_catalog.pg_am'::pg_catalog.regclass
            AND access_method.amname = 'bm25'
            AND dependency.objsubid = 0
            AND dependency.mode = 'ShareUpdateExclusiveLock'
            AND dependency.granted;")"

    PGAPPNAME=lifecycle-managed-lock-signal \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "SET statement_timeout = '15s';
            BEGIN;
            SELECT bm25_spill_index(
              'public.lifecycle_managed_lock_leaf_idx');
            COMMIT;" >"${signal_output}" 2>&1 &
    signal_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-managed-lock-signal';")" = "0" ]; then
            signal_completed=true
            break
        fi
        sleep 0.1
    done
    if [ "${signal_completed}" = "true" ]; then
        wait "${signal_pid}" || signal_status=$?
    fi

    PGAPPNAME=lifecycle-managed-lock-reconcile \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "SET statement_timeout = '15s';
            CREATE INDEX lifecycle_managed_lock_parent_idx
              ON public.lifecycle_managed_lock_docs USING bm25(body)
              WITH (text_config = 'english',
                    compaction = 'background',
                    compaction_schedule = '5 4 3 2 *');" \
        >"${reconcile_output}" 2>&1 &
    reconcile_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-managed-lock-reconcile';")" = "0" ]; then
            reconcile_completed=true
            break
        fi
        sleep 0.1
    done
    if [ "${reconcile_completed}" = "true" ]; then
        wait "${reconcile_pid}" || reconcile_status=$?
    fi

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-managed-lock-blocker';" >/dev/null
    wait "${blocker_pid}" || true
    if [ "${signal_completed}" != "true" ]; then
        wait "${signal_pid}" || signal_status=$?
    fi
    if [ "${reconcile_completed}" != "true" ]; then
        wait "${reconcile_pid}" || reconcile_status=$?
    fi
    if [ "${signal_completed}" != "true" ] ||
        [ "${reconcile_completed}" != "true" ] ||
        [ "${reconcile_status}" -ne 0 ] || [ "${signal_status}" -ne 0 ]; then
        error "managed lifecycle lock ordering failed:
reconcile: $(cat "${reconcile_output}")
signal: $(cat "${signal_output}")"
    fi
    if grep -Fq "deadlock detected" "${reconcile_output}" ||
        grep -Fq "deadlock detected" "${signal_output}"; then
        error "managed lifecycle lock ordering reported a caught deadlock:
reconcile: $(cat "${reconcile_output}")
signal: $(cat "${signal_output}")"
    fi
    if ! grep -Fq \
        "background compaction lifecycle reconciliation was deferred" \
        "${reconcile_output}"; then
        error "contended partition reconciliation was not reported:
$(cat "${reconcile_output}")"
    fi
    log "PASS: PRECOMMIT managed paths defer instead of waiting"
    assert_eq "partition option reconciliation completes" "t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
            ':',
            reloptions @> ARRAY['compaction=background'],
            reloptions @>
              ARRAY['compaction_schedule=5 4 3 2 *'])
          FROM pg_catalog.pg_class WHERE oid = ${index_oid};")"
    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_managed_lock_leaf_idx
          SET (compaction_schedule = '5 4 3 2 *');" >/dev/null
    assert_eq "deferred partition schedule is retryable" "t" \
        "$(sql_super -c "SELECT EXISTS (
          SELECT 1 FROM df.instances
          WHERE label OPERATOR(pg_catalog.~~)
                ('pg_textsearch:bg:v1:%:${index_oid}:%:' ||
                 pg_catalog.encode(
                   pg_catalog.convert_to('5 4 3 2 *', 'UTF8'), 'hex'))
            AND status OPERATOR(pg_catalog.=)
                ANY (ARRAY['pending', 'running']::pg_catalog.text[]));")"

    sql_super -c "DROP TABLE public.lifecycle_managed_lock_docs;"
}

test_alter_reindex_lock_order() {
    local alter_admission_before alter_output alter_pid alter_status=0
    local alter_waited=false gate_pid index_oid reindex_inverse
    local reindex_output reindex_pid reindex_status=0

    alter_output="${DATA_DIR}/alter-reindex-alter.out"
    reindex_output="${DATA_DIR}/alter-reindex-reindex.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_alter_reindex_docs (body text);
CREATE INDEX lifecycle_alter_reindex_idx
    ON public.lifecycle_alter_reindex_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_uppercase_background_docs (body text);
CREATE INDEX lifecycle_uppercase_background_idx
    ON public.lifecycle_uppercase_background_docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
CREATE FUNCTION public.lifecycle_alter_reindex_pause()
RETURNS event_trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-alter-reindex-alter' THEN
        PERFORM pg_catalog.pg_advisory_xact_lock_shared(478, 14);
    END IF;
END
$body$;
SQL
    sql_super -c "
        CREATE EVENT TRIGGER lifecycle_alter_reindex_pause
          ON ddl_command_start
          WHEN TAG IN ('ALTER INDEX')
          EXECUTE FUNCTION public.lifecycle_alter_reindex_pause();" \
        >/dev/null
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_alter_reindex_idx'::regclass::oid;")"

    PGAPPNAME=lifecycle-alter-reindex-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 14);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/alter-reindex-gate.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-alter-reindex-gate'
                AND wait_event = 'PgSleep';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-alter-reindex-alter \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "ALTER INDEX public.lifecycle_alter_reindex_idx
              SET (compaction_schedule = '1 0 1 1 *');" \
        >"${alter_output}" 2>&1 &
    alter_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-alter-reindex-alter'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "managed ALTER reaches the pre-core barrier" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name =
                'lifecycle-alter-reindex-alter'
            AND wait_event = 'advisory';")"
    alter_admission_before="$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS admission
            ON admission.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-alter-reindex-alter'
            AND activity.wait_event = 'advisory'
            AND admission.locktype = 'object'
            AND admission.classid =
                'pg_catalog.pg_am'::pg_catalog.regclass
            AND admission.objid = ${index_oid}
            AND admission.objsubid = 1
            AND admission.mode = 'ExclusiveLock'
            AND admission.granted;")"

    if [ "${alter_admission_before}" = "0" ]; then
        sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name =
                'lifecycle-alter-reindex-gate';" >/dev/null
        wait "${gate_pid}" || true
        wait "${alter_pid}" || alter_status=$?
        alter_waited=true
    fi

    PGAPPNAME=lifecycle-alter-reindex-reindex \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "REINDEX INDEX CONCURRENTLY
              public.lifecycle_alter_reindex_idx;" \
        >"${reindex_output}" 2>&1 &
    reindex_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS admission
                ON admission.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-alter-reindex-reindex'
                AND admission.locktype = 'object'
                AND admission.classid =
                    'pg_catalog.pg_am'::pg_catalog.regclass
                AND admission.objid = ${index_oid}
                AND admission.objsubid = 1
                AND admission.mode = 'ExclusiveLock'
                AND NOT admission.granted;")" = "1" ]; then
            break
        fi
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-alter-reindex-reindex';")" = "0" ]; then
            break
        fi
        sleep 0.1
    done
    reindex_inverse="$(sql_super -c "SELECT
            count(*) FILTER (
              WHERE relation_lock.locktype = 'relation'
                AND relation_lock.relation = ${index_oid}
                AND relation_lock.mode = 'ShareUpdateExclusiveLock'
                AND relation_lock.granted)
            || ':' ||
            count(*) FILTER (
              WHERE relation_lock.locktype = 'object'
                AND relation_lock.classid =
                    'pg_catalog.pg_am'::pg_catalog.regclass
                AND relation_lock.objid = ${index_oid}
                AND relation_lock.objsubid = 1
                AND relation_lock.mode = 'ExclusiveLock'
                AND NOT relation_lock.granted)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS relation_lock
            ON relation_lock.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-alter-reindex-reindex';")"
    if [ "${alter_admission_before}" = "1" ]; then
        assert_eq "old-order REINDEX reaches the inverse lock edge" "1:1" \
            "${reindex_inverse}"
        sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name =
                'lifecycle-alter-reindex-gate';" >/dev/null
        wait "${gate_pid}" || true
    fi
    if [ "${alter_waited}" = "false" ]; then
        wait "${alter_pid}" || alter_status=$?
    fi
    wait "${reindex_pid}" || reindex_status=$?
    if [ "${alter_status}" -ne 0 ] || [ "${reindex_status}" -ne 0 ] ||
        grep -Fq "deadlock detected" "${alter_output}" ||
        grep -Fq "deadlock detected" "${reindex_output}"; then
        error "ALTER/concurrent REINDEX lock order failed:
alter: $(cat "${alter_output}")
reindex: $(cat "${reindex_output}")"
    fi
    assert_eq "managed ALTER takes no admission before core relation lock" \
        "0" "${alter_admission_before}"
    assert_eq "ALTER schedule survives concurrent REINDEX" "t" \
        "$(sql_super -c "SELECT reloptions @>
          ARRAY['compaction_schedule=1 0 1 1 *']
          FROM pg_catalog.pg_class WHERE oid =
            'public.lifecycle_alter_reindex_idx'::regclass;")"

    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_uppercase_background_idx
          SET (compaction = 'BACKGROUND');" >/dev/null
    assert_eq "accepted mixed-case background option activates" "1" \
        "$(active_jobs_for_index "$(sql_super -c "SELECT
          'public.lifecycle_uppercase_background_idx'::regclass::oid;")")"

    sql_super -c "
        DROP EVENT TRIGGER lifecycle_alter_reindex_pause;
        DROP FUNCTION public.lifecycle_alter_reindex_pause();
        DROP TABLE public.lifecycle_alter_reindex_docs,
                   public.lifecycle_uppercase_background_docs;" >/dev/null
}

test_cross_statement_reindex_lock_order() {
    local am_oid dependency_before first_output first_pid first_status=0
    local gate_pid second_index_oid second_output
    local second_pid second_status=0

    first_output="${DATA_DIR}/cross-reindex-first.out"
    second_output="${DATA_DIR}/cross-reindex-second.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_cross_reindex_first_docs (body text);
CREATE INDEX lifecycle_cross_reindex_first_idx
    ON public.lifecycle_cross_reindex_first_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_cross_reindex_second_docs (body text);
CREATE INDEX lifecycle_cross_reindex_second_idx
    ON public.lifecycle_cross_reindex_second_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    second_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_cross_reindex_second_idx'::regclass::oid;")"
    am_oid="$(sql_super -c \
        "SELECT oid FROM pg_catalog.pg_am WHERE amname = 'bm25';")"

    PGAPPNAME=lifecycle-cross-reindex-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 15);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/cross-reindex-gate.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-cross-reindex-gate'
                AND wait_event = 'PgSleep';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-cross-reindex-first \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            REINDEX INDEX public.lifecycle_cross_reindex_first_idx;
            SELECT pg_catalog.pg_advisory_xact_lock(478, 15);
            REINDEX INDEX public.lifecycle_cross_reindex_second_idx;
            COMMIT;" >"${first_output}" 2>&1 &
    first_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-cross-reindex-first'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "first REINDEX transaction reaches the statement barrier" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-cross-reindex-first'
            AND wait_event = 'advisory';")"
    dependency_before="$(sql_super -c "SELECT count(*)
      FROM pg_catalog.pg_stat_activity AS activity
      JOIN pg_catalog.pg_locks AS dependency
        ON dependency.pid = activity.pid
      WHERE activity.application_name = 'lifecycle-cross-reindex-first'
        AND dependency.locktype = 'object'
        AND dependency.classid =
            'pg_catalog.pg_am'::pg_catalog.regclass
        AND dependency.objid = ${am_oid}
        AND dependency.objsubid = 0
        AND dependency.mode = 'ShareRowExclusiveLock'
        AND dependency.granted;")"

    PGAPPNAME=lifecycle-cross-reindex-second \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            REINDEX INDEX public.lifecycle_cross_reindex_second_idx;
            COMMIT;" >"${second_output}" 2>&1 &
    second_pid=$!
    if [ "${dependency_before}" = "1" ]; then
        for _ in $(seq 1 100); do
            if [ "$(sql_super -c "SELECT count(*)
                  FROM pg_catalog.pg_stat_activity AS activity
                  JOIN pg_catalog.pg_locks AS dependency
                    ON dependency.pid = activity.pid
                  WHERE activity.application_name =
                        'lifecycle-cross-reindex-second'
                    AND dependency.locktype = 'object'
                    AND dependency.classid =
                        'pg_catalog.pg_am'::pg_catalog.regclass
                    AND dependency.objid = ${am_oid}
                    AND dependency.objsubid = 0
                    AND dependency.mode = 'ShareRowExclusiveLock'
                    AND NOT dependency.granted;")" = "1" ]; then
                break
            fi
            sleep 0.1
        done
        assert_eq "inverse REINDEX holds relation while waiting dependency" \
            "1:1" \
            "$(sql_super -c "SELECT
                count(*) FILTER (
                  WHERE managed.locktype = 'relation'
                    AND managed.relation = ${second_index_oid}
                    AND managed.mode = 'AccessExclusiveLock'
                    AND managed.granted)
                || ':' ||
                count(*) FILTER (
                  WHERE managed.locktype = 'object'
                    AND managed.classid =
                        'pg_catalog.pg_am'::pg_catalog.regclass
                    AND managed.objid = ${am_oid}
                    AND managed.objsubid = 0
                    AND managed.mode = 'ShareRowExclusiveLock'
                    AND NOT managed.granted)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS managed
                ON managed.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-cross-reindex-second';")"
    fi

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-cross-reindex-gate';" >/dev/null
    wait "${gate_pid}" || true
    wait "${first_pid}" || first_status=$?
    wait "${second_pid}" || second_status=$?
    if [ "${first_status}" -ne 0 ] || [ "${second_status}" -ne 0 ] ||
        grep -Fq "deadlock detected" "${first_output}" ||
        grep -Fq "deadlock detected" "${second_output}"; then
        error "cross-statement REINDEX lock order failed:
first: $(cat "${first_output}")
second: $(cat "${second_output}")"
    fi
    assert_eq "REINDEX defers dependency work to the terminal batch" \
        "0" "${dependency_before}"
    assert_eq "both REINDEX workflows remain current" "1:1" \
        "$(current_generation_job_count "$(sql_super -c "SELECT
          'public.lifecycle_cross_reindex_first_idx'::regclass::oid;")"):$(
          current_generation_job_count "$(sql_super -c "SELECT
          'public.lifecycle_cross_reindex_second_idx'::regclass::oid;")")"

    sql_super -c "
        DROP TABLE public.lifecycle_cross_reindex_first_docs,
                   public.lifecycle_cross_reindex_second_docs;" >/dev/null
}

test_cross_statement_owner_lock_order() {
    local am_oid dependency_before first_output first_pid first_status=0
    local gate_pid second_heap_oid second_output
    local second_pid second_status=0

    first_output="${DATA_DIR}/cross-owner-first.out"
    second_output="${DATA_DIR}/cross-owner-second.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_cross_owner_first_docs (body text);
CREATE INDEX lifecycle_cross_owner_first_idx
    ON public.lifecycle_cross_owner_first_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_cross_owner_second_docs (body text);
CREATE INDEX lifecycle_cross_owner_second_idx
    ON public.lifecycle_cross_owner_second_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    second_heap_oid="$(sql_super -c "SELECT
        'public.lifecycle_cross_owner_second_docs'::regclass::oid;")"
    am_oid="$(sql_super -c \
        "SELECT oid FROM pg_catalog.pg_am WHERE amname = 'bm25';")"

    PGAPPNAME=lifecycle-cross-owner-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 16);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/cross-owner-gate.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-cross-owner-gate'
                AND wait_event = 'PgSleep';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-cross-owner-first \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            ALTER TABLE public.lifecycle_cross_owner_first_docs
              OWNER TO durable_owner_two;
            SELECT pg_catalog.pg_advisory_xact_lock(478, 16);
            ALTER TABLE public.lifecycle_cross_owner_second_docs
              OWNER TO durable_owner_two;
            COMMIT;" >"${first_output}" 2>&1 &
    first_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-cross-owner-first'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "first owner transaction reaches the statement barrier" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-cross-owner-first'
            AND wait_event = 'advisory';")"
    dependency_before="$(sql_super -c "SELECT count(*)
      FROM pg_catalog.pg_stat_activity AS activity
      JOIN pg_catalog.pg_locks AS dependency
        ON dependency.pid = activity.pid
      WHERE activity.application_name = 'lifecycle-cross-owner-first'
        AND dependency.locktype = 'object'
        AND dependency.classid =
            'pg_catalog.pg_am'::pg_catalog.regclass
        AND dependency.objid = ${am_oid}
        AND dependency.objsubid = 0
        AND dependency.mode = 'ShareRowExclusiveLock'
        AND dependency.granted;")"

    PGAPPNAME=lifecycle-cross-owner-second \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            ALTER TABLE public.lifecycle_cross_owner_second_docs
              OWNER TO durable_owner_two;
            COMMIT;" >"${second_output}" 2>&1 &
    second_pid=$!
    if [ "${dependency_before}" = "1" ]; then
        for _ in $(seq 1 100); do
            if [ "$(sql_super -c "SELECT count(*)
                  FROM pg_catalog.pg_stat_activity AS activity
                  JOIN pg_catalog.pg_locks AS dependency
                    ON dependency.pid = activity.pid
                  WHERE activity.application_name =
                        'lifecycle-cross-owner-second'
                    AND dependency.locktype = 'object'
                    AND dependency.classid =
                        'pg_catalog.pg_am'::pg_catalog.regclass
                    AND dependency.objid = ${am_oid}
                    AND dependency.objsubid = 0
                    AND dependency.mode = 'ShareRowExclusiveLock'
                    AND NOT dependency.granted;")" = "1" ]; then
                break
            fi
            sleep 0.1
        done
        assert_eq "inverse owner change holds heap while waiting dependency" \
            "1:1" \
            "$(sql_super -c "SELECT
                count(*) FILTER (
                  WHERE managed.locktype = 'relation'
                    AND managed.relation = ${second_heap_oid}
                    AND managed.mode = 'AccessExclusiveLock'
                    AND managed.granted)
                || ':' ||
                count(*) FILTER (
                  WHERE managed.locktype = 'object'
                    AND managed.classid =
                        'pg_catalog.pg_am'::pg_catalog.regclass
                    AND managed.objid = ${am_oid}
                    AND managed.objsubid = 0
                    AND managed.mode = 'ShareRowExclusiveLock'
                    AND NOT managed.granted)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS managed
                ON managed.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-cross-owner-second';")"
    fi

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-cross-owner-gate';" >/dev/null
    wait "${gate_pid}" || true
    wait "${first_pid}" || first_status=$?
    wait "${second_pid}" || second_status=$?
    if [ "${first_status}" -ne 0 ] || [ "${second_status}" -ne 0 ] ||
        grep -Fq "deadlock detected" "${first_output}" ||
        grep -Fq "deadlock detected" "${second_output}"; then
        error "cross-statement owner lock order failed:
first: $(cat "${first_output}")
second: $(cat "${second_output}")"
    fi
    assert_eq "owner changes defer dependency work to the terminal batch" \
        "0" "${dependency_before}"
    assert_eq "both owner changes preserve current workflows" "1:1" \
        "$(current_generation_job_count "$(sql_super -c "SELECT
          'public.lifecycle_cross_owner_first_idx'::regclass::oid;")"):$(
          current_generation_job_count "$(sql_super -c "SELECT
          'public.lifecycle_cross_owner_second_idx'::regclass::oid;")")"
    assert_eq "both owner changes complete" \
        "durable_owner_two:durable_owner_two" \
        "$(sql_super -c "SELECT
          pg_catalog.pg_get_userbyid(
            (SELECT relowner FROM pg_catalog.pg_class
             WHERE oid =
               'public.lifecycle_cross_owner_first_docs'::regclass))
          || ':' ||
          pg_catalog.pg_get_userbyid(
            (SELECT relowner FROM pg_catalog.pg_class
             WHERE oid =
               'public.lifecycle_cross_owner_second_docs'::regclass));")"

    sql_super -c "
        DROP TABLE public.lifecycle_cross_owner_first_docs,
                   public.lifecycle_cross_owner_second_docs;" >/dev/null
}

test_multi_family_partition_attach_batch() {
    local attach_output
    local child_a_oid child_b_oid parent_a_lineage parent_b_lineage

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_multi_family_parent (
    id integer,
    body_a text,
    body_b text
) PARTITION BY RANGE (id);
CREATE INDEX lifecycle_multi_family_parent_a_idx
    ON public.lifecycle_multi_family_parent USING bm25(body_a)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '1 0 1 1 *');
CREATE INDEX lifecycle_multi_family_parent_b_idx
    ON public.lifecycle_multi_family_parent USING bm25(body_b)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '2 0 1 1 *');
CREATE TABLE public.lifecycle_multi_family_child (
    id integer,
    body_a text,
    body_b text
);
SQL
    if ! attach_output="$(sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_multi_family_parent
          ATTACH PARTITION public.lifecycle_multi_family_child
          FOR VALUES FROM (0) TO (100);" 2>&1)"; then
        error "two-family partition attachment failed: ${attach_output}"
    fi

    child_a_oid="$(sql_super -c "SELECT inhrelid
      FROM pg_catalog.pg_inherits
      WHERE inhparent =
        'public.lifecycle_multi_family_parent_a_idx'::regclass;")"
    child_b_oid="$(sql_super -c "SELECT inhrelid
      FROM pg_catalog.pg_inherits
      WHERE inhparent =
        'public.lifecycle_multi_family_parent_b_idx'::regclass;")"
    parent_a_lineage="$(
        index_lineage public.lifecycle_multi_family_parent_a_idx
    )"
    parent_b_lineage="$(
        index_lineage public.lifecycle_multi_family_parent_b_idx
    )"

    assert_eq "two-family attachment creates both physical indexes" "t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
          ':',
          ${child_a_oid} <> ${child_b_oid},
          (SELECT count(*) FROM pg_catalog.pg_class
           WHERE oid IN (${child_a_oid}, ${child_b_oid})) = 2);")"
    assert_eq "first attached family inherits lifecycle options" \
        "${parent_a_lineage}:true" \
        "$(sql_super -c "SELECT
          pg_catalog.substr(
            lineage_option,
            pg_catalog.length('compaction_lineage=') + 1)
          || ':' ||
          (relation.reloptions @>
             ARRAY['compaction_schedule=1 0 1 1 *'])::text
          FROM pg_catalog.pg_class AS relation
          CROSS JOIN LATERAL pg_catalog.unnest(
            relation.reloptions) AS lineage_option
          WHERE relation.oid = ${child_a_oid}
            AND lineage_option OPERATOR(pg_catalog.~~)
                'compaction_lineage=%';")"
    assert_eq "second attached family inherits lifecycle options" \
        "${parent_b_lineage}:true" \
        "$(sql_super -c "SELECT
          pg_catalog.substr(
            lineage_option,
            pg_catalog.length('compaction_lineage=') + 1)
          || ':' ||
          (relation.reloptions @>
             ARRAY['compaction_schedule=2 0 1 1 *'])::text
          FROM pg_catalog.pg_class AS relation
          CROSS JOIN LATERAL pg_catalog.unnest(
            relation.reloptions) AS lineage_option
          WHERE relation.oid = ${child_b_oid}
            AND lineage_option OPERATOR(pg_catalog.~~)
                'compaction_lineage=%';")"
    assert_eq "two-family attachment activates both leaves" "1:1" \
        "$(current_generation_job_count "${child_a_oid}"):$(
          current_generation_job_count "${child_b_oid}")"

    sql_super -c "DROP TABLE public.lifecycle_multi_family_parent;" \
        >/dev/null
}

test_attached_index_rewrite_preserves_parent_options() {
    local child_oid parent_lineage tablespace_dir

    tablespace_dir="$(
        mktemp -d "${TMPDIR:-/tmp}/pg_textsearch_attach_ts.XXXXXX"
    )"
    sql_super -c "CREATE TABLESPACE lifecycle_attach_rewrite_ts
      LOCATION '${tablespace_dir}';" >/dev/null
    sql_super -c "GRANT CREATE ON TABLESPACE lifecycle_attach_rewrite_ts
      TO durable_owner;" >/dev/null

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_attach_reindex_parent (
    id integer,
    body text
) PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_attach_reindex_child
    PARTITION OF public.lifecycle_attach_reindex_parent
    FOR VALUES FROM (0) TO (100);
CREATE INDEX lifecycle_attach_reindex_parent_idx
    ON ONLY public.lifecycle_attach_reindex_parent USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '1 0 1 1 *');
CREATE INDEX lifecycle_attach_reindex_child_idx
    ON public.lifecycle_attach_reindex_child USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '2 0 1 1 *');
BEGIN;
ALTER INDEX public.lifecycle_attach_reindex_parent_idx
    ATTACH PARTITION public.lifecycle_attach_reindex_child_idx;
ALTER TABLE public.lifecycle_attach_reindex_child
    SET TABLESPACE lifecycle_attach_rewrite_ts;
COMMIT;
SQL
    child_oid="$(sql_super -c "SELECT
        'public.lifecycle_attach_reindex_child_idx'::regclass::oid;")"
    parent_lineage="$(
        index_lineage public.lifecycle_attach_reindex_parent_idx
    )"

    assert_eq "attached then rewritten child inherits parent options" \
        "${parent_lineage}:true" \
        "$(sql_super -c "SELECT
          pg_catalog.substr(
            lineage_option,
            pg_catalog.length('compaction_lineage=') + 1)
          || ':' ||
          (relation.reloptions @>
             ARRAY['compaction_schedule=1 0 1 1 *'])::text
          FROM pg_catalog.pg_class AS relation
          CROSS JOIN LATERAL pg_catalog.unnest(
            relation.reloptions) AS lineage_option
          WHERE relation.oid = ${child_oid}
            AND lineage_option OPERATOR(pg_catalog.~~)
                'compaction_lineage=%';")"
    assert_eq "attached then rewritten child activates parent schedule" \
        "true" \
        "$(sql_super -c "SELECT
            (label OPERATOR(pg_catalog.~~)
            ('%:' || pg_catalog.encode(
              pg_catalog.convert_to('1 0 1 1 *', 'UTF8'), 'hex')))::text
          FROM df.instances
          WHERE id = '$(current_generation_job_id "${child_oid}")';")"

    sql_super -c \
        "DROP TABLE public.lifecycle_attach_reindex_parent;" >/dev/null
    sql_super -c "DROP TABLESPACE lifecycle_attach_rewrite_ts;" >/dev/null
    rmdir "${tablespace_dir}"
}

test_lineage_lookup_drop_durable_order() {
    local create_output create_pid create_status=0 durable_oid
    local drop_output drop_pid drop_status=0 gate_create_pid gate_drop_pid
    local instances_oid member_before

    create_output="${DATA_DIR}/lineage-drop-create.out"
    drop_output="${DATA_DIR}/lineage-drop-extension.out"
    durable_oid="$(sql_super -c "SELECT oid FROM pg_catalog.pg_extension
      WHERE extname = 'pg_durable';")"
    instances_oid="$(sql_super -c "SELECT 'df.instances'::regclass::oid;")"

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_lineage_drop_docs (body text);
CREATE FUNCTION public.lifecycle_lineage_drop_pause()
RETURNS event_trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-lineage-drop-create' THEN
        PERFORM pg_catalog.pg_advisory_xact_lock_shared(478, 17);
    END IF;
END
$body$;
SQL
    sql_super -c "
        CREATE EVENT TRIGGER lifecycle_lineage_drop_pause
          ON ddl_command_start
          WHEN TAG IN ('CREATE INDEX')
          EXECUTE FUNCTION public.lifecycle_lineage_drop_pause();" \
        >/dev/null

    PGAPPNAME=lifecycle-lineage-drop-create-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 17);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/lineage-drop-create-gate.out" 2>&1 &
    gate_create_pid=$!
    PGAPPNAME=lifecycle-lineage-drop-extension-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 18);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/lineage-drop-extension-gate.out" 2>&1 &
    gate_drop_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name IN (
                'lifecycle-lineage-drop-create-gate',
                'lifecycle-lineage-drop-extension-gate')
                AND wait_event = 'PgSleep';")" = "2" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-lineage-drop-create \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "CREATE INDEX lifecycle_lineage_drop_idx
              ON public.lifecycle_lineage_drop_docs USING bm25(body)
              WITH (text_config = 'english',
                    compaction = 'background',
                    compaction_schedule = '0 0 1 1 *');" \
        >"${create_output}" 2>&1 &
    create_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-lineage-drop-create'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "lineage CREATE reaches its pre-core barrier" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-lineage-drop-create'
            AND wait_event = 'advisory';")"
    member_before="$(sql_super -c "SELECT count(*)
      FROM pg_catalog.pg_stat_activity AS activity
      JOIN pg_catalog.pg_locks AS member_lock
        ON member_lock.pid = activity.pid
      WHERE activity.application_name = 'lifecycle-lineage-drop-create'
        AND member_lock.locktype = 'relation'
        AND member_lock.relation = ${instances_oid}
        AND member_lock.mode = 'RowExclusiveLock'
        AND member_lock.granted;")"

    PGAPPNAME=lifecycle-lineage-drop-extension \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            ALTER EXTENSION pg_durable DROP TABLE df.instances;
            LOCK TABLE df.instances IN ACCESS EXCLUSIVE MODE;
            SELECT pg_catalog.pg_advisory_xact_lock(478, 18);
            ROLLBACK;" >"${drop_output}" 2>&1 &
    drop_pid=$!

    if [ "${member_before}" = "1" ]; then
        for _ in $(seq 1 100); do
            if [ "$(sql_super -c "SELECT count(*)
                  FROM pg_catalog.pg_stat_activity AS activity
                  JOIN pg_catalog.pg_locks AS member_lock
                    ON member_lock.pid = activity.pid
                  WHERE activity.application_name =
                        'lifecycle-lineage-drop-extension'
                    AND member_lock.locktype = 'relation'
                    AND member_lock.relation = ${instances_oid}
                    AND member_lock.mode = 'AccessExclusiveLock'
                    AND NOT member_lock.granted;")" = "1" ]; then
                break
            fi
            sleep 0.1
        done
        assert_eq "pg_durable member change waits behind lineage SPI" "1:1" \
            "$(sql_super -c "SELECT
              count(*) FILTER (
                WHERE extension_lock.locktype = 'object'
                  AND extension_lock.classid =
                      'pg_catalog.pg_extension'::pg_catalog.regclass
                  AND extension_lock.objid = ${durable_oid}
                  AND extension_lock.mode = 'AccessExclusiveLock'
                  AND extension_lock.granted)
              || ':' ||
              count(*) FILTER (
                WHERE extension_lock.locktype = 'relation'
                  AND extension_lock.relation = ${instances_oid}
                  AND extension_lock.mode = 'AccessExclusiveLock'
                  AND NOT extension_lock.granted)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS extension_lock
                ON extension_lock.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-lineage-drop-extension';")"
    else
        for _ in $(seq 1 100); do
            if [ "$(sql_super -c "SELECT count(*)
                  FROM pg_catalog.pg_stat_activity
                  WHERE application_name =
                        'lifecycle-lineage-drop-extension'
                    AND wait_event = 'advisory';")" = "1" ]; then
                break
            fi
            sleep 0.1
        done
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-lineage-drop-extension'
                AND wait_event = 'advisory';")" != "1" ]; then
            error "pg_durable member change did not reach its rollback barrier:
activity: $(sql_super -c "SELECT
  pg_catalog.concat_ws(':', state, wait_event_type, wait_event, query)
  FROM pg_catalog.pg_stat_activity
  WHERE application_name = 'lifecycle-lineage-drop-extension';")
output: $(cat "${drop_output}")"
        fi
        log "PASS: pg_durable member change reaches its rollback barrier"
    fi

    if [ "${member_before}" = "0" ]; then
        sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name =
                'lifecycle-lineage-drop-extension-gate';" >/dev/null
        wait "${gate_drop_pid}" || true
        wait "${drop_pid}" || drop_status=$?
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-lineage-drop-create-gate';" >/dev/null
    wait "${gate_create_pid}" || true
    wait "${create_pid}" || create_status=$?
    if [ "${member_before}" = "1" ]; then
        sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name =
                'lifecycle-lineage-drop-extension-gate';" >/dev/null
        wait "${gate_drop_pid}" || true
        wait "${drop_pid}" || drop_status=$?
    fi

    if [ "${create_status}" -ne 0 ] || [ "${drop_status}" -ne 0 ] ||
        grep -Fq "deadlock detected" "${create_output}" ||
        grep -Fq "deadlock detected" "${drop_output}"; then
        error "lineage lookup and DROP pg_durable lock order failed:
create: $(cat "${create_output}")
drop: $(cat "${drop_output}")"
    fi
    assert_eq "lineage collection takes no pg_durable member lock" \
        "0" "${member_before}"
    assert_eq "rolled-back pg_durable drop preserves both extensions" \
        "1:1" \
        "$(sql_super -c "SELECT
          count(*) FILTER (WHERE extname = 'pg_durable')
          || ':' ||
          count(*) FILTER (WHERE extname = 'pg_textsearch')
          FROM pg_catalog.pg_extension;")"
    assert_eq "lineage CREATE activates after the drop rollback" "1" \
        "$(current_generation_job_count "$(sql_super -c "SELECT
          'public.lifecycle_lineage_drop_idx'::regclass::oid;")")"

    sql_super -c "
        DROP EVENT TRIGGER lifecycle_lineage_drop_pause;
        DROP FUNCTION public.lifecycle_lineage_drop_pause();
        DROP TABLE public.lifecycle_lineage_drop_docs;" >/dev/null
}

test_textsearch_extension_dependency_order() {
    local alter_completed=false alter_output alter_pid alter_status=0
    local create_output
    local extension_oid extension_output extension_pid extension_status=0
    local gate_pid index_oid step_function_oid

    alter_output="${DATA_DIR}/textsearch-extension-alter.out"
    extension_output="${DATA_DIR}/textsearch-extension-holder.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_textsearch_extension_docs (body text);
CREATE INDEX lifecycle_textsearch_extension_idx
    ON public.lifecycle_textsearch_extension_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_textsearch_lineage_docs (body text);
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_textsearch_extension_idx'::regclass::oid;")"
    extension_oid="$(sql_super -c "SELECT oid
      FROM pg_catalog.pg_extension WHERE extname = 'pg_textsearch';")"
    step_function_oid="$(sql_super -c "SELECT
      'bm25_compact_step_if_current(oid,oid,oid,oid,oid)'::regprocedure::oid;")"

    PGAPPNAME=lifecycle-textsearch-extension-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 19);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/textsearch-extension-gate.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-textsearch-extension-gate'
                AND wait_event = 'PgSleep';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-textsearch-extension-holder \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=30s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            ALTER EXTENSION pg_textsearch DROP FUNCTION
              bm25_compact_step_if_current(oid, oid, oid, oid, oid);
            DROP FUNCTION
              bm25_compact_step_if_current(oid, oid, oid, oid, oid);
            SELECT pg_catalog.pg_advisory_xact_lock_shared(478, 19);
            DROP EXTENSION pg_textsearch CASCADE;
            ROLLBACK;" >"${extension_output}" 2>&1 &
    extension_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS member_lock
                ON member_lock.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-textsearch-extension-holder'
                AND activity.wait_event = 'advisory'
                AND member_lock.locktype = 'object'
                AND member_lock.classid =
                    'pg_catalog.pg_proc'::pg_catalog.regclass
                AND member_lock.objid = ${step_function_oid}
                AND member_lock.mode = 'AccessExclusiveLock'
                AND member_lock.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    if [ "$(sql_super -c "SELECT
          count(*) FILTER (
            WHERE object_lock.classid =
                    'pg_catalog.pg_extension'::pg_catalog.regclass
              AND object_lock.objid = ${extension_oid}
              AND object_lock.mode = 'AccessShareLock')
          || ':' ||
          count(*) FILTER (
            WHERE object_lock.classid =
                    'pg_catalog.pg_proc'::pg_catalog.regclass
              AND object_lock.objid = ${step_function_oid}
              AND object_lock.mode = 'AccessExclusiveLock')
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS object_lock
            ON object_lock.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-textsearch-extension-holder'
            AND activity.wait_event = 'advisory'
            AND object_lock.locktype = 'object'
            AND object_lock.granted;")" != "1:1" ]; then
        error "pg_textsearch drop did not hold its extension and member:
activity: $(sql_super -c "SELECT
  pg_catalog.concat_ws(':', state, wait_event_type, wait_event, query)
  FROM pg_catalog.pg_stat_activity
  WHERE application_name = 'lifecycle-textsearch-extension-holder';")
locks: $(sql_super -c "SELECT pg_catalog.string_agg(
  lock.locktype || ':' || coalesce(lock.classid::text, '') || ':' ||
  coalesce(lock.objid::text, '') || ':' || lock.mode || ':' ||
  lock.granted::text, ',')
  FROM pg_catalog.pg_stat_activity AS activity
  JOIN pg_catalog.pg_locks AS lock ON lock.pid = activity.pid
  WHERE activity.application_name =
        'lifecycle-textsearch-extension-holder';")
output: $(cat "${extension_output}")"
    fi
    log "PASS: pg_textsearch drop holds its extension and member"

    if create_output="$(PGOPTIONS="-c statement_timeout=2s" \
        sql_as durable_owner -c "
        CREATE INDEX lifecycle_textsearch_lineage_idx
          ON public.lifecycle_textsearch_lineage_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_lineage =
                  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb');" 2>&1)"; then
        error "supplied lineage bypassed extension lifecycle serialization:
${create_output}"
    fi
    if ! grep -Fq "canceling statement due to statement timeout" \
        <<<"${create_output}"; then
        error "supplied lineage lifecycle wait failed unexpectedly:
${create_output}"
    fi
    assert_eq "serialized supplied lineage leaves no index" "" \
        "$(sql_super -c "SELECT pg_catalog.to_regclass(
          'public.lifecycle_textsearch_lineage_idx');")"
    log "PASS: extension lifecycle serializes supplied lineage CREATE"

    PGAPPNAME=lifecycle-textsearch-extension-alter \
        PGOPTIONS="-c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "ALTER INDEX public.lifecycle_textsearch_extension_idx
              SET (compaction_schedule = '7 0 1 1 *');" \
        >"${alter_output}" 2>&1 &
    alter_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-textsearch-extension-alter';")" = "0" ]; then
            alter_completed=true
            break
        fi
        sleep 0.1
    done
    if [ "${alter_completed}" = "true" ]; then
        wait "${alter_pid}" || alter_status=$?
    fi
    assert_eq "contended extension root defers terminal activation" "f" \
        "$(sql_super -c "SELECT EXISTS (
          SELECT 1 FROM df.instances
          WHERE label OPERATOR(pg_catalog.~~)
                ('pg_textsearch:bg:v1:%:${index_oid}:%:' ||
                 pg_catalog.encode(
                   pg_catalog.convert_to('7 0 1 1 *', 'UTF8'), 'hex'))
            AND status OPERATOR(pg_catalog.=)
                ANY (ARRAY['pending', 'running']::pg_catalog.text[]));")"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-textsearch-extension-gate';" >/dev/null
    wait "${gate_pid}" || true
    wait "${extension_pid}" || extension_status=$?
    if [ "${alter_completed}" != "true" ]; then
        wait "${alter_pid}" || alter_status=$?
    fi
    if [ "${alter_completed}" != "true" ] ||
        [ "${alter_status}" -ne 0 ] ||
        [ "${extension_status}" -ne 0 ] ||
        ! grep -Fq \
            "background compaction lifecycle reconciliation was deferred" \
            "${alter_output}" ||
        grep -Fq "deadlock detected" "${alter_output}" ||
        grep -Fq "deadlock detected" "${extension_output}"; then
        error "pg_textsearch extension/dependency ordering failed:
alter: $(cat "${alter_output}")
extension: $(cat "${extension_output}")"
    fi
    log "PASS: contended extension root is checked before dependency work"

    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_textsearch_extension_idx
          SET (compaction_schedule = '7 0 1 1 *');" >/dev/null
    assert_eq "explicit retry publishes the deferred schedule" "t" \
        "$(sql_super -c "SELECT EXISTS (
          SELECT 1 FROM df.instances
          WHERE label OPERATOR(pg_catalog.~~)
                ('pg_textsearch:bg:v1:%:${index_oid}:%:' ||
                 pg_catalog.encode(
                   pg_catalog.convert_to('7 0 1 1 *', 'UTF8'), 'hex'))
            AND status OPERATOR(pg_catalog.=)
                ANY (ARRAY['pending', 'running']::pg_catalog.text[]));")"

    sql_super -c "
        DROP TABLE public.lifecycle_textsearch_extension_docs,
                   public.lifecycle_textsearch_lineage_docs;" >/dev/null
}

test_precommit_request_admission_nowait() {
    local blocker_output blocker_pid blocker_status=0 gate_fifo gate_pid
    local index_oid
    local writer_output writer_pid writer_status=0 writer_waiting

    blocker_output="${DATA_DIR}/precommit-admission-blocker.out"
    writer_output="${DATA_DIR}/precommit-admission-writer.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_precommit_admission_docs (
    id integer,
    body text
);
CREATE INDEX lifecycle_precommit_admission_idx
    ON public.lifecycle_precommit_admission_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_precommit_admission_idx'::regclass::oid;")"
    install_signal_probe
    reset_signal_probe
    sql_super -c "INSERT INTO public.compaction_signal_fault
      VALUES ('*', 'gate');"

    gate_fifo="${DATA_DIR}/precommit-admission-gate.fifo"
    mkfifo "${gate_fifo}"
    exec 7<>"${gate_fifo}"
    PGAPPNAME=lifecycle-precommit-admission-gate \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        <"${gate_fifo}" >"${DATA_DIR}/precommit-admission-gate.out" 2>&1 &
    gate_pid=$!
    printf '%s\n' "SELECT pg_catalog.pg_advisory_lock(478, 11);" >&7
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS gate
                ON gate.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-precommit-admission-gate'
                AND activity.state = 'idle'
                AND gate.locktype = 'advisory'
                AND gate.classid = 478
                AND gate.objid = 11
                AND gate.mode = 'ExclusiveLock'
                AND gate.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-precommit-admission-blocker \
        PGOPTIONS="-c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            INSERT INTO public.lifecycle_precommit_admission_docs
            SELECT value,
                   pg_catalog.format('blocker %s filler', value)
            FROM pg_catalog.generate_series(1, 20) AS value;
            SELECT bm25_spill_index(
              'public.lifecycle_precommit_admission_idx');
            INSERT INTO public.lifecycle_precommit_admission_docs
            SELECT 50 + value,
                   pg_catalog.format('blocker second %s filler', value)
            FROM pg_catalog.generate_series(1, 20) AS value;
            SELECT bm25_spill_index(
              'public.lifecycle_precommit_admission_idx');
            COMMIT;" \
        >"${blocker_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS admission
                ON admission.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-precommit-admission-blocker'
                AND activity.wait_event = 'advisory'
                AND admission.locktype = 'object'
                AND admission.classid =
                    'pg_catalog.pg_am'::pg_catalog.regclass
                AND admission.objid = ${index_oid}
                AND admission.objsubid = 1
                AND admission.mode = 'ExclusiveLock'
                AND admission.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "request blocker holds the target admission" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS admission
            ON admission.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-precommit-admission-blocker'
            AND activity.wait_event = 'advisory'
            AND admission.locktype = 'object'
            AND admission.classid =
                'pg_catalog.pg_am'::pg_catalog.regclass
            AND admission.objid = ${index_oid}
            AND admission.objsubid = 1
            AND admission.mode = 'ExclusiveLock'
            AND admission.granted;")"

    PGAPPNAME=lifecycle-precommit-admission-writer \
        PGOPTIONS="-c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            INSERT INTO public.lifecycle_precommit_admission_docs
            SELECT 100 + value,
                   pg_catalog.format('writer %s filler', value)
            FROM pg_catalog.generate_series(1, 20) AS value;
            SELECT bm25_spill_index(
              'public.lifecycle_precommit_admission_idx');
            COMMIT;" >"${writer_output}" 2>&1 &
    writer_pid=$!
    writer_waiting=0
    for _ in $(seq 1 100); do
        writer_waiting="$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS admission
            ON admission.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-precommit-admission-writer'
            AND admission.locktype = 'object'
            AND admission.classid =
                'pg_catalog.pg_am'::pg_catalog.regclass
            AND admission.objid = ${index_oid}
            AND admission.objsubid = 1
            AND admission.mode = 'ExclusiveLock'
            AND NOT admission.granted;")"
        if [ "${writer_waiting}" = "1" ] ||
            [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-precommit-admission-writer';")" = "0" ]; then
            break
        fi
        sleep 0.1
    done

    if [ "${writer_waiting}" = "0" ]; then
        wait "${writer_pid}" || writer_status=$?
    fi
    printf '%s\n' \
        "SELECT pg_catalog.pg_advisory_unlock(478, 11);" \
        '\q' >&7
    exec 7>&-
    wait "${gate_pid}" || true
    if [ "${writer_waiting}" = "1" ]; then
        wait "${writer_pid}" || writer_status=$?
    fi
    wait "${blocker_pid}" || blocker_status=$?

    if [ "${writer_status}" -ne 0 ] || [ "${blocker_status}" -ne 0 ] ||
        grep -Fq "deadlock detected" "${writer_output}" ||
        grep -Fq "deadlock detected" "${blocker_output}"; then
        error "PRE_COMMIT admission deferral failed:
writer: $(cat "${writer_output}")
blocker: $(cat "${blocker_output}")"
    fi
    assert_eq "PRE_COMMIT request never waits on admission" \
        "0" "${writer_waiting}"
    assert_eq "deferred request preserves writer rows" "60" \
        "$(sql_super -c "SELECT count(*)
          FROM public.lifecycle_precommit_admission_docs;")"

    restore_signal_probe
    sql_super -c "DROP TABLE
      public.lifecycle_precommit_admission_docs;" >/dev/null
}

test_terminal_grant_reentry_is_rejected() {
    local alter_output

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_grant_reentry_docs (
    body_a text,
    body_b text
);
CREATE INDEX lifecycle_grant_reentry_outer_idx
    ON public.lifecycle_grant_reentry_docs USING bm25(body_a)
    WITH (text_config = 'english', compaction = 'manual');
CREATE FUNCTION public.lifecycle_grant_reenter()
RETURNS event_trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-grant-reentry'
       AND pg_catalog.to_regclass(
             'public.lifecycle_grant_reentry_nested_idx') IS NULL THEN
        EXECUTE
            'CREATE INDEX lifecycle_grant_reentry_nested_idx '
            'ON public.lifecycle_grant_reentry_docs USING bm25(body_b) '
            'WITH (text_config = ''english'', '
            'compaction = ''background'')';
    END IF;
END
$body$;
SQL
    sql_super -c "
        CREATE EVENT TRIGGER lifecycle_grant_reenter
          ON ddl_command_end
          WHEN TAG IN ('GRANT')
          EXECUTE FUNCTION public.lifecycle_grant_reenter();
        REVOKE EXECUTE ON FUNCTION
          bm25_compact_step_if_current(oid, oid, oid, oid, oid),
          bm25_background_target_is_current(oid, oid, oid, oid, oid)
          FROM durable_owner;" >/dev/null

    if alter_output="$(PGAPPNAME=lifecycle-grant-reentry \
        sql_as durable_owner -c "
          ALTER INDEX public.lifecycle_grant_reentry_outer_idx
            SET (compaction = 'background');" 2>&1)"; then
        error "managed DDL reentered terminal reconciliation:
${alter_output}"
    fi
    if ! grep -Fq \
        "cannot execute DDL during background compaction reconciliation" \
        <<<"${alter_output}"; then
        error "terminal reentry failed for the wrong reason:
${alter_output}"
    fi
    assert_eq "terminal reentry rolls back nested managed DDL" "" \
        "$(sql_super -c "SELECT pg_catalog.to_regclass(
          'public.lifecycle_grant_reentry_nested_idx');")"

    sql_super -c "
        DROP EVENT TRIGGER lifecycle_grant_reenter;
        DROP FUNCTION public.lifecycle_grant_reenter();
        DROP TABLE public.lifecycle_grant_reentry_docs;
        SELECT df.grant_usage('durable_owner');" >/dev/null
}

test_post_publication_reindex_defers() {
    local blocker_output blocker_pid blocker_status=0 gate_fifo gate_pid
    local index_oid_after index_oid_before outer_output outer_pid
    local outer_status=0 outer_waiting relfilenumber_before signal_gate_fifo
    local renamed_output signal_gate_pid

    blocker_output="${DATA_DIR}/post-publication-blocker.out"
    outer_output="${DATA_DIR}/post-publication-reindex.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_post_publication_docs (
    id integer,
    body text
);
INSERT INTO public.lifecycle_post_publication_docs
VALUES (1, 'before');
CREATE INDEX lifecycle_post_publication_idx
    ON public.lifecycle_post_publication_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_post_publication_nested_docs (body text);
CREATE INDEX lifecycle_post_publication_nested_idx
    ON public.lifecycle_post_publication_nested_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE FUNCTION public.lifecycle_post_publication_pause()
RETURNS event_trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-post-publication-reindex'
       AND COALESCE(
             pg_catalog.current_setting(
               'lifecycle.post_publication_reentry', true),
             '') OPERATOR(pg_catalog.<>) 'done' THEN
        PERFORM pg_catalog.set_config(
            'lifecycle.post_publication_reentry', 'done', false);
        EXECUTE
            'ALTER INDEX public.lifecycle_post_publication_nested_idx '
            'SET (compaction_schedule = ''1 0 1 1 *'')';
        PERFORM pg_catalog.pg_advisory_xact_lock_shared(478, 20);
    END IF;
END
$body$;
SQL
    sql_super -c "
        CREATE EVENT TRIGGER lifecycle_post_publication_pause
          ON ddl_command_end
          WHEN TAG IN ('REINDEX')
          EXECUTE FUNCTION public.lifecycle_post_publication_pause();" \
        >/dev/null
    index_oid_before="$(sql_super -c "SELECT
        'public.lifecycle_post_publication_idx'::regclass::oid;")"
    relfilenumber_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(
          'public.lifecycle_post_publication_idx'::regclass);")"
    install_signal_probe
    reset_signal_probe
    sql_super -c "INSERT INTO public.compaction_signal_fault
      VALUES ('*', 'gate');"

    gate_fifo="${DATA_DIR}/post-publication-gate.fifo"
    mkfifo "${gate_fifo}"
    exec 7<>"${gate_fifo}"
    PGAPPNAME=lifecycle-post-publication-gate \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        <"${gate_fifo}" >"${DATA_DIR}/post-publication-gate.out" 2>&1 &
    gate_pid=$!
    printf '%s\n' \
        "SELECT pg_catalog.pg_advisory_lock(478, 20);" >&7
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS gate
                ON gate.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-post-publication-gate'
                AND activity.state = 'idle'
                AND gate.locktype = 'advisory'
                AND gate.classid = 478
                AND gate.objid = 20
                AND gate.mode = 'ExclusiveLock'
                AND gate.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-post-publication-reindex \
        PGOPTIONS="-c deadlock_timeout=100ms -c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "REINDEX INDEX CONCURRENTLY
              public.lifecycle_post_publication_idx;" \
        >"${outer_output}" 2>&1 &
    outer_pid=$!
    for _ in $(seq 1 200); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-post-publication-reindex'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "concurrent REINDEX reaches post-publication reentry" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name =
                'lifecycle-post-publication-reindex'
            AND wait_event = 'advisory';")"
    index_oid_after="$(sql_super -c "SELECT
        'public.lifecycle_post_publication_idx'::regclass::oid;")"
    if [ "$(sql_super -c "SELECT
          pg_catalog.pg_relation_filenode(
            'public.lifecycle_post_publication_idx'::regclass);")" = \
         "${relfilenumber_before}" ]; then
        error "concurrent REINDEX paused before publishing replacement storage"
    fi

    signal_gate_fifo="${DATA_DIR}/post-publication-signal-gate.fifo"
    mkfifo "${signal_gate_fifo}"
    exec 8<>"${signal_gate_fifo}"
    PGAPPNAME=lifecycle-post-publication-signal-gate \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        <"${signal_gate_fifo}" \
        >"${DATA_DIR}/post-publication-signal-gate.out" 2>&1 &
    signal_gate_pid=$!
    printf '%s\n' \
        "SELECT pg_catalog.pg_advisory_lock(478, 11);" >&8
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS gate
                ON gate.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-post-publication-signal-gate'
                AND activity.state = 'idle'
                AND gate.locktype = 'advisory'
                AND gate.classid = 478
                AND gate.objid = 11
                AND gate.mode = 'ExclusiveLock'
                AND gate.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-post-publication-blocker \
        PGOPTIONS="-c statement_timeout=15s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            INSERT INTO public.lifecycle_post_publication_docs
            SELECT 100 + value,
                   pg_catalog.format(
                     'post publication first %s filler', value)
            FROM pg_catalog.generate_series(1, 20) AS value;
            SELECT bm25_spill_index(
              'public.lifecycle_post_publication_idx');
            INSERT INTO public.lifecycle_post_publication_docs
            SELECT 200 + value,
                   pg_catalog.format(
                     'post publication second %s filler', value)
            FROM pg_catalog.generate_series(1, 20) AS value;
            SELECT bm25_spill_index(
              'public.lifecycle_post_publication_idx');
            COMMIT;" >"${blocker_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 200); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS admission
                ON admission.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-post-publication-blocker'
                AND activity.wait_event = 'advisory'
                AND admission.locktype = 'object'
                AND admission.classid =
                    'pg_catalog.pg_am'::pg_catalog.regclass
                AND admission.objid = ${index_oid_after}
                AND admission.objsubid = 1
                AND admission.mode = 'ExclusiveLock'
                AND admission.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "signal blocker holds the published target admission" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS admission
            ON admission.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-post-publication-blocker'
            AND activity.wait_event = 'advisory'
            AND admission.locktype = 'object'
            AND admission.classid =
                'pg_catalog.pg_am'::pg_catalog.regclass
            AND admission.objid = ${index_oid_after}
            AND admission.objsubid = 1
            AND admission.mode = 'ExclusiveLock'
            AND admission.granted;")"

    printf '%s\n' \
        "SELECT pg_catalog.pg_advisory_unlock(478, 20);" \
        '\q' >&7
    exec 7>&-
    wait "${gate_pid}" || true
    outer_waiting=0
    for _ in $(seq 1 50); do
        outer_waiting="$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS admission
            ON admission.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-post-publication-reindex'
            AND admission.locktype = 'object'
            AND admission.classid =
                'pg_catalog.pg_am'::pg_catalog.regclass
            AND admission.objid = ${index_oid_after}
            AND admission.objsubid = 1
            AND admission.mode = 'ExclusiveLock'
            AND NOT admission.granted;")"
        if [ "${outer_waiting}" = "1" ] ||
            [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-post-publication-reindex';")" = "0" ]; then
            break
        fi
        sleep 0.1
    done
    if [ "${outer_waiting}" = "0" ]; then
        wait "${outer_pid}" || outer_status=$?
    fi

    printf '%s\n' \
        "SELECT pg_catalog.pg_advisory_unlock(478, 11);" \
        '\q' >&8
    exec 8>&-
    wait "${signal_gate_pid}" || true
    wait "${blocker_pid}" || blocker_status=$?
    if [ "${outer_waiting}" = "1" ]; then
        wait "${outer_pid}" || outer_status=$?
    fi

    if [ "${outer_status}" -ne 0 ] || [ "${blocker_status}" -ne 0 ] ||
        grep -Fq "deadlock detected" "${outer_output}" ||
        grep -Fq "deadlock detected" "${blocker_output}"; then
        error "post-publication reconciliation failed:
reindex: $(cat "${outer_output}")
blocker: $(cat "${blocker_output}")"
    fi
    assert_eq "post-publication reconciliation never waits on admission" \
        "0" "${outer_waiting}"
    assert_eq "published concurrent REINDEX remains visible" \
        "${index_oid_after}" \
        "$(sql_super -c "SELECT
          'public.lifecycle_post_publication_idx'::regclass::oid;")"
    assert_eq "nested reentry option change commits" "t" \
        "$(sql_super -c "SELECT reloptions @>
          ARRAY['compaction_schedule=1 0 1 1 *']
          FROM pg_catalog.pg_class WHERE oid =
            'public.lifecycle_post_publication_nested_idx'::regclass;")"
    restore_signal_probe
    renamed_output="${DATA_DIR}/post-publication-renamed-object.out"
    sql_super -c "ALTER FUNCTION df.explain(text)
      RENAME TO explain_temporarily_unavailable;"
    if ! sql_as durable_owner -c "
        REINDEX INDEX CONCURRENTLY
          public.lifecycle_post_publication_idx;" \
        >"${renamed_output}" 2>&1; then
        error "published REINDEX reported an object-discovery failure:
$(cat "${renamed_output}")"
    fi
    if ! grep -Fq \
        "background compaction lifecycle reconciliation was deferred" \
        "${renamed_output}"; then
        error "published REINDEX did not report deferred object discovery:
$(cat "${renamed_output}")"
    fi
    sql_super -c "ALTER FUNCTION df.explain_temporarily_unavailable(text)
      RENAME TO explain;"
    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_post_publication_idx
          SET (compaction_schedule = '0 0 1 1 *');" >/dev/null
    assert_eq "object-discovery deferral remains explicitly retryable" "1" \
        "$(current_generation_job_count "$(sql_super -c "SELECT
          'public.lifecycle_post_publication_idx'::regclass::oid;")")"
    sql_super -c "
        DROP EVENT TRIGGER lifecycle_post_publication_pause;
        DROP FUNCTION public.lifecycle_post_publication_pause();
        DROP TABLE public.lifecycle_post_publication_docs,
                   public.lifecycle_post_publication_nested_docs;" >/dev/null
}

test_post_publication_schedule_override() {
    local index_oid job_after job_label reindex_output schedule_matches

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_post_publication_schedule_docs (body text);
INSERT INTO public.lifecycle_post_publication_schedule_docs
VALUES ('before');
CREATE INDEX lifecycle_post_publication_schedule_idx
    ON public.lifecycle_post_publication_schedule_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE FUNCTION public.lifecycle_post_publication_schedule_change()
RETURNS event_trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-post-publication-schedule' THEN
        ALTER INDEX public.lifecycle_post_publication_schedule_idx
          SET (compaction_schedule = '2 0 1 1 *');
    END IF;
END
$body$;
SQL
    sql_super -c "
        CREATE EVENT TRIGGER lifecycle_post_publication_schedule_change
          ON ddl_command_end
          WHEN TAG IN ('REINDEX')
          EXECUTE FUNCTION
            public.lifecycle_post_publication_schedule_change();" >/dev/null
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_post_publication_schedule_idx'::regclass::oid;")"

    reindex_output="$(PGAPPNAME=lifecycle-post-publication-schedule \
        sql_as durable_owner -c "
          REINDEX INDEX CONCURRENTLY
            public.lifecycle_post_publication_schedule_idx;" 2>&1)"

    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_post_publication_schedule_idx'::regclass::oid;")"
    assert_eq "post-publication schedule option change commits" "t" \
        "$(sql_super -c "SELECT reloptions @>
          ARRAY['compaction_schedule=2 0 1 1 *']
          FROM pg_catalog.pg_class
          WHERE oid = ${index_oid};")"
    schedule_matches=0
    for _ in $(seq 1 100); do
        schedule_matches="$(
            current_generation_schedule_job_count \
                "${index_oid}" "2 0 1 1 *"
        )"
        if [ "${schedule_matches}" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    if [ "${schedule_matches}" != "1" ]; then
        job_after="$(current_generation_job_id "${index_oid}")"
        job_label="$(sql_super -c "SELECT label
          FROM df.instances WHERE id = '${job_after}';")"
        error "post-publication schedule change was not authoritative:
workflow ${job_after} has label ${job_label}
REINDEX output: ${reindex_output}"
    fi
    log "PASS: post-publication schedule change remains authoritative"

    sql_super -c "
        DROP EVENT TRIGGER lifecycle_post_publication_schedule_change;
        DROP FUNCTION public.lifecycle_post_publication_schedule_change();
        DROP TABLE public.lifecycle_post_publication_schedule_docs;" \
        >/dev/null
}

test_post_publication_background_activation() {
    local index_oid schedule_matches

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_post_publication_activation_docs (body text);
INSERT INTO public.lifecycle_post_publication_activation_docs
VALUES ('before');
CREATE INDEX lifecycle_post_publication_activation_idx
    ON public.lifecycle_post_publication_activation_docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'inline');
CREATE FUNCTION public.lifecycle_post_publication_enable_background()
RETURNS event_trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-post-publication-activation' THEN
        ALTER INDEX public.lifecycle_post_publication_activation_idx
          SET (compaction = 'background',
               compaction_schedule = '3 0 1 1 *');
    END IF;
END
$body$;
SQL
    sql_super -c "
        CREATE EVENT TRIGGER lifecycle_post_publication_enable_background
          ON ddl_command_end
          WHEN TAG IN ('REINDEX')
          EXECUTE FUNCTION
            public.lifecycle_post_publication_enable_background();" \
        >/dev/null

    PGAPPNAME=lifecycle-post-publication-activation \
        sql_as durable_owner -c "
          REINDEX INDEX CONCURRENTLY
            public.lifecycle_post_publication_activation_idx;" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_post_publication_activation_idx'::regclass::oid;")"
    assert_eq "post-publication background activation commits" "t" \
        "$(sql_super -c "SELECT reloptions @>
          ARRAY['compaction=background',
                'compaction_schedule=3 0 1 1 *']
          FROM pg_catalog.pg_class
          WHERE oid = ${index_oid};")"

    schedule_matches=0
    for _ in $(seq 1 100); do
        schedule_matches="$(
            current_generation_schedule_job_count \
                "${index_oid}" "3 0 1 1 *"
        )"
        if [ "${schedule_matches}" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "post-publication activation creates the current workflow" \
        "1" "${schedule_matches}"

    sql_super -c "
        DROP EVENT TRIGGER
          lifecycle_post_publication_enable_background;
        DROP FUNCTION
          public.lifecycle_post_publication_enable_background();
        DROP TABLE public.lifecycle_post_publication_activation_docs;" \
        >/dev/null
}

test_post_publication_activation_error_defers() {
    local index_oid output relfilenumber_before

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_post_publication_error_docs (body text);
INSERT INTO public.lifecycle_post_publication_error_docs
VALUES ('before');
CREATE INDEX lifecycle_post_publication_error_idx
    ON public.lifecycle_post_publication_error_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE FUNCTION public.lifecycle_post_publication_revoke()
RETURNS event_trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-post-publication-error' THEN
        ALTER INDEX public.lifecycle_post_publication_error_idx
          RENAME TO lifecycle_post_publication_error_renamed_idx;
        REVOKE USAGE ON SCHEMA df FROM durable_owner;
    END IF;
END
$body$;
SQL
    sql_super -c "
        ALTER FUNCTION public.lifecycle_post_publication_revoke()
          OWNER TO postgres;
        CREATE EVENT TRIGGER lifecycle_post_publication_revoke
          ON ddl_command_end
          WHEN TAG IN ('REINDEX')
          EXECUTE FUNCTION public.lifecycle_post_publication_revoke();" \
        >/dev/null
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_post_publication_error_idx'::regclass::oid;")"
    relfilenumber_before="$(sql_super -c "SELECT
        pg_catalog.pg_relation_filenode(${index_oid});")"

    if ! output="$(PGAPPNAME=lifecycle-post-publication-error \
        sql_as durable_owner -c "
          REINDEX INDEX CONCURRENTLY
            public.lifecycle_post_publication_error_idx;" 2>&1)"; then
        error "published REINDEX reported an activation error:
${output}"
    fi
    if ! grep -Fq \
        "background compaction lifecycle reconciliation was deferred" \
        <<<"${output}"; then
        error "published REINDEX did not report activation deferral:
${output}"
    fi
    if [ "$(sql_super -c "SELECT pg_catalog.pg_relation_filenode(
          'public.lifecycle_post_publication_error_renamed_idx'
            ::regclass);")" = \
         "${relfilenumber_before}" ]; then
        error "activation-error test did not publish replacement storage"
    fi

    sql_super -c "SELECT df.grant_usage('durable_owner');" >/dev/null
    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_post_publication_error_renamed_idx
          SET (compaction_schedule = '0 0 1 1 *');" >/dev/null
    assert_eq "activation-error deferral remains explicitly retryable" "1" \
        "$(current_generation_job_count "$(sql_super -c "SELECT
          'public.lifecycle_post_publication_error_renamed_idx'
            ::regclass::oid;")")"

    sql_super -c "
        DROP EVENT TRIGGER lifecycle_post_publication_revoke;
        DROP FUNCTION public.lifecycle_post_publication_revoke();
        DROP TABLE public.lifecycle_post_publication_error_docs;" >/dev/null
}

test_cross_statement_managed_lock_order() {
    local am_oid first_index_oid first_output first_pid first_status=0
    local gate_pid second_index_oid second_output second_pid second_status=0

    first_output="${DATA_DIR}/cross-statement-lock-first.out"
    second_output="${DATA_DIR}/cross-statement-lock-second.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_cross_statement_first_docs (body text);
CREATE INDEX lifecycle_cross_statement_first_idx
    ON public.lifecycle_cross_statement_first_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_cross_statement_second_docs (body text);
CREATE INDEX lifecycle_cross_statement_second_idx
    ON public.lifecycle_cross_statement_second_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    first_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_cross_statement_first_idx'::regclass::oid;")"
    second_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_cross_statement_second_idx'::regclass::oid;")"
    am_oid="$(sql_super -c \
        "SELECT oid FROM pg_catalog.pg_am WHERE amname = 'bm25';")"

    PGAPPNAME=lifecycle-cross-statement-gate sql_super -c "
        SELECT pg_catalog.pg_advisory_lock(478, 13);
        SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/cross-statement-lock-gate.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity AS activity
              JOIN pg_catalog.pg_locks AS gate
                ON gate.pid = activity.pid
              WHERE activity.application_name =
                    'lifecycle-cross-statement-gate'
                AND activity.wait_event = 'PgSleep'
                AND gate.locktype = 'advisory'
                AND gate.classid = 478
                AND gate.objid = 13
                AND gate.mode = 'ExclusiveLock'
                AND gate.granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "cross-statement gate is held" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS gate ON gate.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-cross-statement-gate'
            AND activity.wait_event = 'PgSleep'
            AND gate.locktype = 'advisory'
            AND gate.classid = 478
            AND gate.objid = 13
            AND gate.mode = 'ExclusiveLock'
            AND gate.granted;")"

    PGAPPNAME=lifecycle-cross-statement-first \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "SET deadlock_timeout = '100ms';
            SET statement_timeout = '15s';
            BEGIN;
            ALTER INDEX public.lifecycle_cross_statement_first_idx
              SET (compaction_schedule = '1 0 1 1 *');
            SELECT pg_catalog.pg_advisory_xact_lock(478, 13);
            ALTER INDEX public.lifecycle_cross_statement_second_idx
              SET (compaction_schedule = '2 0 1 1 *');
            COMMIT;" >"${first_output}" 2>&1 &
    first_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-cross-statement-first'
                AND wait_event_type = 'Lock';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "first transaction reaches the statement barrier" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-cross-statement-first'
            AND wait_event_type = 'Lock';")"
    assert_eq "collecting transaction holds no managed locks" "0" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity AS activity
          JOIN pg_catalog.pg_locks AS managed
            ON managed.pid = activity.pid
          WHERE activity.application_name =
                'lifecycle-cross-statement-first'
            AND managed.locktype = 'object'
            AND managed.classid =
                'pg_catalog.pg_am'::pg_catalog.regclass
            AND managed.granted
            AND ((managed.objid = ${first_index_oid}
                  AND managed.objsubid = 1
                  AND managed.mode = 'ExclusiveLock')
                 OR
                 (managed.objid = ${am_oid}
                  AND managed.objsubid = 0
                  AND managed.mode = 'ShareRowExclusiveLock'));")"

    PGAPPNAME=lifecycle-cross-statement-second \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "SET deadlock_timeout = '10s';
            SET statement_timeout = '15s';
            BEGIN;
            ALTER INDEX public.lifecycle_cross_statement_second_idx
              SET (compaction_schedule = '3 0 1 1 *');
            COMMIT;" >"${second_output}" 2>&1 &
    second_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name =
                    'lifecycle-cross-statement-second';")" = "0" ]; then
            break
        fi
        sleep 0.1
    done
    wait "${second_pid}" || second_status=$?
    if [ "${second_status}" -ne 0 ]; then
        error "independent managed transaction was blocked:
$(cat "${second_output}")"
    fi
    log "PASS: independent managed transaction completes during collection"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-cross-statement-gate';" >/dev/null
    wait "${gate_pid}" || true
    wait "${first_pid}" || first_status=$?
    if [ "${first_status}" -ne 0 ]; then
        error "cross-statement managed lock test failed:
first: $(cat "${first_output}")
second: $(cat "${second_output}")"
    fi
    if grep -Fq "deadlock detected" "${first_output}" ||
        grep -Fq "deadlock detected" "${second_output}"; then
        error "cross-statement managed operations deadlocked:
first: $(cat "${first_output}")
second: $(cat "${second_output}")"
    fi
    assert_eq "later statement wins after collected transaction resumes" "t" \
        "$(sql_super -c "SELECT reloptions @>
          ARRAY['compaction_schedule=2 0 1 1 *']
          FROM pg_catalog.pg_class WHERE oid = ${second_index_oid};")"
    assert_eq "cross-statement batch publishes the first schedule" "t" \
        "$(sql_super -c "SELECT EXISTS (
          SELECT 1 FROM df.instances
          WHERE label OPERATOR(pg_catalog.~~)
                ('pg_textsearch:bg:v1:%:${first_index_oid}:%:' ||
                 pg_catalog.encode(
                   pg_catalog.convert_to('1 0 1 1 *', 'UTF8'), 'hex'))
            AND status OPERATOR(pg_catalog.=)
                ANY (ARRAY['pending', 'running']::pg_catalog.text[]));")"
    assert_eq "cross-statement batch publishes the second schedule" "t" \
        "$(sql_super -c "SELECT EXISTS (
          SELECT 1 FROM df.instances
          WHERE label OPERATOR(pg_catalog.~~)
                ('pg_textsearch:bg:v1:%:${second_index_oid}:%:' ||
                 pg_catalog.encode(
                   pg_catalog.convert_to('2 0 1 1 *', 'UTF8'), 'hex'))
            AND status OPERATOR(pg_catalog.=)
                ANY (ARRAY['pending', 'running']::pg_catalog.text[]));")"

    sql_super -c "
        DROP TABLE public.lifecycle_cross_statement_first_docs;
        DROP TABLE public.lifecycle_cross_statement_second_docs;"
}

test_managed_intent_savepoint_recovery() {
    local fifo output pid status=0

    fifo="${DATA_DIR}/managed-intent-savepoint.fifo"
    output="${DATA_DIR}/managed-intent-savepoint.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_savepoint_first_docs (body text);
CREATE INDEX lifecycle_savepoint_first_idx
    ON public.lifecycle_savepoint_first_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_savepoint_second_docs (body text);
CREATE INDEX lifecycle_savepoint_second_idx
    ON public.lifecycle_savepoint_second_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_savepoint_created_docs (body text);
CREATE TABLE public.lifecycle_intent_disabled_docs (body text);
CREATE TABLE public.lifecycle_intent_rollback_docs (body text);
CREATE TABLE public.lifecycle_intent_timestamp_docs (body text);
CREATE INDEX lifecycle_intent_timestamp_idx
    ON public.lifecycle_intent_timestamp_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_intent_order_a_docs (body text);
CREATE INDEX lifecycle_intent_order_a_idx
    ON public.lifecycle_intent_order_a_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_intent_order_b_docs (body text);
CREATE INDEX lifecycle_intent_order_b_idx
    ON public.lifecycle_intent_order_b_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL

    mkfifo "${fifo}"
    exec 9<>"${fifo}"
    PGAPPNAME=lifecycle-managed-intent-savepoint \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=0 \
        <"${fifo}" >"${output}" 2>&1 &
    pid=$!

    printf '%s\n' 'BEGIN;' '\echo begin_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "begin_done" "${output}" && break
        sleep 0.1
    done
    sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_intent_timestamp_idx
          SET (compaction_schedule = '14 0 1 1 *');" >/dev/null
    printf '%s\n' \
        "ALTER INDEX public.lifecycle_intent_timestamp_idx
           SET (compaction_schedule = '15 0 1 1 *');" \
        '\echo timestamp_intent_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "timestamp_intent_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' \
        "ALTER INDEX public.lifecycle_savepoint_first_idx
           SET (compaction_schedule = '4 0 1 1 *');" \
        '\echo first_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "first_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' \
        "CREATE INDEX lifecycle_savepoint_created_idx
           ON public.lifecycle_savepoint_created_docs USING bm25(body)
           WITH (text_config = 'english',
                 compaction = 'background',
                 compaction_schedule = '3 0 1 1 *');" \
        "ALTER INDEX public.lifecycle_savepoint_created_idx
           SET (compaction_schedule = '9 0 1 1 *');" \
        '\echo create_alter_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "create_alter_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' \
        "CREATE INDEX lifecycle_intent_disabled_idx
           ON public.lifecycle_intent_disabled_docs USING bm25(body)
           WITH (text_config = 'english',
                 compaction = 'background',
                 compaction_schedule = '12 0 1 1 *');" \
        "ALTER INDEX public.lifecycle_intent_disabled_idx
           SET (compaction = 'manual');" \
        "CREATE INDEX lifecycle_intent_rollback_idx
           ON public.lifecycle_intent_rollback_docs USING bm25(body)
           WITH (text_config = 'english',
                 compaction = 'background',
                 compaction_schedule = '13 0 1 1 *');" \
        'SAVEPOINT disable_intent;' \
        "ALTER INDEX public.lifecycle_intent_rollback_idx
           SET (compaction = 'manual');" \
        'ROLLBACK TO SAVEPOINT disable_intent;' \
        '\echo mode_intents_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "mode_intents_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' \
        "ALTER INDEX public.lifecycle_intent_order_a_idx
           SET (compaction_schedule = '10 0 1 1 *');" \
        "ALTER TABLE public.lifecycle_intent_order_a_docs
           OWNER TO durable_owner_two;" \
        "ALTER TABLE public.lifecycle_intent_order_b_docs
           OWNER TO durable_owner_two;" \
        "ALTER INDEX public.lifecycle_intent_order_b_idx
           SET (compaction_schedule = '11 0 1 1 *');" \
        '\echo intent_order_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "intent_order_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' 'SAVEPOINT managed_intent;' '\echo savepoint_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "savepoint_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' \
        "ALTER INDEX public.lifecycle_savepoint_second_idx
           SET (compaction_schedule = '5 0 1 1 *');" \
        '\echo rolled_back_intent_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "rolled_back_intent_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' 'SELECT 1 / 0;' '\echo expected_error_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "expected_error_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' \
        'ROLLBACK TO SAVEPOINT managed_intent;' \
        '\echo rollback_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "rollback_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' \
        'SAVEPOINT merged_intent;' \
        "ALTER INDEX public.lifecycle_savepoint_first_idx
           SET (compaction_schedule = '8 0 1 1 *');" \
        'RELEASE SAVEPOINT merged_intent;' \
        '\echo release_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "release_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' \
        "ALTER INDEX public.lifecycle_savepoint_second_idx
           SET (compaction_schedule = '6 0 1 1 *');" \
        '\echo replacement_intent_done' >&9
    for _ in $(seq 1 100); do
        grep -Fq "replacement_intent_done" "${output}" && break
        sleep 0.1
    done
    printf '%s\n' 'COMMIT;' '\echo commit_done' '\q' >&9
    exec 9>&-
    wait "${pid}" || status=$?

    if [ "${status}" -ne 0 ] ||
        ! grep -Fq "division by zero" "${output}" ||
        ! grep -Fq "rollback_done" "${output}" ||
        ! grep -Fq "commit_done" "${output}" ||
        grep -Fq "current transaction is aborted" "${output}"; then
        error "managed intent savepoint recovery failed:
$(cat "${output}")"
    fi
    assert_eq "savepoint recovery commits the parent intent" "t" \
        "$(sql_super -c "SELECT reloptions @>
          ARRAY['compaction_schedule=8 0 1 1 *']
          FROM pg_catalog.pg_class WHERE oid =
            'public.lifecycle_savepoint_first_idx'::regclass;")"
    assert_eq "savepoint recovery discards and replaces the child intent" \
        "t" \
        "$(sql_super -c "SELECT reloptions @>
          ARRAY['compaction_schedule=6 0 1 1 *']
          FROM pg_catalog.pg_class WHERE oid =
            'public.lifecycle_savepoint_second_idx'::regclass;")"
    assert_eq "later ALTER overrides a CREATE intent schedule" "t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
          ':',
          relation.reloptions @> ARRAY['compaction_schedule=9 0 1 1 *'],
          EXISTS (
            SELECT 1 FROM df.instances
            WHERE label OPERATOR(pg_catalog.~~)
                  ('pg_textsearch:bg:v1:%:' || relation.oid || ':%:' ||
                   pg_catalog.encode(
                     pg_catalog.convert_to('9 0 1 1 *', 'UTF8'), 'hex'))
              AND status OPERATOR(pg_catalog.=)
                  ANY (ARRAY['pending', 'running']::pg_catalog.text[])))
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid =
            'public.lifecycle_savepoint_created_idx'::regclass;")"
    assert_eq "later manual mode cancels a CREATE activation" "t:0" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
          ':',
          relation.reloptions @> ARRAY['compaction=manual'],
          (SELECT count(*) FROM df.instances
           WHERE label OPERATOR(pg_catalog.~~)
                 ('pg_textsearch:bg:v1:%:' || relation.oid || ':%')
             AND status OPERATOR(pg_catalog.=)
                 ANY (ARRAY['pending', 'running']::pg_catalog.text[])))
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid =
            'public.lifecycle_intent_disabled_idx'::regclass;")"
    assert_eq "rolled-back manual mode preserves CREATE activation" "t:1" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
          ':',
          relation.reloptions @> ARRAY['compaction=background'],
          (SELECT count(*) FROM df.instances
           WHERE label OPERATOR(pg_catalog.~~)
                 ('pg_textsearch:bg:v1:%:' || relation.oid || ':%')
             AND status OPERATOR(pg_catalog.=)
                 ANY (ARRAY['pending', 'running']::pg_catalog.text[])))
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid =
            'public.lifecycle_intent_rollback_idx'::regclass;")"
    assert_eq "workflow ordering follows activation rather than xact start" \
        "t" \
        "$(sql_super -c "SELECT (
          SELECT created_at FROM df.instances
          WHERE label OPERATOR(pg_catalog.~~)
                ('pg_textsearch:bg:v1:%:' || relation.oid || ':%:' ||
                 pg_catalog.encode(
                   pg_catalog.convert_to('15 0 1 1 *', 'UTF8'), 'hex'))
          ORDER BY created_at DESC, id DESC LIMIT 1) >
        (SELECT created_at FROM df.instances
         WHERE label OPERATOR(pg_catalog.~~)
               ('pg_textsearch:bg:v1:%:' || relation.oid || ':%:' ||
                pg_catalog.encode(
                  pg_catalog.convert_to('14 0 1 1 *', 'UTF8'), 'hex'))
         ORDER BY created_at DESC, id DESC LIMIT 1)
        FROM pg_catalog.pg_class AS relation
        WHERE relation.oid =
          'public.lifecycle_intent_timestamp_idx'::regclass;")"
    assert_eq "ALTER then owner change keeps the altered schedule" "t:t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
          ':',
          relation.relowner = 'durable_owner_two'::regrole,
          relation.reloptions @> ARRAY['compaction_schedule=10 0 1 1 *'],
          EXISTS (
            SELECT 1 FROM df.instances
            WHERE label OPERATOR(pg_catalog.~~)
                  ('pg_textsearch:bg:v1:%:' || relation.oid || ':%:' ||
                   relation.relowner || ':%:' ||
                   pg_catalog.encode(
                     pg_catalog.convert_to('10 0 1 1 *', 'UTF8'), 'hex'))
              AND submitted_by::pg_catalog.oid = relation.relowner
              AND status OPERATOR(pg_catalog.=)
                  ANY (ARRAY['pending', 'running']::pg_catalog.text[])))
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid =
            'public.lifecycle_intent_order_a_idx'::regclass;")"
    assert_eq "owner change then ALTER uses the final schedule" "t:t:t" \
        "$(sql_super -c "SELECT pg_catalog.concat_ws(
          ':',
          relation.relowner = 'durable_owner_two'::regrole,
          relation.reloptions @> ARRAY['compaction_schedule=11 0 1 1 *'],
          EXISTS (
            SELECT 1 FROM df.instances
            WHERE label OPERATOR(pg_catalog.~~)
                  ('pg_textsearch:bg:v1:%:' || relation.oid || ':%:' ||
                   relation.relowner || ':%:' ||
                   pg_catalog.encode(
                     pg_catalog.convert_to('11 0 1 1 *', 'UTF8'), 'hex'))
              AND submitted_by::pg_catalog.oid = relation.relowner
              AND status OPERATOR(pg_catalog.=)
                  ANY (ARRAY['pending', 'running']::pg_catalog.text[])))
          FROM pg_catalog.pg_class AS relation
          WHERE relation.oid =
            'public.lifecycle_intent_order_b_idx'::regclass;")"

    sql_super -c "
        DROP TABLE public.lifecycle_savepoint_first_docs;
        DROP TABLE public.lifecycle_savepoint_second_docs;
        DROP TABLE public.lifecycle_savepoint_created_docs;
        DROP TABLE public.lifecycle_intent_disabled_docs;
        DROP TABLE public.lifecycle_intent_rollback_docs;
        DROP TABLE public.lifecycle_intent_timestamp_docs;
        DROP TABLE public.lifecycle_intent_order_a_docs;
        DROP TABLE public.lifecycle_intent_order_b_docs;"
}

test_internal_lock_namespace() {
    local admission_error database_oid gate_pid index_oid lock_key owner_error

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_private_lock_docs (body text);
CREATE INDEX lifecycle_private_lock_idx
    ON public.lifecycle_private_lock_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_private_lock_idx'::regclass::oid;")"
    remove_index_lineage public.lifecycle_private_lock_idx
    lock_key="$((1885828211 * 4294967296 + index_oid))"

    PGAPPNAME=lifecycle-public-advisory-lock sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(${lock_key});
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/public-advisory-lock.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 1885828211
                AND objid = ${index_oid}
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "public advisory collision gate is held" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_locks
          WHERE locktype = 'advisory'
            AND classid = 1885828211
            AND objid = ${index_oid}
            AND granted;")"

    if ! owner_error="$(sql_super -c "
        SET statement_timeout = '2s';
        ALTER TABLE public.lifecycle_private_lock_docs
          OWNER TO durable_owner_two;" 2>&1)"; then
        error "public advisory lock blocked internal lineage serialization: \
${owner_error}"
    fi
    assert_eq "public advisory lock does not block internal lineage lock" \
        "durable_owner_two:32" \
        "$(sql_super -c "SELECT
            pg_catalog.pg_get_userbyid(relation.relowner) || ':' ||
            pg_catalog.length(pg_catalog.substr(
              option, pg_catalog.length('compaction_lineage=') + 1))
          FROM pg_catalog.pg_class AS relation,
               LATERAL pg_catalog.unnest(relation.reloptions) AS option
          WHERE relation.oid = ${index_oid}
            AND option OPERATOR(pg_catalog.~~)
                'compaction_lineage=%';")"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-public-advisory-lock';" >/dev/null
    wait "${gate_pid}" || true

    database_oid="$(sql_super -c \
        "SELECT oid FROM pg_catalog.pg_database
          WHERE datname = pg_catalog.current_database();")"
    PGAPPNAME=lifecycle-admission-advisory-lock \
        sql_as durable_writer -c \
        "SELECT pg_catalog.pg_advisory_lock(
             ${database_oid}::integer, ${index_oid}::integer);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/admission-advisory-lock.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = ${database_oid}
                AND objid = ${index_oid}
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "public admission collision gate is held" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_locks
          WHERE locktype = 'advisory'
            AND classid = ${database_oid}
            AND objid = ${index_oid}
            AND granted;")"

    if ! admission_error="$(sql_as durable_owner_two -c "
        SET statement_timeout = '2s';
        ALTER INDEX public.lifecycle_private_lock_idx SET (
          compaction_schedule = '1 0 1 1 *');" 2>&1)"; then
        error "public advisory lock blocked workflow admission: \
${admission_error}"
    fi

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-admission-advisory-lock';" >/dev/null
    wait "${gate_pid}" || true
    sql_super -c "DROP TABLE public.lifecycle_private_lock_docs;"
}

test_lineage_ddl_guards() {
    local cic_error duplicate_error lineage replay_error reset_error set_error

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_lineage_source_docs (body text);
CREATE INDEX lifecycle_lineage_source_idx
    ON public.lifecycle_lineage_source_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_lineage_duplicate_docs (body text);
SQL
    lineage="$(index_lineage public.lifecycle_lineage_source_idx)"

    if set_error="$(sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_lineage_source_idx SET (
          compaction_lineage =
            '11111111111111111111111111111111');" 2>&1)"; then
        error "ALTER INDEX SET changed managed lineage"
    fi
    if ! grep -Fq "cannot alter internal background compaction lineage" \
        <<<"${set_error}"; then
        error "lineage SET failed for the wrong reason: ${set_error}"
    fi
    assert_eq "rejected lineage SET preserves identity" "${lineage}" \
        "$(index_lineage public.lifecycle_lineage_source_idx)"

    if reset_error="$(sql_as durable_owner -c "
        ALTER INDEX public.lifecycle_lineage_source_idx
          RESET (compaction_lineage);" 2>&1)"; then
        error "ALTER INDEX RESET removed managed lineage"
    fi
    if ! grep -Fq "cannot alter internal background compaction lineage" \
        <<<"${reset_error}"; then
        error "lineage RESET failed for the wrong reason: ${reset_error}"
    fi
    assert_eq "rejected lineage RESET preserves identity" "${lineage}" \
        "$(index_lineage public.lifecycle_lineage_source_idx)"

    if duplicate_error="$(sql_as durable_owner -c "
        CREATE INDEX lifecycle_lineage_duplicate_options_idx
          ON public.lifecycle_lineage_duplicate_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_lineage = '${lineage}',
                compaction_lineage = '${lineage}');" 2>&1)"; then
        error "CREATE accepted duplicate lineage options"
    fi
    if ! grep -Fq "compaction_lineage" <<<"${duplicate_error}"; then
        error "duplicate lineage options failed unexpectedly: \
${duplicate_error}"
    fi

    if duplicate_error="$(sql_as durable_owner -c "
        CREATE INDEX lifecycle_lineage_duplicate_idx
          ON public.lifecycle_lineage_duplicate_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_lineage = '${lineage}');" 2>&1)"; then
        error "CREATE reused a live background compaction lineage"
    fi
    if ! grep -Fq "background compaction lineage is already in use" \
        <<<"${duplicate_error}"; then
        error "live lineage reuse failed unexpectedly: ${duplicate_error}"
    fi
    assert_eq "rejected live lineage reuse creates no index" "" \
        "$(sql_super -c "SELECT pg_catalog.to_regclass(
          'public.lifecycle_lineage_duplicate_idx');")"

    if cic_error="$(sql_as durable_owner -c "
        CREATE INDEX CONCURRENTLY lifecycle_lineage_cic_idx
          ON public.lifecycle_lineage_duplicate_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_lineage =
                  'cccccccccccccccccccccccccccccccc');" 2>&1)"; then
        error "concurrent CREATE accepted an explicit lineage"
    fi
    if ! grep -Fq \
        "explicit background compaction lineage is not supported" \
        <<<"${cic_error}"; then
        error "concurrent explicit lineage failed unexpectedly:
${cic_error}"
    fi
    assert_eq "rejected concurrent explicit lineage leaves no index" "" \
        "$(sql_super -c "SELECT pg_catalog.to_regclass(
          'public.lifecycle_lineage_cic_idx');")"

    sql_as durable_owner -c \
        "DROP INDEX public.lifecycle_lineage_source_idx;" >/dev/null
    if replay_error="$(sql_as durable_owner -c "
        CREATE INDEX lifecycle_lineage_source_idx
          ON public.lifecycle_lineage_source_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_lineage = '${lineage}');" 2>&1)"; then
        error "drop/recreate replay reused retained lineage history"
    fi
    if ! grep -Fq "background compaction lineage is already in use" \
        <<<"${replay_error}"; then
        error "retained lineage replay failed unexpectedly: ${replay_error}"
    fi

    sql_super -c "DROP TABLE public.lifecycle_lineage_source_docs,
                             public.lifecycle_lineage_duplicate_docs;"
}

test_partitioned_lineage_history() {
    local lineage replay_error

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_partition_history_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_partition_history_low
    PARTITION OF public.lifecycle_partition_history_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_partition_history_high
    PARTITION OF public.lifecycle_partition_history_docs
    FOR VALUES FROM (100) TO (200);
SQL
    sql_super -c "ALTER TABLE public.lifecycle_partition_history_high
                   OWNER TO durable_owner_two;"
    sql_as durable_owner -c "
CREATE INDEX lifecycle_partition_history_idx
    ON public.lifecycle_partition_history_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
"
    lineage="$(index_lineage public.lifecycle_partition_history_idx)"
    assert_eq "partition history has mixed physical leaf owners" "2" \
        "$(sql_super -c "SELECT count(DISTINCT heap.relowner)
          FROM pg_catalog.pg_partition_tree(
                 'public.lifecycle_partition_history_idx'::regclass) AS tree
          JOIN pg_catalog.pg_index AS index_catalog
            ON index_catalog.indexrelid = tree.relid
          JOIN pg_catalog.pg_class AS heap
            ON heap.oid = index_catalog.indrelid
          WHERE tree.isleaf;")"
    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'partition lineage history test')
      FROM df.instances AS instance
      JOIN pg_catalog.pg_inherits AS inheritance
        ON instance.label OPERATOR(pg_catalog.~~)
           ('pg_textsearch:bg:v1:%:' ||
            inheritance.inhrelid::pg_catalog.text || ':%')
      WHERE inheritance.inhparent =
            'public.lifecycle_partition_history_idx'::regclass
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    sql_super -c "UPDATE df.instances
      SET label = 'retired-partition-owner-' || id
      WHERE submitted_by = 'durable_owner'::regrole
        AND label OPERATOR(pg_catalog.~~)
            'pg_textsearch:bg:v1:%:%:%:%:%:%:${lineage}:%';"
    assert_eq "mixed-owner retained leaf history remains" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM df.instances
          WHERE submitted_by = 'durable_owner_two'::regrole
            AND label OPERATOR(pg_catalog.~~)
                'pg_textsearch:bg:v1:%:%:%:%:%:%:${lineage}:%';")"
    sql_as durable_owner -c \
        "DROP INDEX public.lifecycle_partition_history_idx;" >/dev/null

    if replay_error="$(sql_as durable_owner -c "
        CREATE INDEX lifecycle_partition_history_idx
          ON public.lifecycle_partition_history_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *',
                compaction_lineage = '${lineage}');" 2>&1)"; then
        error "partitioned drop/recreate reused retained leaf lineage"
    fi
    if ! grep -Fq "background compaction lineage is already in use" \
        <<<"${replay_error}"; then
        error "partitioned retained history failed unexpectedly: \
${replay_error}"
    fi
    log "PASS: partition root rejects retained physical-leaf lineage"

    sql_super -c \
        "DROP TABLE public.lifecycle_partition_history_docs;"
}

test_forged_lineage_submitter() {
    local create_error foreign_instance forged_label lineage new_index_oid
    local new_instance original_instance

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_forged_history_docs (body text);
CREATE INDEX lifecycle_forged_history_idx
    ON public.lifecycle_forged_history_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    new_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_forged_history_idx'::regclass::oid;")"
    lineage="$(index_lineage public.lifecycle_forged_history_idx)"
    original_instance="$(current_generation_job_id "${new_index_oid}")"
    wait_for_signal_node "${original_instance}" 30
    forged_label="$(sql_super -c "SELECT label
      FROM df.instances WHERE id = '${original_instance}';")"
    sql_as durable_owner -c "SELECT df.cancel(
        '${original_instance}', 'forge submitter test');" >/dev/null
    wait_for_terminal "${original_instance}" 30
    sql_super -c "UPDATE df.instances
      SET label = 'retired-forged-source-' || id
      WHERE id = '${original_instance}';"
    sql_as durable_owner -c \
        "DROP INDEX public.lifecycle_forged_history_idx;" >/dev/null

    foreign_instance="$(sql_as durable_owner_two -c "
      SELECT df.start(
        df.wait_for_signal('hold', 300),
        '${forged_label}',
        pg_catalog.current_database(),
        'caller');")"
    wait_for_signal_node "${foreign_instance}" 30

    if ! create_error="$(sql_as durable_owner -c "
        CREATE INDEX lifecycle_forged_history_idx
          ON public.lifecycle_forged_history_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *',
                compaction_lineage = '${lineage}');" 2>&1)"; then
        error "foreign-submitter forged label blocked legitimate lineage \
reuse: ${create_error}"
    fi
    new_index_oid="$(sql_super -c "SELECT
        'public.lifecycle_forged_history_idx'::regclass::oid;")"
    new_instance="$(current_generation_job_id "${new_index_oid}")"
    if [ -z "${new_instance}" ]; then
        error "foreign-submitter forged label prevented owner activation"
    fi
    assert_eq "forged label remains owned by its foreign submitter" \
        "durable_owner_two" \
        "$(sql_super -c "SELECT submitted_by::pg_catalog.text
          FROM df.instances WHERE id = '${foreign_instance}';")"
    assert_eq "owner may reuse lineage present only in forged history" \
        "${lineage}" \
        "$(index_lineage public.lifecycle_forged_history_idx)"

    sql_as durable_owner_two -c "SELECT df.cancel(
        '${foreign_instance}', 'forge submitter test complete');" >/dev/null
    sql_as durable_owner -c "SELECT df.cancel(
        '${new_instance}', 'forge submitter test complete');" >/dev/null
    sql_super -c "DROP TABLE public.lifecycle_forged_history_docs;"
}

test_concurrent_supplied_lineage_create() {
    local first_output first_pid first_status=0 gate_pid
    local second_output second_pid second_status=0 successes
    local supplied_lineage=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

    first_output="${DATA_DIR}/lineage-create-first.out"
    second_output="${DATA_DIR}/lineage-create-second.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE FUNCTION public.lifecycle_lineage_create_pause(value text)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    PERFORM pg_catalog.pg_advisory_xact_lock_shared(478, 7);
    RETURN value;
END
$body$;

CREATE TABLE public.lifecycle_lineage_create_docs (body text);
INSERT INTO public.lifecycle_lineage_create_docs VALUES ('one'), ('two');
SQL

    PGAPPNAME=lifecycle-lineage-create-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 7);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/lineage-create-gate.out" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 7
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-lineage-create-first \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "CREATE INDEX lifecycle_lineage_create_first_idx
            ON public.lifecycle_lineage_create_docs
            USING bm25 (
              public.lifecycle_lineage_create_pause(body))
            WITH (text_config = 'english',
                  compaction = 'background',
                  compaction_lineage = '${supplied_lineage}');" \
        >"${first_output}" 2>&1 &
    first_pid=$!
    PGAPPNAME=lifecycle-lineage-create-second \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "CREATE INDEX lifecycle_lineage_create_second_idx
            ON public.lifecycle_lineage_create_docs
            USING bm25 (
              public.lifecycle_lineage_create_pause(body))
            WITH (text_config = 'english',
                  compaction = 'background',
                  compaction_lineage = '${supplied_lineage}');" \
        >"${second_output}" 2>&1 &
    second_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name IN (
                'lifecycle-lineage-create-first',
                'lifecycle-lineage-create-second')
                    AND wait_event_type = 'Lock';")" = "2" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "concurrent lineage CREATE reaches serialization gates" "2" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name IN (
            'lifecycle-lineage-create-first',
            'lifecycle-lineage-create-second')
            AND wait_event_type = 'Lock';")"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-lineage-create-gate';" >/dev/null
    wait "${gate_pid}" || true
    wait "${first_pid}" || first_status=$?
    wait "${second_pid}" || second_status=$?
    successes=0
    if [ "${first_status}" -eq 0 ]; then
        successes=$((successes + 1))
    elif ! grep -Eq \
        "background compaction lineage is already in use|could not validate background compaction lineage" \
        "${first_output}"; then
        error "first concurrent CREATE failed unexpectedly: \
$(cat "${first_output}")"
    fi
    if [ "${second_status}" -eq 0 ]; then
        successes=$((successes + 1))
    elif ! grep -Eq \
        "background compaction lineage is already in use|could not validate background compaction lineage" \
        "${second_output}"; then
        error "second concurrent CREATE failed unexpectedly: \
$(cat "${second_output}")"
    fi
    assert_eq "one concurrent same-heap lineage CREATE succeeds" "1" \
        "${successes}"
    assert_eq "same-heap lineage remains unique after concurrent CREATE" \
        "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_class AS relation
          JOIN pg_catalog.pg_index AS index_catalog
            ON index_catalog.indexrelid = relation.oid
          CROSS JOIN LATERAL
            pg_catalog.unnest(relation.reloptions) AS option
          WHERE index_catalog.indrelid =
                'public.lifecycle_lineage_create_docs'::regclass
            AND option OPERATOR(pg_catalog.=)
                'compaction_lineage=${supplied_lineage}';")"
    assert_eq "concurrent same-heap CREATE leaves one workflow" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM df.instances AS instance
          JOIN pg_catalog.pg_class AS relation
            ON instance.label OPERATOR(pg_catalog.~~)
               ('pg_textsearch:bg:v1:%:' ||
                relation.oid::pg_catalog.text || ':%')
          JOIN pg_catalog.pg_index AS index_catalog
            ON index_catalog.indexrelid = relation.oid
          WHERE index_catalog.indrelid =
                'public.lifecycle_lineage_create_docs'::regclass
            AND instance.status OPERATOR(pg_catalog.=)
                ANY (ARRAY['pending', 'running']::pg_catalog.text[]);")"

    sql_super -c "DROP TABLE public.lifecycle_lineage_create_docs;
                   DROP FUNCTION
                     public.lifecycle_lineage_create_pause(text);"
}

test_reindex_lineage_replacement_isolation() {
    local blocker_pid old_index_name old_index_oid old_lineage
    local reindex_pid reindex_output replacement_instance replacement_oid
    local replacement_lineage
    local lock_output="${DATA_DIR}/reindex-lineage-lock.out"

    reindex_output="${DATA_DIR}/reindex-lineage-replacement.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE FUNCTION public.lifecycle_lineage_pause(value text)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF value OPERATOR(pg_catalog.=) 'pause' THEN
        PERFORM pg_catalog.pg_advisory_xact_lock(478, 4);
    END IF;
    RETURN value;
END
$body$;

CREATE TABLE public.lifecycle_lineage_reindex_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_lineage_reindex_low
    PARTITION OF public.lifecycle_lineage_reindex_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_lineage_reindex_high
    PARTITION OF public.lifecycle_lineage_reindex_docs
    FOR VALUES FROM (100) TO (200);
INSERT INTO public.lifecycle_lineage_reindex_docs
VALUES (1, 'pause'), (101, 'continue');
CREATE INDEX lifecycle_lineage_reindex_idx
    ON public.lifecycle_lineage_reindex_docs
    USING bm25 (public.lifecycle_lineage_pause(body))
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    old_index_oid="$(sql_super -c "SELECT index_class.oid
      FROM pg_catalog.pg_inherits AS inheritance
      JOIN pg_catalog.pg_class AS index_class
        ON index_class.oid = inheritance.inhrelid
      JOIN pg_catalog.pg_index AS index_catalog
        ON index_catalog.indexrelid = index_class.oid
      WHERE inheritance.inhparent =
            'public.lifecycle_lineage_reindex_idx'::regclass
        AND index_catalog.indrelid =
            'public.lifecycle_lineage_reindex_high'::regclass;")"
    old_index_name="$(sql_super -c "SELECT relname
      FROM pg_catalog.pg_class WHERE oid = ${old_index_oid};")"
    old_lineage="$(index_lineage "public.${old_index_name}")"

    sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 4);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${lock_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 4
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-lineage-replacement \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "REINDEX INDEX public.lifecycle_lineage_reindex_idx;" \
        >"${reindex_output}" 2>&1 &
    reindex_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-lineage-replacement'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "lineage isolation pauses before the pending leaf" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-lineage-replacement'
            AND wait_event = 'advisory';")"

    sql_as durable_owner -c "
        ALTER TABLE public.lifecycle_lineage_reindex_docs
          DETACH PARTITION public.lifecycle_lineage_reindex_high;
        DROP INDEX public.${old_index_name};
        CREATE INDEX ${old_index_name}
          ON public.lifecycle_lineage_reindex_high
          USING bm25 (public.lifecycle_lineage_pause(body))
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '5 4 3 2 *');" >/dev/null 2>&1
    replacement_oid="$(sql_super -c \
        "SELECT 'public.${old_index_name}'::regclass::oid;")"
    replacement_lineage="$(index_lineage "public.${old_index_name}")"
    if [ "${replacement_oid}" = "${old_index_oid}" ] ||
        [ "${replacement_lineage}" = "${old_lineage}" ]; then
        error "lineage replacement setup reused the original identity"
    fi
    replacement_instance="$(current_generation_job_id "${replacement_oid}")"
    sql_as durable_owner -c "SELECT df.cancel(
        '${replacement_instance}', 'hide unrelated replacement');" >/dev/null
    wait_for_terminal "${replacement_instance}" 30
    sql_super -c "UPDATE df.instances
      SET label = 'retired-lineage-replacement-' || id
      WHERE id = '${replacement_instance}';"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE pid <> pg_catalog.pg_backend_pid()
        AND query OPERATOR(pg_catalog.~~)
            'SELECT pg_catalog.pg_advisory_lock(478, 4)%';" >/dev/null
    wait "${blocker_pid}" || true
    if ! wait "${reindex_pid}"; then
        error "partitioned REINDEX replacement test failed: \
$(cat "${reindex_output}")"
    fi
    assert_eq "outer REINDEX ignores same-name different-lineage index" "" \
        "$(current_generation_job_id "${replacement_oid}")"

    sql_as durable_owner -c "
        DROP TABLE public.lifecycle_lineage_reindex_docs,
                   public.lifecycle_lineage_reindex_high;
        DROP FUNCTION public.lifecycle_lineage_pause(text);"
}

test_reindex_authorization_ordering() {
    local blocker_pid index_error index_oid jobs_before lineage_before
    local lock_output="${DATA_DIR}/reindex-auth-lock.out"
    local partition_child_index partition_error partition_jobs_before
    local partition_parent_oid
    local tablespace_dir tablespace_error transaction_error

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_auth_docs (body text);
CREATE INDEX lifecycle_auth_idx
    ON public.lifecycle_auth_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');

CREATE TABLE public.lifecycle_auth_partitioned_docs
    (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE public.lifecycle_auth_partitioned_low
    PARTITION OF public.lifecycle_auth_partitioned_docs
    FOR VALUES FROM (0) TO (100);
CREATE TABLE public.lifecycle_auth_partitioned_high
    PARTITION OF public.lifecycle_auth_partitioned_docs
    FOR VALUES FROM (100) TO (200);
CREATE INDEX lifecycle_auth_partitioned_idx
    ON public.lifecycle_auth_partitioned_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
SQL
    tablespace_dir="${DATA_DIR}-lifecycle-auth-tablespace"
    mkdir -p "${tablespace_dir}"
    sql_super -c "
        CREATE TABLESPACE lifecycle_auth_tablespace
          OWNER durable_writer LOCATION '${tablespace_dir}';" >/dev/null
    index_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_auth_idx'::regclass::oid;")"
    lineage_before="$(index_lineage public.lifecycle_auth_idx)"
    jobs_before="$(managed_job_count)"

    PGAPPNAME=lifecycle-auth-index-lock sql_as durable_owner -c \
        "BEGIN;
         LOCK TABLE public.lifecycle_auth_docs IN SHARE MODE;
         SELECT pg_catalog.pg_sleep(120);" >"${lock_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_auth_docs'::regclass
                AND mode = 'ShareLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    if transaction_error="$(sql_as durable_owner -c "
        BEGIN;
        SET LOCAL statement_timeout = '1s';
        REINDEX INDEX CONCURRENTLY
          public.lifecycle_auth_idx;" 2>&1)"; then
        error "concurrent REINDEX unexpectedly ran inside a transaction"
    fi
    if ! grep -Fq "cannot run inside a transaction block" \
        <<<"${transaction_error}"; then
        error "concurrent REINDEX waited before its transaction check: \
${transaction_error}"
    fi
    if tablespace_error="$(sql_as durable_owner -c "
        SET statement_timeout = '1s';
        REINDEX (TABLESPACE lifecycle_auth_missing)
          INDEX public.lifecycle_auth_idx;" 2>&1)"; then
        error "REINDEX unexpectedly accepted a missing tablespace"
    fi
    if ! grep -Fq \
        'tablespace "lifecycle_auth_missing" does not exist' \
        <<<"${tablespace_error}"; then
        error "REINDEX waited before destination tablespace lookup: \
${tablespace_error}"
    fi
    if tablespace_error="$(sql_as durable_owner -c "
        SET statement_timeout = '1s';
        REINDEX (TABLESPACE lifecycle_auth_tablespace)
          INDEX public.lifecycle_auth_idx;" 2>&1)"; then
        error "REINDEX unexpectedly bypassed tablespace CREATE privilege"
    fi
    if ! grep -Fq \
        "permission denied for tablespace lifecycle_auth_tablespace" \
        <<<"${tablespace_error}"; then
        error "REINDEX waited before destination tablespace ACL checks: \
${tablespace_error}"
    fi
    if index_error="$(PGOPTIONS='-c statement_timeout=1s' \
        sql_as durable_writer -c "
        REINDEX INDEX CONCURRENTLY public.lifecycle_auth_idx;" 2>&1)"; then
        error "unauthorized concurrent REINDEX unexpectedly succeeded"
    fi
    if ! grep -Fq "permission denied for index lifecycle_auth_idx" \
        <<<"${index_error}"; then
        error "unauthorized concurrent REINDEX did not fail before locking: \
${index_error}"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE pid <> pg_catalog.pg_backend_pid()
        AND application_name =
            'lifecycle-auth-index-lock';" >/dev/null
    wait "${blocker_pid}" || true
    assert_eq "unauthorized concurrent REINDEX preserves lineage" \
        "${lineage_before}" "$(index_lineage public.lifecycle_auth_idx)"
    assert_eq "unauthorized concurrent REINDEX preserves index OID" \
        "${index_oid}" \
        "$(sql_super -c "SELECT
          'public.lifecycle_auth_idx'::regclass::oid;")"
    assert_eq "unauthorized concurrent REINDEX creates no workflow" \
        "${jobs_before}" "$(managed_job_count)"

    partition_parent_oid="$(sql_super -c "SELECT
      'public.lifecycle_auth_partitioned_idx'::regclass::oid;")"
    partition_jobs_before="$(managed_job_count)"
    partition_child_index="$(sql_super -c "
        SELECT child_index.oid::regclass::text
        FROM pg_catalog.pg_inherits AS inheritance
        JOIN pg_catalog.pg_class AS child_index
          ON child_index.oid = inheritance.inhrelid
        WHERE inheritance.inhparent = ${partition_parent_oid}
        ORDER BY child_index.oid
        LIMIT 1;")"
    PGAPPNAME=lifecycle-auth-partition-child-lock \
        sql_as durable_owner -c "
        BEGIN;
        ALTER INDEX ${partition_child_index}
          SET (compaction_schedule = '1 2 3 4 *');
        SELECT pg_catalog.pg_sleep(120);" >"${lock_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE relation = '${partition_child_index}'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    if transaction_error="$(sql_as durable_owner -c "
        BEGIN;
        SET LOCAL statement_timeout = '1s';
        REINDEX TABLE
          public.lifecycle_auth_partitioned_docs;" 2>&1)"; then
        error "partitioned REINDEX unexpectedly ran inside a transaction"
    fi
    if ! grep -Fq "cannot run inside a transaction block" \
        <<<"${transaction_error}"; then
        error "partitioned REINDEX prelocked descendants before preflight: \
${transaction_error}"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-auth-partition-child-lock';" >/dev/null
    wait "${blocker_pid}" || true

    PGAPPNAME=lifecycle-auth-partition-lock sql_as durable_owner -c \
        "BEGIN;
         LOCK TABLE public.lifecycle_auth_partitioned_docs
           IN ACCESS EXCLUSIVE MODE;
         SELECT pg_catalog.pg_sleep(120);" >"${lock_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_auth_partitioned_docs'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    if transaction_error="$(sql_as durable_owner -c "
        BEGIN;
        SET LOCAL statement_timeout = '1s';
        REINDEX TABLE CONCURRENTLY
          public.lifecycle_auth_partitioned_docs;" 2>&1)"; then
        error "partitioned concurrent REINDEX ran inside a transaction"
    fi
    if ! grep -Fq "cannot run inside a transaction block" \
        <<<"${transaction_error}"; then
        error "partitioned REINDEX enumerated descendants before preflight: \
${transaction_error}"
    fi
    if partition_error="$(sql_as durable_writer -c "
        SET statement_timeout = '1s';
        REINDEX TABLE
          public.lifecycle_auth_partitioned_docs;" 2>&1)"; then
        error "unauthorized partitioned REINDEX unexpectedly succeeded"
    fi
    if ! grep -Fq \
        "permission denied for table lifecycle_auth_partitioned_docs" \
        <<<"${partition_error}"; then
        error "unauthorized partitioned REINDEX did not fail before locking: \
${partition_error}"
    fi
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE pid <> pg_catalog.pg_backend_pid()
        AND application_name =
            'lifecycle-auth-partition-lock';" >/dev/null
    wait "${blocker_pid}" || true
    assert_eq "unauthorized partitioned REINDEX preserves parent OID" \
        "${partition_parent_oid}" \
        "$(sql_super -c "SELECT
          'public.lifecycle_auth_partitioned_idx'::regclass::oid;")"
    assert_eq "unauthorized partitioned REINDEX creates no workflow" \
        "${partition_jobs_before}" "$(managed_job_count)"

    sql_super -c "GRANT MAINTAIN ON public.lifecycle_auth_docs
                   TO durable_writer;"
    sql_as durable_writer -c "REINDEX INDEX CONCURRENTLY
        public.lifecycle_auth_idx;" >/dev/null 2>&1
    log "PASS: MAINTAIN permits tracked concurrent REINDEX"

    sql_super -c "DROP TABLE public.lifecycle_auth_docs,
                             public.lifecycle_auth_partitioned_docs;" \
        >/dev/null
    sql_super -c "DROP TABLESPACE lifecycle_auth_tablespace;" >/dev/null
}

test_create_authorization_ordering() {
    local blocker_pid create_error jobs_before malformed_error
    local lock_output="${DATA_DIR}/create-auth-lock.out"

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_create_auth_docs (body text);" \
        >/dev/null
    jobs_before="$(managed_job_count)"
    PGAPPNAME=lifecycle-create-auth-lock sql_as durable_owner -c \
        "BEGIN;
         LOCK TABLE public.lifecycle_create_auth_docs
           IN ACCESS EXCLUSIVE MODE;
         SELECT pg_catalog.pg_sleep(120);" >"${lock_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_create_auth_docs'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "CREATE authorization blocker holds the table lock" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_locks
          WHERE relation =
                'public.lifecycle_create_auth_docs'::regclass
            AND mode = 'AccessExclusiveLock'
            AND granted;")"

    if create_error="$(sql_as durable_writer -c "
        SET statement_timeout = '1s';
        CREATE INDEX lifecycle_create_auth_idx
          ON public.lifecycle_create_auth_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background');" 2>&1)"; then
        error "unauthorized CREATE INDEX unexpectedly succeeded"
    fi
    if ! grep -Fq "must be owner of table lifecycle_create_auth_docs" \
        <<<"${create_error}"; then
        error "unauthorized CREATE waited on the custom strong lock: \
${create_error}"
    fi
    log "PASS: unauthorized CREATE fails before custom strong locking"

    if malformed_error="$(sql_as durable_writer -c "
        SET statement_timeout = '1s';
        CREATE INDEX lifecycle_create_auth_malformed_idx
          ON public.lifecycle_create_auth_docs USING bm25(body)
          WITH (text_config = 'english', compaction);" 2>&1)"; then
        error "unauthorized malformed CREATE INDEX unexpectedly succeeded"
    fi
    if ! grep -Fq "must be owner of table lifecycle_create_auth_docs" \
        <<<"${malformed_error}"; then
        error "malformed compaction option preceded core permission error: \
${malformed_error}"
    fi
    log "PASS: CREATE ownership check precedes malformed option extraction"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-create-auth-lock';" >/dev/null
    wait "${blocker_pid}" || true
    assert_eq "unauthorized CREATE leaves no index" "" \
        "$(sql_super -c "SELECT pg_catalog.to_regclass(
          'public.lifecycle_create_auth_idx');")"
    assert_eq "unauthorized CREATE creates no workflow" \
        "${jobs_before}" "$(managed_job_count)"
    sql_super -c "DROP TABLE public.lifecycle_create_auth_docs;"
}

test_lineage_guard_name_race() {
    local alter_output alter_pid alter_status=0 blocker_pid
    local lineage rename_output rename_pid rename_status=0 replacement_oid

    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_lineage_guard_old_docs (body text);
CREATE INDEX lifecycle_lineage_guard_idx
    ON public.lifecycle_lineage_guard_old_docs USING btree(body);
CREATE TABLE public.lifecycle_lineage_guard_new_docs (body text);
CREATE INDEX lifecycle_lineage_guard_replacement_idx
    ON public.lifecycle_lineage_guard_new_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE FUNCTION public.lifecycle_lineage_guard_pause()
RETURNS event_trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    IF pg_catalog.current_setting('application_name')
           OPERATOR(pg_catalog.=) 'lifecycle-lineage-guard-alter' THEN
        PERFORM pg_catalog.pg_advisory_xact_lock_shared(478, 10);
    END IF;
END
$body$;
SQL
    sql_super -c "
        CREATE EVENT TRIGGER lifecycle_lineage_guard_pause
          ON ddl_command_start
          WHEN TAG IN ('ALTER INDEX')
          EXECUTE FUNCTION public.lifecycle_lineage_guard_pause();" \
        >/dev/null
    lineage="$(
        index_lineage public.lifecycle_lineage_guard_replacement_idx
    )"
    replacement_oid="$(sql_super -c "SELECT
        'public.lifecycle_lineage_guard_replacement_idx'::regclass::oid;")"

    PGAPPNAME=lifecycle-lineage-guard-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 10);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/lineage-guard-gate.out" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 10
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    alter_output="${DATA_DIR}/lineage-guard-alter.out"
    PGAPPNAME=lifecycle-lineage-guard-alter \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "ALTER INDEX public.lifecycle_lineage_guard_idx
              RESET (compaction_lineage);" >"${alter_output}" 2>&1 &
    alter_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-lineage-guard-alter'
                AND wait_event = 'advisory';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "lineage guard pauses after inspecting its target" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-lineage-guard-alter'
            AND wait_event = 'advisory';")"

    rename_output="${DATA_DIR}/lineage-guard-rename.out"
    PGAPPNAME=lifecycle-lineage-guard-rename \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            ALTER INDEX public.lifecycle_lineage_guard_idx
              RENAME TO lifecycle_lineage_guard_retired_idx;
            ALTER INDEX public.lifecycle_lineage_guard_replacement_idx
              RENAME TO lifecycle_lineage_guard_idx;
            COMMIT;" >"${rename_output}" 2>&1 &
    rename_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT
              CASE
                WHEN 'public.lifecycle_lineage_guard_idx'::regclass::oid =
                     ${replacement_oid}
                  THEN 1
                ELSE 0
              END;" 2>/dev/null || printf '0')" = "1" ] ||
            [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-lineage-guard-rename'
                AND wait_event_type = 'Lock';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-lineage-guard-gate';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${alter_pid}" || alter_status=$?
    wait "${rename_pid}" || rename_status=$?
    if [ "${rename_status}" -ne 0 ]; then
        error "lineage guard name swap failed: $(cat "${rename_output}")"
    fi
    if [ "${alter_status}" -ne 0 ] &&
        ! grep -Fq "compaction_lineage" "${alter_output}"; then
        error "lineage guard failed for an unexpected reason: \
$(cat "${alter_output}")"
    fi
    assert_eq "name-swapped BM25 replacement keeps its lineage" \
        "${lineage}" \
        "$(index_lineage public.lifecycle_lineage_guard_idx)"

    sql_super -c "
        DROP EVENT TRIGGER lifecycle_lineage_guard_pause;
        DROP FUNCTION public.lifecycle_lineage_guard_pause();
        DROP TABLE public.lifecycle_lineage_guard_old_docs,
                   public.lifecycle_lineage_guard_new_docs;" >/dev/null
}

test_reindex_authorization_resolution_race() {
    local gate_pid lock_output locker_pid rename_output rename_pid
    local reindex_error reindex_pid reindex_waiting

    lock_output="${DATA_DIR}/reindex-auth-race-lock.out"
    rename_output="${DATA_DIR}/reindex-auth-race-rename.out"
    sql_as durable_owner <<'SQL' >/dev/null 2>&1
CREATE TABLE public.lifecycle_auth_race_old_docs (body text);
CREATE INDEX lifecycle_auth_race_idx
    ON public.lifecycle_auth_race_old_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
CREATE TABLE public.lifecycle_auth_race_new_docs (body text);
CREATE INDEX lifecycle_auth_race_replacement_idx
    ON public.lifecycle_auth_race_new_docs USING bm25(body)
    WITH (text_config = 'english',
          compaction = 'background',
          compaction_schedule = '0 0 1 1 *');
GRANT MAINTAIN ON public.lifecycle_auth_race_old_docs TO durable_writer;
SQL

    PGAPPNAME=lifecycle-auth-race-new-lock sql_as durable_owner -c \
        "BEGIN;
         LOCK TABLE public.lifecycle_auth_race_new_docs IN SHARE MODE;
         SELECT pg_catalog.pg_sleep(120);" >"${lock_output}" 2>&1 &
    locker_pid=$!
    PGAPPNAME=lifecycle-auth-race-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 5);
         SELECT pg_catalog.pg_sleep(120);" >"${lock_output}.gate" 2>&1 &
    gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 5
                AND granted;")" = "1" ] &&
            [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_auth_race_new_docs'::regclass
                AND mode = 'ShareLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-auth-race-rename sql_as durable_owner -c \
        "BEGIN;
         LOCK TABLE public.lifecycle_auth_race_old_docs
           IN ACCESS EXCLUSIVE MODE;
         SELECT pg_catalog.pg_advisory_lock(478, 5);
         ALTER INDEX public.lifecycle_auth_race_idx
           RENAME TO lifecycle_auth_race_retired_idx;
         ALTER INDEX public.lifecycle_auth_race_replacement_idx
           RENAME TO lifecycle_auth_race_idx;
         COMMIT;" >"${rename_output}" 2>&1 &
    rename_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE relation =
                    'public.lifecycle_auth_race_old_docs'::regclass
                AND mode = 'AccessExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    PGAPPNAME=lifecycle-auth-race-reindex \
        PGOPTIONS="-c statement_timeout=5s" \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_writer -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "REINDEX INDEX CONCURRENTLY
              public.lifecycle_auth_race_idx;" \
        >"${lock_output}.reindex" 2>&1 &
    reindex_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-auth-race-reindex'
                AND wait_event_type = 'Lock';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    reindex_waiting="$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-auth-race-reindex'
            AND wait_event_type = 'Lock';")"
    if [ "${reindex_waiting}" != "1" ]; then
        error "authorization race did not reach the original heap lock:
$(cat "${lock_output}.reindex")"
    fi
    log "PASS: authorization race reaches the original heap lock"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'lifecycle-auth-race-gate';" >/dev/null
    wait "${gate_pid}" || true
    if ! wait "${rename_pid}"; then
        error "authorization race rename failed: $(cat "${rename_output}")"
    fi
    if wait "${reindex_pid}"; then
        error "authorization race REINDEX unexpectedly succeeded"
    fi
    reindex_error="$(cat "${lock_output}.reindex")"
    if ! grep -Fq "permission denied for index lifecycle_auth_race_idx" \
        <<<"${reindex_error}"; then
        error "authorization followed a stale relation after rename: \
${reindex_error}"
    fi
    log "PASS: REINDEX authorizes the relation resolved under lock"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name =
            'lifecycle-auth-race-new-lock';" >/dev/null
    wait "${locker_pid}" || true
    sql_super -c "DROP TABLE public.lifecycle_auth_race_old_docs,
                             public.lifecycle_auth_race_new_docs;"
}

test_prior_generation_spill_adoption() {
    local index_oid index_oid_before old_instance current_instance
    local invalid_lineage_error lineage_after lineage_before
    local replacement_instance reuse_output reused_instance reused_lineage
    local threshold_before

    install_signal_probe
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_adopt_docs (id integer, body text);
        INSERT INTO public.lifecycle_adopt_docs
          SELECT i, pg_catalog.format('seed document %s filler', i)
          FROM generate_series(1, 100) AS i;" >/dev/null
    if invalid_lineage_error="$(sql_as durable_owner -c "
        CREATE INDEX lifecycle_adopt_invalid_idx
          ON public.lifecycle_adopt_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_lineage = 'bad');" 2>&1)"; then
        error "background CREATE accepted an invalid lineage"
    fi
    if ! grep -Fq "invalid background compaction lineage" \
        <<<"${invalid_lineage_error}"; then
        error "invalid lineage failed for the wrong reason: \
${invalid_lineage_error}"
    fi
    sql_as durable_owner -c "
        CREATE INDEX lifecycle_adopt_idx
          ON public.lifecycle_adopt_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        GRANT INSERT ON public.lifecycle_adopt_docs TO durable_writer;" \
        >/dev/null 2>&1
    index_oid_before="$(sql_super -c \
        "SELECT 'public.lifecycle_adopt_idx'::regclass::oid;")"
    lineage_before="$(sql_super -c "SELECT pg_catalog.substr(
        option, pg_catalog.length('compaction_lineage=') + 1)
      FROM pg_catalog.pg_class AS relation,
           LATERAL pg_catalog.unnest(relation.reloptions) AS option
      WHERE relation.oid = ${index_oid_before}
        AND option OPERATOR(pg_catalog.~~) 'compaction_lineage=%';")"
    old_instance="$(current_generation_job_id "${index_oid_before}")"
    wait_for_signal_node "${old_instance}" 30

    sql_as durable_owner -c \
        "REINDEX INDEX CONCURRENTLY
           public.lifecycle_adopt_idx;" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_adopt_idx'::regclass::oid;")"
    if [ "${index_oid}" = "${index_oid_before}" ]; then
        error "spill adoption setup did not replace the index OID"
    fi
    lineage_after="$(sql_super -c "SELECT pg_catalog.substr(
        option, pg_catalog.length('compaction_lineage=') + 1)
      FROM pg_catalog.pg_class AS relation,
           LATERAL pg_catalog.unnest(relation.reloptions) AS option
      WHERE relation.oid = ${index_oid}
        AND option OPERATOR(pg_catalog.~~) 'compaction_lineage=%';")"
    assert_eq "concurrent REINDEX preserves logical lineage" \
        "${lineage_before}" "${lineage_after}"
    current_instance="$(current_generation_job_id "${index_oid}")"
    if [ -n "${current_instance}" ] &&
        [ "${current_instance}" != "${old_instance}" ]; then
        sql_as durable_owner -c "SELECT df.cancel(
            '${current_instance}', 'force spill-time adoption');" >/dev/null
        wait_for_terminal "${current_instance}" 30
        sql_super -c "UPDATE df.instances
          SET label = 'retired-current-generation-' || id
          WHERE id = '${current_instance}';"
    fi

    sql_super -c "REVOKE EXECUTE ON FUNCTION
        bm25_compact_step_if_current(oid, oid, oid, oid, oid),
        bm25_background_target_is_current(oid, oid, oid, oid, oid)
        FROM durable_owner;"
    threshold_before="$(sql_super -c \
        "SHOW pg_textsearch.memtable_pages_threshold;")"
    sql_super -c "ALTER SYSTEM SET
                     pg_textsearch.memtable_pages_threshold = 1;" >/dev/null
    sql_super -c "SELECT pg_catalog.pg_reload_conf();" >/dev/null

    reset_signal_probe
    sql_as durable_writer -c "INSERT INTO public.lifecycle_adopt_docs
      SELECT 1000 + document_number,
             (SELECT pg_catalog.string_agg(
                         pg_catalog.format(
                           'adopt%sterm%s',
                           document_number, term_number),
                         ' ')
              FROM generate_series(1, 200) AS term_number)
      FROM generate_series(1, 6) AS document_number;" >/dev/null
    replacement_instance="$(current_generation_job_id "${index_oid}")"
    if [ -z "${replacement_instance}" ] ||
        [ "${replacement_instance}" = "${old_instance}" ] ||
        [ "${replacement_instance}" = "${current_instance}" ]; then
        error "spill did not adopt the prior generation schedule"
    fi
    assert_eq "spill adoption submits as the physical index owner" \
        "durable_owner" \
        "$(sql_super -c "SELECT submitted_by::pg_catalog.text
                          FROM df.instances
                          WHERE id = '${replacement_instance}';")"
    assert_eq "spill adoption signals the replacement once as owner" \
        "${replacement_instance}:durable_owner" \
        "$(sql_super -c "SELECT instance_id || ':' || role_name
                          FROM public.compaction_signal_audit;")"
    assert_eq "spill adoption attempts exactly one signal" "1" \
        "$(signal_attempt_count)"
    assert_eq "spill adoption preserves the prior generation schedule" "t" \
        "$(sql_super -c "SELECT label LIKE '%:' ||
            pg_catalog.encode(
              pg_catalog.convert_to('0 0 1 1 *', 'UTF8'), 'hex')
          FROM df.instances
          WHERE id = '${replacement_instance}';")"
    assert_eq "spill adoption restores private helper access" "t:t" \
        "$(helper_privileges_for_role durable_owner)"
    assert_eq "spill adoption preserves the durable dependency" "1" \
        "$(dependency_count)"

    sql_as durable_owner -c "SELECT df.cancel(
        '${replacement_instance}', 'lineage reuse test');" >/dev/null
    wait_for_terminal "${replacement_instance}" 30
    sql_as durable_owner -c "
        DROP INDEX public.lifecycle_adopt_idx;
        CREATE INDEX lifecycle_adopt_idx
          ON public.lifecycle_adopt_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '5 4 3 2 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'public.lifecycle_adopt_idx'::regclass::oid;")"
    reused_lineage="$(sql_super -c "SELECT pg_catalog.substr(
        option, pg_catalog.length('compaction_lineage=') + 1)
      FROM pg_catalog.pg_class AS relation,
           LATERAL pg_catalog.unnest(relation.reloptions) AS option
      WHERE relation.oid = ${index_oid}
        AND option OPERATOR(pg_catalog.~~) 'compaction_lineage=%';")"
    if [ "${reused_lineage}" = "${lineage_before}" ]; then
        error "recreated index reused the dropped index lineage"
    fi
    reused_instance="$(current_generation_job_id "${index_oid}")"
    wait_for_signal_node "${reused_instance}" 30
    sql_as durable_owner -c "SELECT df.cancel(
        '${reused_instance}', 'hide replacement lineage history');" \
        >/dev/null
    wait_for_terminal "${reused_instance}" 30
    sql_super -c "UPDATE df.instances
      SET label = 'retired-reused-index-' || id
      WHERE id = '${reused_instance}';"

    reset_signal_probe
    reuse_output="$(sql_as durable_writer -c "
      INSERT INTO public.lifecycle_adopt_docs
      SELECT 2000 + document_number,
             (SELECT pg_catalog.string_agg(
                         pg_catalog.format(
                           'reuse%sterm%s',
                           document_number, term_number),
                         ' ')
              FROM generate_series(1, 200) AS term_number)
      FROM generate_series(1, 6) AS document_number;" 2>&1)"
    if ! grep -Fq "requires explicit adoption" <<<"${reuse_output}"; then
        error "recreated index did not reject unrelated lineage history: \
${reuse_output}"
    fi
    assert_eq "recreated index does not signal an unrelated workflow" \
        "0" "$(signal_attempt_count)"
    assert_eq "recreated index does not adopt an unrelated schedule" \
        "" "$(current_generation_job_id "${index_oid}")"

    sql_super -c "ALTER SYSTEM SET
        pg_textsearch.memtable_pages_threshold = '${threshold_before}';" \
        >/dev/null
    sql_super -c "SELECT pg_catalog.pg_reload_conf();" >/dev/null
    sql_as durable_owner -c "SELECT df.cancel(
        instance.id, 'spill adoption test complete')
      FROM df.instances AS instance
      WHERE instance.label OPERATOR(pg_catalog.~~)
            'pg_textsearch:bg:v1:%:${index_oid}:%'
        AND instance.status OPERATOR(pg_catalog.=)
            ANY (ARRAY['pending', 'running']::pg_catalog.text[]);" \
        >/dev/null
    restore_signal_probe
    sql_as durable_owner -c "DROP TABLE public.lifecycle_adopt_docs;"
}

install_signal_probe() {
    sql_super <<'SQL'
ALTER FUNCTION df.signal(text, text, text) RENAME TO signal_v028;

CREATE SEQUENCE public.compaction_signal_attempt_seq;
CREATE TABLE public.compaction_signal_audit (
    attempt bigint PRIMARY KEY,
    instance_id text NOT NULL,
    role_name name NOT NULL
);
CREATE TABLE public.compaction_signal_fault (
    instance_id text PRIMARY KEY,
    fault text NOT NULL
);

GRANT USAGE, SELECT ON SEQUENCE public.compaction_signal_attempt_seq
    TO durable_owner, durable_owner_two;
GRANT INSERT ON public.compaction_signal_audit
    TO durable_owner, durable_owner_two;
GRANT SELECT ON public.compaction_signal_fault
    TO durable_owner, durable_owner_two;

CREATE FUNCTION df.signal(
    instance_id text,
    signal_name text,
    signal_data text DEFAULT '{}')
RETURNS text
LANGUAGE plpgsql
STRICT
SET search_path = pg_catalog, pg_temp
AS $body$
DECLARE
    attempt_number bigint;
    injected_fault text;
BEGIN
    attempt_number :=
        nextval('public.compaction_signal_attempt_seq'::regclass);
    SELECT fault
      INTO injected_fault
      FROM public.compaction_signal_fault AS control
      WHERE control.instance_id OPERATOR(pg_catalog.=) $1
         OR control.instance_id OPERATOR(pg_catalog.=) '*'
      ORDER BY control.instance_id OPERATOR(pg_catalog.=) $1 DESC
      LIMIT 1;

    IF injected_fault OPERATOR(pg_catalog.=) 'error' THEN
        RAISE EXCEPTION 'probe ordinary signal failure';
    ELSIF injected_fault OPERATOR(pg_catalog.=) 'cancel' THEN
        RAISE EXCEPTION 'probe query cancellation'
            USING ERRCODE = '57014';
    ELSIF injected_fault OPERATOR(pg_catalog.=) 'gate' THEN
        PERFORM pg_catalog.pg_advisory_xact_lock_shared(478, 11);
    END IF;

    INSERT INTO public.compaction_signal_audit(
        attempt, instance_id, role_name)
    VALUES (attempt_number, $1, current_user);
    RETURN pg_catalog.format('probe-%s', attempt_number);
END
$body$;

ALTER EXTENSION pg_durable ADD FUNCTION df.signal(text, text, text);
REVOKE ALL ON FUNCTION df.signal(text, text, text) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION df.signal(text, text, text)
    TO durable_owner, durable_owner_two;

CREATE FUNCTION public.queue_force_spills(
    target_table regclass,
    target_index regclass,
    first_id integer,
    rounds integer)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    FOR round_number IN 1..rounds LOOP
        EXECUTE pg_catalog.format(
            'INSERT INTO %s(id, body) '
            'SELECT $1 + $2 * 100 + value, '
            'pg_catalog.format(''queue round %%s row %%s filler'', '
            '$2, value) FROM pg_catalog.generate_series(1, 20) AS value',
            target_table)
        USING first_id, round_number;
        PERFORM public.bm25_spill_index(target_index::text);
    END LOOP;
END
$body$;
SQL
}

restore_signal_probe() {
    sql_super <<'SQL'
ALTER EXTENSION pg_durable DROP FUNCTION df.signal(text, text, text);
DROP FUNCTION df.signal(text, text, text);
ALTER FUNCTION df.signal_v028(text, text, text) RENAME TO signal;
DROP FUNCTION public.queue_force_spills(regclass, regclass, integer, integer);
DROP TABLE public.compaction_signal_fault;
DROP TABLE public.compaction_signal_audit;
DROP SEQUENCE public.compaction_signal_attempt_seq;
SQL
}

reset_signal_probe() {
    sql_super -c "TRUNCATE public.compaction_signal_audit,
                           public.compaction_signal_fault;
                   ALTER SEQUENCE public.compaction_signal_attempt_seq
                     RESTART WITH 1;"
}

signal_attempt_count() {
    sql_super -c "SELECT CASE WHEN is_called THEN last_value ELSE 0 END
      FROM public.compaction_signal_attempt_seq;"
}

test_request_queue_runtime() {
    local abort_instance cancel_error cancel_instance dedup_a_instance
    local dedup_b_instance drop_instance failure_a_instance failure_b_instance
    local failure_output lock_a_instance lock_b_instance lock_a_output
    local lock_a_pid lock_a_status=0 lock_b_output lock_b_pid lock_b_status=0
    local lock_gate_pid savepoint_instance

    sql_as durable_owner -c "
        CREATE TABLE public.queue_below_docs (id integer, body text);
        CREATE INDEX queue_below_idx
          ON public.queue_below_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_abort_docs (id integer, body text);
        CREATE INDEX queue_abort_idx
          ON public.queue_abort_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_dedup_a_docs (id integer, body text);
        CREATE INDEX queue_dedup_a_idx
          ON public.queue_dedup_a_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_dedup_b_docs (id integer, body text);
        CREATE INDEX queue_dedup_b_idx
          ON public.queue_dedup_b_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_savepoint_docs (id integer, body text);
        CREATE INDEX queue_savepoint_idx
          ON public.queue_savepoint_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_drop_docs (id integer, body text);
        CREATE INDEX queue_drop_idx
          ON public.queue_drop_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_failure_a_docs (id integer, body text);
        CREATE INDEX queue_failure_a_idx
          ON public.queue_failure_a_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_failure_b_docs (id integer, body text);
        CREATE INDEX queue_failure_b_idx
          ON public.queue_failure_b_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_cancel_docs (id integer, body text);
        CREATE INDEX queue_cancel_idx
          ON public.queue_cancel_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_lock_a_docs (id integer, body text);
        CREATE INDEX queue_lock_a_idx
          ON public.queue_lock_a_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE TABLE public.queue_lock_b_docs (id integer, body text);
        CREATE INDEX queue_lock_b_idx
          ON public.queue_lock_b_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1

    abort_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_abort_idx'::regclass::oid;")")"
    dedup_a_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_dedup_a_idx'::regclass::oid;")")"
    dedup_b_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_dedup_b_idx'::regclass::oid;")")"
    savepoint_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_savepoint_idx'::regclass::oid;")")"
    drop_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_drop_idx'::regclass::oid;")")"
    failure_a_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_failure_a_idx'::regclass::oid;")")"
    failure_b_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_failure_b_idx'::regclass::oid;")")"
    cancel_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_cancel_idx'::regclass::oid;")")"
    lock_a_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_lock_a_idx'::regclass::oid;")")"
    lock_b_instance="$(current_generation_job_id \
        "$(sql_super -c \
            "SELECT 'public.queue_lock_b_idx'::regclass::oid;")")"

    install_signal_probe

    reset_signal_probe
    sql_as durable_owner -c "SELECT public.queue_force_spills(
        'public.queue_below_docs'::regclass,
        'public.queue_below_idx'::regclass, 0, 1);" >/dev/null
    assert_eq "below-threshold spill does not signal" "0" \
        "$(signal_attempt_count)"

    reset_signal_probe
    sql_as durable_owner <<'SQL' >/dev/null
BEGIN;
SELECT public.queue_force_spills(
    'public.queue_abort_docs'::regclass,
    'public.queue_abort_idx'::regclass, 1000, 2);
ROLLBACK;
BEGIN;
SELECT 1;
COMMIT;
SQL
    assert_eq "top-level abort does not leak a pending request" "0" \
        "$(signal_attempt_count)"
    assert_eq "aborted queue test targeted a live workflow" "1" \
        "$(sql_super -c "SELECT count(*) FROM df.instances
                          WHERE id = '${abort_instance}';")"

    reset_signal_probe
    sql_as durable_owner <<'SQL' >/dev/null
BEGIN;
SELECT public.queue_force_spills(
    'public.queue_dedup_a_docs'::regclass,
    'public.queue_dedup_a_idx'::regclass, 2000, 3);
SELECT public.queue_force_spills(
    'public.queue_dedup_b_docs'::regclass,
    'public.queue_dedup_b_idx'::regclass, 3000, 2);
COMMIT;
SQL
    assert_eq "request queue signals two indexes once each" "2" \
        "$(signal_attempt_count)"
    assert_eq "multiple request hits deduplicate per index" "1:1" \
        "$(sql_super -c "SELECT
            count(*) FILTER (WHERE instance_id = '${dedup_a_instance}')
            || ':' ||
            count(*) FILTER (WHERE instance_id = '${dedup_b_instance}')
          FROM public.compaction_signal_audit;")"

    reset_signal_probe
    sql_as durable_owner <<'SQL' >/dev/null
BEGIN;
SAVEPOINT queue_spill;
SELECT public.queue_force_spills(
    'public.queue_savepoint_docs'::regclass,
    'public.queue_savepoint_idx'::regclass, 4000, 2);
DROP INDEX public.queue_savepoint_idx;
ROLLBACK TO SAVEPOINT queue_spill;
COMMIT;
SQL
    assert_eq "savepoint-surviving spill request dispatches" "1" \
        "$(signal_attempt_count)"
    assert_eq "rolled-back DROP retains the original request target" \
        "${savepoint_instance}" \
        "$(sql_super -c "SELECT instance_id
                          FROM public.compaction_signal_audit;")"

    reset_signal_probe
    sql_as durable_owner <<'SQL' >/dev/null
BEGIN;
SELECT public.queue_force_spills(
    'public.queue_drop_docs'::regclass,
    'public.queue_drop_idx'::regclass, 5000, 2);
DROP INDEX public.queue_drop_idx;
COMMIT;
SQL
    assert_eq "committed DROP suppresses stale-OID dispatch" "0" \
        "$(signal_attempt_count)"
    assert_eq "dropped request targeted an existing workflow" "1" \
        "$(sql_super -c "SELECT count(*) FROM df.instances
                          WHERE id = '${drop_instance}';")"

    reset_signal_probe
    sql_super -c "INSERT INTO public.compaction_signal_fault
        VALUES ('${failure_a_instance}', 'error');"
    if ! failure_output="$(sql_as durable_owner <<'SQL' 2>&1
BEGIN;
SELECT public.queue_force_spills(
    'public.queue_failure_a_docs'::regclass,
    'public.queue_failure_a_idx'::regclass, 6000, 2);
SELECT public.queue_force_spills(
    'public.queue_failure_b_docs'::regclass,
    'public.queue_failure_b_idx'::regclass, 7000, 2);
COMMIT;
SQL
    )"; then
        error "ordinary PRE_COMMIT signal failure aborted writer data: \
${failure_output}"
    fi
    if ! grep -Fq "probe ordinary signal failure" \
        <<<"${failure_output}"; then
        error "ordinary PRE_COMMIT signal failure emitted no warning"
    fi
    assert_eq "ordinary failure continues later dispatch" "2" \
        "$(signal_attempt_count)"
    assert_eq "ordinary failure preserves both writers' rows" "40:40" \
        "$(sql_super -c "SELECT
            (SELECT count(*) FROM public.queue_failure_a_docs)
            || ':' ||
            (SELECT count(*) FROM public.queue_failure_b_docs);")"
    assert_eq "ordinary failure continues to the second index" \
        "${failure_b_instance}:durable_owner" \
        "$(sql_super -c "SELECT instance_id || ':' || role_name
                          FROM public.compaction_signal_audit;")"

    reset_signal_probe
    sql_super -c "INSERT INTO public.compaction_signal_fault
        VALUES ('${cancel_instance}', 'cancel');"
    if cancel_error="$(sql_as durable_owner <<'SQL' 2>&1
BEGIN;
SELECT public.queue_force_spills(
    'public.queue_cancel_docs'::regclass,
    'public.queue_cancel_idx'::regclass, 8000, 2);
COMMIT;
SQL
    )"; then
        error "query cancellation during PRE_COMMIT unexpectedly committed"
    fi
    if ! grep -Fq "probe query cancellation" <<<"${cancel_error}"; then
        error "query cancellation did not propagate: ${cancel_error}"
    fi
    assert_eq "query cancellation attempted one signal" "1" \
        "$(signal_attempt_count)"
    assert_eq "query cancellation rolls back writer data" "0" \
        "$(sql_super -c \
            "SELECT count(*) FROM public.queue_cancel_docs;")"

    reset_signal_probe
    sql_super -c "INSERT INTO public.compaction_signal_fault
        VALUES ('${lock_a_instance}', 'gate'),
               ('${lock_b_instance}', 'gate');"
    wait_for_signal_node "${lock_a_instance}" 30
    wait_for_signal_node "${lock_b_instance}" 30
    PGAPPNAME=queue-lock-gate sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 11);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${DATA_DIR}/queue-lock-gate.out" 2>&1 &
    lock_gate_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 11
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    lock_a_output="${DATA_DIR}/queue-lock-a.out"
    lock_b_output="${DATA_DIR}/queue-lock-b.out"
    PGAPPNAME=queue-lock-a \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            SELECT public.queue_force_spills(
              'public.queue_lock_a_docs'::regclass,
              'public.queue_lock_a_idx'::regclass, 9000, 2);
            SELECT public.queue_force_spills(
              'public.queue_lock_b_docs'::regclass,
              'public.queue_lock_b_idx'::regclass, 10000, 2);
            COMMIT;" >"${lock_a_output}" 2>&1 &
    lock_a_pid=$!
    PGAPPNAME=queue-lock-b \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            SELECT public.queue_force_spills(
              'public.queue_lock_b_docs'::regclass,
              'public.queue_lock_b_idx'::regclass, 11000, 2);
            SELECT public.queue_force_spills(
              'public.queue_lock_a_docs'::regclass,
              'public.queue_lock_a_idx'::regclass, 12000, 2);
            COMMIT;" >"${lock_b_output}" 2>&1 &
    lock_b_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT pg_catalog.count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name IN ('queue-lock-a', 'queue-lock-b')
                AND wait_event_type = 'Lock';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "reverse-order request flush has one admitted writer" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name IN ('queue-lock-a', 'queue-lock-b')
            AND wait_event_type = 'Lock';")"
    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'queue-lock-gate';" >/dev/null
    wait "${lock_gate_pid}" || true
    wait "${lock_a_pid}" || lock_a_status=$?
    wait "${lock_b_pid}" || lock_b_status=$?
    if [ "${lock_a_status}" -ne 0 ] || [ "${lock_b_status}" -ne 0 ]; then
        error "reverse-order request flush deadlocked:
first: $(cat "${lock_a_output}")
second: $(cat "${lock_b_output}")"
    fi
    if grep -Fq "deadlock detected" "${lock_a_output}" ||
        grep -Fq "deadlock detected" "${lock_b_output}"; then
        error "reverse-order request flush reported a caught deadlock"
    fi
    assert_eq "reverse-order request flush commits both writers" "80:80" \
        "$(sql_super -c "SELECT
            (SELECT count(*) FROM public.queue_lock_a_docs)
            || ':' ||
            (SELECT count(*) FROM public.queue_lock_b_docs);")"
    assert_eq "contended request flush signals admitted work once" "t" \
        "$(sql_super -c "SELECT
            count(*) OPERATOR(pg_catalog.>=) 1
            AND count(*) OPERATOR(pg_catalog.<=) 2
            AND count(*) FILTER (
              WHERE instance_id = '${lock_a_instance}')
                OPERATOR(pg_catalog.<=) 1
            AND count(*) FILTER (
              WHERE instance_id = '${lock_b_instance}')
                OPERATOR(pg_catalog.<=) 1
          FROM public.compaction_signal_audit;")"

    restore_signal_probe
    sql_super -c "DROP TABLE public.queue_below_docs,
                              public.queue_abort_docs,
                              public.queue_dedup_a_docs,
                              public.queue_dedup_b_docs,
                              public.queue_savepoint_docs,
                              public.queue_drop_docs,
                              public.queue_failure_a_docs,
                              public.queue_failure_b_docs,
                              public.queue_cancel_docs,
                              public.queue_lock_a_docs,
                              public.queue_lock_b_docs;"
}

test_precommit_signal_observes_published_spill() {
    local blocker_pid blocker_output index_oid instance_id
    local writer_pid writer_output writer_status=0

    sql_as durable_owner -c "
        CREATE TABLE public.precommit_signal_docs (id integer, body text);
        CREATE INDEX precommit_signal_idx
          ON public.precommit_signal_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'public.precommit_signal_idx'::regclass::oid;")"
    instance_id="$(current_generation_job_id "${index_oid}")"
    wait_for_signal_node "${instance_id}" 90

    sql_super <<'SQL'
CREATE TABLE public.precommit_compaction_audit (
    role_name name NOT NULL
);
ALTER TABLE public.precommit_compaction_audit OWNER TO durable_owner;
REVOKE ALL ON public.precommit_compaction_audit FROM PUBLIC;
GRANT INSERT ON public.precommit_compaction_audit TO durable_owner;

CREATE FUNCTION bm25_compact_step_if_current_precommit_test_c(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS '$libdir/pg_textsearch', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;

REVOKE ALL ON FUNCTION
    bm25_compact_step_if_current_precommit_test_c(oid, oid, oid, oid, oid)
    FROM PUBLIC;
GRANT EXECUTE ON FUNCTION
    bm25_compact_step_if_current_precommit_test_c(oid, oid, oid, oid, oid)
    TO durable_owner;

CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
LANGUAGE plpgsql
VOLATILE
STRICT
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    INSERT INTO public.precommit_compaction_audit(role_name)
    VALUES (current_user);
    RETURN public.bm25_compact_step_if_current_precommit_test_c(
        index_oid, database_oid, tablespace_oid, relfilenumber, owner_oid);
END
$body$;

ALTER FUNCTION df.signal(text, text, text) RENAME TO signal_v028;

CREATE FUNCTION df.signal(
    instance_id text,
    signal_name text,
    signal_data text DEFAULT '{}')
RETURNS text
LANGUAGE plpgsql
STRICT
SET search_path = pg_catalog, pg_temp
AS $body$
DECLARE
    result text;
BEGIN
    result := df.signal_v028($1, $2, $3);
    PERFORM pg_catalog.pg_advisory_xact_lock_shared(478, 12);
    RETURN result;
END
$body$;

ALTER EXTENSION pg_durable ADD FUNCTION df.signal(text, text, text);
REVOKE ALL ON FUNCTION df.signal(text, text, text) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION df.signal(text, text, text) TO durable_owner;
SQL

    blocker_output="${DATA_DIR}/precommit-signal-blocker.out"
    PGAPPNAME=precommit-signal-blocker sql_super -c \
        "SELECT pg_catalog.pg_advisory_lock(478, 12);
         SELECT pg_catalog.pg_sleep(120);" \
        >"${blocker_output}" 2>&1 &
    blocker_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_locks
              WHERE locktype = 'advisory'
                AND classid = 478
                AND objid = 12
                AND mode = 'ExclusiveLock'
                AND granted;")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    writer_output="${DATA_DIR}/precommit-signal-writer.out"
    PGAPPNAME=precommit-signal-writer \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "BEGIN;
            INSERT INTO public.precommit_signal_docs
            SELECT value, pg_catalog.format(
                'precommit first row %s filler', value)
            FROM pg_catalog.generate_series(1, 20) AS value;
            SELECT bm25_spill_index('public.precommit_signal_idx');
            INSERT INTO public.precommit_signal_docs
            SELECT value + 20, pg_catalog.format(
                'precommit second row %s filler', value)
            FROM pg_catalog.generate_series(1, 20) AS value;
            SELECT bm25_spill_index('public.precommit_signal_idx');
            COMMIT;" >"${writer_output}" 2>&1 &
    writer_pid=$!

    for _ in $(seq 1 300); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'precommit-signal-writer'
                AND wait_event_type = 'Lock';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "writer waits after sending the PRE_COMMIT signal" "1" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'precommit-signal-writer'
            AND wait_event_type = 'Lock';")"
    assert_eq "writer rows remain uncommitted while the signal is live" "0" \
        "$(sql_super -c \
            "SELECT count(*) FROM public.precommit_signal_docs;")"

    for _ in $(seq 1 300); do
        if [ "$(sql_super -c \
            "SELECT count(*) FROM public.precommit_compaction_audit;")" \
            -gt 0 ]; then
            break
        fi
        sleep 0.1
    done
    assert_eq "worker runs before the spilling writer commits" "t" \
        "$(sql_super -c "SELECT count(*) > 0
                                AND pg_catalog.bool_and(
                                    role_name = 'durable_owner')
                          FROM public.precommit_compaction_audit;")"
    assert_eq "pre-commit worker consumes the published compaction debt" "f" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'public.precommit_signal_idx'::regclass);")"

    sql_super -c "SELECT pg_catalog.pg_terminate_backend(pid)
      FROM pg_catalog.pg_stat_activity
      WHERE application_name = 'precommit-signal-blocker';" >/dev/null
    wait "${blocker_pid}" || true
    wait "${writer_pid}" || writer_status=$?
    if [ "${writer_status}" -ne 0 ]; then
        error "writer failed after PRE_COMMIT signal gate:
$(cat "${writer_output}")"
    fi
    assert_eq "writer commits after the worker has compacted" "40" \
        "$(sql_super -c \
            "SELECT count(*) FROM public.precommit_signal_docs;")"

    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS '$libdir/pg_textsearch', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;

ALTER EXTENSION pg_durable DROP FUNCTION df.signal(text, text, text);
DROP FUNCTION df.signal(text, text, text);
ALTER FUNCTION df.signal_v028(text, text, text) RENAME TO signal;
DROP FUNCTION
    bm25_compact_step_if_current_precommit_test_c(oid, oid, oid, oid, oid);
DROP TABLE public.precommit_compaction_audit;
SQL
    sql_as durable_owner -c \
        "SELECT df.cancel('${instance_id}', 'precommit test complete');" \
        >/dev/null
    wait_for_terminal "${instance_id}" 30
    sql_as durable_owner -c "DROP TABLE public.precommit_signal_docs;"
}

test_repeatable_read_admission_snapshot() {
    local index_oid initial_job reader_output reader_pid

    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_rr_docs
          (id integer, body text);
        CREATE INDEX lifecycle_rr_idx
          ON public.lifecycle_rr_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    index_oid="$(sql_super -c "SELECT
        'public.lifecycle_rr_idx'::regclass::oid;")"
    initial_job="$(current_generation_job_id "${index_oid}")"
    sql_as durable_owner -c \
        "SELECT df.cancel('${initial_job}', 'repeatable-read setup');" \
        >/dev/null
    wait_for_terminal "${initial_job}" 30

    reader_output="${DATA_DIR}/repeatable-read-admission.out"
    PGAPPNAME=lifecycle-repeatable-read sql_as durable_owner <<'SQL' \
        >"${reader_output}" 2>&1 &
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM df.instances;
SELECT pg_catalog.pg_sleep(5);
INSERT INTO public.lifecycle_rr_docs
SELECT value, pg_catalog.format('reader document %s alpha beta gamma', value)
FROM pg_catalog.generate_series(1, 20) AS value;
SELECT bm25_spill_index('public.lifecycle_rr_idx');
INSERT INTO public.lifecycle_rr_docs
SELECT 100 + value,
       pg_catalog.format('reader second document %s delta epsilon', value)
FROM pg_catalog.generate_series(1, 20) AS value;
SELECT bm25_spill_index('public.lifecycle_rr_idx');
COMMIT;
SQL
    reader_pid=$!
    for _ in $(seq 1 100); do
        if [ "$(sql_super -c "SELECT count(*)
              FROM pg_catalog.pg_stat_activity
              WHERE application_name = 'lifecycle-repeatable-read'
                AND query OPERATOR(pg_catalog.~~) '%pg_sleep%';")" = "1" ]; then
            break
        fi
        sleep 0.1
    done

    sql_as durable_owner -c "
        INSERT INTO public.lifecycle_rr_docs
        SELECT 1000 + value,
               pg_catalog.format('writer document %s one two three', value)
        FROM pg_catalog.generate_series(1, 20) AS value;
        SELECT bm25_spill_index('public.lifecycle_rr_idx');
        INSERT INTO public.lifecycle_rr_docs
        SELECT 2000 + value,
               pg_catalog.format('writer second %s four five six', value)
        FROM pg_catalog.generate_series(1, 20) AS value;
        SELECT bm25_spill_index('public.lifecycle_rr_idx');" >/dev/null 2>&1
    wait "${reader_pid}" || {
        cat "${reader_output}" >&2
        error "repeatable-read writer failed"
    }
    assert_eq "repeatable-read admission keeps one current workflow" "1" \
        "$(current_generation_job_count "${index_oid}")"

    sql_super -c "DROP TABLE public.lifecycle_rr_docs;"
}

test_actor_writer_worker_identity() {
    local activation_audit_id activation_audit_rows index_oid instance_id
    local memtable_threshold_before writer_audit_id writer_audit_rows

    sql_super -c "REVOKE durable_no_textsearch_schema FROM durable_writer;
                   GRANT durable_owner TO durable_actor;
                   REVOKE CREATE ON SCHEMA public FROM durable_writer;
                   CREATE TABLE identity_docs (id integer, body text);
                   ALTER TABLE identity_docs OWNER TO durable_owner;
                   GRANT INSERT ON identity_docs TO durable_writer;
                   CREATE INDEX identity_docs_idx
                     ON identity_docs USING bm25(body)
                     WITH (text_config = 'english',
                           compaction = 'manual',
                           compaction_schedule = '0 0 1 1 *');
                   CREATE TABLE worker_identity_audit (
                       audit_id bigint GENERATED ALWAYS AS IDENTITY,
                       role_name name);
                   ALTER TABLE worker_identity_audit OWNER TO durable_owner;
                   REVOKE ALL ON worker_identity_audit FROM PUBLIC;
                   GRANT INSERT ON worker_identity_audit TO durable_owner;"
    sql_super <<'SQL'
CREATE FUNCTION bm25_compact_step_if_current_test_c(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS '$libdir/pg_textsearch', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;

REVOKE ALL ON FUNCTION
    bm25_compact_step_if_current_test_c(oid, oid, oid, oid, oid)
    FROM PUBLIC;
GRANT EXECUTE ON FUNCTION
    bm25_compact_step_if_current_test_c(oid, oid, oid, oid, oid)
    TO durable_owner;

CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
LANGUAGE plpgsql
VOLATILE
STRICT
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    INSERT INTO public.worker_identity_audit(role_name)
    VALUES (current_user);
    RETURN false;
END
$body$;
SQL

    assert_eq "writer is not a member of a pg_durable-enabled role" "f" \
        "$(sql_super -c "SELECT pg_catalog.pg_has_role(
            'durable_writer', 'durable_no_textsearch_schema', 'MEMBER');")"
    assert_eq "writer lacks effective df schema access" "f" \
        "$(sql_super -c "SELECT pg_catalog.has_schema_privilege(
            'durable_writer', 'df', 'USAGE');")"
    assert_eq "writer lacks effective df.start access" "t" \
        "$(sql_super -c "SELECT NOT (
            pg_catalog.has_schema_privilege(
                'durable_writer', 'df', 'USAGE')
            AND pg_catalog.has_function_privilege(
                'durable_writer', procedure.oid, 'EXECUTE'))
          FROM pg_catalog.pg_proc AS procedure
          JOIN pg_catalog.pg_namespace AS namespace
            ON namespace.oid = procedure.pronamespace
          JOIN pg_catalog.pg_depend AS dependency
            ON dependency.classid = 'pg_catalog.pg_proc'::regclass
           AND dependency.objid = procedure.oid
           AND dependency.deptype = 'e'
          JOIN pg_catalog.pg_extension AS extension
            ON extension.oid = dependency.refobjid
          WHERE namespace.nspname = 'df'
            AND procedure.proname = 'start'
            AND extension.extname = 'pg_durable';")"
    assert_eq "writer lacks privileges on required df tables" "t" \
        "$(sql_super -c "SELECT
            NOT pg_catalog.has_any_column_privilege(
                'durable_writer', 'df.instances', 'SELECT')
            AND NOT pg_catalog.has_any_column_privilege(
                'durable_writer', 'df.instances', 'INSERT')
            AND NOT pg_catalog.has_any_column_privilege(
                'durable_writer', 'df.nodes', 'INSERT')
            AND NOT pg_catalog.has_any_column_privilege(
                'durable_writer', 'df.vars', 'SELECT');")"
    assert_eq "writer lacks private helper EXECUTE" "t" \
        "$(sql_super -c "SELECT
            NOT pg_catalog.has_function_privilege(
                'durable_writer',
                'bm25_compact_step_if_current(oid,oid,oid,oid,oid)',
                'EXECUTE')
            AND NOT pg_catalog.has_function_privilege(
                'durable_writer',
                'bm25_background_target_is_current(oid,oid,oid,oid,oid)',
                'EXECUTE');")"
    assert_eq "writer has only INSERT table privilege" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.has_table_privilege(
                'durable_writer', 'identity_docs', 'INSERT')
            AND NOT pg_catalog.has_table_privilege(
                'durable_writer', 'identity_docs',
                'SELECT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER, ' ||
                'MAINTAIN')
            AND NOT pg_catalog.has_any_column_privilege(
                'durable_writer', 'identity_docs', 'SELECT')
            AND NOT pg_catalog.has_any_column_privilege(
                'durable_writer', 'identity_docs', 'UPDATE')
            AND NOT pg_catalog.has_any_column_privilege(
                'durable_writer', 'identity_docs', 'REFERENCES');")"

    assert_eq "actor command starts without SET ROLE" \
        "durable_actor:durable_actor" \
        "$(sql_as durable_actor -c \
            "SELECT current_user || ':' || session_user;")"
    sql_as durable_actor -c "
        ALTER INDEX public.identity_docs_idx
          SET (compaction = 'background');" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'identity_docs_idx'::regclass::oid;")"
    instance_id="$(active_job_id_for_owner \
        "${index_oid}" durable_owner)"
    [ -n "${instance_id}" ] ||
        error "actor-enabled index has no durable_owner workflow"
    assert_eq "actor activation submits as the index owner" \
        "durable_owner" \
        "$(sql_super -c "SELECT submitted_by::text
                          FROM df.instances
                          WHERE id = '${instance_id}';")"

    wait_for_audit_rows 1 30
    wait_for_signal_node "${instance_id}" 90
    activation_audit_rows="$(sql_super -c \
        "SELECT count(*) FROM worker_identity_audit;")"
    activation_audit_id="$(sql_super -c \
        "SELECT max(audit_id) FROM worker_identity_audit;")"
    assert_eq "initial activation cascade executes as the index owner" "t" \
        "$(sql_super -c "SELECT count(*) > 0
                                AND pg_catalog.bool_and(
                                    role_name = 'durable_owner')
                          FROM worker_identity_audit
                          WHERE audit_id <= ${activation_audit_id};")"

    memtable_threshold_before="$(sql_super -c \
        "SHOW pg_textsearch.memtable_pages_threshold;")"
    sql_super -c "ALTER SYSTEM SET
                     pg_textsearch.memtable_pages_threshold = 1;" >/dev/null
    sql_super -c "SELECT pg_catalog.pg_reload_conf();" >/dev/null
    assert_eq "writer spill threshold is active" "1" \
        "$(sql_super -c \
            "SHOW pg_textsearch.memtable_pages_threshold;")"
    sql_as durable_writer -c "INSERT INTO public.identity_docs
        SELECT document_number,
               (SELECT pg_catalog.string_agg(
                           pg_catalog.format(
                               'writer%sterm%s', document_number, term_number),
                           ' ')
                FROM generate_series(1, 200) AS term_number)
        FROM generate_series(1, 6) AS document_number;" >/dev/null
    wait_for_audit_rows "$((activation_audit_rows + 1))" 30
    assert_eq "writer-triggered signal executes as the index owner" "t" \
        "$(sql_super -c "SELECT count(*) > 0
                                AND pg_catalog.bool_and(
                                    role_name = 'durable_owner')
                          FROM worker_identity_audit
                          WHERE audit_id > ${activation_audit_id};")"
    wait_for_signal_node "${instance_id}" 30
    writer_audit_rows="$(sql_super -c \
        "SELECT count(*) FROM worker_identity_audit;")"
    writer_audit_id="$(sql_super -c \
        "SELECT max(audit_id) FROM worker_identity_audit;")"
    sql_super -c "ALTER SYSTEM RESET
                     pg_textsearch.memtable_pages_threshold;" >/dev/null
    sql_super -c "SELECT pg_catalog.pg_reload_conf();" >/dev/null
    assert_eq "writer spill threshold is restored" \
        "${memtable_threshold_before}" \
        "$(sql_super -c \
            "SHOW pg_textsearch.memtable_pages_threshold;")"
    assert_eq "insert-only writer creates compaction debt" "t" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'identity_docs_idx'::regclass);")"
    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
LANGUAGE plpgsql
VOLATILE
STRICT
SET search_path = pg_catalog, pg_temp
AS $body$
BEGIN
    INSERT INTO public.worker_identity_audit(role_name)
    VALUES (current_user);
    RETURN public.bm25_compact_step_if_current_test_c(
        index_oid, database_oid, tablespace_oid, relfilenumber, owner_oid);
END
$body$;
SQL
    sql_as durable_owner -c \
        "SELECT df.signal('${instance_id}', 'compact', '{}');" >/dev/null
    wait_for_audit_rows "$((writer_audit_rows + 1))" 30
    assert_eq "controlled consuming signal executes as the index owner" "t" \
        "$(sql_super -c "SELECT count(*) > 0
                                AND pg_catalog.bool_and(
                                    role_name = 'durable_owner')
                          FROM worker_identity_audit
                          WHERE audit_id > ${writer_audit_id};")"
    wait_for_no_debt identity_docs_idx 60
    assert_eq "owner signal consumes writer-created debt" "f" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'identity_docs_idx'::regclass);")"
    assert_eq "every worker step executes as the index owner" "t" \
        "$(sql_super -c "SELECT count(*) >= 3
                                AND pg_catalog.bool_and(
                                    role_name = 'durable_owner')
                          FROM worker_identity_audit;")"

    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS '$libdir/pg_textsearch', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;
SQL
    assert_eq "worker helper definition is restored" \
        "c:tp_compact_index_step_if_current:true" \
        "$(sql_super -c "SELECT language.lanname || ':' ||
                                procedure.prosrc || ':' ||
                                (procedure.proconfig IS NULL)
                          FROM pg_catalog.pg_proc AS procedure
                          JOIN pg_catalog.pg_language AS language
                            ON language.oid = procedure.prolang
                          WHERE procedure.oid =
                            pg_catalog.to_regprocedure(
                              'bm25_compact_step_if_current' ||
                              '(oid,oid,oid,oid,oid)');")"
    sql_as durable_owner -c \
        "SELECT df.cancel('${instance_id}', 'identity test complete');" \
        >/dev/null
    wait_for_terminal "${instance_id}" 30
    sql_super -c "DROP FUNCTION
                     bm25_compact_step_if_current_test_c(
                         oid, oid, oid, oid, oid);
                   DROP TABLE identity_docs, worker_identity_audit;
                   GRANT CREATE ON SCHEMA public TO durable_writer;
                   REVOKE durable_owner FROM durable_actor;"
}

test_scheduled_failure_continuation() {
    local index_oid instance_id failure_message

    index_oid="$(sql_super -c \
        "SELECT 'documents_idx'::regclass::oid;")"
    instance_id="$(active_job_id "${index_oid}")"
    [ -n "${instance_id}" ] ||
        error "failure-continuation test found no managed workflow"
    wait_for_signal_node "${instance_id}" 30

    assert_eq "scheduled loop continues after body failures" "t" \
        "$(sql_super -c "SELECT df.explain('${instance_id}')
                          LIKE '%LOOP (infinite, continue on failure)%';")"

    failure_message="pg_textsearch transient compaction test failure"
    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
LANGUAGE plpgsql
VOLATILE
STRICT
AS $body$
BEGIN
    RAISE EXCEPTION 'pg_textsearch transient compaction test failure';
END
$body$;
SQL

    create_compaction_debt
    wait_for_log_message "${failure_message}" 30
    assert_eq "failed scheduled iteration leaves workflow running" "running" \
        "$(sql_super -c "SELECT status FROM df.instances
                          WHERE id = '${instance_id}';")"
    assert_eq "failed scheduled iteration preserves compaction debt" "t" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'documents_idx'::regclass);")"
    wait_for_signal_node "${instance_id}" 30

    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS '$libdir/pg_textsearch', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;
SQL

    sql_as durable_owner -c \
        "SELECT df.signal('${instance_id}', 'compact', '{}');" >/dev/null
    wait_for_no_debt documents_idx 30
    assert_eq "same workflow recovers after the transient failure" \
        "${instance_id}" "$(active_job_id "${index_oid}")"
}

test_initial_failure_continuation() {
    local index_oid instance_id failure_message

    sql_super -c "CREATE TABLE initial_failure_docs
                      (id integer, body text);
                   ALTER TABLE initial_failure_docs OWNER TO durable_owner;"
    sql_as durable_owner -c "
        CREATE INDEX initial_failure_docs_idx
          ON initial_failure_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'manual');"
    sql_as durable_owner <<'SQL' >/dev/null
DO $body$
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO initial_failure_docs (id, body)
        SELECT n * 100 + i,
               format('initial failure round %s document %s', n, i)
        FROM generate_series(1, 20) AS i;
        PERFORM bm25_spill_index('initial_failure_docs_idx');
    END LOOP;
END
$body$;
SQL
    assert_eq "manual index has startup compaction debt" "t" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'initial_failure_docs_idx'::regclass);")"

    failure_message="pg_textsearch initial compaction test failure"
    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
LANGUAGE plpgsql
VOLATILE
STRICT
AS $body$
BEGIN
    RAISE EXCEPTION 'pg_textsearch initial compaction test failure';
END
$body$;
SQL

    sql_as durable_owner -c "
        ALTER INDEX initial_failure_docs_idx
          SET (compaction = 'background');" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'initial_failure_docs_idx'::regclass::oid;")"
    instance_id="$(latest_job_id "${index_oid}")"
    [ -n "${instance_id}" ] ||
        error "startup failure test found no managed workflow"
    wait_for_log_message "${failure_message}" 30
    assert_eq "failed startup cascade leaves workflow running" "running" \
        "$(sql_super -c "SELECT status FROM df.instances
                          WHERE id = '${instance_id}';")"

    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS '$libdir/pg_textsearch', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;
SQL

    wait_for_no_debt initial_failure_docs_idx 30
    assert_eq "same workflow recovers its startup cascade" \
        "${instance_id}" "$(active_job_id "${index_oid}")"
    sql_as durable_owner -c \
        "SELECT df.cancel('${instance_id}', 'startup test complete');" \
        >/dev/null
    wait_for_terminal "${instance_id}" 30
    sql_super -c "DROP TABLE initial_failure_docs;"
}

test_cross_owner_helper_isolation() {
    local current_error index_one_oid index_two_oid instance_one instance_two
    local debt_before level_counts_before step_error workflow_state_before
    local database_oid tablespace_oid relfilenumber owner_oid

    sql_super -c "CREATE TABLE isolation_one_docs (body text);
                   ALTER TABLE isolation_one_docs OWNER TO durable_owner;
                   CREATE TABLE isolation_two_docs (body text);
                   ALTER TABLE isolation_two_docs OWNER TO durable_owner_two;"
    sql_as durable_owner -c "
        CREATE INDEX isolation_one_idx
          ON public.isolation_one_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'manual',
                compaction_schedule = '0 0 1 1 *');
        ALTER INDEX public.isolation_one_idx
          SET (compaction = 'background');" >/dev/null 2>&1
    sql_as durable_owner_two -c "
        CREATE INDEX isolation_two_idx
          ON public.isolation_two_docs USING bm25(body)
          WITH (text_config = 'english', compaction = 'manual',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1

    index_one_oid="$(sql_super -c \
        "SELECT 'isolation_one_idx'::regclass::oid;")"
    instance_one="$(active_job_id_for_owner \
        "${index_one_oid}" durable_owner)"
    [ -n "${instance_one}" ] ||
        error "first isolation owner has no managed workflow"
    wait_for_signal_node "${instance_one}" 30

    sql_as durable_owner_two <<'SQL' >/dev/null
BEGIN;
DO $body$
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO isolation_two_docs (body)
        SELECT format(
            'second owner round %s document %s filler', n, i)
        FROM generate_series(1, 20) AS i;
        PERFORM bm25_spill_index('isolation_two_idx');
    END LOOP;
END
$body$;
COMMIT;
SQL
    assert_eq "manual second-owner target starts with compaction debt" "t" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'isolation_two_idx'::regclass);")"

    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
LANGUAGE sql
VOLATILE
STRICT
SET search_path = pg_catalog, pg_temp
AS $body$
    SELECT false
$body$;
SQL
    sql_as durable_owner_two -c "
        ALTER INDEX public.isolation_two_idx
          SET (compaction = 'background');" >/dev/null 2>&1
    index_two_oid="$(sql_super -c \
        "SELECT 'isolation_two_idx'::regclass::oid;")"
    instance_two="$(active_job_id_for_owner \
        "${index_two_oid}" durable_owner_two)"
    [ -n "${instance_two}" ] ||
        error "second isolation owner has no managed workflow"
    wait_for_signal_node "${instance_two}" 30
    sql_as durable_owner_two -c \
        "SELECT df.cancel('${instance_two}', 'hold isolation debt');" \
        >/dev/null
    wait_for_terminal "${instance_two}" 30

    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS '$libdir/pg_textsearch', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;
SQL
    assert_eq "cross-owner step helper is restored before rejection" \
        "c:tp_compact_index_step_if_current:true" \
        "$(sql_super -c "SELECT language.lanname || ':' ||
                                procedure.prosrc || ':' ||
                                (procedure.proconfig IS NULL)
                          FROM pg_catalog.pg_proc AS procedure
                          JOIN pg_catalog.pg_language AS language
                            ON language.oid = procedure.prolang
                          WHERE procedure.oid =
                            pg_catalog.to_regprocedure(
                              'bm25_compact_step_if_current' ||
                              '(oid,oid,oid,oid,oid)');")"
    assert_eq "both owners receive private helper EXECUTE" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.has_function_privilege(
                'durable_owner',
                'bm25_compact_step_if_current(oid,oid,oid,oid,oid)',
                'EXECUTE')
            AND pg_catalog.has_function_privilege(
                'durable_owner',
                'bm25_background_target_is_current(oid,oid,oid,oid,oid)',
                'EXECUTE')
            AND pg_catalog.has_function_privilege(
                'durable_owner_two',
                'bm25_compact_step_if_current(oid,oid,oid,oid,oid)',
                'EXECUTE')
            AND pg_catalog.has_function_privilege(
                'durable_owner_two',
                'bm25_background_target_is_current(oid,oid,oid,oid,oid)',
                'EXECUTE');")"

    IFS='|' read -r database_oid tablespace_oid relfilenumber owner_oid < <(
        sql_super -c "SELECT database.oid,
                             coalesce(nullif(relation.reltablespace, 0),
                                      database.dattablespace),
                             pg_catalog.pg_relation_filenode(relation.oid),
                             relation.relowner
                      FROM pg_catalog.pg_class AS relation
                      JOIN pg_catalog.pg_database AS database
                        ON database.datname = pg_catalog.current_database()
                      WHERE relation.oid =
                            'isolation_two_idx'::regclass;"
    )
    assert_eq "second owner's physical target remains current" "t" \
        "$(sql_as durable_owner_two -c "
            SELECT public.bm25_background_target_is_current(
                ${index_two_oid}, ${database_oid}, ${tablespace_oid},
                ${relfilenumber}, ${owner_oid});")"
    debt_before="$(sql_super -c "SELECT bm25_needs_compaction(
                                      'isolation_two_idx'::regclass);")"
    assert_eq "second owner's target has compaction debt" "t" \
        "${debt_before}"
    level_counts_before="$(sql_super -c \
        "SELECT bm25_level_counts('isolation_two_idx'::regclass);")"
    workflow_state_before="$(sql_super -c \
        "SELECT id || ':' || status FROM df.instances
          WHERE id = '${instance_two}';")"

    if step_error="$(sql_as durable_owner -c "
        SELECT public.bm25_compact_step_if_current(
            ${index_two_oid}, ${database_oid}, ${tablespace_oid},
            ${relfilenumber}, ${owner_oid});" 2>&1)"; then
        error "first owner compacted the second owner's captured target"
    fi
    if ! grep -Fq "must be owner" <<<"${step_error}"; then
        error "cross-owner compaction used an unexpected error: ${step_error}"
    fi
    assert_eq "rejected cross-owner step preserves level counts" \
        "${level_counts_before}" \
        "$(sql_super -c \
            "SELECT bm25_level_counts('isolation_two_idx'::regclass);")"
    assert_eq "rejected cross-owner step preserves compaction debt" \
        "${debt_before}" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'isolation_two_idx'::regclass);")"
    assert_eq "rejected cross-owner step preserves workflow state" \
        "${workflow_state_before}" \
        "$(sql_super -c "SELECT id || ':' || status FROM df.instances
                          WHERE id = '${instance_two}';")"

    if current_error="$(sql_as durable_owner -c "
        SELECT public.bm25_background_target_is_current(
            ${index_two_oid}, ${database_oid}, ${tablespace_oid},
            ${relfilenumber}, ${owner_oid});" 2>&1)"; then
        error "first owner inspected the second owner's captured target"
    fi
    if ! grep -Fq "must be owner" <<<"${current_error}"; then
        error "cross-owner current check used an unexpected error: \
${current_error}"
    fi
    assert_eq "rejected cross-owner current check preserves level counts" \
        "${level_counts_before}" \
        "$(sql_super -c \
            "SELECT bm25_level_counts('isolation_two_idx'::regclass);")"
    assert_eq "rejected cross-owner current check preserves debt" \
        "${debt_before}" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'isolation_two_idx'::regclass);")"
    assert_eq "rejected cross-owner current check preserves workflow state" \
        "${workflow_state_before}" \
        "$(sql_super -c "SELECT id || ':' || status FROM df.instances
                          WHERE id = '${instance_two}';")"

    sql_as durable_owner -c \
        "SELECT df.cancel('${instance_one}', 'isolation test complete');" \
        >/dev/null
    wait_for_terminal "${instance_one}" 30
    wait_for_terminal "${instance_two}" 30
    sql_super -c "DROP TABLE isolation_one_docs, isolation_two_docs;"
}

test_bypassrls_owner_isolation() {
    local index_oid label owner_instance foreign_instance replacement_instance

    sql_super -c "CREATE TABLE bypass_documents (id integer, body text);
                   ALTER TABLE bypass_documents OWNER TO durable_bypass;"
    sql_as durable_bypass -c "INSERT INTO bypass_documents
        SELECT i, 'bypass document ' || i || ' filler filler filler'
        FROM generate_series(1, 10000) AS i;"
    sql_as durable_bypass -c "
        CREATE INDEX CONCURRENTLY bypass_documents_idx
          ON bypass_documents USING bm25(body)
          WITH (text_config = 'english', compaction = 'background');" \
        >/dev/null 2>&1

    index_oid="$(sql_super -c \
        "SELECT 'bypass_documents_idx'::regclass::oid;")"
    owner_instance="$(active_job_id_for_owner \
        "${index_oid}" durable_bypass)"
    [ -n "${owner_instance}" ] ||
        error "BYPASSRLS index owner has no managed workflow"
    wait_for_signal_node "${owner_instance}" 30
    label="$(sql_super -c "SELECT label FROM df.instances
                            WHERE id = '${owner_instance}';")"

    foreign_instance="$(sql_as durable_owner -c "
        SELECT df.start(
            df.wait_for_signal('compact', 300),
            '${label}',
            current_database(),
            'caller');")"
    wait_for_signal_node "${foreign_instance}" 30
    assert_eq "lookalike workflow has a foreign submitter" \
        "durable_owner" \
        "$(sql_super -c "SELECT submitted_by::text FROM df.instances
                          WHERE id = '${foreign_instance}';")"

    sql_as durable_bypass -c \
        "SELECT df.signal('${owner_instance}', 'compact', '{}');" \
        >/dev/null
    wait_for_signal_node "${owner_instance}" 30
    assert_eq "foreign lookalike cannot retire the owner workflow" \
        "${owner_instance}" \
        "$(active_job_id_for_owner "${index_oid}" durable_bypass)"

    sql_as durable_bypass -c \
        "SELECT df.cancel('${owner_instance}', 'owner isolation recovery');" \
        >/dev/null
    wait_for_terminal "${owner_instance}" 30
    create_bypass_compaction_debt
    wait_for_no_debt bypass_documents_idx 30
    replacement_instance="$(active_job_id_for_owner \
        "${index_oid}" durable_bypass)"
    if [ -z "${replacement_instance}" ] ||
        [ "${replacement_instance}" = "${owner_instance}" ]; then
        error "owner workflow was not recovered past a foreign lookalike"
    fi
    assert_eq "foreign lookalike cannot be selected or signaled" "t" \
        "$(sql_super -c "SELECT status IN ('pending', 'running')
                          FROM df.instances
                          WHERE id = '${foreign_instance}';")"
    assert_eq "recovered workflow retains the index owner" \
        "durable_bypass" \
        "$(sql_super -c "SELECT submitted_by::text FROM df.instances
                          WHERE id = '${replacement_instance}';")"
}

test_superuser_policy_success() {
    local index_oid instance_id setting_before

    setting_before="$(sql_super -c \
        "SHOW pg_durable.enable_superuser_instances;")"
    assert_eq "superuser workflow policy starts disabled" "off" \
        "${setting_before}"
    sql_super -c "ALTER SYSTEM SET
                     pg_durable.enable_superuser_instances = on;" >/dev/null
    PGHOST="${SOCKET_DIR}" "${PGBINDIR}/pg_ctl" restart -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w >/dev/null
    wait_for_durable_worker
    assert_eq "superuser workflow policy is enabled" "on" \
        "$(sql_super -c \
            "SHOW pg_durable.enable_superuser_instances;")"
    sql_super -c "CREATE TABLE superuser_policy_docs
                      (id integer, body text);
                   CREATE INDEX superuser_policy_docs_idx
                     ON superuser_policy_docs USING bm25(body)
                     WITH (text_config = 'english', compaction = 'manual',
                           compaction_schedule = '0 0 1 1 *');
                   ALTER INDEX superuser_policy_docs_idx
                     SET (compaction = 'background');" >/dev/null 2>&1
    index_oid="$(sql_super -c \
        "SELECT 'superuser_policy_docs_idx'::regclass::oid;")"
    instance_id="$(active_job_id_for_owner "${index_oid}" postgres)"
    [ -n "${instance_id}" ] ||
        error "enabled superuser policy created no postgres workflow"
    wait_for_signal_node "${instance_id}" 30
    assert_eq "superuser workflow is submitted as postgres" "postgres" \
        "$(sql_super -c "SELECT submitted_by::text FROM df.instances
                          WHERE id = '${instance_id}';")"

    sql_super <<'SQL' >/dev/null
BEGIN;
DO $body$
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO superuser_policy_docs (id, body)
        SELECT n * 100 + i,
               format('superuser round %s document %s filler', n, i)
        FROM generate_series(1, 20) AS i;
        PERFORM bm25_spill_index('superuser_policy_docs_idx');
    END LOOP;
END
$body$;
COMMIT;
SQL
    wait_for_no_debt superuser_policy_docs_idx 30
    assert_eq "postgres workflow executes after a spill signal" \
        "${instance_id}" "$(active_job_id_for_owner "${index_oid}" postgres)"

    sql_super -c \
        "SELECT df.cancel('${instance_id}', 'superuser test complete');" \
        >/dev/null
    wait_for_terminal "${instance_id}" 30
    sql_super -c "DROP TABLE superuser_policy_docs;"
    sql_super -c "ALTER SYSTEM RESET
                     pg_durable.enable_superuser_instances;" >/dev/null
    PGHOST="${SOCKET_DIR}" "${PGBINDIR}/pg_ctl" restart -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w >/dev/null
    wait_for_durable_worker
    assert_eq "superuser workflow policy is restored" "${setting_before}" \
        "$(sql_super -c \
            "SHOW pg_durable.enable_superuser_instances;")"
}

test_rollback_in_fresh_database() {
    local alter_error dump_file dump_lineage nologin_error
    local partition_lineage restore_output restored_lineage

    dump_file="${DATA_DIR}/lineage-dump.sql"
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_dump_docs (body text);" >/dev/null
    sql_as durable_owner -c "
        CREATE INDEX lifecycle_dump_idx
          ON public.lifecycle_dump_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    sql_as durable_owner -c "
        CREATE TABLE public.lifecycle_dump_partitioned_docs
          (id integer, body text)
          PARTITION BY RANGE (id);
        CREATE TABLE public.lifecycle_dump_partitioned_low
          PARTITION OF public.lifecycle_dump_partitioned_docs
          FOR VALUES FROM (0) TO (100);
        CREATE TABLE public.lifecycle_dump_partitioned_high
          PARTITION OF public.lifecycle_dump_partitioned_docs
          FOR VALUES FROM (100) TO (200);
        CREATE INDEX lifecycle_dump_partitioned_idx
          ON public.lifecycle_dump_partitioned_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1
    dump_lineage="$(index_lineage public.lifecycle_dump_idx)"
    partition_lineage="$(
        index_lineage public.lifecycle_dump_partitioned_idx
    )"
    assert_eq "partitioned source parent has a 128-bit lineage" "32" \
        "${#partition_lineage}"
    assert_eq "every partitioned source leaf matches parent lineage" "2:2" \
        "$(sql_super -c "SELECT
            count(*) || ':' ||
            count(*) FILTER (
              WHERE pg_catalog.substr(
                option, pg_catalog.length('compaction_lineage=') + 1)
                    OPERATOR(pg_catalog.=) '${partition_lineage}')
          FROM pg_catalog.pg_partition_tree(
                 'public.lifecycle_dump_partitioned_idx'::regclass) AS tree
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = tree.relid
          CROSS JOIN LATERAL
            pg_catalog.unnest(relation.reloptions) AS option
          WHERE tree.isleaf
            AND relation.relkind = 'i'
            AND option OPERATOR(pg_catalog.~~)
                'compaction_lineage=%';")"
    "${PGBINDIR}/pg_dump" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_owner -d "${TEST_DB}" --schema-only --no-owner \
        --table=public.lifecycle_dump_docs \
        --table=public.lifecycle_dump_partitioned_docs \
        --table=public.lifecycle_dump_partitioned_low \
        --table=public.lifecycle_dump_partitioned_high >"${dump_file}"
    if ! grep -Fq "compaction_lineage" "${dump_file}"; then
        error "pg_dump omitted the managed lineage reloption"
    fi

    sql_super -c "CREATE DATABASE ${ROLLBACK_DB};"
    sql_super -c "ALTER SYSTEM SET pg_durable.database = '${ROLLBACK_DB}';"
    PGHOST="${SOCKET_DIR}" "${PGBINDIR}/pg_ctl" restart -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w >/dev/null
    TEST_DB="${ROLLBACK_DB}"

    initialize_database
    sql_super -c "GRANT CREATE ON SCHEMA public TO durable_nologin;
                   CREATE TABLE nologin_documents (id integer, body text);
                   ALTER TABLE nologin_documents OWNER TO durable_nologin;"
    if nologin_error="$(sql_super \
        -c "SET ROLE durable_nologin;" \
        -c "
        CREATE INDEX CONCURRENTLY nologin_documents_idx
          ON nologin_documents USING bm25(body)
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
    assert_eq "rejected NOLOGIN CIC leaves no relation" "t" \
        "$(sql_super -c "SELECT
            pg_catalog.to_regclass('nologin_documents_idx') IS NULL;")"
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

    sql_super -c "CREATE TABLE alter_rollback_documents (
                       id integer, body text);
                   ALTER TABLE alter_rollback_documents
                     OWNER TO durable_owner;"
    sql_as durable_owner -c "
        CREATE INDEX alter_rollback_documents_idx
          ON alter_rollback_documents USING bm25(body)
          WITH (text_config = 'english', compaction = 'manual');" >/dev/null
    if alter_error="$(sql_as durable_owner <<'SQL' 2>&1
BEGIN;
ALTER INDEX alter_rollback_documents_idx
  SET (compaction = 'background');
DO $body$
BEGIN
    RAISE EXCEPTION 'force background ALTER rollback';
END
$body$;
COMMIT;
SQL
    )"; then
        error "background ALTER rollback transaction unexpectedly committed"
    fi
    if ! grep -Fq "force background ALTER rollback" \
        <<<"${alter_error}"; then
        error "background ALTER rollback did not reach forced failure: \
${alter_error}"
    fi
    assert_eq "rolled-back ALTER preserves manual reloption" "t" \
        "$(sql_super -c "SELECT reloptions @> ARRAY['compaction=manual']
                          FROM pg_catalog.pg_class
                          WHERE oid =
                            'alter_rollback_documents_idx'::regclass;")"
    assert_eq "rolled-back ALTER leaves no managed job" "0" \
        "$(managed_job_count)"
    assert_eq "rolled-back ALTER leaves no sticky dependency" "0" \
        "$(dependency_count)"
    assert_eq "rolled-back ALTER leaves no private-helper grant" "f" \
        "$(sql_super -c "SELECT
            pg_catalog.has_function_privilege(
                'durable_owner',
                'bm25_compact_step_if_current(oid,oid,oid,oid,oid)',
                'EXECUTE')
            OR pg_catalog.has_function_privilege(
                'durable_owner',
                'bm25_background_target_is_current(oid,oid,oid,oid,oid)',
                'EXECUTE');")"

    if ! restore_output="$(sql_as durable_owner -f "${dump_file}" 2>&1)"; then
        error "normal pg_dump restore rejected a fresh-database lineage: \
${restore_output}"
    fi
    restored_lineage="$(index_lineage public.lifecycle_dump_idx)"
    assert_eq "pg_dump restore preserves a fresh-database lineage" \
        "${dump_lineage}" "${restored_lineage}"
    assert_eq "partitioned pg_dump restore preserves shared lineage" \
        "${partition_lineage}" \
        "$(index_lineage public.lifecycle_dump_partitioned_idx)"
    restored_lineage="$(
        index_lineage public.lifecycle_dump_partitioned_idx
    )"
    assert_eq "partitioned restored parent has a 128-bit lineage" "32" \
        "${#restored_lineage}"
    assert_eq "every restored physical leaf matches parent lineage" "2:2" \
        "$(sql_super -c "SELECT
            count(*) || ':' ||
            count(*) FILTER (
              WHERE pg_catalog.substr(
                option, pg_catalog.length('compaction_lineage=') + 1)
                    OPERATOR(pg_catalog.=) '${restored_lineage}')
          FROM pg_catalog.pg_partition_tree(
                 'public.lifecycle_dump_partitioned_idx'::regclass) AS tree
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = tree.relid
          CROSS JOIN LATERAL
            pg_catalog.unnest(relation.reloptions) AS option
          WHERE tree.isleaf
            AND relation.relkind = 'i'
            AND option OPERATOR(pg_catalog.~~)
                'compaction_lineage=%';")"
    assert_eq "partitioned restore activates every physical leaf" "2" \
        "$(sql_super -c "SELECT count(*)
          FROM pg_catalog.pg_inherits AS inheritance
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = inheritance.inhrelid
          WHERE inheritance.inhparent =
                'public.lifecycle_dump_partitioned_idx'::regclass
            AND relation.relkind = 'i'
            AND EXISTS (
              SELECT 1 FROM df.instances AS instance
              WHERE instance.label OPERATOR(pg_catalog.~~)
                    ('pg_textsearch:bg:v1:%:' ||
                     relation.oid::pg_catalog.text || ':%')
                AND instance.status OPERATOR(pg_catalog.=)
                    ANY (ARRAY['pending', 'running']::pg_catalog.text[]));")"
    sql_as durable_owner -c \
        "DROP TABLE public.lifecycle_dump_docs,
                    public.lifecycle_dump_partitioned_docs;" >/dev/null
}

test_sticky_dependency() {
    quiesce_durable_worker

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

run_test() {
    local test_name=$1
    local selected=",${DURABLE_TEST_FILTER:-},"

    if [ -z "${DURABLE_TEST_FILTER:-}" ] ||
        [[ "${selected}" == *",${test_name},"* ]]; then
        "${test_name}"
    fi
}

stage_durable_package
setup_cluster
run_test test_missing_durable_cic
initialize_database
run_test test_cic_preflight_rejections
run_test test_cic_owner_privilege_preflight
run_test test_alter_preflight_rejections
run_test test_defaulted_start_arity
run_test test_reindex_nonrelation_passthrough
run_test test_late_bulk_reindex_target
run_test test_bulk_reindex_concurrent_mode_change
run_test test_bulk_reindex_concurrent_schedule_change
run_test test_failed_bulk_reindex_reconciliation
run_test test_partitioned_create_activation
run_test test_direct_index_partition_attach
run_test test_reused_intermediate_partition_options
run_test test_partition_detach_lineage
run_test test_partitioned_existing_leaf_reconciliation
run_test test_create_tracking_reentry
run_test test_cached_create_uses_fresh_lineage
run_test test_create_like_regenerates_lineage
run_test test_create_tracking_concurrent
run_test test_create_tracking_table_rename
run_test test_owner_reconciliation
run_test test_owner_change_preserves_captured_schedule
run_test test_reassign_owned_reconciliation
run_test test_reindex_reconciliation
run_test test_plain_reindex_preserves_captured_schedule
run_test test_physical_rewrite_reconciliation
run_test test_database_owner_vacuum_full
run_test test_vacuum_full_skip_locked
run_test test_partition_vacuum_child_authorization
run_test test_global_cluster_scope
run_test test_global_cluster_concurrent_mode_change
run_test test_inheritance_vacuum_full_scope
run_test test_inheritance_alter_rewrite_scope
run_test test_additional_rewrite_reconciliation
run_test test_truncate_cascade_reconciliation
run_test test_maintenance_privilege_compatibility
run_test test_tablespace_move_without_owned_by
run_test test_rewrite_preflight_ordering
run_test test_concurrent_reindex_reconciliation
run_test test_partitioned_reindex_reconciliation
run_test test_partitioned_reindex_failure_reconciliation
run_test test_partitioned_reindex_rename_reconciliation
run_test test_reindex_tracking_reentry
run_test test_ordinary_inheritance_reindex_scope
run_test test_legacy_lineage_backfill
run_test test_legacy_reconciliation_edges
run_test test_refresh_selects_requested_workflow
run_test test_concurrent_legacy_lineage_backfill
run_test test_legacy_reindex_spill_lock_order
run_test test_managed_lock_order
run_test test_alter_reindex_lock_order
run_test test_cross_statement_reindex_lock_order
run_test test_cross_statement_owner_lock_order
run_test test_multi_family_partition_attach_batch
run_test test_attached_index_rewrite_preserves_parent_options
run_test test_lineage_lookup_drop_durable_order
run_test test_textsearch_extension_dependency_order
run_test test_precommit_request_admission_nowait
run_test test_terminal_grant_reentry_is_rejected
run_test test_post_publication_reindex_defers
run_test test_post_publication_schedule_override
run_test test_post_publication_background_activation
run_test test_post_publication_activation_error_defers
run_test test_cross_statement_managed_lock_order
run_test test_managed_intent_savepoint_recovery
run_test test_internal_lock_namespace
run_test test_lineage_ddl_guards
run_test test_partitioned_lineage_history
run_test test_forged_lineage_submitter
run_test test_concurrent_supplied_lineage_create
run_test test_reindex_lineage_replacement_isolation
run_test test_reindex_authorization_ordering
run_test test_create_authorization_ordering
run_test test_lineage_guard_name_race
run_test test_reindex_authorization_resolution_race
run_test test_prior_generation_spill_adoption
run_test test_request_queue_runtime
run_test test_precommit_signal_observes_published_spill
run_test test_repeatable_read_admission_snapshot
run_test test_repeatable_read_direct_activation_snapshot
run_test test_owner_repair_without_old_durable_acl
run_test test_writer_search_path_isolation
run_test test_persisted_helper_oid_guard
run_test test_reassign_owned_heap_lock_order
run_test test_actor_writer_worker_identity
run_test test_create_activation
run_test test_scheduled_failure_continuation
run_test test_initial_failure_continuation
run_test test_cross_owner_helper_isolation
run_test test_bypassrls_owner_isolation
run_test test_superuser_policy_success
run_test test_rollback_in_fresh_database
run_test test_sticky_dependency
log "Managed pg_durable compaction tests passed"
