#!/bin/bash
#
# End-to-end test for background BM25 segment compaction driven by
# pg_durable (see docs/superpowers/plans/2026-08-26-background-compaction-
# pg-durable.md, Task 7, and scripts/durable_compaction/README.md for the
# mechanism this exercises).
#
# This test brings up its OWN PostgreSQL instance with both pg_durable and
# pg_textsearch preloaded, wires up the three glue scripts in
# scripts/durable_compaction/ exactly as an operator would, and then
# verifies every criterion from the plan's "acceptance criteria" section:
#
#   1. Transaction boundaries: rollback, independent start, and PREPARE.
#   2. Temporary indexes compact inline without durable work.
#   3. Background compaction, latency, cascades, and permission failures.
#   4. Dropped and reindexed durable targets terminate without touching
#      replacement storage.
#   5. Dropped pending requests, including partition cascades, are removed.
#   6. The scheduled backstop repairs undispatched compaction debt.
#
# This test requires the pg_durable extension to be installed
# (module + control/SQL files discoverable via pg_config). It is NOT part
# of `make test-all`: CI has no pg_durable. Run it explicitly with
# `make test-durable`.
#
# Every pg_textsearch compaction GUC (segments_per_level, compaction_mode,
# compaction_request_function, memtable_pages_threshold) is PGC_SUSET, and
# critically is read by TWO different sessions: the writer's (to decide
# whether to enqueue) and the compactor's worker session (to decide
# whether df.loop's condition keeps stepping). This script always changes
# them via `ALTER SYSTEM ... ; SELECT pg_reload_conf()` as the superuser,
# which both sessions pick up -- never a plain per-session SET, which the
# worker session would never see. See scripts/durable_compaction/README.md
# ("Set the compaction threshold where the worker can see it").

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GLUE_DIR="${SCRIPT_DIR}/../../scripts/durable_compaction"
TEST_PORT=55447
TEST_DB=durable_compaction_test
EXT_SCHEMA=textsearch_ext
DATA_DIR="${SCRIPT_DIR}/../tmp_durable_compaction"
SOCKET_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOGFILE="${DATA_DIR}/postgres.log"
KEEP_DIR="${SCRIPT_DIR}/../tmp_durable_compaction_logs"
TEST_SIZE_MULTIPLIER=${TEST_SIZE_MULTIPLIER:-1.0}

# Pin every binary to the installation pg_config points at, matching
# the convention in shutdown_spill.sh. This must be the same install
# `make install` targeted, because that is the only $libdir where
# pg_textsearch.so is discoverable -- relying on an ambient PATH
# silently picks up a system Postgres that has neither extension and
# fails much later with a bare "could not access file".
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
PKGLIBDIR="$("${PG_CONFIG}" --pkglibdir)"

if [ "${DURABLE_PG_CONFIG_PROBE:-0}" = "1" ]; then
    printf '%s|%s\n' "${PGBINDIR}" "${PKGLIBDIR}"
    exit 0
fi

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
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    local exit_code=$?
    log "Cleaning up (exit code: $exit_code)..."
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate \
            &>/dev/null || true
    fi
    if [ "$exit_code" -ne 0 ] && [ -d "${DATA_DIR}" ]; then
        rm -rf "${KEEP_DIR}"
        mkdir -p "${KEEP_DIR}"
        cp "${LOGFILE}" "${KEEP_DIR}/" 2>/dev/null || true
        warn "Preserved server log in ${KEEP_DIR}"
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------
# psql helpers
# ---------------------------------------------------------------------

sql_as() {
    local role="$1"
    shift
    "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U "${role}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 "$@"
}

sql_super() { sql_as postgres "$@"; }

assert_compactor_ungranted() {
    local desc="$1"
    local granted
    granted=$(sql_super -c "SELECT
        EXISTS (
            SELECT 1
            FROM pg_catalog.pg_auth_members member
            WHERE member.member =
                    'textsearch_compactor'::pg_catalog.regrole
              AND member.roleid = 'app_owner'::pg_catalog.regrole)
        OR EXISTS (
            SELECT 1
            FROM pg_catalog.pg_namespace namespace
            CROSS JOIN LATERAL pg_catalog.aclexplode(namespace.nspacl) acl
            WHERE namespace.nspname = 'df'
              AND acl.grantee =
                    'textsearch_compactor'::pg_catalog.regrole);")
    assert_eq "$desc" "f" "$granted"
}

set_guc() {
    # ALTER SYSTEM cannot run inside a transaction block, and psql folds
    # multiple statements passed to one -c into an implicit transaction
    # -- so this must be two separate -c invocations, not one.
    sql_super -c "ALTER SYSTEM SET ${1} = '${2}';" >/dev/null
    sql_super -c "SELECT pg_reload_conf();" >/dev/null
}

reset_guc() {
    sql_super -c "ALTER SYSTEM RESET ${1};" >/dev/null
    sql_super -c "SELECT pg_reload_conf();" >/dev/null
}

# Exact label pg_textsearch/bm25_request_compaction uses for an index.
label_for() {
    sql_super -c "SELECT 'bm25-compact-' || '${1}'::regclass::oid;"
}

canonical_backstop_ids() {
    sql_super -c "SELECT durable_instance.id
        FROM df.instances durable_instance
        JOIN df.nodes loop_node
          ON loop_node.instance_id = durable_instance.id
         AND loop_node.id = durable_instance.root_node
        JOIN df.nodes sequence_node
          ON sequence_node.instance_id = durable_instance.id
         AND sequence_node.id = loop_node.left_node
        JOIN df.nodes schedule_node
          ON schedule_node.instance_id = durable_instance.id
         AND schedule_node.id = sequence_node.left_node
        JOIN df.nodes body_node
          ON body_node.instance_id = durable_instance.id
         AND body_node.id = sequence_node.right_node
        WHERE durable_instance.submitted_by =
                  'textsearch_compactor'::pg_catalog.regrole
          AND COALESCE(
                  durable_instance.database,
                  pg_catalog.current_database()::pg_catalog.text) =
                  pg_catalog.current_database()
          AND durable_instance.status IN ('pending', 'running')
          AND loop_node.node_type = 'LOOP'
          AND loop_node.right_node IS NULL
          AND sequence_node.node_type = 'THEN'
          AND schedule_node.node_type = 'WAIT_SCHEDULE'
          AND body_node.node_type = 'SQL'
          AND body_node.query =
                  'SELECT ${EXT_SCHEMA}.bm25_compact_pending()'
        ORDER BY durable_instance.created_at, durable_instance.id;"
}

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" != "$actual" ]; then
        error "ASSERTION FAILED: ${desc} (expected '${expected}', got \
'${actual}')"
    fi
    log "PASS: ${desc} (= ${actual})"
}

assert_true() {
    local desc="$1" actual="$2"
    if [ "$actual" != "t" ]; then
        error "ASSERTION FAILED: ${desc} (expected t, got '${actual}')"
    fi
    log "PASS: ${desc}"
}

# Poll df.instances (as superuser, which bypasses its RLS) until the
# given instance id reaches a terminal state or the timeout elapses.
wait_for_instance() {
    local id="$1" timeout="$2"
    local waited=0
    local st=""
    while [ "$waited" -lt "$timeout" ]; do
        st=$(sql_super -c \
            "SELECT status FROM df.instances WHERE id = '${id}';")
        if [ "$st" = "completed" ] || [ "$st" = "failed" ] \
            || [ "$st" = "cancelled" ]; then
            echo "$st"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "instance ${id} did not reach a terminal state within \
${timeout}s (last status: '${st}')"
}

wait_for_sleep_node() {
    local timeout="$1"
    local waited=0
    local active=""
    while [ "$waited" -lt "$timeout" ]; do
        active=$(sql_super -c "SELECT EXISTS (
            SELECT 1
            FROM pg_catalog.pg_stat_activity
            WHERE usename = 'textsearch_compactor'
              AND state = 'active'
              AND wait_event = 'PgSleep'
              AND query LIKE '%pg_sleep(15)%');")
        if [ "$active" = "t" ]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "pg_durable blocker did not start within ${timeout}s"
}

# Count distinct orchestration generations (execution_id values) the
# server log recorded for a durable instance. df.nodes is NOT usable for
# this: pg_durable reuses node rows across continue_as_new generations, so
# it always shows one row per node type regardless of how many iterations
# ran (see scripts/durable_compaction/README.md, "Known limitations").
count_generations() {
    grep -F "instance_id=${1} execution_id=" "${LOGFILE}" \
        | grep -oE 'execution_id=[0-9]+' \
        | sort -u | wc -l
}

# ---------------------------------------------------------------------
# Cluster setup
# ---------------------------------------------------------------------

setup_test_cluster() {
    log "Setting up dedicated PostgreSQL instance on port ${TEST_PORT}..."

    # Fail early and legibly if either module is missing from the
    # install pg_config points at. Without this the cluster dies at
    # startup with only "could not access file" in a log the caller
    # has to go hunting for.
    local lib
    for lib in pg_durable pg_textsearch; do
        if [ ! -f "${PKGLIBDIR}/${lib}.so" ]; then
            error "${lib}.so not found in ${PKGLIBDIR}. This test \
needs both pg_durable and pg_textsearch installed into the Postgres \
that '${PG_CONFIG}' points at. Set PG_CONFIG to the intended pg_config, \
put that installation's bin directory first on PATH, or run \
'make install'."
        fi
    done

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    # -U postgres: pg_durable.worker_role defaults to 'postgres' and is
    # used for the background worker's own internal connection, distinct
    # from the per-node submitted_by connection. Matching it to the
    # cluster's superuser avoids a "role does not exist" surprise.
    "${PGBINDIR}/initdb" -D "${DATA_DIR}" -U postgres \
        --auth-local=trust --auth-host=reject >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
unix_socket_directories = '${SOCKET_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_durable,pg_textsearch'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
max_prepared_transactions = 10
pg_durable.database = '${TEST_DB}'
pg_durable.worker_role = 'postgres'
pg_durable.max_user_connections = 1
EOF

    # pg_durable defaults PGHOST to 127.0.0.1.  Set it in the server
    # environment so both worker and per-node connections use the socket.
    PGHOST="${SOCKET_DIR}" "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" \
        -l "${LOGFILE}" -w -o "-p ${TEST_PORT}" \
        || error "Failed to start PostgreSQL"

    if "${PGBINDIR}/psql" -h 127.0.0.1 -p "${TEST_PORT}" \
        -U postgres -d postgres -c "SELECT 1" >/dev/null 2>&1; then
        error "disposable cluster unexpectedly accepts TCP connections"
    fi
    log "PASS: disposable cluster permits local trust only"

    "${PGBINDIR}/createdb" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U postgres "${TEST_DB}"
    sql_super -c "CREATE EXTENSION pg_durable;"
    sql_super -c "CREATE SCHEMA ${EXT_SCHEMA};"
    sql_super -c "GRANT USAGE ON SCHEMA ${EXT_SCHEMA} TO PUBLIC;"
    sql_super -c "ALTER DATABASE ${TEST_DB} \
SET search_path = public, ${EXT_SCHEMA};"
    sql_super -c "CREATE EXTENSION pg_textsearch \
WITH SCHEMA ${EXT_SCHEMA};"
    sql_super -c "CREATE SCHEMA hostile_path;"
    sql_super -c "CREATE DOMAIN hostile_path.oid AS pg_catalog.text;"
    sql_super -c "CREATE DOMAIN hostile_path.regclass AS pg_catalog.text;"
    sql_super -c "CREATE FUNCTION hostile_path.name_equal(
        pg_catalog.name, pg_catalog.name)
        RETURNS pg_catalog.bool
        LANGUAGE sql IMMUTABLE
        RETURN false;"
    sql_super -c "CREATE OPERATOR hostile_path.= (
        FUNCTION = hostile_path.name_equal,
        LEFTARG = pg_catalog.name,
        RIGHTARG = pg_catalog.name);"
    sql_super -c "GRANT USAGE ON SCHEMA hostile_path TO PUBLIC;"
    log "Test cluster ready (db=${TEST_DB})"
}

