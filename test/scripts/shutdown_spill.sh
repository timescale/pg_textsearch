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
TEST_HOST=127.0.0.1
DATA_DIR="${SCRIPT_DIR}/../tmp_shutdown_spill_test"
LOGFILE="${DATA_DIR}/postgres.log"
SLEEPER_PID=

PGBINDIR="$(pg_config --bindir)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()   { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn()  { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

wait_for_child_exit() {
    local pid=$1
    local attempts=$2

    for _ in $(seq 1 "${attempts}"); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            # The bounded poll established that the child exited; this wait
            # only reaps its already-available status.
            wait "${pid}" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}

stop_sleeper() {
    if [ -z "${SLEEPER_PID}" ]; then
        return
    fi

    if ! wait_for_child_exit "${SLEEPER_PID}" 50; then
        warn "Sleeper PID ${SLEEPER_PID} did not exit in 5s; sending SIGTERM"
        kill -TERM "${SLEEPER_PID}" 2>/dev/null || true
        if ! wait_for_child_exit "${SLEEPER_PID}" 50; then
            warn "Sleeper PID ${SLEEPER_PID} ignored SIGTERM for 5s; \
sending SIGKILL (state: $(ps -o pid=,stat=,cmd= -p "${SLEEPER_PID}" \
2>/dev/null || echo unavailable))"
            kill -9 "${SLEEPER_PID}" 2>/dev/null || true
            if ! wait_for_child_exit "${SLEEPER_PID}" 50; then
                warn "Sleeper PID ${SLEEPER_PID} still exists 5s after \
SIGKILL (state: $(ps -o pid=,stat=,cmd= -p "${SLEEPER_PID}" \
2>/dev/null || echo unavailable)); continuing bounded cleanup"
            fi
        fi
    fi
    SLEEPER_PID=
}

cleanup() {
    local exit_code=$?
    log "Cleaning up (exit code: $exit_code)..."
    stop_sleeper
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m fast -w -t 30 \
            >/dev/null 2>&1 || \
            "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate -w -t 30 \
                >/dev/null 2>&1 || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup() {
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
    "${PGBINDIR}/initdb" -D "${DATA_DIR}" \
        --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
shared_buffers = 128MB
max_connections = 20
shared_preload_libraries = 'pg_textsearch'
listen_addresses = '${TEST_HOST}'
unix_socket_directories = ''
EOF

    "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" -l "${LOGFILE}" -w \
        >/dev/null
    "${PGBINDIR}/createdb" -h "${TEST_HOST}" -p "${TEST_PORT}" "${TEST_DB}"
    "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

sql() {
    "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

sql_quiet() {
    "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
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
    } | "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
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
    "${PGBINDIR}/psql" -h "${TEST_HOST}" -p "${TEST_PORT}" \
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
    stop_sleeper

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

    log "=== PASSED ==="
}

main "$@"
