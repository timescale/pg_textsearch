-- Test script to reproduce crash
DROP DATABASE IF EXISTS test_crash;
CREATE DATABASE test_crash;
\c test_crash

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
CREATE INDEX idx1 ON test_docs
USING bm25 (content)
WITH (text_config = 'english');

-- Drop first index
DROP INDEX idx1;

-- Insert more data
INSERT INTO test_docs (content)
SELECT 'Document ' || i || ': ' || repeat('test word ', 100)
FROM generate_series(11, 20) i;

-- Create second index - this might crash
CREATE INDEX idx2 ON test_docs
USING bm25 (content)
WITH (text_config = 'english');

SELECT 'SUCCESS';
