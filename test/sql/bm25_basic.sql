-- Test case: bm25_basic
-- Generated BM25 test with 3 documents and 3 queries
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;
SET enable_seqscan = off;

-- Create test table
CREATE TABLE bm25_basic_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO bm25_basic_docs (content) VALUES ('the quick brown fox');
INSERT INTO bm25_basic_docs (content) VALUES ('jumped over the lazy dog');
INSERT INTO bm25_basic_docs (content) VALUES ('the fox and the hound');

-- Create Tapir index
CREATE INDEX bm25_basic_idx ON bm25_basic_docs USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Test query 1: 'fox'
SELECT id, content, ROUND((content <@> to_tpquery('fox', 'bm25_basic_idx'))::numeric, 4) as score
FROM bm25_basic_docs
ORDER BY content <@> to_tpquery('fox', 'bm25_basic_idx'), id;

-- Test query 2: 'the fox'
SELECT id, content, ROUND((content <@> to_tpquery('the fox', 'bm25_basic_idx'))::numeric, 4) as score
FROM bm25_basic_docs
ORDER BY content <@> to_tpquery('the fox', 'bm25_basic_idx'), id;

-- Test query 3: 'quick brown'
SELECT id, content, ROUND((content <@> to_tpquery('quick brown', 'bm25_basic_idx'))::numeric, 4) as score
FROM bm25_basic_docs
ORDER BY content <@> to_tpquery('quick brown', 'bm25_basic_idx'), id;

-- Cleanup
DROP TABLE bm25_basic_docs CASCADE;
DROP EXTENSION tapir CASCADE;
