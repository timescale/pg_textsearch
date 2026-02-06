-- MS-MARCO Ground Truth Precomputation
-- Computes BM25 scores using the exact formula for validation purposes
-- Output: ground_truth.tsv with (query_id, query_text, rank, doc_id, score)
--
-- Prerequisites:
--   - msmarco_passages table with BM25 index
--   - benchmark_queries table will be loaded
--
-- Usage:
--   psql -p PORT -f precompute_ground_truth.sql
--
-- This is a ONE-TIME computation. Results are saved and reused for validation.
-- Note: First precomputes document frequencies for all query terms (~6 min),
-- then computes ground truth scores (~1 min per query).

\set ON_ERROR_STOP on
\timing on

-- Ensure validation functions exist (for fieldnorm_quantize)
\i test/sql/validation.sql

-- Ensure benchmark_queries table exists
\echo 'Loading benchmark queries...'
DROP TABLE IF EXISTS benchmark_queries;
CREATE TABLE benchmark_queries (
    query_id INTEGER,
    query_text TEXT,
    token_bucket INTEGER
);
\copy benchmark_queries FROM 'benchmarks/datasets/msmarco/benchmark_queries.tsv' WITH (FORMAT text, DELIMITER E'\t')

SELECT 'Loaded ' || COUNT(*) || ' benchmark queries' as status FROM benchmark_queries;

-- Get corpus statistics from the index
\echo ''
\echo 'Getting corpus statistics...'
DO $$
DECLARE
    v_summary text;
    v_total_docs bigint;
    v_total_len bigint;
    v_avg_doc_len float8;
BEGIN
    SELECT bm25_summarize_index('msmarco_bm25_idx') INTO v_summary;

    -- Parse total_docs and total_len from summary
    -- IMPORTANT: Compute avg_doc_len from total_len/total_docs for full precision.
    -- Do NOT parse the displayed avg_doc_len which is rounded for display.
    v_total_docs := (regexp_match(v_summary, 'total_docs: (\d+)'))[1]::bigint;
    v_total_len := (regexp_match(v_summary, 'total_len: (\d+)'))[1]::bigint;
    v_avg_doc_len := v_total_len::float8 / v_total_docs::float8;

    -- Store in temp table for use by compute function
    DROP TABLE IF EXISTS corpus_stats;
    CREATE TABLE corpus_stats AS
    SELECT v_total_docs as total_docs, v_avg_doc_len as avg_doc_len;

    RAISE NOTICE 'Corpus stats: total_docs=%, total_len=%, avg_doc_len=%',
        v_total_docs, v_total_len, v_avg_doc_len;
END;
$$;

-- Sample queries for validation (10 per token bucket)
\echo ''
\echo 'Selecting validation queries (10 per token bucket)...'
DROP TABLE IF EXISTS validation_queries;
CREATE TABLE validation_queries AS
WITH ranked AS (
    SELECT
        query_id,
        query_text,
        token_bucket,
        ROW_NUMBER() OVER (PARTITION BY token_bucket ORDER BY query_id) as rn
    FROM benchmark_queries
)
SELECT query_id, query_text, token_bucket
FROM ranked
WHERE rn <= 10;

SELECT 'Selected ' || COUNT(*) || ' queries for validation' as status FROM validation_queries;
SELECT token_bucket, COUNT(*) as count FROM validation_queries GROUP BY token_bucket ORDER BY token_bucket;

-- Step 1: Precompute document frequencies for all unique query terms
-- This is the slow part (~1 sec/term) but only needs to run once
\echo ''
\echo 'Step 1: Precomputing document frequencies for all query terms...'

DROP TABLE IF EXISTS term_doc_freq;
CREATE TABLE term_doc_freq (
    term text PRIMARY KEY,
    df bigint
);

DO $$
DECLARE
    v_term record;
    v_count int := 0;
    v_total int;
    v_df bigint;
    v_start timestamp;
    v_elapsed interval;
