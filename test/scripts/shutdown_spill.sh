#!/bin/bash
#
# Clean-shutdown spill test (regression test for issue #333, scenario A).
#
# On a clean pg_ctl stop, backends that are still alive receive SIGTERM.
# The before_shmem_exit hook spills any un-spilled memtable contents to
# segments and drains the docid-page chain.  Without this, the first
# query after the next server start has to walk the entire chain and
# re-tokenize every referenced heap tuple.
#
# This test only exercises the path where a backend is still alive at
# shutdown time (hook fires).  The short-connection path (no backend
# alive at shutdown) is handled by the VACUUM-based fix; see
# vacuum_extended.sql Test 7.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55441
TEST_DB=shutdown_spill_test
DATA_DIR="${SCRIPT_DIR}/../tmp_shutdown_spill_test"
SOCKET_DIR="${TMPDIR:-/tmp}/pgts_shutdown_${TEST_PORT}"
LOGFILE="${DATA_DIR}/postgres.log"

PGBINDIR="$(pg_config --bindir)"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log()   { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    local exit_code=$?
    log "Cleaning up (exit code: $exit_code)..."
    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m fast \
            >/dev/null 2>&1 || \
            "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate \
                >/dev/null 2>&1 || true
    fi
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup() {
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    mkdir -p "${DATA_DIR}" "${SOCKET_DIR}"
    "${PGBINDIR}/initdb" -D "${DATA_DIR}" \
        --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
shared_buffers = 128MB
max_connections = 20
shared_preload_libraries = 'pg_textsearch'
unix_socket_directories = '${SOCKET_DIR}'
EOF

    "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" -l "${LOGFILE}" -w \
        >/dev/null
    "${PGBINDIR}/createdb" -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

sql() {
    "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

sql_quiet() {
    "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "$1" >/dev/null 2>&1
}

main() {
    log "=== Clean-shutdown spill test (issue #333, scenario A) ==="

    command -v "${PGBINDIR}/pg_ctl" >/dev/null 2>&1 || \
        error "pg_ctl not found"

    setup

    # Two tables + indexes so the hook iterates more than one entry.
    sql_quiet "CREATE TABLE docs (id serial PRIMARY KEY, body text);"
    sql_quiet "CREATE INDEX docs_idx ON docs USING bm25 (body)
        WITH (text_config='english');"
    sql_quiet "CREATE TABLE extras (id serial PRIMARY KEY, body text);"
    sql_quiet "CREATE INDEX extras_idx ON extras USING bm25 (body)
        WITH (text_config='english');"

    # Populate the memtable chains without tripping bulk-load
    # auto-spill.  One INSERT per autocommit statement keeps each
    # transaction well under the bulk-load threshold, so the rows
    # accumulate in the on-disk memtable chain instead of
    # spilling to a segment.
    log "Inserting 500 rows per table in single-statement transactions..."
    {
        for i in $(seq 1 500); do
            echo "INSERT INTO docs (body) VALUES ('doc ${i} "\
"alpha beta gamma delta epsilon zeta');"
            echo "INSERT INTO extras (body) VALUES ('extra ${i} "\
"alpha beta gamma delta epsilon zeta');"
        done
    } | "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" >/dev/null 2>&1

    for idx in docs_idx extras_idx; do
        local pre
        pre=$(sql "SELECT (
                COALESCE(sum(n_records), 0) = 500
            )::int FROM bm25_memtable_chain('${idx}')")
        if [ "${pre}" != "1" ]; then
            error "Pre-shutdown: ${idx} chain not populated as expected"
        fi
    done
    log "Pre-shutdown: both chains hold 500 entries (good)"

    # Long-running backend that has touched BOTH indexes so both
    # appear in local_state_cache and the shutdown hook's hash-loop
    # has more than one entry to iterate over.  pg_sleep keeps the
    # backend alive until pg_ctl stop SIGTERMs it.
    "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "
            SELECT count(*) FROM (
                SELECT 1 FROM docs
                ORDER BY body <@> to_bm25query('alpha', 'docs_idx')
                LIMIT 1
            ) s;
            SELECT count(*) FROM (
                SELECT 1 FROM extras
                ORDER BY body <@> to_bm25query('alpha', 'extras_idx')
                LIMIT 1
            ) s;
            SELECT pg_sleep(60);
        " >/dev/null 2>&1 &
    SLEEPER_PID=$!
    sleep 2  # let the backend attach to both indexes

    # Clean shutdown.  Postmaster sends SIGTERM to the sleeping
    # backend → die handler fires → FATAL → proc_exit(1) → our
    # before_shmem_exit hook walks local_state_cache and spills
    # both indexes.
    log "Clean-shutting down postgres (hook should fire on the live backend)..."
    "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m fast -w \
        >/dev/null
    wait "${SLEEPER_PID}" 2>/dev/null || true

    # Restart.
    "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" -l "${LOGFILE}" \
        -w -t 30 >/dev/null
    sleep 1

    # Both chains must be drained and both indexes queryable.  We
    # explicitly do not trigger recovery by querying the index
    # first — bm25_memtable_chain reads the on-disk metapage and
    # chain pages directly, so we can observe the post-spill
    # state without any in-process bootstrap.
    for idx in docs_idx extras_idx; do
        local post
        post=$(sql "SELECT (
                COALESCE(sum(n_records), 0) = 0
            )::int FROM bm25_memtable_chain('${idx}')")
        if [ "${post}" != "1" ]; then
            local summary
            summary=$(sql "SELECT bm25_summarize_index('${idx}')")
            error "Post-restart: ${idx} chain not drained. Summary:
${summary}"
        fi
    done
    log "Post-restart: both chains are empty (spill happened for each)"

    for pair in "docs:docs_idx" "extras:extras_idx"; do
        local tbl="${pair%:*}"
        local idx="${pair#*:}"
        local count
        count=$(sql "SELECT count(*) FROM (
                SELECT 1 FROM ${tbl}
                ORDER BY body <@> to_bm25query('alpha', '${idx}')
                LIMIT 1000
            ) s")
        if [ "${count}" != "500" ]; then
            error "Post-restart: ${idx} returned ${count} (expected 500)"
        fi
    done
    log "Post-restart: both indexes return all 500 rows (good)"

    # A targeted backend termination must not enter blocking maintenance
    # from before_shmem_exit.  Hold ShareUpdateExclusiveLock in another
    # transaction, then terminate a backend whose local cache contains an
    # index with a spillable chain.
    sql_quiet "CREATE TABLE blocked_docs (
        id serial PRIMARY KEY, body text NOT NULL);"
    sql_quiet "CREATE INDEX blocked_docs_idx ON blocked_docs USING bm25(body)
        WITH (text_config='english', compaction='inline');"
    {
        for i in $(seq 1 500); do
            echo "INSERT INTO blocked_docs (body)
                VALUES ('blocked ${i} alpha beta gamma delta epsilon');"
        done
    } | "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" >/dev/null 2>&1

    local blocked_pre
    blocked_pre=$(sql "
        SELECT COALESCE(sum(n_records), 0)
        FROM bm25_memtable_chain('blocked_docs_idx');")
    [ "${blocked_pre}" = "500" ] ||
        error "termination test chain was not populated"

    PGAPPNAME=pgts-shutdown-blocker \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "
            BEGIN;
            ALTER INDEX blocked_docs_idx SET (compaction='inline');
            SELECT pg_sleep(60);
        " >/dev/null 2>&1 &
    BLOCKER_PID=$!

    local blocker_ready=f
    for _ in $(seq 1 100); do
        blocker_ready=$(sql "
            SELECT EXISTS (
                SELECT 1
                FROM pg_locks l
                JOIN pg_stat_activity a USING (pid)
                WHERE a.application_name = 'pgts-shutdown-blocker'
                  AND l.relation = 'blocked_docs_idx'::regclass
                  AND l.mode = 'ShareUpdateExclusiveLock'
                  AND l.granted
            );")
        [ "${blocker_ready}" = "t" ] && break
        sleep 0.1
    done
    [ "${blocker_ready}" = "t" ] ||
        error "maintenance blocker did not acquire the index lock"

    PGAPPNAME=pgts-shutdown-target \
        "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "
            SELECT count(*) FROM (
                SELECT 1 FROM blocked_docs
                ORDER BY body <@> to_bm25query(
                    'alpha', 'blocked_docs_idx')
                LIMIT 1
            ) ranked;
            SELECT pg_sleep(60);
        " >/dev/null 2>&1 &
    TARGET_PID=$!

    local target_backend=
    for _ in $(seq 1 100); do
        target_backend=$(sql "
            SELECT pid
            FROM pg_stat_activity
            WHERE application_name = 'pgts-shutdown-target'
              AND wait_event = 'PgSleep';")
        [ -n "${target_backend}" ] && break
        sleep 0.1
    done
    [ -n "${target_backend}" ] ||
        error "shutdown target did not attach to the index"

    local terminated
    terminated=$(sql "
        SELECT pg_terminate_backend(${target_backend}, 3000);")
    if [ "${terminated}" != "t" ]; then
        error "terminated backend blocked in shutdown spill maintenance"
    fi
    wait "${TARGET_PID}" 2>/dev/null || true

    local blocked_post
    blocked_post=$(sql "
        SELECT COALESCE(sum(n_records), 0)
        FROM bm25_memtable_chain('blocked_docs_idx');")
    [ "${blocked_post}" = "0" ] ||
        error "terminated backend did not finish its shutdown spill"

    kill "${BLOCKER_PID}" 2>/dev/null || true
    wait "${BLOCKER_PID}" 2>/dev/null || true
    log "Targeted backend termination completed with maintenance admission held"

    log "=== PASSED ==="
}

main "$@"
