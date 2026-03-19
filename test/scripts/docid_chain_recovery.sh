#!/bin/bash
#
# Docid chain crash recovery test (regression test for issue #291)
#
# When inserts fill more than one docid page (~1358 entries per page),
# the chain extension must flush the new page to disk before flushing
# the pointer that references it. Without this ordering, a crash can
# leave the chain pointing to an uninitialized (all-zero) block,
# causing "Invalid docid page magic" errors on recovery.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55435
TEST_DB=docid_chain_test
DATA_DIR="${SCRIPT_DIR}/../tmp_docid_chain_test"
LOGFILE="${DATA_DIR}/postgres.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()   { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn()  { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    local exit_code=$?
    log "Cleaning up..."
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}TEST FAILED${NC}"
    fi
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup() {
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
    initdb -D "${DATA_DIR}" --auth-local=trust \
        --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
shared_buffers = 128MB
max_connections = 20
logging_collector = on
log_filename = 'postgres.log'
shared_preload_libraries = 'pg_textsearch'
unix_socket_directories = '${DATA_DIR}'
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w
    createdb "${TEST_DB}"
    psql -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

sql() {
    psql -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

sql_verbose() {
    psql -d "${TEST_DB}" -c "$1" 2>&1
}

simulate_crash() {
    local pid
    pid=$(head -1 "${DATA_DIR}/postmaster.pid")
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid"
        sleep 2
        rm -f "${DATA_DIR}/postmaster.pid"
        log "Crashed postgres (PID $pid)"
    else
        error "No postgres process to kill"
    fi
}

restart() {
    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w -t 30
    sleep 1
    psql -d "${TEST_DB}" -c "SELECT 1;" >/dev/null
    log "Postgres restarted"
}

main() {
    log "=== Docid chain crash recovery test (issue #291) ==="

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"

    # Override inherited PGHOST/PGPORT so we connect to our own
    # instance, not the caller's (e.g., CI sanitizer instance).
    export PGHOST="${DATA_DIR}"
    export PGPORT="${TEST_PORT}"

    setup

    # Create table and index
    log "Creating table and BM25 index..."
    sql_verbose "
CREATE TABLE docs (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL
);
CREATE INDEX docs_bm25 ON docs USING bm25(content)
    WITH (text_config='english');
" >/dev/null

    # Insert enough rows to fill more than one docid page.
    # Each page holds ~1358 entries, so 2000 rows guarantees
    # the chain extends to at least a second page.
    log "Inserting 2000 documents (fills >1 docid page)..."
    sql "INSERT INTO docs (content)
         SELECT 'Document ' || g || ' about search terms'
         FROM generate_series(1, 2000) g;" >/dev/null

    # Verify pre-crash
    local pre_count
    pre_count=$(sql "SELECT count(*) FROM docs
        WHERE content <@> to_bm25query('document',
            'docs_bm25') < 0")
    log "Pre-crash: ${pre_count} search results"
    if [ -z "$pre_count" ] || [ "$pre_count" -lt 1000 ]; then
        error "Pre-crash search returned too few results: ${pre_count}"
    fi

    # Crash immediately after inserts -- before checkpoint
    # can flush the second docid page to disk
    log "Simulating crash..."
    simulate_crash

    # Restart and query -- this is where the bug manifested as:
    #   ERROR: Invalid docid page magic at block N:
    #          expected 0x54504944, found 0x00000000
    log "Restarting and querying (recovery must succeed)..."
    restart

    # Run the query and capture both stdout and stderr.
    # Recovery INFO/WARNING messages go to stderr.
    local post_result post_err post_exit
    post_result=$(psql -d "${TEST_DB}" -tA \
        -c "SET statement_timeout = '30s'" \
        -c "SELECT count(*) FROM docs
            WHERE content <@> to_bm25query('document',
                'docs_bm25') < 0" \
        2>"${DATA_DIR}/query_err.txt") \
        || post_exit=$?
    # Strip the SET output, keep only the count
    post_result=$(echo "$post_result" | grep -E '^[0-9]+$' | tail -1)

    post_err=$(cat "${DATA_DIR}/query_err.txt" 2>/dev/null || true)

    # Check for the specific error from issue #291
    if echo "$post_err" | grep -q "Invalid docid page magic"; then
        error "Bug #291 reproduced: ${post_err}"
    fi

    if [ -n "$post_exit" ] && [ "$post_exit" -ne 0 ]; then
        error "Query failed (exit $post_exit): ${post_err}"
    fi

    log "Post-crash: ${post_result} search results"
    if [ -z "$post_result" ] || [ "$post_result" -lt 1000 ]; then
        error "Post-crash search returned too few results: ${post_result}"
    fi

    log "=== PASSED ==="
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
