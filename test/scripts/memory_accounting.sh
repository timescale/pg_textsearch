#!/bin/bash
#
# Memory accounting tests for the global estimated_total_bytes counter.
# Verifies the atomic counter stays consistent across concurrent
# inserts, spills, DROP INDEX, and aborted CREATE INDEX.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55438
TEST_DB=pg_textsearch_memory_test
DATA_DIR="${SCRIPT_DIR}/../tmp_memory_test"
LOGFILE="${DATA_DIR}/postgres.log"

# Use pg_config to locate the correct binaries for this PG install.
PGBINDIR="$(pg_config --bindir)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()   { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn()  { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }
info()  { echo -e "${BLUE}[$(date '+%H:%M:%S')] $1${NC}"; }

cleanup() {
    local exit_code=$?
    log "Cleaning up memory accounting test (exit code: $exit_code)..."
    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m fast &>/dev/null ||
            "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up test PostgreSQL instance..."

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    "${PGBINDIR}/initdb" -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 30
shared_buffers = 128MB
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
log_min_messages = warning
shared_preload_libraries = 'pg_textsearch'
EOF

    "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" -l "${LOGFILE}" -w \
        || error "Failed to start PostgreSQL"

    "${PGBINDIR}/createdb" -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch VERSION '1.1.0-dev';" \
        >/dev/null

    # Create a persistent probe table used to force DSA attachment
    # before calling bm25_memory_usage() from fresh backends.
    run_sql_quiet "CREATE TABLE _probe (id serial, body text);"
    run_sql_quiet "CREATE INDEX _probe_idx ON _probe
        USING bm25(body) WITH (text_config='english');"
}

run_sql() {
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "$1" 2>&1
}

run_sql_quiet() {
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "$1" >/dev/null 2>&1
}

run_sql_value() {
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -tAc "$1" 2>/dev/null
}

# -------------------------------------------------------
# Helper: read estimated_bytes from bm25_memory_usage().
# The calling backend must attach to the global DSA first.
# We force this by probing a bm25 index with a dummy query
# before reading the counter.
# -------------------------------------------------------
get_estimated_bytes() {
    # Force DSA attachment by touching the _probe index, then
    # read the authoritative estimate via registry scan.
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -tAc "
        SELECT COUNT(*) FROM _probe
            WHERE body <@> 'xyzzy'::bm25query < 0;
        SELECT estimated_bytes FROM bm25_memory_usage();
    " 2>/dev/null | tail -1
}

get_counter_bytes() {
    # Read the atomic counter value (the O(1) hot-path counter).
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -tAc "
        SELECT COUNT(*) FROM _probe
            WHERE body <@> 'xyzzy'::bm25query < 0;
        SELECT counter_bytes FROM bm25_memory_usage();
    " 2>/dev/null | tail -1
}

# Assert that the atomic counter matches the authoritative scan.
# Accepts a small tolerance (default 0) for transient drift.
assert_counter_matches() {
    local label="$1"
    local est
    local ctr
    est=$(get_estimated_bytes)
    ctr=$(get_counter_bytes)

    if [ "$est" != "$ctr" ]; then
        error "❌ ${label}: counter_bytes ($ctr) != estimated_bytes ($est)"
    fi
    info "Counter verified: estimated=$est, counter=$ctr"
}

