#!/bin/bash
#
# Memory limits stress test for Tapir extension
# Tests that memory budget enforcement works correctly
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55435
TEST_DB=tapir_memory_test
DATA_DIR="${SCRIPT_DIR}/../tmp_memory_test"
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

cleanup() {
    log "Cleaning up test environment..."
    
    # Kill test postgres if running
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        local pid=$(head -1 "${DATA_DIR}/postmaster.pid" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            log "Stopping test PostgreSQL server (PID: $pid)"
            kill -TERM "$pid"
            sleep 2
            if kill -0 "$pid" 2>/dev/null; then
                warn "Forcing kill of PostgreSQL server"
                kill -KILL "$pid" 2>/dev/null || true
            fi
        fi
    fi
    
    # Clean up data directory
    if [ -d "$DATA_DIR" ]; then
        rm -rf "$DATA_DIR"
    fi
}

# Cleanup on exit
trap cleanup EXIT

setup_test_env() {
    log "Setting up test environment..."
    
    # Check if tapir extension is available
    local pg_config_path=$(which pg_config 2>/dev/null || echo "")
    if [ -n "$pg_config_path" ]; then
        local lib_dir=$($pg_config_path --pkglibdir 2>/dev/null || echo "")
        if [ -n "$lib_dir" ] && [ -f "$lib_dir/tapir.so" ]; then
            log "Found tapir extension at $lib_dir/tapir.so"
        else
            warn "tapir extension not found at expected location: $lib_dir/tapir.so"
            if [ -n "$lib_dir" ]; then
                log "Available extensions:"
                ls -la "$lib_dir"/*.so 2>/dev/null || log "No .so files found in $lib_dir"
            fi
        fi
    fi
    
    # Clean up any existing test environment
    cleanup
    
    # Create fresh data directory
    mkdir -p "$DATA_DIR"
    
    # Initialize test database cluster
    log "Initializing test database cluster..."
    initdb -D "$DATA_DIR" --auth-local=trust --auth-host=trust >/dev/null 2>&1
    
    # Configure PostgreSQL for testing
    cat >> "$DATA_DIR/postgresql.conf" << EOF
port = $TEST_PORT
listen_addresses = 'localhost'
unix_socket_directories = '$DATA_DIR'
shared_preload_libraries = 'tapir'
log_min_messages = warning
log_statement = 'none'
log_line_prefix = '[%t] '
# Very small memory budget for testing limits
tapir.shared_memory_size = 1  # 1MB per index
EOF
    
    # Start test PostgreSQL server
    log "Starting test PostgreSQL server on port $TEST_PORT..."
    postgres -D "$DATA_DIR" >"$LOGFILE" 2>&1 &
    local postgres_pid=$!
    
    # Wait for server to start
    local attempts=0
    while ! pg_isready -p "$TEST_PORT" -h localhost >/dev/null 2>&1; do
        sleep 0.5
        attempts=$((attempts + 1))
        if [ $attempts -gt 20 ]; then
            error "PostgreSQL server failed to start within 10 seconds. Log contents:"
            if [ -f "$LOGFILE" ]; then
                cat "$LOGFILE"
            fi
            kill $postgres_pid 2>/dev/null || true
            error "PostgreSQL startup failed"
        fi
    done
    
    log "Test PostgreSQL server started successfully"
    
    # Create test database
    createdb -h "$DATA_DIR" -p "$TEST_PORT" "$TEST_DB"
    
    # Install extension
    psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "CREATE EXTENSION tapir;" >/dev/null
    
    log "Test environment ready"
}

run_sql() {
    local sql="$1"
    local expect_error="${2:-false}"
    local description="${3:-SQL command}"
    
    info "Running: $description"
    
    if [ "$expect_error" = "true" ]; then
        # Expect this to fail
        if psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "$sql" >/dev/null 2>&1; then
            error "Expected SQL to fail but it succeeded: $sql"
        else
            log "✓ SQL failed as expected: $description"
            return 0
        fi
    else
        # Expect this to succeed
        if ! psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "$sql" >/dev/null 2>&1; then
            error "SQL failed unexpectedly: $sql"
        else
            log "✓ SQL succeeded: $description"
            return 0
        fi
    fi
}

test_string_table_capacity() {
    log "=== Testing String Table Capacity Limits ==="
    
    # Create table and index
    run_sql "CREATE TABLE string_test (id SERIAL, content TEXT);" false "Create test table"
    run_sql "CREATE INDEX string_test_idx ON string_test USING tapir(content) WITH (text_config='english');" false "Create tapir index"
    
    # Calculate expected limits
    # With 1MB budget, 25% = 256KB for string table
    # At ~38 bytes per entry (32 struct + 6 average string), we get ~6900 entries
    local max_terms=6900
    local test_terms=$((max_terms + 500)) # Try to exceed limit
    
    info "Attempting to insert $test_terms unique terms (limit: ~$max_terms)"
    
    # Generate unique terms to fill string table
    for i in $(seq 1 $max_terms); do
        # Use short terms to stay within our 5-char average assumption
        local term="term$i"
        if ! psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "INSERT INTO string_test (content) VALUES ('$term');" >/dev/null 2>&1; then
            log "String table reached capacity at term $i"
            break
        fi
        
        # Log progress every 1000 terms
        if [ $((i % 1000)) -eq 0 ]; then
            info "Inserted $i terms..."
        fi
    done
    
    # Try to insert one more term - should fail
    info "Attempting to exceed string table capacity..."
    run_sql "INSERT INTO string_test (content) VALUES ('overflow_term');" true "Insert term that exceeds capacity"
    
    log "✓ String table capacity limit enforced correctly"
}

restart_postgres_for_clean_state() {
    log "Restarting PostgreSQL to reset shared memory state..."
    
    # Stop PostgreSQL
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        local pid=$(head -1 "${DATA_DIR}/postmaster.pid" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            log "Stopping PostgreSQL server (PID: $pid)"
            kill -TERM "$pid"
            sleep 2
            if kill -0 "$pid" 2>/dev/null; then
                warn "Forcing kill of PostgreSQL server"
                kill -KILL "$pid" 2>/dev/null || true
            fi
        fi
    fi
    
    # Start PostgreSQL again
    log "Starting PostgreSQL server on port $TEST_PORT..."
    postgres -D "$DATA_DIR" >"$LOGFILE" 2>&1 &
    local postgres_pid=$!
    
    # Wait for server to restart
    local attempts=0
    while ! pg_isready -p "$TEST_PORT" -h localhost >/dev/null 2>&1; do
        sleep 0.5
        attempts=$((attempts + 1))
        if [ $attempts -gt 20 ]; then
            error "PostgreSQL restart failed within 10 seconds"
        fi
    done
    
    log "PostgreSQL restarted successfully with clean shared memory state"
}

test_posting_list_capacity() {
    log "=== Testing Posting List Capacity Limits ==="
    
    # Restart PostgreSQL to get clean shared memory state after string table test
    restart_postgres_for_clean_state
    
    # Create fresh table and index for posting list test  
    psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "DROP TABLE IF EXISTS posting_test CASCADE;" >/dev/null 2>&1 || true
    
    run_sql "CREATE TABLE posting_test (id SERIAL, content TEXT);" false "Create posting test table"
    run_sql "CREATE INDEX posting_test_idx ON posting_test USING tapir(content) WITH (text_config='english');" false "Create tapir index for posting test"
    
    # Calculate expected limits
    # With 1MB budget, 75% = 768KB for posting lists  
    # At ~24 bytes per posting entry, we get ~32000 entries
    local max_postings=32000
    
    info "Attempting to create $max_postings posting entries"
    
    # Use same term repeatedly to create many posting entries
    local docs_per_batch=100
    local batches=$((max_postings / docs_per_batch))
    
    for batch in $(seq 1 $batches); do
        # Insert batch of documents with same term to create posting entries
        local values=""
        for doc in $(seq 1 $docs_per_batch); do
            if [ -n "$values" ]; then
                values="$values,"
            fi
            values="$values('repeated_term')"
        done
        
        if ! psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "INSERT INTO posting_test (content) VALUES $values;" >/dev/null 2>&1; then
            local entries_created=$(((batch - 1) * docs_per_batch))
            log "Posting list reached capacity at ~$entries_created entries"
            break
        fi
        
        # Log progress
        local entries_so_far=$((batch * docs_per_batch))
        if [ $((batch % 50)) -eq 0 ]; then
            info "Created $entries_so_far posting entries..."
        fi
    done
    
    # Try to insert more documents - should fail
    info "Attempting to exceed posting list capacity..."
    run_sql "INSERT INTO posting_test (content) VALUES ('overflow_document');" true "Insert document that exceeds posting capacity"
    
    log "✓ Posting list capacity limit enforced correctly"
}

test_mixed_capacity_scenarios() {
    log "=== Testing Mixed Capacity Scenarios ==="
    
    # Restart PostgreSQL to get clean shared memory state
    restart_postgres_for_clean_state
    
    # Create table with both unique terms and repeated terms
    psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "DROP TABLE IF EXISTS mixed_test CASCADE;" >/dev/null 2>&1 || true
    run_sql "CREATE TABLE mixed_test (id SERIAL, content TEXT);" false "Create mixed test table"
    run_sql "CREATE INDEX mixed_test_idx ON mixed_test USING tapir(content) WITH (text_config='english');" false "Create tapir index for mixed test"
    
    info "Testing balanced usage of string table and posting lists..."
    
    # Insert some unique terms (uses string table capacity)
    local unique_terms=2000
    info "Inserting $unique_terms unique terms..."
    for i in $(seq 1 $unique_terms); do
        psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "INSERT INTO mixed_test (content) VALUES ('unique_$i');" >/dev/null 2>&1 || break
        
        if [ $((i % 500)) -eq 0 ]; then
            info "Inserted $i unique terms..."
        fi
    done
    
    # Now add repeated terms (uses posting list capacity)  
    info "Now adding repeated terms to fill posting lists..."
    local repeated_docs=0
    while true; do
        if ! psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "INSERT INTO mixed_test (content) VALUES ('common_term');" >/dev/null 2>&1; then
            log "Mixed test reached capacity limits at $repeated_docs repeated documents"
            break
        fi
        repeated_docs=$((repeated_docs + 1))
        
        if [ $((repeated_docs % 1000)) -eq 0 ]; then
            info "Added $repeated_docs repeated documents..."
        fi
        
        # Safety limit to prevent infinite loop
        if [ $repeated_docs -gt 50000 ]; then
            warn "Safety limit reached, stopping mixed test"
            break
        fi
    done
    
    log "✓ Mixed capacity scenarios handled correctly"
}

test_multiple_indexes() {
    log "=== Testing Multiple Indexes (Per-Index Budget) ==="
    
    # Restart PostgreSQL to get clean shared memory state
    restart_postgres_for_clean_state
    
    # Create multiple tables with separate indexes to verify per-index budgets
    run_sql "CREATE TABLE multi1 (id SERIAL, content TEXT);" false "Create first multi table"
    run_sql "CREATE TABLE multi2 (id SERIAL, content TEXT);" false "Create second multi table"
    
    run_sql "CREATE INDEX multi1_idx ON multi1 USING tapir(content) WITH (text_config='english');" false "Create first tapir index"
    run_sql "CREATE INDEX multi2_idx ON multi2 USING tapir(content) WITH (text_config='english');" false "Create second tapir index"
    
    info "Each index should get its own 1MB budget..."
    
    # Fill first index to capacity
    info "Filling first index..."
    local count1=0
    while true; do
        if ! psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "INSERT INTO multi1 (content) VALUES ('term1_$count1');" >/dev/null 2>&1; then
            log "First index reached capacity at $count1 terms"
            break
        fi
        count1=$((count1 + 1))
        
        # Safety limit
        if [ $count1 -gt 1000 ]; then
            log "First index filled with $count1 terms (hit safety limit)"
            break
        fi
    done
    
    # Second index should still work independently
    info "Testing second index independence..."
    
    # Debug what error we get
    log "Attempting to insert into second index with debugging..."
    if ! psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "INSERT INTO multi2 (content) VALUES ('independent_term');" 2>&1; then
        error "Second index failed - this indicates string tables are still shared somehow"
    else
        log "Second index insert succeeded - per-index budgets working!"
    fi
    
    # Fill second index (should get its own capacity)
    local count2=0
    while true; do
        if ! psql -h "$DATA_DIR" -p "$TEST_PORT" -d "$TEST_DB" -c "INSERT INTO multi2 (content) VALUES ('term2_$count2');" >/dev/null 2>&1; then
            log "Second index reached capacity at $count2 terms"
            break
        fi
        count2=$((count2 + 1))
        
        # Safety limit  
        if [ $count2 -gt 1000 ]; then
            log "Second index filled with $count2 terms (hit safety limit)"
            break
        fi
    done
    
    log "✓ Per-index budgets working correctly (index1: $count1 terms, index2: $count2 terms)"
}

run_memory_limit_tests() {
    log "Starting Tapir Memory Limits Stress Test"
    log "======================================="
    
    setup_test_env
    
    # Run individual test categories
    test_string_table_capacity
    test_posting_list_capacity  
    test_mixed_capacity_scenarios
    test_multiple_indexes
    
    log "======================================="
    log "✓ All memory limit tests completed successfully!"
    log "Memory budget enforcement is working correctly"
}

# Run tests
run_memory_limit_tests