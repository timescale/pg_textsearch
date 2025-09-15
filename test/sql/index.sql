-- Test tapir index access method functionality

-- Load tapir extension
CREATE EXTENSION IF NOT EXISTS tapir;

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

-- Test index creation with different text_config options
CREATE INDEX docs_english_idx ON test_docs USING tapir(content) WITH (text_config='english');
CREATE INDEX docs_simple_idx ON test_docs USING tapir(content) WITH (text_config='simple', k1=1.5, b=0.8);

-- Verify indexes were created
\d+ test_docs

-- Test basic index operations
INSERT INTO test_docs (content) VALUES ('new document for testing');

-- Test that to_tpvector works with our indexes
SELECT to_tpvector('test document content', 'docs_english_idx');
SELECT to_tpvector('test document content', 'docs_simple_idx');

-- Test that index options are correctly stored and retrievable
SELECT
    i.relname as index_name,
    CASE
        WHEN i.reloptions IS NOT NULL THEN 'has_options'
        ELSE 'no_options'
    END as options_status
FROM pg_class i
WHERE i.relname IN ('docs_english_idx', 'docs_simple_idx')
ORDER BY i.relname;

-- Test vectorization with both indexes
SELECT
    'docs_english_idx' as index_name,
    to_tpvector('postgresql database system', 'docs_english_idx') as vector;

SELECT
    'docs_simple_idx' as index_name,
    to_tpvector('postgresql database system', 'docs_simple_idx') as vector;

-- Cleanup
DROP INDEX docs_english_idx;
DROP INDEX docs_simple_idx;
DROP TABLE test_docs;
