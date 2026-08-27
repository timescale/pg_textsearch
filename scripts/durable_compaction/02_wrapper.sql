-- 02_wrapper.sql -- the SECURITY DEFINER compaction request wrapper.
--
-- Run as a superuser (or as a role that is a member of both
-- textsearch_compactor and the schema owner), in the database
-- named by pg_durable.database, AFTER 01_setup_role.sql:
--
--   psql -d postgres -f 02_wrapper.sql -v writer_role=app_writer
--
-- This is the function that pg_textsearch calls at PRE_COMMIT when
-- pg_textsearch.compaction_mode = 'background'.  The extension emits
--
--     SELECT <compaction_request_function>(<oid>::oid::regclass)
--
-- once per index that requested compaction during the transaction.
--
-- Why SECURITY DEFINER: df.start() stamps df.instances.submitted_by
-- from GetUserId() (pg_durable src/dsl.rs:1113), and the background
-- worker later runs every SQL node in a connection opened as that
-- role.  Running the wrapper as its owner therefore re-attributes
-- the whole durable task to textsearch_compactor.  That is the
-- entire mechanism by which an unprivileged writer -- who holds no
-- df privileges at all, only EXECUTE on this one function -- can
-- enqueue compaction that then runs with the index owner's rights.
--
-- The caller supplies only a regclass and must be the login role with
-- INSERT privilege on the indexed heap.  The DSL shape and function
-- names are fixed here; only the numeric index OID is constructed
-- into the SQL text.

\set ON_ERROR_STOP on

\if :{?writer_role}
\else
\set writer_role ''
\endif

DROP FUNCTION IF EXISTS public.bm25_request_compaction(regclass);

-- Recreate rather than replace so reinstalling this script removes
-- every prior named EXECUTE grant before applying writer_role below.
CREATE FUNCTION public.bm25_request_compaction(idx regclass)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $fn$
DECLARE
    ext_schema text;
    idx_oid    text;
    body       text;
    cond       text;
BEGIN
    -- Reject anything that is not a bm25 index before enqueuing a
    -- task that could only ever fail.  EXECUTE on this function is
    -- granted to writers, so it should not be usable as a generic
    -- "make the compactor do something" button.
    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_class c
             JOIN pg_catalog.pg_am am ON am.oid = c.relam
        WHERE c.oid = idx AND am.amname = 'bm25')
    THEN
        RAISE EXCEPTION '"%" is not a bm25 index', idx::text;
    END IF;

    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_index i
        WHERE i.indexrelid = idx
          AND pg_catalog.has_table_privilege(
                  session_user, i.indrelid, 'INSERT'))
    THEN
        RAISE EXCEPTION
            'permission denied to request compaction for %', idx
            USING ERRCODE = 'insufficient_privilege';
    END IF;

    SELECT n.nspname INTO ext_schema
    FROM pg_catalog.pg_extension e
         JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace
    WHERE e.extname = 'pg_textsearch';

    IF ext_schema IS NULL THEN
        RAISE EXCEPTION 'extension pg_textsearch is not installed here';
    END IF;

    -- Pass the index as a bare OID reconstituted with
    -- <oid>::oid::regclass.  The worker session has its own
    -- search_path, so a name would be ambiguous; an OID is
    -- search_path-independent and, being an integer, leaves no room
    -- for injection.
    idx_oid := idx::oid::text;

    body := pg_catalog.format(
        'SELECT %I.bm25_compact_step(%s::oid::regclass)',
        ext_schema, idx_oid);
    cond := pg_catalog.format(
        'SELECT %I.bm25_needs_compaction(%s::oid::regclass)',
        ext_schema, idx_oid);

    -- The stepped shape.  df.loop(body, condition) runs the body,
    -- then evaluates the condition as an INDEPENDENT SQL expression
    -- and continues while it is truthy.  Every node execution --
    -- body and condition alike -- is dispatched on its own
    -- connection and therefore in its own transaction, so the
    -- per-index LW_EXCLUSIVE lock is dropped between cascade levels
    -- instead of being held for the whole cascade.  That is exactly
    -- what bm25_compact_step() exists for; calling whole-cascade
    -- bm25_compact() here would give back the property we are
    -- buying.
    --
    -- Termination is guaranteed: bm25_compact_step() merges at the
    -- lowest over-threshold level in levels 0..6 and promotes
    -- strictly upward, and bm25_needs_compaction() tests the same
    -- levels against the same threshold, so the condition goes false
    -- once no level is over threshold.
    --
    -- The GenericXLog-logged level state is the durable truth.  This
    -- request is only an accelerator: a redundant or early request is
    -- a harmless no-op, while the next spill and the periodic backstop
    -- re-check that durable state.  Persist the request independently
    -- so a later failure of the caller's transaction cannot discard an
    -- accelerator that has already been submitted.
    RETURN df.start(
        df.loop(body, cond),
        label            => 'bm25-compact-' || idx_oid,
        transaction_mode => 'new');
END;
$fn$;

ALTER FUNCTION public.bm25_request_compaction(regclass)
    OWNER TO textsearch_compactor;

REVOKE EXECUTE ON FUNCTION public.bm25_request_compaction(regclass)
    FROM PUBLIC;

-- Grant EXECUTE to the writer role, if one was supplied.  Without
-- -v writer_role=... nothing is granted here; do it yourself with
--   GRANT EXECUTE ON FUNCTION public.bm25_request_compaction(regclass)
--       TO <writer role>;
SELECT CASE WHEN nullif(:'writer_role', '') IS NULL
            THEN 'off' ELSE 'on' END       AS have_writer,
       coalesce(nullif(:'writer_role', ''), 'none') AS writer
\gset

\if :have_writer
GRANT EXECUTE ON FUNCTION public.bm25_request_compaction(regclass)
    TO :"writer";
\endif

\echo 'bm25_request_compaction() ready.'
\echo 'Now set, e.g. in postgresql.conf:'
\echo '  pg_textsearch.compaction_mode = ''background'''
\echo '  pg_textsearch.compaction_request_function ='
\echo '      ''public.bm25_request_compaction'''
