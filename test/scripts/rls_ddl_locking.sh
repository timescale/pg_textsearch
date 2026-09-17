#!/bin/bash
#
# Verify that RLS/BM25 DDL serializes before PostgreSQL takes relation locks.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT="${TEST_PORT:-55463}"
TEST_DB=pg_textsearch_rls_locking_test
DATA_DIR="${SCRIPT_DIR}/../tmp_rls_locking_${TEST_PORT}_$$"
LOGFILE="${DATA_DIR}/postgres.log"
SESSION_A_INPUT="${DATA_DIR}/session_a.in"
SESSION_A_OUTPUT="${DATA_DIR}/session_a.out"
SESSION_B_OUTPUT="${DATA_DIR}/session_b.out"
SESSION_C_OUTPUT="${DATA_DIR}/session_c.out"
SESSION_A_PID=
SESSION_B_PID=
SESSION_C_PID=
PARTITION_ORDER_FAILED=0

cleanup() {
    local exit_code=$?

    trap - EXIT INT TERM
    exec 3>&- 2>/dev/null || true

    if [ -n "${SESSION_A_PID}" ] && kill -0 "${SESSION_A_PID}" 2>/dev/null; then
        kill "${SESSION_A_PID}" 2>/dev/null || true
        wait "${SESSION_A_PID}" 2>/dev/null || true
    fi
    if [ -n "${SESSION_B_PID}" ] && kill -0 "${SESSION_B_PID}" 2>/dev/null; then
        kill "${SESSION_B_PID}" 2>/dev/null || true
        wait "${SESSION_B_PID}" 2>/dev/null || true
    fi
    if [ -n "${SESSION_C_PID}" ] && kill -0 "${SESSION_C_PID}" 2>/dev/null; then
        kill "${SESSION_C_PID}" 2>/dev/null || true
        wait "${SESSION_C_PID}" 2>/dev/null || true
    fi
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m fast -w >/dev/null 2>&1 ||
            pg_ctl stop -D "${DATA_DIR}" -m immediate -w >/dev/null 2>&1 ||
            true
    fi
    rm -rf "${DATA_DIR}"
    exit "${exit_code}"
}

trap cleanup EXIT INT TERM

run_value() {
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -tAc "$1"
}

wait_for_true() {
    local query=$1
    local description=$2
    local i

    for i in $(seq 1 200); do
        if [ "$(run_value "${query}")" = "t" ]; then
            return 0
        fi
        sleep 0.05
    done

    echo "Timed out waiting for ${description}" >&2
    return 1
}

start_rls_holder() {
    local table_name=$1

    rm -f "${SESSION_A_INPUT}" "${SESSION_A_OUTPUT}"
    mkfifo "${SESSION_A_INPUT}"
    PGAPPNAME=rls-lock-session-a \
        psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -v ON_ERROR_STOP=1 < "${SESSION_A_INPUT}" \
        > "${SESSION_A_OUTPUT}" 2>&1 &
    SESSION_A_PID=$!
    exec 3> "${SESSION_A_INPUT}"

    printf '%s\n' \
        "SET pg_textsearch.allow_rls = off;" \
        "BEGIN;" \
        "ALTER TABLE ${table_name} ENABLE ROW LEVEL SECURITY;" >&3

    wait_for_true "
        SELECT EXISTS (
            SELECT 1
            FROM pg_stat_activity AS a
            JOIN pg_locks AS l ON l.pid = a.pid
            WHERE a.application_name = 'rls-lock-session-a'
              AND a.state = 'idle in transaction'
              AND l.locktype = 'relation'
              AND l.relation = '${table_name}'::regclass
              AND l.mode = 'AccessExclusiveLock'
              AND l.granted
        );
    " "session A to hold ${table_name}"
}

release_session_a() {
    printf '%s\n' "COMMIT;" "\\q" >&3
    exec 3>&-
    wait "${SESSION_A_PID}"
    SESSION_A_PID=
}

mkdir -p "${DATA_DIR}"
initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
    >/dev/null 2>&1

cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 20
shared_buffers = 128MB
unix_socket_directories = '${DATA_DIR}'
listen_addresses = ''
log_min_messages = warning
shared_preload_libraries = 'pg_textsearch'
EOF

pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null
createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 >/dev/null << 'SQL'
CREATE EXTENSION pg_textsearch;
CREATE TABLE lock_parent (id integer, content text);
CREATE TABLE lock_child () INHERITS (lock_parent);
CREATE TABLE noop_inherit_parent (id integer);
CREATE TABLE noop_inherit_child () INHERITS (noop_inherit_parent);
CREATE TABLE noop_reindex (id integer);
CREATE INDEX noop_reindex_idx ON noop_reindex(id);
CREATE TABLE extension_replace_parent (id integer, content text);
CREATE TABLE extension_replace_child () INHERITS (extension_replace_parent);
SQL

rm -f "${SESSION_A_INPUT}" "${SESSION_A_OUTPUT}"
mkfifo "${SESSION_A_INPUT}"
PGAPPNAME=rls-extension-replace-session-a \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 < "${SESSION_A_INPUT}" \
    > "${SESSION_A_OUTPUT}" 2>&1 &
SESSION_A_PID=$!
exec 3> "${SESSION_A_INPUT}"
printf '%s\n' \
    "BEGIN;" \
    "DROP EXTENSION pg_textsearch CASCADE;" \
    "CREATE EXTENSION pg_textsearch;" >&3

wait_for_true "
    SELECT state = 'idle in transaction'
    FROM pg_stat_activity
    WHERE application_name = 'rls-extension-replace-session-a';
" "session A to replace the extension"

PGAPPNAME=rls-extension-replace-session-b \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = off;
        BEGIN;
        ALTER TABLE extension_replace_parent ENABLE ROW LEVEL SECURITY;
        SELECT pg_sleep(5);
        COMMIT;" \
    > "${SESSION_B_OUTPUT}" 2>&1 &
SESSION_B_PID=$!

wait_for_true "
    SELECT wait_event_type = 'Lock'
    FROM pg_stat_activity
    WHERE application_name = 'rls-extension-replace-session-b';
" "session B to wait for extension replacement"

release_session_a

wait_for_true "
    SELECT wait_event_type = 'Timeout'
    FROM pg_stat_activity
    WHERE application_name = 'rls-extension-replace-session-b';
" "session B to finish RLS enablement"

PGAPPNAME=rls-extension-replace-session-c \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = off;
        CREATE INDEX extension_replace_child_idx
        ON extension_replace_child USING bm25(content)
        WITH (text_config='english');" \
    > "${SESSION_C_OUTPUT}" 2>&1 &
SESSION_C_PID=$!

set +e
wait "${SESSION_B_PID}"
session_b_status=$?
SESSION_B_PID=
wait "${SESSION_C_PID}"
session_c_status=$?
SESSION_C_PID=
set -e

if [ "${session_b_status}" -ne 0 ]; then
    echo "RLS enablement failed after extension replacement" >&2
    cat "${SESSION_B_OUTPUT}" >&2
    exit 1
fi
if [ "${session_c_status}" -eq 0 ]; then
    echo "Extension replacement changed the RLS DDL lock identity" >&2
    exit 1
fi
if ! grep -q "BM25 indexes are not allowed on row-level security" \
    "${SESSION_C_OUTPUT}"; then
    echo "Missing expected post-replacement RLS rejection" >&2
    cat "${SESSION_C_OUTPUT}" >&2
    exit 1
fi