BEGIN
    -- Get unique terms from all validation queries
    CREATE TEMP TABLE unique_query_terms AS
    SELECT DISTINCT qt.lexeme::text as term
    FROM validation_queries vq,
    LATERAL unnest(to_tsvector('english', vq.query_text)) as qt(lexeme, positions, weights);

    SELECT COUNT(*) INTO v_total FROM unique_query_terms;
    v_start := clock_timestamp();

    RAISE NOTICE 'Computing df for % validation query terms...', v_total;
    RAISE NOTICE 'Estimated time: ~% minutes', round(v_total * 1.0 / 60);

    FOR v_term IN SELECT term FROM unique_query_terms ORDER BY term LOOP
        v_count := v_count + 1;

        -- Count documents containing this term (use direct tsquery cast to
        -- avoid double-stemming since v_term.term is already stemmed)
        SELECT COUNT(*) INTO v_df
        FROM msmarco_passages
        WHERE to_tsvector('english', passage_text) @@ (v_term.term)::tsquery;

        INSERT INTO term_doc_freq (term, df) VALUES (v_term.term, v_df);

        IF v_count % 25 = 0 THEN
            v_elapsed := clock_timestamp() - v_start;
            RAISE NOTICE '[%/%] Elapsed: %, avg: %/term',
                v_count, v_total, v_elapsed, v_elapsed / v_count;
        END IF;
    END LOOP;

    v_elapsed := clock_timestamp() - v_start;
    RAISE NOTICE 'Completed % terms in %', v_count, v_elapsed;

    DROP TABLE unique_query_terms;
END;
$$;

SELECT 'Computed df for ' || COUNT(*) || ' terms' as status FROM term_doc_freq;

\echo ''
\echo 'Most common terms:'
SELECT term, df FROM term_doc_freq ORDER BY df DESC LIMIT 5;

-- Step 2: Ground truth computation using precomputed df
-- IMPORTANT: Must account for QUERY term frequencies (how many times a term
-- appears in the query). E.g., "ash who is ash" has query_freq=2 for "ash".
-- The score contribution for each term is multiplied by its query frequency.
\echo ''
\echo 'Step 2: Creating ground truth computation function...'

CREATE OR REPLACE FUNCTION compute_ground_truth(
    p_query_id int,
    p_query_text text,
    p_k1 float DEFAULT 1.2,
    p_b float DEFAULT 0.75,
    p_limit int DEFAULT 10
) RETURNS TABLE(
    query_id int,
    query_text text,
    rank int,
    doc_id int,
    score float8
) AS $$
DECLARE
    v_total_docs bigint;
    v_avg_doc_len float8;
    v_query_terms text[];
