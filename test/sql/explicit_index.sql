-- Test explicit index name in to_bm25query
-- Validates fixes for GitHub issues #183 and #194
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-----------------------------------------------------------------------
-- Issue #194: to_bm25query with explicit index ignores index column
-- Using an index on a different column should error
-----------------------------------------------------------------------

-- Create table with TWO text columns
CREATE TABLE explicit_index_test (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT
);

-- Index on 'content' column only
CREATE INDEX content_idx ON explicit_index_test USING bm25 (content)
    WITH (text_config = 'english');

-- Insert test data
INSERT INTO explicit_index_test (title, content) VALUES
    ('rust c java', 'postgres mysql'),
    ('python ruby', 'redis mongodb');

-- This should ERROR: using content_idx to search 'title' column
-- The index is on 'content' but we're searching 'title'
SELECT * FROM explicit_index_test
    ORDER BY title <@> to_bm25query('postgres', 'content_idx');

-- Positive test: using correct column should work
SELECT id, content <@> to_bm25query('postgres', 'content_idx') AS score
FROM explicit_index_test
ORDER BY score
LIMIT 10;

-- This should ERROR: left operand is a constant, not a column
SELECT 'hello' <@> to_bm25query('postgres', 'content_idx') AS score;

-- Non-BM25 index (e.g. B-tree) and heap table names must be rejected
CREATE INDEX title_btree_idx ON explicit_index_test (title);
SELECT to_bm25query('postgres', 'title_btree_idx');
SELECT to_bm25query('postgres', 'explicit_index_test');
SELECT 'title_btree_idx:postgres'::bm25query;
SELECT 'explicit_index_test:postgres'::bm25query;

-- Clean up
DROP TABLE explicit_index_test;

-----------------------------------------------------------------------
-- Issue #183: to_bm25query with explicit index name does not force
-- planner to use that index
-----------------------------------------------------------------------

-- Create table
CREATE TABLE multi_index_test (
    id SERIAL PRIMARY KEY,
    category_id INT,
    content TEXT
);

-- Insert test data - enough rows to make planner consider index choices
INSERT INTO multi_index_test (category_id, content)
SELECT
    (i % 10),
    'Les ressources humaines gèrent les employés et leurs contrats de travail'
FROM generate_series(1, 1000) i;

-- Create two BM25 indexes with different text configurations on same column
CREATE INDEX content_idx_french ON multi_index_test USING bm25 (content)
    WITH (text_config = 'french');
CREATE INDEX content_idx_simple ON multi_index_test USING bm25 (content)
    WITH (text_config = 'simple');

-- Analyze for statistics
ANALYZE multi_index_test;

-- Query explicitly requesting the French index
-- The planner should use content_idx_french as specified
EXPLAIN (COSTS OFF)
SELECT id, content <@> to_bm25query('ressources', 'content_idx_french') AS score
FROM multi_index_test
WHERE category_id = 1
ORDER BY score
LIMIT 10;

-- Issue #553: custom plans resolve parameterized index names after the
-- initial explicit-index collection. Each execution must use its index.
SET plan_cache_mode = force_custom_plan;
PREPARE explicit_prepared(text, text) AS
SELECT content <@> to_bm25query($1, $2) AS score
FROM multi_index_test
ORDER BY score
LIMIT 3;

EXPLAIN (COSTS OFF)
EXECUTE explicit_prepared('ressource', 'content_idx_french');
EXECUTE explicit_prepared('ressource', 'content_idx_french');
EXPLAIN (COSTS OFF)
EXECUTE explicit_prepared('ressource', 'content_idx_simple');
EXECUTE explicit_prepared('ressource', 'content_idx_simple');
DEALLOCATE explicit_prepared;

-- A partial index whose predicate is not implied is not a valid alternative.
CREATE INDEX content_partial_french ON multi_index_test USING bm25 (content)
    WITH (text_config = 'french') WHERE category_id = 1;
