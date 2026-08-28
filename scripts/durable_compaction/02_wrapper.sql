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
--     SELECT <compaction_request_function>(
--         <oid>::pg_catalog.oid::pg_catalog.regclass)
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
-- INSERT privilege on the indexed heap or one of its partition
-- ancestors.  The DSL shape and function names are fixed here; only
-- the numeric target identity is constructed into the SQL text.
-- Granting EXECUTE deliberately also permits direct calls: this function
-- is the approved writer's durable-job submission capability, not a
-- callback-provenance gate. Labels are observability metadata, not
-- authorization or deduplication.

\set ON_ERROR_STOP on

\if :{?writer_role}
\else
\set writer_role ''
\endif

BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;

-- The wrapper body is parsed without resolving its callees. Check the exact
-- pg_durable contract before dropping the previously installed wrapper.
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
    THEN
        RAISE EXCEPTION
            'pg_durable 0.2.6 API is incomplete: required extension members '
            'df.start(text, text, text, text) and df.loop(text, text)'
            USING HINT =
                'Install or upgrade pg_durable, then rerun this script.';
    END IF;
END
$$;

DROP FUNCTION IF EXISTS
    public.bm25_request_compaction(pg_catalog.regclass);

-- Recreate rather than replace to discard old ACLs.  The surrounding
-- transaction leaves the previous wrapper intact if any later step
-- fails.
CREATE FUNCTION public.bm25_request_compaction(idx pg_catalog.regclass)
RETURNS pg_catalog.text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $fn$
DECLARE
    ext_schema     pg_catalog.text;
    idx_oid        pg_catalog.text;
    database_oid   pg_catalog.text;
    tablespace_oid pg_catalog.text;
    relfilenumber  pg_catalog.text;
    body           pg_catalog.text;
    cond           pg_catalog.text;
BEGIN
    -- Reject anything that is not a bm25 index before enqueuing a
    -- task that could only ever fail.  EXECUTE on this function is
    -- granted to writers, so it should not be usable as a generic
    -- "make the compactor do something" button.
    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_class c
             JOIN pg_catalog.pg_am am ON am.oid = c.relam
        WHERE c.oid = idx
          AND c.relkind = 'i'
          AND am.amname = 'bm25')
    THEN
        RAISE EXCEPTION
            '"%" is not a bm25 index', idx::pg_catalog.text;
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_class c
        WHERE c.oid = idx
          AND c.relpersistence = 't')
    THEN
        RAISE EXCEPTION
            'temporary bm25 indexes cannot use background compaction'
            USING ERRCODE = 'feature_not_supported';
    END IF;

    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_index i
        WHERE i.indexrelid = idx
          AND (
              pg_catalog.has_table_privilege(
                  session_user, i.indrelid, 'INSERT')
              OR EXISTS (
                  SELECT 1
                  FROM pg_catalog.pg_partition_ancestors(i.indrelid)
                       ancestor(relid)
                  WHERE pg_catalog.has_table_privilege(
                            session_user, ancestor.relid, 'INSERT'))))
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

    SELECT idx::pg_catalog.oid::pg_catalog.text,
           db.oid::pg_catalog.text,
           coalesce(
               nullif(c.reltablespace, 0),
               db.dattablespace)::pg_catalog.text,
           pg_catalog.pg_relation_filenode(idx)::pg_catalog.text
    INTO idx_oid, database_oid, tablespace_oid, relfilenumber
    FROM pg_catalog.pg_class c
         JOIN pg_catalog.pg_database db
           ON db.datname = pg_catalog.current_database()
    WHERE c.oid = idx;

    body := pg_catalog.format(
        'SELECT %I.bm25_compact_step_if_current('
        '%s::pg_catalog.oid, %s::pg_catalog.oid, '
        '%s::pg_catalog.oid, %s::pg_catalog.oid)',
        ext_schema, idx_oid, database_oid, tablespace_oid, relfilenumber);
    cond := pg_catalog.format(
        'SELECT %I.bm25_needs_compaction_if_current('
        '%s::pg_catalog.oid, %s::pg_catalog.oid, '
        '%s::pg_catalog.oid, %s::pg_catalog.oid)',
        ext_schema, idx_oid, database_oid, tablespace_oid, relfilenumber);

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

ALTER FUNCTION public.bm25_request_compaction(pg_catalog.regclass)
    OWNER TO textsearch_compactor;

REVOKE ALL ON FUNCTION
    public.bm25_request_compaction(pg_catalog.regclass)
    FROM PUBLIC CASCADE;

-- Default privileges can add named grants to a newly created function.
-- Remove every non-owner grantee before adding the intended writer.
DO $$
DECLARE
    grantee_name name;
BEGIN
    FOR grantee_name IN
        SELECT DISTINCT role.rolname
        FROM pg_catalog.pg_proc proc
             CROSS JOIN LATERAL pg_catalog.aclexplode(
                 coalesce(
                     proc.proacl,
                     pg_catalog.acldefault('f', proc.proowner))) acl
             JOIN pg_catalog.pg_roles role ON role.oid = acl.grantee
        WHERE proc.oid =
                  'public.bm25_request_compaction(pg_catalog.regclass)'
                      ::pg_catalog.regprocedure
          AND acl.grantee <> proc.proowner
    LOOP
        EXECUTE pg_catalog.format(
            'REVOKE ALL ON FUNCTION '
            'public.bm25_request_compaction(pg_catalog.regclass) '
            'FROM %I CASCADE',
            grantee_name);
    END LOOP;
END
$$;

-- Grant EXECUTE to the writer role, if one was supplied.  Without
-- -v writer_role=... nothing is granted here; do it yourself with
--   GRANT EXECUTE ON FUNCTION public.bm25_request_compaction(regclass)
--       TO <writer role>;
SELECT CASE WHEN nullif(:'writer_role', '') IS NULL
            THEN 'off' ELSE 'on' END       AS have_writer,
       coalesce(nullif(:'writer_role', ''), 'none') AS writer
\gset

\if :have_writer
GRANT EXECUTE ON FUNCTION
    public.bm25_request_compaction(pg_catalog.regclass)
    TO :"writer";
\endif

COMMIT;

\echo 'bm25_request_compaction() ready.'
\echo 'Now set, e.g. in postgresql.conf:'
\echo '  pg_textsearch.compaction_mode = ''background'''
\echo '  pg_textsearch.compaction_request_function ='
\echo '      ''public.bm25_request_compaction'''
