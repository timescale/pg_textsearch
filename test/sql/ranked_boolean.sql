CREATE EXTENSION pg_textsearch;
SET default_text_search_config = 'pg_catalog.simple';
SET client_min_messages = WARNING;
SET enable_seqscan = off;
SET jit = off;
SET pg_textsearch.filtered_seed = off;
SET pg_textsearch.default_limit = 1;

CREATE TABLE ranked_boolean_docs (id integer PRIMARY KEY, body text);
INSERT INTO ranked_boolean_docs
SELECT g, repeat('alpha ', 100) FROM generate_series(1, 1024) g;
INSERT INTO ranked_boolean_docs
SELECT g, 'alpha ' || repeat('padding ', 100) || 'beta'
FROM generate_series(1025, 1044) g;
INSERT INTO ranked_boolean_docs VALUES (1045, 'alpha alpha beta');
CREATE INDEX ranked_boolean_idx ON ranked_boolean_docs USING bm25(body)
WITH (text_config = 'simple');
INSERT INTO ranked_boolean_docs VALUES
    (9001, 'alpha alpha alpha beta'),
    (9002, 'beta alpha'),
    (9003, 'alpha alpha alpha alpha padding beta');

CREATE FUNCTION pg_temp.ranked_boolean_plan(query text)
RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    plan json;
BEGIN
    EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || query INTO plan;
    RETURN plan->0->'Plan'->'Plans'->0->>'Node Type' = 'Index Scan'
       AND plan->0->'Plan'->'Plans'->0->>'Index Cond' IS NOT NULL
       AND plan->0->'Plan'->'Plans'->0->>'Order By' IS NOT NULL;
END
$$;

SELECT pg_temp.ranked_boolean_plan($q$
    SELECT id FROM ranked_boolean_docs
    WHERE body @@ 'alpha & beta'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
    LIMIT 1
$q$) AS boolean_ordered_index_scan;

-- Rejected high-scoring documents must not fill the internal top-k heap.
SELECT bm25_debug_scoring_passes(true) >= 0 AS reset_passes;
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'alpha & beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
LIMIT 1;
SELECT bm25_debug_scoring_passes(true) = 1 AS filter_before_topk;

SET pg_textsearch.memtable_cache_enabled = off;
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'alpha & beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
LIMIT 1;
RESET pg_textsearch.memtable_cache_enabled;

-- Positions are rechecked against the heap, with deeper scoring as needed.
SELECT array_agg(id ORDER BY id) AS phrase_matches FROM (
    SELECT id FROM ranked_boolean_docs
    WHERE body @@ 'alpha <-> beta'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
    LIMIT 2
) s;
SELECT bm25_debug_scoring_passes(true) > 1 AS phrase_recheck_backoff;

-- Full-set oracles use hand-derived IDs, independently of scoring.
SELECT array_agg(id ORDER BY id) AS boolean_matches FROM (
    SELECT id FROM ranked_boolean_docs
    WHERE body @@ '(beta | (alpha & absent)) & !padding'::tsquery
    ORDER BY body <@> to_bm25query('alpha beta', 'ranked_boolean_idx')
    LIMIT 2000
) s;
SELECT array_agg(id ORDER BY id) AS prefix_matches FROM (
    SELECT id FROM ranked_boolean_docs
    WHERE body @@ 'alpha & bet:* & !padding'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
    LIMIT 2000
) s;
SELECT count(*) AS weighted_matches FROM (
    SELECT id FROM ranked_boolean_docs
    WHERE body @@ 'alpha:A'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
    LIMIT 2000
) s;

SET plan_cache_mode = force_generic_plan;
PREPARE ranked_boolean_scan(tsquery) AS
SELECT id FROM ranked_boolean_docs WHERE body @@ $1
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
LIMIT 1;
SELECT pg_temp.ranked_boolean_plan(
    'EXECUTE ranked_boolean_scan(''beta & !padding''::tsquery)'
) AS generic_boolean_ordered_index_scan;
EXECUTE ranked_boolean_scan('beta & !padding'::tsquery);
EXECUTE ranked_boolean_scan('alpha <-> beta'::tsquery);
EXECUTE ranked_boolean_scan(NULL);
EXECUTE ranked_boolean_scan(''::tsquery);
-- Unknown predicates use SQL filtering, including over-budget queries.
SELECT '(beta & !padding) | ' || string_agg('unused' || g, ' | ')
    AS oversized_query FROM generate_series(1, 63) g \gset
EXECUTE ranked_boolean_scan(:'oversized_query'::tsquery);
SELECT '!alpha & ' || string_agg('!unused' || g, ' & ')
    AS oversized_query FROM generate_series(1, 64) g \gset
