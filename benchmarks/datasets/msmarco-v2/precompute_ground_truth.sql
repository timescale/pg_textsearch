-- MS-MARCO v2 Ground Truth Precomputation
-- Computes BM25 scores using the exact formula for validation purposes
-- Output: ground_truth.tsv with (query_id, query_text, rank, doc_id, score)
--
-- IMPORTANT: Different Postgres versions may produce different ground truth
-- due to Snowball stemmer changes (e.g., PG17 vs PG18). After generating,
-- rename the output to ground_truth_pgNN.tsv for the target version.
--
-- Prerequisites:
--   - msmarco_v2_passages table loaded (index NOT required)
--   - msmarco_v2_queries table loaded
--
-- Usage:
--   psql -p PORT -f precompute_ground_truth.sql
--
-- This is a ONE-TIME computation. Results are saved and reused for
-- validation. At 138M rows, full validation of all 3,903 dev queries
-- is infeasible (~1.5 min/query = ~4 days). Instead we validate 400
-- queries: 10 curated high-frequency term queries (to detect the
-- block_count overflow bug fixed by PR #266), 10 curated
-- low-frequency term queries (baseline correctness), and 380
-- randomly sampled queries from the full dev set.
--
-- Note: Uses single-pass doc_freq computation (~13 min total) instead
-- of one COUNT(*) per term (~13 min each) as in v1.

\set ON_ERROR_STOP on
\timing on

-- Ensure validation functions exist (for fieldnorm_quantize)
\i test/sql/validation.sql

-- Mark fieldnorm_quantize as PARALLEL SAFE so that queries using it
-- can leverage parallel workers. It's a pure computation (no side
-- effects, no transaction state).
ALTER FUNCTION fieldnorm_quantize(int) PARALLEL SAFE;

-- ============================================================
-- Step 0: Select validation queries (400 total)
-- ============================================================
-- 10 hand-picked high-frequency term queries (terms with
-- doc_freq > 8.4M, the uint16 block_count overflow threshold).
-- 10 hand-picked low-frequency term queries (rare/specific).
-- 380 randomly sampled from the remaining dev queries.
-- Random seed is fixed (0.42) for reproducibility.

\echo 'Creating validation queries...'
DROP TABLE IF EXISTS validation_queries;
CREATE TABLE validation_queries (
    query_id INTEGER,
    query_text TEXT,
    category TEXT  -- 'high_freq', 'low_freq', or 'random'
);

-- High-frequency term queries (terms likely > 8.4M postings)
INSERT INTO validation_queries
SELECT q.query_id, q.query_text, 'high_freq'
FROM msmarco_v2_queries q
WHERE q.query_id IN (
    107077,  -- 'cost of slate' (cost)
    195693,  -- 'golf cost' (cost)
    46081,   -- 'average time for an appraisal' (time)
    167156,  -- 'does highster mobile really work' (work)
    166043,  -- 'does estrogen make me start my period' (make)
    80590,   -- 'can you use denture tablets...' (use)
    46095,   -- 'average time for hair to grow' (time)
    109276,  -- 'cost to dig a pond' (cost)
    108037,  -- 'cost of wooden shutters' (cost)
    102695   -- 'cost of bamboo hr' (cost)
);

-- Low-frequency term queries (rare/specific terms)
INSERT INTO validation_queries
SELECT q.query_id, q.query_text, 'low_freq'
FROM msmarco_v2_queries q
WHERE q.query_id IN (
    49802,   -- 'belizean cuisine'
    61531,   -- 'campagnolo bikes'
    558263,  -- 'what are endotoxins?'
    561448,  -- 'what are mandrakes'
    563347,  -- 'what are plastids'
    687375,  -- 'what is a hydromyelia'
    699510,  -- 'what is a serigraph'
    703383,  -- 'what is a thoracentesis'
    724121,  -- 'what is bioflavonoids'
    724571   -- 'what is blackleg'
);

