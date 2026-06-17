#!/bin/bash
#
# Regression test for the duplicate-results bug in the BM25 index scan.
#
# tp_gettuple() sizes its first scoring pass to the query LIMIT.  When the
# corpus is larger than the limit, the buffer fills and, once it is
# exhausted, the scan re-executes the scoring query with a doubled limit and
# resumes by INTEGER POSITION (so->current_pos = old_count).  Under
# concurrent writes the corpus (and thus IDF / score ordering) changes
# between the two passes, so a row already returned by the first pass can
# move past old_count in the second pass and be emitted again: the same heap
# tuple is returned twice, with two different scores.
#
# This test drives concurrent inserters while readers run a top-k query whose
# LIMIT is smaller than the corpus, and checks that no single query ever
# returns the same ctid twice.  Before the fix this trips within seconds;
# after it (tp_gettuple skips already-returned ctids by identity) it stays
# clean.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55459
TEST_DB=concurrent_duplicate_read_test
DATA_DIR="${SCRIPT_DIR}/../tmp_concurrent_duplicate_read"
LOGFILE="${DATA_DIR}/postgres.log"
ERR_DIR="${DATA_DIR}/client_logs"

# Tunables (kept small so the test is fast but still reproduces reliably).
QUERY_LIMIT=3000      # top-k limit; must be < corpus to force re-execution
SEED_ROWS=3500        # initial corpus, just above QUERY_LIMIT
DURATION=45           # seconds of concurrent insert + read activity
N_INSERTERS=6
N_READERS=2

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
# Insert-only workload: keep autovacuum out so this test targets the scan
# re-execution bug, not the (separate) VACUUM-vs-merge race.
autovacuum = off
# Spill / merge aggressively so the chain churns under concurrent inserts,
# which is what makes the re-execution path fire.
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

-- Every doc shares the common token 'alpha' so the query matches the whole
-- corpus; a few random tokens add per-doc variation.
INSERT INTO docs(content)
SELECT 'alpha w' || (gs % 50)
FROM generate_series(1, ${SEED_ROWS}) gs;

-- Signal table: a reader inserts here if it ever sees a duplicate ctid.
CREATE TABLE dup_signal (detected_at timestamptz, extra int, sample_query text);

-- Inserter: commit frequently so the corpus grows between a reader's two
-- scoring passes.  Time-bounded so the test always terminates.
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

    # The bug only manifests on the index-scan path (ORDER BY ... LIMIT).
    # Fail loudly if the planner stops choosing it.
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
    $PSQL -c "CALL insert_docs(${DURATION});" \
        >>"${ERR_DIR}/inserter_$1.log" 2>&1 || return 10
}

# Reader: repeatedly run a top-k query and check whether any single result
# set contains the same ctid more than once.  Records the first violation in
# dup_signal and stops.
reader() {
    local tag=$1
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
    dups=$($PSQL -c "SELECT count(*) FROM dup_signal;" 2>/dev/null || echo "0")
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

# Main
setup_test_db
seed_data
run_test

log "All tests passed!"
exit 0
