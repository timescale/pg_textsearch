-- Test case: epsilon_handling
-- Generated BM25 test with 5 documents and 4 queries
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;
SET enable_seqscan = off;

-- Create test table
CREATE TABLE epsilon_handling_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO epsilon_handling_docs (content) VALUES ('apple banana cherry');
INSERT INTO epsilon_handling_docs (content) VALUES ('apple banana');
INSERT INTO epsilon_handling_docs (content) VALUES ('cherry');
INSERT INTO epsilon_handling_docs (content) VALUES ('banana');
INSERT INTO epsilon_handling_docs (content) VALUES ('apple cherry banana');

-- Create Tapir index
CREATE INDEX epsilon_handling_idx ON epsilon_handling_docs USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Test query 1: 'apple'
SELECT id, content, ROUND((content <@> to_tpquery('apple', 'epsilon_handling_idx'))::numeric, 4) as score
FROM epsilon_handling_docs
ORDER BY content <@> to_tpquery('apple', 'epsilon_handling_idx'), id;

-- Test query 2: 'banana'
SELECT id, content, ROUND((content <@> to_tpquery('banana', 'epsilon_handling_idx'))::numeric, 4) as score
FROM epsilon_handling_docs
ORDER BY content <@> to_tpquery('banana', 'epsilon_handling_idx'), id;

-- Test query 3: 'cherry'
SELECT id, content, ROUND((content <@> to_tpquery('cherry', 'epsilon_handling_idx'))::numeric, 4) as score
FROM epsilon_handling_docs
ORDER BY content <@> to_tpquery('cherry', 'epsilon_handling_idx'), id;

-- Test query 4: 'apple banana cherry'
SELECT id, content, ROUND((content <@> to_tpquery('apple banana cherry', 'epsilon_handling_idx'))::numeric, 4) as score
FROM epsilon_handling_docs
ORDER BY content <@> to_tpquery('apple banana cherry', 'epsilon_handling_idx'), id;

-- Cleanup
DROP TABLE epsilon_handling_docs CASCADE;
DROP EXTENSION tapir CASCADE;
