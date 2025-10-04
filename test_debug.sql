-- Test to debug ItemPointer crash
\c test_debug
CREATE EXTENSION pgtextsearch;

-- Create test table with data
CREATE TABLE test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO test_docs (content)
SELECT 'Document ' || i || ': ' || repeat('test word ', 100)
FROM generate_series(1, 10) i;

-- Create first index - this should work
\echo 'Creating first index...'
CREATE INDEX idx1 ON test_docs
USING bm25 (content)
WITH (text_config = 'english');

-- Drop first index
\echo 'Dropping first index...'
DROP INDEX idx1;

-- Don't insert more data - just try to create second index
\echo 'Creating second index on same data...'
CREATE INDEX idx2 ON test_docs
USING bm25 (content)
WITH (text_config = 'english');

SELECT 'SUCCESS';
