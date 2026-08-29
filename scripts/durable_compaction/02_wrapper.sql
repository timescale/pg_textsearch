-- Install the SECURITY DEFINER pg_durable request wrapper.
--
-- Run as a superuser in pg_durable.database, after 01_setup_role.sql:
--
--   psql -f 02_wrapper.sql -v writer_role=app_writer

\set ON_ERROR_STOP on

\if :{?writer_role}
\else
\set writer_role ''
\endif

BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;

-- Keep the unreleased loop-policy dependency in this one preflight block.
-- Replace the capability-only error with a version floor once a release
-- contains df.loop(text, text, text).
DO $$
DECLARE
    durable_oid pg_catalog.oid;
BEGIN
    SELECT extension.oid
    INTO durable_oid
    FROM pg_catalog.pg_extension extension
    WHERE extension.extname OPERATOR(pg_catalog.=) 'pg_durable';

    IF durable_oid IS NULL THEN
        RAISE EXCEPTION
            'pg_durable with df.loop(..., on_error) is required'
            USING HINT =
                'Install the released pg_durable loop-policy build, then '
                'rerun this script.';
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
                      'df.loop(pg_catalog.text,pg_catalog.text,'
                      'pg_catalog.text)')
          AND procedure.prorettype OPERATOR(pg_catalog.=)
                  'pg_catalog.text'::pg_catalog.regtype
          AND procedure.proargnames[3] OPERATOR(pg_catalog.=) 'on_error')
       OR pg_catalog.to_regclass('df.instances') IS NULL
       OR pg_catalog.to_regclass('df.nodes') IS NULL
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
              OR (attribute.attname OPERATOR(pg_catalog.=) 'status'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.text'::pg_catalog.regtype)))
           OPERATOR(pg_catalog.<>) 2
       OR (
        SELECT pg_catalog.count(*)
        FROM pg_catalog.pg_attribute attribute
        WHERE attribute.attrelid OPERATOR(pg_catalog.=)
                  pg_catalog.to_regclass('df.nodes')
          AND attribute.attnum OPERATOR(pg_catalog.>) 0
          AND NOT attribute.attisdropped
          AND (
              (attribute.attname OPERATOR(pg_catalog.=) 'instance_id'
               AND attribute.atttypid OPERATOR(pg_catalog.=)
                       'pg_catalog.varchar'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'status'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.text'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'result'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.jsonb'::pg_catalog.regtype)
              OR (attribute.attname OPERATOR(pg_catalog.=) 'updated_at'
                  AND attribute.atttypid OPERATOR(pg_catalog.=)
                          'pg_catalog.timestamptz'::pg_catalog.regtype)))
           OPERATOR(pg_catalog.<>) 4
    THEN
        RAISE EXCEPTION
            'pg_durable API is incomplete: df.start(), '
            'df.loop(..., on_error), df.instances, and df.nodes are required'
            USING HINT =
                'Install the released pg_durable loop-policy build, then '
                'rerun this script.';
    END IF;
END
$$;

SELECT nullif(:'writer_role', '') IS NOT NULL AS have_writer,
       coalesce(nullif(:'writer_role', ''), 'none') AS writer
\gset

\if :have_writer
SELECT EXISTS (
           SELECT 1
           FROM pg_catalog.pg_roles
           WHERE rolname OPERATOR(pg_catalog.=) :'writer'
       ) AS writer_exists,
       coalesce((
           SELECT rolsuper
           FROM pg_catalog.pg_roles
           WHERE rolname OPERATOR(pg_catalog.=) :'writer'
       ), false) AS writer_is_superuser
\gset

\if :writer_exists
\else
DO $$
BEGIN
    RAISE EXCEPTION 'writer_role does not exist';
END
$$;
\endif

\if :writer_is_superuser
DO $$
BEGIN
    RAISE EXCEPTION 'writer_role must not be a superuser';
END
$$;
\endif
\endif

DO $$
DECLARE
    compactor_oid pg_catalog.oid;
    wrapper_oid   pg_catalog.oid;
BEGIN
    SELECT oid
    INTO compactor_oid
    FROM pg_catalog.pg_roles
    WHERE rolname OPERATOR(pg_catalog.=) 'textsearch_compactor';

    IF compactor_oid IS NULL THEN
        RAISE EXCEPTION
            'textsearch_compactor does not exist'
            USING HINT = 'Run 01_setup_role.sql first.';
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_authid
        WHERE oid OPERATOR(pg_catalog.=) compactor_oid
          AND (rolpassword IS NOT NULL
               OR NOT rolcanlogin
               OR NOT rolinherit
               OR rolsuper
               OR rolcreatedb
               OR rolcreaterole
               OR rolreplication
               OR rolbypassrls
               OR rolconnlimit OPERATOR(pg_catalog.<>) -1))
    THEN
        RAISE EXCEPTION
            'textsearch_compactor has unsafe credentials or attributes';
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_namespace namespace
             CROSS JOIN LATERAL pg_catalog.aclexplode(
                 coalesce(
                     namespace.nspacl,
                     pg_catalog.acldefault(
                         'n', namespace.nspowner))) acl
        WHERE namespace.nspname OPERATOR(pg_catalog.=) 'public'
          AND acl.grantee OPERATOR(pg_catalog.=) 0
          AND acl.privilege_type OPERATOR(pg_catalog.=) 'CREATE')
    THEN
        RAISE EXCEPTION
            'PUBLIC must not have CREATE on wrapper schema public';
    END IF;

    wrapper_oid := pg_catalog.to_regprocedure(
        'public.bm25_request_compaction(pg_catalog.regclass)');

    IF wrapper_oid IS NOT NULL
       AND NOT EXISTS (
           SELECT 1
           FROM pg_catalog.pg_proc procedure
                JOIN pg_catalog.pg_namespace namespace
                  ON namespace.oid OPERATOR(pg_catalog.=)
                         procedure.pronamespace
                JOIN pg_catalog.pg_language language
                  ON language.oid OPERATOR(pg_catalog.=) procedure.prolang
           WHERE procedure.oid OPERATOR(pg_catalog.=) wrapper_oid
             AND namespace.nspname OPERATOR(pg_catalog.=) 'public'
             AND procedure.proname OPERATOR(pg_catalog.=)
                     'bm25_request_compaction'
             AND procedure.proowner OPERATOR(pg_catalog.=) compactor_oid
             AND procedure.prokind OPERATOR(pg_catalog.=) 'f'
             AND procedure.pronargs OPERATOR(pg_catalog.=) 1
             AND procedure.proargtypes[0] OPERATOR(pg_catalog.=)
                     'pg_catalog.regclass'::pg_catalog.regtype
             AND procedure.provariadic OPERATOR(pg_catalog.=) 0
             AND procedure.prorettype OPERATOR(pg_catalog.=)
                     'pg_catalog.text'::pg_catalog.regtype
             AND NOT procedure.proretset
             AND procedure.pronargdefaults OPERATOR(pg_catalog.=) 0
             AND procedure.proallargtypes IS NULL
             AND procedure.proargmodes IS NULL
             AND procedure.proargnames OPERATOR(pg_catalog.=)
                     ARRAY['idx']::pg_catalog.text[]
             AND language.lanname OPERATOR(pg_catalog.=) 'plpgsql'
             AND procedure.prosecdef
             AND NOT procedure.proleakproof
             AND NOT procedure.proisstrict
             AND procedure.provolatile OPERATOR(pg_catalog.=) 'v'
             AND procedure.proparallel OPERATOR(pg_catalog.=) 'u'
             AND procedure.prosupport OPERATOR(pg_catalog.=) 0
             AND procedure.proconfig OPERATOR(pg_catalog.=)
                     ARRAY['search_path=pg_catalog, pg_temp']
                         ::pg_catalog.text[]
             AND pg_catalog.md5(procedure.prosrc)
                     OPERATOR(pg_catalog.=)
                     'c2980482b2b93b9e53511146f1fa8b47'
             AND NOT EXISTS (
                 SELECT 1
                 FROM pg_catalog.aclexplode(
                     coalesce(
                         procedure.proacl,
                         pg_catalog.acldefault(
                             'f', procedure.proowner))) acl
                 WHERE (
                     acl.grantee OPERATOR(pg_catalog.=) 0
                     AND acl.privilege_type
                             OPERATOR(pg_catalog.=) 'EXECUTE')
                    OR (
                     acl.grantee OPERATOR(pg_catalog.<>)
                             procedure.proowner
                     AND acl.is_grantable)))
    THEN
        RAISE EXCEPTION
            'existing bm25_request_compaction wrapper is not the managed '
            'definition'
            USING HINT =
                'Inspect and remove the hostile or drifted wrapper manually.';
    END IF;