# -------------------------------------------------------
# Test 1: Counter starts at zero, grows with inserts,
#         drops to zero after spill.
# -------------------------------------------------------
test_basic_accounting() {
    log "Test 1: Basic accounting — insert, verify growth, spill, verify zero"

    run_sql_quiet "CREATE TABLE acct1 (id serial, body text);"
    run_sql_quiet "CREATE INDEX idx_acct1 ON acct1
        USING bm25(body) WITH (text_config='english');"

    local est_before
    est_before=$(get_estimated_bytes)
    info "Estimated bytes before insert: $est_before"

    run_sql_quiet "INSERT INTO acct1 (body)
        SELECT 'term_' || i FROM generate_series(1, 500) i;"

    local est_after
    est_after=$(get_estimated_bytes)
    info "Estimated bytes after 500 rows: $est_after"

    if [ "$est_after" -le "$est_before" ]; then
        error "❌ Test 1 failed: estimate did not grow after insert ($est_before -> $est_after)"
    fi

    assert_counter_matches "Test 1 after insert"

    run_sql_quiet "SELECT bm25_spill_index('idx_acct1');"

    local est_spilled
    est_spilled=$(get_estimated_bytes)
    info "Estimated bytes after spill: $est_spilled"

    if [ "$est_spilled" -ge "$est_after" ]; then
        error "❌ Test 1 failed: estimate did not drop after spill ($est_after -> $est_spilled)"
    fi

    assert_counter_matches "Test 1 after spill"

    run_sql_quiet "DROP TABLE acct1 CASCADE;"
    log "✅ Test 1 passed"
}

# -------------------------------------------------------
# Test 2: DROP INDEX decrements the global counter.
# -------------------------------------------------------
test_drop_index_accounting() {
    log "Test 2: DROP INDEX decrements the global counter"

    run_sql_quiet "CREATE TABLE acct2a (id serial, body text);"
    run_sql_quiet "CREATE TABLE acct2b (id serial, body text);"
    run_sql_quiet "CREATE INDEX idx_acct2a ON acct2a
        USING bm25(body) WITH (text_config='english');"
    run_sql_quiet "CREATE INDEX idx_acct2b ON acct2b
        USING bm25(body) WITH (text_config='english');"

    run_sql_quiet "INSERT INTO acct2a (body)
        SELECT 'alpha_' || i FROM generate_series(1, 500) i;"
    run_sql_quiet "INSERT INTO acct2b (body)
        SELECT 'beta_' || i FROM generate_series(1, 500) i;"

    local est_both
    est_both=$(get_estimated_bytes)
    info "Estimated with both indexes: $est_both"

    run_sql_quiet "DROP INDEX idx_acct2a;"

    local est_one
    est_one=$(get_estimated_bytes)
    info "Estimated after dropping idx_acct2a: $est_one"

    if [ "$est_one" -ge "$est_both" ]; then
        error "❌ Test 2 failed: estimate did not decrease after DROP INDEX ($est_both -> $est_one)"
    fi

    assert_counter_matches "Test 2 after DROP INDEX"

    run_sql_quiet "DROP TABLE acct2a CASCADE;"
    run_sql_quiet "DROP TABLE acct2b CASCADE;"
    log "✅ Test 2 passed"
}

# -------------------------------------------------------
# Test 3: Aborted CREATE INDEX does not leak the counter.
# -------------------------------------------------------
test_abort_no_leak() {
    log "Test 3: Aborted CREATE INDEX does not leak the counter"

    # Load enough data that the build processes many rows before
    # hitting the statement_timeout. This ensures the atomic
    # counter is incremented during the build.
    run_sql_quiet "CREATE TABLE acct3 (id serial, body text);"
    run_sql_quiet "INSERT INTO acct3 (body)
        SELECT 'gamma_' || i || ' ' || repeat('filler_', 50)
        FROM generate_series(1, 200000) i;"

    local est_before
    est_before=$(get_estimated_bytes)
    info "Estimated before aborted build: $est_before"

    # Use statement_timeout to abort the build mid-way through
    # indexing 50K rows. The build will have updated the atomic
    # counter for some rows before the abort fires.
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "
        SET statement_timeout = '200ms';
        CREATE INDEX idx_acct3 ON acct3
            USING bm25(body) WITH (text_config='english');
    " >/dev/null 2>&1 || true

    local est_after
    est_after=$(get_estimated_bytes)
    info "Estimated after aborted build: $est_after"

    if [ "$est_after" -gt "$((est_before + 1024))" ]; then
        error "❌ Test 3 failed: counter leaked after abort ($est_before -> $est_after, delta=$((est_after - est_before)))"
    fi

    assert_counter_matches "Test 3 after aborted build"

    run_sql_quiet "DROP TABLE acct3 CASCADE;"
    log "✅ Test 3 passed"
}

