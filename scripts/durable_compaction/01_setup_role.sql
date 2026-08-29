-- Create or validate the dedicated pg_durable compactor role.
--
-- Run as a superuser in pg_durable.database:
--
--   psql -f 01_setup_role.sql -v index_owner=app_owner
--
-- Cluster authentication remains an operator responsibility. The role is
-- deliberately passwordless and must connect through a scoped peer/ident
-- rule or an equivalent passwordless authenticated route.

\set ON_ERROR_STOP on

\if :{?index_owner}
\else
\set index_owner ''
\endif

BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;

-- Keep the unreleased failure-policy dependency in this one preflight block.
-- Replace the capability-only error with a version floor once the release
-- containing microsoft/pg_durable#354 is available.
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
            'pg_durable with the df.start() failure policy is required'
            USING HINT =
                'Install the release containing microsoft/pg_durable#354, '
                'then rerun this script.';
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
                      'df.grant_usage(pg_catalog.text,pg_catalog.bool,'
                      'pg_catalog.bool)')
          AND procedure.prorettype OPERATOR(pg_catalog.=)
                  'pg_catalog.void'::pg_catalog.regtype
          AND procedure.pronargdefaults OPERATOR(pg_catalog.=) 2)
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
                      'df.start(pg_catalog.text,pg_catalog.text,'
                      'pg_catalog.text,pg_catalog.text,pg_catalog.int4,'
                      'pg_catalog.interval,pg_catalog.text)')
          AND procedure.prorettype OPERATOR(pg_catalog.=)
                  'pg_catalog.text'::pg_catalog.regtype
          AND procedure.pronargdefaults OPERATOR(pg_catalog.=) 6
          AND procedure.proargnames OPERATOR(pg_catalog.=)
                  ARRAY[
                      'fut',
                      'label',
                      'database',
                      'transaction_mode',
                      'max_attempts',
                      'max_backoff',
                      'on_failure']::pg_catalog.text[])
    THEN
        RAISE EXCEPTION
            'pg_durable API is incomplete: df.grant_usage() and '
            'df.start() with the node failure policy are required'
            USING HINT =
                'Install the release containing microsoft/pg_durable#354, '
                'then rerun this script.';
    END IF;
END
$$;

SELECT nullif(:'index_owner', '') IS NOT NULL AS have_index_owner
\gset

\if :have_index_owner
\else
DO $$
BEGIN
    RAISE EXCEPTION 'index_owner is required';
END
$$;
\endif

SELECT EXISTS (
           SELECT 1
           FROM pg_catalog.pg_roles
           WHERE rolname OPERATOR(pg_catalog.=) :'index_owner'
       ) AS index_owner_exists,
       coalesce((
           SELECT rolsuper
           FROM pg_catalog.pg_roles
           WHERE rolname OPERATOR(pg_catalog.=) :'index_owner'
       ), false) AS index_owner_is_superuser
\gset

\if :index_owner_exists
\else
DO $$
BEGIN
    RAISE EXCEPTION 'index_owner role does not exist';
END
$$;
\endif

\if :index_owner_is_superuser
DO $$
BEGIN
    RAISE EXCEPTION 'index_owner must not be a superuser';
END
$$;
\endif

SELECT EXISTS (
           SELECT 1
           FROM pg_catalog.pg_namespace namespace
                CROSS JOIN LATERAL pg_catalog.aclexplode(
                    coalesce(
                        namespace.nspacl,
                        pg_catalog.acldefault(
                            'n', namespace.nspowner))) acl
           WHERE namespace.nspname OPERATOR(pg_catalog.=) 'public'
             AND acl.grantee OPERATOR(pg_catalog.=) 0
             AND acl.privilege_type OPERATOR(pg_catalog.=) 'CREATE'
       ) AS public_can_create_wrapper
\gset

\if :public_can_create_wrapper
DO $$
BEGIN
    RAISE EXCEPTION
        'PUBLIC must not have CREATE on wrapper schema public';
END
$$;
\endif

-- Validate every security-relevant property before changing memberships or
-- granting access. Existing hostile state is never normalized implicitly.
DO $$
DECLARE
    compactor_oid pg_catalog.oid;
BEGIN
    SELECT oid
    INTO compactor_oid
    FROM pg_catalog.pg_roles
    WHERE rolname OPERATOR(pg_catalog.=) 'textsearch_compactor';

    IF compactor_oid IS NULL THEN
        RETURN;
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_authid
        WHERE oid OPERATOR(pg_catalog.=) compactor_oid
          AND rolpassword IS NOT NULL)
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role must not have credentials';
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_authid
        WHERE oid OPERATOR(pg_catalog.=) compactor_oid
          AND (NOT rolcanlogin
               OR NOT rolinherit
               OR rolsuper
               OR rolcreatedb
               OR rolcreaterole
               OR rolreplication
               OR rolbypassrls
               OR rolconnlimit OPERATOR(pg_catalog.<>) -1))
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role has unsafe attributes';
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_db_role_setting setting
             CROSS JOIN LATERAL
                 pg_catalog.unnest(setting.setconfig) config(value)
        WHERE setting.setrole OPERATOR(pg_catalog.=) compactor_oid
          AND setting.setdatabase IN (
                  0,
                  (SELECT oid
                   FROM pg_catalog.pg_database
                   WHERE datname OPERATOR(pg_catalog.=)
                           pg_catalog.current_database()))
          AND pg_catalog.split_part(config.value, '=', 1) IN
                  ('search_path', 'role', 'session_authorization'))
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role has unsafe settings';
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_auth_members member
             JOIN pg_catalog.pg_roles grantor
               ON grantor.oid OPERATOR(pg_catalog.=) member.grantor
        WHERE member.member OPERATOR(pg_catalog.=) compactor_oid
          AND (member.admin_option
               OR NOT member.inherit_option
               OR member.set_option
               OR NOT grantor.rolsuper))
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role has unexpected memberships';
    END IF;
