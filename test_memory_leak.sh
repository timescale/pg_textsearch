#!/bin/bash

# Memory leak test - runs many CREATE/DROP cycles while monitoring memory
# This test ensures we're properly cleaning up all resources

DB="test_tapir"
CYCLES=10000  # 10,000 cycles should reveal any leaks
REPORT_INTERVAL=100  # Report memory every 100 cycles

echo "Starting memory leak test with $CYCLES cycles..."
echo "This will take a while..."

# Create test database if it doesn't exist
psql postgres -c "DROP DATABASE IF EXISTS $DB" 2>/dev/null
psql postgres -c "CREATE DATABASE $DB"

# Create extension once to start
psql $DB -c "CREATE EXTENSION pgtextsearch"

# Get PostgreSQL backend PID for memory monitoring
BACKEND_PID=$(psql $DB -t -c "SELECT pg_backend_pid()")
echo "Monitoring PostgreSQL backend PID: $BACKEND_PID"

# Function to get memory usage in KB
get_memory_usage() {
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS - use ps to get RSS in KB
        ps -o rss= -p $1 2>/dev/null | tr -d ' '
    else
        # Linux - use /proc filesystem
        cat /proc/$1/status 2>/dev/null | grep VmRSS | awk '{print $2}'
    fi
}

# Get initial memory usage
INITIAL_MEM=$(get_memory_usage $BACKEND_PID)
echo "Initial memory: ${INITIAL_MEM} KB"
echo ""
echo "Cycle | Memory (KB) | Delta from start (KB) | Delta from last (KB)"
echo "------|-------------|----------------------|--------------------"

LAST_MEM=$INITIAL_MEM

# Run the test cycles
for i in $(seq 1 $CYCLES); do
    # Create and drop index
    psql $DB -q <<EOF
-- Create test table if it doesn't exist
CREATE TABLE IF NOT EXISTS test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Ensure we have data
INSERT INTO test_docs (content)
SELECT 'Document ' || i || ': ' || repeat('test word ', 100)
FROM generate_series(1, 10) i
ON CONFLICT DO NOTHING;

-- Create BM25 index
CREATE INDEX idx_test_content_$i ON test_docs
USING bm25 (content)
WITH (text_config = 'english');

-- Do a query to exercise the index
SELECT COUNT(*) FROM test_docs
WHERE content <@> to_bm25query('test word', 'idx_test_content_$i') > -100;

-- Drop the index
DROP INDEX idx_test_content_$i;
EOF

    if [ $? -ne 0 ]; then
        echo "Failed at cycle $i"
        exit 1
    fi

    # Every 500 cycles, truncate the table to reset
    if [ $((i % 500)) -eq 0 ]; then
        psql $DB -q -c "TRUNCATE test_docs;"
    fi

    # Report memory usage at intervals
    if [ $((i % REPORT_INTERVAL)) -eq 0 ]; then
        CURRENT_MEM=$(get_memory_usage $BACKEND_PID)
        DELTA_FROM_START=$((CURRENT_MEM - INITIAL_MEM))
        DELTA_FROM_LAST=$((CURRENT_MEM - LAST_MEM))

        printf "%5d | %11s | %20d | %19d\n" \
            $i "$CURRENT_MEM" $DELTA_FROM_START $DELTA_FROM_LAST

        # Check for significant memory growth (more than 100MB from start)
        if [ $DELTA_FROM_START -gt 102400 ]; then
            echo ""
            echo "WARNING: Memory has grown by more than 100MB!"
            echo "This might indicate a memory leak."
        fi

        LAST_MEM=$CURRENT_MEM
    fi
done

# Final memory check
echo ""
FINAL_MEM=$(get_memory_usage $BACKEND_PID)
TOTAL_DELTA=$((FINAL_MEM - INITIAL_MEM))
echo "Final memory: ${FINAL_MEM} KB"
echo "Total memory growth: ${TOTAL_DELTA} KB"

if [ $TOTAL_DELTA -gt 51200 ]; then
    echo "WARNING: Significant memory growth detected (>50MB)"
    echo "This may indicate a memory leak!"
    exit 1
else
    echo "Memory usage appears stable - no significant leaks detected!"
fi

# Cleanup
psql postgres -c "DROP DATABASE IF EXISTS $DB"
echo "Test completed successfully!"