PREPARE explicit_partial(text, text) AS
SELECT id FROM multi_index_test
ORDER BY content <@> to_bm25query($1, $2)
LIMIT 3;
\set VERBOSITY sqlstate
EXPLAIN (COSTS OFF)
EXECUTE explicit_partial('ressource', 'content_partial_french');
\set VERBOSITY default
DEALLOCATE explicit_partial;
DROP INDEX content_partial_french;

-- The same requirement applies to expression indexes.
CREATE INDEX content_lower_french ON multi_index_test
    USING bm25 (lower(content)) WITH (text_config = 'french');
CREATE INDEX content_lower_simple ON multi_index_test
    USING bm25 (lower(content)) WITH (text_config = 'simple');

PREPARE explicit_expression(text, text) AS
SELECT lower(content) <@> to_bm25query($1, $2) AS score
FROM multi_index_test
ORDER BY score
LIMIT 3;

EXPLAIN (COSTS OFF)
EXECUTE explicit_expression('ressource', 'content_lower_french');
EXECUTE explicit_expression('ressource', 'content_lower_french');
EXPLAIN (COSTS OFF)
EXECUTE explicit_expression('ressource', 'content_lower_simple');
EXECUTE explicit_expression('ressource', 'content_lower_simple');
DEALLOCATE explicit_expression;
DROP INDEX content_lower_french, content_lower_simple;
RESET plan_cache_mode;

-- Positive test: without an explicit index, implicit resolution still
-- works. All matching rows share the same text, so the BM25 scores are
-- deterministic even though the tied tuple ids (and which tied index is
-- scanned) are not.
SET client_min_messages = error;
\pset format unaligned
SELECT content <@> 'ressources' AS score
FROM multi_index_test
WHERE category_id = 1
ORDER BY score
LIMIT 3;
\pset format aligned
RESET client_min_messages;

-- Positive test: explicit index matching the scanned index works
-- Drop the simple index so french is the only choice
DROP INDEX content_idx_simple;
EXPLAIN (COSTS OFF)
SELECT id, content <@> to_bm25query('ressources', 'content_idx_french') AS score
FROM multi_index_test
WHERE category_id = 1
ORDER BY score
LIMIT 10;

-- Clean up
DROP TABLE multi_index_test;

-- A named partitioned index must select its own child indexes.
CREATE TABLE multi_index_part (id INT, content TEXT)
    PARTITION BY RANGE (id);
CREATE TABLE multi_index_part_a PARTITION OF multi_index_part
    FOR VALUES FROM (0) TO (100);
CREATE TABLE multi_index_part_b PARTITION OF multi_index_part
    FOR VALUES FROM (100) TO (200);
INSERT INTO multi_index_part
SELECT i, 'search requests number ' || i
FROM generate_series(1, 199) i;
CREATE INDEX multi_part_english ON multi_index_part USING bm25 (content)
    WITH (text_config = 'english');
CREATE INDEX multi_part_simple ON multi_index_part USING bm25 (content)
    WITH (text_config = 'simple');
ANALYZE multi_index_part;

SET plan_cache_mode = force_custom_plan;
PREPARE explicit_partition(text, text) AS
SELECT count(*) FROM (
    SELECT id FROM multi_index_part
    ORDER BY content <@> to_bm25query($1, $2)
    LIMIT 3
) matches;

EXPLAIN (COSTS OFF)
EXECUTE explicit_partition('request', 'multi_part_english');
EXECUTE explicit_partition('request', 'multi_part_english');
EXPLAIN (COSTS OFF)
EXECUTE explicit_partition('request', 'multi_part_simple');
EXECUTE explicit_partition('request', 'multi_part_simple');
-- A leaf index cannot satisfy scans of its sibling partition.
\set VERBOSITY sqlstate
EXPLAIN (COSTS OFF)
EXECUTE explicit_partition('request', 'multi_index_part_a_content_idx');
\set VERBOSITY default
DEALLOCATE explicit_partition;
RESET plan_cache_mode;
DROP TABLE multi_index_part;

DROP EXTENSION pg_textsearch CASCADE;
