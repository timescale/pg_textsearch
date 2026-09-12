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

assert_eq() {
    local description=$1 expected=$2 actual=$3

    if [ "${actual}" != "${expected}" ]; then
        error "${description}: expected '${expected}', got '${actual}'"
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
        "index owner lacks required pg_durable privileges"
}

test_cic_owner_privilege_preflight() {
    local create_error jobs_before schema_error

    schema_error='Role "durable_no_textsearch_schema" lacks USAGE privilege'
    schema_error+=' on schema public.'

    sql_super -c "REVOKE CONNECT ON DATABASE durable_compaction_test
                   FROM PUBLIC;
                   GRANT CONNECT ON DATABASE durable_compaction_test
                   TO postgres, durable_owner, durable_usage_only,
                      durable_read_only, durable_bypass, durable_actor,
                      durable_writer;"

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
                   TO postgres, durable_owner, durable_nologin,
                      durable_usage_only, durable_read_only, durable_bypass,
                      durable_no_connect, durable_actor, durable_writer;"

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
    if ! grep -Fq "index owner lacks required pg_durable privileges" \
        <<<"${create_error}"; then
        error "no-schema-USAGE owner did not fail stable privilege check: \
${create_error}"
    fi
    if ! grep -Fq "${schema_error}" <<<"${create_error}"; then
        error "no-schema-USAGE owner error did not identify public: \
${create_error}"
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
                   TO durable_owner, durable_usage_only, durable_read_only,
                      durable_bypass, durable_actor, durable_writer;"
    sql_super -c "SELECT df.grant_usage('durable_owner');" >/dev/null
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

managed_job_count() {
    sql_super -c "SELECT count(*) FROM df.instances
      WHERE label LIKE 'pg_textsearch:bg:v1:%';"
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
    sql_super -c "CREATE ROLE durable_nologin NOLOGIN;"
    sql_super -c "CREATE ROLE durable_usage_only LOGIN;"
    sql_super -c "CREATE ROLE durable_read_only LOGIN;"
    sql_super -c "CREATE ROLE durable_bypass LOGIN BYPASSRLS;"
    sql_super -c "CREATE ROLE durable_no_connect LOGIN;"
    sql_super -c "CREATE ROLE durable_no_textsearch_schema LOGIN;"
    sql_super -c "CREATE ROLE durable_actor LOGIN;"
    sql_super -c "CREATE ROLE durable_writer LOGIN;"
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

test_actor_writer_worker_identity() {
    local audit_rows_before index_oid instance_id memtable_threshold_before

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
                   CREATE TABLE worker_identity_audit (role_name name);
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
    assert_eq "initial owner-managed cascade preserves writer debt" "t" \
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
    audit_rows_before="$(sql_super -c \
        "SELECT count(*) FROM worker_identity_audit;")"
    sql_as durable_owner -c \
        "SELECT df.signal('${instance_id}', 'compact', '{}');" >/dev/null
    wait_for_audit_rows "$((audit_rows_before + 1))" 30
    wait_for_no_debt identity_docs_idx 60
    assert_eq "owner signal consumes writer-created debt" "f" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
                              'identity_docs_idx'::regclass);")"
    assert_eq "every worker step executes as the index owner" "t" \
        "$(sql_super -c "SELECT count(*) >= 2
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

    sql_super -c "CREATE ROLE durable_owner_two LOGIN;
                   GRANT CONNECT ON DATABASE ${TEST_DB}
                     TO durable_owner_two;
                   GRANT USAGE, CREATE ON SCHEMA public
                     TO durable_owner_two;"
    sql_super -c "SELECT df.grant_usage('durable_owner_two');" >/dev/null
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

stage_durable_package
setup_cluster
test_missing_durable_cic
initialize_database
test_cic_preflight_rejections
test_cic_owner_privilege_preflight
test_alter_preflight_rejections
test_defaulted_start_arity
test_actor_writer_worker_identity
test_create_activation
test_scheduled_failure_continuation
test_initial_failure_continuation
test_cross_owner_helper_isolation
test_bypassrls_owner_isolation
test_superuser_policy_success
test_sticky_dependency
test_rollback_in_fresh_database
log "Managed pg_durable compaction tests passed"
