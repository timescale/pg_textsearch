#!/bin/bash
#
# Focused structural and live test for the per-index pg_durable adapter.
# The live portion requires pg_durable's df.start() failure policy and is
# intentionally separate from test-all.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GLUE_DIR="${ROOT_DIR}/scripts/durable_compaction"
DATA_DIR="${ROOT_DIR}/test/tmp_durable_compaction"
SOCKET_DIR="${ROOT_DIR}"
TEST_PORT=55447
TEST_DB=durable_compaction_test
EXT_SCHEMA=textsearch_ext
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
PKGLIBDIR="$("${PG_CONFIG}" --pkglibdir)"

if [ "${DURABLE_PG_CONFIG_PROBE:-0}" = "1" ]; then
    printf '%s|%s\n' "${PGBINDIR}" "${PKGLIBDIR}"
    exit 0
fi

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

assert_contains() {
    local file="$1"
    local text="$2"

    grep -Fq -- "${text}" "${file}" \
        || fail "${file} does not contain: ${text}"
}

assert_not_contains() {
    local file="$1"
    local text="$2"

    if grep -Fq -- "${text}" "${file}"; then
        fail "${file} unexpectedly contains: ${text}"
    fi
}

assert_sql_true() {
    local description="$1"
    local query="$2"
    local result

    result=$(psql_super -c "${query}")
    [ "${result}" = "t" ] \
        || fail "${description} (expected t, got ${result})"
}

