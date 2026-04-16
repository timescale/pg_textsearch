--
-- Test transaction abort scenarios
--
-- Verify that pg_textsearch hooks handle aborted transactions gracefully.
-- The post_parse_analyze_hook fires on every SQL statement including
-- ROLLBACK. When a transaction is in an aborted state, catalog lookups
-- are forbidden, so the hook must skip its work.
--

CREATE EXTENSION pg_textsearch;

-- Scenario 1: Basic abort with no pg_textsearch objects
-- This is the original bug report (#247): ROLLBACK after an error in a
-- transaction that has nothing to do with BM25 should not crash.
BEGIN;
CREATE TABLE abort_test1 (id serial PRIMARY KEY);
SELECT 1/0;
ROLLBACK;

-- Scenario 2: Abort with a BM25 index present (created before the xact)
CREATE TABLE abort_test2 (id serial PRIMARY KEY, body text);
CREATE INDEX abort_test2_idx ON abort_test2
    USING bm25 (body) WITH (text_config = 'english');
BEGIN;
INSERT INTO abort_test2 (body) VALUES ('hello world');
SELECT 1/0;
ROLLBACK;
-- Verify the index is still usable after the abort
INSERT INTO abort_test2 (body) VALUES ('hello world');
SELECT id, ROUND((body <@> to_bm25query('hello', 'abort_test2_idx'))::numeric, 4) AS score
    FROM abort_test2
    ORDER BY body <@> to_bm25query('hello', 'abort_test2_idx')
    LIMIT 5;

-- Scenario 3: Abort during CREATE INDEX
BEGIN;
CREATE TABLE abort_test3 (id serial PRIMARY KEY, body text);
INSERT INTO abort_test3 (body) VALUES ('test document');
CREATE INDEX ON abort_test3
    USING bm25 (body) WITH (text_config = 'english');
SELECT 1/0;
ROLLBACK;
-- Table and index should not exist after rollback
SELECT count(*) FROM pg_class WHERE relname = 'abort_test3';

-- Scenario 4: Abort with DDL (DROP TABLE that has a BM25 index)
CREATE TABLE abort_test4 (id serial PRIMARY KEY, body text);
CREATE INDEX abort_test4_idx ON abort_test4
    USING bm25 (body) WITH (text_config = 'english');
INSERT INTO abort_test4 (body) VALUES ('still here');
BEGIN;
DROP TABLE abort_test4;
SELECT 1/0;
ROLLBACK;
-- Table should still exist after the aborted DROP
SET client_min_messages = warning;
SELECT count(*) FROM abort_test4;
RESET client_min_messages;

-- Scenario 5: Multiple errors in sequence
BEGIN;
SELECT 1/0;
SELECT 2/0;
ROLLBACK;

-- Scenario 6: SAVEPOINT and error within subtransaction
BEGIN;
INSERT INTO abort_test2 (body) VALUES ('before savepoint');
SAVEPOINT sp1;
SELECT 1/0;
ROLLBACK TO sp1;
INSERT INTO abort_test2 (body)
    VALUES ('after rollback to savepoint');
COMMIT;
SELECT id, body FROM abort_test2
    ORDER BY body <@> to_bm25query('savepoint', 'abort_test2_idx');

-- Scenario 7: Error in a BM25 query
BEGIN;
INSERT INTO abort_test2 (body) VALUES ('scenario seven');
-- Force an error via division by zero in the same statement
SELECT (body <@> to_bm25query('scenario', 'abort_test2_idx'))::int / 0
    FROM abort_test2;
ROLLBACK;

-- Scenario 8: Abort after BM25 query succeeds, then later stmt fails
BEGIN;
SELECT id FROM abort_test2
    ORDER BY body <@> to_bm25query('hello', 'abort_test2_idx')
    LIMIT 3;
SELECT 1/0;
ROLLBACK;

-- Scenario 9: SAVEPOINT rollback of CREATE INDEX with populated memtable
-- Exercises SubXactCallback cleanup of dshash tables in memtable
BEGIN;
CREATE TABLE abort_subxact1 (id serial PRIMARY KEY, body text);
SAVEPOINT sp1;
CREATE INDEX abort_subxact1_idx ON abort_subxact1
    USING bm25 (body) WITH (text_config = 'english');
-- Insert data to populate the memtable (creates string_hash + doc_lengths)
INSERT INTO abort_subxact1 (body) VALUES ('memtable data one');
INSERT INTO abort_subxact1 (body) VALUES ('memtable data two');
ROLLBACK TO sp1;
-- Table should still be usable without the index
INSERT INTO abort_subxact1 (body) VALUES ('after rollback');
COMMIT;
SELECT count(*) FROM abort_subxact1;
DROP TABLE abort_subxact1;

-- Scenario 10: Nested savepoints with RELEASE (promotes subxact state)
BEGIN;
CREATE TABLE abort_subxact2 (id serial PRIMARY KEY, body text);
SAVEPOINT sp1;
SAVEPOINT sp2;
CREATE INDEX abort_subxact2_idx ON abort_subxact2
    USING bm25 (body) WITH (text_config = 'english');
RELEASE SAVEPOINT sp2;
-- Index alive, state promoted from sp2 to sp1
INSERT INTO abort_subxact2 (body) VALUES ('after release');
ROLLBACK TO sp1;
-- Index (promoted to sp1) should be cleaned up
COMMIT;
SELECT count(*) FROM abort_subxact2;
DROP TABLE abort_subxact2;

-- Scenario 11: SAVEPOINT abort followed by full transaction abort
-- Validates no double-free between subxact and xact cleanup
BEGIN;
CREATE TABLE abort_subxact3 (id serial PRIMARY KEY, body text);
SAVEPOINT sp1;
CREATE INDEX abort_subxact3_idx ON abort_subxact3
    USING bm25 (body) WITH (text_config = 'english');
INSERT INTO abort_subxact3 (body) VALUES ('will be rolled back');
ROLLBACK TO sp1;
-- Subxact cleanup already ran; now abort the whole transaction
ROLLBACK;
-- Table should not exist after full rollback
SELECT count(*) FROM pg_class WHERE relname = 'abort_subxact3';

-- Cleanup
DROP TABLE abort_test2;
DROP TABLE abort_test4;
DROP EXTENSION pg_textsearch CASCADE;
