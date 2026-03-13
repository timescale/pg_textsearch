-- Test case: quoted_identifiers
-- Tests that BM25 indexes work correctly with quoted (case-sensitive)
-- identifiers for both index names and schema names.
-- Regression test for https://github.com/timescale/pg_textsearch/issues/285
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Test 1: Uppercase index name (the reported bug)
CREATE TABLE qi_test1 (
    id SERIAL PRIMARY KEY,
    content TEXT
);
CREATE INDEX "IX_qi_test1_content" ON qi_test1 USING bm25(content)
  WITH (text_config='english');
INSERT INTO qi_test1 (content) VALUES ('lorem ipsum dolor sit amet');
INSERT INTO qi_test1 (content) VALUES ('quick brown fox jumps');
INSERT INTO qi_test1 (content) VALUES ('lorem ipsum again');
SELECT id, content FROM qi_test1 ORDER BY id;

-- Test 2: Query with uppercase index name
SET enable_seqscan = off;
SELECT id, content,
       ROUND((content <@> to_bm25query('lorem', 'IX_qi_test1_content'))::numeric, 4) AS score
FROM qi_test1
WHERE content <@> to_bm25query('lorem', 'IX_qi_test1_content') < 0
ORDER BY content <@> to_bm25query('lorem', 'IX_qi_test1_content'), id;

-- Test 3: Uppercase schema name with lowercase index
CREATE SCHEMA "MySchema";
CREATE TABLE "MySchema".docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);
CREATE INDEX idx_myschema_docs ON "MySchema".docs USING bm25(content)
  WITH (text_config='english');
INSERT INTO "MySchema".docs (content) VALUES ('hello world');
INSERT INTO "MySchema".docs (content) VALUES ('goodbye world');
SELECT id, content FROM "MySchema".docs ORDER BY id;

-- Query using schema-qualified index
SELECT id, content,
       ROUND((content <@> to_bm25query('hello',
         '"MySchema".idx_myschema_docs'))::numeric, 4) AS score
FROM "MySchema".docs
WHERE content <@> to_bm25query('hello',
  '"MySchema".idx_myschema_docs') < 0
ORDER BY content <@> to_bm25query('hello',
  '"MySchema".idx_myschema_docs'), id;

-- Test 4: Both uppercase schema and uppercase index
CREATE TABLE "MySchema".docs2 (
    id SERIAL PRIMARY KEY,
    content TEXT
);
CREATE INDEX "IX_MyDocs" ON "MySchema".docs2 USING bm25(content)
  WITH (text_config='english');
INSERT INTO "MySchema".docs2 (content) VALUES ('alpha beta gamma');
INSERT INTO "MySchema".docs2 (content) VALUES ('delta epsilon');
SELECT id, content FROM "MySchema".docs2 ORDER BY id;

-- Test 5: UPDATE with uppercase index name
UPDATE qi_test1 SET content = 'updated lorem ipsum' WHERE id = 1;
SELECT id, content FROM qi_test1 ORDER BY id;

-- Test 6: DELETE with uppercase index name
DELETE FROM qi_test1 WHERE id = 2;
SELECT id, content FROM qi_test1 ORDER BY id;

-- Test 7: VACUUM with uppercase index name
VACUUM qi_test1;
SELECT id, content FROM qi_test1 ORDER BY id;

-- Test 8: bm25_dump_index with schema-qualified uppercase index
SELECT bm25_dump_index('"MySchema"."IX_MyDocs"')::text
  ~ 'Tapir Index Debug' AS dump_works;

-- Test 9: bm25_summarize_index with schema-qualified uppercase index
SELECT bm25_summarize_index('"MySchema"."IX_MyDocs"')::text
  ~ 'total_docs: 2' AS summarize_works;

-- Cleanup
DROP TABLE qi_test1 CASCADE;
DROP TABLE "MySchema".docs CASCADE;
DROP TABLE "MySchema".docs2 CASCADE;
DROP SCHEMA "MySchema" CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
