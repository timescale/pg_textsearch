-- Test basic functionality after fixing compilation issues

\echo 'Creating extension...'
DROP EXTENSION IF EXISTS tapir CASCADE;
CREATE EXTENSION tapir;

\echo 'Creating test table...'
DROP TABLE IF EXISTS test_docs;
CREATE TABLE test_docs (id INT, content TEXT);
INSERT INTO test_docs VALUES
    (1, 'database systems store data efficiently'),
    (2, 'information retrieval finds relevant documents'),
    (3, 'data structures organize information effectively');

\echo 'Creating Tapir index...'
CREATE INDEX test_idx ON test_docs USING tapir(content)
WITH (text_config = 'english', k1 = 1.2, b = 0.75);

\echo 'Testing basic BM25 scoring...'
SELECT id, content, content <@> to_tpquery('database', 'test_idx') as bm25_score
FROM test_docs
ORDER BY content <@> to_tpquery('database', 'test_idx')
LIMIT 3;

\echo 'Test completed successfully!'