wait_for_instance() {
    local instance_id="$1"
    local waited=0
    local state

    while [ "${waited}" -lt 60 ]; do
        state=$(psql_super -c "SELECT status
            FROM df.instances WHERE id = '${instance_id}';")
        if [ "${state}" = "completed" ] || [ "${state}" = "failed" ] \
            || [ "${state}" = "cancelled" ]; then
            printf '%s\n' "${state}"
            return
        fi
        sleep 1
        waited=$((waited + 1))
    done

    fail "durable instance ${instance_id} did not finish"
}

static_checks() {
    local fresh_sql="${ROOT_DIR}/sql/pg_textsearch--1.5.0-dev.sql"
    local upgrade_sql="${ROOT_DIR}/sql/pg_textsearch--1.4.0--1.5.0-dev.sql"
    local wrapper="${GLUE_DIR}/02_wrapper.sql"

    [ ! -e "${GLUE_DIR}/03_backstop.sql" ] \
        || fail "03_backstop.sql is outside this PR"

    local compaction_api="${ROOT_DIR}/src/access/compaction_api.c"

    assert_contains "${compaction_api}" "tp_open_current_bm25_index"
    assert_contains "${compaction_api}" "tp_compact_index_step_if_current"
    assert_contains "${compaction_api}" "tp_needs_compaction_if_current"
    assert_contains "${compaction_api}" "!index_rel->rd_index->indisvalid"

    for sql in "${fresh_sql}" "${upgrade_sql}"; do
        assert_contains "${sql}" \
            "bm25_compact_step_if_current(oid, oid, oid, oid)"
        assert_contains "${sql}" \
            "bm25_needs_compaction_if_current(oid, oid, oid, oid)"
        assert_contains "${sql}" "REVOKE ALL ON FUNCTION"
        assert_contains "${sql}" "pg_catalog.aclexplode"
    done

    assert_contains "${GLUE_DIR}/01_setup_role.sql" \
        "pg_catalog.pg_shdepend"
    assert_contains "${GLUE_DIR}/01_setup_role.sql" \
        "WITH INHERIT TRUE, SET FALSE"
    assert_contains "${wrapper}" "SECURITY DEFINER"
    assert_contains "${wrapper}" \
        "SET search_path = pg_catalog, pg_temp"
    assert_contains "${wrapper}" "df.loop(body, cond)"
    assert_not_contains "${wrapper}" \
        "df.loop(body, cond, on_error => 'continue')"
    assert_contains "${wrapper}" "max_attempts     => 5"
    assert_contains "${wrapper}" "on_failure      => 'continue'"
    assert_contains "${wrapper}" "transaction_mode => 'new'"
    assert_contains "${wrapper}" "pg_textsearch_compaction.canary_instance"
    assert_contains "${wrapper}" \
        "IF NOT FOUND OR relfilenumber IS NULL THEN"
    assert_contains "${GLUE_DIR}/README.md" \
        "public.bm25_request_compaction(regclass)"

    printf 'Static pg_durable adapter checks passed.\n'
}

static_checks

if [ "${DURABLE_STATIC_ONLY:-0}" = "1" ]; then
    exit 0
fi

for library in pg_durable pg_textsearch; do
    [ -f "${PKGLIBDIR}/${library}.so" ] \
        || fail "${library}.so not found in ${PKGLIBDIR}"
done

psql_as() {
    local role="$1"
    shift
    "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U "${role}" -d "${TEST_DB}" -qAt -v ON_ERROR_STOP=1 "$@"
}

psql_super() {
    psql_as postgres "$@"
}

cleanup() {
    local status=$?

    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate \
            >/dev/null 2>&1 || true
    fi
    rm -rf "${DATA_DIR}"
    exit "${status}"
}

trap cleanup EXIT INT TERM

rm -rf "${DATA_DIR}"
mkdir -p "${DATA_DIR}"
"${PGBINDIR}/initdb" -D "${DATA_DIR}" -U postgres \
    --auth-local=trust --auth-host=reject >/dev/null
{
    printf "port = %s\n" "${TEST_PORT}"
    printf "unix_socket_directories = '%s'\n" "${SOCKET_DIR}"
    printf "listen_addresses = ''\n"
    printf "shared_preload_libraries = 'pg_durable,pg_textsearch'\n"
    printf "max_prepared_transactions = 10\n"
    printf "pg_durable.database = '%s'\n" "${TEST_DB}"
    printf "pg_durable.worker_role = 'postgres'\n"
} >>"${DATA_DIR}/postgresql.conf"

PGHOST="${SOCKET_DIR}" "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" \
    -l "${DATA_DIR}/postgres.log" -w >/dev/null
"${PGBINDIR}/createdb" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
    -U postgres "${TEST_DB}"

psql_super -c "CREATE EXTENSION pg_durable;"
psql_super -c "CREATE SCHEMA ${EXT_SCHEMA};"
psql_super -c "GRANT USAGE ON SCHEMA ${EXT_SCHEMA} TO PUBLIC;"
psql_super -c "CREATE ROLE default_privilege_writer LOGIN;"
psql_super -c "ALTER DEFAULT PRIVILEGES FOR ROLE postgres
    IN SCHEMA ${EXT_SCHEMA}
    GRANT EXECUTE ON FUNCTIONS TO default_privilege_writer;"
psql_super -c "CREATE EXTENSION pg_textsearch WITH SCHEMA ${EXT_SCHEMA};"

failure_policy_available=$(psql_super -c "SELECT pg_catalog.to_regprocedure(
    'df.start(pg_catalog.text,pg_catalog.text,pg_catalog.text,'
    'pg_catalog.text,pg_catalog.int4,pg_catalog.interval,pg_catalog.text)')
    IS NOT NULL;")
if [ "${failure_policy_available}" != "t" ]; then
    if psql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=missing_owner >/dev/null 2>&1; then
        fail "role setup accepted pg_durable without start failure policy"
    fi
    assert_sql_true "failed role preflight created no compactor role" \
        "SELECT NOT EXISTS (
            SELECT 1 FROM pg_catalog.pg_roles
            WHERE rolname = 'textsearch_compactor');"

    psql_super <<'SQL'
CREATE FUNCTION public.bm25_request_compaction(pg_catalog.regclass)
RETURNS pg_catalog.text
LANGUAGE sql
RETURN 'preflight-sentinel'::pg_catalog.text;
SQL
    preflight_oid=$(psql_super -c "SELECT
        'public.bm25_request_compaction(regclass)'::regprocedure::oid;")
    if psql_super -f "${GLUE_DIR}/02_wrapper.sql" \
        -v writer_role=missing_writer >/dev/null 2>&1; then
        fail "wrapper setup accepted pg_durable without start failure policy"
    fi
    [ "${preflight_oid}" = "$(psql_super -c "SELECT
        'public.bm25_request_compaction(regclass)'::regprocedure::oid;")" ] \
        || fail "failed wrapper preflight replaced the existing wrapper"
    printf '%s\n' \
        "Live positive checks skipped: df.start() failure policy unavailable."
    exit 0
fi

psql_super -c "CREATE ROLE app_owner LOGIN;"
psql_super -c "CREATE ROLE app_writer LOGIN;"
psql_super -c "ALTER DEFAULT PRIVILEGES FOR ROLE postgres
    IN SCHEMA public
    GRANT EXECUTE ON FUNCTIONS TO default_privilege_writer;"
psql_super -f "${GLUE_DIR}/01_setup_role.sql" \
    -v index_owner=app_owner >/dev/null

psql_super -c "ALTER ROLE textsearch_compactor PASSWORD 'hostile';"
if psql_super -f "${GLUE_DIR}/01_setup_role.sql" \
    -v index_owner=app_owner >/dev/null 2>&1; then
    fail "role setup accepted credentials on an existing role"
fi
assert_sql_true "failed role reuse did not mutate owner membership" \
    "SELECT NOT pg_catalog.pg_has_role(
        'textsearch_compactor', 'app_owner', 'SET');"
