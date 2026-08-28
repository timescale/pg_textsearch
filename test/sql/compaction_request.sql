CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\pset format unaligned
SET client_min_messages = warning;
SET pg_textsearch.segments_per_level = 2;
SET pg_textsearch.compaction_mode = 'background';

CREATE TABLE compaction_log (idx regclass);

CREATE FUNCTION log_compaction(idx regclass) RETURNS void
LANGUAGE sql AS $$
    INSERT INTO compaction_log VALUES (idx);
$$;

CREATE FUNCTION fail_compaction(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'intentional request failure for %', idx;
END;
$$;

-- COMMIT disables statement_timeout before PRE_COMMIT callbacks, so
-- self-cancel provides a deterministic query-cancellation error.
CREATE FUNCTION slow_compaction(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_catalog.pg_cancel_backend(pg_catalog.pg_backend_pid());
    PERFORM pg_catalog.pg_sleep(1);
END;
$$;

CREATE TABLE compaction_request_docs (
    id serial PRIMARY KEY,
    body text
);
CREATE INDEX compaction_request_docs_idx ON compaction_request_docs
    USING bm25(body) WITH (text_config = 'english');

CREATE TABLE compaction_request_docs2 (
    id serial PRIMARY KEY,
    body text
);
CREATE INDEX compaction_request_docs2_idx ON compaction_request_docs2
    USING bm25(body) WITH (text_config = 'english');

SET pg_textsearch.compaction_request_function = 'log_compaction';

-- Seed each index below threshold so later spills exercise dispatch.
INSERT INTO compaction_request_docs (body)
SELECT 'seed doc ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS request_seed_spill;
INSERT INTO compaction_request_docs2 (body)
SELECT 'seed doc ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs2_idx') IS NOT NULL
       AS request_seed_spill2;
TRUNCATE compaction_log;

-- Explicit rollback does not reach PRE_COMMIT dispatch.
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'rollback doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS rollback_spill;
ROLLBACK;
SELECT count(*) AS rollback_log_rows FROM compaction_log;

-- Commit reaches PRE_COMMIT and dispatches one request.
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'commit doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS commit_spill;
COMMIT;
SELECT count(*) AS commit_log_rows
FROM compaction_log
WHERE idx = 'compaction_request_docs_idx'::regclass;

-- Two spills for the same index in one transaction are deduplicated.
TRUNCATE compaction_log;
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'dedup first doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS dedup_spill1;
INSERT INTO compaction_request_docs (body)
SELECT 'dedup second doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS dedup_spill2;
COMMIT;
SELECT count(*) AS dedup_log_rows
FROM compaction_log
WHERE idx = 'compaction_request_docs_idx'::regclass;

-- Different indexes in one transaction each dispatch once.
TRUNCATE compaction_log;
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'multi first doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS multi_spill1;
INSERT INTO compaction_request_docs2 (body)
SELECT 'multi second doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs2_idx') IS NOT NULL
       AS multi_spill2;
COMMIT;
SELECT idx::text, count(*) AS log_rows
FROM compaction_log
GROUP BY idx
ORDER BY idx::text;

-- Missing request function warns once and writer data still commits.
TRUNCATE compaction_log;
SET pg_textsearch.compaction_request_function = 'missing_compaction_fn';
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'missing function doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS missing_function_spill;
COMMIT;
SELECT count(*) AS missing_function_log_rows FROM compaction_log;
SELECT count(*) >= 100 AS missing_function_committed
FROM compaction_request_docs;

-- Empty request function warns once and writer data still commits.
TRUNCATE compaction_log;
SET pg_textsearch.compaction_request_function = '';
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'empty function doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS empty_function_spill;
COMMIT;
SELECT count(*) AS empty_function_log_rows FROM compaction_log;
SELECT count(*) >= 120 AS empty_function_committed
FROM compaction_request_docs;

-- Inline mode does not enqueue background requests.
TRUNCATE compaction_log;
SET pg_textsearch.compaction_request_function = 'log_compaction';
SET pg_textsearch.compaction_mode = 'inline';
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'inline doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS inline_spill;
COMMIT;
SELECT count(*) AS inline_log_rows FROM compaction_log;