-- Random sample of 380 queries (fixed seed for reproducibility)
INSERT INTO validation_queries
SELECT q.query_id, q.query_text, 'random'
FROM msmarco_v2_queries q
WHERE q.query_id NOT IN (
    SELECT vq.query_id FROM validation_queries vq
)
ORDER BY hashint4(q.query_id + 42)
LIMIT 380;

DO $$
DECLARE
    v_high int;
    v_low int;
    v_random int;
BEGIN
    SELECT COUNT(*) INTO v_high FROM validation_queries
    WHERE category = 'high_freq';
    SELECT COUNT(*) INTO v_low FROM validation_queries
    WHERE category = 'low_freq';
    SELECT COUNT(*) INTO v_random FROM validation_queries
    WHERE category = 'random';
    RAISE NOTICE 'Selected % high-freq + % low-freq + % random'
        ' = % queries',
        v_high, v_low, v_random,
        v_high + v_low + v_random;
END;
$$;

\echo ''
\echo 'Validation queries by category:'
SELECT category, COUNT(*) as count
FROM validation_queries GROUP BY category ORDER BY category;

-- ============================================================
-- Step 1: Compute corpus statistics
-- ============================================================
-- Scans the full 138M-row table to count tokens per document.
-- Documents with empty tsvectors (no indexable terms) are excluded,
-- matching the index behavior.
--
-- Uses LATERAL unnest join (not correlated subquery) so Postgres
-- can parallelize with Gather Merge. Tune parallel costs to
-- ensure workers are used on this CPU-heavy scan.
\echo ''
\echo 'Step 1: Computing corpus statistics (parallel scan)...'

SET max_parallel_workers_per_gather = 15;
SET parallel_tuple_cost = 0;
SET parallel_setup_cost = 0;
SET min_parallel_table_scan_size = 0;

DROP TABLE IF EXISTS corpus_stats;
CREATE TABLE corpus_stats AS
SELECT
    COUNT(*)::bigint as total_docs,
    SUM(doc_len)::bigint as total_len,
    (SUM(doc_len)::float8 / COUNT(*)::float8) as avg_doc_len
FROM (
    SELECT p.passage_id,
           SUM(array_length(
               string_to_array(t.positions::text, ','), 1
           )) as doc_len
    FROM msmarco_v2_passages p,
         LATERAL unnest(to_tsvector('english', p.passage_text))
             AS t(lexeme, positions, weights)
    GROUP BY p.passage_id
) dl;

RESET max_parallel_workers_per_gather;
RESET parallel_tuple_cost;
RESET parallel_setup_cost;
RESET min_parallel_table_scan_size;

DO $$
DECLARE
    r record;
BEGIN
    SELECT * INTO r FROM corpus_stats;
    RAISE NOTICE 'Corpus stats: total_docs=%, total_len=%, '
        'avg_doc_len=%',
        r.total_docs, r.total_len,
        round(r.avg_doc_len::numeric, 4);
END;
$$;

-- ============================================================
-- Step 2: Precompute document frequencies (single-pass)
-- ============================================================
-- At 138M rows, one COUNT(*) per term takes ~13 min each.
-- Instead, compute all term doc_freqs in one table scan using
-- LATERAL unnest + GROUP BY. This takes ~13 min total.
\echo ''
\echo 'Step 2: Precomputing document frequencies (single-pass, ~13 min)...'

-- First collect all unique query terms
DROP TABLE IF EXISTS unique_query_terms;
CREATE TABLE unique_query_terms AS
SELECT DISTINCT qt.lexeme::text as term
FROM validation_queries vq,
LATERAL unnest(to_tsvector('english', vq.query_text))
    as qt(lexeme, positions, weights);

SELECT 'Computing df for ' || COUNT(*) || ' unique query terms'
    as status
FROM unique_query_terms;

-- Single-pass: scan entire corpus once, unnest tsvectors, filter
-- to only our query terms, count distinct passage_ids per term
SET max_parallel_workers_per_gather = 15;
SET parallel_tuple_cost = 0;
SET parallel_setup_cost = 0;
SET min_parallel_table_scan_size = 0;

