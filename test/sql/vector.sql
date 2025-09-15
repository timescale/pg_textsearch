-- Test tpvector type and operators functionality

-- Load tapir extension
CREATE EXTENSION IF NOT EXISTS tapir;

-- Cleanup any existing state first
DROP TABLE IF EXISTS test_docs CASCADE;

-- Setup test table
CREATE TABLE test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO test_docs (content) VALUES
    ('the quick brown fox'),
    ('jumped over the lazy dog'),
    ('sphinx of black quartz'),
    ('hello world example'),
    ('postgresql full text search');

-- Create tapir index
CREATE INDEX docs_vector_idx ON test_docs USING tapir(content) WITH (text_config='english');

-- Test tpvector I/O functions
-- Test input/output with index name format
SELECT 'docs_vector_idx:{hello:2,world:1}'::tpvector;
SELECT 'docs_vector_idx:{}'::tpvector;

-- Test to_tpvector function
SELECT to_tpvector('hello world', 'docs_vector_idx');
SELECT to_tpvector('postgresql search', 'docs_vector_idx');

-- Test to_tpvector edge cases
SELECT to_tpvector('', 'docs_vector_idx');  -- empty text
SELECT to_tpvector('hello, world! How are you? I''m fine.', 'docs_vector_idx');  -- punctuation
SELECT to_tpvector('Testing 123 with numbers and text', 'docs_vector_idx');  -- numbers

-- Test that different queries create different vectors
SELECT to_tpvector('hello', 'docs_vector_idx') = to_tpvector('world', 'docs_vector_idx');

-- Test error cases (tpvector <@> tpvector operator removed for consistency)

-- Test nonexistent index error
\set VERBOSITY terse
\set ON_ERROR_STOP off
SELECT to_tpvector('test text', 'nonexistent_index');
\set ON_ERROR_STOP on
\set VERBOSITY default

-- Test tapir scoring using standalone text <@> tpquery operations
SELECT
    content,
    ROUND((content <@> to_tpquery('hello world', 'docs_vector_idx'))::numeric, 4) as score
FROM test_docs
ORDER BY score;

-- Test different query terms with standalone text <@> tpquery operations
SELECT
    content,
    ROUND((content <@> to_tpquery('postgresql', 'docs_vector_idx'))::numeric, 4) as postgresql_score,
    ROUND((content <@> to_tpquery('search', 'docs_vector_idx'))::numeric, 4) as search_score
FROM test_docs
ORDER BY (content <@> to_tpquery('postgresql', 'docs_vector_idx'))::float8 +
         (content <@> to_tpquery('search', 'docs_vector_idx'))::float8;

-- Test vector serialization/deserialization
SELECT to_tpvector('hello', 'docs_vector_idx')::text;
SELECT to_tpvector('test word', 'docs_vector_idx')::text;

-- Test with longer text
SELECT to_tpvector('this is a longer test with multiple words and repetitions test test', 'docs_vector_idx');

-- Test tpvector equality operator
SELECT
    to_tpvector('hello world', 'docs_vector_idx') = to_tpvector('hello world', 'docs_vector_idx') as should_be_true,
    to_tpvector('hello world', 'docs_vector_idx') = to_tpvector('world hello', 'docs_vector_idx') as depends_on_order;

-- Test direct tpvector equality (including order-independence fix)
SELECT 'docs_vector_idx:{hello:1,world:2}'::tpvector = 'docs_vector_idx:{hello:1,world:2}'::tpvector;
SELECT 'docs_vector_idx:{hello:1,world:2}'::tpvector = 'docs_vector_idx:{world:2,hello:1}'::tpvector;

-- Test scoring with real documents
SELECT
    id,
    content,
    ROUND((content <@> to_tpquery('quick fox', 'docs_vector_idx'))::numeric, 4) as relevance
FROM test_docs
ORDER BY content <@> to_tpquery('quick fox', 'docs_vector_idx')  -- BM25 scoring for ranking
LIMIT 3;

-- Test with another index using simple config
CREATE INDEX docs_simple_idx ON test_docs USING tapir(content) WITH (text_config='simple');

-- Compare stemmed vs non-stemmed
SELECT
    'running' as query,
    to_tpvector('running runner runs', 'docs_vector_idx') as english_stemmed,
    to_tpvector('running runner runs', 'docs_simple_idx') as simple_no_stem;

-- Cleanup
DROP INDEX docs_vector_idx;
DROP INDEX docs_simple_idx;
DROP TABLE test_docs;