extension_replace_state=$(run_value "
    SELECT relrowsecurity,
           to_regclass('extension_replace_child_idx') IS NULL
    FROM pg_class
    WHERE oid = 'extension_replace_parent'::regclass;
")
if [ "${extension_replace_state}" != "t|t" ]; then
    echo "Extension replacement allowed opposite hierarchy mutations" >&2
    exit 1
fi

rm -f "${SESSION_A_INPUT}" "${SESSION_A_OUTPUT}"
mkfifo "${SESSION_A_INPUT}"
PGAPPNAME=rls-noop-session \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 < "${SESSION_A_INPUT}" \
    > "${SESSION_A_OUTPUT}" 2>&1 &
SESSION_A_PID=$!
exec 3> "${SESSION_A_INPUT}"
printf '%s\n' \
    "SET pg_textsearch.allow_rls = off;" \
    "BEGIN;" \
    "ALTER TABLE IF EXISTS missing_rls_table ENABLE ROW LEVEL SECURITY;" \
    "CREATE TABLE IF NOT EXISTS noop_inherit_child () INHERITS (noop_inherit_parent);" \
    "REINDEX INDEX noop_reindex_idx;" >&3

wait_for_true "
    SELECT state = 'idle in transaction'
    FROM pg_stat_activity
    WHERE application_name = 'rls-noop-session';
" "no-op RLS command to finish"

noop_object_locks=$(run_value "
    SELECT count(*)
    FROM pg_stat_activity AS a
    JOIN pg_locks AS l ON l.pid = a.pid
    WHERE a.application_name = 'rls-noop-session'
      AND l.locktype = 'object'
      AND l.classid = 'pg_extension'::regclass
      AND l.objid = 0
      AND l.granted;
")
if [ "${noop_object_locks}" != "0" ]; then
    echo "No-op or unrelated DDL retained the extension object lock" >&2
    exit 1
fi

release_session_a

PGAPPNAME=rls-multireindex-session \
PGOPTIONS="-c pg_textsearch.allow_rls=off" \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "REINDEX DATABASE ${TEST_DB};" \
    > "${SESSION_A_OUTPUT}" 2>&1

if grep -q "you don't own a lock" "${SESSION_A_OUTPUT}"; then
    echo "Multi-relation REINDEX lost transaction lock ownership" >&2
    cat "${SESSION_A_OUTPUT}" >&2
    exit 1
fi

psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 >/dev/null << 'SQL'
CREATE TABLE upgrade_outer_a (id integer);
CREATE TABLE upgrade_outer_b (id integer);
CREATE TABLE upgrade_nested_a (id integer);
CREATE TABLE upgrade_nested_b (id integer);

CREATE FUNCTION test_nested_policy_upgrade()
RETURNS event_trigger AS $$
BEGIN
    IF current_setting('rls_upgrade_test.action', true)
            IS DISTINCT FROM 'outer' THEN
        RETURN;
    END IF;

    PERFORM set_config('rls_upgrade_test.action', 'running', false);
    IF current_setting('rls_upgrade_test.pause', true) = 'on' THEN
        PERFORM pg_sleep(5);
    END IF;
    PERFORM set_config('pg_textsearch.allow_rls', 'off', false);
    EXECUTE format(
        'ALTER TABLE %I ENABLE ROW LEVEL SECURITY',
        current_setting('rls_upgrade_test.nested_table')
    );
END
$$ LANGUAGE plpgsql;

CREATE EVENT TRIGGER test_nested_policy_upgrade_trigger
ON ddl_command_start
WHEN TAG IN ('ALTER TABLE')
EXECUTE FUNCTION test_nested_policy_upgrade();
SQL

PGAPPNAME=rls-upgrade-session-a \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = on;
        SET rls_upgrade_test.action = 'outer';
        SET rls_upgrade_test.nested_table = 'upgrade_nested_a';
        SET rls_upgrade_test.pause = on;
        ALTER TABLE upgrade_outer_a ENABLE ROW LEVEL SECURITY;" \
    > "${SESSION_A_OUTPUT}" 2>&1 &
SESSION_A_PID=$!

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity AS a
        JOIN pg_locks AS l ON l.pid = a.pid
        WHERE a.application_name = 'rls-upgrade-session-a'
          AND a.wait_event_type = 'Timeout'
          AND l.locktype = 'object'
          AND l.classid = 'pg_extension'::regclass
          AND l.objid = 0
          AND l.mode = 'ShareLock'
          AND l.granted
    );
" "session A to hold the policy lock"

PGAPPNAME=rls-upgrade-session-b \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = on;
        SET rls_upgrade_test.action = 'outer';
        SET rls_upgrade_test.nested_table = 'upgrade_nested_b';
        SET rls_upgrade_test.pause = off;
        ALTER TABLE upgrade_outer_b ENABLE ROW LEVEL SECURITY;" \
    > "${SESSION_B_OUTPUT}" 2>&1 &
SESSION_B_PID=$!

set +e
wait "${SESSION_B_PID}"
session_b_status=$?
SESSION_B_PID=
wait "${SESSION_A_PID}"
session_a_status=$?
SESSION_A_PID=
set -e

if [ "${session_a_status}" -ne 0 ] || [ "${session_b_status}" -eq 0 ]; then
    echo "Nested RLS policy lock upgrade did not fail safely" >&2
    cat "${SESSION_A_OUTPUT}" "${SESSION_B_OUTPUT}" >&2
    exit 1
fi
if grep -q "deadlock detected" "${SESSION_A_OUTPUT}" "${SESSION_B_OUTPUT}"; then
    echo "Nested RLS policy lock upgrade deadlocked" >&2
    cat "${SESSION_A_OUTPUT}" "${SESSION_B_OUTPUT}" >&2
    exit 1
fi
if ! grep -q "could not acquire the pg_textsearch RLS DDL lock" \
    "${SESSION_B_OUTPUT}"; then
    echo "Nested RLS policy lock upgrade did not report lock contention" >&2
    cat "${SESSION_B_OUTPUT}" >&2
    exit 1
fi

psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 >/dev/null << 'SQL'
DROP EVENT TRIGGER test_nested_policy_upgrade_trigger;
DROP FUNCTION test_nested_policy_upgrade();
SQL

psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 >/dev/null << 'SQL'
CREATE TABLE end_trigger_outer (id integer);
CREATE TABLE end_trigger_nested (id integer);

CREATE FUNCTION test_end_trigger_nested_ddl()
RETURNS event_trigger AS $$
BEGIN
    IF current_setting('rls_end_test.action', true)
            IS DISTINCT FROM 'outer' THEN
        RETURN;
    END IF;

    PERFORM set_config('rls_end_test.action', 'running', false);
    PERFORM pg_sleep(5);
    PERFORM set_config('pg_textsearch.allow_rls', 'off', false);
    EXECUTE 'ALTER TABLE end_trigger_nested ENABLE ROW LEVEL SECURITY';
END
$$ LANGUAGE plpgsql;

CREATE EVENT TRIGGER test_end_trigger_nested_ddl_trigger
ON ddl_command_end
WHEN TAG IN ('ALTER TABLE')
EXECUTE FUNCTION test_end_trigger_nested_ddl();
SQL

PGAPPNAME=rls-end-trigger-session-a \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = on;
        SET rls_end_test.action = 'outer';
        ALTER TABLE end_trigger_outer ADD COLUMN extra integer;" \
    > "${SESSION_A_OUTPUT}" 2>&1 &
SESSION_A_PID=$!

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity AS a
        JOIN pg_locks AS l ON l.pid = a.pid
        WHERE a.application_name = 'rls-end-trigger-session-a'
          AND a.wait_event_type = 'Timeout'
          AND l.locktype = 'relation'
          AND l.relation = 'end_trigger_outer'::regclass
          AND l.mode = 'AccessExclusiveLock'
          AND l.granted
    );