DROP TABLE IF EXISTS term_doc_freq;
CREATE TABLE term_doc_freq AS
SELECT t.lexeme::text as term, COUNT(DISTINCT p.passage_id) as df
FROM msmarco_v2_passages p,
     LATERAL unnest(to_tsvector('english', p.passage_text))
         AS t(lexeme, positions, weights)
WHERE t.lexeme::text IN (SELECT term FROM unique_query_terms)
GROUP BY t.lexeme;

RESET max_parallel_workers_per_gather;
RESET parallel_tuple_cost;
RESET parallel_setup_cost;
RESET min_parallel_table_scan_size;

SELECT 'Computed df for ' || COUNT(*) || ' terms' as status
FROM term_doc_freq;

\echo ''
\echo 'Document frequencies (sorted by df):'
SELECT term, df FROM term_doc_freq ORDER BY df DESC;

DROP TABLE unique_query_terms;

-- ============================================================
-- Step 3: Materialize document vectors
-- ============================================================
-- Pre-compute tsvectors and quantized doc lengths for all docs
-- matching any query term. This avoids recomputing to_tsvector
-- per query and eliminates correlated subqueries in scoring.
--
-- Strategy: single parallel scan of 138M rows, unnest tsvectors,
-- filter to docs containing any query term, extract per-term tf
-- and quantized doc length. The result is a flat table suitable
-- for join-based BM25 scoring.
\echo ''
\echo 'Step 3: Materializing document term frequencies...'

SET max_parallel_workers_per_gather = 15;
SET parallel_tuple_cost = 0;
SET parallel_setup_cost = 0;
SET min_parallel_table_scan_size = 0;

-- 3a: Extract per-document term frequencies for query terms.
-- One row per (passage_id, term). Single parallel scan that
-- unnests all tsvectors and filters to query terms only.
\echo '  3a: Extracting term frequencies (~20 min)...'
DROP TABLE IF EXISTS doc_term_data;
CREATE TABLE doc_term_data AS
SELECT
    p.passage_id,
    t.lexeme::text as term,
    array_length(
        string_to_array(t.positions::text, ','), 1
    ) as tf
FROM msmarco_v2_passages p,
LATERAL unnest(to_tsvector('english', p.passage_text))
    AS t(lexeme, positions, weights)
WHERE t.lexeme::text IN (SELECT term FROM term_doc_freq);

SELECT 'Extracted ' || COUNT(*) || ' (doc, term) pairs' as status
FROM doc_term_data;

CREATE INDEX ON doc_term_data (passage_id, term);
ANALYZE doc_term_data;

-- 3b: Compute quantized doc lengths for ALL documents.
-- Full parallel scan — no joins that would block parallelism.
-- Requires fieldnorm_quantize to be marked PARALLEL SAFE.
\echo '  3b: Computing quantized doc lengths (~18 min)...'
DROP TABLE IF EXISTS doc_lengths;
CREATE TABLE doc_lengths AS
SELECT
    p.passage_id,
    fieldnorm_quantize(
        SUM(array_length(
            string_to_array(t.positions::text, ','), 1
        ))::int
    ) as doc_len_q
FROM msmarco_v2_passages p,
LATERAL unnest(to_tsvector('english', p.passage_text))
    AS t(lexeme, positions, weights)
GROUP BY p.passage_id;

SELECT 'Computed doc lengths for ' || COUNT(*)
    || ' passages' as status
FROM doc_lengths;

CREATE INDEX ON doc_lengths (passage_id);
ANALYZE doc_lengths;

RESET max_parallel_workers_per_gather;
RESET parallel_tuple_cost;
RESET parallel_setup_cost;
RESET min_parallel_table_scan_size;

-- ============================================================
-- Step 4: Compute ground truth using flat joins
-- ============================================================
-- No correlated subqueries. For each query, join doc_term_data
-- and doc_lengths to compute BM25 scores with SUM/GROUP BY.
\echo ''
\echo 'Step 4: Computing ground truth (400 queries, flat joins)...'

DROP TABLE IF EXISTS ground_truth;
CREATE TABLE ground_truth (
    query_id int,
    query_text text,
    rank int,
    doc_id text,
    score float8
);

