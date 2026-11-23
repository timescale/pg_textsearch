\pset format unaligned
CREATE EXTENSION pg_textsearch;
SET default_text_search_config = 'pg_catalog.simple';
SET enable_seqscan = off;
SET jit = off;
SET pg_textsearch.filtered_seed = off;
CREATE TABLE boolean_candidates (id int PRIMARY KEY, body text);
INSERT INTO boolean_candidates
SELECT g, 'common padding' FROM generate_series(1, 4096) g;
INSERT INTO boolean_candidates VALUES
    (4097, 'common rareone'), (4098, 'common raretwo');
CREATE INDEX boolean_candidates_idx ON boolean_candidates USING bm25(body)
    WITH (text_config = 'simple');

-- Selective filters must not traverse all common ranking postings.
SELECT id FROM boolean_candidates
WHERE body @@ 'common & rareone'::tsquery
ORDER BY body <@> to_bm25query('common', 'boolean_candidates_idx')
LIMIT 10;
SELECT id FROM boolean_candidates
WHERE body @@ 'common & rareone'::tsquery
ORDER BY body <@> to_bm25query('common padding', 'boolean_candidates_idx')
LIMIT 10;
SELECT id FROM boolean_candidates
WHERE body @@ 'rareone'::tsquery
ORDER BY body <@> to_bm25query('padding', 'boolean_candidates_idx')
LIMIT 10;
SET log_statement = none;
SET client_min_messages = log;
SET pg_textsearch.log_bmw_stats = on;
SELECT id FROM boolean_candidates
WHERE body @@ 'common & rareone & raretwo'::tsquery
ORDER BY body <@> to_bm25query('common padding', 'boolean_candidates_idx')
LIMIT 10;
SELECT id FROM boolean_candidates
WHERE body @@ 'common & rareone & raretwo'::tsquery
ORDER BY body <@> to_bm25query('common', 'boolean_candidates_idx')
LIMIT 10;
SELECT id FROM boolean_candidates
WHERE body @@ 'common & (rareone <-> raretwo)'::tsquery
ORDER BY body <@> to_bm25query('common padding', 'boolean_candidates_idx')
LIMIT 10;
RESET pg_textsearch.log_bmw_stats;
SET client_min_messages = warning;
RESET log_statement;

INSERT INTO boolean_candidates VALUES
    (4099, 'common rareone raretwo'),
    (4100, 'common rareone padding raretwo'),
    (4101, 'rareone raretwo');
