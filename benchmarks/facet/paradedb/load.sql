-- Faceted MS-MARCO benchmark - Load and Index (ParadeDB / pg_search)
--
-- Mirror of ../pg_textsearch/load.sql for ParadeDB's pg_search. Loads the same
-- MS-MARCO collection, derives the SAME synthetic facet column (identical md5
-- expression, so the assignment is byte-for-byte identical to the pg_textsearch
-- variant), and builds a pg_search BM25 index that indexes the facet column as
-- a numeric fast field so the facet filter can be pushed natively into the
-- index -- the apples-to-apples counterpart of pg_textsearch's facet pushdown.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -U postgres -h <host> -p <port> -f load.sql
--
-- DATA_DIR must contain collection.tsv (passage_id<TAB>passage_text).

\set ON_ERROR_STOP on
\timing on

\echo '=== Faceted MS-MARCO - Data Loading (ParadeDB) ==='

CREATE EXTENSION IF NOT EXISTS pg_search;

DROP TABLE IF EXISTS msmarco_facet_pdb CASCADE;
DROP TABLE IF EXISTS msmarco_facet_pdb_staging CASCADE;

CREATE TABLE msmarco_facet_pdb_staging (
    passage_id   TEXT,
    passage_text TEXT
);

\echo 'Loading passages from collection.tsv (this may take several minutes)...'
\copy msmarco_facet_pdb_staging(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \"\\\"\" \$1 \"\\\"\", \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Identical md5-based facet derivation as the pg_textsearch variant so both
-- systems answer the exact same faceted queries over the exact same rows.
\echo 'Materializing facet table (single pass)...'
CREATE TABLE msmarco_facet_pdb AS
SELECT passage_id,
       (abs(('x' || substr(md5(passage_id), 1, 8))::bit(32)::int::bigint)
            % 100)::int AS facet_id,
       'cat_' || lpad(
           ((abs(('x' || substr(md5(passage_id), 1, 8))::bit(32)::int::bigint)
                % 100) / 10)::text, 2, '0') AS category,
       passage_text
FROM msmarco_facet_pdb_staging;

ALTER TABLE msmarco_facet_pdb ADD PRIMARY KEY (passage_id);
DROP TABLE msmarco_facet_pdb_staging;

\echo ''
\echo '=== Facet Distribution (sanity) ==='
SELECT 'rows'              AS metric, COUNT(*)::text       AS value FROM msmarco_facet_pdb
UNION ALL
SELECT 'facet_id<1 (~1%)',  COUNT(*)::text FROM msmarco_facet_pdb WHERE facet_id < 1
UNION ALL
SELECT 'facet_id<5 (~5%)',  COUNT(*)::text FROM msmarco_facet_pdb WHERE facet_id < 5
UNION ALL
SELECT 'facet_id<10 (~10%)', COUNT(*)::text FROM msmarco_facet_pdb WHERE facet_id < 10
ORDER BY metric;

-- BM25 index: passage_text (English tokenizer, matching pg_textsearch's
-- 'english' config) plus facet_id as a numeric fast field for native filtering.
\echo ''
\echo '=== Building ParadeDB BM25 Index ==='
CREATE INDEX msmarco_facet_pdb_idx ON msmarco_facet_pdb
    USING bm25 (passage_id, passage_text, facet_id)
    WITH (
        key_field = 'passage_id',
        text_fields = '{
            "passage_text": {
                "tokenizer": {
                    "type": "default",
                    "stopwords_language": "English",
                    "stemmer": "English"
                }
            }
        }',
        numeric_fields = '{ "facet_id": {} }'
    );

-- Compact index segments (ParadeDB merges during VACUUM).
\echo 'VACUUM to compact index segments...'
VACUUM msmarco_facet_pdb;

\echo ''
\echo '=== Size Report ==='
SELECT 'INDEX_SIZE:' AS label,
       pg_size_pretty(pg_relation_size('msmarco_facet_pdb_idx')) AS index_size,
       pg_relation_size('msmarco_facet_pdb_idx') AS index_bytes;
SELECT 'TABLE_SIZE:' AS label,
       pg_size_pretty(pg_total_relation_size('msmarco_facet_pdb')) AS table_size,
       pg_total_relation_size('msmarco_facet_pdb') AS table_bytes;

\echo ''
\echo '=== Faceted MS-MARCO Load Complete (ParadeDB) ==='