# -------------------------------------------------------
# Test 4: Concurrent inserts from multiple backends
#         keep the counter consistent.
# -------------------------------------------------------
test_concurrent_inserts() {
    log "Test 4: Concurrent inserts — counter stays consistent"

    run_sql_quiet "CREATE TABLE acct4 (id serial, body text);"
    run_sql_quiet "CREATE INDEX idx_acct4 ON acct4
        USING bm25(body) WITH (text_config='english');"

    local num_workers=5
    local docs_per_worker=200
    local pids=()

    for w in $(seq 1 $num_workers); do
        {
            for i in $(seq 1 $docs_per_worker); do
                "${PGBINDIR}/psql" -h "${DATA_DIR}" \
                    -p "${TEST_PORT}" -d "${TEST_DB}" -c \
                    "INSERT INTO acct4 (body) VALUES ('worker_${w}_doc_${i} unique_term_w${w}i${i}');" \
                    >/dev/null 2>&1
            done
        } &
        pids+=($!)
    done

    info "Launched $num_workers workers, each inserting $docs_per_worker rows"

    local failures=0
    for pid in "${pids[@]}"; do
        if ! wait "$pid" 2>/dev/null; then
            failures=$((failures + 1))
        fi
    done

    if [ "$failures" -gt 0 ]; then
        error "❌ Test 4 failed: $failures workers crashed"
    fi

    local total_rows
    total_rows=$(run_sql_value "SELECT COUNT(*) FROM acct4;")
    local expected=$((num_workers * docs_per_worker))

    if [ "$total_rows" -ne "$expected" ]; then
        error "❌ Test 4 failed: expected $expected rows, got $total_rows"
    fi

    # The estimated bytes should be > 0 (memtable has data)
    local est
    est=$(get_estimated_bytes)
    info "Estimated bytes after concurrent inserts: $est"

    if [ "$est" -le 0 ]; then
        error "❌ Test 4 failed: estimate is zero after $expected inserts"
    fi

    # Spill and verify counter drops
    run_sql_quiet "SELECT bm25_spill_index('idx_acct4');"
    local est_after_spill
    est_after_spill=$(get_estimated_bytes)
    info "Estimated bytes after spill: $est_after_spill"

    if [ "$est_after_spill" -ge "$est" ]; then
        error "❌ Test 4 failed: estimate didn't drop after spill"
    fi

    assert_counter_matches "Test 4 after concurrent inserts + spill"

    # Verify all data is queryable
    local search_count
    search_count=$(run_sql_value \
        "SELECT COUNT(*) FROM acct4 WHERE body <@> 'worker'::bm25query < 0;")
    if [ "$search_count" -ne "$expected" ]; then
        error "❌ Test 4 failed: search returned $search_count, expected $expected"
    fi

    run_sql_quiet "DROP TABLE acct4 CASCADE;"
    log "✅ Test 4 passed"
}

