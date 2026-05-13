-- Wikipedia Ground Truth Precomputation
-- Computes BM25 scores using the exact BM25 formula (pure SQL, no
-- pg_textsearch index involvement) and writes ground_truth.tsv for
-- the validator (validate_queries.sql) to compare index output
-- against.
--
-- Prerequisites:
--   - wikipedia_articles table loaded (from load.sql)
--   - benchmark_queries table (from queries.sql; do NOT run queries.sql
--     to completion -- it drops the table at the end)
--
-- Usage:
--   psql -p PORT -f precompute_ground_truth.sql
--
-- Implementation notes:
--   The original version of this script iterated over query terms one
--   at a time, computing document frequency via a full to_tsvector()
--   scan of wikipedia_articles per term. On 100K Simple Wikipedia
--   this is hopelessly slow (still in Step 1 after 10+ minutes).
--
--   This version materializes (article_id, term, tf) once via a
--   single tsvector unnest pass, indexes the result on term, then
--   computes corpus stats, df, and per-query top-10 scores via
--   cheap index lookups. Total runtime on 100K Simple Wikipedia is
--   ~30s end-to-end.
--
--   No reference to wikipedia_bm25_idx anywhere -- this is pure SQL
--   ground truth, which is exactly the independent reference the
--   validator needs to compare the index against.

\set ON_ERROR_STOP on
\timing on