SELECT bm25_spill_index('boolean_candidates_idx') > 0 AS spilled;
SELECT array_agg(id ORDER BY id) AS matching FROM (
    SELECT id FROM boolean_candidates
    WHERE body @@ 'common & rareone & raretwo'::tsquery
    ORDER BY body <@> to_bm25query('common', 'boolean_candidates_idx')
    LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS phrase_matching FROM (
    SELECT id FROM boolean_candidates
    WHERE body @@ 'common & (rareone <-> raretwo)'::tsquery
    ORDER BY body <@> to_bm25query('common padding', 'boolean_candidates_idx')
    LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS disjunction_matching FROM (
    SELECT id FROM boolean_candidates
    WHERE body @@ '(rareone & raretwo) | nonexistent'::tsquery
    ORDER BY body <@> to_bm25query('common', 'boolean_candidates_idx')
    LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS negation_matching FROM (
    SELECT id FROM boolean_candidates
    WHERE body @@ 'common & rareone & !padding'::tsquery
    ORDER BY body <@> to_bm25query('common', 'boolean_candidates_idx')
    LIMIT 10
) s;
SELECT count(*) AS exact_or_count FROM boolean_candidates
WHERE body @@ 'common | rareone | raretwo'::tsquery;
SELECT count(*) AS lossy_or_count FROM boolean_candidates
WHERE body @@ '(rareone <-> raretwo) | nonexistent'::tsquery;
DROP TABLE boolean_candidates;
CREATE TABLE boolean_candidate_ties (id int, body text);
INSERT INTO boolean_candidate_ties
SELECT g, 'common' || CASE WHEN g % 100 = 0 THEN ' rare' ELSE '' END
FROM generate_series(1, 1000) g;
CREATE INDEX boolean_candidate_ties_idx
ON boolean_candidate_ties USING bm25(body) WITH (text_config = 'simple');
SET pg_textsearch.default_limit = 10;
SELECT bm25_debug_scoring_passes(true) >= 0 AS reset_passes;
SELECT array_agg(id ORDER BY id) AS tied_candidates FROM (
    SELECT id FROM boolean_candidate_ties
    WHERE body @@ 'common & rare'::tsquery
    ORDER BY body <@> to_bm25query('common', 'boolean_candidate_ties_idx')
    LIMIT 10
) s;
SELECT bm25_debug_scoring_passes(true) = 1 AS no_duplicate_backoff;
DROP TABLE boolean_candidate_ties;

CREATE TABLE boolean_candidate_prefix (id int, body text);
INSERT INTO boolean_candidate_prefix
SELECT g, CASE WHEN g = 1 THEN repeat('rankcommon ', 100)
               ELSE 'rankcommon ' END || 'prefix' || (g % 512)
FROM generate_series(1, 4096) g;
CREATE INDEX boolean_candidate_prefix_idx
ON boolean_candidate_prefix USING bm25(body) WITH (text_config = 'simple');
SET pg_textsearch.default_limit = 1;
CREATE FUNCTION pg_temp.bounded_candidate_reads(
    query text, max_reads bigint DEFAULT 256)
RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
    plan json;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, BUFFERS, FORMAT JSON) ' || query
        INTO plan;
    RETURN plan->0->'Plan'->'Plans'->0->>'Node Type' = 'Index Scan'
       AND plan->0->'Plan'->'Plans'->0->>'Order By' IS NOT NULL
       AND (plan->0->'Plan'->>'Shared Hit Blocks')::bigint
         + (plan->0->'Plan'->>'Shared Read Blocks')::bigint < max_reads;
END
$$;
-- A dense prefix must not enumerate all postings before falling back.
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_prefix
    WHERE body @@ 'rankcommon & prefix:*'::tsquery
    ORDER BY body <@> to_bm25query('rankcommon',
                                 'boolean_candidate_prefix_idx')
    LIMIT 1
$q$) AS dense_prefix_bounded;
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_prefix
    WHERE body @@ 'absentfirst & prefix:*'::tsquery
    ORDER BY body <@> to_bm25query('absentfirst absentsecond',
                                 'boolean_candidate_prefix_idx')
    LIMIT 1
$q$) AS absent_ranking_bounded;
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_prefix
    WHERE body @@ 'prefix1 | prefix2'::tsquery
    ORDER BY body <@> to_bm25query('absentfirst absentsecond',
                                 'boolean_candidate_prefix_idx')
    LIMIT 1
$q$) AS independent_candidates_bounded;
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_prefix
    WHERE body @@ '!absent'::tsquery
    ORDER BY body <@> to_bm25query('absent',
                                 'boolean_candidate_prefix_idx')
    LIMIT 1
$q$) AS negative_zero_fill_bounded;
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_prefix
    WHERE body @@ 'prefix:*'::tsquery
    ORDER BY body <@> to_bm25query('rankcommon',
                                 'boolean_candidate_prefix_idx')
    LIMIT 1
$q$) AS unanchored_prefix_bounded;
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_prefix
    WHERE body @@ 'prefix:*'::tsquery
    ORDER BY body <@> to_bm25query('absent',
                                 'boolean_candidate_prefix_idx')
    LIMIT 1
$q$) AS prefix_zero_fill_bounded;
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_prefix
    WHERE body @@ 'missing & prefix:*'::tsquery
    ORDER BY body <@> to_bm25query('absent',
                                 'boolean_candidate_prefix_idx')
    LIMIT 1
