-- Test DROP TABLE CASCADE with multiple indexes
CREATE EXTENSION IF NOT EXISTS pgtextsearch;

CREATE TABLE test_cascade (
    id serial PRIMARY KEY,
    content text
);

INSERT INTO test_cascade (content) VALUES ('test document');

-- Create multiple indexes
CREATE INDEX idx1 ON test_cascade USING bm25(content) WITH (text_config='english');
CREATE INDEX idx2 ON test_cascade USING bm25(content) WITH (text_config='simple');

-- This should drop both indexes then the table
DROP TABLE test_cascade CASCADE;

-- If we get here, test passed
SELECT 'Test passed' as result;

DROP EXTENSION pgtextsearch;
