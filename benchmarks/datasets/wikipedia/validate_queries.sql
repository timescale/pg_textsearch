-- Wikipedia Query Validation
-- Compares Tapir query results against precomputed ground truth
--
-- Requires: ground_truth.tsv in the same directory (created by CI or
--   run_benchmark.sh from the versioned ground_truth_pgNN.tsv files)
-- Usage: psql -p PORT -f validate_queries.sql
--
-- Validates: the score at each rank (1..10) in the index's result
-- matches the score at the same rank in ground truth, within absolute
-- tolerance (0.001).
--
-- Why per-rank score comparison rather than per-doc:
--
-- BM25 scoring can produce *tied* docs (often clusters of 3+ docs at
-- the rank-10 boundary on real corpora). Tie-break ordering is
-- undefined and varies between:
--   - single-writer COPY vs concurrent INSERT (different CTID order)
--   - BMW top-k vs sequential scan
--   - across pgbench runs (timing-dependent ordering)
--
-- A naive doc-set comparison flags these tie-break differences as
-- correctness failures. Per-rank score comparison treats them as the
-- same result -- which they are, since BM25 ranking by score is what
-- the index actually promises.
--
-- For diagnostic purposes the doc set is still reported when scores
-- diverge or as supplementary info, but pass/fail is decided on
-- scores.
--
-- This script mirrors benchmarks/datasets/msmarco/validate_queries.sql;
-- keep them structurally aligned when making changes.

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia BM25 Validation ==='
\echo ''

-- Load ground truth data
\echo 'Loading ground truth...'
DROP TABLE IF EXISTS ground_truth;
CREATE TABLE ground_truth (
    query_id int,
    query_text text,
    rank int,
    doc_id int,
    score float8
);
\copy ground_truth FROM 'benchmarks/datasets/wikipedia/ground_truth.tsv' WITH (FORMAT text, DELIMITER E'\t', HEADER true)

SELECT 'Loaded ' || COUNT(DISTINCT query_id) || ' queries, ' || COUNT(*) || ' result rows' as status
FROM ground_truth;

-- Score-based validation: compare score at each rank between index
-- and ground truth.
CREATE OR REPLACE FUNCTION validate_single_query(
    p_query_id int,
    p_query_text text,
    p_tolerance float8 DEFAULT 0.001
) RETURNS TABLE(
    query_id int,
    scores_match boolean,
    docs_match boolean,
    max_score_diff float8,
    worst_rank int,
    details text
) AS $$
DECLARE
    v_max_score_diff float8 := 0;
    v_worst_rank int := 0;
    v_all_match boolean := true;
    v_docs_match boolean;
    v_details text := '';
    r record;
BEGIN
    -- Get Tapir's top-10 results
    CREATE TEMP TABLE IF NOT EXISTS tapir_results (
        rank int,
        doc_id int,
        score float8
    );
    TRUNCATE tapir_results;

    INSERT INTO tapir_results
    -- IMPORTANT: keep the LIMIT 10 in the inner subquery so it
    -- propagates to the BM25 index scan and BMW runs with K=10.
    -- If LIMIT 10 sits above a window function in the outer SELECT,
    -- the planner can't push it past the WindowAgg and the index
    -- scan falls back to pg_textsearch.default_limit (1000). BMW
    -- with K=1000 reports subtly-wrong scores for some docs on real
    -- corpora -- see #365.
    SELECT row_number() OVER ()::int as rank, t.article_id, t.score FROM (
        SELECT
            article_id,
            -(content <@> to_bm25query(p_query_text, 'wikipedia_bm25_idx'))::float8 as score
        FROM wikipedia_articles
        ORDER BY content <@> to_bm25query(p_query_text, 'wikipedia_bm25_idx')
        LIMIT 10
    ) t;

    -- Compare score at each rank
    FOR r IN
        SELECT
            gt.rank,
            gt.score as gt_score,
            t.score as tapir_score,
            gt.doc_id as gt_doc,
            t.doc_id as tapir_doc,
            ABS(gt.score - t.score) as abs_diff
        FROM ground_truth gt
        JOIN tapir_results t ON t.rank = gt.rank
        WHERE gt.query_id = p_query_id
        ORDER BY gt.rank
    LOOP
        IF r.abs_diff > p_tolerance THEN
            v_all_match := false;
            IF r.abs_diff > v_max_score_diff THEN
                v_max_score_diff := r.abs_diff;
                v_worst_rank := r.rank;
            END IF;
            v_details := v_details || 'rank ' || r.rank ||
                ': gt=' || round(r.gt_score::numeric, 4)::text ||
                ' (doc ' || r.gt_doc || '), tapir=' ||
                round(r.tapir_score::numeric, 4)::text ||
                ' (doc ' || r.tapir_doc || '); ';
        ELSE
            v_max_score_diff := GREATEST(v_max_score_diff, r.abs_diff);
        END IF;
    END LOOP;

    -- Also check doc set as supplementary info (not part of pass/fail)
    SELECT COALESCE(
        (SELECT array_agg(gt.doc_id ORDER BY gt.doc_id) FROM ground_truth gt
         WHERE gt.query_id = p_query_id) =
        (SELECT array_agg(doc_id ORDER BY doc_id) FROM tapir_results),
        false
    ) INTO v_docs_match;

    query_id := p_query_id;
    scores_match := v_all_match;
    docs_match := v_docs_match;
    max_score_diff := v_max_score_diff;
    worst_rank := v_worst_rank;
    details := CASE WHEN v_details = '' THEN 'OK'
               ELSE trim(trailing '; ' from v_details) END;

    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- Run validation for all ground truth queries
