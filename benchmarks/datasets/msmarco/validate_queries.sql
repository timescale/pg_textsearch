-- MS-MARCO Query Validation
-- Compares Tapir query results against precomputed ground truth
--
-- Requires: ground_truth.tsv in the same directory (created by CI or
--   run_benchmark.sh from the versioned ground_truth_pgNN.tsv files)
-- Usage: psql -p PORT -f validate_queries.sql
--
-- Validates:
--   1. Same documents appear in top-10 (order may vary for ties)
--   2. BM25 scores match within absolute tolerance (0.001)
--      This accounts for float4 vs float8 precision in the index

\set ON_ERROR_STOP on
\timing on

\echo '=== MS-MARCO BM25 Validation ==='
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
\copy ground_truth FROM 'benchmarks/datasets/msmarco/ground_truth.tsv' WITH (FORMAT text, DELIMITER E'\t', HEADER true)

SELECT 'Loaded ' || COUNT(DISTINCT query_id) || ' queries, ' || COUNT(*) || ' result rows' as status
FROM ground_truth;

-- Validation function that handles ties properly.
-- Uses absolute tolerance (default 0.001) to account for float4 vs float8
-- precision differences between index scoring and ground truth.
-- Rank-boundary ties (missing/extra docs with scores within tolerance of
-- the rank-10 cutoff) are counted separately from real mismatches.
CREATE OR REPLACE FUNCTION validate_single_query(
    p_query_id int,
    p_query_text text,
    p_tolerance float8 DEFAULT 0.001
) RETURNS TABLE(
    query_id int,
    docs_match boolean,
    scores_match boolean,
    missing_docs int,
    extra_docs int,
    max_score_diff float8,
    details text
) AS $$
DECLARE
    v_gt_docs int[];
    v_tapir_docs int[];
    v_missing int[];
    v_extra int[];
    v_max_score_diff float8 := 0;
    v_all_scores_match boolean := true;
    v_details text := '';
    v_gt_min_score float8;
    v_tapir_min_score float8;
    v_boundary_tolerance float8;
    v_real_missing int := 0;
    r record;
BEGIN
    -- Get Tapir's top-10 results
    CREATE TEMP TABLE IF NOT EXISTS tapir_results (
        doc_id int,
        score float8
    );
    TRUNCATE tapir_results;

    INSERT INTO tapir_results
    SELECT
        passage_id,
        -(passage_text <@> to_bm25query(p_query_text, 'msmarco_bm25_idx'))::float8 as score
    FROM msmarco_passages
    ORDER BY passage_text <@> to_bm25query(p_query_text, 'msmarco_bm25_idx')
    LIMIT 10;

    -- Get document sets
    SELECT array_agg(gt.doc_id ORDER BY gt.doc_id) INTO v_gt_docs
    FROM ground_truth gt WHERE gt.query_id = p_query_id;

    SELECT array_agg(doc_id ORDER BY doc_id) INTO v_tapir_docs
    FROM tapir_results;

    -- Find missing and extra documents
    SELECT array_agg(d) INTO v_missing
    FROM unnest(v_gt_docs) d
    WHERE d NOT IN (SELECT doc_id FROM tapir_results);

    SELECT array_agg(d) INTO v_extra
    FROM unnest(v_tapir_docs) d
    WHERE d NOT IN (SELECT gt2.doc_id FROM ground_truth gt2
                    WHERE gt2.query_id = p_query_id);

    -- Compare scores for matching documents using absolute tolerance
    FOR r IN
        SELECT
            gt.doc_id,
            gt.score as gt_score,
            t.score as tapir_score,
            ABS(gt.score - t.score) as abs_diff
        FROM ground_truth gt
        JOIN tapir_results t ON gt.doc_id = t.doc_id
        WHERE gt.query_id = p_query_id
    LOOP
        IF r.abs_diff > p_tolerance THEN
            v_all_scores_match := false;
            v_details := v_details || 'doc ' || r.doc_id::text ||
                ': gt=' || ROUND(r.gt_score::numeric, 4)::text ||
                ', tapir=' || ROUND(r.tapir_score::numeric, 4)::text ||
                '; ';
        END IF;
        v_max_score_diff := GREATEST(v_max_score_diff, r.abs_diff);
    END LOOP;

    -- Check if missing/extra docs are rank-boundary ties.
    -- If the missing doc's gt score is within tolerance of the rank-10
    -- boundary score, it's a benign tie-break difference.
    --
    -- Use the LARGER of (gt.min_score, tapir.min_score) as the boundary
    -- reference, and a slightly wider tie-break tolerance than the
    -- per-doc score-match tolerance: at the rank-10 boundary many docs
    -- can cluster with scores within a few thousandths, and the choice
    -- of which subset makes the top-10 cut becomes data-dependent
    -- (e.g., differs between single-txn COPY and concurrent-insert
    -- builds due to doc_id assignment order). Both sides agreeing on
    -- scores within tolerance for overlapping docs is the real
    -- correctness signal; which arbitrary tie-break wins is not.
    IF v_missing IS NOT NULL AND array_length(v_missing, 1) > 0 THEN
        SELECT MIN(score) INTO v_gt_min_score FROM ground_truth
        WHERE ground_truth.query_id = p_query_id;
        SELECT MIN(score) INTO v_tapir_min_score FROM tapir_results;
        v_boundary_tolerance := GREATEST(p_tolerance, 0.01);

        FOR r IN
            SELECT d as doc_id, gt.score as gt_score
            FROM unnest(v_missing) d
            JOIN ground_truth gt ON gt.doc_id = d
                AND gt.query_id = p_query_id
        LOOP
            -- Benign if the missing doc's gt score is within tolerance
            -- of EITHER rank-10 boundary (gt's or tapir's) -- both
            -- mean we're in the tied cluster at the cutoff.
            IF ABS(r.gt_score - v_tapir_min_score) > v_boundary_tolerance
               AND ABS(r.gt_score - v_gt_min_score) > v_boundary_tolerance
            THEN
                v_real_missing := v_real_missing + 1;
            END IF;
        END LOOP;
    END IF;

    -- Build result
    query_id := p_query_id;
    -- docs_match is false only for non-tie missing docs
    docs_match := (v_real_missing = 0);
    scores_match := v_all_scores_match;
    missing_docs := COALESCE(array_length(v_missing, 1), 0);
    extra_docs := COALESCE(array_length(v_extra, 1), 0);
    max_score_diff := v_max_score_diff;

    IF v_missing IS NOT NULL AND array_length(v_missing, 1) > 0 THEN
        v_details := v_details || 'Missing: ' ||
            array_to_string(v_missing, ',') || '; ';
    END IF;
    IF v_extra IS NOT NULL AND array_length(v_extra, 1) > 0 THEN
        v_details := v_details || 'Extra: ' ||
            array_to_string(v_extra, ',') || '; ';
    END IF;

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
    SUM(CASE WHEN docs_match THEN 1 ELSE 0 END) as docs_match_count,
    SUM(CASE WHEN scores_match THEN 1 ELSE 0 END) as scores_match_count,
    round(100.0 * SUM(CASE WHEN docs_match THEN 1 ELSE 0 END) / NULLIF(COUNT(*), 0), 1) as docs_match_pct,
    round(100.0 * SUM(CASE WHEN scores_match THEN 1 ELSE 0 END) / NULLIF(COUNT(*), 0), 1) as scores_match_pct,
    round(MAX(max_score_diff)::numeric, 6) as worst_abs_diff
