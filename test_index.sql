DROP EXTENSION IF EXISTS pgtextsearch CASCADE;
DROP TABLE IF EXISTS test_docs CASCADE;

CREATE EXTENSION pgtextsearch;
CREATE TABLE test_docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO test_docs (content) VALUES
    ('hello world example'),
    ('database system design'),
    ('the quick brown fox'),
    ('jumped over lazy dog'),
    ('sphinx of black quartz'),
    ('postgresql database'),
    ('text search indexing'),
    ('full text retrieval'),
    ('information systems'),
    ('search engine technology');

CREATE INDEX docs_english_idx ON test_docs USING bm25(content) WITH (text_config='english');
CREATE INDEX docs_simple_idx ON test_docs USING bm25(content) WITH (text_config='simple', k1=1.5, b=0.8);

-- Test the function that crashes
SELECT to_bm25vector('test document content', 'docs_english_idx');
