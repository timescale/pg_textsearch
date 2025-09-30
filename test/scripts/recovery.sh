#!/bin/bash
#
# Crash recovery test script for Tapir extension
# This script simulates actual crashes and verifies recovery functionality
#

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55435
TEST_DB=crash_recovery_test
DATA_DIR="${SCRIPT_DIR}/../tmp_crash_test"
LOGFILE="${DATA_DIR}/postgres.log"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

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

cleanup() {
    log "Cleaning up test environment..."
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit 0
}

# Set up cleanup trap
trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up temporary PostgreSQL instance..."

    # Clean up any existing test data
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    # Initialize test cluster
    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1

    # Configure test instance
    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
log_statement = 'all'
shared_buffers = 128MB
max_connections = 20
log_min_messages = notice
logging_collector = on
log_filename = 'postgres.log'
EOF

    # Start PostgreSQL
    log "Starting test PostgreSQL instance on port ${TEST_PORT}..."
    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w

    # Create test database
    createdb -p "${TEST_PORT}" "${TEST_DB}"

    # Install extension
    psql -p "${TEST_PORT}" -d "${TEST_DB}" -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

run_sql() {
    psql -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" 2>/dev/null
}

run_sql_file() {
    psql -p "${TEST_PORT}" -d "${TEST_DB}" -f "$1" 2>/dev/null
}

simulate_crash() {
    log "Simulating PostgreSQL crash (SIGKILL)..."

    # Get PostgreSQL process PID
    POSTGRES_PID=$(head -1 "${DATA_DIR}/postmaster.pid")

    if [ -n "$POSTGRES_PID" ] && kill -0 "$POSTGRES_PID" 2>/dev/null; then
        # Kill PostgreSQL immediately (simulates crash)
        kill -9 "$POSTGRES_PID"

        # Wait for process to die
        sleep 2

        # Clean up PID file if it still exists
        rm -f "${DATA_DIR}/postmaster.pid"

        log "PostgreSQL crashed (PID: $POSTGRES_PID)"
    else
        error "Could not find PostgreSQL process to kill"
    fi
}

restart_postgres() {
    log "Restarting PostgreSQL after crash..."

    # Start PostgreSQL again
    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w

    # Wait for startup
    sleep 2

    # Verify connection works
    psql -p "${TEST_PORT}" -d "${TEST_DB}" -c "SELECT 1;" >/dev/null
    log "PostgreSQL restarted successfully"
}

test_crash_recovery() {
    log "Running crash recovery test..."

    # Phase 1: Set up initial data
    log "Phase 1: Creating initial test data..."
    cat << 'EOF' | run_sql
DROP TABLE IF EXISTS crash_test_docs CASCADE;
CREATE TABLE crash_test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL,
    category VARCHAR(50)
);

INSERT INTO crash_test_docs (content, category) VALUES
('database systems and query optimization performance', 'tech'),
('full text search algorithms implementation', 'tech'),
('postgresql extension development guide', 'tech'),
('crash recovery mechanisms testing', 'tech'),
('bm25 ranking algorithm implementation details', 'tech'),
('advanced database indexing structures', 'tech'),
('search engine optimization techniques', 'tech'),
('distributed systems architecture patterns', 'tech'),
('machine learning algorithms overview', 'ai'),
('natural language processing methods', 'ai'),
('computer vision applications', 'ai'),
('deep learning neural networks', 'ai'),
('data mining and analytics', 'data'),
('big data processing frameworks', 'data'),
('real time streaming systems', 'data'),
('cloud computing infrastructure', 'cloud'),
('microservices architecture design', 'cloud'),
('container orchestration platforms', 'cloud'),
('security protocols and encryption', 'security'),
('network security best practices', 'security');

CREATE INDEX crash_idx ON crash_test_docs USING pg_textsearch(content)
  WITH (text_config='english', k1=1.2, b=0.75);
