#!/bin/bash
# Test case to demonstrate DSA detach bug with multiple backends
# If the bug exists, dropping an index in one backend should break
# other indexes in different backends

set -e

echo "Testing DSA detach bug with multiple backends..."

# Start fresh
psql -c "DROP EXTENSION IF EXISTS pg_textsearch CASCADE;" 2>/dev/null || true
psql -c "DROP TABLE IF EXISTS test1, test2, test3 CASCADE;" 2>/dev/null || true

# Create extension and initial tables
psql <<EOF
CREATE EXTENSION pg_textsearch;
CREATE TABLE test1 (id int, content text);
CREATE TABLE test2 (id int, content text);
INSERT INTO test1 VALUES (1, 'hello world'), (2, 'goodbye world');
INSERT INTO test2 VALUES (1, 'foo bar'), (2, 'baz qux');
CREATE INDEX idx1 ON test1 USING bm25(content) WITH (text_config='english');
CREATE INDEX idx2 ON test2 USING bm25(content) WITH (text_config='english');
EOF

echo "Created two indexes. Testing initial queries..."

# Backend 1: Use both indexes to cache local state
psql <<EOF
-- Cache local state for both indexes
SELECT content, content <@> to_bm25query('hello', 'idx1') as score
FROM test1 WHERE content <@> to_bm25query('hello', 'idx1') < 0;

SELECT content, content <@> to_bm25query('foo', 'idx2') as score
FROM test2 WHERE content <@> to_bm25query('foo', 'idx2') < 0;
EOF

echo "Backend 1 cached local state for both indexes."

# Backend 2: Drop idx1 - this should trigger relcache callback
echo "Backend 2 dropping idx1..."
psql -c "DROP INDEX idx1;"

# Backend 1: Try to use idx2 - if bug exists, DSA is detached and this fails
echo "Backend 1 trying to use idx2 after idx1 was dropped..."
psql <<EOF
-- This should fail if the bug exists (DSA was detached)
SELECT content, content <@> to_bm25query('foo', 'idx2') as score
FROM test2 WHERE content <@> to_bm25query('foo', 'idx2') < 0;
EOF

# Backend 3: Try to create new index - should fail if DSA is broken
echo "Backend 3 trying to create new index..."
psql <<EOF
CREATE TABLE test3 (id int, content text);
INSERT INTO test3 VALUES (1, 'new test');
CREATE INDEX idx3 ON test3 USING bm25(content) WITH (text_config='english');

-- Try to use it
SELECT content, content <@> to_bm25query('new', 'idx3') as score
FROM test3 WHERE content <@> to_bm25query('new', 'idx3') < 0;
EOF

echo "Test completed successfully - no DSA detach bug detected!"

# Cleanup
psql -c "DROP TABLE IF EXISTS test1, test2, test3 CASCADE;"
psql -c "DROP EXTENSION pg_textsearch CASCADE;"
