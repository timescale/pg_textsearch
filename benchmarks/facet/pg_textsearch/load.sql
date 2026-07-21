-- Faceted MS-MARCO benchmark - Load and Index (pg_textsearch)
--
-- Loads an MS-MARCO passage collection, adds a synthetic categorical facet
-- column, and builds a BM25 index. Designed to measure the faceted-search
-- filter pushdown (pg_textsearch.enable_facet_pushdown) introduced in PR #408.
--
-- Scale-agnostic: works for MS-MARCO v1 (8.8M, integer ids) and v2 (138M,
-- string pids) because the passage id is stored as TEXT in both cases. v1 and
-- v2 are run separately, so a single fixed table name (msmarco_facet) is used.
--
-- The facet column is derived deterministically from the passage id with
-- hashtext(), so the assignment is identical for the pg_textsearch and
-- ParadeDB variants (both run in the same PostgreSQL): this guarantees the
-- two systems answer the exact same faceted queries over the exact same rows.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f load.sql
--
-- DATA_DIR must contain collection.tsv (passage_id<TAB>passage_text).

\set ON_ERROR_STOP on
\timing on

\echo '=== Faceted MS-MARCO - Data Loading (pg_textsearch) ==='

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

DROP TABLE IF EXISTS msmarco_facet CASCADE;
DROP TABLE IF EXISTS msmarco_facet_staging CASCADE;

-- Stage the raw passages first, then materialize the final table in a single
-- pass that computes the facet columns inline. This avoids post-load UPDATEs,
-- which would bloat the heap with dead tuples / HOT chains and add noise to the
-- measurements. passage_id is TEXT so the same schema serves v1 (int ids) and
-- v2 (string pids).
CREATE TABLE msmarco_facet_staging (
    passage_id   TEXT,
    passage_text TEXT
);

\echo 'Loading passages from collection.tsv (this may take several minutes)...'
-- Stream the TSV through awk: emit CSV of (passage_id, passage_text) so quotes
-- and stray backslashes in the text don't confuse COPY's text format.
-- \copy runs the PROGRAM on the psql client, so $DATA_DIR comes from the
-- client environment.
-- MAXROWS (client env) caps how many leading passages are loaded, so the same
-- script serves a quick CI-scale run and a 1M scale-up. Unset => full 8.84M.
\copy msmarco_facet_staging(passage_id, passage_text) FROM PROGRAM 'head -n "${MAXROWS:-8841823}" "$DATA_DIR/collection.tsv" | awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \"\\\"\" \$1 \"\\\"\", \"\\\"\" \$2 \"\\\"\"}"' WITH (FORMAT csv);

-- Materialize with the facet columns. The facet is derived from a portable
-- md5 hash of the passage id (not hashtext, whose output can vary across
-- PostgreSQL versions) so the assignment is byte-for-byte identical in the
-- ParadeDB variant, which runs in a separate PostgreSQL. abs(...bit(32)..) maps
-- each id uniformly onto 100 buckets, so the two systems answer the exact same
-- faceted queries over the exact same rows. facet_id is a uniform integer in
-- [0, 100); category is a coarse text label for realistic equality facets.
\echo 'Materializing facet table (single pass, no dead tuples)...'
-- facet_id granularity: FACET_BUCKETS (client env) sets the number of uniform
-- buckets. Default 100 => 1%-granularity facets; larger => finer sub-1%
-- selectivities (for locating the pushdown crossover). category stays a coarse
-- 10-label facet regardless, for realistic equality facets.
\getenv facet_buckets FACET_BUCKETS
\if :{?facet_buckets}
\else
  \set facet_buckets 100
\endif
\echo 'facet buckets:' :facet_buckets
CREATE TABLE msmarco_facet AS
SELECT passage_id,
       (abs(('x' || substr(md5(passage_id), 1, 8))::bit(32)::int::bigint)
            % :facet_buckets)::int AS facet_id,
       'cat_' || lpad(
           ((abs(('x' || substr(md5(passage_id), 1, 8))::bit(32)::int::bigint)
                % 100) / 10)::text, 2, '0') AS category,
       passage_text
FROM msmarco_facet_staging;

ALTER TABLE msmarco_facet ADD PRIMARY KEY (passage_id);
DROP TABLE msmarco_facet_staging;

\echo ''
\echo '=== Facet Distribution (sanity) ==='
SELECT 'rows'              AS metric, COUNT(*)::text          AS value FROM msmarco_facet
UNION ALL
SELECT 'distinct facet_id', COUNT(DISTINCT facet_id)::text    FROM msmarco_facet
UNION ALL
SELECT 'facet_id<1 (~1%)',  COUNT(*)::text FROM msmarco_facet WHERE facet_id < 1
UNION ALL
SELECT 'facet_id<5 (~5%)',  COUNT(*)::text FROM msmarco_facet WHERE facet_id < 5
UNION ALL
SELECT 'facet_id<10 (~10%)', COUNT(*)::text FROM msmarco_facet WHERE facet_id < 10
ORDER BY metric;

\echo ''
\echo '=== Building BM25 Index ==='
CREATE INDEX msmarco_facet_bm25_idx ON msmarco_facet
    USING bm25(passage_text) WITH (text_config='english');

\echo 'Force-merging segments for a deterministic single-segment layout...'
SELECT bm25_force_merge('msmarco_facet_bm25_idx');

-- Btree on the facet column: the filter pushdown builds its allow-list with an
-- index scan on this (O(matching rows)) instead of a full heap scan, which is
-- what lets the pushdown pay off at scale.
\echo 'Building btree on facet_id for the pushdown allow-list...'
CREATE INDEX msmarco_facet_facet_id_idx ON msmarco_facet (facet_id);

-- Statistics needed by the facet selectivity gate (clause_selectivity).
\echo 'ANALYZE for selectivity statistics...'
ANALYZE msmarco_facet;

\echo ''
\echo '=== Size Report ==='
SELECT 'INDEX_SIZE:' AS label,
       pg_size_pretty(pg_relation_size('msmarco_facet_bm25_idx')) AS index_size,
       pg_relation_size('msmarco_facet_bm25_idx') AS index_bytes;
SELECT 'TABLE_SIZE:' AS label,
       pg_size_pretty(pg_total_relation_size('msmarco_facet')) AS table_size,
       pg_total_relation_size('msmarco_facet') AS table_bytes;

\echo ''
\echo '=== Faceted MS-MARCO Load Complete (pg_textsearch) ==='