-- Pull in fieldnorm_quantize (matches the index's length quantization)
\i test/sql/validation.sql

\echo ''
\echo 'Step 1: Materializing (article_id, term, tf) with a single tsvector pass...'
DROP TABLE IF EXISTS doc_terms_tmp;
CREATE UNLOGGED TABLE doc_terms_tmp AS
SELECT
    a.article_id,
    t.lexeme::text AS term,
    array_length(string_to_array(t.positions::text, ','), 1)::int AS tf
FROM wikipedia_articles a,
     LATERAL unnest(to_tsvector('english', a.content))
         AS t(lexeme, positions, weights);

CREATE INDEX ON doc_terms_tmp (term);
CREATE INDEX ON doc_terms_tmp (article_id);
ANALYZE doc_terms_tmp;
SELECT COUNT(*) AS doc_term_pairs FROM doc_terms_tmp;

\echo ''
\echo 'Step 2: Materializing per-doc lengths...'
DROP TABLE IF EXISTS doc_lengths_tmp;
CREATE UNLOGGED TABLE doc_lengths_tmp AS
SELECT article_id, SUM(tf)::int AS dl
FROM doc_terms_tmp
GROUP BY article_id;
CREATE UNIQUE INDEX ON doc_lengths_tmp (article_id);
ANALYZE doc_lengths_tmp;

\echo ''
\echo 'Step 3: Computing corpus statistics...'
-- Matches the index behavior: documents with empty tsvector contribute
-- nothing (they're absent from doc_lengths_tmp since the LATERAL unnest
-- in step 1 produced zero rows for them).
DROP TABLE IF EXISTS corpus_stats;
CREATE TABLE corpus_stats AS
SELECT
    COUNT(*)::bigint AS total_docs,
    SUM(dl)::bigint AS total_len,
    (SUM(dl)::float8 / COUNT(*)::float8) AS avg_doc_len
FROM doc_lengths_tmp;
SELECT * FROM corpus_stats;

\echo ''
\echo 'Step 4: Selecting validation queries (10 per token bucket)...'
DROP TABLE IF EXISTS validation_queries;
CREATE TABLE validation_queries AS
WITH ranked AS (
    SELECT query_id, query_text, token_bucket,
           ROW_NUMBER() OVER (PARTITION BY token_bucket
                              ORDER BY query_id) AS rn
    FROM benchmark_queries
)
SELECT query_id, query_text, token_bucket
FROM ranked WHERE rn <= 10;
SELECT COUNT(*) AS validation_query_count FROM validation_queries;

\echo ''
\echo 'Step 5: Computing per-term document frequency...'
DROP TABLE IF EXISTS term_doc_freq;
CREATE TABLE term_doc_freq AS
WITH query_terms AS (
    SELECT DISTINCT qt.lexeme::text AS term
    FROM validation_queries vq,
         LATERAL unnest(to_tsvector('english', vq.query_text))
             AS qt(lexeme, positions, weights)
)
SELECT qt.term,
       COALESCE((SELECT COUNT(DISTINCT dt.article_id)
                 FROM doc_terms_tmp dt
                 WHERE dt.term = qt.term), 0)::bigint AS df
FROM query_terms qt;
CREATE UNIQUE INDEX ON term_doc_freq (term);
ANALYZE term_doc_freq;
SELECT COUNT(*) AS term_count FROM term_doc_freq;

\echo ''
\echo 'Step 6: Computing top-10 ground truth per validation query...'
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
    v_total_docs bigint;
    v_avg_doc_len float8;
    v_k1 float8 := 1.2;
    v_b float8 := 0.75;
    v_count int := 0;
    v_total int;
    v_start timestamp;
BEGIN
    SELECT cs.total_docs, cs.avg_doc_len
        INTO v_total_docs, v_avg_doc_len
    FROM corpus_stats cs;

    SELECT COUNT(*) INTO v_total FROM validation_queries;
    v_start := clock_timestamp();

    FOR q IN SELECT query_id, query_text
             FROM validation_queries
             ORDER BY query_id LOOP
        v_count := v_count + 1;

        INSERT INTO ground_truth
        WITH qt AS (
            SELECT t.lexeme::text AS term,
                   array_length(string_to_array(t.positions::text, ','),
                                1)::int AS qf
            FROM unnest(to_tsvector('english', q.query_text))
                AS t(lexeme, positions, weights)
        ),
        per_doc_term AS (
            -- One row per (matching doc, query term)
            SELECT dt.article_id, qt.term, dt.tf, qt.qf, tdf.df
            FROM qt
            JOIN term_doc_freq tdf ON tdf.term = qt.term
            JOIN doc_terms_tmp dt  ON dt.term  = qt.term
            WHERE tdf.df > 0
        ),
        scored AS (
            SELECT
                pdt.article_id,
                SUM(
                    -- IDF: ln(1 + (N - df + 0.5) / (df + 0.5))
                    ln(1.0 + (v_total_docs - pdt.df + 0.5)
                                / (pdt.df + 0.5))
                    *
                    -- TF normalization with quantized field length
                    (pdt.tf * (v_k1 + 1.0))
                    /
                    (pdt.tf + v_k1 *
                        (1.0 - v_b
                            + v_b * fieldnorm_quantize(dl.dl)
                                  / v_avg_doc_len))
                    *
                    -- Multiply by query-term frequency for repeated terms
                    pdt.qf
                )::float8 AS bm25_score
            FROM per_doc_term pdt
            JOIN doc_lengths_tmp dl ON dl.article_id = pdt.article_id
            GROUP BY pdt.article_id
        ),
        ranked AS (
            SELECT article_id, bm25_score,
                   ROW_NUMBER() OVER (ORDER BY bm25_score DESC,
                                               article_id) AS rn
            FROM scored
            WHERE bm25_score > 0
        )
        SELECT q.query_id, q.query_text, rn::int,
               article_id::int, bm25_score
        FROM ranked WHERE rn <= 10;

        IF v_count % 10 = 0 THEN
            RAISE NOTICE '[%/%] elapsed=%',
                v_count, v_total, clock_timestamp() - v_start;
        END IF;
    END LOOP;

    RAISE NOTICE 'Completed % queries in %',
        v_count, clock_timestamp() - v_start;
END;
$$;

\echo ''
\echo 'Step 7: Exporting ground truth...'
\copy ground_truth TO 'benchmarks/datasets/wikipedia/ground_truth.tsv' WITH (FORMAT text, DELIMITER E'\t', HEADER true)

SELECT 'Exported ' || COUNT(*) || ' rows for ' ||
       COUNT(DISTINCT query_id) || ' queries' AS status
FROM ground_truth;

-- Cleanup (keep corpus_stats and term_doc_freq for potential debugging)
DROP TABLE IF EXISTS doc_terms_tmp;
DROP TABLE IF EXISTS doc_lengths_tmp;
DROP TABLE IF EXISTS validation_queries;

\echo ''
\echo '=== Ground Truth Precomputation Complete ==='
\echo 'Output: benchmarks/datasets/wikipedia/ground_truth.tsv'
