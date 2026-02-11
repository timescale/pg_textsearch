-- Test that BM25 index scans report statistics to pg_stat_user_indexes

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create a test table with BM25 index
CREATE TABLE test_pgstats (id serial PRIMARY KEY, content text);
CREATE INDEX test_pgstats_idx ON test_pgstats
    USING bm25(content) WITH (text_config='english');

INSERT INTO test_pgstats (content) VALUES
    ('the quick brown fox'),
    ('jumped over the lazy dog'),
    ('sphinx of black quartz judge my vow');

-- Reset statistics for our index
SELECT pg_stat_reset_single_table_counters('test_pgstats_idx'::regclass);

-- Run a query that uses the BM25 index scan
SELECT content
    FROM test_pgstats
    ORDER BY content <@> to_bm25query('fox', 'test_pgstats_idx')
    LIMIT 5;

-- Check that idx_scan was incremented
-- (pg_stat counters are updated asynchronously, so force a stats flush)
SELECT pg_stat_force_next_flush();

SELECT idx_scan > 0 AS has_scan_stats,
       idx_tup_read > 0 AS has_tup_read_stats
    FROM pg_stat_user_indexes
    WHERE indexrelname = 'test_pgstats_idx';

-- Run a second query and verify the count increases
SELECT content
    FROM test_pgstats
    ORDER BY content <@> to_bm25query('dog', 'test_pgstats_idx')
    LIMIT 5;

SELECT pg_stat_force_next_flush();

SELECT idx_scan > 1 AS multiple_scans
    FROM pg_stat_user_indexes
    WHERE indexrelname = 'test_pgstats_idx';

-- Cleanup
DROP TABLE test_pgstats;
