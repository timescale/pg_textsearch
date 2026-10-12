-- Test for Issue #1: NULL ORDER BY crash vulnerability
-- Ensures that queries with NULL in ORDER BY clause are properly rejected

-- Create test table
CREATE TABLE test_null_orderby (id INT, content TEXT);
INSERT INTO test_null_orderby VALUES
	(1, 'PostgreSQL full text search'),
	(2, 'BM25 ranking algorithm'),
	(3, 'information retrieval');

-- Create BM25 index
CREATE INDEX idx_null_orderby_bm25 ON test_null_orderby
	USING bm25 (content) WITH (key='id');

-- Test 1: Normal BM25 query with valid ORDER BY (should work)
SELECT id, content FROM test_null_orderby
	WHERE content <@> 'PostgreSQL'::bm25query
	ORDER BY content <@> 'PostgreSQL'::bm25query;

-- Test 2: Prepared statement with NULL binding in ORDER BY (should error gracefully)
-- This should fail with a clear error message, not SIGSEGV
PREPARE test_null_prep(bm25query) AS
	SELECT id, content FROM test_null_orderby
	WHERE content <@> 'PostgreSQL'::bm25query
	ORDER BY content <@> $1;

-- Execute with NULL (should get ereport ERROR, not crash)
BEGIN;
	-- This should raise an error about NULL ORDER BY clause
	SELECT id, content FROM test_null_orderby
	WHERE content <@> 'PostgreSQL'::bm25query
	ORDER BY content <@> NULL::bm25query;
EXCEPTION
	WHEN OTHERS THEN
		RAISE NOTICE 'Caught expected error: %', SQLERRM;
END;

-- Clean up
DROP INDEX idx_null_orderby_bm25;
DROP TABLE test_null_orderby;