" "session A to hold the outer relation lock"

PGAPPNAME=rls-end-trigger-session-b \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = off;
        ALTER TABLE end_trigger_outer ENABLE ROW LEVEL SECURITY;" \
    > "${SESSION_B_OUTPUT}" 2>&1 &
SESSION_B_PID=$!

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity AS a
        JOIN pg_locks AS object_lock ON object_lock.pid = a.pid
        JOIN pg_locks AS relation_lock ON relation_lock.pid = a.pid
        WHERE a.application_name = 'rls-end-trigger-session-b'
          AND object_lock.locktype = 'object'
          AND object_lock.classid = 'pg_extension'::regclass
          AND object_lock.objid = 0
          AND object_lock.mode = 'ExclusiveLock'
          AND object_lock.granted
          AND relation_lock.locktype = 'relation'
          AND relation_lock.relation = 'end_trigger_outer'::regclass
          AND relation_lock.mode = 'AccessExclusiveLock'
          AND NOT relation_lock.granted
    );
" "session B to hold the policy lock while waiting on the outer table"

set +e
wait "${SESSION_A_PID}"
session_a_status=$?
SESSION_A_PID=
wait "${SESSION_B_PID}"
session_b_status=$?
SESSION_B_PID=
set -e

if [ "${session_a_status}" -eq 0 ] || [ "${session_b_status}" -ne 0 ]; then
    echo "End-trigger nested DDL did not fail safely" >&2
    cat "${SESSION_A_OUTPUT}" "${SESSION_B_OUTPUT}" >&2
    exit 1
