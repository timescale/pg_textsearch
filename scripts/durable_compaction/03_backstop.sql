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
-- Re-running this script reuses exactly one canonical pending/running
-- instance. If multiple canonical instances are live, it fails with their
-- IDs; if only terminal instances exist, it creates one replacement.
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

BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;

-- Validate every pg_durable object this script calls or reads before starting
-- a canary or registering a schedule.
DO $$
DECLARE
    durable_oid     pg_catalog.oid;
    durable_version pg_catalog.text;
    version_parts   pg_catalog.text[];
BEGIN
    SELECT extension.oid, extension.extversion
    INTO durable_oid, durable_version
    FROM pg_catalog.pg_extension extension
    WHERE extension.extname OPERATOR(pg_catalog.=) 'pg_durable';

    IF durable_oid IS NULL THEN
        RAISE EXCEPTION
            'pg_durable 0.2.6 or newer is required; extension is not installed'
            USING HINT =
                'Install pg_durable in this database, then rerun this script.';
    END IF;

    version_parts := pg_catalog.regexp_match(
        durable_version, '^([0-9]+)\.([0-9]+)\.([0-9]+)');
    IF version_parts IS NULL
       OR ARRAY[
              version_parts[1]::pg_catalog.int4,
              version_parts[2]::pg_catalog.int4,
              version_parts[3]::pg_catalog.int4
          ] OPERATOR(pg_catalog.<) ARRAY[0, 2, 6]
    THEN
        RAISE EXCEPTION
            'pg_durable 0.2.6 or newer is required; found %',
            durable_version
            USING HINT =
                'Upgrade pg_durable, then rerun this script.';
    END IF;

    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_proc procedure
             JOIN pg_catalog.pg_depend dependency
               ON dependency.classid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_proc'::pg_catalog.regclass
              AND dependency.objid OPERATOR(pg_catalog.=) procedure.oid
              AND dependency.refclassid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_extension'::pg_catalog.regclass
              AND dependency.refobjid OPERATOR(pg_catalog.=) durable_oid
              AND dependency.deptype OPERATOR(pg_catalog.=) 'e'
        WHERE procedure.oid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regprocedure(
                      'df.start(pg_catalog.text,pg_catalog.text,'
                      'pg_catalog.text,pg_catalog.text)')
          AND procedure.prorettype OPERATOR(pg_catalog.=)
                  'pg_catalog.text'::pg_catalog.regtype
          AND procedure.pronargdefaults OPERATOR(pg_catalog.=) 3
          AND procedure.proargnames OPERATOR(pg_catalog.=)
                  ARRAY['fut', 'label', 'database', 'transaction_mode'])
       OR NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_proc procedure
             JOIN pg_catalog.pg_depend dependency
               ON dependency.classid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_proc'::pg_catalog.regclass
              AND dependency.objid OPERATOR(pg_catalog.=) procedure.oid
              AND dependency.refclassid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_extension'::pg_catalog.regclass
              AND dependency.refobjid OPERATOR(pg_catalog.=) durable_oid
              AND dependency.deptype OPERATOR(pg_catalog.=) 'e'
        WHERE procedure.oid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regprocedure(
                      'df.loop(pg_catalog.text,pg_catalog.text)')
          AND procedure.prorettype OPERATOR(pg_catalog.=)
                  'pg_catalog.text'::pg_catalog.regtype
          AND procedure.pronargdefaults OPERATOR(pg_catalog.=) 1)
       OR NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_proc procedure
             JOIN pg_catalog.pg_depend dependency
               ON dependency.classid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_proc'::pg_catalog.regclass
              AND dependency.objid OPERATOR(pg_catalog.=) procedure.oid
              AND dependency.refclassid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_extension'::pg_catalog.regclass
              AND dependency.refobjid OPERATOR(pg_catalog.=) durable_oid
              AND dependency.deptype OPERATOR(pg_catalog.=) 'e'
        WHERE procedure.oid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regprocedure(
                      'df.wait_for_schedule(pg_catalog.text)')
          AND procedure.prorettype OPERATOR(pg_catalog.=)
                  'pg_catalog.text'::pg_catalog.regtype)
       OR NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_operator operator
             JOIN pg_catalog.pg_depend dependency
               ON dependency.classid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_operator'::pg_catalog.regclass
              AND dependency.objid OPERATOR(pg_catalog.=) operator.oid
              AND dependency.refclassid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_extension'::pg_catalog.regclass
              AND dependency.refobjid OPERATOR(pg_catalog.=) durable_oid
              AND dependency.deptype OPERATOR(pg_catalog.=) 'e'
        WHERE operator.oid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regoperator(
                      'pg_catalog.~>(pg_catalog.text,pg_catalog.text)')
          AND operator.oprresult OPERATOR(pg_catalog.=)
                  'pg_catalog.text'::pg_catalog.regtype
          AND operator.oprcode OPERATOR(pg_catalog.=)
                  pg_catalog.to_regprocedure(
                      'df.seq(pg_catalog.text,pg_catalog.text)'))
       OR NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_class relation
             JOIN pg_catalog.pg_depend dependency
               ON dependency.classid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_class'::pg_catalog.regclass
              AND dependency.objid OPERATOR(pg_catalog.=) relation.oid
              AND dependency.refclassid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_extension'::pg_catalog.regclass
              AND dependency.refobjid OPERATOR(pg_catalog.=) durable_oid
              AND dependency.deptype OPERATOR(pg_catalog.=) 'e'
        WHERE relation.oid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regclass('df.instances'))
       OR (
        SELECT pg_catalog.count(*)
        FROM pg_catalog.pg_attribute attribute
        WHERE attribute.attrelid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regclass('df.instances')
          AND attribute.attnum OPERATOR(pg_catalog.>) 0
          AND NOT attribute.attisdropped
          AND (
              (attribute.attname OPERATOR(pg_catalog.=) 'id'
               AND attribute.atttypid OPERATOR(pg_catalog.=)
                       'pg_catalog.varchar'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'root_node'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.varchar'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'label'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.text'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'status'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.text'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'submitted_by'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.regrole'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'database'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.text'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'created_at'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.timestamptz'::pg_catalog.regtype)))
           OPERATOR(pg_catalog.<>) 7
       OR NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_class relation
             JOIN pg_catalog.pg_depend dependency
               ON dependency.classid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_class'::pg_catalog.regclass
              AND dependency.objid OPERATOR(pg_catalog.=) relation.oid
              AND dependency.refclassid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_extension'::pg_catalog.regclass
              AND dependency.refobjid OPERATOR(pg_catalog.=) durable_oid
              AND dependency.deptype OPERATOR(pg_catalog.=) 'e'
        WHERE relation.oid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regclass('df.nodes'))
       OR (
        SELECT pg_catalog.count(*)
        FROM pg_catalog.pg_attribute attribute
        WHERE attribute.attrelid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regclass('df.nodes')
          AND attribute.attnum OPERATOR(pg_catalog.>) 0
          AND NOT attribute.attisdropped
          AND (
              (attribute.attname OPERATOR(pg_catalog.=) 'id'
               AND attribute.atttypid OPERATOR(pg_catalog.=)
                       'pg_catalog.varchar'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'instance_id'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.varchar'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'node_type'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.text'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'query'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.text'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'left_node'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.varchar'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'right_node'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.varchar'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'status'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.text'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'result'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.jsonb'::pg_catalog.regtype)))
           OPERATOR(pg_catalog.<>) 8
    THEN
        RAISE EXCEPTION
            'pg_durable 0.2.6 API is incomplete: required start/loop/schedule '
            'functions, text ~> operator, or instance/node diagnostics '
            'do not match'
            USING HINT =
                'Install or upgrade pg_durable, then rerun this script.';
    END IF;
END
$$;

COMMIT;

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

-- Submit this outside the registration transaction so the worker can see it.
-- The label is observability metadata only; the exact returned id is the
-- identity used for every status and result check.
BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;

DO $$
BEGIN
    IF CURRENT_USER OPERATOR(pg_catalog.<>) 'textsearch_compactor'
    THEN
        RAISE EXCEPTION
            '03_backstop.sql must run as textsearch_compactor; current user is %',
            CURRENT_USER;
    END IF;
END
$$;

SELECT pg_catalog.set_config(
    'pg_textsearch_backstop.canary_instance',
    df.start(
        'SELECT 1 AS pg_textsearch_canary',
        label => 'bm25-compaction-canary'),
    false);

COMMIT;

DO $$
DECLARE
    canary_id      pg_catalog.text :=
        pg_catalog.current_setting(
            'pg_textsearch_backstop.canary_instance');
    instance_state pg_catalog.text;
    node_state     pg_catalog.text;
    node_result    pg_catalog.jsonb;
    deadline       pg_catalog.timestamptz :=
        pg_catalog.clock_timestamp() + interval '60 seconds';
BEGIN
    LOOP
        SELECT instance.status, node.status, node.result
        INTO instance_state, node_state, node_result
        FROM df.instances instance
             LEFT JOIN df.nodes node
               ON node.instance_id OPERATOR(pg_catalog.=) instance.id
              AND node.id OPERATOR(pg_catalog.=) instance.root_node
        WHERE instance.id OPERATOR(pg_catalog.=) canary_id;

        IF instance_state OPERATOR(pg_catalog.=) 'completed' THEN
            IF node_state OPERATOR(pg_catalog.<>) 'completed'
               OR pg_catalog.jsonb_extract_path_text(
                      node_result,
                      'rows',
                      '0',
                      'pg_textsearch_canary')
                      IS DISTINCT FROM '1'
            THEN
                RAISE EXCEPTION
                    'textsearch_compactor execution canary returned an '
                    'unexpected result (instance %, node status %, result %)',
                    canary_id, node_state, node_result;
            END IF;
            RAISE NOTICE
                'textsearch_compactor execution canary % completed',
                canary_id;
            EXIT;
        ELSIF instance_state OPERATOR(pg_catalog.=)
                  ANY (ARRAY['failed', 'cancelled'])
        THEN
            RAISE EXCEPTION
                'textsearch_compactor execution canary failed '
                '(instance %, status %, result %)',
                canary_id, instance_state, node_result
                USING HINT =
                    'Fix textsearch_compactor LOGIN/authentication and inspect '
                    'the PostgreSQL server log before registering the backstop.';
        END IF;

        IF pg_catalog.clock_timestamp() OPERATOR(pg_catalog.>=) deadline THEN
            RAISE EXCEPTION
                'textsearch_compactor execution canary failed to reach a '
                'terminal state within 60 seconds (instance %, status %)',
                canary_id, instance_state
                USING HINT =
                    'Verify the pg_durable worker is running and inspect the '
                    'PostgreSQL server log before registering the backstop.';
        END IF;

        PERFORM pg_catalog.pg_sleep(1);
    END LOOP;
END
$$;

BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;

-- Concurrent installers share a cluster-wide advisory namespace, so include
-- the database name in the stable key and hold it through registration.
SELECT pg_catalog.pg_advisory_xact_lock(
    pg_catalog.hashtextextended(
        pg_catalog.current_database()::pg_catalog.text
        OPERATOR(pg_catalog.||)
        ':pg_textsearch:bm25-compaction-backstop',
        0));

DO $$
DECLARE
    instance             text;
    canonical_instances text[];
    extension_schema     text;
    body                 text;
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

    -- The label remains observability metadata, not authorization or identity.
    -- Match the database, submitter, and exact loop/sequence/body graph before
    -- reusing a live instance.
    SELECT pg_catalog.array_agg(
               canonical.id ORDER BY canonical.created_at, canonical.id)
    INTO canonical_instances
    FROM (
        SELECT DISTINCT durable_instance.id, durable_instance.created_at
        FROM df.instances durable_instance
             JOIN df.nodes loop_node
               ON loop_node.instance_id OPERATOR(pg_catalog.=)
                      durable_instance.id
              AND loop_node.id OPERATOR(pg_catalog.=)
                      durable_instance.root_node
             JOIN df.nodes sequence_node
               ON sequence_node.instance_id OPERATOR(pg_catalog.=)
                      durable_instance.id
              AND sequence_node.id OPERATOR(pg_catalog.=) loop_node.left_node
             JOIN df.nodes schedule_node
               ON schedule_node.instance_id OPERATOR(pg_catalog.=)
                      durable_instance.id
              AND schedule_node.id OPERATOR(pg_catalog.=)
                      sequence_node.left_node
             JOIN df.nodes body_node
               ON body_node.instance_id OPERATOR(pg_catalog.=)
                      durable_instance.id
              AND body_node.id OPERATOR(pg_catalog.=)
                      sequence_node.right_node
        WHERE durable_instance.submitted_by OPERATOR(pg_catalog.=)
                  pg_catalog.to_regrole(CURRENT_USER)
          AND COALESCE(
                  durable_instance.database,
                  pg_catalog.current_database()::pg_catalog.text)
                  OPERATOR(pg_catalog.=) pg_catalog.current_database()
          AND durable_instance.status OPERATOR(pg_catalog.=)
                  ANY (ARRAY['pending', 'running'])
          AND loop_node.node_type OPERATOR(pg_catalog.=) 'LOOP'
          AND loop_node.right_node IS NULL
          AND sequence_node.node_type OPERATOR(pg_catalog.=) 'THEN'
          AND schedule_node.node_type OPERATOR(pg_catalog.=) 'WAIT_SCHEDULE'
          AND body_node.node_type OPERATOR(pg_catalog.=) 'SQL'
          AND body_node.query OPERATOR(pg_catalog.=) body
    ) canonical;

    IF pg_catalog.cardinality(canonical_instances) OPERATOR(pg_catalog.>) 1
    THEN
        RAISE EXCEPTION
            'multiple live canonical backstops found: %',
            pg_catalog.array_to_string(canonical_instances, ', ')
            USING HINT =
                'Cancel all but one listed instance with '
                'SELECT df.cancel(''<instance-id>''); wait for terminal '
                'status, then rerun this script.';
    END IF;

    instance := canonical_instances[1];

    IF instance IS NULL THEN
        SELECT df.start(
            df.loop(
                df.wait_for_schedule(
                    pg_catalog.current_setting(
                        'pg_textsearch_backstop.cron'))
                OPERATOR(pg_catalog.~>) body),
            label => 'bm25-compaction-backstop')
        INTO instance;

        RAISE NOTICE 'backstop started as instance %', instance;
    ELSE
        RAISE NOTICE 'reusing live backstop instance %', instance;
    END IF;

    PERFORM pg_catalog.set_config(
        'pg_textsearch_backstop.instance', instance, false);
END
$$;

COMMIT;

-- Confirm the canonical instance is live. It should stay 'running' forever;
-- terminal history remains available but is not printed here.
SELECT id, label, status, submitted_by, created_at
FROM df.instances
WHERE id OPERATOR(pg_catalog.=)
          pg_catalog.current_setting('pg_textsearch_backstop.instance')
  AND submitted_by OPERATOR(pg_catalog.=)
          pg_catalog.to_regrole(CURRENT_USER)
  AND status OPERATOR(pg_catalog.=) ANY (ARRAY['pending', 'running'])
ORDER BY created_at DESC;
