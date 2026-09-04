CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\pset format unaligned
SET client_min_messages = warning;
SET pg_textsearch.segments_per_level = 2;

SHOW pg_textsearch.background_compaction_schedule;

-- The compaction policy and schedule are per-index options.
CREATE TABLE relopt_docs (id serial PRIMARY KEY, body text);
CREATE INDEX relopt_bad_idx ON relopt_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');
CREATE INDEX relopt_docs_idx ON relopt_docs
    USING bm25(body) WITH (text_config = 'english');
ALTER INDEX relopt_docs_idx SET (compaction = 'manual');
SELECT reloptions @> ARRAY['compaction=manual']
FROM pg_class WHERE oid = 'relopt_docs_idx'::regclass;

ALTER INDEX relopt_docs_idx
    SET (compaction_schedule = '17 * * * *');
SELECT reloptions @> ARRAY['compaction_schedule=17 * * * *']
FROM pg_class WHERE oid = 'relopt_docs_idx'::regclass;

-- Managed background mode requires pg_durable, but temporary indexes are
-- rejected before admission is attempted.
CREATE INDEX relopt_background_idx ON relopt_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE TEMP TABLE relopt_temp_docs (id integer, body text);
CREATE INDEX relopt_temp_background_idx ON relopt_temp_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');

-- Manual mode leaves spill-time compaction debt in place.
CREATE TABLE manual_docs (id serial PRIMARY KEY, body text);
CREATE INDEX manual_docs_idx ON manual_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
INSERT INTO manual_docs (body)
SELECT 'manual one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('manual_docs_idx') IS NOT NULL;
INSERT INTO manual_docs (body)
SELECT 'manual two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('manual_docs_idx') IS NOT NULL;
SELECT bm25_needs_compaction('manual_docs_idx'::regclass)
       AS manual_debt_remains;

-- The final serial build batch gets the same inline policy as
-- intermediate batches.
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '1MB';
CREATE TABLE final_build_docs (
    id integer PRIMARY KEY,
    body text
);
INSERT INTO final_build_docs
SELECT i, 'token' || i || ' ' || repeat(md5(i::text) || ' ', 8)
FROM generate_series(1, 10000) i;
CREATE INDEX final_build_docs_idx ON final_build_docs
    USING bm25(body) WITH (text_config = 'simple');
SELECT NOT bm25_needs_compaction('final_build_docs_idx'::regclass)
       AS final_build_batch_compacted;

-- CREATE INDEX honors manual mode, leaving its build batches uncompacted.
CREATE TABLE manual_build_docs (
    id integer PRIMARY KEY,
    body text
);
INSERT INTO manual_build_docs
SELECT i, 'token' || i || ' ' || repeat(md5(i::text) || ' ', 8)
FROM generate_series(1, 10000) i;
CREATE INDEX manual_build_docs_idx ON manual_build_docs
    USING bm25(body) WITH (text_config = 'simple', compaction = 'manual');
SELECT bm25_needs_compaction('manual_build_docs_idx'::regclass)
       AS manual_build_debt_remains;
RESET maintenance_work_mem;
RESET max_parallel_maintenance_workers;

-- Inline mode compacts synchronously.
CREATE TABLE inline_docs (id serial PRIMARY KEY, body text);
CREATE INDEX inline_docs_idx ON inline_docs
    USING bm25(body) WITH (text_config = 'english', compaction = 'inline');
INSERT INTO inline_docs (body)
SELECT 'inline one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('inline_docs_idx') IS NOT NULL;
INSERT INTO inline_docs (body)
SELECT 'inline two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('inline_docs_idx') IS NOT NULL;
SELECT NOT bm25_needs_compaction('inline_docs_idx'::regclass)
       AS inline_compacted;

RESET pg_textsearch.segments_per_level;
RESET client_min_messages;
DROP TABLE inline_docs CASCADE;
DROP TABLE manual_build_docs CASCADE;
DROP TABLE final_build_docs CASCADE;
DROP TABLE manual_docs CASCADE;
DROP TABLE relopt_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
