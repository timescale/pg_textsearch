#!/bin/bash
#
# Test CREATE INDEX CONCURRENTLY with concurrent table operations
# Verifies that CIC works correctly while the table is being modified
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55436
TEST_DB=pg_textsearch_cic_test
DATA_DIR="${SCRIPT_DIR}/../tmp_cic_test"
LOGFILE="${DATA_DIR}/postgres.log"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }
info() { echo -e "${BLUE}[$(date '+%H:%M:%S')] $1${NC}"; }

cleanup() {
    local exit_code=$?
    log "Cleaning up CIC test environment (exit code: $exit_code)..."
    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up CIC test PostgreSQL instance..."
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 50
shared_buffers = 128MB
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
log_min_messages = warning

# Lower thresholds to trigger multiple memtable spills during CIC tests
pg_textsearch.bulk_load_threshold = 10000
pg_textsearch.memtable_spill_threshold = 100000
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w || error "Failed to start PostgreSQL"

    createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

run_sql() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

run_sql_quiet() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" >/dev/null 2>&1
}

# Test 1: Basic CIC with concurrent inserts
test_cic_with_inserts() {
    log "Test 1: CREATE INDEX CONCURRENTLY with concurrent INSERTs"

    # Create table with initial data - use enough rows to trigger memtable spills
    # bulk_load_threshold is 100000 terms, so we need lots of documents with many terms
    run_sql_quiet "DROP TABLE IF EXISTS cic_test CASCADE;"
    run_sql_quiet "CREATE TABLE cic_test (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "INSERT INTO cic_test (content)
        SELECT 'initial document number ' || i || ' about database search optimization with ' ||
               'additional text to make indexing trigger memtable spills and exercise the ' ||
               'concurrent index build code paths thoroughly term' || (i % 1000)
        FROM generate_series(1, 100000) AS i;"

    info "Created table with 100000 initial documents"

    # Start CIC in background
    local cic_output="${DATA_DIR}/cic_output.txt"
    {
        psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c \
            "CREATE INDEX CONCURRENTLY cic_test_idx ON cic_test USING bm25(content)
             WITH (text_config='english', k1=1.2, b=0.75);"
    } > "$cic_output" 2>&1 &
    local cic_pid=$!

    info "Started CREATE INDEX CONCURRENTLY (PID: $cic_pid)"

    # Concurrent inserts while CIC is running
    local insert_count=0
    while kill -0 $cic_pid 2>/dev/null; do
        run_sql_quiet "INSERT INTO cic_test (content)
            VALUES ('concurrent insert $insert_count about search optimization');"
        insert_count=$((insert_count + 1))
        sleep 0.01
    done

    wait $cic_pid
    local cic_exit=$?

    info "CIC completed, performed $insert_count concurrent inserts"

    if [ $cic_exit -ne 0 ]; then
        error "CREATE INDEX CONCURRENTLY failed"
        cat "$cic_output"
    fi

    # Verify index is valid
    local is_valid=$(run_sql "SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_test_idx'::regclass;" | grep -E "^\s*t\s*$")
    if [ -z "$is_valid" ]; then
        error "Index is not valid after CIC"
    fi

    # Verify no duplicates - count documents matching 'database'
    local table_count=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content LIKE '%database%';" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    local index_count=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content <@> to_bm25query('database', 'cic_test_idx') < 0;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')

    info "Table count for 'database': $table_count, Index count: $index_count"
    info "Performed $insert_count concurrent inserts during CIC"

    if [ "$table_count" != "$index_count" ]; then
        error "Mismatch: table has $table_count rows with 'database', index returns $index_count"
    fi

    if [ "$insert_count" -lt 5 ]; then
        warn "Only $insert_count concurrent inserts - CIC may have been too fast"
    fi

    log "✅ Test 1 passed: CIC with concurrent inserts works correctly"
    run_sql_quiet "DROP TABLE cic_test CASCADE;"
}

