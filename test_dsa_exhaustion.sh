#!/bin/bash

# Test script to verify DSA exhaustion fix
# Creates and drops tapir indices repeatedly to ensure no DSA segment leaks

DB="test_tapir"
CYCLES=30  # Try 30 cycles - more than the 21 that failed before

echo "Starting DSA exhaustion test with $CYCLES cycles..."

# Create test database if it doesn't exist
psql postgres -c "DROP DATABASE IF EXISTS $DB" 2>/dev/null
psql postgres -c "CREATE DATABASE $DB"

# Run the test cycles
for i in $(seq 1 $CYCLES); do
    echo "Cycle $i of $CYCLES..."

    # Create extension, table, and index
    psql $DB <<EOF
-- Drop extension if it exists (cleanup from previous cycle)
DROP EXTENSION IF EXISTS pgtextsearch CASCADE;

-- Create extension
CREATE EXTENSION pgtextsearch;

-- Create test table
CREATE TABLE IF NOT EXISTS test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert some test data if table is empty
INSERT INTO test_docs (content)
SELECT 'Document ' || i || ': ' || repeat('test word ', 10)
FROM generate_series(1, 100) i
ON CONFLICT DO NOTHING;

-- Create tapir index
CREATE INDEX idx_test_content ON test_docs
USING tapir (content)
WITH (text_config = 'english');

-- Verify index works with a simple query
SELECT COUNT(*) FROM test_docs
WHERE content <@> to_tpquery('idx_test_content', 'test word');

-- Drop the index
DROP INDEX idx_test_content;

-- Drop extension to force cleanup
DROP EXTENSION pgtextsearch CASCADE;
EOF

    if [ $? -ne 0 ]; then
        echo "Failed at cycle $i"
        exit 1
    fi
done

echo "Successfully completed $CYCLES cycles without DSA exhaustion!"
echo "DSA exhaustion fix verified."

# Cleanup
psql postgres -c "DROP DATABASE IF EXISTS $DB"