END
$$;

COMMIT;

-- Commit a real task before replacing the wrapper. This validates that the
-- worker can connect as textsearch_compactor, not merely that SQL objects
-- have the expected catalog shape.
BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;
SET LOCAL ROLE textsearch_compactor;

SELECT pg_catalog.set_config(
    'pg_textsearch_compaction.canary_instance',
    df.start(
        'SELECT 1 AS pg_textsearch_canary',
        label => 'bm25-compaction-canary',
        transaction_mode => 'new'),
    false);

RESET ROLE;
COMMIT;

DO $$
DECLARE
    canary_id      pg_catalog.text :=
        pg_catalog.current_setting(
            'pg_textsearch_compaction.canary_instance');
    instance_state pg_catalog.text;
    node_result    pg_catalog.text;
    deadline       pg_catalog.timestamptz :=
        pg_catalog.clock_timestamp() + interval '60 seconds';
BEGIN
    LOOP
        SELECT instance.status
        INTO instance_state
        FROM df.instances instance
        WHERE instance.id OPERATOR(pg_catalog.=) canary_id;

        EXIT WHEN instance_state IN ('completed', 'failed', 'cancelled');

        IF pg_catalog.clock_timestamp() OPERATOR(pg_catalog.>=) deadline THEN
            RAISE EXCEPTION
                'compactor execution canary % did not finish; status %',
                canary_id,
                coalesce(instance_state, '<missing>')
                USING HINT =
                    'Check pg_durable worker readiness, PGHOST, pg_hba.conf, '
                    'and PostgreSQL logs.';
        END IF;

        PERFORM pg_catalog.pg_sleep(0.25);
    END LOOP;

    IF instance_state OPERATOR(pg_catalog.<>) 'completed' THEN
        SELECT node.result #>> '{}'
        INTO node_result
        FROM df.nodes node
        WHERE node.instance_id OPERATOR(pg_catalog.=) canary_id
          AND node.status OPERATOR(pg_catalog.=) 'failed'
        ORDER BY node.updated_at DESC
        LIMIT 1;

        RAISE EXCEPTION
            'compactor execution canary % failed: %',
            canary_id,
            coalesce(node_result, '<no node diagnostic>')
            USING HINT =
                'Check textsearch_compactor authentication and server logs.';
    END IF;
