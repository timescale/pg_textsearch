-- Simple test to debug why "system" isn't found in doc 1
DROP EXTENSION IF EXISTS tapir CASCADE;
CREATE EXTENSION tapir;

CREATE TABLE test_doc (id int, content text);
INSERT INTO test_doc VALUES (1, 'database management system');

CREATE INDEX test_idx ON test_doc USING tapir(content) WITH (text_config='english');

SET tapir.log_scores = true;

-- This should find both "databas" and "system"
SELECT content <@> to_tpquery('database system', 'test_idx') as score
FROM test_doc;

DROP TABLE test_doc CASCADE;
