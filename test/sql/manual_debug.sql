-- Test case: manual_debug
-- Generated BM25 test with 3 documents and 4 queries
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;
SET enable_seqscan = off;

-- Create test table
CREATE TABLE manual_debug_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO manual_debug_docs (content) VALUES ('hello world');
INSERT INTO manual_debug_docs (content) VALUES ('goodbye cruel world');
INSERT INTO manual_debug_docs (content) VALUES ('goodbye nerds');

-- Create Tapir index
CREATE INDEX manual_debug_idx ON manual_debug_docs USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Test query 1: 'hello'
SELECT id, content, ROUND((content <@> to_tpquery('hello', 'manual_debug_idx'))::numeric, 4) as score
FROM manual_debug_docs
ORDER BY content <@> to_tpquery('hello', 'manual_debug_idx'), id;

-- Test query 2: 'cruel'
SELECT id, content, ROUND((content <@> to_tpquery('cruel', 'manual_debug_idx'))::numeric, 4) as score
FROM manual_debug_docs
ORDER BY content <@> to_tpquery('cruel', 'manual_debug_idx'), id;

-- Test query 3: 'world'
SELECT id, content, ROUND((content <@> to_tpquery('world', 'manual_debug_idx'))::numeric, 4) as score
FROM manual_debug_docs
ORDER BY content <@> to_tpquery('world', 'manual_debug_idx'), id;

-- Test query 4: 'goodbye'
SELECT id, content, ROUND((content <@> to_tpquery('goodbye', 'manual_debug_idx'))::numeric, 4) as score
FROM manual_debug_docs
ORDER BY content <@> to_tpquery('goodbye', 'manual_debug_idx'), id;

-- Cleanup
DROP TABLE manual_debug_docs CASCADE;
DROP EXTENSION tapir CASCADE;
