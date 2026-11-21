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

-- An empty selective filter must not traverse the common ranking postings.
SELECT id FROM boolean_candidates
WHERE body @@ 'rareone'::tsquery
ORDER BY body <@> to_bm25query('common', 'boolean_candidates_idx')
LIMIT 10;
SELECT id FROM boolean_candidates
WHERE body @@ 'rareone'::tsquery
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
WHERE body @@ 'rareone & raretwo'::tsquery
ORDER BY body <@> to_bm25query('common padding', 'boolean_candidates_idx')
LIMIT 10;
SELECT id FROM boolean_candidates
WHERE body @@ 'rareone & raretwo'::tsquery
ORDER BY body <@> to_bm25query('common', 'boolean_candidates_idx')
LIMIT 10;
SELECT id FROM boolean_candidates
WHERE body @@ 'rareone <-> raretwo'::tsquery
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
    WHERE body @@ 'rareone & raretwo'::tsquery
    ORDER BY body <@> to_bm25query('common', 'boolean_candidates_idx')
    LIMIT 10
) s;
SELECT array_agg(id ORDER BY id) AS phrase_matching FROM (
    SELECT id FROM boolean_candidates
    WHERE body @@ 'rareone <-> raretwo'::tsquery
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
    WHERE body @@ 'rareone & !padding'::tsquery
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
    SELECT id FROM boolean_candidate_ties WHERE body @@ 'rare'::tsquery
    ORDER BY body <@> to_bm25query('common', 'boolean_candidate_ties_idx')
    LIMIT 10
) s;
SELECT bm25_debug_scoring_passes(true) = 1 AS no_duplicate_backoff;
DROP TABLE boolean_candidate_ties;
DROP EXTENSION pg_textsearch;
