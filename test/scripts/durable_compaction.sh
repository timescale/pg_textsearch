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
                      durable_actor, durable_writer;"

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
                      durable_writer;"

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
                      durable_writer;"
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
                compaction_schedule = '0 0 1 1 *');
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

test_concurrent_reindex_reconciliation() {
    local a_before a_after a_oid_before a_oid_after
    local b_before b_after b_oid_before b_oid_after

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
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
        CREATE INDEX lifecycle_concurrent_b_idx
          ON public.lifecycle_concurrent_docs USING bm25(body_b)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');" >/dev/null 2>&1

    a_oid_before="$(sql_super -c \
        "SELECT 'public.lifecycle_concurrent_a_idx'::regclass::oid;")"
    a_before="$(current_generation_job_id "${a_oid_before}")"
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
    local owner_lineage reindex_lineage reindex_oid_before reindex_oid_after

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
    assert_eq "legacy signal retires the unscoped workflow" "cancelled" \
        "$(sql_super -c "SELECT status FROM df.instances
                          WHERE id = '${legacy_instance}';")"
    assert_eq "legacy signal leaves one active physical workflow" "1" \
        "$(current_generation_job_count "${legacy_oid}")"

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
    assert_eq "concurrent legacy writers leave one managed workflow" "1" \
        "$(current_generation_job_count "${index_oid}")"

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

test_lineage_ddl_guards() {
    local duplicate_error lineage replay_error reset_error set_error

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
                AND wait_event = 'advisory';")" = "2" ]; then
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
            AND wait_event = 'advisory';")"

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
    elif ! grep -Fq "background compaction lineage is already in use" \
        "${first_output}"; then
        error "first concurrent CREATE failed unexpectedly: \
$(cat "${first_output}")"
    fi
    if [ "${second_status}" -eq 0 ]; then
        successes=$((successes + 1))
    elif ! grep -Fq "background compaction lineage is already in use" \
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
    local partition_error partition_jobs_before partition_parent_oid

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
    if index_error="$(sql_as durable_writer -c "
        SET statement_timeout = '1s';
        REINDEX INDEX CONCURRENTLY
          public.lifecycle_auth_idx;" 2>&1)"; then
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
                             public.lifecycle_auth_partitioned_docs;"
}

test_reindex_authorization_resolution_race() {
    local gate_pid lock_output locker_pid rename_output rename_pid
    local reindex_error reindex_pid

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
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U durable_writer -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 \
        -c "SET statement_timeout = '5s';
            REINDEX INDEX CONCURRENTLY
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
    assert_eq "authorization race reaches the original heap lock" "1" \
        "$(sql_super -c "SELECT pg_catalog.count(*)
          FROM pg_catalog.pg_stat_activity
          WHERE application_name = 'lifecycle-auth-race-reindex'
            AND wait_event_type = 'Lock';")"

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
      WHERE control.instance_id OPERATOR(pg_catalog.=) $1;

    IF injected_fault OPERATOR(pg_catalog.=) 'error' THEN
        RAISE EXCEPTION 'probe ordinary signal failure';
    ELSIF injected_fault OPERATOR(pg_catalog.=) 'cancel' THEN
        RAISE EXCEPTION 'probe query cancellation'
            USING ERRCODE = '57014';
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
    local dedup_b_instance drop_instance failure_a_instance
    local failure_b_instance failure_output savepoint_instance

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

    restore_signal_probe
    sql_super -c "DROP TABLE public.queue_below_docs,
                              public.queue_abort_docs,
                              public.queue_dedup_a_docs,
                              public.queue_dedup_b_docs,
                              public.queue_savepoint_docs,
                              public.queue_drop_docs,
                              public.queue_failure_a_docs,
                              public.queue_failure_b_docs,
                              public.queue_cancel_docs;"
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
        CREATE TABLE public.lifecycle_dump_docs (body text);
        CREATE INDEX lifecycle_dump_idx
          ON public.lifecycle_dump_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background',
                compaction_schedule = '0 0 1 1 *');
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
    assert_eq "partitioned source indexes share one lineage" "1" \
        "$(sql_super -c "SELECT count(DISTINCT pg_catalog.substr(
            option, pg_catalog.length('compaction_lineage=') + 1))
          FROM pg_catalog.pg_inherits AS inheritance
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = inheritance.inhrelid
          CROSS JOIN LATERAL
            pg_catalog.unnest(relation.reloptions) AS option
          WHERE inheritance.inhparent =
                'public.lifecycle_dump_partitioned_idx'::regclass
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
    assert_eq "partitioned restore keeps one lineage across leaf heaps" "1" \
        "$(sql_super -c "SELECT count(DISTINCT pg_catalog.substr(
            option, pg_catalog.length('compaction_lineage=') + 1))
          FROM pg_catalog.pg_inherits AS inheritance
          JOIN pg_catalog.pg_class AS relation
            ON relation.oid = inheritance.inhrelid
          CROSS JOIN LATERAL
            pg_catalog.unnest(relation.reloptions) AS option
          WHERE inheritance.inhparent =
                'public.lifecycle_dump_partitioned_idx'::regclass
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
run_test test_partitioned_create_activation
run_test test_owner_reconciliation
run_test test_reindex_reconciliation
run_test test_concurrent_reindex_reconciliation
run_test test_partitioned_reindex_reconciliation
run_test test_partitioned_reindex_failure_reconciliation
run_test test_partitioned_reindex_rename_reconciliation
run_test test_reindex_tracking_reentry
run_test test_ordinary_inheritance_reindex_scope
run_test test_legacy_lineage_backfill
run_test test_concurrent_legacy_lineage_backfill
run_test test_lineage_ddl_guards
run_test test_concurrent_supplied_lineage_create
run_test test_reindex_lineage_replacement_isolation
run_test test_reindex_authorization_ordering
run_test test_reindex_authorization_resolution_race
run_test test_prior_generation_spill_adoption
run_test test_request_queue_runtime
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
