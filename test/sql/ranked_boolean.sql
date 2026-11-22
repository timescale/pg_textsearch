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
WHERE body @@ 'beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
LIMIT 1;
SELECT bm25_debug_scoring_passes(true) = 1 AS filter_before_topk;

SET pg_textsearch.memtable_cache_enabled = off;
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'beta & !padding'::tsquery
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
    WHERE body @@ '(beta | absent) & !padding'::tsquery
    ORDER BY body <@> to_bm25query('alpha beta', 'ranked_boolean_idx')
    LIMIT 2000
) s;
SELECT array_agg(id ORDER BY id) AS prefix_matches FROM (
    SELECT id FROM ranked_boolean_docs
    WHERE body @@ 'bet:* & !padding'::tsquery
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
-- Generic plans must recheck queries beyond the internal predicate budget.
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
    WHERE body @@ 'beta & !padding'::tsquery
    ORDER BY body <@> to_bm25query('alpha beta', 'ranked_boolean_idx')
    LIMIT 10
) s;

BEGIN;
DECLARE ranked_boolean_cursor CURSOR FOR
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx');
FETCH 1 FROM ranked_boolean_cursor;
SELECT bm25_spill_index('ranked_boolean_idx') > 0 AS cursor_spilled;
FETCH 2 FROM ranked_boolean_cursor;
CLOSE ranked_boolean_cursor;
COMMIT;
INSERT INTO ranked_boolean_docs VALUES (1046, 'alpha padding beta');

DELETE FROM ranked_boolean_docs WHERE id = 9001;
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
LIMIT 1;
SELECT bm25_spill_index('ranked_boolean_idx') > 0 AS spilled;
SELECT id FROM ranked_boolean_docs
WHERE body @@ 'beta & !padding'::tsquery
ORDER BY body <@> to_bm25query('alpha', 'ranked_boolean_idx')
LIMIT 1;
SELECT bm25_force_merge('ranked_boolean_idx') IS NOT NULL AS merged;
SELECT array_agg(id ORDER BY id) AS merged_matches FROM (
    SELECT id FROM ranked_boolean_docs
    WHERE body @@ 'beta & !padding'::tsquery
    ORDER BY body <@> to_bm25query('alpha beta', 'ranked_boolean_idx')
    LIMIT 2000
) s;

DROP TABLE ranked_boolean_docs;
DROP EXTENSION pg_textsearch;