FROM validation_results;

-- Known-mismatch allowlist.
-- These queries have stable, pre-existing score discrepancies against
-- ground_truth.tsv that are tracked separately and excluded from the
-- pass/fail signal so the watchdog can still catch *new* regressions.
-- Each entry must reference a tracking issue.
DROP TABLE IF EXISTS known_mismatches;
CREATE TABLE known_mismatches (
    query_id int PRIMARY KEY,
    issue text NOT NULL,
    note text
);

INSERT INTO known_mismatches (query_id, issue, note) VALUES
    -- See #361. doc 1838655 scores ~1.5 below ground truth on this
    -- query; root cause not yet isolated (predates #360, likely a
    -- tokenization or corpus-stats edge case specific to the leading
    -- '+' / double-space pattern).
    (158, 'https://github.com/timescale/pg_textsearch/issues/361',
     '+how to add differnt names on labels in word: doc 1838655 score diff ~1.5'),
    -- See #363. Concurrent-INSERT path only: doc 1956260 (rank 5 in
    -- ground truth, score 20.297) absent from the index's top-10.
    -- Overlapping docs match scores within 3e-6, so it's not a
    -- scoring-formula bug -- either pgbench failed to insert that
    -- specific row, or another concurrent-path indexing quirk is at
    -- play. Single-txn COPY path passes this query cleanly.
    (1267, 'https://github.com/timescale/pg_textsearch/issues/363',
     '3 ways a log can move when bucking on side hill: doc 1956260 missing on concurrent path only');

-- Check for failures and report
DO $$
DECLARE
    v_total int;
    v_failures int;
    v_match_pct numeric;
    v_known int;
BEGIN
    SELECT COUNT(*) INTO v_total FROM validation_results;

    -- A query fails validation if scores differ beyond tolerance
    -- OR if docs differ for non-tie-break reasons.
    -- Allowlisted queries (known pre-existing discrepancies) are
    -- counted separately and do NOT trigger VALIDATION FAILED.
    SELECT COUNT(*) INTO v_failures
    FROM validation_results vr
    WHERE (NOT scores_match OR NOT docs_match)
      AND NOT EXISTS (
          SELECT 1 FROM known_mismatches km
          WHERE km.query_id = vr.query_id
      );

    SELECT COUNT(*) INTO v_known
    FROM validation_results vr
    JOIN known_mismatches km ON km.query_id = vr.query_id
    WHERE NOT scores_match OR NOT docs_match;

    v_match_pct := 100.0 * (v_total - v_failures - v_known) / NULLIF(v_total, 0);

    IF v_known > 0 THEN
        RAISE NOTICE 'VALIDATION: % allowlisted known mismatch(es) ignored (see known_mismatches table)', v_known;
    END IF;

    IF v_failures > 0 THEN
        RAISE NOTICE 'VALIDATION FAILED: % of % queries failed (scores differ beyond 0.001 tolerance or non-tie doc mismatches, excluding allowlist)',
            v_failures, v_total;
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
    docs_match,
    scores_match,
    missing_docs,
    extra_docs,
    round(max_score_diff::numeric, 6) as max_abs_diff,
    left(details, 200) as details
FROM validation_results
WHERE NOT docs_match OR NOT scores_match
ORDER BY missing_docs DESC, max_score_diff DESC
LIMIT 20;

-- Cleanup
DROP TABLE IF EXISTS ground_truth;
DROP TABLE IF EXISTS validation_results;
DROP TABLE IF EXISTS known_mismatches;
DROP TABLE IF EXISTS tapir_results;
DROP FUNCTION IF EXISTS validate_single_query;

\echo ''
\echo '=== Validation Complete ==='