DO $$
DECLARE
    q record;
    v_count int := 0;
    v_total int;
    v_start timestamp;
    v_elapsed interval;
    v_total_docs bigint;
    v_avg_doc_len float8;
    v_k1 float8 := 1.2;
    v_b float8 := 0.75;
BEGIN
    SELECT COUNT(*) INTO v_total FROM validation_queries;
    SELECT cs.total_docs, cs.avg_doc_len
    INTO v_total_docs, v_avg_doc_len
    FROM corpus_stats cs;

    v_start := clock_timestamp();

    FOR q IN
        SELECT query_id, query_text, category
        FROM validation_queries
        ORDER BY category, query_id
    LOOP
        v_count := v_count + 1;
        RAISE NOTICE '[%/%] % - Query %: %...',
            v_count, v_total, q.category, q.query_id,
            left(q.query_text, 40);

        INSERT INTO ground_truth
        WITH query_terms AS (
            SELECT
                qt.lexeme::text as term,
                array_length(
                    string_to_array(qt.positions::text, ','),
                    1
                ) as query_freq
            FROM unnest(
                to_tsvector('english', q.query_text)
            ) as qt(lexeme, positions, weights)
        ),
        doc_scores AS (
            SELECT
                dt.passage_id,
                SUM(
                    -- IDF
                    ln(1.0 + (v_total_docs - tdf.df + 0.5)
                        / (tdf.df + 0.5))
                    -- TF saturation
                    * (dt.tf * (v_k1 + 1.0))
                    / (dt.tf + v_k1 * (1.0 - v_b + v_b
                        * dl.doc_len_q / v_avg_doc_len))
                    -- Query term frequency
                    * qt.query_freq
                ) as bm25_score
            FROM query_terms qt
            JOIN term_doc_freq tdf ON tdf.term = qt.term
            JOIN doc_term_data dt ON dt.term = qt.term
            JOIN doc_lengths dl
                ON dl.passage_id = dt.passage_id
            GROUP BY dt.passage_id
        ),
        ranked AS (
            SELECT
                passage_id,
                bm25_score,
                ROW_NUMBER() OVER (
                    ORDER BY bm25_score DESC
                ) as rn
            FROM doc_scores
            WHERE bm25_score > 0
        )
        SELECT
            q.query_id,
            q.query_text,
            rn::int,
            passage_id::text,
            bm25_score::float8
        FROM ranked
        WHERE rn <= 10;

        v_elapsed := clock_timestamp() - v_start;
        RAISE NOTICE '  Elapsed: %, avg %/query',
            v_elapsed, v_elapsed / v_count;
    END LOOP;

    v_elapsed := clock_timestamp() - v_start;
    RAISE NOTICE 'Completed % queries in %', v_count, v_elapsed;
END;
$$;

-- Export to TSV
\echo ''
\echo 'Exporting ground truth to TSV...'
\copy ground_truth TO 'benchmarks/datasets/msmarco-v2/ground_truth.tsv' WITH (FORMAT text, DELIMITER E'\t', HEADER true)

SELECT 'Exported ' || COUNT(*) || ' rows for '
    || COUNT(DISTINCT query_id) || ' queries' as status
FROM ground_truth;

-- Show summary per category
\echo ''
\echo 'Results per category:'
SELECT
    vq.category,
    COUNT(DISTINCT gt.query_id) as queries,
    COUNT(*) as result_rows,
    round(AVG(gt.score)::numeric, 4) as avg_score
FROM ground_truth gt
JOIN validation_queries vq ON vq.query_id = gt.query_id
GROUP BY vq.category
ORDER BY vq.category;

-- Cleanup
DROP TABLE IF EXISTS validation_queries;
DROP TABLE IF EXISTS doc_term_data;
DROP TABLE IF EXISTS doc_lengths;

\echo ''
\echo '=== Ground Truth Precomputation Complete ==='
\echo 'Output: benchmarks/datasets/msmarco-v2/ground_truth.tsv'
\echo 'Note: term_doc_freq and corpus_stats tables retained'
\echo '      for debugging'