END
$$;

BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;

DROP FUNCTION IF EXISTS
    public.bm25_request_compaction(pg_catalog.regclass);

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
    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_class relation
             JOIN pg_catalog.pg_am access_method
               ON access_method.oid = relation.relam
        WHERE relation.oid = idx
          AND relation.relkind = 'i'
          AND access_method.amname = 'bm25')
    THEN
        RAISE EXCEPTION '"%" is not a bm25 index', idx::pg_catalog.text;
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_class relation
        WHERE relation.oid = idx
          AND relation.relpersistence = 't')
    THEN
        RAISE EXCEPTION
            'temporary bm25 indexes cannot use background compaction'
            USING ERRCODE = 'feature_not_supported';
    END IF;

    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_index index_catalog
        WHERE index_catalog.indexrelid = idx
          AND (
              pg_catalog.has_table_privilege(
                  session_user, index_catalog.indrelid, 'INSERT')
              OR EXISTS (
                  SELECT 1
                  FROM pg_catalog.pg_partition_ancestors(
                           index_catalog.indrelid) ancestor(relid)
                  WHERE pg_catalog.has_table_privilege(
                            session_user, ancestor.relid, 'INSERT'))))
    THEN
        RAISE EXCEPTION
            'permission denied to request compaction for %', idx
            USING ERRCODE = 'insufficient_privilege';
    END IF;

    SELECT namespace.nspname
    INTO ext_schema
    FROM pg_catalog.pg_extension extension
         JOIN pg_catalog.pg_namespace namespace
           ON namespace.oid = extension.extnamespace
    WHERE extension.extname = 'pg_textsearch';

    IF ext_schema IS NULL THEN
        RAISE EXCEPTION 'extension pg_textsearch is not installed here';
    END IF;

    SELECT idx::pg_catalog.oid::pg_catalog.text,
           database.oid::pg_catalog.text,
           coalesce(
               nullif(relation.reltablespace, 0),
               database.dattablespace)::pg_catalog.text,
           pg_catalog.pg_relation_filenode(idx)::pg_catalog.text
    INTO idx_oid, database_oid, tablespace_oid, relfilenumber
    FROM pg_catalog.pg_class relation
         JOIN pg_catalog.pg_database database
           ON database.datname = pg_catalog.current_database()
    WHERE relation.oid = idx;

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

    RETURN df.start(
        df.loop(body, cond, on_error => 'continue'),
        label            => 'bm25-compact-' || idx_oid,
        transaction_mode => 'new');