\echo ''
\echo '=== Running Validation ==='

DROP TABLE IF EXISTS validation_results;
CREATE TABLE validation_results AS
SELECT v.*
FROM (SELECT DISTINCT gt.query_id, gt.query_text FROM ground_truth gt) q,
LATERAL validate_single_query(q.query_id, q.query_text) v;

-- Summary
\echo ''
\echo '=== Validation Summary ==='

SELECT
    COUNT(*) as total_queries,
    SUM(CASE WHEN scores_match THEN 1 ELSE 0 END) as scores_match_count,
    SUM(CASE WHEN docs_match THEN 1 ELSE 0 END) as docs_match_count,
    round(100.0 * SUM(CASE WHEN scores_match THEN 1 ELSE 0 END) / NULLIF(COUNT(*), 0), 1) as scores_match_pct,
    round(100.0 * SUM(CASE WHEN docs_match THEN 1 ELSE 0 END) / NULLIF(COUNT(*), 0), 1) as docs_match_pct,
    round(MAX(max_score_diff)::numeric, 6) as worst_abs_diff
FROM validation_results;

-- Known-mismatch allowlist.
-- Entries here have stable, pre-existing per-rank score discrepancies
-- against ground_truth.tsv that are tracked separately and excluded
-- from the pass/fail signal so the watchdog can still catch new
-- regressions. Each entry must reference a tracking issue.
DROP TABLE IF EXISTS known_mismatches;
CREATE TABLE known_mismatches (
    query_id int PRIMARY KEY,
    issue text NOT NULL,
    note text
);

-- (No allowlist entries currently. Add as needed when a stable
-- pre-existing score divergence is investigated and tracked.)

-- Check for failures and report
DO $$
DECLARE
    v_total int;
    v_failures int;
    v_known int;
BEGIN
    SELECT COUNT(*) INTO v_total FROM validation_results;

    -- A query fails validation if any per-rank score differs beyond
    -- tolerance. Doc-set differences (tied-cluster ordering) do not
    -- fail validation as long as scores match. Allowlisted queries
    -- (known pre-existing score discrepancies) are counted
    -- separately and do NOT trigger VALIDATION FAILED.
    SELECT COUNT(*) INTO v_failures
    FROM validation_results vr
    WHERE NOT scores_match
      AND NOT EXISTS (
          SELECT 1 FROM known_mismatches km
          WHERE km.query_id = vr.query_id
      );

    SELECT COUNT(*) INTO v_known
    FROM validation_results vr
    JOIN known_mismatches km ON km.query_id = vr.query_id
    WHERE NOT scores_match;

    IF v_known > 0 THEN
        RAISE NOTICE 'VALIDATION: % allowlisted known mismatch(es) ignored (see known_mismatches table)', v_known;
    END IF;

    IF v_failures > 0 THEN
        RAISE NOTICE 'VALIDATION FAILED: % of % queries failed (per-rank scores differ beyond % tolerance, excluding allowlist)',
            v_failures, v_total, 0.001;
    ELSE
        RAISE NOTICE 'VALIDATION PASSED: All % queries match within tolerance (% allowlisted known mismatches)',
            v_total - v_known, v_known;
    END IF;
END;
$$;

-- Failed queries detail
\echo ''
\echo '=== Validation Details (failures only) ==='
SELECT
    query_id,
    scores_match,
    docs_match,
    round(max_score_diff::numeric, 6) as max_abs_diff,
    worst_rank,
    left(details, 300) as details
FROM validation_results
WHERE NOT scores_match
ORDER BY max_score_diff DESC
LIMIT 20;

-- Cleanup
DROP TABLE IF EXISTS ground_truth;
DROP TABLE IF EXISTS validation_results;
DROP TABLE IF EXISTS known_mismatches;
DROP TABLE IF EXISTS tapir_results;
DROP FUNCTION IF EXISTS validate_single_query;

\echo ''
\echo '=== Validation Complete ==='
