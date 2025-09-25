-- Test case: scoring1
-- Generated BM25 test with 3 documents and 3 queries
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;
SET enable_seqscan = off;

-- Create test table
CREATE TABLE scoring1_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO scoring1_docs (content) VALUES ('the quick brown fox');
INSERT INTO scoring1_docs (content) VALUES ('jumped over the lazy dog');
INSERT INTO scoring1_docs (content) VALUES ('the fox and the hound');

-- Create Tapir index
CREATE INDEX scoring1_idx ON scoring1_docs USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Test query 1: 'fox'
SELECT id, content, ROUND((content <@> to_tpquery('fox', 'scoring1_idx'))::numeric, 4) as score
FROM scoring1_docs
ORDER BY content <@> to_tpquery('fox', 'scoring1_idx'), id;

-- Test query 2: 'the fox'
SELECT id, content, ROUND((content <@> to_tpquery('the fox', 'scoring1_idx'))::numeric, 4) as score
FROM scoring1_docs
ORDER BY content <@> to_tpquery('the fox', 'scoring1_idx'), id;

-- Test query 3: 'quick brown'
SELECT id, content, ROUND((content <@> to_tpquery('quick brown', 'scoring1_idx'))::numeric, 4) as score
FROM scoring1_docs
ORDER BY content <@> to_tpquery('quick brown', 'scoring1_idx'), id;

-- Cleanup
DROP TABLE scoring1_docs CASCADE;
DROP EXTENSION tapir CASCADE;
