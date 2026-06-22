#!/bin/bash
#
# Regression test for duplicate CTIDs during concurrent top-k index scans.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55459
TEST_DB=concurrent_duplicate_read_test
DATA_DIR="${SCRIPT_DIR}/../tmp_concurrent_duplicate_read"
LOGFILE="${DATA_DIR}/postgres.log"
ERR_DIR="${DATA_DIR}/client_logs"

# Tunables.
QUERY_LIMIT=3000      # top-k limit; must be < corpus to force re-execution
SEED_ROWS=3500        # initial corpus, just above QUERY_LIMIT
DURATION=45           # seconds of concurrent insert + read activity
N_INSERTERS=6
N_READERS=2

# Per-session safety nets for long-running CALL/DO statements.
STMT_TIMEOUT="$((DURATION + 30))s"
LOCK_TIMEOUT="30s"

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
    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up PostgreSQL instance..."
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1
    mkdir -p "${ERR_DIR}"

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_textsearch'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
# Keep this focused on scan re-execution, not VACUUM races.
autovacuum = off
# Churn the index under concurrent inserts.
pg_textsearch.memtable_pages_threshold = 2
pg_textsearch.segments_per_level = 2
pg_textsearch.bulk_load_threshold = 0
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w -o "-p ${TEST_PORT}" || \
        error "Failed to start PostgreSQL"

    createdb -h "${DATA_DIR}" -p ${TEST_PORT} ${TEST_DB}
    psql -h "${DATA_DIR}" -p ${TEST_PORT} -d ${TEST_DB} \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
    log "Test database ready"
}

PSQL="psql -h ${DATA_DIR} -p ${TEST_PORT} -d ${TEST_DB} -qAt -v ON_ERROR_STOP=1"