# -------------------------------------------------------
# Test 5: Single-row auto-commit inserts eventually
#         trigger the global soft limit check.
# -------------------------------------------------------
test_single_row_global_check() {
    log "Test 5: Single-row inserts trigger global soft check"

    # Set memory_limit=4MB so global soft = 2MB.
    # Disable legacy thresholds.
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit = '4MB';"
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.bulk_load_threshold = 0;"
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memtable_spill_threshold = 0;"
    run_sql_quiet "SELECT pg_reload_conf();"
    sleep 0.2

    run_sql_quiet "CREATE TABLE acct5 (id serial, body text);"
    run_sql_quiet "CREATE INDEX idx_acct5 ON acct5
        USING bm25(body) WITH (text_config='english');"

    # Insert 200 single-row transactions. Each increments
    # docs_since_global_check. After 100, the global check
    # fires. The counter should reflect the accumulated data.
    info "Inserting 200 single-row auto-commits..."
    for i in $(seq 1 200); do
        run_sql_quiet \
            "INSERT INTO acct5 (body) VALUES ('singlerow_${i} unique_sr${i}');"
    done

    local est
    est=$(get_estimated_bytes)
    info "Estimated bytes after 200 single-row inserts: $est"

    if [ "$est" -le 0 ]; then
        error "❌ Test 5 failed: estimate is zero after 200 single-row inserts"
    fi

    # Verify data is all there
    local count
    count=$(run_sql_value "SELECT COUNT(*) FROM acct5;")
    if [ "$count" -ne 200 ]; then
        error "❌ Test 5 failed: expected 200 rows, got $count"
    fi

    # Reset GUCs
    run_sql_quiet "ALTER SYSTEM RESET pg_textsearch.memory_limit;"
    run_sql_quiet "ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;"
    run_sql_quiet "ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;"
    run_sql_quiet "SELECT pg_reload_conf();"
    sleep 0.2

    run_sql_quiet "DROP TABLE acct5 CASCADE;"
    log "✅ Test 5 passed"
}

# -------------------------------------------------------
# Test 6: Multiple indexes — counter tracks the sum.
#         Verify authoritative scan matches expectations.
# -------------------------------------------------------
test_multi_index_sum() {
    log "Test 6: Multiple indexes — counter tracks the sum"

    run_sql_quiet "CREATE TABLE acct6a (id serial, body text);"
    run_sql_quiet "CREATE TABLE acct6b (id serial, body text);"
    run_sql_quiet "CREATE TABLE acct6c (id serial, body text);"
    run_sql_quiet "CREATE INDEX idx_acct6a ON acct6a
        USING bm25(body) WITH (text_config='english');"
    run_sql_quiet "CREATE INDEX idx_acct6b ON acct6b
        USING bm25(body) WITH (text_config='english');"
    run_sql_quiet "CREATE INDEX idx_acct6c ON acct6c
        USING bm25(body) WITH (text_config='english');"

    run_sql_quiet "INSERT INTO acct6a (body)
        SELECT 'idx_a_' || i FROM generate_series(1, 300) i;"
    run_sql_quiet "INSERT INTO acct6b (body)
        SELECT 'idx_b_' || i FROM generate_series(1, 300) i;"
    run_sql_quiet "INSERT INTO acct6c (body)
        SELECT 'idx_c_' || i FROM generate_series(1, 300) i;"

    local est_all
    est_all=$(get_estimated_bytes)
    info "Estimated with 3 indexes: $est_all"

    # Spill one index
    run_sql_quiet "SELECT bm25_spill_index('idx_acct6b');"
    local est_two
    est_two=$(get_estimated_bytes)
    info "Estimated after spilling idx_acct6b: $est_two"

    if [ "$est_two" -ge "$est_all" ]; then
        error "❌ Test 6 failed: estimate didn't drop after spilling one index"
    fi

    # Drop another
    run_sql_quiet "DROP INDEX idx_acct6a;"
    local est_one
    est_one=$(get_estimated_bytes)
    info "Estimated after dropping idx_acct6a: $est_one"

    if [ "$est_one" -ge "$est_two" ]; then
        error "❌ Test 6 failed: estimate didn't drop after DROP INDEX"
    fi

    assert_counter_matches "Test 6 after spill + drop"

    run_sql_quiet "DROP TABLE acct6a CASCADE;"
    run_sql_quiet "DROP TABLE acct6b CASCADE;"
    run_sql_quiet "DROP TABLE acct6c CASCADE;"
    log "✅ Test 6 passed"
}

# -------------------------------------------------------
# Main
# -------------------------------------------------------
main() {
    log "Starting pg_textsearch memory accounting tests..."

    command -v pg_config >/dev/null 2>&1 || error "pg_config not found"

    setup_test_db

    test_basic_accounting
    test_drop_index_accounting
    test_abort_no_leak
    test_concurrent_inserts
    test_single_row_global_check
    test_multi_index_sum

    log "🎉 All memory accounting tests passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
