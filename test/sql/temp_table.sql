--
-- Tests for BM25 indexes on temporary tables.
-- Regression test for GitHub issue #247: crash on rollback after
-- scanning a BM25 index on a temp table.
--
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Basic: create, populate, query, drop
CREATE TEMP TABLE t_basic (id serial, body text);
INSERT INTO t_basic VALUES (DEFAULT, 'hello world'), (DEFAULT, 'foo bar baz');
CREATE INDEX t_basic_idx ON t_basic USING bm25(body) WITH (text_config = 'english');
SELECT id, body <@> 'hello' AS score FROM t_basic ORDER BY score LIMIT 5;
DROP TABLE t_basic;

-- Rollback to savepoint after scan (original crash from #247)
BEGIN;
CREATE TEMP TABLE t_sp (id serial, body text);
INSERT INTO t_sp VALUES (DEFAULT, 'hello world'), (DEFAULT, 'foo bar baz');
CREATE INDEX t_sp_idx ON t_sp USING bm25(body) WITH (text_config = 'english');
SAVEPOINT sp1;
SELECT body <@> 'hello' FROM t_sp LIMIT 1;
ROLLBACK TO SAVEPOINT sp1;
-- Table and index should still be usable after savepoint rollback
SELECT body <@> 'hello' FROM t_sp LIMIT 1;
COMMIT;
DROP TABLE t_sp;

-- Full rollback after scan (second crash case from #247)
BEGIN;
CREATE TEMP TABLE t_rb (id serial, body text);
INSERT INTO t_rb VALUES (DEFAULT, 'hello world');
CREATE INDEX t_rb_idx ON t_rb USING bm25(body) WITH (text_config = 'english');
SELECT body <@> 'hello' FROM t_rb LIMIT 1;
ROLLBACK;

-- Rollback of CREATE INDEX itself
BEGIN;
CREATE TEMP TABLE t_rb2 (id serial, body text);
INSERT INTO t_rb2 VALUES (DEFAULT, 'hello world');
SAVEPOINT before_idx;
CREATE INDEX t_rb2_idx ON t_rb2 USING bm25(body) WITH (text_config = 'english');
ROLLBACK TO SAVEPOINT before_idx;
COMMIT;
DROP TABLE t_rb2;

-- ON COMMIT DROP temp table with index
BEGIN;
CREATE TEMP TABLE t_ocd (id serial, body text) ON COMMIT DROP;
INSERT INTO t_ocd VALUES (DEFAULT, 'on commit drop test');
CREATE INDEX t_ocd_idx ON t_ocd USING bm25(body) WITH (text_config = 'english');
SELECT body <@> 'test' FROM t_ocd LIMIT 1;
COMMIT;
-- Table should be gone now
SELECT * FROM t_ocd; -- expect error

DROP EXTENSION pg_textsearch CASCADE;
