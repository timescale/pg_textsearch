-- MS MARCO insert benchmark: post-query size report.
--
-- Runs AFTER queries.sql so the query phase exercises the realistic
-- post-load state (in-flight memtable chain + cache + segments).
-- Spilling here ensures pg_relation_size reflects all data on disk
-- and bm25_summarize_index reports the final segment layout that
-- downstream metric extractors expect.

\echo ''
\echo '=== MS MARCO Insert: Post-Query Size Report ==='

-- Spill memtable to disk so pg_relation_size reflects actual index data
SELECT bm25_spill_index('msmarco_bm25_idx');

\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('msmarco_bm25_idx')
    ) as index_size,
    pg_relation_size('msmarco_bm25_idx') as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size('msmarco_passages')
    ) as table_size,
    pg_total_relation_size('msmarco_passages')
        as table_bytes;

\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('msmarco_bm25_idx');