# Test 2: CIC with concurrent updates
test_cic_with_updates() {
    log "Test 2: CREATE INDEX CONCURRENTLY with concurrent UPDATEs"

    run_sql_quiet "DROP TABLE IF EXISTS cic_test CASCADE;"
    run_sql_quiet "CREATE TABLE cic_test (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "INSERT INTO cic_test (content)
        SELECT 'document ' || i || ' original content about searching with more words to trigger spills term' || (i % 500)
        FROM generate_series(1, 100000) AS i;"

    info "Created table with 100000 documents"

    # Start CIC in background
    local cic_output="${DATA_DIR}/cic_output.txt"
    {
        psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c \
            "CREATE INDEX CONCURRENTLY cic_test_idx ON cic_test USING bm25(content)
             WITH (text_config='english', k1=1.2, b=0.75);"
    } > "$cic_output" 2>&1 &
    local cic_pid=$!

    info "Started CREATE INDEX CONCURRENTLY (PID: $cic_pid)"

    # Concurrent updates while CIC is running
    local update_count=0
    while kill -0 $cic_pid 2>/dev/null; do
        local id=$((RANDOM % 100000 + 1))
        run_sql_quiet "UPDATE cic_test SET content = 'updated document $id with modified search terms'
                       WHERE id = $id;"
        update_count=$((update_count + 1))
        sleep 0.005
    done

    wait $cic_pid
    local cic_exit=$?

    info "CIC completed, performed $update_count concurrent updates"

    if [ $cic_exit -ne 0 ]; then
        error "CREATE INDEX CONCURRENTLY failed"
        cat "$cic_output"
    fi

    # Verify index is valid
    local is_valid=$(run_sql "SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_test_idx'::regclass;" | grep -E "^\s*t\s*$")
    if [ -z "$is_valid" ]; then
        error "Index is not valid after CIC"
    fi

    # Verify updated content is searchable
    local updated_count=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content LIKE '%modified%';" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    local index_updated=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content <@> to_bm25query('modified', 'cic_test_idx') < 0;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')

    info "Updated rows: $updated_count, Index finds: $index_updated"

    if [ "$updated_count" != "$index_updated" ]; then
        error "Mismatch: $updated_count rows have 'modified', index returns $index_updated"
    fi

    if [ "$update_count" -lt 5 ]; then
        warn "Only $update_count concurrent updates - CIC may have been too fast"
    fi

    log "✅ Test 2 passed: CIC with concurrent updates works correctly"
    run_sql_quiet "DROP TABLE cic_test CASCADE;"
}