BEGIN
    -- Get corpus stats
    SELECT cs.total_docs, cs.avg_doc_len INTO v_total_docs, v_avg_doc_len
    FROM corpus_stats cs;

    -- Get unique query terms for matching
    SELECT array_agg(lexeme::text) INTO v_query_terms
    FROM unnest(to_tsvector('english', p_query_text));

    IF v_query_terms IS NULL OR array_length(v_query_terms, 1) = 0 THEN
        RETURN;
    END IF;

    RETURN QUERY
    WITH query_terms_with_freq AS (
        -- Extract query terms with their frequencies
        -- This is CRITICAL: if "ash" appears twice in the query, query_freq=2
        SELECT
            qt.lexeme::text as term,
            array_length(string_to_array(qt.positions::text, ','), 1) as query_freq
        FROM unnest(to_tsvector('english', p_query_text)) as qt(lexeme, positions, weights)
    ),
    matching_docs AS (
        -- Find documents matching any query term
        SELECT
            passage_id,
            to_tsvector('english', passage_text) as doc_tsv
        FROM msmarco_passages
        WHERE to_tsvector('english', passage_text) @@
              to_tsquery('english', array_to_string(v_query_terms, ' | '))
    ),
    doc_scores AS (
        SELECT
            m.passage_id,
            (
                SELECT SUM(
                    -- IDF from precomputed df
                    ln(1.0 + (v_total_docs - tdf.df + 0.5) / (tdf.df + 0.5)) *
                    -- TF component: tf*(k1+1) / (tf + k1*(1-b+b*dl/avgdl))
                    (doc_tf * (p_k1 + 1.0)) /
                    (doc_tf + p_k1 * (1.0 - p_b + p_b * fieldnorm_quantize(
                        (SELECT COALESCE(SUM(array_length(string_to_array(t2.positions::text, ','), 1)), 0)::int
                         FROM unnest(m.doc_tsv) as t2(lexeme, positions, weights))
                    ) / v_avg_doc_len))
                    -- CRITICAL: Multiply by query term frequency for repeated terms
                    * qtf.query_freq
                )
                FROM query_terms_with_freq qtf
                JOIN term_doc_freq tdf ON tdf.term = qtf.term
                -- Get document term frequency for this query term
                CROSS JOIN LATERAL (
                    SELECT COALESCE(
                        (SELECT array_length(string_to_array(t.positions::text, ','), 1)
                         FROM unnest(m.doc_tsv) as t(lexeme, positions, weights)
                         WHERE t.lexeme::text = qtf.term),
                        0
                    ) as doc_tf
                ) tf_lookup
                WHERE doc_tf > 0
            ) as bm25_score
        FROM matching_docs m
    ),
    ranked AS (
        SELECT passage_id, bm25_score, ROW_NUMBER() OVER (ORDER BY bm25_score DESC) as rn
        FROM doc_scores WHERE bm25_score > 0
    )
    SELECT p_query_id, p_query_text, rn::int, passage_id::int, bm25_score::float8
    FROM ranked WHERE rn <= p_limit;
END;
$$ LANGUAGE plpgsql;

-- Step 3: Compute ground truth for all validation queries
\echo ''
\echo 'Step 3: Computing ground truth for validation queries...'

DROP TABLE IF EXISTS ground_truth;
CREATE TABLE ground_truth (
    query_id int,
    query_text text,
    rank int,
    doc_id int,
    score float8
);

DO $$
DECLARE
    q record;
    v_count int := 0;
    v_total int;
    v_start timestamp;
    v_elapsed interval;
BEGIN
    SELECT COUNT(*) INTO v_total FROM validation_queries;
    v_start := clock_timestamp();

    FOR q IN SELECT query_id, query_text, token_bucket FROM validation_queries ORDER BY token_bucket, query_id LOOP
        v_count := v_count + 1;
        RAISE NOTICE '[%/%] Bucket % - Query %: %...',
            v_count, v_total, q.token_bucket, q.query_id, left(q.query_text, 40);

        INSERT INTO ground_truth
        SELECT * FROM compute_ground_truth(q.query_id, q.query_text);

        IF v_count % 10 = 0 THEN
            v_elapsed := clock_timestamp() - v_start;
            RAISE NOTICE 'Progress: % queries in %, avg %/query',
                v_count, v_elapsed, v_elapsed / v_count;
        END IF;
    END LOOP;

    v_elapsed := clock_timestamp() - v_start;
    RAISE NOTICE 'Completed % queries in %', v_count, v_elapsed;
END;
$$;

-- Export to TSV
\echo ''
\echo 'Exporting ground truth to TSV...'
\copy ground_truth TO 'benchmarks/datasets/msmarco/ground_truth.tsv' WITH (FORMAT text, DELIMITER E'\t', HEADER true)

SELECT 'Exported ' || COUNT(*) || ' rows for ' || COUNT(DISTINCT query_id) || ' queries' as status
FROM ground_truth;

-- Cleanup (keep term_doc_freq for potential debugging)
DROP TABLE IF EXISTS validation_queries;
DROP FUNCTION IF EXISTS compute_ground_truth;

\echo ''
\echo '=== Ground Truth Precomputation Complete ==='
\echo 'Output: benchmarks/datasets/msmarco/ground_truth.tsv'
\echo 'Note: term_doc_freq table retained for debugging'