fi
if grep -q "deadlock detected" "${SESSION_A_OUTPUT}" "${SESSION_B_OUTPUT}"; then
    echo "End-trigger nested DDL deadlocked" >&2
    cat "${SESSION_A_OUTPUT}" "${SESSION_B_OUTPUT}" >&2
    exit 1
fi
if ! grep -q "could not acquire the pg_textsearch RLS DDL lock" \
    "${SESSION_A_OUTPUT}"; then
    echo "End-trigger nested DDL did not report lock contention" >&2
    cat "${SESSION_A_OUTPUT}" >&2
    exit 1
fi

psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 >/dev/null << 'SQL'
DROP EVENT TRIGGER test_end_trigger_nested_ddl_trigger;
DROP FUNCTION test_end_trigger_nested_ddl();
SQL

start_rls_holder lock_parent

PGAPPNAME=rls-lock-session-b \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = off;
        CREATE INDEX lock_child_idx ON lock_child USING bm25(content)
        WITH (text_config='english');" \
    > "${SESSION_B_OUTPUT}" 2>&1 &
SESSION_B_PID=$!

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity AS a
        JOIN pg_locks AS l ON l.pid = a.pid
        WHERE a.application_name = 'rls-lock-session-b'
          AND l.locktype = 'object'
          AND l.classid = 'pg_extension'::regclass
          AND l.objid = 0
          AND l.mode = 'ExclusiveLock'
          AND NOT l.granted
    );
" "session B to wait on the extension object lock"

child_locks=$(run_value "
    SELECT count(*)
    FROM pg_stat_activity AS a
    JOIN pg_locks AS l ON l.pid = a.pid
    WHERE a.application_name = 'rls-lock-session-b'
      AND l.locktype = 'relation'
      AND l.relation = 'lock_child'::regclass
      AND l.mode IN ('ShareLock', 'ShareUpdateExclusiveLock')
      AND l.granted;
")
if [ "${child_locks}" != "0" ]; then
    echo "Session B locked the child before DDL serialization" >&2
    exit 1
fi

release_session_a

set +e
wait "${SESSION_B_PID}"
session_b_status=$?
set -e
SESSION_B_PID=

if [ "${session_b_status}" -eq 0 ]; then
    echo "BM25 index creation unexpectedly succeeded" >&2
    exit 1
fi
if grep -q "deadlock detected" "${SESSION_B_OUTPUT}"; then
    echo "RLS/BM25 DDL deadlocked" >&2
    cat "${SESSION_B_OUTPUT}" >&2
    exit 1
fi
if ! grep -q "BM25 indexes are not allowed on row-level security" \
    "${SESSION_B_OUTPUT}"; then
    echo "Missing expected RLS rejection" >&2
    cat "${SESSION_B_OUTPUT}" >&2
    exit 1
fi

psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 >/dev/null << 'SQL'
CREATE TABLE create_lock_gate (id integer);
CREATE TABLE partition_parent (id integer, content text)
    PARTITION BY RANGE (id);
CREATE INDEX partition_parent_idx
    ON partition_parent USING bm25(content)
    WITH (text_config='english');
SQL

start_rls_holder create_lock_gate

PGAPPNAME=rls-lock-session-b \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = off;
        CREATE TABLE partition_child PARTITION OF partition_parent
        FOR VALUES FROM (0) TO (10);" \
    > "${SESSION_B_OUTPUT}" 2>&1 &
SESSION_B_PID=$!

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity AS a
        JOIN pg_locks AS l ON l.pid = a.pid
        WHERE a.application_name = 'rls-lock-session-b'
          AND l.locktype = 'object'
          AND l.classid = 'pg_extension'::regclass
          AND l.objid = 0
          AND l.mode = 'ExclusiveLock'
          AND NOT l.granted
    );