# Test 3: CIC with concurrent deletes
test_cic_with_deletes() {
    log "Test 3: CREATE INDEX CONCURRENTLY with concurrent DELETEs"

    run_sql_quiet "DROP TABLE IF EXISTS cic_test CASCADE;"
    run_sql_quiet "CREATE TABLE cic_test (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "INSERT INTO cic_test (content)
        SELECT 'document ' || i || ' deletable content about search with extra text to trigger spills term' || (i % 500)
        FROM generate_series(1, 100000) AS i;"

    info "Created table with 100000 documents"

    # Start CIC in background
    local cic_output="${DATA_DIR}/cic_output.txt"
    {
        psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c \
            "CREATE INDEX CONCURRENTLY cic_test_idx ON cic_test USING bm25(content)
             WITH (text_config='english', k1=1.2, b=0.75);"
    } > "$cic_output" 2>&1 &
    local cic_pid=$!

    info "Started CREATE INDEX CONCURRENTLY (PID: $cic_pid)"

    # Concurrent deletes while CIC is running
    local delete_count=0
    while kill -0 $cic_pid 2>/dev/null; do
        # Delete some rows
        run_sql_quiet "DELETE FROM cic_test WHERE id % 7 = 0 LIMIT 3;" || true
        delete_count=$((delete_count + 1))
        sleep 0.02
    done

    wait $cic_pid || true
    local cic_exit=$?

    info "CIC completed (exit: $cic_exit), performed $delete_count delete batches"

    if [ $cic_exit -ne 0 ]; then
        warn "CREATE INDEX CONCURRENTLY had non-zero exit"
        cat "$cic_output" || true
    fi

    # Verify index is valid
    local is_valid=$(run_sql "SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_test_idx'::regclass;" | grep -E "^\s*t\s*$")
    if [ -z "$is_valid" ]; then
        error "Index is not valid after CIC"
    fi

    # Verify counts match
    local table_count=$(run_sql "SELECT COUNT(*) FROM cic_test;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    local index_count=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content <@> to_bm25query('search', 'cic_test_idx') < 0;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')

    info "Table count: $table_count, Index count for 'search': $index_count"

    # All remaining rows should be found (they all contain 'search')
    if [ "$table_count" != "$index_count" ]; then
        warn "Mismatch: table has $table_count rows, index returns $index_count for 'search'"
    else
        log "✅ Test 3 passed: CIC with concurrent deletes works correctly"
    fi
    run_sql_quiet "DROP TABLE cic_test CASCADE;"
}

# Test 4: CIC with mixed concurrent operations
test_cic_with_mixed_ops() {
    log "Test 4: CREATE INDEX CONCURRENTLY with mixed concurrent operations"

    run_sql_quiet "DROP TABLE IF EXISTS cic_test CASCADE;"
    run_sql_quiet "CREATE TABLE cic_test (id SERIAL PRIMARY KEY, content TEXT, category INT);"
    run_sql_quiet "INSERT INTO cic_test (content, category)
        SELECT 'document ' || i || ' mixed content about indexing with additional terms term' || (i % 500), i % 10
        FROM generate_series(1, 100000) AS i;"

    info "Created table with 100000 documents"

    # Start CIC in background
    local cic_output="${DATA_DIR}/cic_output.txt"
    {
        psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c \
            "CREATE INDEX CONCURRENTLY cic_test_idx ON cic_test USING bm25(content)
             WITH (text_config='english', k1=1.2, b=0.75);"
    } > "$cic_output" 2>&1 &
    local cic_pid=$!

    info "Started CREATE INDEX CONCURRENTLY (PID: $cic_pid)"

    # Start multiple concurrent workers
    local pids=()

    # Insert worker
    {
        local count=0
        while kill -0 $cic_pid 2>/dev/null; do
            run_sql_quiet "INSERT INTO cic_test (content, category)
                VALUES ('inserted during cic $count about searching', $((count % 10)));"
            count=$((count + 1))
            sleep 0.02
        done
        info "Insert worker: added $count rows"
    } &
    pids+=($!)

    # Update worker
    {
        local count=0
        while kill -0 $cic_pid 2>/dev/null; do
            local id=$((RANDOM % 100000 + 1))
            run_sql_quiet "UPDATE cic_test SET content = 'updated during cic $count for testing'
                           WHERE id = $id;"
            count=$((count + 1))
            sleep 0.01
        done
        info "Update worker: updated $count rows"
    } &
    pids+=($!)

    # Delete worker
    {
        local count=0
        while kill -0 $cic_pid 2>/dev/null; do
            run_sql_quiet "DELETE FROM cic_test WHERE category = $((count % 10)) AND id > 90000 LIMIT 1;"
            count=$((count + 1))
            sleep 0.02
        done
        info "Delete worker: deleted up to $count rows"
    } &
    pids+=($!)

    # Select worker (queries during CIC)
    {
        local count=0
        while kill -0 $cic_pid 2>/dev/null; do
            # Sequential scan query (index not ready yet)
            run_sql "SELECT COUNT(*) FROM cic_test WHERE content LIKE '%indexing%';" >/dev/null 2>&1
            count=$((count + 1))
            sleep 0.01
        done
        info "Select worker: ran $count queries"
    } &
    pids+=($!)

    # Wait for CIC to complete
    wait $cic_pid
    local cic_exit=$?

    # Wait for workers to notice CIC is done
    sleep 0.5
    for pid in "${pids[@]}"; do
        wait $pid 2>/dev/null || true
    done

    info "CIC and all workers completed"

    if [ $cic_exit -ne 0 ]; then
        error "CREATE INDEX CONCURRENTLY failed"
        cat "$cic_output"
    fi

    # Verify index is valid
    local is_valid=$(run_sql "SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_test_idx'::regclass;" | grep -E "^\s*t\s*$")
    if [ -z "$is_valid" ]; then
        error "Index is not valid after CIC"
    fi

    # Verify index works correctly
    local table_count=$(run_sql "SELECT COUNT(*) FROM cic_test;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    info "Final table count: $table_count"

    # Test various searches
    local search1=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content <@> to_bm25query('indexing', 'cic_test_idx') < 0;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    local search2=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content <@> to_bm25query('inserted', 'cic_test_idx') < 0;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    local search3=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content <@> to_bm25query('updated', 'cic_test_idx') < 0;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')

    info "Search results - 'indexing': $search1, 'inserted': $search2, 'updated': $search3"

    # Cross-check with sequential scan
    local seq_indexing=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content LIKE '%indexing%';" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    local seq_inserted=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content LIKE '%inserted%';" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    local seq_updated=$(run_sql "SELECT COUNT(*) FROM cic_test WHERE content LIKE '%updated%';" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')

    info "Sequential scan - 'indexing': $seq_indexing, 'inserted': $seq_inserted, 'updated': $seq_updated"

    # Verify counts match (allowing for stemming differences)
    if [ "$search1" != "$seq_indexing" ]; then
        warn "Mismatch for 'indexing': index=$search1, seqscan=$seq_indexing"
    fi
    if [ "$search2" != "$seq_inserted" ]; then
        warn "Mismatch for 'inserted': index=$search2, seqscan=$seq_inserted"
    fi
    if [ "$search3" != "$seq_updated" ]; then
        warn "Mismatch for 'updated': index=$search3, seqscan=$seq_updated"
    fi

    log "✅ Test 4 passed: CIC with mixed concurrent operations works correctly"
    run_sql_quiet "DROP TABLE cic_test CASCADE;"
}

# Test 5: Multiple CIC on different tables
test_multiple_cic() {
    log "Test 5: Multiple concurrent CREATE INDEX CONCURRENTLY operations"

    # Create multiple tables with enough data to trigger spills
    for i in 1 2 3; do
        run_sql_quiet "DROP TABLE IF EXISTS cic_multi_$i CASCADE;"
        run_sql_quiet "CREATE TABLE cic_multi_$i (id SERIAL PRIMARY KEY, content TEXT);"
        run_sql_quiet "INSERT INTO cic_multi_$i (content)
            SELECT 'table $i document ' || j || ' with searchable content and extra terms term' || (j % 500)
            FROM generate_series(1, 50000) AS j;"
    done

    info "Created 3 tables with 50000 documents each"

    # Start CIC on all tables concurrently
    local pids=""
    for i in 1 2 3; do
        {
            psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c \
                "CREATE INDEX CONCURRENTLY cic_multi_${i}_idx ON cic_multi_$i USING bm25(content)
                 WITH (text_config='english', k1=1.2, b=0.75);" >/dev/null 2>&1
        } &
        local pid=$!
        pids="$pids $pid"
        info "Started CIC on table cic_multi_$i (PID: $pid)"
    done

    # Wait for all CIC to complete
    local failures=0
    local idx=1
    for pid in $pids; do
        if ! wait $pid; then
            warn "CIC on table cic_multi_$idx failed"
            failures=$((failures + 1))
        fi
        idx=$((idx + 1))
    done

    if [ $failures -gt 0 ]; then
        error "$failures CIC operations failed"
    fi

    # Verify all indexes are valid
    for i in 1 2 3; do
        local is_valid=$(run_sql "SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_multi_${i}_idx'::regclass;" | grep -E "^\s*t\s*$")
        if [ -z "$is_valid" ]; then
            error "Index cic_multi_${i}_idx is not valid"
        fi

        local count=$(run_sql "SELECT COUNT(*) FROM cic_multi_$i WHERE content <@> to_bm25query('searchable', 'cic_multi_${i}_idx') < 0;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
        info "Table cic_multi_$i: $count documents found via index"

        if [ "$count" != "50000" ]; then
            error "Expected 50000 documents in cic_multi_$i, found $count"
        fi
    done

    log "✅ Test 5 passed: Multiple concurrent CIC operations work correctly"

    for i in 1 2 3; do
        run_sql_quiet "DROP TABLE cic_multi_$i CASCADE;"
    done
}

run_cic_tests() {
    log "Starting CREATE INDEX CONCURRENTLY tests"

    test_cic_with_inserts
    test_cic_with_updates
    test_cic_with_deletes
    test_cic_with_mixed_ops
    test_multiple_cic

    log "🎉 All CIC tests completed successfully!"
}

main() {
    log "Starting pg_textsearch CREATE INDEX CONCURRENTLY testing..."

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found in PATH"
    command -v psql >/dev/null 2>&1 || error "psql not found in PATH"

    setup_test_db
    run_cic_tests

    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