$q$) AS missing_prefix_anchor_bounded;
SELECT bool_and(pg_temp.bounded_candidate_reads(format($q$
    SELECT id FROM boolean_candidate_prefix WHERE body @@ %L::tsquery
    ORDER BY body <@> to_bm25query('absent',
                                 'boolean_candidate_prefix_idx')
    LIMIT 1
$q$, predicate), 32)) AS negative_prefix_anchors_bounded
FROM (VALUES
    ('!absent & missingprefix:*'),
    ('missingprefix:* & !absent'),
    ('(!absent | another) & missingprefix:*')
) queries(predicate);
SELECT count(*) = 4096 AS complete_prefix_zero_matches FROM (
    SELECT id FROM boolean_candidate_prefix
    WHERE body @@ 'prefix:*'::tsquery
    ORDER BY body <@> to_bm25query('absent',
                                 'boolean_candidate_prefix_idx')
    LIMIT 5000
) s;
SELECT id FROM boolean_candidate_prefix
WHERE body @@ 'rankcommon & prefix:*'::tsquery
ORDER BY body <@> to_bm25query('rankcommon', 'boolean_candidate_prefix_idx')
LIMIT 1;
DROP TABLE boolean_candidate_prefix;

SET pg_textsearch.bulk_load_threshold = 0;
SET pg_textsearch.memtable_pages_threshold = 0;
SET max_parallel_maintenance_workers = 0;
CREATE TABLE boolean_candidate_inblock (id int, body text);
INSERT INTO boolean_candidate_inblock
SELECT g, CASE WHEN g % 2 = 0 THEN 'rank ' ELSE 'filler ' END ||
    CASE WHEN g <= 256 AND g % 4 = 0 THEN 'tag' ELSE 'padding' END
FROM generate_series(1, 32768) g;
CREATE INDEX boolean_candidate_inblock_idx
ON boolean_candidate_inblock USING bm25(body) WITH (text_config = 'simple');
-- Sparse candidates within one posting block must not repeatedly reload it.
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_inblock
    WHERE body @@ 'rank & tag'::tsquery
    ORDER BY body <@> to_bm25query('rank', 'boolean_candidate_inblock_idx')
    LIMIT 1
$q$, 64) AS inblock_candidates_bounded;
SELECT id FROM boolean_candidate_inblock
WHERE body @@ 'rank & tag'::tsquery
ORDER BY body <@> to_bm25query('rank', 'boolean_candidate_inblock_idx')
LIMIT 1;
DROP TABLE boolean_candidate_inblock;

CREATE TABLE boolean_candidate_dominated (id int, body text);
INSERT INTO boolean_candidate_dominated
SELECT g, CASE WHEN g = 2 THEN repeat('rank ', 100)
               WHEN g % 2 = 0 THEN 'rank ' ELSE 'filler ' END ||
    CASE WHEN g BETWEEN 8194 AND 24322 AND g % 256 = 2
         THEN 'tag' ELSE 'padding' END
FROM generate_series(1, 32768) g;
CREATE INDEX boolean_candidate_dominated_idx
ON boolean_candidate_dominated USING bm25(body)
WITH (text_config = 'simple');
INSERT INTO boolean_candidate_dominated
VALUES (32769, repeat('rank ', 100) || 'tag');
-- A memtable winner must prune dominated candidate blocks before loading.
SELECT pg_temp.bounded_candidate_reads($q$
    SELECT id FROM boolean_candidate_dominated
    WHERE body @@ 'rank & tag'::tsquery
    ORDER BY body <@> to_bm25query('rank', 'boolean_candidate_dominated_idx')
    LIMIT 1
$q$) AS dominated_candidates_bounded;
SELECT id FROM boolean_candidate_dominated
WHERE body @@ 'rank & tag'::tsquery
ORDER BY body <@> to_bm25query('rank', 'boolean_candidate_dominated_idx')
LIMIT 1;
DROP TABLE boolean_candidate_dominated;
RESET max_parallel_maintenance_workers;
RESET pg_textsearch.memtable_pages_threshold;
RESET pg_textsearch.bulk_load_threshold;
DROP EXTENSION pg_textsearch;
