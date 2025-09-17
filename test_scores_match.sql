-- Test that both scoring paths give the same result
DROP EXTENSION IF EXISTS tapir CASCADE;
CREATE EXTENSION tapir;

CREATE TABLE test_docs (id int, content text);
INSERT INTO test_docs VALUES
    (1, 'database management system'),
    (2, 'distributed database system'),
    (3, 'management of database systems');

CREATE INDEX test_idx ON test_docs USING tapir(content) WITH (text_config='english');

-- Test <@> operator scoring
SELECT id, content,
       content <@> to_tpquery('database system', 'test_idx') as score
FROM test_docs
ORDER BY score;

-- Test ORDER BY scoring (should match)
SELECT id, content
FROM test_docs
WHERE content <@> to_tpquery('database system', 'test_idx') IS NOT NULL
ORDER BY content <@> to_tpquery('database system', 'test_idx')
LIMIT 5;

DROP TABLE test_docs CASCADE;
