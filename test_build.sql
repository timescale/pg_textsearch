-- Test index build functionality
DROP TABLE IF EXISTS test_docs;
CREATE TABLE test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert some test documents
INSERT INTO test_docs (content) VALUES
    ('quick brown fox jumps over the lazy dog'),
    ('the quick brown fox'),
    ('lazy dog sleeps all day'),
    ('brown fox is quick');

-- Test index creation (this will trigger tp_build)
DROP INDEX IF EXISTS idx_test_docs_content;
CREATE INDEX idx_test_docs_content ON test_docs USING tapir (content) WITH (text_config = 'english');

-- Check if index was created successfully
\d+ test_docs

-- Test basic query to see if it works
SELECT * FROM test_docs WHERE content <@> to_tpquery('idx_test_docs_content:fox');