EOF

    # Verify initial search works and capture baseline results
    log "Verifying initial search functionality..."

    # Test 1: Search for 'database' - should return documents with database-related content
    INITIAL_RESULTS=$(run_sql "SELECT id, content FROM crash_test_docs ORDER BY content <@> to_tpquery('database', 'crash_idx') LIMIT 5;" | grep -E "^\s*[0-9]+\s*\|" || echo "")

    # Count results and validate
    INITIAL_COUNT=$(echo "$INITIAL_RESULTS" | wc -l | tr -d ' ')
    if [ "$INITIAL_COUNT" -eq 0 ]; then
        error "Initial search returned no results - this indicates a problem"
    fi

    log "Initial search test passed (found $INITIAL_COUNT documents)"
    log "Sample results: $(echo "$INITIAL_RESULTS" | head -2 | tr '\n' '; ')..."

    # Store expected results for comparison after recovery
    echo "$INITIAL_RESULTS" > "${DATA_DIR}/expected_results.txt"

    # Phase 2: Simulate crash during operation
    log "Phase 2: Simulating crash..."

    # Add more data that will be in WAL but not necessarily flushed
    run_sql "INSERT INTO crash_test_docs (content, category) VALUES ('crash recovery test document with database terms', 'test');" &
    INSERT_PID=$!

    # Give the insert a moment to start
    sleep 0.1

    # Crash PostgreSQL
    simulate_crash

    # Wait for insert process to complete (it will fail due to crash)
    wait $INSERT_PID 2>/dev/null || true

    # Phase 3: Recovery
    log "Phase 3: Testing recovery..."
    restart_postgres

    # Verify data consistency after crash
    log "Verifying data consistency after recovery..."

    # Test same query as before crash
    RECOVERY_RESULTS=$(run_sql "SELECT id, content FROM crash_test_docs ORDER BY content <@> to_tpquery('database', 'crash_idx') LIMIT 5;" | grep -E "^\s*[0-9]+\s*\|" || echo "")

    RECOVERY_COUNT=$(echo "$RECOVERY_RESULTS" | wc -l | tr -d ' ')
    if [ "$RECOVERY_COUNT" -eq 0 ]; then
        error "Recovery search returned no results - recovery failed!"
    fi

    # Compare results with pre-crash baseline
    if [ -f "${DATA_DIR}/expected_results.txt" ]; then
        EXPECTED_RESULTS=$(cat "${DATA_DIR}/expected_results.txt")
        if [ "$RECOVERY_RESULTS" != "$EXPECTED_RESULTS" ]; then
            warn "Search results differ after recovery:"
            warn "Expected: $(echo "$EXPECTED_RESULTS" | head -2 | tr '\n' '; ')..."
            warn "Got: $(echo "$RECOVERY_RESULTS" | head -2 | tr '\n' '; ')..."
            # Don't fail here as insert during crash might have succeeded
        else
            log "Search results match pre-crash baseline perfectly"
        fi
    fi

    log "Recovery search test passed (found $RECOVERY_COUNT documents)"

    # Test that we can still insert and search
    log "Testing post-recovery functionality..."
    run_sql "INSERT INTO crash_test_docs (content, category) VALUES ('post recovery test document with algorithms', 'test');"

    # Test search for 'algorithms' which should find multiple documents
    POST_RECOVERY_RESULTS=$(run_sql "SELECT id, content FROM crash_test_docs ORDER BY content <@> to_tpquery('algorithms', 'crash_idx') LIMIT 5;" | grep -E "^\s*[0-9]+\s*\|" || echo "")

    POST_RECOVERY_COUNT=$(echo "$POST_RECOVERY_RESULTS" | wc -l | tr -d ' ')
    if [ "$POST_RECOVERY_COUNT" -eq 0 ]; then
        error "Post-recovery search returned no results - functionality broken!"
    fi

    log "Post-recovery insert and search test passed (found $POST_RECOVERY_COUNT documents)"
    log "Sample results: $(echo "$POST_RECOVERY_RESULTS" | head -2 | tr '\n' '; ')..."

    # Test debug dump functionality after recovery - this exposes the bug
    log "Phase 4: Testing debug dump functionality after recovery..."

    # This is the critical test that should expose the recovery bug
    DEBUG_RESULT=$(run_sql "SELECT tp_debug_dump_index('crash_idx');" 2>&1 | head -5 || echo "FAILED: Debug dump crashed")

    if echo "$DEBUG_RESULT" | grep -q "FAILED\|ERROR\|connection.*lost"; then
        error "Debug dump failed after recovery - this exposes the recovery bug!"
    fi

    log "Debug dump test passed - recovery working correctly"

    # Test index rebuild scenario
    log "Phase 5: Testing index rebuild after crash..."

    # Drop and recreate index to test rebuild from heap
    run_sql "DROP INDEX crash_idx;"
    run_sql "CREATE INDEX crash_idx_rebuilt ON crash_test_docs USING pg_textsearch(content) WITH (text_config='english', k1=1.2, b=0.75);"

    # Test rebuild with 'systems' query to verify full functionality
    REBUILD_RESULTS=$(run_sql "SELECT id, content FROM crash_test_docs ORDER BY content <@> to_tpquery('systems', 'crash_idx_rebuilt') LIMIT 5;" | grep -E "^\s*[0-9]+\s*\|" || echo "")

    REBUILD_COUNT=$(echo "$REBUILD_RESULTS" | wc -l | tr -d ' ')
    if [ "$REBUILD_COUNT" -eq 0 ]; then
        error "Index rebuild search returned no results - rebuild failed!"
    fi

    log "Index rebuild test passed (found $REBUILD_COUNT documents)"
    log "Sample results: $(echo "$REBUILD_RESULTS" | head -2 | tr '\n' '; ')..."
}

main() {
    log "Starting Tapir crash recovery test..."

    # Check if we have necessary tools
    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found in PATH"
    command -v psql >/dev/null 2>&1 || error "psql not found in PATH"
    command -v createdb >/dev/null 2>&1 || error "createdb not found in PATH"
    command -v initdb >/dev/null 2>&1 || error "initdb not found in PATH"

    # Run the test
    setup_test_db
    test_crash_recovery

    log "✅ All crash recovery tests passed!"
    exit 0
}

# Only run main if script is executed directly
if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
