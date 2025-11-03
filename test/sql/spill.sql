-- Test memtable spilling to segments using tp_spill_memtable()
-- This test verifies that forced spills work correctly with small datasets

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET pg_textsearch.log_scores = true;
SET enable_seqscan = off;

-- Test 1: Basic spill with small dataset
DROP TABLE IF EXISTS spill_basic CASCADE;
CREATE TABLE spill_basic (id INT PRIMARY KEY, doc TEXT);
CREATE INDEX spill_basic_idx ON spill_basic USING bm25(doc)
  WITH (text_config='english');

INSERT INTO spill_basic VALUES
  (1, 'hello world'),
  (2, 'goodbye world'),
  (3, 'test document');

-- Query before spill
SELECT id, ROUND((doc <@> to_bm25query('world', 'spill_basic_idx'))::numeric, 4)
  as score
FROM spill_basic
ORDER BY doc <@> to_bm25query('world', 'spill_basic_idx');

-- Force memtable spill
SELECT tp_spill_memtable('spill_basic_idx');

-- Query after spill - should still work
SELECT id, ROUND((doc <@> to_bm25query('world', 'spill_basic_idx'))::numeric, 4)
  as score
FROM spill_basic
ORDER BY doc <@> to_bm25query('world', 'spill_basic_idx');

-- Test 2: Spill with updates
DROP TABLE IF EXISTS spill_update CASCADE;
CREATE TABLE spill_update (id INT PRIMARY KEY, doc TEXT);
CREATE INDEX spill_update_idx ON spill_update USING bm25(doc)
  WITH (text_config='english');

INSERT INTO spill_update VALUES
  (1, 'original document one'),
  (2, 'original document two');

-- Force spill
SELECT tp_spill_memtable('spill_update_idx');

-- Add more documents after spill
INSERT INTO spill_update VALUES
  (3, 'new document three'),
  (4, 'new document four');

-- Query should work across both memtable and segment
SELECT id, ROUND((doc <@> to_bm25query('document', 'spill_update_idx'))::numeric, 4)
  as score
FROM spill_update
ORDER BY doc <@> to_bm25query('document', 'spill_update_idx');

-- Test 3: Multiple spills
DROP TABLE IF EXISTS spill_multiple CASCADE;
CREATE TABLE spill_multiple (id INT PRIMARY KEY, doc TEXT);
CREATE INDEX spill_multiple_idx ON spill_multiple USING bm25(doc)
  WITH (text_config='english');

-- First batch
INSERT INTO spill_multiple VALUES (1, 'first batch document');
SELECT tp_spill_memtable('spill_multiple_idx');

-- Second batch
INSERT INTO spill_multiple VALUES (2, 'second batch document');
SELECT tp_spill_memtable('spill_multiple_idx');

-- Third batch
INSERT INTO spill_multiple VALUES (3, 'third batch document');

-- Query across all three (two segments + memtable)
SELECT id, ROUND((doc <@> to_bm25query('document', 'spill_multiple_idx'))::numeric, 4)
  as score
FROM spill_multiple
ORDER BY doc <@> to_bm25query('document', 'spill_multiple_idx');

-- Test 4: Spill with different query terms
DROP TABLE IF EXISTS spill_terms CASCADE;
CREATE TABLE spill_terms (id INT PRIMARY KEY, doc TEXT);
CREATE INDEX spill_terms_idx ON spill_terms USING bm25(doc)
  WITH (text_config='english');

INSERT INTO spill_terms VALUES
  (1, 'apple banana cherry'),
  (2, 'date elderberry fig');

SELECT tp_spill_memtable('spill_terms_idx');

INSERT INTO spill_terms VALUES
  (3, 'grape honeydew apple'),
  (4, 'kiwi lemon mango');

-- Query for term in segment only
SELECT id, doc, ROUND((doc <@> to_bm25query('banana', 'spill_terms_idx'))::numeric, 4)
  as score
FROM spill_terms
WHERE doc <@> to_bm25query('banana', 'spill_terms_idx') < 0
ORDER BY doc <@> to_bm25query('banana', 'spill_terms_idx');

-- Query for term in memtable only
SELECT id, doc, ROUND((doc <@> to_bm25query('kiwi', 'spill_terms_idx'))::numeric, 4)
  as score
FROM spill_terms
WHERE doc <@> to_bm25query('kiwi', 'spill_terms_idx') < 0
ORDER BY doc <@> to_bm25query('kiwi', 'spill_terms_idx');

-- Query for term in both segment and memtable
SELECT id, doc, ROUND((doc <@> to_bm25query('apple', 'spill_terms_idx'))::numeric, 4)
  as score
FROM spill_terms
WHERE doc <@> to_bm25query('apple', 'spill_terms_idx') < 0
ORDER BY doc <@> to_bm25query('apple', 'spill_terms_idx');

-- Cleanup
DROP TABLE spill_basic CASCADE;
DROP TABLE spill_update CASCADE;
DROP TABLE spill_multiple CASCADE;
DROP TABLE spill_terms CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
