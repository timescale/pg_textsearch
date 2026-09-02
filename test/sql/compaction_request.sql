CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\pset format unaligned
SET client_min_messages = warning;
SET pg_textsearch.segments_per_level = 2;

-- The two-phase commit coverage below is meaningless without this.
SELECT current_setting('max_prepared_transactions')::int > 0
       AS twophase_enabled;

CREATE SEQUENCE compaction_request_calls;
SELECT setval('compaction_request_calls', 1, true) AS calls_before \gset
CREATE SEQUENCE compaction_request_calls_docs2;
-- Mark as called so the first nextval advances last_value, matching
-- compaction_request_calls; otherwise the first call leaves it at 1.
SELECT setval('compaction_request_calls_docs2', 1, true) AS docs2_init \gset
CREATE TABLE compaction_callback_rows (idx regclass);

CREATE FUNCTION public.record_compaction_request(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM nextval('compaction_request_calls');
    /*
     * Sequence advances are non-transactional, so they survive the
     * subtransaction rollback that discards the callback's other work.
     * A second counter for one specific index is therefore the only way
     * to observe which index the callback was actually handed.
     */
    IF idx::text LIKE '%request_docs2_idx' THEN
        PERFORM nextval('compaction_request_calls_docs2');
    END IF;
    INSERT INTO compaction_callback_rows VALUES (idx);
END;
$$;

CREATE FUNCTION public.fail_compaction_request(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'intentional request failure for %', idx;
END;
$$;

CREATE FUNCTION public.cancel_compaction_request(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_catalog.pg_cancel_backend(pg_catalog.pg_backend_pid());
    PERFORM pg_catalog.pg_sleep(1);
END;
$$;

-- The callback GUC requires a schema-qualified name.
SET pg_textsearch.compaction_request_function = 'record_compaction_request';
SET pg_textsearch.compaction_request_function = 'invalid..callback';
SET pg_textsearch.compaction_request_function = 'db.schema.callback';
SET pg_textsearch.compaction_request_function = '""';
SET pg_textsearch.compaction_request_function = 'schema.""';
SET pg_textsearch.compaction_request_function = '"".callback';
-- A qualified name passes; existence is checked at dispatch, not here.
SET pg_textsearch.compaction_request_function = 'public.missing_callback';
SET pg_textsearch.compaction_request_function =
    'public.record_compaction_request';

-- The compaction policy is a per-index option defaulting to inline.
CREATE TABLE relopt_docs (id serial PRIMARY KEY, body text);
CREATE INDEX relopt_docs_idx ON relopt_docs
    USING bm25(body) WITH (text_config = 'english', compaction = 'bogus');
CREATE INDEX relopt_docs_idx ON relopt_docs
    USING bm25(body) WITH (text_config = 'english');
SELECT reloptions IS NULL OR NOT (reloptions::text LIKE '%compaction%')
       AS compaction_unset_by_default
FROM pg_class WHERE relname = 'relopt_docs_idx';
ALTER INDEX relopt_docs_idx SET (compaction = 'background');
SELECT reloptions::text LIKE '%compaction=background%' AS compaction_altered
FROM pg_class WHERE relname = 'relopt_docs_idx';

CREATE TABLE request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX request_docs_idx ON request_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE TABLE request_docs2 (id serial PRIMARY KEY, body text);
CREATE INDEX request_docs2_idx ON request_docs2
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');

-- Seeding stays below the threshold, so it must dispatch nothing.
-- Without this, every later delta would still hold if the code
-- dispatched on every spill rather than on real debt.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'seed one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS seed_one;
INSERT INTO request_docs2 (body)
SELECT 'seed two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs2_idx') IS NOT NULL AS seed_two;
COMMIT;
SELECT last_value - :calls_before AS below_threshold_requests
FROM compaction_request_calls;
SELECT NOT bm25_needs_compaction('request_docs_idx'::regclass)
       AS seed_below_threshold;

-- Top-level abort discards the pending request.  The interposed
-- transaction flushes any leaked request before the delta is read.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'abort ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS abort_spill;
ROLLBACK;
BEGIN;
SELECT 1 AS flush_any_leaked_request;
COMMIT;
SELECT last_value - :calls_before AS abort_requests
FROM compaction_request_calls;

-- Commit dispatches once and callback-local writes are rolled back.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'commit ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS commit_spill;
COMMIT;
SELECT last_value - :calls_before AS commit_requests
FROM compaction_request_calls;
SELECT count(*) AS callback_local_rows FROM compaction_callback_rows;

-- Repeated requests for one index are deduplicated.  The segment delta
-- proves both spills reached the request path.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
SELECT (SELECT sum(c) FROM unnest(bm25_level_counts('request_docs_idx')) c)
    AS segs_before \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'dedup one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS dedup_one;
INSERT INTO request_docs (body)
SELECT 'dedup two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS dedup_two;
COMMIT;
SELECT (SELECT sum(c) FROM unnest(bm25_level_counts('request_docs_idx')) c)
    - :segs_before AS dedup_segments_added;
SELECT last_value - :calls_before AS deduplicated_requests
FROM compaction_request_calls;

-- Different indexes each dispatch once.  The per-index counter proves
-- the second index was dispatched, not the first one twice.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
SELECT last_value AS docs2_before FROM compaction_request_calls_docs2 \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'multi one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS multi_one;
INSERT INTO request_docs2 (body)
SELECT 'multi two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs2_idx') IS NOT NULL AS multi_two;
COMMIT;
SELECT last_value - :calls_before AS multiple_index_requests
FROM compaction_request_calls;
SELECT last_value - :docs2_before AS multiple_index_docs2_requests
FROM compaction_request_calls_docs2;

-- The callback is resolved as written, not through the committing
-- backend's search_path.
CREATE SCHEMA request_shadow;
CREATE SEQUENCE compaction_request_calls_shadow;
SELECT setval('compaction_request_calls_shadow', 1, true)
    AS shadow_init \gset
CREATE FUNCTION request_shadow.record_compaction_request(idx regclass)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    PERFORM nextval('compaction_request_calls_shadow');
END;
$$;
SET search_path = request_shadow, public;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
SELECT last_value AS shadow_before
FROM compaction_request_calls_shadow \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'shadowed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS shadow_spill;
COMMIT;
SELECT last_value - :calls_before AS shadowed_trusted_requests
FROM compaction_request_calls;
SELECT last_value - :shadow_before AS shadowed_attacker_requests
FROM compaction_request_calls_shadow;
RESET search_path;

-- A savepoint rollback retains physical compaction debt and its request.
CREATE TABLE savepoint_docs (id serial PRIMARY KEY, body text);
CREATE INDEX savepoint_docs_idx ON savepoint_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
INSERT INTO savepoint_docs (body)
SELECT 'savepoint seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('savepoint_docs_idx') IS NOT NULL;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
SAVEPOINT request_savepoint;
INSERT INTO savepoint_docs (body)
SELECT 'savepoint rollback ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('savepoint_docs_idx') IS NOT NULL;
ROLLBACK TO request_savepoint;
COMMIT;
SELECT last_value - :calls_before AS savepoint_requests
FROM compaction_request_calls;

-- Rolling back a DROP restores an already-pending request.
CREATE TABLE rollback_drop_docs (id serial PRIMARY KEY, body text);
CREATE INDEX rollback_drop_docs_idx ON rollback_drop_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
INSERT INTO rollback_drop_docs (body)
SELECT 'rollback drop seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('rollback_drop_docs_idx') IS NOT NULL;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO rollback_drop_docs (body)
SELECT 'rollback drop pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('rollback_drop_docs_idx') IS NOT NULL;
SAVEPOINT drop_request;
DROP INDEX rollback_drop_docs_idx;
ROLLBACK TO drop_request;
COMMIT;
SELECT last_value - :calls_before AS rollback_drop_requests
FROM compaction_request_calls;

-- An index and request created in an aborted savepoint are not
-- dispatched.  The recording callback stays configured, so a zero delta
-- means the request was discarded, not that the counter was unreachable.
CREATE TABLE aborted_index_docs (id serial PRIMARY KEY, body text);
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
SAVEPOINT create_request;
CREATE INDEX aborted_index_docs_idx ON aborted_index_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
INSERT INTO aborted_index_docs (body)
SELECT 'aborted index one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('aborted_index_docs_idx') IS NOT NULL;
INSERT INTO aborted_index_docs (body)
SELECT 'aborted index two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('aborted_index_docs_idx') IS NOT NULL;
ROLLBACK TO create_request;
COMMIT;
SELECT last_value - :calls_before AS aborted_index_requests
FROM compaction_request_calls;
SELECT to_regclass('aborted_index_docs_idx') IS NULL
       AS aborted_index_absent;

-- A dropped index is removed from the pending set.
CREATE TABLE dropped_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX dropped_request_docs_idx ON dropped_request_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
INSERT INTO dropped_request_docs (body)
SELECT 'drop seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('dropped_request_docs_idx') IS NOT NULL;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO dropped_request_docs (body)
SELECT 'drop pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('dropped_request_docs_idx') IS NOT NULL;
DROP INDEX dropped_request_docs_idx;
COMMIT;
SELECT last_value - :calls_before AS dropped_index_requests
FROM compaction_request_calls;

-- A callback that vanished between SET and dispatch warns, and the
-- writer transaction still commits.  The warning is the assertion.
CREATE FUNCTION public.disappearing_request(regclass) RETURNS void
LANGUAGE sql AS $$ SELECT NULL::void $$;
SET pg_textsearch.compaction_request_function =
    'public.disappearing_request';
DROP FUNCTION public.disappearing_request(regclass);
BEGIN;
INSERT INTO request_docs (body)
SELECT 'disappearing callback ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL;
COMMIT;
SELECT count(*) = 20 AS disappeared_callback_committed
FROM request_docs WHERE body LIKE 'disappearing callback %';

-- The callback is resolved by name at dispatch, not cached as an OID.
-- Renaming the original away and creating a different function under the
-- same name must divert the call; a cached OID would reach the original,
-- which keeps its OID across the rename.
CREATE SEQUENCE compaction_request_calls_replaced;
SELECT setval('compaction_request_calls_replaced', 1, true)
    AS replaced_init \gset
CREATE FUNCTION public.callback_slot(regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM nextval('compaction_request_calls');
END;
$$;
SET pg_textsearch.compaction_request_function = 'public.callback_slot';
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'slot original ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL;
COMMIT;
SELECT last_value - :calls_before AS original_callback_requests
FROM compaction_request_calls;

ALTER FUNCTION public.callback_slot(regclass) RENAME TO callback_slot_old;
CREATE FUNCTION public.callback_slot(regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM nextval('compaction_request_calls_replaced');
END;
$$;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
SELECT last_value AS replaced_before
FROM compaction_request_calls_replaced \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'slot replacement ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL;
COMMIT;
SELECT last_value - :calls_before AS renamed_callback_old_requests
FROM compaction_request_calls;
SELECT last_value - :replaced_before AS renamed_callback_new_requests
FROM compaction_request_calls_replaced;
SET pg_textsearch.compaction_request_function =
    'public.record_compaction_request';

-- Ordinary callback errors warn and preserve the writer transaction.
SET pg_textsearch.compaction_request_function =
    'public.fail_compaction_request';
BEGIN;
INSERT INTO request_docs (body)
SELECT 'callback failure ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL;
COMMIT;
SELECT count(*) = 20 AS callback_failure_committed
FROM request_docs WHERE body LIKE 'callback failure %';

-- A two-phase transaction dispatches nothing; the debt is left for a
-- later spill or the scheduler's sweep.
CREATE TABLE prepared_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX prepared_request_docs_idx ON prepared_request_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
SET pg_textsearch.compaction_request_function =
    'public.record_compaction_request';
INSERT INTO prepared_request_docs (body)
SELECT 'prepare seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('prepared_request_docs_idx') IS NOT NULL;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO prepared_request_docs (body)
SELECT 'prepare pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('prepared_request_docs_idx') IS NOT NULL;
PREPARE TRANSACTION 'compaction_request_prepare';
SELECT last_value - :calls_before AS prepare_requests
FROM compaction_request_calls;
SELECT bm25_needs_compaction('prepared_request_docs_idx'::regclass)
       AS prepare_debt_retained;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
SELECT 1 AS backend_reused;
COMMIT;
SELECT last_value - :calls_before AS stale_requests
FROM compaction_request_calls;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
COMMIT PREPARED 'compaction_request_prepare';
SELECT last_value - :calls_before AS commit_prepared_requests
FROM compaction_request_calls;

-- A callback touching a temporary object would set transaction-global
-- state that PostgreSQL validates after PRE_PREPARE.  Because two-phase
-- transactions do not dispatch, PREPARE still succeeds.
CREATE TEMP TABLE prepare_temp_probe (x int);
CREATE FUNCTION public.temp_touching_request(regclass) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
    SELECT count(*) INTO n FROM prepare_temp_probe;
END;
$$;
SET pg_textsearch.compaction_request_function =
    'public.temp_touching_request';
BEGIN;
INSERT INTO prepared_request_docs (body)
SELECT 'prepare temp ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('prepared_request_docs_idx') IS NOT NULL;
PREPARE TRANSACTION 'compaction_request_prepare_temp';
COMMIT PREPARED 'compaction_request_prepare_temp';
SELECT count(*) = 20 AS prepare_temp_committed
FROM prepared_request_docs WHERE body LIKE 'prepare temp %';
SET pg_textsearch.compaction_request_function =
    'public.record_compaction_request';

-- A spill caused by the callback compacts inline.  Its request would
-- land in a list this dispatch has stopped reading and be freed at
-- commit, while the spill survives the callback's rollback.
CREATE TABLE reentrant_outer (id serial PRIMARY KEY, body text);
CREATE INDEX reentrant_outer_idx ON reentrant_outer
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE TABLE reentrant_inner (id serial PRIMARY KEY, body text);
CREATE INDEX reentrant_inner_idx ON reentrant_inner
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
INSERT INTO reentrant_outer (body)
SELECT 'reentrant outer seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('reentrant_outer_idx') IS NOT NULL;
INSERT INTO reentrant_inner (body)
SELECT 'reentrant inner seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('reentrant_inner_idx') IS NOT NULL;
CREATE FUNCTION public.reentrant_request(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM nextval('compaction_request_calls');
    IF idx::text LIKE '%reentrant_outer_idx' THEN
        INSERT INTO reentrant_inner (body)
        SELECT 'reentrant callback ' || i FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('reentrant_inner_idx');
    END IF;
END;
$$;
SET pg_textsearch.compaction_request_function = 'public.reentrant_request';
BEGIN;
INSERT INTO reentrant_outer (body)
SELECT 'reentrant trigger ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('reentrant_outer_idx') IS NOT NULL;
COMMIT;
SELECT NOT bm25_needs_compaction('reentrant_inner_idx'::regclass)
       AS reentrant_inner_compacted;
SET pg_textsearch.compaction_request_function =
    'public.record_compaction_request';

-- Temporary indexes compact inline even when set to background.
CREATE TEMP TABLE temp_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX temp_request_docs_idx ON temp_request_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
SELECT last_value AS calls_before FROM compaction_request_calls \gset
INSERT INTO temp_request_docs (body)
SELECT 'temp one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('temp_request_docs_idx') IS NOT NULL;
INSERT INTO temp_request_docs (body)
SELECT 'temp two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('temp_request_docs_idx') IS NOT NULL;
SELECT last_value - :calls_before AS temp_requests
FROM compaction_request_calls;
SELECT NOT bm25_needs_compaction('temp_request_docs_idx'::regclass)
       AS temp_compacted_inline;

-- A temporary index set to off keeps its debt: off is off everywhere.
CREATE TEMP TABLE temp_off_docs (id serial PRIMARY KEY, body text);
CREATE INDEX temp_off_docs_idx ON temp_off_docs
    USING bm25(body) WITH (text_config = 'english', compaction = 'off');
INSERT INTO temp_off_docs (body)
SELECT 'temp off one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('temp_off_docs_idx') IS NOT NULL;
INSERT INTO temp_off_docs (body)
SELECT 'temp off two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('temp_off_docs_idx') IS NOT NULL;
SELECT bm25_needs_compaction('temp_off_docs_idx'::regclass)
       AS temp_off_debt_remains;

-- The final serial build batch gets the same policy as intermediate
-- batches.
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

-- CREATE INDEX honors off, leaving its build batches uncompacted.
CREATE TABLE off_build_docs (
    id integer PRIMARY KEY,
    body text
);
INSERT INTO off_build_docs
SELECT i, 'token' || i || ' ' || repeat(md5(i::text) || ' ', 8)
FROM generate_series(1, 10000) i;
CREATE INDEX off_build_docs_idx ON off_build_docs
    USING bm25(body) WITH (text_config = 'simple', compaction = 'off');
SELECT bm25_needs_compaction('off_build_docs_idx'::regclass)
       AS off_build_debt_remains;
RESET maintenance_work_mem;
RESET max_parallel_maintenance_workers;

-- Off leaves debt in place and dispatches nothing.
CREATE TABLE off_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX off_request_docs_idx ON off_request_docs
    USING bm25(body) WITH (text_config = 'english', compaction = 'off');
SELECT last_value AS calls_before FROM compaction_request_calls \gset
INSERT INTO off_request_docs (body)
SELECT 'off one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('off_request_docs_idx') IS NOT NULL;
INSERT INTO off_request_docs (body)
SELECT 'off two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('off_request_docs_idx') IS NOT NULL;
SELECT last_value - :calls_before AS off_requests
FROM compaction_request_calls;
SELECT bm25_needs_compaction('off_request_docs_idx'::regclass)
       AS off_debt_remains;

-- Switching an existing index to background makes it dispatch.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
ALTER INDEX off_request_docs_idx SET (compaction = 'background');
BEGIN;
INSERT INTO off_request_docs (body)
SELECT 'off switched ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('off_request_docs_idx') IS NOT NULL;
COMMIT;
SELECT last_value - :calls_before AS switched_requests
FROM compaction_request_calls;

-- Inline compacts synchronously and dispatches nothing.
CREATE TABLE inline_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX inline_request_docs_idx ON inline_request_docs
    USING bm25(body) WITH (text_config = 'english', compaction = 'inline');
SELECT last_value AS calls_before FROM compaction_request_calls \gset
INSERT INTO inline_request_docs (body)
SELECT 'inline one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('inline_request_docs_idx') IS NOT NULL;
INSERT INTO inline_request_docs (body)
SELECT 'inline two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('inline_request_docs_idx') IS NOT NULL;
SELECT last_value - :calls_before AS inline_requests
FROM compaction_request_calls;
SELECT NOT bm25_needs_compaction('inline_request_docs_idx'::regclass)
       AS inline_compacted;

-- Cancellation remains an error and aborts the writer transaction.
CREATE TABLE canceled_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX canceled_request_docs_idx ON canceled_request_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
SET pg_textsearch.compaction_request_function =
    'public.cancel_compaction_request';
INSERT INTO canceled_request_docs (body)
SELECT 'cancel seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('canceled_request_docs_idx') IS NOT NULL;
BEGIN;
INSERT INTO canceled_request_docs (body)
SELECT 'cancel pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('canceled_request_docs_idx') IS NOT NULL;
\set VERBOSITY terse
COMMIT;
\set VERBOSITY default
SELECT count(*) AS canceled_rows
FROM canceled_request_docs WHERE body LIKE 'cancel pending %';

RESET pg_textsearch.compaction_request_function;
RESET pg_textsearch.segments_per_level;
RESET client_min_messages;
DROP TABLE canceled_request_docs CASCADE;
DROP TABLE inline_request_docs CASCADE;
DROP TABLE off_request_docs CASCADE;
DROP TABLE off_build_docs CASCADE;
DROP TABLE final_build_docs CASCADE;
DROP TABLE reentrant_outer CASCADE;
DROP TABLE reentrant_inner CASCADE;
DROP TABLE prepared_request_docs CASCADE;
DROP TABLE dropped_request_docs CASCADE;
DROP TABLE aborted_index_docs CASCADE;
DROP TABLE rollback_drop_docs CASCADE;
DROP TABLE savepoint_docs CASCADE;
DROP TABLE relopt_docs CASCADE;
DROP TABLE request_docs2 CASCADE;
DROP TABLE request_docs CASCADE;
DROP TABLE compaction_callback_rows;
DROP SEQUENCE compaction_request_calls CASCADE;
DROP SEQUENCE compaction_request_calls_docs2 CASCADE;
DROP SEQUENCE compaction_request_calls_replaced CASCADE;
DROP SEQUENCE compaction_request_calls_shadow CASCADE;
DROP FUNCTION public.record_compaction_request(regclass);
DROP FUNCTION public.fail_compaction_request(regclass);
DROP FUNCTION public.cancel_compaction_request(regclass);
DROP FUNCTION public.temp_touching_request(regclass);
DROP FUNCTION public.reentrant_request(regclass);
DROP FUNCTION public.callback_slot(regclass);
DROP FUNCTION public.callback_slot_old(regclass);
DROP SCHEMA request_shadow CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
