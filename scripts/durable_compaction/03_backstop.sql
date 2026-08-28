-- 03_backstop.sql -- the hourly rescue sweep.
--
-- Run AS the compactor role, in the database named by
-- pg_durable.database, AFTER 01_setup_role.sql:
--
--   psql -U textsearch_compactor -d postgres -f 03_backstop.sql
--
-- Running it as the compactor is deliberate: df.start() stamps
-- df.instances.submitted_by from GetUserId(), so submitting the
-- backstop directly as textsearch_compactor gets the same
-- identity that 02_wrapper.sql obtains via SECURITY DEFINER -- no
-- definer function is needed here.
--
-- Start this once.  It is a single long-lived instance.
--
-- The sweep cadence defaults to hourly and can be overridden with
-- -v cron=... (e.g. for a faster cadence in a test harness):
--
--   psql -U textsearch_compactor -d postgres -f 03_backstop.sql \
--       -v cron='* * * * *'
--
--
-- WHY THIS EXISTS
--
-- pg_durable 0.2.6 has no retry, no backoff and no on_failure: a
-- node that raises kills its instance outright.  So a compaction
-- request fired by a spill is an accelerator, not a guarantee.  Two
-- layers cover failure:
--
--   1. Next-spill retry.  A failed compaction self-heals the moment
--      another spill fires a fresh request.  Free for any index that
--      is still being written to.
--   2. This backstop.  It covers the case layer 1 cannot: writes
--      stop right after a failed compaction, and the index stays
--      degraded indefinitely -- precisely when queries are slowest
--      relative to the work outstanding.
--
-- Hourly is deliberately slow.  This is a rescue path, not a poller.
--
--
-- TRANSACTION-GRANULARITY CAVEAT -- READ THIS
--
-- bm25_compact_pending() calls whole-cascade bm25_compact() per
-- index, and the entire sweep is one node execution, hence ONE
-- transaction holding the per-index LW_EXCLUSIVE for a full cascade.
-- That is the opposite of the hot path, where
-- bm25_request_compaction() drives bm25_compact_step() through
-- df.loop() so each merge batch gets its own transaction.
--
-- The stepped shape was tried here and rejected on evidence.  It is
-- expressible -- nested df.loop() works in 0.2.6, and
--
--     df.loop(
--         'SELECT public.bm25_compact_step(s.i)
--            FROM (SELECT i
--                    FROM public.bm25_indexes_needing_compaction() i
--                   WHERE public.bm25_needs_compaction(i)
--                   ORDER BY i LIMIT 1) s(i)',
--         'SELECT EXISTS (SELECT 1
--             FROM public.bm25_indexes_needing_compaction() i
--            WHERE public.bm25_needs_compaction(i))')
--
-- does one merge batch per transaction across all pending indexes.
-- But it loses bm25_compact_pending()'s per-index
-- `EXCEPTION WHEN OTHERS`, and that isolation is what keeps one
-- broken index from taking down the whole guarantee path:
--
--   * Without isolation, an index the compactor can no longer
--     compact (ownership revoked, say) raises, kills this single
--     long-lived instance, and -- with no retry in 0.2.6 -- the
--     backstop is simply gone until an operator notices.
--   * With isolation added back, it is worse: the swallowed error
--     means no progress, while the loop condition is an INDEPENDENT
--     expression that cannot see that, so it stays true and the
--     inner loop spins at pg_durable's 1 s minimum iteration rate
--     until MAX_LOOP_ITERATIONS (100 000, ~27 h) finally kills the
--     instance anyway.
--
-- Holding an exclusive lock for one cascade on a path that should
-- rarely fire is the cheaper of those three costs, so the
-- whole-cascade sweep stays.  Revisit if pg_durable gains
-- per-node retry / on_failure (upstream PR #354): with those, the
-- stepped shape above becomes the better option.

\set ON_ERROR_STOP on

SET search_path = pg_catalog, pg_temp;

\if :{?cron}
\else
\set cron '0 * * * *'
\endif

-- psql's :'var' interpolation does not reach inside a dollar-quoted
-- ($$ ... $$) body -- deliberately, so it cannot mangle plpgsql's
-- own ':=' assignment operator.  Stash the cadence in a custom GUC
-- instead, set here at the top level where interpolation does
-- apply, and read it back with current_setting() below.
SELECT pg_catalog.set_config('pg_textsearch_backstop.cron', :'cron', false);

DO $$
DECLARE
    instance         text;
    extension_schema text;
    body             text;
    attempts         int := 0;
    ready            bool;
BEGIN
    SELECT namespace.nspname
    INTO extension_schema
    FROM pg_catalog.pg_extension extension
         JOIN pg_catalog.pg_namespace namespace
           ON namespace.oid = extension.extnamespace
         JOIN pg_catalog.pg_proc procedure
           ON procedure.oid = pg_catalog.to_regprocedure(
               pg_catalog.format(
                   '%I.bm25_compact_pending()', namespace.nspname))
         JOIN pg_catalog.pg_depend dependency
           ON dependency.classid =
                  'pg_catalog.pg_proc'::pg_catalog.regclass
          AND dependency.objid = procedure.oid
          AND dependency.refclassid =
                  'pg_catalog.pg_extension'::pg_catalog.regclass
          AND dependency.refobjid = extension.oid
          AND dependency.deptype = 'e'
    WHERE extension.extname = 'pg_textsearch';

    IF extension_schema IS NULL THEN
        RAISE EXCEPTION
            'pg_textsearch bm25_compact_pending() extension member not found';
    END IF;

    body := pg_catalog.format(
        'SELECT %I.bm25_compact_pending()', extension_schema);

    -- The background worker initializes ASYNCHRONOUSLY after
    -- CREATE EXTENSION pg_durable / after a server restart.  Until
    -- it has written its epoch sentinel, df.start() cannot hand the
    -- instance to the durable engine.  Wait for the sentinel first
    -- (best effort -- df._worker_epoch is internal and may not be
    -- readable), then retry df.start() itself.
    FOR i IN 1..60 LOOP
        BEGIN
            SELECT EXISTS (
                SELECT 1 FROM df._worker_epoch
                WHERE last_seen_at > pg_catalog.clock_timestamp()
                                     - interval '2 minutes')
            INTO ready;
        EXCEPTION WHEN OTHERS THEN
            ready := true;  -- cannot probe; fall through to retry
        END;
        EXIT WHEN ready;
        PERFORM pg_catalog.pg_sleep(1);
    END LOOP;

    LOOP
        BEGIN
            SELECT df.start(
                df.loop(
                    df.wait_for_schedule(
                        pg_catalog.current_setting('pg_textsearch_backstop.cron'))
                    OPERATOR(pg_catalog.~>) body),
                label => 'bm25-compaction-backstop')
            INTO instance;

            RAISE NOTICE 'backstop started as instance %', instance;
            EXIT;
        EXCEPTION WHEN OTHERS THEN
            attempts := attempts + 1;
            IF attempts >= 30 THEN
                RAISE;
            END IF;
            RAISE NOTICE
                'pg_durable not ready yet (%); retrying', SQLERRM;
            PERFORM pg_catalog.pg_sleep(2);
        END;
    END LOOP;
END
$$;

-- Confirm it is live.  Exactly one row should be here, and it should
-- stay 'running' forever -- the loop never terminates on its own.
SELECT id, label, status, submitted_by, created_at
FROM df.instances
WHERE label = 'bm25-compaction-backstop'
ORDER BY created_at DESC;
