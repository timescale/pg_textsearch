\pset format unaligned
SET client_min_messages = warning;
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION injection_points;
CREATE EXTENSION pg_textsearch_test;
SET enable_seqscan = off;

-- Legacy token totals are rebuilt from heap data.
CREATE TABLE vacuum_legacy_stats (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_legacy_stats_idx
    ON vacuum_legacy_stats USING bm25(content)
    WITH (text_config = 'english', compaction = 'off');
INSERT INTO vacuum_legacy_stats (content)
SELECT 'legacy inflated token document ' || i
FROM generate_series(1, 10) AS i;
SELECT pg_textsearch_test_attach_legacy_segment(1000000);
SELECT bm25_spill_index('vacuum_legacy_stats_idx') > 0
    AS legacy_header_inflated;
SELECT injection_points_detach('pg-textsearch-legacy-segment');

DELETE FROM vacuum_legacy_stats WHERE id <= 5;
VACUUM vacuum_legacy_stats;

SELECT bm25_level_counts('vacuum_legacy_stats_idx'::regclass)
    AS levels_after_legacy_vacuum;
SELECT bm25_summarize_index('vacuum_legacy_stats_idx')
           ~ E'total_docs: 5\n'
       AND bm25_summarize_index('vacuum_legacy_stats_idx')
           ~ E'total_len: 25\n'
    AS legacy_totals_rebuilt;
SELECT bm25_dump_index('vacuum_legacy_stats_idx')
           LIKE '%Alive: 5 / 5 docs%'
    AS legacy_replacement_published;
DROP TABLE vacuum_legacy_stats;

-- Legacy replacement rebases a changed text configuration.
CREATE TEXT SEARCH CONFIGURATION public.vacuum_mutable_cfg (COPY = english);
CREATE TABLE vacuum_mutable_config (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_mutable_config_idx
    ON vacuum_mutable_config USING bm25(content)
    WITH (text_config = 'public.vacuum_mutable_cfg', compaction = 'off');
INSERT INTO vacuum_mutable_config (content)
VALUES ('the the the alpha'), ('the the the alpha');
SELECT pg_textsearch_test_attach_legacy_segment(2);
SELECT bm25_spill_index('vacuum_mutable_config_idx') > 0
    AS mutable_legacy_created;
SELECT injection_points_detach('pg-textsearch-legacy-segment');

ALTER TEXT SEARCH CONFIGURATION public.vacuum_mutable_cfg
    ALTER MAPPING FOR asciiword WITH simple;
DELETE FROM vacuum_mutable_config WHERE id = 1;
VACUUM vacuum_mutable_config;

SELECT bm25_summarize_index('vacuum_mutable_config_idx')
           ~ E'total_docs: 1\n'
       AND bm25_summarize_index('vacuum_mutable_config_idx')
           ~ E'total_len: 4\n'
    AS mutable_config_totals_rebased;
DROP TABLE vacuum_mutable_config;
DROP TEXT SEARCH CONFIGURATION public.vacuum_mutable_cfg;

-- Inconsistent multi-segment legacy totals are fully rebased.
CREATE TABLE vacuum_multi_legacy (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_multi_legacy_idx
    ON vacuum_multi_legacy USING bm25(content)
    WITH (text_config = 'english', compaction = 'off');
INSERT INTO vacuum_multi_legacy (content)
SELECT 'multi legacy token document ' || i
FROM generate_series(1, 5) AS i;
SELECT pg_textsearch_test_attach_legacy_segment(1000000);
SELECT bm25_spill_index('vacuum_multi_legacy_idx') > 0
    AS first_legacy_created;
SELECT injection_points_detach('pg-textsearch-legacy-segment');

INSERT INTO vacuum_multi_legacy (content)
SELECT 'multi legacy token document ' || i
FROM generate_series(6, 10) AS i;
SELECT pg_textsearch_test_attach_legacy_segment(2000000);
SELECT bm25_spill_index('vacuum_multi_legacy_idx') > 0
    AS second_legacy_created;
SELECT injection_points_detach('pg-textsearch-legacy-segment');
SELECT pg_textsearch_test_attach_vacuum_total_len(4000000);

DELETE FROM vacuum_multi_legacy WHERE id = 1;
VACUUM vacuum_multi_legacy;
SELECT injection_points_detach('pg-textsearch-vacuum-total-len');

SELECT bm25_summarize_index('vacuum_multi_legacy_idx')
           ~ E'total_docs: 9\n'
       AND bm25_summarize_index('vacuum_multi_legacy_idx')
           ~ E'total_len: 45\n'
    AS multi_legacy_totals_rebased;
SELECT bm25_dump_index('vacuum_multi_legacy_idx')
           NOT LIKE '%Version: 4%'
    AS all_legacy_segments_rebuilt;
DROP TABLE vacuum_multi_legacy;

-- Impossible current-format totals fail closed.
CREATE TABLE vacuum_current_corrupt (
    id serial PRIMARY KEY,
    content text
);
INSERT INTO vacuum_current_corrupt (content)
VALUES ('current token document one'), ('current token document two');
CREATE INDEX vacuum_current_corrupt_idx
    ON vacuum_current_corrupt USING bm25(content)
    WITH (text_config = 'english');

SELECT pg_textsearch_test_attach_vacuum_total_len(0);
DELETE FROM vacuum_current_corrupt WHERE id = 1;
VACUUM vacuum_current_corrupt;
SELECT injection_points_detach('pg-textsearch-vacuum-total-len');
DROP TABLE vacuum_current_corrupt;

CREATE TABLE vacuum_current_inflated (
    id serial PRIMARY KEY,
    content text
);
INSERT INTO vacuum_current_inflated (content)
VALUES ('inflated token document one'), ('inflated token document two');
CREATE INDEX vacuum_current_inflated_idx
    ON vacuum_current_inflated USING bm25(content)
    WITH (text_config = 'english');

SELECT pg_textsearch_test_attach_vacuum_total_len(1000);
VACUUM vacuum_current_inflated;
SELECT injection_points_detach('pg-textsearch-vacuum-total-len');
DROP TABLE vacuum_current_inflated;

CREATE TABLE vacuum_mixed_no_dead (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_mixed_no_dead_idx
    ON vacuum_mixed_no_dead USING bm25(content)
    WITH (text_config = 'english', compaction = 'off');
INSERT INTO vacuum_mixed_no_dead (content)
VALUES ('mixed legacy document one'), ('mixed legacy document two');
SELECT pg_textsearch_test_attach_legacy_segment(1000);
SELECT bm25_spill_index('vacuum_mixed_no_dead_idx') > 0
    AS no_dead_legacy_created;
SELECT injection_points_detach('pg-textsearch-legacy-segment');

INSERT INTO vacuum_mixed_no_dead (content)
VALUES ('mixed current document three'), ('mixed current document four');
SELECT bm25_spill_index('vacuum_mixed_no_dead_idx') > 0
    AS no_dead_current_spilled;
SELECT pg_textsearch_test_attach_vacuum_total_len(100);
VACUUM vacuum_mixed_no_dead;
SELECT injection_points_detach('pg-textsearch-vacuum-total-len');

SELECT bm25_summarize_index('vacuum_mixed_no_dead_idx')
           ~ E'total_docs: 4\n'
       AND bm25_summarize_index('vacuum_mixed_no_dead_idx')
           ~ E'total_len: 16\n'
    AS no_dead_mixed_totals_rebased;
SELECT bm25_dump_index('vacuum_mixed_no_dead_idx')
           NOT LIKE '%Version: 4%'
    AS no_dead_legacy_rebuilt;
DROP TABLE vacuum_mixed_no_dead;

-- Spill cannot compact corrupt legacy totals before VACUUM repairs them.
CREATE TABLE vacuum_spill_legacy (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_spill_legacy_idx
    ON vacuum_spill_legacy USING bm25(content)
    WITH (text_config = 'english', compaction = 'off');

DO $$
BEGIN
    FOR batch IN 1..4 LOOP
        INSERT INTO vacuum_spill_legacy (content)
        SELECT 'alpha beta gamma delta'
        FROM generate_series(1, 10);
        PERFORM pg_textsearch_test_attach_legacy_segment(batch * 1000);
        PERFORM bm25_spill_index('vacuum_spill_legacy_idx');
        PERFORM injection_points_detach('pg-textsearch-legacy-segment');
    END LOOP;
END
$$;

SELECT pg_textsearch_test_attach_vacuum_total_len(5000);
ALTER INDEX vacuum_spill_legacy_idx SET (compaction = 'inline');
SET pg_textsearch.segments_per_level = 2;
INSERT INTO vacuum_spill_legacy (content)
VALUES ('alpha beta gamma delta');
DELETE FROM vacuum_spill_legacy WHERE id = 1;
VACUUM vacuum_spill_legacy;
SELECT injection_points_detach('pg-textsearch-vacuum-total-len');

SELECT bm25_summarize_index('vacuum_spill_legacy_idx')
           ~ E'total_docs: 40\n'
       AND bm25_summarize_index('vacuum_spill_legacy_idx')
           ~ E'total_len: 160\n'
    AS spill_legacy_totals_rebased;
SELECT bm25_dump_index('vacuum_spill_legacy_idx')
           NOT LIKE '%Version: 4%'
    AS spill_legacy_segments_rebuilt;
SELECT (bm25_level_counts(
            'vacuum_spill_legacy_idx'::regclass))[2] > 0
    AS deferred_inline_compaction_ran;

RESET pg_textsearch.segments_per_level;
DROP TABLE vacuum_spill_legacy;

RESET enable_seqscan;
DROP EXTENSION pg_textsearch_test;
DROP EXTENSION injection_points;
DROP EXTENSION pg_textsearch;
\pset format aligned