" "partition creation to wait on the extension object lock"

parent_locks=$(run_value "
    SELECT count(*)
    FROM pg_stat_activity AS a
    JOIN pg_locks AS l ON l.pid = a.pid
    WHERE a.application_name = 'rls-lock-session-b'
      AND l.locktype = 'relation'
      AND l.relation = 'partition_parent'::regclass
      AND l.granted;
")
if [ "${parent_locks}" != "0" ]; then
    echo "Partition creation locked its parent before DDL serialization" >&2
    PARTITION_ORDER_FAILED=1
fi

release_session_a
wait "${SESSION_B_PID}"
SESSION_B_PID=

psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 >/dev/null << 'SQL'
CREATE TABLE cic_lock_docs (id integer, content text);
INSERT INTO cic_lock_docs
SELECT g, 'document ' || g
FROM generate_series(1, 1000) AS g;
SQL

rm -f "${SESSION_A_INPUT}" "${SESSION_A_OUTPUT}"
mkfifo "${SESSION_A_INPUT}"
PGAPPNAME=rls-lock-session-a \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 < "${SESSION_A_INPUT}" \
    > "${SESSION_A_OUTPUT}" 2>&1 &
SESSION_A_PID=$!
exec 3> "${SESSION_A_INPUT}"
printf '%s\n' \
    "BEGIN;" \
    "INSERT INTO cic_lock_docs VALUES (1001, 'old writer');" >&3

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity AS a
        JOIN pg_locks AS l ON l.pid = a.pid
        WHERE a.application_name = 'rls-lock-session-a'
          AND a.state = 'idle in transaction'
          AND l.locktype = 'relation'
          AND l.relation = 'cic_lock_docs'::regclass
          AND l.mode = 'RowExclusiveLock'
          AND l.granted
    );
" "session A to hold an old writer lock"

PGAPPNAME=rls-lock-session-b \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 > "${SESSION_B_OUTPUT}" 2>&1 << 'SQL' &
SET pg_textsearch.allow_rls = off;
CREATE INDEX CONCURRENTLY cic_lock_idx
    ON cic_lock_docs USING bm25(content)
    WITH (text_config='english');
SQL
SESSION_B_PID=$!

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_index
        WHERE indexrelid = to_regclass('cic_lock_idx')
          AND NOT indisready
          AND NOT indisvalid
    );
" "concurrent index creation to cross its first internal commit"

