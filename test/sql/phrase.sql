-- Test phrase queries with positional information (V6 segments)
-- Exercises the position index, phrase matching, and backward compatibility.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- =============================================================
-- Test 1: Basic phrase match — adjacent terms
-- =============================================================
CREATE TABLE ph_basic (id serial PRIMARY KEY, content text);

INSERT INTO ph_basic (content) VALUES
    ('the quick brown fox jumped over the lazy dog'),
    ('brown fox is a quick animal'),
    ('quick brown fox is a common phrase'),
    ('fox brown quick reversed order'),
    ('completely unrelated content here');

CREATE INDEX ph_basic_idx ON ph_basic
    USING bm25 (content) WITH (text_config = 'english');

-- Phrase "quick brown fox" — docs 1 and 3 have it adjacent
SELECT id FROM ph_basic
ORDER BY content <@> to_bm25query('"quick brown fox"', 'ph_basic_idx')
LIMIT 10;

DROP TABLE ph_basic;

-- =============================================================
-- Test 2: Non-adjacent terms should NOT match phrase query
-- =============================================================
CREATE TABLE ph_nonadj (id serial PRIMARY KEY, content text);

INSERT INTO ph_nonadj (content) VALUES
    ('full text search is powerful'),
    ('full and text and search separated'),
    ('text full search wrong order');

CREATE INDEX ph_nonadj_idx ON ph_nonadj
    USING bm25 (content) WITH (text_config = 'english');

-- Only doc 1 has "full text search" adjacent
SELECT id FROM ph_nonadj
ORDER BY content <@> to_bm25query('"full text search"', 'ph_nonadj_idx')
LIMIT 10;

DROP TABLE ph_nonadj;

-- =============================================================
-- Test 3: Single-word "phrase" falls back to normal BM25
-- =============================================================
CREATE TABLE ph_single (id serial PRIMARY KEY, content text);

INSERT INTO ph_single (content) VALUES
    ('database performance tuning'),
    ('database indexing strategies');

CREATE INDEX ph_single_idx ON ph_single
    USING bm25 (content) WITH (text_config = 'english');

-- Single word in quotes — should still return results
SELECT count(*) FROM (
    SELECT id FROM ph_single
    ORDER BY content <@> to_bm25query('"database"', 'ph_single_idx')
    LIMIT 10
) q;

DROP TABLE ph_single;

-- =============================================================
-- Test 4: Phrase with repeated terms
-- =============================================================
CREATE TABLE ph_repeat (id serial PRIMARY KEY, content text);

INSERT INTO ph_repeat (content) VALUES
    ('the the the repeated words'),
    ('no repetition here at all');

CREATE INDEX ph_repeat_idx ON ph_repeat
    USING bm25 (content) WITH (text_config = 'english');

-- "the the" should match doc 1
SELECT id FROM ph_repeat
ORDER BY content <@> to_bm25query('"the the"', 'ph_repeat_idx')
LIMIT 10;

DROP TABLE ph_repeat;

-- =============================================================
-- Test 5: Phrase query survives VACUUM (alive bitset compat)
-- =============================================================
CREATE TABLE ph_vacuum (id serial PRIMARY KEY, content text);

INSERT INTO ph_vacuum (content)
SELECT 'full text search document ' || i
FROM generate_series(1, 100) i;

CREATE INDEX ph_vacuum_idx ON ph_vacuum
    USING bm25 (content) WITH (text_config = 'english');

-- Delete half the docs
DELETE FROM ph_vacuum WHERE id <= 50;
VACUUM ph_vacuum;

-- Phrase query should return only live docs
SELECT count(*) FROM (
    SELECT id FROM ph_vacuum
    ORDER BY content <@> to_bm25query('"text search"', 'ph_vacuum_idx')
    LIMIT 1000
) q;

DROP TABLE ph_vacuum;

-- =============================================================
-- Test 6: Phrase query after force merge
-- =============================================================
CREATE TABLE ph_merge (id serial PRIMARY KEY, content text);

INSERT INTO ph_merge (content)
SELECT 'machine learning model ' || i
FROM generate_series(1, 50) i;

CREATE INDEX ph_merge_idx ON ph_merge
    USING bm25 (content) WITH (text_config = 'english');

SELECT bm25_spill_index('ph_merge_idx');

INSERT INTO ph_merge (content)
SELECT 'machine learning inference ' || i
FROM generate_series(1, 50) i;