EXECUTE ranked_boolean_scan(:'oversized_query'::tsquery);
DEALLOCATE ranked_boolean_scan;
RESET plan_cache_mode;

SELECT wanted.n, found.id
FROM (VALUES
    (1, 'beta & !padding'::tsquery),
    (2, 'alpha <-> beta'::tsquery),
    (3, 'padding'::tsquery),
    (4, NULL::tsquery)
) wanted(n, query)
LEFT JOIN LATERAL (
    SELECT id FROM ranked_boolean_docs WHERE body @@ wanted.query
    ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
    LIMIT 1
) found ON true
ORDER BY wanted.n;

CREATE FUNCTION pg_temp.ranked_boolean_standalone(doc_id integer)
RETURNS float8 LANGUAGE plpgsql AS $$
BEGIN
    RETURN (
        SELECT body <@> to_bm25query('alpha beta', 'ranked_boolean_idx')
        FROM ranked_boolean_docs WHERE id = doc_id
    );
END
$$;
-- Deliberately compare cached index scores with standalone scoring.
SELECT bool_and(abs(score - pg_temp.ranked_boolean_standalone(id))
                < 0.000001) AS score_parity
FROM (
    SELECT id,
           body <@> to_bm25query('alpha beta', 'ranked_boolean_idx') AS score
    FROM ranked_boolean_docs
    WHERE body @@ 'alpha & beta & !padding'::tsquery
    ORDER BY body <@> to_bm25query('alpha beta', 'ranked_boolean_idx')
    LIMIT 10
) s;

BEGIN;
DECLARE ranked_boolean_cursor CURSOR FOR
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'alpha & beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx');
FETCH 1 FROM ranked_boolean_cursor;
SELECT bm25_spill_index('ranked_boolean_idx') > 0 AS cursor_spilled;
FETCH 2 FROM ranked_boolean_cursor;
CLOSE ranked_boolean_cursor;
COMMIT;
INSERT INTO ranked_boolean_docs VALUES (1046, 'alpha padding beta');

DELETE FROM ranked_boolean_docs WHERE id = 9001;
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'alpha & beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
LIMIT 1;
SELECT bm25_spill_index('ranked_boolean_idx') > 0 AS spilled;
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'alpha & beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
LIMIT 1;
SELECT bm25_force_merge('ranked_boolean_idx') IS NOT NULL AS merged;
SELECT array_agg(id ORDER BY id) AS merged_matches FROM (
    SELECT id FROM ranked_boolean_docs
    WHERE body @@ 'alpha & beta & !padding'::tsquery
    ORDER BY body <@> to_bm25query('alpha beta', 'ranked_boolean_idx')
    LIMIT 2000
) s;

DROP TABLE ranked_boolean_docs;

CREATE TABLE ranked_disjoint_docs (id integer PRIMARY KEY, body text);
INSERT INTO ranked_disjoint_docs VALUES
    (1, 'alpha beta'), (2, 'beta'), (3, ''), (4, 'gamma');
CREATE INDEX ranked_disjoint_idx ON ranked_disjoint_docs USING bm25(body)
WITH (text_config = 'simple');
CREATE FUNCTION pg_temp.ranked_score_is_cached()
RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    plan json;
BEGIN
    EXECUTE $q$
        EXPLAIN (FORMAT JSON, COSTS OFF, VERBOSE)
        SELECT id,
               body <@> to_bm25query('alpha', 'ranked_disjoint_idx') AS score
        FROM ranked_disjoint_docs WHERE body @@ 'alpha'::tsquery
        ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 1
    $q$ INTO plan;
    RETURN (plan->0->'Plan'->'Plans'->0->'Output')::text
           LIKE '%bm25_get_current_score%';
