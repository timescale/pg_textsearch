-- Basic functionality tests for tapir extension

-- Test extension creation
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;

-- Test tpvector type exists
SELECT pg_typeof('my_index:{database:2,system:1}'::tpvector);

-- Test tpvector input/output
SELECT 'my_index:{database:2,system:1}'::tpvector;

-- Test empty tpvector
SELECT 'my_index:{}'::tpvector;

-- Test tpquery type
SELECT pg_typeof('search terms'::tpquery);

-- Test tpquery input/output
SELECT 'search terms'::tpquery;

-- Test to_tpquery functions
SELECT to_tpquery('hello world');
SELECT to_tpquery('test query', 'my_index');

-- Test tapir access method exists
SELECT amname FROM pg_am WHERE amname = 'tapir';

-- Test creating a tapir index with text_config
CREATE TABLE test_docs (id SERIAL PRIMARY KEY, content TEXT);
CREATE INDEX test_tapir_idx ON test_docs USING tapir(content) WITH (text_config='english');

-- Verify index was created
SELECT indexrelid::regclass FROM pg_index
WHERE indrelid = 'test_docs'::regclass
AND indexrelid::regclass::text LIKE '%tapir%';

-- Test tpquery with new operators
INSERT INTO test_docs (content) VALUES
    ('hello world example'),
    ('database system design'),
    ('the quick brown fox'),
    ('jumped over lazy dog'),
    ('sphinx of black quartz');

-- Test text <@> tpquery operator (should work)
SELECT content, content <@> to_tpquery('hello', 'test_tapir_idx') as score
FROM test_docs
ORDER BY score
LIMIT 1;

-- Test index name mismatch error in index scan context
\set VERBOSITY terse
\set ON_ERROR_STOP off
SELECT content
FROM test_docs
ORDER BY content <@> to_tpquery('hello', 'wrong_index')
LIMIT 1;
\set ON_ERROR_STOP on
\set VERBOSITY default

-- Clean up
DROP TABLE test_docs CASCADE;