object_locks=$(run_value "
    SELECT count(*)
    FROM pg_stat_activity AS a
    JOIN pg_locks AS l ON l.pid = a.pid
    WHERE a.application_name = 'rls-lock-session-b'
      AND l.locktype = 'object'
      AND l.classid = 'pg_extension'::regclass
      AND l.objid = 0
      AND l.mode = 'ExclusiveLock'
      AND l.granted;
")
if [ "${object_locks}" != "1" ]; then
    echo "Concurrent index creation lost its DDL serialization lock" >&2
    exit 1
fi

release_session_a
wait "${SESSION_B_PID}"
SESSION_B_PID=

psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 >/dev/null << 'SQL'
CREATE TABLE policy_pin_parent (id integer, content text);
CREATE TABLE policy_pin_child () INHERITS (policy_pin_parent);
CREATE TABLE policy_pin_nested (id integer, content text);
CREATE INDEX policy_pin_nested_idx
    ON policy_pin_nested USING bm25(content)
    WITH (text_config='english');

CREATE FUNCTION policy_pin_flip()
RETURNS event_trigger AS $$
DECLARE
    action text := current_setting('rls_pin_test.action', true);
BEGIN
    IF action NOT IN ('flip', 'flip_nested') THEN
        RETURN;
    END IF;

    PERFORM set_config('rls_pin_test.action', 'running', true);
    PERFORM set_config('pg_textsearch.allow_rls', 'on', false);

    IF action = 'flip_nested' THEN
        EXECUTE 'ALTER TABLE policy_pin_nested ENABLE ROW LEVEL SECURITY';

        IF NOT EXISTS (
            SELECT 1
            FROM pg_locks
            WHERE pid = pg_backend_pid()
              AND locktype = 'object'
              AND classid = 'pg_extension'::regclass
              AND objid = 0
              AND mode = 'ShareLock'
              AND granted
        ) THEN
            RAISE EXCEPTION
                'nested utility command did not acquire a policy lock';
        END IF;
    END IF;
END
$$ LANGUAGE plpgsql;

CREATE EVENT TRIGGER policy_pin_flip_trigger
ON ddl_command_start
WHEN TAG IN ('ALTER TABLE', 'CREATE INDEX')
EXECUTE FUNCTION policy_pin_flip();
SQL

rm -f "${SESSION_A_INPUT}" "${SESSION_A_OUTPUT}"
mkfifo "${SESSION_A_INPUT}"
PGAPPNAME=rls-policy-pin-session-a \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 < "${SESSION_A_INPUT}" \
    > "${SESSION_A_OUTPUT}" 2>&1 &
SESSION_A_PID=$!
exec 3> "${SESSION_A_INPUT}"
printf '%s\n' \
    "SET pg_textsearch.allow_rls = off;" \
    "SET rls_pin_test.action = 'flip_nested';" \
    "BEGIN;" \
    "ALTER TABLE policy_pin_parent ENABLE ROW LEVEL SECURITY;" >&3

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity AS a
        JOIN pg_locks AS l ON l.pid = a.pid
        WHERE a.application_name = 'rls-policy-pin-session-a'
          AND a.state = 'idle in transaction'
          AND l.locktype = 'object'
          AND l.classid = 'pg_extension'::regclass
          AND l.objid = 0
          AND l.mode = 'ExclusiveLock'
          AND l.granted
    );
" "session A to finish RLS enablement with its pinned policy lock"

PGAPPNAME=rls-policy-pin-session-b \
    psql -X -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
    -v ON_ERROR_STOP=1 \
    -c "SET pg_textsearch.allow_rls = off;
        SET rls_pin_test.action = 'flip';
        CREATE INDEX policy_pin_child_idx
        ON policy_pin_child USING bm25(content)
        WITH (text_config='english');" \
    > "${SESSION_B_OUTPUT}" 2>&1 &
SESSION_B_PID=$!

wait_for_true "
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity AS a
        JOIN pg_locks AS l ON l.pid = a.pid
        WHERE a.application_name = 'rls-policy-pin-session-b'
          AND l.locktype = 'object'
          AND l.classid = 'pg_extension'::regclass
          AND l.objid = 0
          AND l.mode = 'ExclusiveLock'
          AND NOT l.granted
    );
" "session B to wait on session A's pinned policy lock"

release_session_a

set +e
wait "${SESSION_B_PID}"
session_b_status=$?
set -e
SESSION_B_PID=

if [ "${session_b_status}" -eq 0 ]; then
    echo "GUC-changing trigger bypassed the pinned RLS policy" >&2
    exit 1
fi
if ! grep -q "BM25 indexes are not allowed on row-level security" \
    "${SESSION_B_OUTPUT}"; then
    echo "Missing expected pinned-policy rejection" >&2
    cat "${SESSION_B_OUTPUT}" >&2
    exit 1
fi

policy_pin_state=$(run_value "
    SELECT relrowsecurity,
           to_regclass('policy_pin_child_idx') IS NULL
    FROM pg_class
    WHERE oid = 'policy_pin_parent'::regclass;
")
if [ "${policy_pin_state}" != "t|t" ]; then
    echo "Opposite hierarchy mutations jointly committed" >&2
    exit 1
fi

if [ "${PARTITION_ORDER_FAILED}" -ne 0 ]; then
    exit 1
fi

echo "RLS DDL locking tests passed"