END
$$;
SELECT pg_temp.ranked_score_is_cached() AS constant_filter_score_cached;
-- Filtering must retain matches that have a zero ranking score.
SELECT array_agg(id ORDER BY id) AS disjoint_matches FROM (
    SELECT id FROM ranked_disjoint_docs WHERE body @@ 'beta'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS disjoint_or_matches FROM (
    SELECT id FROM ranked_disjoint_docs WHERE body @@ 'alpha | gamma'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS prefix_zero_scores FROM (
    SELECT id FROM ranked_disjoint_docs WHERE body @@ 'bet:*'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS pure_not_matches FROM (
    SELECT id FROM ranked_disjoint_docs WHERE body @@ '!absent'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS no_rank_matches FROM (
    SELECT id FROM ranked_disjoint_docs WHERE body @@ 'beta'::tsquery
    ORDER BY body <@> to_bm25query('absent', 'ranked_disjoint_idx') LIMIT 10
) s;
SET plan_cache_mode = force_generic_plan;
PREPARE ranked_disjoint_scan(tsquery) AS
SELECT id FROM ranked_disjoint_docs WHERE body @@ $1
ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10;
EXECUTE ranked_disjoint_scan('beta'::tsquery);
DEALLOCATE ranked_disjoint_scan;
PREPARE ranked_unknown_score(bm25query) AS
SELECT id FROM ranked_disjoint_docs WHERE body @@ 'alpha'::tsquery
ORDER BY body <@> $1 LIMIT 10;
EXECUTE ranked_unknown_score(to_bm25query('absent', 'ranked_disjoint_idx'));
DEALLOCATE ranked_unknown_score;
RESET plan_cache_mode;
CREATE FUNCTION pg_temp.ranked_recheck_query()
RETURNS tsquery LANGUAGE plpgsql STABLE AS $$
BEGIN
    PERFORM id FROM ranked_disjoint_docs
    ORDER BY body <@> to_bm25query('beta', 'ranked_disjoint_idx') LIMIT 1;
    RETURN 'alpha:D'::tsquery;
END
$$;
CREATE FUNCTION pg_temp.ranked_disjoint_score(doc_id integer)
RETURNS float8 LANGUAGE plpgsql AS $$
BEGIN
    RETURN (
        SELECT body <@> to_bm25query('alpha', 'ranked_disjoint_idx')
        FROM ranked_disjoint_docs WHERE id = doc_id
    );
END
$$;
-- A nested scan in an index recheck must not replace the projected score.
CREATE TEMP TABLE ranked_recheck_scores AS
SELECT id, body <@> to_bm25query('alpha', 'ranked_disjoint_idx') AS score
FROM ranked_disjoint_docs WHERE body @@ pg_temp.ranked_recheck_query()
ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10;
SELECT count(*) = 1 AND
       bool_and(abs(score - pg_temp.ranked_disjoint_score(id)) < 0.000001)
       AS recheck_score_parity
FROM ranked_recheck_scores;
DROP TABLE ranked_recheck_scores;

\pset format unaligned
SELECT name, pg_temp.ranked_boolean_plan(format($q$
    SELECT id FROM ranked_disjoint_docs WHERE body @@ %L::tsquery
    ORDER BY body <@> to_bm25query(%L, 'ranked_disjoint_idx') LIMIT 10
$q$, predicate, ranking)) AS ordered_index_scan
FROM (VALUES
    ('independent', 'beta', 'alpha'),
    ('nested', '(beta | gamma) & !alpha', 'alpha gamma'),
    ('negative', '!alpha', 'alpha'),
    ('prefix', 'bet:*', 'alpha'),
    ('anchored prefix', 'beta & bet:*', 'alpha'),
    ('anchored disjunction', '(beta & bet:*) | (gamma & gam:*)', 'alpha'),
    ('prefix disjunction', 'beta | bet:*', 'alpha'),
    ('negated prefix', '!alpha & bet:*', 'alpha'),
    ('weighted', 'beta:A', 'alpha'),
    ('empty ranking', 'beta', '')
) cases(name, predicate, ranking)
ORDER BY name;
SELECT pg_temp.ranked_boolean_plan($q$
    SELECT id FROM ranked_disjoint_docs
    WHERE body @@ 'beta'::tsquery AND body @@ '!alpha'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10
$q$) AS multiple_boolean_keys;
-- One key that requires an exact term lets ranked scans filter the rest.
SELECT pg_temp.ranked_boolean_plan($q$
    SELECT id FROM ranked_disjoint_docs
    WHERE body @@ 'bet:*'::tsquery AND body @@ 'beta | gamma'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10
$q$) AS anchored_prefix_keys;
SELECT 'beta | ' || string_agg('unused' || g, ' | ') AS oversized_query
FROM generate_series(1, 64) g \gset
SELECT pg_temp.ranked_boolean_plan(format($q$
    SELECT id FROM ranked_disjoint_docs WHERE body @@ %L::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10
$q$, :'oversized_query')) AS oversized_ordered_index_scan;
SELECT array_agg(id ORDER BY id) AS oversized_matches FROM (
    SELECT id FROM ranked_disjoint_docs
    WHERE body @@ :'oversized_query'::tsquery
    ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS nested_zero_matches FROM (
    SELECT id FROM ranked_disjoint_docs
    WHERE body @@ '(beta | gamma) & !alpha'::tsquery
    ORDER BY body <@> to_bm25query('alpha absent', 'ranked_disjoint_idx')
    LIMIT 10
) s;
SELECT count(*) = 2 AND bool_and(score = 0) AS empty_ranking_scores FROM (
    SELECT body <@> to_bm25query('', 'ranked_disjoint_idx') AS score
    FROM ranked_disjoint_docs WHERE body @@ 'beta'::tsquery
    ORDER BY body <@> to_bm25query('', 'ranked_disjoint_idx') LIMIT 10
) s;

SET plan_cache_mode = force_generic_plan;
PREPARE ranked_runtime_both(tsquery, bm25query) AS
SELECT id, (body <@> $2) IS NULL AS null_score
FROM ranked_disjoint_docs WHERE body @@ $1
ORDER BY body <@> $2 LIMIT 10;
SELECT pg_temp.ranked_boolean_plan($q$
    EXECUTE ranked_runtime_both(
        'beta', to_bm25query('absent', 'ranked_disjoint_idx'))
$q$) AS runtime_queries_ordered_index_scan;
EXECUTE ranked_runtime_both(
    'beta', to_bm25query('absent', 'ranked_disjoint_idx'));
EXECUTE ranked_runtime_both('!absent', NULL);
EXECUTE ranked_runtime_both(
    NULL, to_bm25query('alpha', 'ranked_disjoint_idx'));
EXECUTE ranked_runtime_both(
    'beta', to_bm25query('absent', 'ranked_disjoint_idx'));
DEALLOCATE ranked_runtime_both;
PREPARE ranked_runtime_keys(tsquery, tsquery) AS
SELECT id FROM ranked_disjoint_docs WHERE body @@ $1 AND body @@ $2
ORDER BY body <@> to_bm25query('alpha', 'ranked_disjoint_idx') LIMIT 10;
EXECUTE ranked_runtime_keys('beta', '!alpha');
EXECUTE ranked_runtime_keys('', 'beta');
EXECUTE ranked_runtime_keys('beta', '');
EXECUTE ranked_runtime_keys(NULL, 'beta');
EXECUTE ranked_runtime_keys('beta', '!absent');

-- Individually bounded runtime keys can exceed the combined operand cap.
SELECT 'beta | ' || string_agg('unused' || g, ' | ') AS first_query,
       '!alpha & ' || string_agg('!unused' || g, ' & ') AS second_query
FROM generate_series(1, 39) g \gset
EXECUTE ranked_runtime_keys(:'first_query', :'second_query');
DEALLOCATE ranked_runtime_keys;
PREPARE boolean_runtime_keys(tsquery, tsquery) AS
SELECT array_agg(id ORDER BY id) AS runtime_matches
FROM ranked_disjoint_docs WHERE body @@ $1 AND body @@ $2;
EXECUTE boolean_runtime_keys(:'first_query', :'second_query');
DEALLOCATE boolean_runtime_keys;
PREPARE boolean_mixed_keys(tsquery) AS
SELECT array_agg(id ORDER BY id) AS mixed_matches
FROM ranked_disjoint_docs
WHERE body @@ :'first_query'::tsquery AND body @@ $1;
EXECUTE boolean_mixed_keys(:'second_query');
DEALLOCATE boolean_mixed_keys;
RESET plan_cache_mode;

INSERT INTO ranked_disjoint_docs VALUES (5, 'beta delta'), (6, '');
CREATE FUNCTION pg_temp.ranked_zero_matches()
RETURNS integer[] LANGUAGE sql AS $$
    SELECT array_agg(id ORDER BY id) FROM (
        SELECT id FROM ranked_disjoint_docs WHERE body @@ '!alpha'::tsquery
        ORDER BY body <@> to_bm25query('alpha absent', 'ranked_disjoint_idx')
        LIMIT 10
    ) s;
$$;
SELECT pg_temp.ranked_zero_matches() AS mixed_zero_matches;
SET pg_textsearch.memtable_cache_enabled = off;
SELECT pg_temp.ranked_zero_matches() AS chain_zero_matches;
RESET pg_textsearch.memtable_cache_enabled;
UPDATE ranked_disjoint_docs SET body = 'alpha' WHERE id = 2;
DELETE FROM ranked_disjoint_docs WHERE id = 4;
SELECT pg_temp.ranked_zero_matches() AS live_zero_matches;
SELECT bm25_spill_index('ranked_disjoint_idx') > 0 AS zero_spilled;
VACUUM ranked_disjoint_docs;
SELECT pg_temp.ranked_zero_matches() AS spilled_zero_matches;
SELECT bm25_force_merge('ranked_disjoint_idx') IS NOT NULL AS zero_merged;
SELECT pg_temp.ranked_zero_matches() AS merged_zero_matches;
TRUNCATE ranked_disjoint_docs;
INSERT INTO ranked_disjoint_docs VALUES (7, ''), (8, '');
SELECT pg_temp.ranked_zero_matches() AS empty_corpus_matches;
SELECT bm25_spill_index('ranked_disjoint_idx') > 0 AS empty_corpus_spilled;
SELECT pg_temp.ranked_zero_matches() AS empty_segment_matches;
\pset format aligned
DROP TABLE ranked_disjoint_docs;
DROP EXTENSION pg_textsearch;
