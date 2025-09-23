-- Minimal test without ORDER BY to avoid complex sorting code paths

DROP EXTENSION IF EXISTS tapir CASCADE;
CREATE EXTENSION tapir;

DROP TABLE IF EXISTS test_docs;
CREATE TABLE test_docs (id INT, content TEXT);
INSERT INTO test_docs VALUES (1, 'database systems');

CREATE INDEX test_idx ON test_docs USING tapir(content)
WITH (text_config = 'english', k1 = 1.2, b = 0.75);

-- Test simple BM25 scoring without ORDER BY
SELECT id, content <@> to_tpquery('database', 'test_idx') as bm25_score
FROM test_docs
WHERE id = 1;