test_pg_durable_preflight() {
    log "=== Test: pg_durable capability preflight ==="
    local failures=0 output setup_succeeded wrapper_succeeded
    local backstop_succeeded wrapper_oid_before wrapper_oid_after
    local backstops_before backstops_after

    sql_super -c "CREATE ROLE preflight_owner LOGIN;"

    sql_super -c "UPDATE pg_catalog.pg_extension
        SET extversion = '0.2.5'
        WHERE extname = 'pg_durable';"
    setup_succeeded=f
    if output=$(sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=preflight_owner 2>&1); then
        setup_succeeded=t
    fi
    if [ "$setup_succeeded" = "t" ] \
        || [[ "$output" != \
*"pg_durable 0.2.6 or newer is required"* ]] \
        || [ "$(sql_super -c "SELECT EXISTS (
            SELECT 1 FROM pg_catalog.pg_roles
            WHERE rolname = 'textsearch_compactor');")" != "f" ]; then
        warn "RED: role setup did not reject unsupported pg_durable \
before creating textsearch_compactor (succeeded=${setup_succeeded}, \
output='${output}')"
        failures=$((failures + 1))
    fi
    sql_super -c "UPDATE pg_catalog.pg_extension
        SET extversion = '0.2.6'
        WHERE extname = 'pg_durable';"

    if [ "$(sql_super -c "SELECT EXISTS (
        SELECT 1 FROM pg_catalog.pg_roles
        WHERE rolname = 'textsearch_compactor');")" = "f" ]; then
        sql_super -c "CREATE ROLE textsearch_compactor LOGIN;"
        sql_super -c "SELECT df.grant_usage(
            'textsearch_compactor');" >/dev/null
    fi

    sql_super <<'SQL'
