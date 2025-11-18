-- Basic functionality tests for pg_textsearch extension

-- Test extension creation
CREATE EXTENSION IF NOT EXISTS pg_textsearch;


-- Enable score logging for testing
SET pg_textsearch.log_scores = true;

-- Test bm25vector type exists
SELECT pg_typeof('my_index:{database:2,system:1}'::bm25vector);

-- Test bm25vector input/output
SELECT 'my_index:{database:2,system:1}'::bm25vector;

-- Test empty bm25vector
SELECT 'my_index:{}'::bm25vector;

-- Test bm25query type
SELECT pg_typeof('search terms'::bm25query);

-- Test bm25query input/output
SELECT 'search terms'::bm25query;

-- Test to_bm25query functions
SELECT to_bm25query('hello world');
SELECT to_bm25query('test query', 'my_index');

-- Test bm25 access method exists
SELECT amname FROM pg_am WHERE amname = 'bm25';

-- Test creating a bm25 index with text_config
CREATE TABLE test_docs (id SERIAL PRIMARY KEY, content TEXT);
CREATE INDEX test_tapir_idx ON test_docs USING bm25(content) WITH (text_config='english');

-- Verify index was created
SELECT indexrelid::regclass FROM pg_index
WHERE indrelid = 'test_docs'::regclass
AND indexrelid::regclass::text LIKE '%tapir%';

-- Test bm25query with new operators
INSERT INTO test_docs (content) VALUES
    ('hello world example'),
    ('database system design'),
    ('the quick brown fox'),
    ('jumped over lazy dog'),
    ('sphinx of black quartz');

-- Test text <@> bm25query operator (should work)
SELECT content, content <@> to_bm25query('hello', 'test_tapir_idx') as score

FROM test_docs
ORDER BY score
LIMIT 1;

-- Test index name mismatch error in index scan context
\set VERBOSITY terse
\set ON_ERROR_STOP off
SELECT content
FROM test_docs
ORDER BY content <@> to_bm25query('hello', 'wrong_index')
LIMIT 1;
\set ON_ERROR_STOP on
\set VERBOSITY default

-- Clean up
DROP TABLE test_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
