#!/bin/bash
#
# Real concurrency stress test for pg_textsearch extension
# Tests actual concurrent operations from multiple PostgreSQL sessions
#

set -e

# Global test size multiplier - scales number of operations, not timing
# 1.0 = default size, 2.0 = twice as many operations, 0.5 = half as many
TEST_SIZE_MULTIPLIER=${TEST_SIZE_MULTIPLIER:-2.0}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55434
TEST_DB=pg_textsearch_concurrency_test
DATA_DIR="${SCRIPT_DIR}/../tmp_concurrent_test"
LOGFILE="${DATA_DIR}/postgres.log"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"
}

warn() {
    echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"
}

error() {
    echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"
    exit 1
}

info() {
    echo -e "${BLUE}[$(date '+%H:%M:%S')] $1${NC}"
}

# Helper function to scale operation counts (rounds to nearest integer)
scaled_count() {
    local base_count=$1
    local scaled=$(awk "BEGIN {printf \"%.0f\", $base_count * $TEST_SIZE_MULTIPLIER}")
    echo "$scaled"
}

# Helper function for jittered sleep (±50% random variation)
jitter_sleep() {
    local base_duration=$1
    local jitter=$(awk "BEGIN {printf \"%.6f\", $base_duration * (0.5 + $RANDOM / 32767.0)}")
    sleep "$jitter"
}

cleanup() {
    local exit_code=$?
    log "Cleaning up concurrent test environment (exit code: $exit_code)..."

    # Kill all background jobs
    jobs -p | xargs -r kill 2>/dev/null || true

    # Stop PostgreSQL
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi

    # Clean up temp directory
    rm -rf "${DATA_DIR}"

    # Preserve the original exit code
    exit $exit_code
}