psql_super -c "ALTER ROLE textsearch_compactor PASSWORD NULL;"

psql_super <<'SQL'
CREATE FUNCTION public.bm25_request_compaction(pg_catalog.regclass)
RETURNS pg_catalog.text
LANGUAGE sql
SECURITY DEFINER
RETURN 'hostile-wrapper'::pg_catalog.text;
ALTER FUNCTION public.bm25_request_compaction(pg_catalog.regclass)
    OWNER TO textsearch_compactor;
SQL

hostile_oid=$(psql_super -c "SELECT
    'public.bm25_request_compaction(regclass)'::regprocedure::oid;")
if psql_super -f "${GLUE_DIR}/01_setup_role.sql" \
    -v index_owner=app_owner >/dev/null 2>&1; then
    fail "role setup accepted a hostile managed wrapper"
fi
[ "${hostile_oid}" = "$(psql_super -c "SELECT
    'public.bm25_request_compaction(regclass)'::regprocedure::oid;")" ] \
    || fail "failed wrapper reuse replaced hostile state"
psql_super -c "DROP FUNCTION
    public.bm25_request_compaction(pg_catalog.regclass);"

psql_super -f "${GLUE_DIR}/02_wrapper.sql" \
    -v writer_role=app_writer >/dev/null
psql_super -f "${GLUE_DIR}/01_setup_role.sql" \
    -v index_owner=app_owner >/dev/null

psql_super -c "CREATE ROLE delegable_writer LOGIN;"
psql_super -c "GRANT EXECUTE ON FUNCTION
    public.bm25_request_compaction(regclass)
    TO delegable_writer WITH GRANT OPTION;"
managed_oid=$(psql_super -c "SELECT
    'public.bm25_request_compaction(regclass)'::regprocedure::oid;")
if psql_super -f "${GLUE_DIR}/01_setup_role.sql" \
    -v index_owner=app_owner >/dev/null 2>&1; then
    fail "role setup accepted a delegable wrapper ACL"
fi
[ "${managed_oid}" = "$(psql_super -c "SELECT
    'public.bm25_request_compaction(regclass)'::regprocedure::oid;")" ] \
    || fail "failed ACL validation replaced the managed wrapper"
psql_super -c "REVOKE ALL ON FUNCTION
    public.bm25_request_compaction(regclass) FROM delegable_writer;"
psql_super -c "DROP ROLE delegable_writer;"

assert_sql_true "compactor role has the exact safe attributes" \
    "SELECT rolcanlogin AND rolinherit AND NOT rolsuper
            AND NOT rolcreatedb AND NOT rolcreaterole
            AND NOT rolreplication AND NOT rolbypassrls
            AND rolconnlimit = -1
     FROM pg_catalog.pg_roles
     WHERE rolname = 'textsearch_compactor';"
assert_sql_true "owner membership inherits but cannot SET ROLE" \
    "SELECT member.inherit_option AND NOT member.set_option
            AND NOT member.admin_option
     FROM pg_catalog.pg_auth_members member
     WHERE member.member = 'textsearch_compactor'::regrole
       AND member.roleid = 'app_owner'::regrole;"
assert_sql_true "wrapper has the intended security envelope" \
    "SELECT procedure.prosecdef
            AND procedure.proconfig =
                ARRAY['search_path=pg_catalog, pg_temp']::text[]
            AND procedure.proowner =
                'textsearch_compactor'::regrole
     FROM pg_catalog.pg_proc procedure
     WHERE procedure.oid =
        'public.bm25_request_compaction(regclass)'::regprocedure;"
assert_sql_true "wrapper ACL is owner plus non-grantable writer" \
    "SELECT NOT EXISTS (
                SELECT 1
                FROM pg_catalog.aclexplode(procedure.proacl) acl
                WHERE acl.grantee = 0 OR acl.is_grantable)
            AND pg_catalog.has_function_privilege(
                'app_writer',
                'public.bm25_request_compaction(regclass)',
                'EXECUTE')
     FROM pg_catalog.pg_proc procedure
     WHERE procedure.oid =
        'public.bm25_request_compaction(regclass)'::regprocedure;"
assert_sql_true "private helpers are available only to the compactor" \
    "SELECT
        NOT pg_catalog.has_function_privilege(
            'app_writer',
            '${EXT_SCHEMA}.bm25_compact_step_if_current(oid,oid,oid,oid)',
            'EXECUTE')
        AND pg_catalog.has_function_privilege(
            'textsearch_compactor',
            '${EXT_SCHEMA}.bm25_compact_step_if_current(oid,oid,oid,oid)',
            'EXECUTE')
        AND NOT pg_catalog.has_function_privilege(
            'app_writer',
            '${EXT_SCHEMA}.bm25_needs_compaction_if_current(oid,oid,oid,oid)',
            'EXECUTE')
        AND pg_catalog.has_function_privilege(
            'textsearch_compactor',
            '${EXT_SCHEMA}.bm25_needs_compaction_if_current(oid,oid,oid,oid)',
            'EXECUTE');"
assert_sql_true "named default helper grants were removed" \
    "SELECT
        NOT pg_catalog.has_function_privilege(
            'default_privilege_writer',
            '${EXT_SCHEMA}.bm25_compact_step_if_current(oid,oid,oid,oid)',
            'EXECUTE')
        AND NOT pg_catalog.has_function_privilege(
            'default_privilege_writer',
            '${EXT_SCHEMA}.bm25_needs_compaction_if_current(oid,oid,oid,oid)',
            'EXECUTE')
        AND NOT pg_catalog.has_function_privilege(
            'default_privilege_writer',
            'public.bm25_request_compaction(regclass)',
            'EXECUTE');"

psql_super <<SQL
CREATE TABLE adapter_docs (id integer PRIMARY KEY, body text);
CREATE INDEX adapter_docs_idx ON adapter_docs
    USING bm25(body) WITH (text_config = 'english');
ALTER TABLE adapter_docs OWNER TO app_owner;
ALTER INDEX adapter_docs_idx OWNER TO app_owner;
GRANT INSERT ON adapter_docs TO app_writer;
SQL

instance_id=$(psql_as app_writer <<'SQL'
BEGIN;
SELECT public.bm25_request_compaction('adapter_docs_idx');
ROLLBACK;
SQL
)
[ -n "${instance_id}" ] || fail "wrapper returned no durable instance id"
assert_sql_true "new-transaction request survived caller rollback" \
    "SELECT EXISTS (
        SELECT 1 FROM df.instances WHERE id = '${instance_id}');"