CREATE FUNCTION public.bm25_request_compaction(pg_catalog.regclass)
RETURNS pg_catalog.text
LANGUAGE sql
RETURN 'preflight-sentinel'::pg_catalog.text;
SQL
    wrapper_oid_before=$(sql_super -c "SELECT
        'public.bm25_request_compaction(pg_catalog.regclass)'
            ::pg_catalog.regprocedure::pg_catalog.oid;")
    sql_super -c "ALTER FUNCTION
        df.start(pg_catalog.text, pg_catalog.text, pg_catalog.text,
                 pg_catalog.text)
        RENAME TO start_missing;"
    wrapper_succeeded=f
    if output=$(sql_super -f "${GLUE_DIR}/02_wrapper.sql" 2>&1); then
        wrapper_succeeded=t
    fi
    sql_super -c "ALTER FUNCTION
        df.start_missing(pg_catalog.text, pg_catalog.text, pg_catalog.text,
                         pg_catalog.text)
        RENAME TO start;"
    wrapper_oid_after=$(sql_super -c "SELECT
        'public.bm25_request_compaction(pg_catalog.regclass)'
            ::pg_catalog.regprocedure::pg_catalog.oid;")
    if [ "$wrapper_succeeded" = "t" ] \
        || [[ "$output" != *"pg_durable 0.2.6 API is incomplete"* ]] \
        || [ "$wrapper_oid_after" != "$wrapper_oid_before" ]; then
        warn "RED: wrapper setup did not reject incomplete pg_durable \
before replacing the wrapper (succeeded=${wrapper_succeeded}, \
oid=${wrapper_oid_before}->${wrapper_oid_after}, output='${output}')"
        failures=$((failures + 1))
    fi

    sql_super -c "ALTER TABLE df.instances
        RENAME COLUMN created_at TO created_at_missing;"
    backstops_before=$(sql_super -c "SELECT count(*) FROM df.instances
        WHERE label = 'bm25-compaction-backstop'
          AND status IN ('pending', 'running');")
    backstop_succeeded=f
    if output=$(sql_as textsearch_compactor \
        -f "${GLUE_DIR}/03_backstop.sql" 2>&1); then
        backstop_succeeded=t
    fi
    sql_super -c "ALTER TABLE df.instances
        RENAME COLUMN created_at_missing TO created_at;"
    backstops_after=$(sql_super -c "SELECT count(*) FROM df.instances
        WHERE label = 'bm25-compaction-backstop'
          AND status IN ('pending', 'running');")
    if [ "$backstop_succeeded" = "t" ] \
        || [[ "$output" != *"pg_durable 0.2.6 API is incomplete"* ]] \
        || [ "$backstops_after" != "$backstops_before" ]; then
        warn "RED: backstop setup did not reject incomplete pg_durable \
before registration (succeeded=${backstop_succeeded}, \
live=${backstops_before}->${backstops_after}, output='${output}')"
        failures=$((failures + 1))
    fi

    if [ "$failures" -ne 0 ]; then
        error "${failures} pg_durable preflight assertions failed"
    fi

    sql_super -c "DROP FUNCTION
        public.bm25_request_compaction(pg_catalog.regclass);"
    sql_super -c "SELECT df.revoke_usage(
        'textsearch_compactor');" >/dev/null
    sql_super -c "DROP OWNED BY textsearch_compactor;"
    sql_super -c "DROP ROLE textsearch_compactor;"
    sql_super -c "DROP ROLE preflight_owner;"
    log "PASS: every operator script preflights pg_durable before side effects"
}

test_callback_drop_after_lookup() {
    log "=== Test: callback dropped after protected lookup ==="
    local main_output="${DATA_DIR}/callback_drop_main.out"
    local drop_output="${DATA_DIR}/callback_drop_dropper.out"
    local main_pid drop_pid main_status=0 drop_status=0 ready=f

    sql_super <<'SQL'
CREATE TABLE callback_drop_docs_a (id integer PRIMARY KEY, body text);
CREATE INDEX callback_drop_docs_a_idx ON callback_drop_docs_a
    USING bm25(body) WITH (text_config = 'english');
CREATE TABLE callback_drop_docs_b (id integer PRIMARY KEY, body text);
CREATE INDEX callback_drop_docs_b_idx ON callback_drop_docs_b
    USING bm25(body) WITH (text_config = 'english');
CREATE TABLE callback_drop_ready (
    ready boolean NOT NULL,
    dropped boolean NOT NULL DEFAULT false
);
CREATE FUNCTION public.callback_drop_after_lookup(regclass)
RETURNS void
LANGUAGE plpgsql
AS $fn$
DECLARE
    drop_visible boolean := false;
    attempt integer;
BEGIN
    IF pg_catalog.pg_advisory_unlock(471, 7) THEN
        FOR attempt IN 1..100 LOOP
            SELECT dropped INTO drop_visible
            FROM public.callback_drop_ready;
            EXIT WHEN drop_visible;
            PERFORM pg_catalog.pg_sleep(0.05);
        END LOOP;
        IF NOT drop_visible THEN
            RAISE EXCEPTION 'callback drop did not become visible';
        END IF;
    END IF;
END
$fn$;
SQL

    (
        sql_super <<'SQL'
SELECT pg_catalog.pg_advisory_lock(471, 7);
INSERT INTO callback_drop_ready (ready) VALUES (true);
SELECT pg_catalog.pg_sleep(1);
SET pg_textsearch.compaction_mode = 'background';
SET pg_textsearch.compaction_request_function =
    'public.callback_drop_after_lookup';
SET pg_textsearch.segments_per_level = 2;
BEGIN;
INSERT INTO callback_drop_docs_a
SELECT i, 'callback drop a ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('callback_drop_docs_a_idx');
INSERT INTO callback_drop_docs_a
SELECT i, 'callback drop a ' || i FROM generate_series(21, 40) i;
SELECT bm25_spill_index('callback_drop_docs_a_idx');
INSERT INTO callback_drop_docs_b
SELECT i, 'callback drop b ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('callback_drop_docs_b_idx');
INSERT INTO callback_drop_docs_b
SELECT i, 'callback drop b ' || i FROM generate_series(21, 40) i;
SELECT bm25_spill_index('callback_drop_docs_b_idx');
COMMIT;
SQL
    ) >"${main_output}" 2>&1 &
    main_pid=$!

    for _ in $(seq 1 100); do
        ready=$(sql_super -c "SELECT EXISTS (
            SELECT 1 FROM callback_drop_ready WHERE ready);")
        [ "$ready" = "t" ] && break
        sleep 0.1
    done
    [ "$ready" = "t" ] \
        || error "callback-drop writer did not reach its ready point"

    (
        sql_super -c "SELECT pg_catalog.pg_advisory_lock(471, 7);
            DROP FUNCTION public.callback_drop_after_lookup(regclass);
            UPDATE callback_drop_ready SET dropped = true;"
    ) >"${drop_output}" 2>&1 &
    drop_pid=$!

    wait "$main_pid" || main_status=$?
    wait "$drop_pid" || drop_status=$?
    if [ "$main_status" -ne 0 ] || [ "$drop_status" -ne 0 ]; then
        error "callback drop race failed (writer=${main_status}, \
dropper=${drop_status}, writer_output='$(<"${main_output}")', \
dropper_output='$(<"${drop_output}")')"
    fi
    if ! grep -Fq \
        'function public.callback_drop_after_lookup(regclass) does not exist' \
        "${main_output}"; then
        error "callback disappearance was not reported from shielded SPI \
execution (output='$(<"${main_output}")')"
    fi
    assert_eq "callback disappearance preserves both writer transactions" \
        "80" "$(sql_super -c "SELECT
            (SELECT count(*) FROM callback_drop_docs_a)
            + (SELECT count(*) FROM callback_drop_docs_b);")"

    sql_super -c "DROP TABLE callback_drop_docs_a, callback_drop_docs_b,
        callback_drop_ready;"
    log "PASS: a post-lookup callback drop warns inside shielded SPI"
}

setup_roles_and_glue() {
    log "Creating app_owner / app_writer and wiring the glue scripts..."

    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        >/dev/null 2>&1; then
        error "setup accepted a missing index_owner"
    fi
    log "PASS: setup rejects a missing index_owner"

    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=postgres >/dev/null 2>&1; then
        error "setup accepted a superuser index_owner"
    fi
    log "PASS: setup rejects a superuser index_owner"

    sql_super -c "CREATE ROLE app_owner LOGIN;"
    sql_super -c "CREATE ROLE app_writer LOGIN;"

    sql_super -c "CREATE ROLE textsearch_compactor LOGIN
        PASSWORD 'must-not-survive';"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted a credentialed compactor role"
    fi
    log "PASS: setup rejects a credentialed compactor role"
    assert_compactor_ungranted \
        "credential rejection happens before owner/df grants"
    sql_super -c "ALTER ROLE textsearch_compactor PASSWORD NULL;"

    sql_super -c "ALTER ROLE textsearch_compactor
        SUPERUSER CREATEDB CREATEROLE REPLICATION BYPASSRLS;"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted a privileged compactor role"
    fi
    log "PASS: setup rejects privileged compactor attributes"
    assert_compactor_ungranted \
        "attribute rejection happens before owner/df grants"
    sql_super -c "ALTER ROLE textsearch_compactor
        NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS;"

    sql_super -c "ALTER ROLE textsearch_compactor CONNECTION LIMIT 2;"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted a compactor connection limit"
    fi
    log "PASS: setup rejects a non-default compactor connection limit"
    assert_compactor_ungranted \
        "connection-limit rejection happens before owner/df grants"
    sql_super -c "ALTER ROLE textsearch_compactor CONNECTION LIMIT -1;"

    sql_super -c "ALTER ROLE textsearch_compactor IN DATABASE ${TEST_DB}
        SET search_path = hostile_path, pg_catalog;"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted an unsafe compactor search_path setting"
    fi
    log "PASS: setup rejects unsafe role/database settings"
    assert_compactor_ungranted \
        "setting rejection happens before owner/df grants"
    sql_super -c "ALTER ROLE textsearch_compactor IN DATABASE ${TEST_DB}
        RESET search_path;"
    sql_super -c "ALTER ROLE textsearch_compactor
        SET statement_timeout = 0;"

    sql_super -c "CREATE ROLE hostile_parent NOLOGIN;"
    sql_super -c "CREATE ROLE hostile_grantor NOLOGIN;"
    sql_super -c "GRANT hostile_parent TO hostile_grantor
        WITH ADMIN TRUE;"
    sql_super <<'SQL'
SET ROLE hostile_grantor;
GRANT hostile_parent TO textsearch_compactor
    WITH INHERIT TRUE, SET FALSE;
RESET ROLE;
SQL
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted an unexpected compactor membership"
    fi
    log "PASS: setup rejects non-operator compactor memberships"
    assert_compactor_ungranted \
        "membership rejection happens before owner/df grants"
    assert_eq "failed setup preserves the unexpected membership" "t" \
        "$(sql_super -c "SELECT pg_has_role(
            'textsearch_compactor', 'hostile_parent', 'MEMBER');")"
    sql_super <<'SQL'
SET ROLE hostile_grantor;
REVOKE hostile_parent FROM textsearch_compactor;
RESET ROLE;
SQL
    sql_super -c "REVOKE hostile_parent FROM hostile_grantor;"
    sql_super -c "DROP ROLE hostile_grantor;"
    sql_super -c "DROP ROLE hostile_parent;"

    sql_super <<'SQL'
CREATE FUNCTION public.compactor_owner_trap()
RETURNS name
LANGUAGE sql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS 'SELECT current_user';
ALTER FUNCTION public.compactor_owner_trap()
    OWNER TO textsearch_compactor;
SQL
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted an unexpected compactor-owned object"
    fi
    assert_compactor_ungranted \
        "owned-object rejection happens before owner/df grants"
    assert_eq "failed setup preserves the hostile owner object" "t" \
        "$(sql_super -c "SELECT
            proowner = 'textsearch_compactor'::pg_catalog.regrole
            AND prosecdef
            AND has_function_privilege(
                'app_writer',
                'public.compactor_owner_trap()',
                'EXECUTE')
            FROM pg_catalog.pg_proc
            WHERE oid =
                'public.compactor_owner_trap()'::pg_catalog.regprocedure;")"
    log "PASS: setup rejects unexpected compactor-owned objects before grants"
    sql_super -c "DROP FUNCTION public.compactor_owner_trap();"

    sql_super <<'SQL'
CREATE FUNCTION public.bm25_request_compaction(pg_catalog.regclass)
RETURNS pg_catalog.text
LANGUAGE plpgsql
SECURITY DEFINER
AS $fn$
BEGIN
    RETURN 'hostile-wrapper';
END
$fn$;
ALTER FUNCTION public.bm25_request_compaction(pg_catalog.regclass)
    OWNER TO textsearch_compactor;
SQL
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted a hostile exact-identity request wrapper"
    fi
    assert_compactor_ungranted \
        "hostile exact-wrapper rejection happens before owner/df grants"
    assert_eq "failed setup preserves the hostile PUBLIC wrapper" "t" \
        "$(sql_super -c "SELECT
            proowner = 'textsearch_compactor'::pg_catalog.regrole
            AND prosecdef
            AND has_function_privilege(
                'public',
                'public.bm25_request_compaction(regclass)',
                'EXECUTE')
            FROM pg_catalog.pg_proc
            WHERE oid =
                'public.bm25_request_compaction(regclass)'
                    ::pg_catalog.regprocedure;")"
    log "PASS: setup rejects a hostile exact-identity request wrapper"
    sql_super -c "DROP FUNCTION
        public.bm25_request_compaction(pg_catalog.regclass);"

    sql_super -c "GRANT CREATE ON SCHEMA public TO PUBLIC;"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted PUBLIC CREATE on the wrapper schema"
    fi
    log "PASS: setup rejects PUBLIC CREATE on wrapper schema"
    sql_super -c "REVOKE CREATE ON SCHEMA public FROM PUBLIC;"

    sql_super -f "${GLUE_DIR}/02_wrapper.sql" \
        -v writer_role=app_writer >/dev/null
    sql_super -c "CREATE ROLE delegable_writer LOGIN;"
    sql_super -c "GRANT EXECUTE ON FUNCTION \
public.bm25_request_compaction(regclass) TO delegable_writer \
WITH GRANT OPTION;"
    local delegable_output delegable_setup_succeeded=f
    local delegable_wrapper_oid_before delegable_wrapper_oid_after
    delegable_wrapper_oid_before=$(sql_super -c "SELECT \
'public.bm25_request_compaction(regclass)'::regprocedure::oid;")
    if delegable_output=$(sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner 2>&1); then
        delegable_setup_succeeded=t
    fi
    delegable_wrapper_oid_after=$(sql_super -c "SELECT \
'public.bm25_request_compaction(regclass)'::regprocedure::oid;")
    if [ "$delegable_setup_succeeded" = "t" ] \
        || [[ "$delegable_output" != \
*"existing textsearch_compactor role owns unexpected objects"* ]]; then
        error "setup did not reject a named wrapper EXECUTE WITH GRANT \
OPTION (succeeded=${delegable_setup_succeeded}, \
output='${delegable_output}')"
    fi
    assert_compactor_ungranted \
        "grant-option rejection happens before owner/df grants"
    assert_eq "failed setup preserves the exact managed wrapper object" \
        "$delegable_wrapper_oid_before" "$delegable_wrapper_oid_after"
    assert_eq "failed setup preserves approved and grantable wrapper ACLs" \
        "t" "$(sql_super -c "SELECT
            pg_catalog.md5(procedure.prosrc) =
                'c8304e8b8d9218c92625ccd8752864ce'
            AND EXISTS (
                SELECT 1
                FROM pg_catalog.aclexplode(procedure.proacl) acl
                WHERE acl.grantee =
                        'app_writer'::pg_catalog.regrole
                  AND acl.privilege_type = 'EXECUTE'
                  AND NOT acl.is_grantable)
            AND EXISTS (
                SELECT 1
                FROM pg_catalog.aclexplode(procedure.proacl) acl
                WHERE acl.grantee =
                        'delegable_writer'::pg_catalog.regrole
                  AND acl.privilege_type = 'EXECUTE'
                  AND acl.is_grantable)
            FROM pg_catalog.pg_proc procedure
            WHERE procedure.oid =
                'public.bm25_request_compaction(regclass)'
                    ::pg_catalog.regprocedure;")"
    log "PASS: setup rejects delegable wrapper execution before grants"
    sql_super -c "REVOKE ALL ON FUNCTION \
public.bm25_request_compaction(regclass) FROM delegable_writer;"
    sql_super -c "DROP ROLE delegable_writer;"

    PGOPTIONS="-c search_path=hostile_path,pg_catalog,public,${EXT_SCHEMA},df" \
        sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null

    sql_super -c "CREATE ROLE app_owner_two LOGIN;"
    sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner_two >/dev/null
    sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null
    assert_eq "clean multi-owner reruns preserve owner memberships" "t" \
        "$(sql_super -c "SELECT
            pg_has_role('textsearch_compactor', 'app_owner', 'MEMBER')
            AND pg_has_role(
                'textsearch_compactor', 'app_owner_two', 'MEMBER');")"
    assert_eq "owner memberships never grant SET ROLE" "f" \
        "$(sql_super -c "SELECT
            pg_has_role('textsearch_compactor', 'app_owner', 'SET')
            OR pg_has_role(
                'textsearch_compactor', 'app_owner_two', 'SET');")"
    assert_eq "clean reruns preserve harmless role settings" "t" \
        "$(sql_super -c "SELECT EXISTS (
            SELECT 1
            FROM pg_catalog.pg_db_role_setting setting
            CROSS JOIN LATERAL
                pg_catalog.unnest(setting.setconfig) config(value)
            WHERE setting.setrole =
                    'textsearch_compactor'::pg_catalog.regrole
              AND config.value = 'statement_timeout=0');")"

    sql_super -c "CREATE ROLE compactor_member LOGIN;"
    sql_super -c "CREATE ROLE inbound_owner NOLOGIN;"
    sql_super -c "GRANT textsearch_compactor TO compactor_member;"
    local inbound_setup_failed=f
    if ! sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=inbound_owner >/dev/null 2>&1; then
        inbound_setup_failed=t
    fi
    local inbound_owner_granted inbound_membership_preserved
    inbound_owner_granted=$(sql_super -c "SELECT pg_has_role(\
'textsearch_compactor', 'inbound_owner', 'MEMBER');")
    inbound_membership_preserved=$(sql_super -c "SELECT pg_has_role(\
'compactor_member', 'textsearch_compactor', 'MEMBER');")
    if [ "$inbound_setup_failed" != "t" ] \
        || [ "$inbound_owner_granted" != "f" ]; then
        error "ASSERTION FAILED: setup with an inbound compactor member \
must fail transactionally (failed=${inbound_setup_failed}, \
owner_granted=${inbound_owner_granted})"
    fi
    log "PASS: setup rejects inbound compactor membership without \
granting the requested owner"
    assert_eq "failed setup preserves operator-managed inbound membership" \
        "t" "$inbound_membership_preserved"
    sql_super -c "REVOKE textsearch_compactor FROM compactor_member;"
    sql_super -c "DROP ROLE compactor_member;"
    sql_super -c "DROP ROLE inbound_owner;"

    local writer_can_step_current compactor_can_step_current
    writer_can_step_current=$(sql_super -c "SELECT
        has_function_privilege(
            'app_writer',
            '${EXT_SCHEMA}.bm25_compact_step_if_current(oid,oid,oid,oid)',
            'EXECUTE')
        OR has_function_privilege(
            'app_writer',
            '${EXT_SCHEMA}.bm25_needs_compaction_if_current(oid,oid,oid,oid)',
            'EXECUTE');")
    compactor_can_step_current=$(sql_super -c "SELECT
        has_function_privilege(
            'textsearch_compactor',
            '${EXT_SCHEMA}.bm25_compact_step_if_current(oid,oid,oid,oid)',
            'EXECUTE')
        AND has_function_privilege(
            'textsearch_compactor',
            '${EXT_SCHEMA}.bm25_needs_compaction_if_current(oid,oid,oid,oid)',
            'EXECUTE');")
    assert_eq "durable target helpers are revoked from writers" \
        "f" "$writer_can_step_current"
    assert_eq "durable target helpers are granted to compactor" \
        "t" "$compactor_can_step_current"

    if sql_as textsearch_compactor -c "SET ROLE app_owner;" \
        >/dev/null 2>&1; then
        error "compactor can SET ROLE app_owner"
    fi
    log "PASS: compactor cannot SET ROLE app_owner"

    sql_super -c "CREATE ROLE unsafe_owner NOLOGIN;"
    sql_super -c "CREATE ROLE unsafe_owner_superuser \
SUPERUSER NOLOGIN;"
    sql_super -c "GRANT unsafe_owner_superuser TO unsafe_owner \
WITH INHERIT TRUE, SET FALSE;"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=unsafe_owner >/dev/null 2>&1; then
        error "setup accepted an owner nested under a superuser role"
    fi
    log "PASS: setup rejects owner membership in a superuser role"
    local unsafe_owner_membership
    unsafe_owner_membership=$(sql_super -c "SELECT pg_has_role(\
'textsearch_compactor', 'unsafe_owner', 'MEMBER');")
    assert_eq "failed owner setup rolls back compactor membership" \
        "f" "$unsafe_owner_membership"
    sql_super -c "REVOKE unsafe_owner_superuser FROM unsafe_owner;"
    sql_super -c "DROP ROLE unsafe_owner;"
    sql_super -c "DROP ROLE unsafe_owner_superuser;"

    sql_super -c "CREATE ROLE owner_membership_grantor NOLOGIN;"
    sql_super -c "GRANT app_owner TO owner_membership_grantor \
WITH ADMIN TRUE;"
    sql_super <<'SQL'
SET ROLE owner_membership_grantor;
GRANT app_owner TO textsearch_compactor
    WITH INHERIT TRUE, SET TRUE;
RESET ROLE;
SQL
    local compactor_can_set_owner
    compactor_can_set_owner=$(sql_super -c "SELECT pg_has_role(\
'textsearch_compactor', 'app_owner', 'SET');")
    assert_eq "alternate grantor creates a SET path to owner" \
        "t" "$compactor_can_set_owner"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted an alternate SET path to index owner"
    fi
    log "PASS: setup rejects alternate-grantor SET path to owner"
    sql_super <<'SQL'
SET ROLE owner_membership_grantor;
REVOKE app_owner FROM textsearch_compactor;
RESET ROLE;
REVOKE app_owner FROM owner_membership_grantor;
DROP ROLE owner_membership_grantor;
SQL

    sql_super -c "CREATE ROLE compactor_superuser_parent \
SUPERUSER NOLOGIN;"
    sql_super -c "GRANT compactor_superuser_parent TO \
textsearch_compactor WITH INHERIT TRUE, SET FALSE;"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted compactor membership in a superuser role"
    fi
    log "PASS: setup rejects compactor membership in a superuser role"
    sql_super -c "REVOKE compactor_superuser_parent FROM \
textsearch_compactor;"
    sql_super -c "DROP ROLE compactor_superuser_parent;"

    sql_super -c "CREATE ROLE compactor_superuser_bridge NOLOGIN;"
    sql_super -c "CREATE ROLE compactor_superuser_grandparent \
SUPERUSER NOLOGIN;"
    sql_super -c "GRANT compactor_superuser_grandparent TO \
compactor_superuser_bridge WITH INHERIT TRUE, SET FALSE;"
    sql_super -c "GRANT compactor_superuser_bridge TO \
textsearch_compactor WITH INHERIT TRUE, SET FALSE;"
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null 2>&1; then
        error "setup accepted transitive membership in a superuser role"
    fi
    log "PASS: setup rejects transitive superuser-role membership"
    sql_super -c "REVOKE compactor_superuser_bridge FROM \
textsearch_compactor;"
    sql_super -c "REVOKE compactor_superuser_grandparent FROM \
compactor_superuser_bridge;"
    sql_super -c "DROP ROLE compactor_superuser_bridge;"
    sql_super -c "DROP ROLE compactor_superuser_grandparent;"

    PGOPTIONS="-c search_path=hostile_path,pg_catalog,public,${EXT_SCHEMA},df" \
        sql_super -f "${GLUE_DIR}/02_wrapper.sql" \
        -v writer_role=app_writer >/dev/null
    sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null
    log "PASS: role setup rerun accepts the exact managed wrapper"
    assert_eq "role rerun preserves the approved writer grant" "t" \
        "$(sql_super -c "SELECT has_function_privilege(
            'app_writer',
            'public.bm25_request_compaction(regclass)',
            'EXECUTE');")"
    assert_eq "wrapper signature uses pg_catalog.regclass" "regclass" \
        "$(sql_super -c "SELECT pg_catalog.format_type(proargtypes[0], NULL)
            FROM pg_catalog.pg_proc
            WHERE oid =
                'public.bm25_request_compaction(regclass)'::regprocedure;")"

    sql_super -c "CREATE ROLE wrapper_drift_owner LOGIN;"
    sql_super <<'SQL'
CREATE OR REPLACE FUNCTION
    public.bm25_request_compaction(idx pg_catalog.regclass)
RETURNS pg_catalog.text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $fn$
BEGIN
    RETURN 'changed-managed-body';
END
$fn$;
SQL
    if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=wrapper_drift_owner >/dev/null 2>&1; then
        error "setup accepted drift in the managed wrapper body/hash"
    fi
    assert_eq "body-drift rejection happens before the new owner grant" \
        "f" "$(sql_super -c "SELECT pg_has_role(
            'textsearch_compactor', 'wrapper_drift_owner', 'MEMBER');")"
    assert_eq "body-drift rejection preserves approved writer ACLs" "t" \
        "$(sql_super -c "SELECT has_function_privilege(
            'app_writer',
            'public.bm25_request_compaction(regclass)',
            'EXECUTE');")"
    log "PASS: clean rerun rejects managed wrapper body/hash drift"
    sql_super -f "${GLUE_DIR}/02_wrapper.sql" -v writer_role=app_writer \
        >/dev/null
    sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=app_owner >/dev/null
    sql_super -c "DROP ROLE wrapper_drift_owner;"

    sql_super -c "CREATE ROLE old_writer LOGIN;"
    sql_super -c "GRANT EXECUTE ON FUNCTION \
public.bm25_request_compaction(regclass) TO old_writer;"
    sql_super -f "${GLUE_DIR}/02_wrapper.sql" -v writer_role=app_writer \
        >/dev/null

    local old_writer_can_execute
    old_writer_can_execute=$(sql_super -c "SELECT has_function_privilege(\
'old_writer', 'public.bm25_request_compaction(regclass)', 'EXECUTE');")
    assert_eq "wrapper reinstall removes stale named EXECUTE grants" \
        "f" "$old_writer_can_execute"

    sql_super -c "CREATE ROLE default_privilege_writer LOGIN;"
    sql_super -c "ALTER DEFAULT PRIVILEGES FOR ROLE postgres \
IN SCHEMA public GRANT EXECUTE ON FUNCTIONS \
TO default_privilege_writer;"
    sql_super -f "${GLUE_DIR}/02_wrapper.sql" -v writer_role=app_writer \
        >/dev/null

    local default_writer_can_execute
    default_writer_can_execute=$(sql_super -c "SELECT \
has_function_privilege('default_privilege_writer', \
'public.bm25_request_compaction(regclass)', 'EXECUTE');")
    assert_eq "wrapper removes named default EXECUTE grants" \
        "f" "$default_writer_can_execute"
    sql_super -c "ALTER DEFAULT PRIVILEGES FOR ROLE postgres \
IN SCHEMA public REVOKE EXECUTE ON FUNCTIONS \
FROM default_privilege_writer;"
    sql_super -c "DROP ROLE default_privilege_writer;"

    local wrapper_oid_before
    wrapper_oid_before=$(sql_super -c "SELECT \
'public.bm25_request_compaction(regclass)'::regprocedure::oid;")
    if sql_super -f "${GLUE_DIR}/02_wrapper.sql" \
        -v writer_role=missing_writer >/dev/null 2>&1; then
        error "wrapper setup accepted a missing writer role"
    fi
    local wrapper_oid_after
    wrapper_oid_after=$(sql_super -c "SELECT \
'public.bm25_request_compaction(regclass)'::regprocedure::oid;")
    assert_eq "failed wrapper replacement preserves old function" \
        "$wrapper_oid_before" "$wrapper_oid_after"
    local writer_still_can_execute
    writer_still_can_execute=$(sql_super -c "SELECT \
has_function_privilege('app_writer', \
'public.bm25_request_compaction(regclass)', 'EXECUTE');")
    assert_eq "failed wrapper replacement preserves writer grant" \
        "t" "$writer_still_can_execute"

    # app_owner is used as the general spilling writer in most scenarios
    # below (it must own the index to call bm25_spill_index() at all);
    # it needs the same EXECUTE grant 02_wrapper.sql gave app_writer, or
    # its own PRE_COMMIT dispatch is denied.
    sql_super -c "GRANT EXECUTE ON FUNCTION \
public.bm25_request_compaction(regclass) TO app_owner;"

    set_guc pg_textsearch.compaction_request_function \
        'public.bm25_request_compaction'
    set_guc pg_textsearch.compaction_mode 'background'
    set_guc pg_textsearch.segments_per_level '2'
}

# The pg_durable background worker initializes ASYNCHRONOUSLY after
# CREATE EXTENSION. Submitting before it is ready leaves an instance
# stuck 'pending' forever (see README, "The background worker initializes
# asynchronously"). Retry a trivial probe submission until it both
# succeeds and actually reaches 'completed'.
wait_for_durable_worker() {
    log "Waiting for the pg_durable background worker..."
    local attempt id st
    for attempt in $(seq 1 60); do
        id=$(sql_as textsearch_compactor -c \
            "SELECT df.start('SELECT 1', label => 'probe-ready-${attempt}');" \
            2>/dev/null || true)
        if [ -n "$id" ]; then
            local i
            for i in $(seq 1 20); do
                st=$(sql_as textsearch_compactor -c \
                    "SELECT status FROM df.instances WHERE id = '${id}';" \
                    2>/dev/null || true)
                if [ "$st" = "completed" ]; then
                    log "pg_durable worker ready (probe ${id} completed)"
                    return 0
                fi
                [ "$st" = "failed" ] && break
                sleep 1
            done
        fi
        sleep 1
    done
    error "pg_durable background worker never became ready"
}

test_backstop_canary_failure() {
    log "=== Test: failed compactor execution canary ==="
    sql_super <<'SQL'
CREATE FUNCTION public.block_compactor_canary_connections()
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $fn$
BEGIN
    EXECUTE 'ALTER ROLE textsearch_compactor CONNECTION LIMIT 0';
END
$fn$;
REVOKE ALL ON FUNCTION public.block_compactor_canary_connections()
FROM PUBLIC;
GRANT EXECUTE ON FUNCTION public.block_compactor_canary_connections()
TO textsearch_compactor;
SQL

    local live_before live_after output script_succeeded=f
    live_before=$(sql_super -c "SELECT count(*) FROM df.instances
        WHERE label = 'bm25-compaction-backstop'
          AND status IN ('pending', 'running');")
    if output=$(sql_as textsearch_compactor 2>&1 <<SQL
SELECT public.block_compactor_canary_connections();
\i ${GLUE_DIR}/03_backstop.sql
SQL
); then
        script_succeeded=t
    fi
    sql_super -c "ALTER ROLE textsearch_compactor
        CONNECTION LIMIT -1;"
    sql_super -c "DROP FUNCTION
        public.block_compactor_canary_connections();"

    live_after=$(sql_super -c "SELECT count(*) FROM df.instances
        WHERE label = 'bm25-compaction-backstop'
          AND status IN ('pending', 'running');")
    if [ "$script_succeeded" = "t" ]; then
        error "ASSERTION FAILED: backstop registration ignored a failed \
compactor execution canary (output='${output}')"
    fi
    if [[ "$output" != *"compactor execution canary failed"* ]]; then
        error "ASSERTION FAILED: failed execution canary was not reported \
actionably (output='${output}')"
    fi
    assert_eq "failed canary aborts before backstop registration" \
        "$live_before" "$live_after"

    local canary_id canary_status canary_result
    canary_id=$(sql_super -c "SELECT id FROM df.instances
        WHERE label = 'bm25-compaction-canary'
        ORDER BY created_at DESC LIMIT 1;")
    if [ -z "$canary_id" ]; then
        error "ASSERTION FAILED: failed execution canary was not persisted"
    fi
    canary_status=$(sql_super -c "SELECT status FROM df.instances
        WHERE id = '${canary_id}';")
    assert_eq "failed execution canary reaches terminal status" \
        "failed" "$canary_status"
    canary_result=$(sql_super -c "SELECT result #>> '{}'
        FROM df.nodes
        WHERE instance_id = '${canary_id}'
          AND status = 'failed'
        ORDER BY updated_at DESC LIMIT 1;")
    if [ -z "$canary_result" ]; then
        error "ASSERTION FAILED: failed canary diagnostic was not persisted \
in df.nodes.result"
    fi
    log "PASS: failed canary diagnostic persisted in df.nodes.result"
}

test_terminal_backstop_confirmation() {
    log "=== Test: terminal backstop registration confirmation ==="

    sql_super <<'SQL'
CREATE FUNCTION public.force_terminal_backstop()
RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $fn$
BEGIN
    IF NEW.label = 'bm25-compaction-backstop' THEN
        NEW.status := 'cancelled';
    END IF;
    RETURN NEW;
END
$fn$;
CREATE TRIGGER force_terminal_backstop
BEFORE INSERT ON df.instances
FOR EACH ROW
EXECUTE FUNCTION public.force_terminal_backstop();
SQL

    local output script_succeeded=f terminal_id
    if output=$(sql_as textsearch_compactor \
        -f "${GLUE_DIR}/03_backstop.sql" 2>&1); then
        script_succeeded=t
    fi
    terminal_id=$(sql_super -c "SELECT id
        FROM df.instances
        WHERE label = 'bm25-compaction-backstop'
          AND status = 'cancelled'
        ORDER BY created_at DESC
        LIMIT 1;")

    sql_super -c "DROP TRIGGER force_terminal_backstop ON df.instances;"
    sql_super -c "DROP FUNCTION public.force_terminal_backstop();"

    if [ "$script_succeeded" = "t" ] \
        || [ -z "$terminal_id" ] \
        || [[ "$output" != *"$terminal_id"* ]] \
        || [[ "$output" != *"no longer pending or running"* ]]; then
        error "ASSERTION FAILED: terminal registration was not rejected \
(succeeded=${script_succeeded}, id=${terminal_id}, output=${output})"
    fi
    assert_eq "terminal confirmation leaves no live backstop" "0" \
        "$(sql_super -c "SELECT count(*)
            FROM df.instances
            WHERE label = 'bm25-compaction-backstop'
              AND status IN ('pending', 'running');")"
    log "PASS: terminal registration fails with recorded instance \
${terminal_id}"
}

# Build ${1} rounds of INSERT + bm25_spill_index() on ${3} against index
# ${2} inside a single transaction, as ${4} (the index owner, since
# bm25_spill_index() requires ownership). Returns nothing; caller reads
# df.instances afterwards.
spill_rounds() {
    local role="$1" idx="$2" table="$3" rounds="$4"
    sql_as "$role" << SQL
SET client_min_messages = warning;
BEGIN;
DO \$body\$
BEGIN
    FOR n IN 1..${rounds} LOOP
        INSERT INTO ${table} (body)
        SELECT format('doc %s round %s filler filler filler', i, n)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('${idx}');
    END LOOP;
END
\$body\$;
COMMIT;
SQL
}

create_owned_table() {
    local table="$1" idx="$2" owner="${3:-app_owner}"
    sql_super << SQL
SET client_min_messages = warning;
CREATE TABLE ${table} (id serial PRIMARY KEY, body text);
CREATE INDEX ${idx} ON ${table}
    USING bm25(body) WITH (text_config = 'english');
ALTER TABLE ${table} OWNER TO ${owner};
ALTER INDEX ${idx} OWNER TO ${owner};
SQL
}

# ---------------------------------------------------------------------
# Test 1: PRE_COMMIT gating
# ---------------------------------------------------------------------

test_atomicity() {
    log "=== Test: PRE_COMMIT gating ==="
    create_owned_table t_atomic t_atomic_idx
    local label
    label=$(label_for t_atomic_idx)

    sql_as app_owner << SQL >/dev/null
SET client_min_messages = warning;
BEGIN;
    DO \$body\$
    BEGIN
        FOR n IN 1..2 LOOP
            INSERT INTO t_atomic (body)
            SELECT format(
                'rollback round %s doc %s filler filler filler', n, i)
            FROM generate_series(1, 20) i;
            PERFORM bm25_spill_index('t_atomic_idx');
        END LOOP;
    END
    \$body\$;
ROLLBACK;
SQL
    local rollback_count
    rollback_count=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "rollback enqueues nothing" "0" "$rollback_count"

    assert_eq "rolled-back physical spills remain discoverable" "t" \
        "$(sql_super -c \
           "SELECT bm25_needs_compaction('t_atomic_idx'::regclass);")"
    assert_eq "backstop compacts rolled-back physical spills" "1" \
        "$(sql_as textsearch_compactor -c \
           "SELECT bm25_compact_pending();")"
    assert_eq "rollback backstop clears compaction debt" "f" \
        "$(sql_super -c \
           "SELECT bm25_needs_compaction('t_atomic_idx'::regclass);")"

    spill_rounds app_owner t_atomic_idx t_atomic 2
    local commit_count
    commit_count=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "commit enqueues exactly one instance" "1" "$commit_count"

    local id
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}';")
    local st
    st=$(wait_for_instance "$id" 30)
    assert_eq "committed spill instance completes" "completed" "$st"
}

# ---------------------------------------------------------------------
# Test 2: a direct request uses an independent transaction
# ---------------------------------------------------------------------

test_independent_request() {
    log "=== Test: request transaction is independent ==="
    create_owned_table t_independent t_independent_idx
    local label
    label=$(label_for t_independent_idx)

    local id
    id=$(sql_as app_owner << SQL
BEGIN;
SELECT public.bm25_request_compaction('t_independent_idx');
ROLLBACK;
SQL
)

    local request_count
    request_count=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "direct request survives caller rollback" \
        "1" "$request_count"

    assert_eq "direct request returns persisted instance id" \
        "$id" "$(sql_super -c \
            "SELECT id FROM df.instances WHERE label = '${label}';")"

    local st
    st=$(wait_for_instance "$id" 30)
    assert_eq "independent request completes" "completed" "$st"
}

# ---------------------------------------------------------------------
# Test 3: PREPARE flushes pending requests
# ---------------------------------------------------------------------

test_prepared_transaction() {
    log "=== Test: prepared transaction dispatch ==="
    create_owned_table t_prepared t_prepared_idx
    local label
    label=$(label_for t_prepared_idx)

    # The prepared transaction adds well over 50 terms. A one-row transaction
    # on the reused backend does not, so its row must remain in the memtable.
    sql_super -c "ALTER ROLE app_owner SET \
pg_textsearch.bulk_load_threshold = 50;" >/dev/null
    local reuse_spill
    reuse_spill=$(sql_as app_owner <<'SQL'
\o /dev/null
BEGIN;
INSERT INTO t_prepared (body)
SELECT 'prepared first ' || i || ' filler filler filler'
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_prepared_idx');
INSERT INTO t_prepared (body)
SELECT 'prepared second ' || i || ' filler filler filler'
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_prepared_idx');
PREPARE TRANSACTION 'bm25_compaction_prepare';
\o
INSERT INTO t_prepared (body) VALUES ('prepared backend reuse');
SELECT bm25_spill_index('t_prepared_idx') IS NOT NULL;
\o /dev/null
COMMIT PREPARED 'bm25_compaction_prepare';
SQL
)
    sql_super -c "ALTER ROLE app_owner RESET \
pg_textsearch.bulk_load_threshold;" >/dev/null
    assert_eq "PREPARE resets counters before backend reuse" \
        "t" "$reuse_spill"

    local id st
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    if [ -z "$id" ]; then
        error "ASSERTION FAILED: PREPARE did not dispatch a durable task"
    fi
    st=$(wait_for_instance "$id" 30)
    assert_eq "prepared transaction instance completes" "completed" "$st"
}

# ---------------------------------------------------------------------
# Test 4: temporary indexes compact inline
# ---------------------------------------------------------------------

test_temp_index() {
    log "=== Test: temporary index stays backend-local ==="
    local direct_count_before direct_count_after
    direct_count_before=$(sql_super -c "SELECT count(*) FROM df.instances;")
    if sql_as app_owner <<'SQL' >/dev/null 2>&1
CREATE TEMP TABLE t_temp_direct (id serial PRIMARY KEY, body text);
CREATE INDEX t_temp_direct_idx ON t_temp_direct
    USING bm25(body) WITH (text_config = 'english');
SELECT public.bm25_request_compaction('t_temp_direct_idx');
SQL
    then
        error "direct wrapper call accepted a temporary index"
    fi
    direct_count_after=$(sql_super -c "SELECT count(*) FROM df.instances;")
    assert_eq "rejected temporary target creates no durable task" \
        "$direct_count_before" "$direct_count_after"

    local output temp_oid temp_compacted task_count
    output=$(sql_as app_owner <<'SQL'
CREATE TEMP TABLE t_temp (id serial PRIMARY KEY, body text);
CREATE INDEX t_temp_idx ON t_temp
    USING bm25(body) WITH (text_config = 'english');
SELECT 't_temp_idx'::regclass::oid;
INSERT INTO t_temp (body)
SELECT 'temp first ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_temp_idx') IS NOT NULL;
INSERT INTO t_temp (body)
SELECT 'temp second ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_temp_idx') IS NOT NULL;
SELECT NOT bm25_needs_compaction('t_temp_idx'::regclass);
SQL
)
    temp_oid=$(printf '%s\n' "$output" | sed -n '1p')
    temp_compacted=$(printf '%s\n' "$output" | sed -n '4p')
    assert_eq "temporary index compacts inline" "t" "$temp_compacted"

    task_count=$(sql_super -c "SELECT count(*) FROM df.instances
        WHERE label = 'bm25-compact-${temp_oid}';")
    assert_eq "temporary index creates no durable task" "0" "$task_count"
}

# ---------------------------------------------------------------------
# Test 5: hostile writer search_path cannot redirect casts or operators
# ---------------------------------------------------------------------

test_hostile_search_path() {
    log "=== Test: hostile search_path resolution ==="
    sql_super -c "ALTER ROLE app_owner IN DATABASE ${TEST_DB}
        SET search_path =
            hostile_path, pg_catalog, public, ${EXT_SCHEMA}, df;"
    create_owned_table t_hostile_path t_hostile_path_idx
    local label
    label=$(label_for t_hostile_path_idx)

    spill_rounds app_owner t_hostile_path_idx t_hostile_path 2

    local id
    id=$(sql_super -c "SELECT id FROM df.instances
        WHERE label = '${label}' ORDER BY created_at DESC LIMIT 1;")
    if [ -z "$id" ]; then
        error "ASSERTION FAILED: hostile search_path prevented dispatch"
    fi

    local qualified_dsl
    qualified_dsl=$(sql_super -c "SELECT count(*) = 2
        AND bool_and(query ~ (
            '^SELECT ${EXT_SCHEMA}\\.bm25_' ||
            '(compact_step|needs_compaction)_if_current\\(' ||
            '[0-9]+::pg_catalog\\.oid, ' ||
            '[0-9]+::pg_catalog\\.oid, ' ||
            '[0-9]+::pg_catalog\\.oid, ' ||
            '[0-9]+::pg_catalog\\.oid\\)$'))
        FROM df.nodes
        WHERE instance_id = '${id}'
          AND node_type = 'SQL';")
    assert_eq "worker SQL schema-qualifies built-in casts" \
        "t" "$qualified_dsl"
    assert_eq "hostile-search-path request completes" "completed" \
        "$(wait_for_instance "$id" 30)"

    sql_super -c "ALTER ROLE app_owner IN DATABASE ${TEST_DB}
        RESET search_path;"
}

# ---------------------------------------------------------------------
# Test 6: a rolled-back drop remains repairable
# ---------------------------------------------------------------------

test_drop_rollback() {
    log "=== Test: dropped index rollback remains repairable ==="
    create_owned_table t_drop_rollback t_drop_rollback_idx
    local label
    label=$(label_for t_drop_rollback_idx)

    sql_as app_owner <<'SQL' >/dev/null
BEGIN;
INSERT INTO t_drop_rollback (body)
SELECT 'drop rollback first ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_drop_rollback_idx');
INSERT INTO t_drop_rollback (body)
SELECT 'drop rollback second ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_drop_rollback_idx');
DROP INDEX t_drop_rollback_idx;
ROLLBACK;
SQL

    assert_eq "rolled-back drop dispatches no task" "0" \
        "$(sql_super -c "SELECT count(*) FROM df.instances
            WHERE label = '${label}';")"
    assert_eq "rolled-back drop retains physical compaction debt" "t" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
            't_drop_rollback_idx'::regclass);")"
    sql_as textsearch_compactor -c \
        "SELECT bm25_compact_pending();" >/dev/null
    assert_eq "backstop repairs rolled-back drop" "f" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
            't_drop_rollback_idx'::regclass);")"
}

# ---------------------------------------------------------------------
# Test 7: it actually compacts
# ---------------------------------------------------------------------

test_actually_compacts() {
    log "=== Test: background mode actually compacts ==="
    create_owned_table t_actual t_actual_idx
    local label
    label=$(label_for t_actual_idx)

    local l0_before
    l0_before=$(sql_as app_owner <<'SQL'
SET client_min_messages = warning;
BEGIN;
DO $body$
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO t_actual (body)
        SELECT format(
            'doc %s round %s filler filler filler', i, n)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('t_actual_idx');
    END LOOP;
END
$body$;
SELECT (bm25_level_counts('t_actual_idx'::regclass))[1];
COMMIT;
SQL
)
    if [ "$l0_before" -lt 2 ]; then
        error "ASSERTION FAILED: pre-COMMIT L0 sample did not capture \
both physical spills (L0 = ${l0_before})"
    fi
    log "PASS: physical L0 was sampled before COMMIT released the callback"

    local id
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}';")

    local st
    st=$(wait_for_instance "$id" 30)
    assert_eq "compaction instance completes" "completed" "$st"

    local l0_after
    l0_after=$(sql_super -c \
        "SELECT (bm25_level_counts('t_actual_idx'::regclass))[1];")

    # Never assert only "completed" for a compaction case: that alone
    # passes vacuously if the worker session never saw the lowered
    # threshold (the GUC-scope trap). Require the level counts to have
    # actually moved.
    if [ "$l0_after" -ge "$l0_before" ]; then
        error "ASSERTION FAILED: L0 count did not drop \
(before=${l0_before}, after=${l0_after})"
    fi
    log "PASS: L0 segment count dropped (${l0_before} -> ${l0_after})"
}

# ---------------------------------------------------------------------
# Test 8: write latency, inline vs background
# ---------------------------------------------------------------------

time_rounds() {
    local mode="$1" idx="$2" table="$3" rounds="$4"
    set_guc pg_textsearch.compaction_mode "$mode"
    local start end
    start=$(date +%s.%N)
    spill_rounds app_owner "$idx" "$table" "$rounds"
    end=$(date +%s.%N)
    awk "BEGIN { printf \"%.3f\", ${end} - ${start} }"
}

test_write_latency() {
    log "=== Test: write latency, inline vs background ==="
    create_owned_table t_latency_inline t_latency_inline_idx
    create_owned_table t_latency_bg t_latency_bg_idx
    local rounds
    rounds=$(scaled_count 10)

    local inline_seconds
    inline_seconds=$(time_rounds inline t_latency_inline_idx \
        t_latency_inline "$rounds")
    local bg_seconds
    bg_seconds=$(time_rounds background t_latency_bg_idx \
        t_latency_bg "$rounds")

    # Restore the mode every other test relies on.
    set_guc pg_textsearch.compaction_mode background

    log "RESULT: ${rounds} spill rounds, inline=${inline_seconds}s \
background=${bg_seconds}s"

    # A meaningful, non-flaky assertion: both numbers must actually have
    # been measured (not empty, not zero/negative) -- the comparison
    # itself is recorded above for human review rather than asserted,
    # since relative timing under shared-CI load is inherently noisy.
    for pair in "inline:${inline_seconds}" "background:${bg_seconds}"; do
        local name="${pair%%:*}" val="${pair#*:}"
        if ! awk "BEGIN { exit !(${val} > 0) }"; then
            error "ASSERTION FAILED: ${name} duration not a positive \
number (got '${val}')"
        fi
    done
    log "PASS: both inline and background durations were measured"

    # Let the background instance for t_latency_bg finish draining so it
    # does not linger into later log-grep-based assertions.
    local label id
    label=$(label_for t_latency_bg_idx)
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    [ -n "$id" ] && wait_for_instance "$id" 30 >/dev/null
}

# ---------------------------------------------------------------------
# Test 9: failure path
# ---------------------------------------------------------------------

test_failure_path() {
    log "=== Test: failure path (ownership revoked) ==="
    create_owned_table t_failure t_failure_idx
    local label
    label=$(label_for t_failure_idx)

    # bm25_compact_step() gates on object_ownercheck(); revoking the
    # compactor's inherited membership in app_owner makes that check
    # fail inside the worker's own session, with no debug GUC needed.
    sql_super -c "REVOKE app_owner FROM textsearch_compactor;"

    spill_rounds app_owner t_failure_idx t_failure 2
    local id st
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    st=$(wait_for_instance "$id" 30)
    assert_eq "instance fails without ownership" "failed" "$st"

    local failure_result
    failure_result=$(sql_super -c "SELECT result #>> '{}'
        FROM df.nodes
        WHERE instance_id = '${id}'
          AND node_type = 'SQL'
          AND status = 'failed'
        ORDER BY updated_at DESC LIMIT 1;")
    if [[ "$failure_result" != \
*"must be owner of index t_failure_idx"* ]]; then
        error "ASSERTION FAILED: failed SQL node diagnostic was not \
persisted in df.nodes.result (result='${failure_result}')"
    fi
    log "PASS: failed SQL node diagnostic persisted in df.nodes.result"

    local l0_unchanged
    l0_unchanged=$(sql_super -c \
        "SELECT (bm25_level_counts('t_failure_idx'::regclass))[1];")
    if [ "$l0_unchanged" -lt 2 ]; then
        error "ASSERTION FAILED: a failed compact_step must not have \
merged anything (L0 = ${l0_unchanged})"
    fi
    log "PASS: failed instance left level counts untouched (L0 = \
${l0_unchanged})"

    # Restore ownership; the next spill self-heals (README, "Known
    # limitations: No retry" -- next-spill retry is layer 1).
    sql_super -c "GRANT app_owner TO textsearch_compactor \
WITH INHERIT TRUE, SET FALSE;"

    spill_rounds app_owner t_failure_idx t_failure 1
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    st=$(wait_for_instance "$id" 30)
    assert_eq "next instance succeeds after re-grant" "completed" "$st"

    local l0_after
    l0_after=$(sql_super -c \
        "SELECT (bm25_level_counts('t_failure_idx'::regclass))[1];")
    if [ "$l0_after" -ge "$l0_unchanged" ]; then
        error "ASSERTION FAILED: level counts did not change on the \
successful retry (still ${l0_after})"
    fi
    log "PASS: retry actually compacted (L0 ${l0_unchanged} -> \
${l0_after})"
}

# ---------------------------------------------------------------------
# Test 10: cascade splits across transactions
# ---------------------------------------------------------------------

test_cascade() {
    log "=== Test: cascade splits across transactions ==="
    create_owned_table t_cascade t_cascade_idx
    local label
    label=$(label_for t_cascade_idx)

    # 8 L0 segments at segments_per_level=2 cascades L0 -> L1 -> L2 -> L3
    # over 7 df.loop iterations (verified interactively: {8,0,...} ->
    # {0,0,0,1,0,0,0,0}, execution_id 1..7 in the server log).
    spill_rounds app_owner t_cascade_idx t_cascade 8

    local id
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")

    # Wait until the cascade is actually running (it starts almost
    # immediately), then fire a concurrent writer against the SAME index
    # while the cascade is still in flight. Each compact_step only holds
    # the per-index LW_EXCLUSIVE for the duration of one merge batch and
    # releases it between df.loop iterations (~1s apart, 7 iterations),
    # so this commit should land in well under a second -- proving the
    # writer was not blocked for the whole ~7s+ cascade.
    local w=0
    local st="pending"
    while [ "$w" -lt 10 ]; do
        st=$(sql_super -c \
            "SELECT status FROM df.instances WHERE id = '${id}';")
        [ "$st" = "running" ] && break
        sleep 0.3
        w=$((w + 1))
    done

    local concurrent_start concurrent_end
    concurrent_start=$(date +%s.%N)
    sql_as app_owner << SQL >/dev/null
SET client_min_messages = warning;
INSERT INTO t_cascade (body)
SELECT 'concurrent doc ' || i || ' filler filler filler'
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_cascade_idx');
SQL
    concurrent_end=$(date +%s.%N)

    local status_right_after
    status_right_after=$(sql_super -c \
        "SELECT status FROM df.instances WHERE id = '${id}';")

    local concurrent_seconds
    concurrent_seconds=$(awk \
        "BEGIN { printf \"%.3f\", ${concurrent_end} - ${concurrent_start} }")
    log "Concurrent writer commit took ${concurrent_seconds}s; cascade \
status right after was '${status_right_after}'"

    if [ "$status_right_after" = "completed" ] \
        || [ "$status_right_after" = "failed" ]; then
        error "ASSERTION FAILED: concurrent writer only committed after \
the cascade had already finished -- this proves nothing about \
non-blocking behavior"
    fi
    log "PASS: a concurrent writer committed while the cascade instance \
was still '${status_right_after}' (not blocked for the whole cascade)"

    local final_status
    final_status=$(wait_for_instance "$id" 60)
    assert_eq "cascade instance completes" "completed" "$final_status"

    local generations
    generations=$(count_generations "$id")
    if [ "$generations" -le 1 ]; then
        error "ASSERTION FAILED: expected more than one node execution \
for the cascade, server log shows ${generations}"
    fi
    log "PASS: cascade recorded ${generations} distinct node executions \
(execution_id generations) in the server log"

    local level_counts
    level_counts=$(sql_super -c \
        "SELECT bm25_level_counts('t_cascade_idx'::regclass)::text;")
    log "Final level counts after cascade: ${level_counts}"
    # {0,...} at L0/L1/L2 with something promoted at L3+ proves the
    # cascade crossed more than one level, not just a single L0->L1 hop.
    local l3_or_higher
    l3_or_higher=$(sql_super -c \
        "SELECT (bm25_level_counts('t_cascade_idx'::regclass))[4] > 0;")
    assert_true "cascade promoted a segment to L3 or higher" \
        "$l3_or_higher"
}

# ---------------------------------------------------------------------
# Test 11: permissions
# ---------------------------------------------------------------------

test_permissions() {
    log "=== Test: writer permissions (EXECUTE-only) ==="

    sql_super -c "CREATE ROLE other_owner LOGIN;"
    sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
        -v index_owner=other_owner >/dev/null
    create_owned_table t_other t_other_idx other_owner

    sql_as app_writer <<'SQL'
DO $body$
BEGIN
    BEGIN
        PERFORM public.bm25_request_compaction('t_other_idx');
        RAISE EXCEPTION
            'app_writer requested compaction for another owner';
    EXCEPTION
        WHEN insufficient_privilege THEN
            NULL;
    END;
END
$body$;
SQL
    log "PASS: writer cannot request compaction without heap INSERT"

    create_owned_table t_perm t_perm_idx
    sql_super << SQL
GRANT INSERT, SELECT ON t_perm TO app_writer;
GRANT USAGE ON SEQUENCE t_perm_id_seq TO app_writer;
SQL
    local label
    label=$(label_for t_perm_idx)

    # Confirm app_writer genuinely has no df privileges at all: a direct
    # df.* call must fail with a permission error.
    if sql_as app_writer -c "SELECT df.start('SELECT 1');" \
        >/dev/null 2>&1; then
        error "ASSERTION FAILED: app_writer must NOT be able to call \
df.start() directly (it holds no df schema privileges)"
    fi
    log "PASS: app_writer cannot call df.* directly (permission denied)"

    # app_writer is not the index owner, so it cannot call
    # bm25_spill_index() directly either -- it must rely on ordinary
    # auto-spill during a plain INSERT. tp_aminsert's auto-spill check
    # re-fires per tuple, so an overly low page threshold (e.g. 1) makes
    # a single large INSERT spill dozens of times over (each spill
    # resets the chain and immediately starts refilling it), producing
    # a pile of tiny L0 segments whose df.loop cascade does not converge
    # inside any reasonable timeout. threshold=4 was measured to cross
    # once per 400-row transaction. Two transactions therefore reach
    # segments_per_level=2 and make the second commit request
    # compaction.
    set_guc pg_textsearch.memtable_pages_threshold '4'

    local batch
    for batch in 1 2; do
        sql_as app_writer << SQL
SET client_min_messages = warning;
BEGIN;
INSERT INTO t_perm (body)
SELECT 'writer batch ${batch} doc ' || i ||
       ' filler filler filler filler filler'
FROM generate_series(1, 400) i;
COMMIT;
SQL
    done

    reset_guc pg_textsearch.memtable_pages_threshold

    local id submitted_by
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    if [ -z "$id" ]; then
        error "ASSERTION FAILED: no compaction instance was enqueued \
for a writer that only holds EXECUTE on bm25_request_compaction"
    fi
    log "PASS: unprivileged writer's insert still enqueued a task \
(${id})"

    submitted_by=$(sql_super -c \
        "SELECT submitted_by::text FROM df.instances WHERE id = '${id}';")
    assert_eq "instance is attributed to textsearch_compactor" \
        "textsearch_compactor" "$submitted_by"

    local st
    st=$(wait_for_instance "$id" 60)
    assert_eq "permission-path instance completes" "completed" "$st"
    assert_eq "permission-path compaction clears L0" "f" \
        "$(sql_super -c \
           "SELECT bm25_needs_compaction('t_perm_idx'::regclass);")"
}

# ---------------------------------------------------------------------
# Test 12: partition ancestors authorize physical leaf indexes
# ---------------------------------------------------------------------

test_partition_permissions() {
    log "=== Test: partition-ancestor writer permissions ==="

    sql_super -c "GRANT CREATE ON SCHEMA public TO app_owner;"
    sql_as app_owner <<'SQL'
CREATE TABLE t_auth_parent (
    part_key integer,
    body text
) PARTITION BY RANGE (part_key);
CREATE TABLE t_auth_leaf PARTITION OF t_auth_parent
    FOR VALUES FROM (0) TO (10);
CREATE INDEX t_auth_parent_idx ON t_auth_parent
    USING bm25(body) WITH (text_config = 'english');

CREATE TABLE t_auth_root (
    region integer,
    bucket integer,
    body text
) PARTITION BY RANGE (region);
CREATE TABLE t_auth_middle PARTITION OF t_auth_root
    FOR VALUES FROM (0) TO (10)
    PARTITION BY RANGE (bucket);
CREATE TABLE t_auth_deep_leaf PARTITION OF t_auth_middle
    FOR VALUES FROM (0) TO (10);
CREATE INDEX t_auth_root_idx ON t_auth_root
    USING bm25(body) WITH (text_config = 'english');
SQL
    sql_super <<'SQL'
GRANT INSERT ON t_auth_parent TO app_writer;
GRANT INSERT ON t_auth_root TO app_writer;
SQL

    local leaf_index deep_leaf_index
    leaf_index=$(sql_super -c "SELECT i.indexrelid::regclass::text \
FROM pg_catalog.pg_index i \
JOIN pg_catalog.pg_class c ON c.oid = i.indexrelid \
JOIN pg_catalog.pg_am am ON am.oid = c.relam \
WHERE i.indrelid = 't_auth_leaf'::regclass \
  AND am.amname = 'bm25';")
    deep_leaf_index=$(sql_super -c "SELECT i.indexrelid::regclass::text \
FROM pg_catalog.pg_index i \
JOIN pg_catalog.pg_class c ON c.oid = i.indexrelid \
JOIN pg_catalog.pg_am am ON am.oid = c.relam \
WHERE i.indrelid = 't_auth_deep_leaf'::regclass \
  AND am.amname = 'bm25';")

    local leaf_insert deep_leaf_insert
    leaf_insert=$(sql_super -c "SELECT has_table_privilege(\
'app_writer', 't_auth_leaf', 'INSERT');")
    deep_leaf_insert=$(sql_super -c "SELECT has_table_privilege(\
'app_writer', 't_auth_deep_leaf', 'INSERT');")
    assert_eq "writer lacks direct INSERT on leaf" "f" "$leaf_insert"
    assert_eq "writer lacks direct INSERT on deep leaf" \
        "f" "$deep_leaf_insert"

    local parent_allowed=f parent_instance=""
    local multilevel_allowed=f multilevel_instance=""
    if parent_instance=$(sql_as app_writer -c "SELECT \
public.bm25_request_compaction('${leaf_index}'::regclass);" \
        2>/dev/null); then
        parent_allowed=t
    fi
    if multilevel_instance=$(sql_as app_writer -c "SELECT \
public.bm25_request_compaction('${deep_leaf_index}'::regclass);" \
        2>/dev/null); then
        multilevel_allowed=t
    fi

    if [ "$parent_allowed" != "t" ] \
        || [ "$multilevel_allowed" != "t" ]; then
        error "ASSERTION FAILED: partition ancestor INSERT did not \
authorize physical leaf indexes (parent-only=${parent_allowed}, \
multilevel=${multilevel_allowed})"
    fi
    log "PASS: parent-only INSERT authorizes leaf index ${leaf_index}"
    log "PASS: root-only INSERT authorizes deep leaf index \
${deep_leaf_index}"

    wait_for_instance "$parent_instance" 30 >/dev/null
    wait_for_instance "$multilevel_instance" 30 >/dev/null
    sql_super -c "DROP TABLE t_auth_parent, t_auth_root CASCADE;" \
        >/dev/null
}

# ---------------------------------------------------------------------
# Test 13: stale durable targets terminate safely
# ---------------------------------------------------------------------

test_stale_targets() {
    log "=== Test: dropped and reindexed durable targets ==="
    create_owned_table t_stale_drop t_stale_drop_idx
    create_owned_table t_stale_reindex t_stale_reindex_idx

    local blocker_id
    blocker_id=$(sql_as textsearch_compactor -c \
        "SELECT df.start(
            'SELECT pg_catalog.pg_sleep(15)',
            label => 'bm25-stale-target-blocker');")
    wait_for_sleep_node 15

    local drop_id reindex_id
    drop_id=$(sql_as app_owner -c \
        "SELECT public.bm25_request_compaction('t_stale_drop_idx');")
    reindex_id=$(sql_as app_owner -c \
        "SELECT public.bm25_request_compaction('t_stale_reindex_idx');")

    local numeric_dsl
    numeric_dsl=$(sql_super -c "SELECT count(*) = 4
        AND bool_and(query ~ (
            '^SELECT ${EXT_SCHEMA}\\.bm25_' ||
            '(compact_step|needs_compaction)_if_current\\(' ||
            '[0-9]+::pg_catalog\\.oid, ' ||
            '[0-9]+::pg_catalog\\.oid, ' ||
            '[0-9]+::pg_catalog\\.oid, ' ||
            '[0-9]+::pg_catalog\\.oid\\)$'))
        FROM df.nodes
        WHERE instance_id IN ('${drop_id}', '${reindex_id}')
          AND node_type = 'SQL';")
    assert_eq "durable task DSL contains only fixed helper calls and OIDs" \
        "t" "$numeric_dsl"

    sql_as app_owner -c "DROP TABLE t_stale_drop CASCADE;" >/dev/null
    create_owned_table t_stale_drop t_stale_drop_idx

    sql_as app_owner -c \
        "REINDEX INDEX t_stale_reindex_idx;" >/dev/null

    set_guc pg_textsearch.compaction_mode 'off'
    spill_rounds app_owner t_stale_drop_idx t_stale_drop 2
    spill_rounds app_owner t_stale_reindex_idx t_stale_reindex 2
    set_guc pg_textsearch.compaction_mode 'background'

    local blocker_status drop_status reindex_status
    blocker_status=$(wait_for_instance "$blocker_id" 30)
    assert_eq "stale-target blocker completes" "completed" "$blocker_status"
    drop_status=$(wait_for_instance "$drop_id" 30)
    reindex_status=$(wait_for_instance "$reindex_id" 30)
    assert_eq "dropped target task terminates successfully" \
        "completed" "$drop_status"
    assert_eq "reindexed target task terminates successfully" \
        "completed" "$reindex_status"

    assert_eq "dropped target task leaves replacement untouched" "t" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
            't_stale_drop_idx'::regclass);")"
    assert_eq "reindexed target task leaves replacement untouched" "t" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
            't_stale_reindex_idx'::regclass);")"
}

# ---------------------------------------------------------------------
# Test 14: partition cascade removes pending leaf requests
# ---------------------------------------------------------------------

test_partition_drop_cleanup() {
    log "=== Test: partition cascade clears pending leaf requests ==="
    sql_as app_owner <<'SQL'
CREATE TABLE t_drop_partitioned (part_key integer, body text)
PARTITION BY RANGE (part_key);
CREATE TABLE t_drop_partitioned_a PARTITION OF t_drop_partitioned
    FOR VALUES FROM (0) TO (10);
CREATE TABLE t_drop_partitioned_b PARTITION OF t_drop_partitioned
    FOR VALUES FROM (10) TO (20);
CREATE INDEX t_drop_partitioned_idx ON t_drop_partitioned
    USING bm25(body) WITH (text_config = 'english');
SQL

    local leaf_a leaf_b oid_a oid_b
    leaf_a=$(sql_super -c "SELECT i.indexrelid::regclass::text
        FROM pg_catalog.pg_index i
        JOIN pg_catalog.pg_class c ON c.oid = i.indexrelid
        JOIN pg_catalog.pg_am am ON am.oid = c.relam
        WHERE i.indrelid = 't_drop_partitioned_a'::regclass
          AND am.amname = 'bm25';")
    leaf_b=$(sql_super -c "SELECT i.indexrelid::regclass::text
        FROM pg_catalog.pg_index i
        JOIN pg_catalog.pg_class c ON c.oid = i.indexrelid
        JOIN pg_catalog.pg_am am ON am.oid = c.relam
        WHERE i.indrelid = 't_drop_partitioned_b'::regclass
          AND am.amname = 'bm25';")
    oid_a=$(sql_super -c "SELECT '${leaf_a}'::regclass::oid;")
    oid_b=$(sql_super -c "SELECT '${leaf_b}'::regclass::oid;")

    sql_as app_owner << SQL >/dev/null
INSERT INTO t_drop_partitioned_a (part_key, body)
SELECT 1, 'cascade seed a ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('${leaf_a}');
INSERT INTO t_drop_partitioned_b (part_key, body)
SELECT 11, 'cascade seed b ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('${leaf_b}');
SQL

    sql_as app_owner << SQL >/dev/null
BEGIN;
INSERT INTO t_drop_partitioned_a (part_key, body)
SELECT 1, 'cascade a ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('${leaf_a}');
INSERT INTO t_drop_partitioned_b (part_key, body)
SELECT 11, 'cascade b ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('${leaf_b}');
DROP TABLE t_drop_partitioned CASCADE;
COMMIT;
SQL

    assert_eq "partition cascade dispatches no leaf tasks" "0" \
        "$(sql_super -c "SELECT count(*) FROM df.instances
            WHERE label IN (
                'bm25-compact-${oid_a}', 'bm25-compact-${oid_b}');")"
}

# ---------------------------------------------------------------------
# Test 15: ownership drift remains visible to the repair sweep
# ---------------------------------------------------------------------

test_ownership_drift_sweep() {
    log "=== Test: ownership drift warning and sweep isolation ==="
    sql_super -c "CREATE ROLE ownership_drift_owner LOGIN;"
    create_owned_table t_ownership_drift t_ownership_drift_idx
    create_owned_table t_ownership_eligible t_ownership_eligible_idx

    set_guc pg_textsearch.compaction_mode 'off'
    spill_rounds app_owner t_ownership_drift_idx t_ownership_drift 2
    spill_rounds app_owner t_ownership_eligible_idx t_ownership_eligible 2
    sql_super -c "ALTER TABLE t_ownership_drift \
OWNER TO ownership_drift_owner;"

    local candidates
    candidates=$(sql_as textsearch_compactor -c "SELECT
        NOT EXISTS (
            SELECT 1 FROM bm25_indexes_needing_compaction() candidate(idx)
            WHERE candidate.idx = 't_ownership_drift_idx'::regclass)
        AND EXISTS (
            SELECT 1 FROM bm25_indexes_needing_compaction() candidate(idx)
            WHERE candidate.idx = 't_ownership_eligible_idx'::regclass);")
    assert_eq "public candidate API preserves owner visibility" \
        "t" "$candidates"

    local sweep_output
    sweep_output=$(sql_as textsearch_compactor -c \
        "SELECT bm25_compact_pending();" 2>&1)
    if [[ "$sweep_output" != \
*"bm25: compaction of public.t_ownership_drift_idx failed:"* ]]; then
        error "ASSERTION FAILED: ownership drift warning did not name \
t_ownership_drift_idx (output='${sweep_output}')"
    fi
    log "PASS: ownership drift warning names the inaccessible index"

    assert_eq "ownership-drift debt remains pending" "t" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
            't_ownership_drift_idx'::regclass);")"
    assert_eq "sweep continues to a later eligible index" "f" \
        "$(sql_super -c "SELECT bm25_needs_compaction(
            't_ownership_eligible_idx'::regclass);")"

    sql_super -c "ALTER TABLE t_ownership_drift OWNER TO app_owner;"
    sql_as textsearch_compactor -c \
        "SELECT bm25_compact_pending();" >/dev/null
    sql_super -c "DROP TABLE t_ownership_drift, \
t_ownership_eligible CASCADE;"
    sql_super -c "DROP ROLE ownership_drift_owner;"
    set_guc pg_textsearch.compaction_mode 'background'
}

# ---------------------------------------------------------------------
# Test 16: backstop (run LAST -- its per-minute sweep would otherwise
# compact every pending index in the database, corrupting the carefully
# staged segment counts the other tests depend on).
# ---------------------------------------------------------------------

test_backstop() {
    log "=== Test: backstop sweep (compaction_mode = 'off') ==="
    create_owned_table t_backstop t_backstop_idx
    local label
    label=$(label_for t_backstop_idx)

    set_guc pg_textsearch.compaction_mode 'off'

    spill_rounds app_owner t_backstop_idx t_backstop 2

    local writer_side_count
    writer_side_count=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "compaction_mode=off enqueues nothing from the write path" \
        "0" "$writer_side_count"

    local l0_before
    l0_before=$(sql_super -c \
        "SELECT (bm25_level_counts('t_backstop_idx'::regclass))[1];")
    if [ "$l0_before" -lt 2 ]; then
        error "ASSERTION FAILED: setup did not leave the index over \
threshold (L0 = ${l0_before})"
    fi

    sql_super <<'SQL'
CREATE TABLE public.backstop_trap_calls (calls integer NOT NULL);
INSERT INTO public.backstop_trap_calls VALUES (0);
CREATE FUNCTION public.bm25_compact_pending()
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $trap$
BEGIN
    UPDATE public.backstop_trap_calls SET calls = calls + 1;
    RETURN 0;
END
$trap$;
SQL

    PGOPTIONS="-c search_path=hostile_path,pg_catalog,public,${EXT_SCHEMA},df" \
        sql_as textsearch_compactor -f "${GLUE_DIR}/03_backstop.sql" \
        -v cron='* * * * *' >/dev/null

    local backstop_id backstop_status
    backstop_id=$(sql_super -c \
        "SELECT id FROM df.instances \
         WHERE label = 'bm25-compaction-backstop' \
         ORDER BY created_at DESC LIMIT 1;")
    backstop_status=$(sql_super -c \
        "SELECT status FROM df.instances WHERE id = '${backstop_id}';")
    if [ "$backstop_status" != "running" ] \
        && [ "$backstop_status" != "pending" ]; then
        error "ASSERTION FAILED: backstop instance is not live \
(status=${backstop_status})"
    fi
    log "PASS: long-lived backstop instance ${backstop_id} is \
${backstop_status}"

    PGOPTIONS="-c search_path=hostile_path,pg_catalog,public,${EXT_SCHEMA},df" \
        sql_as textsearch_compactor -f "${GLUE_DIR}/03_backstop.sql" \
        -v cron='* * * * *' >/dev/null

    local live_backstop_count reused_backstop_id
    live_backstop_count=$(sql_super -c "SELECT count(*)
        FROM df.instances
        WHERE label = 'bm25-compaction-backstop'
          AND submitted_by =
                'textsearch_compactor'::pg_catalog.regrole
          AND status IN ('pending', 'running');")
    assert_eq "repeated registration keeps one live backstop" \
        "1" "$live_backstop_count"
    reused_backstop_id=$(sql_super -c "SELECT id
        FROM df.instances
        WHERE label = 'bm25-compaction-backstop'
          AND submitted_by =
                'textsearch_compactor'::pg_catalog.regrole
          AND status IN ('pending', 'running')
        ORDER BY created_at DESC LIMIT 1;")
    assert_eq "repeated registration reuses the canonical backstop" \
        "$backstop_id" "$reused_backstop_id"

    sql_super -c "UPDATE df.instances
        SET label = 'operator-renamed-backstop'
        WHERE id = '${backstop_id}';" >/dev/null
    PGOPTIONS="-c search_path=hostile_path,pg_catalog,public,${EXT_SCHEMA},df" \
        sql_as textsearch_compactor -f "${GLUE_DIR}/03_backstop.sql" \
        -v cron='* * * * *' >/dev/null
    assert_eq "changed observability label still reuses canonical backstop" \
        "$backstop_id" "$(canonical_backstop_ids)"

    local duplicate_id multiple_output canonical_ids canonical_count
    duplicate_id=$(sql_as textsearch_compactor -c "SELECT df.start(
        df.loop(
            df.wait_for_schedule('0 0 1 1 *')
            OPERATOR(pg_catalog.~>)
            'SELECT ${EXT_SCHEMA}.bm25_compact_pending()'),
        label => 'operator-duplicate-backstop');")
    multiple_output="${DATA_DIR}/multiple-backstop.out"
    if PGOPTIONS="\
-c search_path=hostile_path,pg_catalog,public,${EXT_SCHEMA},df" \
        sql_as textsearch_compactor -f "${GLUE_DIR}/03_backstop.sql" \
        -v cron='* * * * *' >"${multiple_output}" 2>&1; then
        error "ASSERTION FAILED: registration accepted multiple live \
canonical backstops (${backstop_id}, ${duplicate_id})"
    fi
    if ! grep -Fq "${backstop_id}" "${multiple_output}" \
        || ! grep -Fq "${duplicate_id}" "${multiple_output}" \
        || ! grep -Fq "Cancel all but one listed instance" \
            "${multiple_output}"; then
        error "ASSERTION FAILED: multiple-backstop failure did not provide \
both IDs and cancellation guidance: $(tr '\n' ' ' <"${multiple_output}")"
    fi
    canonical_ids=$(canonical_backstop_ids)
    canonical_count=$(printf '%s\n' "${canonical_ids}" | grep -c .)
    assert_eq "ambiguous registration creates no third canonical backstop" \
        "2" "${canonical_count}"
    log "PASS: multiple canonical live backstops fail closed with IDs \
${backstop_id} and ${duplicate_id}"
    sql_as textsearch_compactor -c \
        "SELECT df.cancel('${duplicate_id}');" >/dev/null
    assert_eq "duplicate backstop reaches terminal status" "cancelled" \
        "$(wait_for_instance "$duplicate_id" 30)"
    assert_eq "original renamed backstop remains canonical after cleanup" \
        "$backstop_id" "$(canonical_backstop_ids)"

    local waited=0
    local l0_after="$l0_before"
    local trap_calls=0
    while [ "$waited" -lt 130 ]; do
        l0_after=$(sql_super -c \
            "SELECT (bm25_level_counts('t_backstop_idx'::regclass))[1];")
        trap_calls=$(sql_super -c \
            "SELECT calls FROM public.backstop_trap_calls;")
        if [ "$l0_after" -lt "$l0_before" ] || [ "$trap_calls" -gt 0 ]; then
            break
        fi
        sleep 5
        waited=$((waited + 5))
    done

    local bound_query
    bound_query=$(sql_super -c "SELECT count(*) = 1
        AND bool_and(query =
            'SELECT ${EXT_SCHEMA}.bm25_compact_pending()')
        FROM df.nodes
        WHERE instance_id = '${backstop_id}'
          AND query LIKE '%bm25_compact_pending%';")
    if [ "$bound_query" != "t" ] || [ "$trap_calls" != "0" ] \
        || [ "$l0_after" -ge "$l0_before" ]; then
        error "ASSERTION FAILED: backstop did not bind only the extension \
member (bound=${bound_query}, trap_calls=${trap_calls}, \
L0=${l0_before}->${l0_after})"
    fi
    log "PASS: backstop ignored the public trap and compacted through \
${EXT_SCHEMA} \
(L0 ${l0_before} -> ${l0_after}) after waiting ${waited}s"

    sql_as textsearch_compactor -c \
        "SELECT df.cancel('${backstop_id}');" >/dev/null
    assert_eq "cancelled backstop reaches terminal status" "cancelled" \
        "$(wait_for_instance "$backstop_id" 30)"

    PGOPTIONS="-c search_path=hostile_path,pg_catalog,public,${EXT_SCHEMA},df" \
        sql_as textsearch_compactor -f "${GLUE_DIR}/03_backstop.sql" \
        -v cron='* * * * *' >/dev/null
    local replacement_id
    replacement_id=$(sql_super -c "SELECT id
        FROM df.instances
        WHERE label = 'bm25-compaction-backstop'
          AND submitted_by =
                'textsearch_compactor'::pg_catalog.regrole
          AND status IN ('pending', 'running')
        ORDER BY created_at DESC LIMIT 1;")
    if [ -z "$replacement_id" ] || [ "$replacement_id" = "$backstop_id" ]; then
        error "ASSERTION FAILED: terminal backstop was not replaced \
(old=${backstop_id}, replacement=${replacement_id})"
    fi
    log "PASS: terminal backstop ${backstop_id} was replaced by \
${replacement_id}"
    sql_as textsearch_compactor -c \
        "SELECT df.cancel('${replacement_id}');" >/dev/null
}

# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------

setup_test_cluster
test_callback_drop_after_lookup
test_pg_durable_preflight
setup_roles_and_glue
wait_for_durable_worker
test_backstop_canary_failure
test_terminal_backstop_confirmation

test_atomicity
test_independent_request
test_prepared_transaction
test_temp_index
test_hostile_search_path
test_drop_rollback
test_actually_compacts
test_write_latency
test_failure_path
test_cascade
test_permissions
test_partition_permissions
test_stale_targets
test_partition_drop_cleanup
test_ownership_drift_sweep
test_backstop

log "All tests passed!"
exit 0