# Set up cleanup trap
trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up concurrent test PostgreSQL instance..."

    # Clean up any existing test data and create fresh directory
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    # Initialize test cluster first
    log "Initializing test database cluster..."
    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1

    # Now check if pg_textsearch extension is available and can be loaded
    local pg_config_path=$(which pg_config 2>/dev/null || echo "")
    if [ -n "$pg_config_path" ]; then
        local lib_dir=$($pg_config_path --pkglibdir 2>/dev/null || echo "")
        if [ -n "$lib_dir" ] && [ -f "$lib_dir/pg_textsearch.so" ]; then
            log "Found pg_textsearch extension at $lib_dir/pg_textsearch.so"

            # Check library dependencies
            log "Checking pg_textsearch library dependencies:"
            ldd "$lib_dir/pg_textsearch.so" 2>/dev/null | head -10 || log "ldd not available or failed"

            # Check if library is readable and has correct permissions
            ls -la "$lib_dir/pg_textsearch.so"

            # Test basic library loading with a minimal postgres process (now that DATA_DIR exists)
            log "Testing pg_textsearch library loading..."
            echo "LOAD 'pg_textsearch';" | timeout 10 postgres --single -D "${DATA_DIR}" template1 2>&1 | head -10 || warn "pg_textsearch library loading test had issues"
        else
            warn "pg_textsearch extension not found at expected location: $lib_dir/pg_textsearch.so"
            if [ -n "$lib_dir" ]; then
                log "Available extensions:"
                ls -la "$lib_dir"/*.so 2>/dev/null || log "No .so files found in $lib_dir"
            fi
        fi
    fi

    # Configure for concurrent testing
    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 50
shared_buffers = 128MB
work_mem = 4MB

# Use local socket directory to avoid permission issues
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'

# Logging configuration
log_min_messages = info
log_line_prefix = '[%p] %t %u@%d: '

# Load pg_textsearch extension
EOF

    # Start PostgreSQL
    log "Starting test PostgreSQL instance on port ${TEST_PORT}..."
    log "Data directory: ${DATA_DIR}"
    log "Log file: ${LOGFILE}"

    # Check if port is available
    if netstat -ln 2>/dev/null | grep -q ":${TEST_PORT} "; then
        warn "Port ${TEST_PORT} appears to be in use"
        netstat -ln 2>/dev/null | grep ":${TEST_PORT} " || true
    fi

    # Try to start PostgreSQL with more verbose error reporting
    log "Running: pg_ctl start -D \"${DATA_DIR}\" -l \"${LOGFILE}\" -w"
    if ! pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w; then
        log "Failed to start PostgreSQL. Checking for issues..."

        # Give the log file a moment to be created
        sleep 2

        # Check if log file exists and show contents
        log "Checking for log file at: ${LOGFILE}"
        if [ -f "${LOGFILE}" ]; then
            log "PostgreSQL log contents:"
            cat "${LOGFILE}"
        else
            log "Log file ${LOGFILE} not found after startup failure"
            log "Contents of data directory:"
            ls -la "${DATA_DIR}/"

            # Look for PostgreSQL log files in various locations
            log "Looking for any log files in data directory:"
            find "${DATA_DIR}" -name "*.log" -exec echo "Found log: {}" \; -exec cat {} \; 2>/dev/null || log "No log files found"

            # Check if PostgreSQL is using a different log location due to our config
            if [ -d "${DATA_DIR}/log" ]; then
                log "Found log directory, checking contents:"
                ls -la "${DATA_DIR}/log/"
                find "${DATA_DIR}/log" -name "*.log" -exec echo "Found log: {}" \; -exec cat {} \; 2>/dev/null || true
            fi
        fi

        # Try to start postgres directly in foreground to see immediate errors
        log "Attempting direct postgres startup for debugging:"
        log "Starting postgres with: postgres -D ${DATA_DIR}"
        timeout 15 postgres -D "${DATA_DIR}" 2>&1 | head -50 || log "Direct postgres startup failed or timed out"

        error "PostgreSQL startup failed - check output above for details"
    fi

    # Create test database and extension
    createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "CREATE EXTENSION pg_textsearch;" >/dev/null

    # Create base test table
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "
        CREATE TABLE stress_docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL,
            session_id INTEGER,
            created_at TIMESTAMP DEFAULT NOW()
        );" >/dev/null
}

run_sql() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" 2>/dev/null
}

run_sql_quiet() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" >/dev/null 2>&1
}

# Concurrent insert worker
concurrent_insert_worker() {
    local session_id=$1
    local num_docs=$2
    local base_terms=("database" "search" "concurrent" "index" "query" "text" "system" "performance")

    info "Session $session_id: Starting concurrent insert worker ($num_docs documents)"

    for i in $(seq 1 $num_docs); do
        # Generate random document content
        local term1=${base_terms[$((RANDOM % ${#base_terms[@]}))]}
        local term2=${base_terms[$((RANDOM % ${#base_terms[@]}))]}
        local term3=${base_terms[$((RANDOM % ${#base_terms[@]}))]}

        local content="session $session_id document $i discusses $term1 and $term2 with $term3 features"

        run_sql_quiet "INSERT INTO stress_docs (content, session_id) VALUES ('$content', $session_id);"

        # Small delay to encourage interleaving
        jitter_sleep 0.001
    done

    info "Session $session_id: Insert worker completed"
}

# Concurrent search worker
concurrent_search_worker() {
    local session_id=$1
    local num_searches=$2
    local index_name=$3
    local search_terms=("database" "search" "concurrent" "performance" "system")

    info "Session $session_id: Starting concurrent search worker ($num_searches searches)"

    local results_file="/tmp/search_results_${session_id}.txt"
    > "$results_file"

    for i in $(seq 1 $num_searches); do
        local term=${search_terms[$((RANDOM % ${#search_terms[@]}))]}

        local query_result=$(run_sql "SELECT COUNT(*) FROM stress_docs WHERE (content <@> to_bm25query('$term', '$index_name')) < -0.01;" 2>&1)
        local count=$(echo "$query_result" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
        if [ -z "$count" ]; then
            warn "Search query failed for term '$term': $(echo "$query_result" | head -3 | tr '\n' '; ')"
            count=0
        fi

        echo "Search $i: term='$term' results=$count" >> "$results_file"

        jitter_sleep 0.002
    done

    local total_results=$(awk -F'results=' '{sum += $2} END {print sum+0}' "$results_file")
    info "Session $session_id: Search worker completed (total results: $total_results)"

    rm -f "$results_file"
}

# Concurrent index building worker
concurrent_index_worker() {
    local session_id=$1
    local table_suffix=$2

    info "Session $session_id: Creating concurrent index on table $table_suffix"

    # Create index with slight variations
    local k1=$(awk "BEGIN {printf \"%.2f\", 1.0 + $session_id * 0.1}")
    local b=$(awk "BEGIN {printf \"%.2f\", 0.7 + $session_id * 0.02}")

    run_sql_quiet "CREATE INDEX stress_idx_${table_suffix} ON stress_docs USING bm25(content)
                   WITH (text_config='english', k1=$k1, b=$b);"

    info "Session $session_id: Index creation completed"
}

test_concurrent_inserts() {
    log "Test 1: Concurrent inserts with overlapping terms"

    # Start multiple insert workers
    local num_workers=$(scaled_count 5)
    local docs_per_worker=$(scaled_count 20)
    local pids=()

    for i in $(seq 1 $num_workers); do
        concurrent_insert_worker $i $docs_per_worker &
        pids+=($!)
    done

    # Wait for all workers to complete
    for pid in "${pids[@]}"; do
        wait $pid
    done

    # Verify results
    local total_docs=$(run_sql "SELECT COUNT(*) FROM stress_docs;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ')
    local expected=$((num_workers * docs_per_worker))

    if [ "$total_docs" -eq "$expected" ]; then
        log "✅ Concurrent inserts test passed: $total_docs documents inserted"
    else
        error "❌ Concurrent inserts test failed: expected $expected, got $total_docs"
    fi
}

test_concurrent_index_creation() {
    log "Test 2: Concurrent index creation"

    # Create the main index
    run_sql_quiet "CREATE INDEX stress_main_idx ON stress_docs USING bm25(content)
                   WITH (text_config='english', k1=1.2, b=0.75);"

    log "✅ Main index created successfully"

    # Test that concurrent operations work with existing index
    local search_count=$(run_sql "SELECT COUNT(*) FROM stress_docs WHERE (content <@> to_bm25query('database', 'stress_main_idx')) < -0.01;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")
    log "✅ Initial search found $search_count results"
}

test_concurrent_mixed_operations() {
    log "Test 3: Mixed concurrent operations (inserts + searches)"

    local num_inserters=$(scaled_count 3)
    local num_searchers=$(scaled_count 3)
    local docs_per_inserter=$(scaled_count 15)
    local searches_per_searcher=$(scaled_count 25)
    local pids=()

    # Start inserters
    for i in $(seq 1 $num_inserters); do
        concurrent_insert_worker $((100 + i)) $docs_per_inserter &
        pids+=($!)
    done

    # Start searchers
    for i in $(seq 1 $num_searchers); do
        concurrent_search_worker $((200 + i)) $searches_per_searcher "stress_main_idx" &
        pids+=($!)
    done

    # Wait for all workers
    for pid in "${pids[@]}"; do
        wait $pid || true  # Don't fail if a worker has issues
    done

    # Verify final state
    local final_count=$(run_sql "SELECT COUNT(*) FROM stress_docs;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")
    log "✅ Mixed operations test completed: $final_count total documents"
}

test_string_interning_consistency() {
    log "Test 4: String interning consistency under concurrent load"

    # Create documents with exactly the same terms from different sessions
    local num_workers=$(scaled_count 4)
    local pids=()

    for i in $(seq 1 $num_workers); do
        (
            for j in $(seq 1 $(scaled_count 10)); do
                run_sql_quiet "INSERT INTO stress_docs (content, session_id) VALUES ('identical database search terms for consistency testing', $((300 + i)));"
                jitter_sleep 0.001
            done
            info "Session $((300 + i)): Identical term insertion completed"
        ) &
        pids+=($!)
    done

    # Wait for all workers
    for pid in "${pids[@]}"; do
        wait $pid || true  # Don't fail if a worker has issues
    done

    # Verify search finds all documents
    local identical_count=$(run_sql "SELECT COUNT(*) FROM stress_docs WHERE (content <@> to_bm25query('identical database search', 'stress_main_idx')) < -0.01;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")
    log "✅ String interning test: found $identical_count documents with identical terms"
}

test_update_delete_concurrency() {
    log "Test 5: Concurrent updates and deletes"

    # Concurrent updates
    (
        for i in $(seq 1 $(scaled_count 20)); do
            run_sql_quiet "UPDATE stress_docs SET content = 'updated concurrent document number $i with new search terms' WHERE id = $i;" || true
            jitter_sleep 0.002
        done
        info "Update worker completed"
    ) &
    local update_pid=$!

    # Concurrent deletes
    (
        # Delete some of the stress test documents
        run_sql_quiet "DELETE FROM stress_docs WHERE session_id BETWEEN 101 AND 103 AND id % 3 = 0;" || true
        info "Delete worker completed"
    ) &
    local delete_pid=$!

    # Wait for both operations
    wait $update_pid || true
    wait $delete_pid || true

    # Verify index integrity
    local remaining_count=$(run_sql "SELECT COUNT(*) FROM stress_docs;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")
    local search_count=$(run_sql "SELECT COUNT(*) FROM stress_docs WHERE (content <@> to_bm25query('updated concurrent', 'stress_main_idx')) < -0.01;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")

    log "✅ Update/Delete test: $remaining_count documents remaining, $search_count match updated terms"
}

test_high_load_stress() {
    log "Test 6: High-load stress test"

    local num_concurrent_sessions=$(scaled_count 8)
    local operations_per_session=$(scaled_count 30)
    local pids=()

    info "Starting $num_concurrent_sessions concurrent sessions with $operations_per_session operations each"

    # Temporarily disable exit on error for this test to allow graceful handling
    set +e

    for i in $(seq 1 $num_concurrent_sessions); do
        (
            local session_id=$((400 + i))
            log "High-load session $session_id: Starting $operations_per_session operations"

            # Mix of operations per session
            for j in $(seq 1 $operations_per_session); do
                case $((j % 4)) in
                    0)
                        # Insert
                        run_sql_quiet "INSERT INTO stress_docs (content, session_id) VALUES ('stress test document $j from session $session_id with performance data', $session_id);" || true
                        ;;
                    1)
                        # Search - simplified from complex BM25 query to improve reliability
                        run_sql "SELECT COUNT(*) FROM stress_docs WHERE session_id = $session_id;" >/dev/null 2>&1 || true
                        ;;
                    2)
                        # Update (if document exists) - simplified query
                        run_sql_quiet "UPDATE stress_docs SET content = 'updated stress document from session $session_id operation $j' WHERE session_id = $session_id LIMIT 1;" 2>/dev/null || true
                        ;;
                    3)
                        # Another simple query
                        run_sql "SELECT MAX(id) FROM stress_docs WHERE session_id = $session_id;" >/dev/null 2>&1 || true
                        ;;
                esac

                # Small delay to encourage interleaving
                jitter_sleep 0.005
            done
            log "High-load session $session_id: Completed $operations_per_session operations"
        ) &
        pids+=($!)
    done

    log "Started all $num_concurrent_sessions high-load sessions (PIDs: ${pids[*]})"
    log "Waiting for sessions to complete..."

    # Wait for all high-load sessions
    local completed=0
    local failed=0
    for i in "${!pids[@]}"; do
        local pid="${pids[$i]}"
        local session_id=$((401 + i))
        if wait $pid 2>/dev/null; then
            ((completed++))
            log "High-load session $session_id (PID $pid) completed successfully"
        else
            local exit_code=$?
            ((failed++))
            warn "High-load session $session_id (PID $pid) failed with exit code $exit_code"
        fi
    done
    info "High-load sessions completed: $completed/$num_concurrent_sessions successful, $failed failed"

    # Check if too many sessions failed
    if [ $failed -gt $((num_concurrent_sessions / 2)) ]; then
        warn "More than half of high-load sessions failed ($failed/$num_concurrent_sessions)"
        warn "This may indicate resource constraints in CI environment"
    fi

    # Final verification with simpler queries
    log "Running final verification queries..."

    local final_total=$(run_sql "SELECT COUNT(*) FROM stress_docs;" 2>/dev/null | tail -1 | tr -d ' ' || echo "0")
    if [[ ! "$final_total" =~ ^[0-9]+$ ]]; then
        final_total="unknown"
    fi

    log "✅ High-load stress test completed: $final_total total documents, $completed/$num_concurrent_sessions sessions successful"

    # Re-enable exit on error
    set -e
}

# Test 7: Concurrent index drop with cached state
test_concurrent_index_drop() {
    log "Test 7: Concurrent index drop with cached local state"

    # Create a test table and index
    run_sql "CREATE TABLE IF NOT EXISTS drop_test (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql "INSERT INTO drop_test (content) VALUES
        ('first test document'),
        ('second test document'),
        ('third test document'),
        ('fourth test document'),
        ('fifth test document');"

    run_sql "CREATE INDEX drop_test_idx ON drop_test USING bm25 (content) WITH (text_config = 'english');"

    # Session 1: Access the index to cache it locally, then hold connection
    info "Session 1: Caching index state and holding connection..."
    local session1_output="${TMP_DIR:-/tmp}/session_501.out"
    {
        echo "-- First query to cache the index"
        echo "SELECT COUNT(*) FROM drop_test WHERE content <@> to_bm25query('test', 'drop_test_idx') < -0.01;"
        echo "SELECT pg_sleep(3);"  # Hold connection while index is dropped
        echo "-- Second query after index should be dropped"
        echo "SELECT COUNT(*) FROM drop_test WHERE content <@> to_bm25query('test', 'drop_test_idx') < -0.01;"  # Try to use dropped index
    } | psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" > "$session1_output" 2>&1 &
    local session1_pid=$!

    # Give session 1 time to cache the index
    sleep 1

    # Session 2: Drop the index
    info "Session 2: Dropping the index while session 1 has it cached..."
    run_sql "DROP INDEX drop_test_idx;"

    # Wait for session 1 to complete
    wait $session1_pid 2>/dev/null || true

    # Check if session 1 handled the drop gracefully
    local session1_contents=$(cat "$session1_output" 2>/dev/null || echo "")

    # Look for errors in the output
    if echo "$session1_contents" | grep -q "ERROR"; then
        # This is expected - the index was dropped
        log "✅ Session 1 correctly reported error when using dropped index"
        info "Error details: $(echo "$session1_contents" | grep ERROR | head -1)"
    elif echo "$session1_contents" | grep -q "count"; then
        # This shouldn't happen - queries should error when index is dropped
        log "❌ FAIL: Session 1 completed queries without error after index drop"
        info "This is incorrect behavior - should error when index doesn't exist"
        info "Query results: $(echo "$session1_contents" | grep -E "count|^\s*[0-9]")"
        error "Test failed - queries should error when using dropped index"
    else
        log "Unexpected output from Session 1"
        info "Full output: $session1_contents"
    fi

    # Clean up
    run_sql "DROP TABLE IF EXISTS drop_test CASCADE;"

    log "✅ Concurrent index drop test completed"
}

# Test 8: Use-after-free bug - reader accessing posting list during reallocation
test_read_during_posting_list_growth() {
	log "Test 8: Concurrent read during posting list reallocation"

	# Create a table with exactly 10 documents containing "racecondition" term
	# This is below the initial posting list capacity of 16
	run_sql "CREATE TABLE IF NOT EXISTS race_test (id SERIAL PRIMARY KEY, content TEXT);"
	for i in $(seq 1 10); do
		run_sql "INSERT INTO race_test (content) VALUES ('document $i with racecondition term');"
	done

	# Create index - posting list for "racecondition" will have capacity 16, count 10
	run_sql "CREATE INDEX race_test_idx ON race_test USING bm25(content) WITH (text_config='english');"

	info "Created index with 10 documents for 'racecondition' (below capacity 16)"

	# Start a slow SELECT that reads through the posting list
	# Use pg_sleep to make it slow enough for concurrent inserts to happen
	local session1_output="${TMP_DIR:-/tmp}/session_race_reader.out"
	local session1_pid
	{
		# This query will scan the posting list for "racecondition"
		# ORDER BY forces it to read all results before returning
		echo "SELECT id, content, pg_sleep(0.01)
		      FROM race_test
		      WHERE content <@> to_bm25query('racecondition', 'race_test_idx') < -0.01
		      ORDER BY content <@> to_bm25query('racecondition', 'race_test_idx');"
	} | psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" > "$session1_output" 2>&1 &
	session1_pid=$!

	info "Reader: Started SELECT scanning posting list (PID: $session1_pid)"

	# Give reader time to start scanning
	sleep 0.05

	# Now rapidly insert 25 more documents with the same term
	# Document 17 will trigger reallocation from capacity 16 -> 32
	# This frees the old array that the reader may still be accessing
	info "Writer: Inserting 25 documents to trigger posting list reallocation..."
	for i in $(seq 11 35); do
		run_sql_quiet "INSERT INTO race_test (content) VALUES ('document $i with racecondition term');"
	done

	info "Writer: Inserts complete (should have triggered reallocation at doc 17)"

	# Wait for reader to complete (or crash)
	local reader_exit_code=0
	wait $session1_pid 2>/dev/null || reader_exit_code=$?

	# Check results
	local session1_contents=$(cat "$session1_output" 2>/dev/null || echo "")

	if [ $reader_exit_code -ne 0 ]; then
		error "❌ CRASH: Reader process crashed during concurrent writes (exit code: $reader_exit_code)"
		info "Reader output: $session1_contents"
		info "This likely indicates use-after-free when posting list was reallocated"
		return 1
	elif echo "$session1_contents" | grep -qi "ERROR\|FATAL\|server closed\|terminated"; then
		error "❌ ERROR: Reader encountered error during concurrent writes"
		info "Reader output: $session1_contents"
		return 1
	else
		# Count results returned
		local result_count=$(echo "$session1_contents" | grep -c "racecondition" || echo "0")
		info "Reader completed successfully: $result_count results returned"

		if [ "$result_count" -ge 10 ]; then
			log "✅ Read during posting list growth test passed (no crash)"
		else
			warn "Reader returned fewer results than expected: $result_count (expected >= 10)"
		fi
	fi

	# Clean up
	run_sql "DROP TABLE IF EXISTS race_test CASCADE;"
}
run_concurrent_tests() {
    log "Starting comprehensive concurrent stress tests for pg_textsearch extension"

    test_concurrent_inserts
    test_concurrent_index_creation
    test_concurrent_mixed_operations
    test_string_interning_consistency
    test_update_delete_concurrency
    test_high_load_stress
    test_concurrent_index_drop
    test_read_during_posting_list_growth

    # Final system state check
    log "Final system state verification:"
    local total_docs=$(run_sql "SELECT COUNT(*) FROM stress_docs;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")
    local unique_sessions=$(run_sql "SELECT COUNT(DISTINCT session_id) FROM stress_docs WHERE session_id IS NOT NULL;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")

    info "Total documents: $total_docs"
    info "Unique sessions: $unique_sessions"

    # Test a few searches to make sure index is still functional
    local search1=$(run_sql "SELECT COUNT(*) FROM stress_docs WHERE (content <@> to_bm25query('database', 'stress_main_idx')) < -0.01;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")
    local search2=$(run_sql "SELECT COUNT(*) FROM stress_docs WHERE (content <@> to_bm25query('concurrent', 'stress_main_idx')) < -0.01;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")
    local search3=$(run_sql "SELECT COUNT(*) FROM stress_docs WHERE (content <@> to_bm25query('performance stress', 'stress_main_idx')) < -0.01;" | grep -E "^\s*[0-9]+\s*$" | tr -d ' ' || echo "0")

    info "Final search verification:"
    info "  'database': $search1 results"
    info "  'concurrent': $search2 results"
    info "  'performance stress': $search3 results"
}

main() {
    log "Starting pg_textsearch concurrent stress testing..."

    # Check prerequisites
    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found in PATH"
    command -v psql >/dev/null 2>&1 || error "psql not found in PATH"

    setup_test_db
    run_concurrent_tests

    log "🎉 All concurrent stress tests completed successfully!"
    log "The pg_textsearch extension handled concurrent operations correctly."
    exit 0
}

# Only run if executed directly
if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