assert_sql_true "durable graph contains current-identity helpers" \
    "SELECT pg_catalog.count(*) = 2
     FROM df.nodes
     WHERE instance_id = '${instance_id}'
       AND query ~ 'bm25_(compact_step|needs_compaction)_if_current';"
[ "$(wait_for_instance "${instance_id}")" = "completed" ] \
    || fail "direct request did not complete"

if psql_as app_writer <<'SQL' >/dev/null 2>&1
CREATE TEMP TABLE adapter_temp (body text);
CREATE INDEX adapter_temp_idx ON adapter_temp
    USING bm25(body) WITH (text_config = 'english');
SELECT public.bm25_request_compaction('adapter_temp_idx');
SQL
then
    fail "wrapper accepted a temporary index"
fi

psql_super <<'SQL'
CREATE ROLE other_owner LOGIN;
CREATE TABLE unauthorized_docs (body text);
CREATE INDEX unauthorized_docs_idx ON unauthorized_docs
    USING bm25(body) WITH (text_config = 'english');
ALTER TABLE unauthorized_docs OWNER TO other_owner;
ALTER INDEX unauthorized_docs_idx OWNER TO other_owner;
SQL
if psql_as app_writer -c "SELECT
    public.bm25_request_compaction('unauthorized_docs_idx');" \
    >/dev/null 2>&1; then
    fail "writer requested compaction without INSERT privilege"
