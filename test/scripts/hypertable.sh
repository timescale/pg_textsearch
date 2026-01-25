#!/bin/bash
# Test BM25 index behavior with TimescaleDB hypertables
#
# This test is OPTIONAL - it only runs if TimescaleDB is installed.
# It tests that the planner hook correctly handles CustomScan nodes
# (like TimescaleDB's ConstraintAwareAppend) when computing BM25 scores.
#
# The test verifies that scores in SELECT expressions are correctly
# replaced with stub functions that retrieve cached scores from the
# index scan, rather than using standalone scoring (which would use
# the parent hypertable's empty index statistics).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PGPORT="${PGPORT:-5432}"
PGHOST="${PGHOST:-localhost}"
PSQL="psql -p $PGPORT -h $PGHOST -v ON_ERROR_STOP=1"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_info() { echo -e "${YELLOW}[INFO]${NC} $1"; }
echo_ok() { echo -e "${GREEN}[OK]${NC} $1"; }
echo_fail() { echo -e "${RED}[FAIL]${NC} $1"; }

# Check if TimescaleDB is available
check_timescaledb() {
    if $PSQL -d postgres -tAc "SELECT 1 FROM pg_available_extensions WHERE name = 'timescaledb'" 2>/dev/null | grep -q 1; then
        return 0
    else
        return 1
    fi
}

# Main test
main() {
    echo_info "Checking for TimescaleDB..."

    if ! check_timescaledb; then
        echo_info "TimescaleDB not available - skipping hypertable tests"
        echo_ok "Test skipped (TimescaleDB not installed)"
        exit 0
    fi

    echo_info "TimescaleDB found - running hypertable tests"

    # Create test database
    TEST_DB="hypertable_test_$$"
    $PSQL -d postgres -c "DROP DATABASE IF EXISTS $TEST_DB" 2>/dev/null || true
    $PSQL -d postgres -c "CREATE DATABASE $TEST_DB"

    cleanup() {
        $PSQL -d postgres -c "DROP DATABASE IF EXISTS $TEST_DB" 2>/dev/null || true
    }
    trap cleanup EXIT

    # Setup extensions and test table
    $PSQL -d $TEST_DB <<'EOF'
-- Setup extensions
CREATE EXTENSION IF NOT EXISTS timescaledb;
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create hypertable
CREATE TABLE hyper_docs (
    created_at TIMESTAMPTZ NOT NULL,
    content TEXT NOT NULL
);

SELECT create_hypertable('hyper_docs', 'created_at', chunk_time_interval => INTERVAL '1 month');

-- Insert test data across multiple chunks
INSERT INTO hyper_docs (created_at, content)
SELECT
    '2024-01-15'::timestamptz + (i * INTERVAL '15 days'),
    CASE (i % 5)
        WHEN 0 THEN 'database query optimization techniques'
        WHEN 1 THEN 'full text search engine'
        WHEN 2 THEN 'postgresql database administration'
        WHEN 3 THEN 'cloud computing services'
        WHEN 4 THEN 'machine learning algorithms'
    END
FROM generate_series(0, 19) i;

-- Create BM25 index
CREATE INDEX hyper_docs_bm25_idx ON hyper_docs USING bm25(content)
    WITH (text_config='english');
EOF

    echo_info "Test 1: Verify BM25 index scans are used"

    # Check that the plan uses BM25 index scans (may be via CustomScan or MergeAppend)
    PLAN=$($PSQL -d $TEST_DB -tA <<'EOF'
EXPLAIN (COSTS OFF)
SELECT content,
       -(content <@> to_bm25query('database', 'hyper_docs_bm25_idx')) as score
FROM hyper_docs
ORDER BY content <@> to_bm25query('database', 'hyper_docs_bm25_idx')
LIMIT 5;
EOF
)

    if echo "$PLAN" | grep -q "Index Scan using.*hyper_docs_bm25_idx"; then
        echo_ok "Query plan uses BM25 index scans"
    else
        echo_fail "Query plan does not use BM25 index scans"
        echo "$PLAN"
        exit 1
    fi

    echo_info "Test 2: Verify scores are non-zero"

    # Query and verify scores
    RESULT=$($PSQL -d $TEST_DB -tA <<'EOF'
SELECT COUNT(*) as cnt
FROM (
    SELECT content,
           -(content <@> to_bm25query('database', 'hyper_docs_bm25_idx')) as score
    FROM hyper_docs
    ORDER BY content <@> to_bm25query('database', 'hyper_docs_bm25_idx')
    LIMIT 5
) t
WHERE score > 0;
EOF
)

    NONZERO_COUNT=$(echo "$RESULT" | tr -d '[:space:]')

    if [ "$NONZERO_COUNT" -gt 0 ]; then
        echo_ok "Scores are non-zero ($NONZERO_COUNT results with positive scores)"
    else
        echo_fail "All scores are zero - planner hook may not be handling CustomScan correctly"

        # Show actual scores for debugging
        $PSQL -d $TEST_DB <<'EOF'
SELECT content,
       -(content <@> to_bm25query('database', 'hyper_docs_bm25_idx')) as score
FROM hyper_docs
ORDER BY content <@> to_bm25query('database', 'hyper_docs_bm25_idx')
LIMIT 5;
EOF
        exit 1
    fi

    echo_info "Test 3: Verify scores with time-based filter (common hypertable pattern)"

    # This pattern often triggers TimescaleDB's CustomScan (ConstraintAwareAppend)
    PLAN=$($PSQL -d $TEST_DB -tA <<'EOF'
EXPLAIN (COSTS OFF)
SELECT content,
       -(content <@> to_bm25query('database', 'hyper_docs_bm25_idx')) as score
FROM hyper_docs
WHERE created_at >= '2024-03-01' AND created_at < '2024-06-01'
ORDER BY content <@> to_bm25query('database', 'hyper_docs_bm25_idx')
LIMIT 5;
EOF
)

    # Check if CustomScan is in the plan (may or may not be depending on TimescaleDB version)
    if echo "$PLAN" | grep -q "Custom Scan"; then
        echo_info "Query uses CustomScan (ConstraintAwareAppend) - testing CustomScan path"
    else
        echo_info "Query uses standard plan nodes - CustomScan not triggered"
    fi

    RESULT=$($PSQL -d $TEST_DB -tA <<'EOF'
SELECT COUNT(*) as cnt
FROM (
    SELECT content,
           -(content <@> to_bm25query('database', 'hyper_docs_bm25_idx')) as score
    FROM hyper_docs
    WHERE created_at >= '2024-03-01' AND created_at < '2024-06-01'
    ORDER BY content <@> to_bm25query('database', 'hyper_docs_bm25_idx')
    LIMIT 5
) t
WHERE score > 0;
EOF
)

    NONZERO_COUNT=$(echo "$RESULT" | tr -d '[:space:]')

    if [ "$NONZERO_COUNT" -gt 0 ]; then
        echo_ok "Scores with time filter are non-zero ($NONZERO_COUNT results)"
    else
        echo_fail "Scores with time filter are zero"
        exit 1
    fi

    echo ""
    echo_ok "All hypertable tests passed!"
}

main "$@"