-- A raising request function is isolated; writer data still commits.
SET pg_textsearch.compaction_mode = 'background';
SET pg_textsearch.compaction_request_function = 'fail_compaction';
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'raising function doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS raising_function_spill;
COMMIT;
SELECT count(*) >= 160 AS raising_function_committed
FROM compaction_request_docs;

-- Query cancellation during dispatch aborts the writer transaction.
CREATE TABLE compaction_cancel_docs (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_cancel_docs_idx ON compaction_cancel_docs
    USING bm25(body) WITH (text_config = 'english');

SET pg_textsearch.segments_per_level = 2;
SET pg_textsearch.compaction_request_function = 'slow_compaction';

-- Seed one physical segment so the transaction under test reaches threshold.
INSERT INTO compaction_cancel_docs (body) VALUES ('seed request');
SELECT bm25_spill_index('compaction_cancel_docs_idx') IS NOT NULL
       AS cancel_seed_spill;
DELETE FROM compaction_cancel_docs;

BEGIN;
INSERT INTO compaction_cancel_docs (body) VALUES ('cancel request');
SELECT bm25_spill_index('compaction_cancel_docs_idx') IS NOT NULL
       AS cancel_spill;
\set VERBOSITY terse
COMMIT;
\set VERBOSITY default
SELECT count(*) AS canceled_rows FROM compaction_cancel_docs;

-- Background requests start only when a level reaches its threshold.
SET pg_textsearch.compaction_request_function = 'log_compaction';
SET pg_textsearch.segments_per_level = 2;
TRUNCATE compaction_log;

CREATE TABLE compaction_threshold_docs (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_threshold_docs_idx ON compaction_threshold_docs
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO compaction_threshold_docs (body)
SELECT 'threshold first ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_threshold_docs_idx') IS NOT NULL;
SELECT count(*) AS below_threshold_requests FROM compaction_log;

INSERT INTO compaction_threshold_docs (body)
SELECT 'threshold second ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_threshold_docs_idx') IS NOT NULL;
SELECT count(*) AS threshold_requests FROM compaction_log
WHERE idx = 'compaction_threshold_docs_idx'::regclass;

-- Dropping an index before PRE_COMMIT removes its pending request.
TRUNCATE compaction_log;
CREATE TABLE compaction_drop_docs (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_drop_docs_idx ON compaction_drop_docs
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO compaction_drop_docs (body)
SELECT 'drop seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_drop_docs_idx') IS NOT NULL;

BEGIN;
INSERT INTO compaction_drop_docs (body)
SELECT 'drop pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_drop_docs_idx') IS NOT NULL;
DROP INDEX compaction_drop_docs_idx;
COMMIT;

SELECT count(*) AS dropped_index_requests FROM compaction_log;
DROP TABLE compaction_drop_docs;

-- Temporary indexes must compact inline because workers cannot access them.
TRUNCATE compaction_log;
CREATE TEMP TABLE compaction_temp_docs (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_temp_docs_idx ON compaction_temp_docs
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO compaction_temp_docs (body)
SELECT 'temp first ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_temp_docs_idx') IS NOT NULL;
INSERT INTO compaction_temp_docs (body)
SELECT 'temp second ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_temp_docs_idx') IS NOT NULL;

SELECT count(*) AS temp_background_requests FROM compaction_log;
SELECT NOT bm25_needs_compaction('compaction_temp_docs_idx'::regclass)
       AS temp_compacted_inline;

RESET pg_textsearch.compaction_mode;
RESET pg_textsearch.compaction_request_function;
DROP TABLE compaction_cancel_docs CASCADE;
DROP TABLE compaction_threshold_docs CASCADE;
DROP TABLE compaction_request_docs2 CASCADE;
DROP TABLE compaction_request_docs CASCADE;
DROP TABLE compaction_log;
DROP EXTENSION pg_textsearch CASCADE;
