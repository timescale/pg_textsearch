-- MS MARCO Passage Ranking - Load and Index (System Y)
-- Loads the full 8.8M passage collection and creates System Y BM25 index
--
-- Usage:
--   DATA_DIR=/path/to/data psql -U postgres postgres -f load.sql
--
-- The DATA_DIR environment variable should point to the directory containing:
--   - collection.tsv (passage_id, passage_text)
--   - queries.dev.tsv (query_id, query_text)
--   - qrels.dev.small.tsv (query_id, 0, passage_id, relevance)

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Passage Ranking - Data Loading (System Y) ==='
\echo 'Loading ~8.8M passages from MS MARCO collection'
\echo ''

-- Ensure System Y extensions are installed
CREATE EXTENSION IF NOT EXISTS pg_tokenizer CASCADE;
CREATE EXTENSION IF NOT EXISTS vchord_bm25 CASCADE;

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_passages_systemy CASCADE;
DROP TABLE IF EXISTS msmarco_queries_systemy CASCADE;
DROP TABLE IF EXISTS msmarco_qrels_systemy CASCADE;

-- Create passages table with bm25vector column for embeddings
\echo 'Creating passages table...'
CREATE TABLE msmarco_passages_systemy (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL,
    embedding bm25vector
);

-- Create queries table
\echo 'Creating queries table...'
CREATE TABLE msmarco_queries_systemy (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

-- Create relevance judgments table
\echo 'Creating relevance judgments table...'
CREATE TABLE msmarco_qrels_systemy (
    query_id INTEGER NOT NULL,
    passage_id INTEGER NOT NULL,
    relevance INTEGER NOT NULL,
    PRIMARY KEY (query_id, passage_id)
);

-- Load passages (this is the big one - 8.8M rows)
-- Convert TSV to CSV to avoid issues with \. sequences in text format
\echo 'Loading passages (this may take several minutes)...'
\copy msmarco_passages_systemy(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Load queries (use a subset for benchmarking - first 10K queries)
\echo 'Loading queries...'
\copy msmarco_queries_systemy(query_id, query_text) FROM PROGRAM 'head -10000 "$DATA_DIR/queries.dev.tsv"' WITH (FORMAT text, DELIMITER E'\t');

-- Load relevance judgments (qrels format: query_id, 0, passage_id, relevance)
\echo 'Loading relevance judgments...'
CREATE TEMP TABLE qrels_raw (
    query_id INTEGER,
    zero INTEGER,
    passage_id INTEGER,
    relevance INTEGER
);
\copy qrels_raw FROM PROGRAM 'cat "$DATA_DIR/qrels.dev.small.tsv"' WITH (FORMAT text, DELIMITER E'\t');
INSERT INTO msmarco_qrels_systemy (query_id, passage_id, relevance)
SELECT query_id, passage_id, relevance FROM qrels_raw;
DROP TABLE qrels_raw;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT 'Passages loaded:' as metric, COUNT(*)::text as count FROM msmarco_passages_systemy
UNION ALL
SELECT 'Queries loaded:', COUNT(*)::text FROM msmarco_queries_systemy
UNION ALL
SELECT 'Relevance judgments:', COUNT(*)::text FROM msmarco_qrels_systemy
ORDER BY metric;

-- Show sample data
\echo ''
\echo 'Sample passages:'
SELECT passage_id, LEFT(passage_text, 100) || '...' as passage_preview
FROM msmarco_passages_systemy
LIMIT 3;

\echo ''
\echo 'Sample queries:'
SELECT query_id, query_text
FROM msmarco_queries_systemy
LIMIT 5;

-- Create text analyzer with English stopwords and stemming
\echo ''
\echo '=== Creating Text Analyzer and Tokenizer ==='
SELECT create_text_analyzer('msmarco_english_analyzer', $$
pre_tokenizer = "unicode_segmentation"
[[character_filters]]
to_lowercase = {}
[[token_filters]]
skip_non_alphanumeric = {}
[[token_filters]]
stopwords = "nltk_english"
[[token_filters]]
stemmer = "english_porter2"
$$);

-- Create custom model tokenizer with trigger for automatic tokenization
SELECT create_custom_model_tokenizer_and_trigger(
    tokenizer_name => 'msmarco_tokenizer',
    model_name => 'msmarco_model',
    text_analyzer_name => 'msmarco_english_analyzer',
    table_name => 'msmarco_passages_systemy',
    source_column => 'passage_text',
    target_column => 'embedding'
);

-- Tokenize all existing passages
\echo ''
\echo '=== Tokenizing Passages (this will take a while)... ==='
UPDATE msmarco_passages_systemy SET embedding = tokenize(passage_text, 'msmarco_tokenizer');

-- Create System Y BM25 index
\echo ''
\echo '=== Building System Y BM25 Index ==='
\echo 'Creating System Y BM25 index on ~8.8M passages (this will take a while)...'
CREATE INDEX msmarco_systemy_idx ON msmarco_passages_systemy
    USING bm25 (embedding bm25_ops);

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_systemy_idx')) as index_size,
    pg_relation_size('msmarco_systemy_idx') as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('msmarco_passages_systemy')) as table_size,
    pg_total_relation_size('msmarco_passages_systemy') as table_bytes;

\echo ''
\echo '=== MS MARCO Load Complete (System Y) ==='
\echo 'Ready for query benchmarks'