seed_data() {
    log "Creating index, seeding ${SEED_ROWS} docs, and defining helpers..."
    $PSQL <<SQL >/dev/null
SET client_min_messages=warning;
CREATE TABLE docs (id bigserial PRIMARY KEY, content text NOT NULL);
CREATE INDEX docs_idx ON docs USING bm25(content) WITH (text_config='english');

-- Match the whole corpus while preserving some score variation.
INSERT INTO docs(content)
SELECT 'alpha w' || (gs % 50)
FROM generate_series(1, ${SEED_ROWS}) gs;

-- Reader-visible failure signal.
CREATE TABLE dup_signal (detected_at timestamptz, extra int, sample_query text);

-- Commit frequently so readers can re-score a changing corpus.
CREATE PROCEDURE insert_docs(seconds int) LANGUAGE plpgsql AS \$\$
DECLARE
    deadline timestamptz := clock_timestamp() + make_interval(secs => seconds);
    i bigint := 0;
BEGIN
    LOOP
        INSERT INTO docs(content) VALUES ('alpha w' || (i % 50));
        i := i + 1;
        IF i % 5 = 0 THEN
            COMMIT;
        END IF;
        EXIT WHEN clock_timestamp() > deadline;
    END LOOP;
    COMMIT;
END
\$\$;
SQL

    # This test must exercise ORDER BY ... LIMIT via index scan.
    local plan
    plan=$($PSQL -c "EXPLAIN (COSTS off)
        SELECT ctid FROM docs
        ORDER BY content <@> to_bm25query('alpha', 'docs_idx')
        LIMIT ${QUERY_LIMIT}")
    if ! echo "$plan" | grep -qi "Index Scan"; then
        warn "Reader plan was not an Index Scan:"
        echo "$plan"
        error "TEST SETUP FAILED: reader no longer exercises the index-scan path"
    fi
}

inserter() {
    PGOPTIONS="-c statement_timeout=${STMT_TIMEOUT} -c lock_timeout=${LOCK_TIMEOUT}" \
        $PSQL -c "CALL insert_docs(${DURATION});" \
        >>"${ERR_DIR}/inserter_$1.log" 2>&1 || return 10
}

# Repeatedly check each top-k result set for duplicate CTIDs.
reader() {
    local tag=$1
    PGOPTIONS="-c statement_timeout=${STMT_TIMEOUT} -c lock_timeout=${LOCK_TIMEOUT}" \
        $PSQL >>"${ERR_DIR}/reader_${tag}.log" 2>&1 <<SQL || return 20
DO \$\$
DECLARE
    d int;
    deadline timestamptz := clock_timestamp() + make_interval(secs => ${DURATION});
BEGIN
    LOOP
        SELECT count(*) - count(DISTINCT ctid)
          INTO d
          FROM (
              SELECT ctid FROM docs
              ORDER BY content <@> to_bm25query('alpha', 'docs_idx')
              LIMIT ${QUERY_LIMIT}
          ) s;
        IF d > 0 THEN
            INSERT INTO dup_signal(detected_at, extra, sample_query)
            VALUES (clock_timestamp(), d, 'reader ${tag}');
            RAISE WARNING 'DUPLICATE DETECTED by reader ${tag}: % extra row(s)', d;
            RETURN;
        END IF;
        EXIT WHEN clock_timestamp() > deadline;
    END LOOP;
END
\$\$;
SQL
}

run_test() {
    log "Running ${N_INSERTERS} inserters + ${N_READERS} readers for ~${DURATION}s..."

    local pids=()
    for i in $(seq 1 ${N_INSERTERS}); do
        inserter "$i" & pids+=("$!")
    done
    for i in $(seq 1 ${N_READERS}); do
        reader "$i" & pids+=("$!")
    done

    local failed=0
    for p in "${pids[@]}"; do
        wait "$p" || { warn "a client (pid $p) exited non-zero"; failed=1; }
    done

    local dups
    if ! dups=$($PSQL -c "SELECT count(*) FROM dup_signal;"); then
        error "TEST FAILED: could not query dup_signal (server down?)"
    fi
    if [ "${dups:-0}" -gt 0 ]; then
        warn "Duplicate ctids observed in a single index-scan result set:"
        $PSQL -c "SELECT detected_at, extra, sample_query FROM dup_signal;" || true
        error "TEST FAILED: BM25 index scan returned the same row more than once"
    fi

    if [ "$failed" -ne 0 ]; then
        warn "Client logs:"
        tail -n 20 "${ERR_DIR}"/*.log 2>/dev/null || true
        error "TEST FAILED: a concurrent client exited with an error"
    fi

    log "TEST PASSED: no duplicate ctids across concurrent top-k reads"
}

# Quiescent sanity checks after the concurrent phase.
verify_postrun() {
    local corpus expected total distinct

    corpus=$($PSQL -c "SELECT count(*) FROM docs;")

    # Ensure readers could hit the limit-doubling path.
    if [ "${corpus:-0}" -le "${QUERY_LIMIT}" ]; then
        error "TEST INCONCLUSIVE: corpus (${corpus}) did not exceed QUERY_LIMIT (${QUERY_LIMIT}); re-execution path not exercised"
    fi

    # Static top-k should be complete and duplicate-free.
    expected=${QUERY_LIMIT}
    read -r total distinct <<<"$($PSQL -c "
        SELECT count(*), count(DISTINCT ctid)
        FROM (
            SELECT ctid FROM docs
            ORDER BY content <@> to_bm25query('alpha', 'docs_idx')
            LIMIT ${QUERY_LIMIT}
        ) s;" | tr '|' ' ')"

    if [ "${total}" != "${expected}" ]; then
        error "TEST FAILED: top-k returned ${total} rows, expected ${expected} (corpus=${corpus}) -- under-return/miss"
    fi
    if [ "${distinct}" != "${total}" ]; then
        error "TEST FAILED: top-k returned ${total} rows but only ${distinct} distinct ctids -- duplicate"
    fi

    log "POST-RUN PASSED: top-k returns ${total} distinct rows on a ${corpus}-row corpus"
}

# Main
setup_test_db
seed_data
run_test
verify_postrun

log "All tests passed!"
exit 0
