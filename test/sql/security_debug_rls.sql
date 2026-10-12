-- Test for Issue #2: DEBUG1 CTID/score leakage before RLS filtering
-- Ensures that CTID and scores are only logged when tp_log_scores is explicitly enabled

-- Create test table
CREATE TABLE test_debug_rls (id INT, content TEXT);
INSERT INTO test_debug_rls VALUES
	(1, 'PostgreSQL full text search'),
	(2, 'BM25 ranking algorithm'),
	(3, 'information retrieval'),
	(4, 'database systems');

-- Create BM25 index
CREATE INDEX idx_debug_rls_bm25 ON test_debug_rls
	USING bm25 (content) WITH (key='id');

-- Test 1: Normal query with tp_log_scores OFF (default)
-- No CTID/score logs should appear even at DEBUG1 level
SET log_min_messages = DEBUG1;
SELECT id, content FROM test_debug_rls
	WHERE content <@> 'PostgreSQL'::bm25query
	ORDER BY content <@> 'PostgreSQL'::bm25query
	LIMIT 2;

-- Test 2: Query with tp_log_scores explicitly enabled
-- NOTICE-level logs should appear with CTID and scores
SET pg_textsearch.log_scores = ON;
SELECT id, content FROM test_debug_rls
	WHERE content <@> 'PostgreSQL'::bm25query
	ORDER BY content <@> 'PostgreSQL'::bm25query
	LIMIT 2;

-- Test 3: Disable tp_log_scores again
SET pg_textsearch.log_scores = OFF;
SELECT id, content FROM test_debug_rls
	WHERE content <@> 'ranking'::bm25query
	ORDER BY content <@> 'ranking'::bm25query
	LIMIT 2;

-- Reset logging
SET log_min_messages = NOTICE;

-- Clean up
DROP INDEX idx_debug_rls_bm25;
DROP TABLE test_debug_rls;