fi

psql_super <<'SQL'
GRANT CREATE ON SCHEMA public TO app_owner;
SET ROLE app_owner;
CREATE TABLE partitioned_docs (part_key integer, body text)
PARTITION BY RANGE (part_key);
CREATE TABLE partitioned_docs_leaf PARTITION OF partitioned_docs
    FOR VALUES FROM (0) TO (10);
CREATE INDEX partitioned_docs_idx ON partitioned_docs
    USING bm25(body) WITH (text_config = 'english');
RESET ROLE;
GRANT INSERT ON partitioned_docs TO app_writer;
SQL
leaf_index=$(psql_super -c "SELECT index_catalog.indexrelid::regclass
    FROM pg_catalog.pg_index index_catalog
         JOIN pg_catalog.pg_class relation
           ON relation.oid = index_catalog.indexrelid
         JOIN pg_catalog.pg_am access_method
           ON access_method.oid = relation.relam
    WHERE index_catalog.indrelid = 'partitioned_docs_leaf'::regclass
      AND access_method.amname = 'bm25';")
partition_instance=$(psql_as app_writer -c "SELECT
    public.bm25_request_compaction('${leaf_index}'::regclass);")
[ "$(wait_for_instance "${partition_instance}")" = "completed" ] \
    || fail "partition-authorized request did not complete"

psql_super -c "GRANT EXECUTE ON FUNCTION
    public.bm25_request_compaction(regclass) TO app_owner;"
psql_super -c "ALTER SYSTEM SET
    pg_textsearch.segments_per_level = '2';"
psql_super -c "SELECT pg_catalog.pg_reload_conf();" >/dev/null
psql_super <<'SQL'
CREATE TABLE compact_docs (id integer PRIMARY KEY, body text);
CREATE INDEX compact_docs_idx ON compact_docs
    USING bm25(body) WITH (text_config = 'english');
ALTER TABLE compact_docs OWNER TO app_owner;
ALTER INDEX compact_docs_idx OWNER TO app_owner;
SQL
levels_before=$(psql_as app_owner <<SQL
INSERT INTO compact_docs
SELECT i, 'first batch ' || i FROM generate_series(1, 20) i;
SELECT ${EXT_SCHEMA}.bm25_spill_index('compact_docs_idx');
INSERT INTO compact_docs
SELECT i, 'second batch ' || i FROM generate_series(21, 40) i;
SELECT ${EXT_SCHEMA}.bm25_spill_index('compact_docs_idx');
SELECT (${EXT_SCHEMA}.bm25_level_counts('compact_docs_idx'))[1];
SQL
)
levels_before=$(printf '%s\n' "${levels_before}" | tail -1)
[ "${levels_before}" -ge 2 ] \
    || fail "test did not create compaction debt"
compact_instance=$(psql_as app_owner -c "SELECT
    public.bm25_request_compaction('compact_docs_idx');")
[ "$(wait_for_instance "${compact_instance}")" = "completed" ] \
    || fail "stepped compaction request did not complete"
levels_after=$(psql_super -c "SELECT
    (${EXT_SCHEMA}.bm25_level_counts('compact_docs_idx'))[1];")
[ "${levels_after}" -lt "${levels_before}" ] \
    || fail "stepped task did not reduce L0"

stale_identity=$(psql_super -c "SELECT
    relation.oid::text || '|' ||
    database.oid::text || '|' ||
    coalesce(nullif(relation.reltablespace, 0),
             database.dattablespace)::text || '|' ||
    pg_catalog.pg_relation_filenode(relation.oid)::text
    FROM pg_catalog.pg_class relation
         JOIN pg_catalog.pg_database database
           ON database.datname = pg_catalog.current_database()
    WHERE relation.oid = 'compact_docs_idx'::regclass;")
IFS='|' read -r stale_oid stale_db stale_spc stale_filenode \
    <<<"${stale_identity}"
psql_as app_owner -c "REINDEX INDEX compact_docs_idx;" >/dev/null
assert_sql_true "stale physical helper is a no-op" \
    "SET ROLE textsearch_compactor;
     SELECT NOT ${EXT_SCHEMA}.bm25_needs_compaction_if_current(
        ${stale_oid}, ${stale_db}, ${stale_spc}, ${stale_filenode});"

printf 'Live pg_durable adapter checks passed.\n'