END
$$;

-- pg_shdepend is cluster-wide. The exact managed wrapper in this database is
-- the only object the role may own on a clean rerun.
DO $$
DECLARE
    compactor_oid pg_catalog.oid;
    database_oid  pg_catalog.oid;
    wrapper_oid   pg_catalog.oid;
BEGIN
    SELECT oid
    INTO compactor_oid
    FROM pg_catalog.pg_roles
    WHERE rolname OPERATOR(pg_catalog.=) 'textsearch_compactor';

    IF compactor_oid IS NULL THEN
        RETURN;
    END IF;

    SELECT oid
    INTO database_oid
    FROM pg_catalog.pg_database
    WHERE datname OPERATOR(pg_catalog.=) pg_catalog.current_database();

    wrapper_oid := pg_catalog.to_regprocedure(
        'public.bm25_request_compaction(pg_catalog.regclass)');

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_shdepend dependency
        WHERE dependency.refclassid OPERATOR(pg_catalog.=)
                  'pg_catalog.pg_authid'::pg_catalog.regclass
          AND dependency.refobjid OPERATOR(pg_catalog.=) compactor_oid
          AND dependency.deptype OPERATOR(pg_catalog.=) 'o'
          AND NOT (
              wrapper_oid IS NOT NULL
              AND dependency.dbid OPERATOR(pg_catalog.=) database_oid
              AND dependency.classid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_proc'::pg_catalog.regclass
              AND dependency.objid OPERATOR(pg_catalog.=) wrapper_oid
              AND dependency.objsubid OPERATOR(pg_catalog.=) 0
              AND EXISTS (
                  SELECT 1
                  FROM pg_catalog.pg_proc procedure
                       JOIN pg_catalog.pg_namespace namespace
                         ON namespace.oid OPERATOR(pg_catalog.=)
                                procedure.pronamespace
                       JOIN pg_catalog.pg_language language
                         ON language.oid OPERATOR(pg_catalog.=)
                                procedure.prolang
                  WHERE procedure.oid OPERATOR(pg_catalog.=) wrapper_oid
                    AND namespace.nspname OPERATOR(pg_catalog.=) 'public'
                    AND procedure.proname OPERATOR(pg_catalog.=)
                            'bm25_request_compaction'
                    AND procedure.proowner OPERATOR(pg_catalog.=)
                            compactor_oid
                    AND procedure.prokind OPERATOR(pg_catalog.=) 'f'
                    AND procedure.pronargs OPERATOR(pg_catalog.=) 1
                    AND procedure.proargtypes[0]
                            OPERATOR(pg_catalog.=)
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
                            '39b927569e6b4a2d24e1999627993a6d'
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
          ))
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role owns unexpected objects'
            USING HINT =
                'Drop or reassign every object except the exact managed '
                'public.bm25_request_compaction(regclass) wrapper.';
    END IF;
END
$$;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_roles
        WHERE rolname OPERATOR(pg_catalog.=) 'textsearch_compactor')
    THEN
        CREATE ROLE textsearch_compactor
            LOGIN NOSUPERUSER INHERIT NOCREATEDB NOCREATEROLE
            NOREPLICATION NOBYPASSRLS CONNECTION LIMIT -1 PASSWORD NULL;
    END IF;
END
$$;

DO $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_roles granted_role
        WHERE granted_role.rolsuper
          AND pg_catalog.pg_has_role(
                  'textsearch_compactor',
                  granted_role.oid,
                  'MEMBER'))
    THEN
        RAISE EXCEPTION
            'textsearch_compactor must not belong to a superuser role';
    END IF;

    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_auth_members member
        WHERE member.roleid OPERATOR(pg_catalog.=)
                  'textsearch_compactor'::pg_catalog.regrole)
    THEN
        RAISE EXCEPTION
            'textsearch_compactor must not be granted to another role';
    END IF;
END
$$;

SELECT df.grant_usage('textsearch_compactor');

GRANT :"index_owner" TO textsearch_compactor
    WITH INHERIT TRUE, SET FALSE;

SELECT pg_catalog.pg_has_role(
           'textsearch_compactor',
           :'index_owner',
           'SET') AS compactor_can_set_owner
\gset

\if :compactor_can_set_owner
DO $$
BEGIN
    RAISE EXCEPTION
        'textsearch_compactor must not SET ROLE to index_owner';
END
$$;
\endif

DO $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_roles granted_role
        WHERE granted_role.rolsuper
          AND pg_catalog.pg_has_role(
                  'textsearch_compactor',
                  granted_role.oid,
                  'MEMBER'))
    THEN
        RAISE EXCEPTION
            'index_owner creates a transitive superuser membership';
    END IF;
END
$$;

SELECT namespace.nspname AS ext_schema
FROM pg_catalog.pg_extension extension
     JOIN pg_catalog.pg_namespace namespace
       ON namespace.oid OPERATOR(pg_catalog.=) extension.extnamespace
WHERE extension.extname OPERATOR(pg_catalog.=) 'pg_textsearch'
\gset

GRANT USAGE ON SCHEMA :"ext_schema" TO textsearch_compactor;
GRANT EXECUTE ON FUNCTION
    :"ext_schema".bm25_compact_step_if_current(oid, oid, oid, oid),
    :"ext_schema".bm25_needs_compaction_if_current(oid, oid, oid, oid)
TO textsearch_compactor;

COMMIT;

\echo 'textsearch_compactor ready.'
