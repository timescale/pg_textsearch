-- Test for Issue #3: Search-path-sensitive type/operator lookup squatting
-- Ensures that bm25query type and <@> operators are resolved from pg_textsearch
-- schema, not from an attacker-controlled schema earlier in search_path

-- Create test table
CREATE TABLE test_schema_squatting (id INT, content TEXT);
INSERT INTO test_schema_squatting VALUES
	(1, 'PostgreSQL full text search'),
	(2, 'BM25 ranking algorithm'),
	(3, 'information retrieval');

-- Create BM25 index
CREATE INDEX idx_schema_squatting_bm25 ON test_schema_squatting
	USING bm25 (content) WITH (key='id');

-- Test 1: Normal query (baseline)
SELECT id, content FROM test_schema_squatting
	WHERE content <@> 'PostgreSQL'::bm25query
	ORDER BY content <@> 'PostgreSQL'::bm25query
	LIMIT 2;

-- Test 2: Create an evil schema with shadow types and operators
CREATE SCHEMA evil;

-- Attempt to create shadow type (should fail if pg_textsearch schema is pinned)
-- OR succeed but be ignored because type resolution is qualified
CREATE TYPE evil.bm25query AS (dummy INT);

-- Prepend evil schema to search_path to try to shadow the real bm25query
SET search_path TO evil, public, pg_textsearch;

-- Test 3: Query should still use the correct bm25query from pg_textsearch
-- (not from evil schema), because the planner uses qualified lookups
SELECT id, content FROM test_schema_squatting
	WHERE content <@> 'PostgreSQL'::bm25query
	ORDER BY content <@> 'PostgreSQL'::bm25query
	LIMIT 2;

-- Reset search_path
RESET search_path;

-- Test 4: Verify the query still works with normal search_path
SELECT id, content FROM test_schema_squatting
	WHERE content <@> 'ranking'::bm25query
	ORDER BY content <@> 'ranking'::bm25query
	LIMIT 2;

-- Clean up
DROP INDEX idx_schema_squatting_bm25;
DROP TABLE test_schema_squatting;
DROP SCHEMA evil CASCADE;