END;
$fn$;

ALTER FUNCTION public.bm25_request_compaction(pg_catalog.regclass)
    OWNER TO textsearch_compactor;

REVOKE ALL ON FUNCTION
    public.bm25_request_compaction(pg_catalog.regclass)
FROM PUBLIC CASCADE;

-- A named ALTER DEFAULT PRIVILEGES grant is copied onto CREATE FUNCTION.
-- Remove every non-owner entry before adding the selected writer.
DO $$
DECLARE
    grantee_name pg_catalog.name;
BEGIN
    FOR grantee_name IN
        SELECT DISTINCT role.rolname
        FROM pg_catalog.pg_proc procedure
             CROSS JOIN LATERAL pg_catalog.aclexplode(
                 coalesce(
                     procedure.proacl,
                     pg_catalog.acldefault(
                         'f', procedure.proowner))) acl
             JOIN pg_catalog.pg_roles role ON role.oid = acl.grantee
        WHERE procedure.oid =
                  'public.bm25_request_compaction(pg_catalog.regclass)'
                      ::pg_catalog.regprocedure
          AND acl.grantee <> procedure.proowner
    LOOP
        EXECUTE pg_catalog.format(
            'REVOKE ALL ON FUNCTION '
            'public.bm25_request_compaction(pg_catalog.regclass) '
            'FROM %I CASCADE',
            grantee_name);
    END LOOP;
END
$$;

\if :have_writer
GRANT EXECUTE ON FUNCTION
    public.bm25_request_compaction(pg_catalog.regclass)
TO :"writer";
\endif

-- Keep this body hash synchronized with 01_setup_role.sql.
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_proc procedure
             JOIN pg_catalog.pg_language language
               ON language.oid OPERATOR(pg_catalog.=) procedure.prolang
        WHERE procedure.oid OPERATOR(pg_catalog.=)
                  'public.bm25_request_compaction(pg_catalog.regclass)'
                      ::pg_catalog.regprocedure
          AND procedure.proowner OPERATOR(pg_catalog.=)
                  'textsearch_compactor'::pg_catalog.regrole
          AND procedure.prokind OPERATOR(pg_catalog.=) 'f'
          AND procedure.pronargs OPERATOR(pg_catalog.=) 1
          AND procedure.proargtypes[0] OPERATOR(pg_catalog.=)
                  'pg_catalog.regclass'::pg_catalog.regtype
          AND procedure.prorettype OPERATOR(pg_catalog.=)
                  'pg_catalog.text'::pg_catalog.regtype
          AND NOT procedure.proretset
          AND procedure.pronargdefaults OPERATOR(pg_catalog.=) 0
          AND procedure.proargnames OPERATOR(pg_catalog.=)
                  ARRAY['idx']::pg_catalog.text[]
          AND language.lanname OPERATOR(pg_catalog.=) 'plpgsql'
          AND procedure.prosecdef
          AND NOT procedure.proleakproof
          AND NOT procedure.proisstrict
          AND procedure.provolatile OPERATOR(pg_catalog.=) 'v'
          AND procedure.proparallel OPERATOR(pg_catalog.=) 'u'
          AND procedure.prosupport OPERATOR(pg_catalog.=) 0
          AND procedure.proconfig OPERATOR(pg_catalog.=)
                  ARRAY['search_path=pg_catalog, pg_temp']
                      ::pg_catalog.text[]
          AND pg_catalog.md5(procedure.prosrc)
                  OPERATOR(pg_catalog.=)
                  'c2980482b2b93b9e53511146f1fa8b47'
          AND NOT EXISTS (
              SELECT 1
              FROM pg_catalog.aclexplode(
                  coalesce(
                      procedure.proacl,
                      pg_catalog.acldefault(
                          'f', procedure.proowner))) acl
              WHERE (
                  acl.grantee OPERATOR(pg_catalog.=) 0
                  AND acl.privilege_type OPERATOR(pg_catalog.=) 'EXECUTE')
                 OR (
                  acl.grantee OPERATOR(pg_catalog.<>)
                          procedure.proowner
                  AND acl.is_grantable)))
    THEN
        RAISE EXCEPTION
            'managed bm25_request_compaction wrapper definition mismatch';
    END IF;
END
$$;

COMMIT;

\echo 'bm25_request_compaction() ready.'