SELECT bm25_spill_index('ph_merge_idx');
SELECT bm25_force_merge('ph_merge_idx');

-- "machine learning" appears in all 100 docs
SELECT count(*) FROM (
    SELECT id FROM ph_merge
    ORDER BY content <@> to_bm25query('"machine learning"', 'ph_merge_idx')
    LIMIT 1000
) q;

DROP TABLE ph_merge;

-- =============================================================
-- Test 7: Mixed phrase and non-phrase queries on same index
-- =============================================================
CREATE TABLE ph_mixed (id serial PRIMARY KEY, content text);

INSERT INTO ph_mixed (content) VALUES
    ('deep learning neural network training'),
    ('neural network architecture design'),
    ('network deep infrastructure planning');

CREATE INDEX ph_mixed_idx ON ph_mixed
    USING bm25 (content) WITH (text_config = 'english');

-- Phrase query: "neural network" adjacent
SELECT id FROM ph_mixed
ORDER BY content <@> to_bm25query('"neural network"', 'ph_mixed_idx')
LIMIT 10;

-- Non-phrase: neural network (any order, any distance)
SELECT count(*) FROM (
    SELECT id FROM ph_mixed
    ORDER BY content <@> to_bm25query('neural network', 'ph_mixed_idx')
    LIMIT 10
) q;

DROP TABLE ph_mixed;

-- =============================================================
-- Test 8: Large document with many repeated phrases
-- =============================================================
CREATE TABLE ph_large (id serial PRIMARY KEY, content text);

INSERT INTO ph_large (content) VALUES
    (repeat('the quick brown fox jumped over the lazy dog ', 100));

CREATE INDEX ph_large_idx ON ph_large
    USING bm25 (content) WITH (text_config = 'english');

-- "quick brown" appears 100 times
SELECT count(*) FROM (
    SELECT id FROM ph_large
    ORDER BY content <@> to_bm25query('"quick brown"', 'ph_large_idx')
    LIMIT 10
) q;

DROP TABLE ph_large;

-- =============================================================
-- Test 9: Phrase with stemming interaction
-- =============================================================
CREATE TABLE ph_stem (id serial PRIMARY KEY, content text);

INSERT INTO ph_stem (content) VALUES
    ('running dogs are jumping higher'),
    ('the runner dog jumps high'),
    ('dogs running and jumping everywhere');

CREATE INDEX ph_stem_idx ON ph_stem
    USING bm25 (content) WITH (text_config = 'english');

-- With english stemming: "running" -> "run", "dogs" -> "dog"
SELECT id FROM ph_stem
ORDER BY content <@> to_bm25query('"running dogs"', 'ph_stem_idx')
LIMIT 10;

DROP TABLE ph_stem;

-- =============================================================
-- Test 10: Multi-segment phrase query (segments never merged)
-- =============================================================
CREATE TABLE ph_multiseg (id serial PRIMARY KEY, content text);

INSERT INTO ph_multiseg (content)
SELECT 'information retrieval system ' || i
FROM generate_series(1, 40) i;

CREATE INDEX ph_multiseg_idx ON ph_multiseg
    USING bm25 (content) WITH (text_config = 'english');

SELECT bm25_spill_index('ph_multiseg_idx');

INSERT INTO ph_multiseg (content)
SELECT 'information retrieval engine ' || i
FROM generate_series(1, 40) i;

SELECT bm25_spill_index('ph_multiseg_idx');

-- "information retrieval" in all 80 docs across 2 segments
SELECT count(*) FROM (
    SELECT id FROM ph_multiseg
    ORDER BY content <@> to_bm25query('"information retrieval"',
                                       'ph_multiseg_idx')
    LIMIT 1000
) q;

DROP TABLE ph_multiseg;

-- =============================================================
-- Test 11: Empty phrase and edge cases
-- =============================================================
CREATE TABLE ph_edge (id serial PRIMARY KEY, content text);

INSERT INTO ph_edge (content) VALUES
    ('single word here'),
    ('two words here'),
    ('');

CREATE INDEX ph_edge_idx ON ph_edge
    USING bm25 (content) WITH (text_config = 'english');

-- Non-phrase query still works
SELECT count(*) FROM (
    SELECT id FROM ph_edge
    ORDER BY content <@> to_bm25query('word', 'ph_edge_idx')
    LIMIT 10
) q;

DROP TABLE ph_edge;

DROP EXTENSION pg_textsearch;
