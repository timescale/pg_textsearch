-- 01_setup_role.sql -- create the textsearch_compactor role.
--
-- NOTE ON THE NAME: this role is called textsearch_compactor, not
-- pg_textsearch_compactor.  PostgreSQL reserves the entire "pg_"
-- role namespace, so CREATE ROLE pg_textsearch_compactor fails
-- outright with "role name ... is reserved".
--
-- Run as a superuser, in the database named by pg_durable.database
-- (that is the only database in which the df schema exists, and
-- transaction_mode => 'new' reconnects there to persist the request,
-- so the BM25 indexes have to live there too).
--
--   psql -d postgres -f 01_setup_role.sql -v index_owner=app_owner
--
-- index_owner is required and must name an existing non-superuser role.
--
-- Requirements this script encodes for pg_durable:
--
--   * LOGIN is mandatory.  The background worker literally opens a
--     new libpq connection as df.instances.submitted_by to run every
--     SQL node (connect_as_user(), pg_durable src/types.rs:233), and
--     df.start() rejects a role that cannot log in up front
--     (require_login_privilege(), pg_durable src/dsl.rs:873).
--
--   * NOSUPERUSER is mandatory.  A superuser may not submit
--     instances unless pg_durable.enable_superuser_instances = on
--     (pg_durable src/dsl.rs:1118), and making the compactor a
--     superuser to dodge a permission error is explicitly off the
--     table.
--
--   * INHERIT plus membership in an explicit, non-superuser index
--     owner role.  bm25_compact() and bm25_compact_step() gate on
--     object_ownercheck(), which resolves through has_privs_of_role(),
--     so inherited membership is sufficient.  The membership is
--     granted with SET FALSE: the worker receives the owner's object
--     privileges but cannot become that role explicitly.
--
--   * pg_hba.conf must let this role connect WITHOUT a password.
--     pg_durable defaults PGHOST to 127.0.0.1, so peer authentication
--     requires setting PGHOST to the Unix socket directory in the
--     PostgreSQL service environment before server start.  Production
--     deployments should authenticate the server's OS account with a
--     peer map, for example:
--
--       # pg_ident.conf
--       pg_durable_compactor  <server-os-user>  textsearch_compactor
--
--       # pg_hba.conf
--       local  all  textsearch_compactor  peer map=pg_durable_compactor
--
--     Place the HBA entry above general rules, set PGHOST before
--     restarting PostgreSQL, and verify it is visible to the server.
--     The integration test uses local trust only inside its throwaway
--     cluster.
--
--   * Untrusted roles must not have CREATE on the wrapper schema.
--     This script rejects PUBLIC CREATE on public; revoke CREATE from
--     any other untrusted roles before installing the wrapper.
--
--   * Reusing textsearch_compactor is allowed only when it remains a
--     dedicated passwordless role with no privileged attributes, custom
--     connection limit, unsafe search_path/identity defaults, or unexpected
--     memberships. Failed validation happens before owner or df grants.

\set ON_ERROR_STOP on

BEGIN;

SET LOCAL search_path = pg_catalog, pg_temp;

-- Fail before changing roles or privileges when pg_durable is too old or its
-- operator-facing grant helper does not match the 0.2.6 contract.
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
                      'df.grant_usage(pg_catalog.text,pg_catalog.bool,'
                      'pg_catalog.bool)')
          AND procedure.prorettype OPERATOR(pg_catalog.=)
                  'pg_catalog.void'::pg_catalog.regtype
          AND procedure.pronargdefaults OPERATOR(pg_catalog.=) 2)
    THEN
        RAISE EXCEPTION
            'pg_durable 0.2.6 API is incomplete: required extension member '
            'df.grant_usage(text, boolean, boolean) with two defaults'
            USING HINT =
                'Install or upgrade pg_durable, then rerun this script.';
    END IF;
END
$$;

\if :{?index_owner}
\else
\echo 'index_owner is required'
\set index_owner ''
\endif

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
           WHERE rolname = :'index_owner'
       ) AS index_owner_exists,
       coalesce((
           SELECT rolsuper
           FROM pg_catalog.pg_roles
           WHERE rolname = :'index_owner'
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
           FROM pg_catalog.pg_namespace schema
                CROSS JOIN LATERAL pg_catalog.aclexplode(
                    coalesce(
                        schema.nspacl,
                        pg_catalog.acldefault('n', schema.nspowner))) acl
           WHERE schema.nspname = 'public'
             AND acl.grantee = 0
             AND acl.privilege_type = 'CREATE'
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

-- LOGIN: required by require_login_privilege().
-- NOSUPERUSER: superuser submission is blocked by default.
-- INHERIT: the owner-role membership granted below must be automatic,
--          because the worker never issues SET ROLE.
DO $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_authid
        WHERE rolname = 'textsearch_compactor'
          AND rolpassword IS NOT NULL)
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role must not have credentials';
    END IF;
END
$$;

-- Only settings that can redirect name resolution or session identity are
-- rejected. Resource and timeout settings are not privilege escalations.
DO $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_db_role_setting setting
             CROSS JOIN LATERAL
                 pg_catalog.unnest(setting.setconfig) config(value)
        WHERE setting.setrole = (
                  SELECT oid
                  FROM pg_catalog.pg_roles
                  WHERE rolname = 'textsearch_compactor')
          AND setting.setdatabase IN (
                  0,
                  (SELECT oid
                   FROM pg_catalog.pg_database
                   WHERE datname = pg_catalog.current_database()))
          AND pg_catalog.split_part(config.value, '=', 1) IN
                  ('search_path', 'role', 'session_authorization'))
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role has unsafe role/database '
            'settings';
    END IF;
END
$$;

-- A prior successful run leaves direct, inherited, non-SET memberships
-- granted by a superuser. Preserve only that exact operator-created shape.
DO $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_auth_members member
             JOIN pg_catalog.pg_roles grantor
               ON grantor.oid = member.grantor
        WHERE member.member = (
                  SELECT oid
                  FROM pg_catalog.pg_roles
                  WHERE rolname = 'textsearch_compactor')
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

DO $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_authid
        WHERE rolname = 'textsearch_compactor'
          AND rolconnlimit <> -1)
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role must use the default '
            'connection limit';
    END IF;
END
$$;

DO $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_authid
        WHERE rolname = 'textsearch_compactor'
          AND (rolsuper
               OR rolcreatedb
               OR rolcreaterole
               OR rolreplication
               OR rolbypassrls))
    THEN
        RAISE EXCEPTION
            'existing textsearch_compactor role has privileged attributes';
    END IF;
END
$$;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM pg_catalog.pg_roles
        WHERE rolname = 'textsearch_compactor')
    THEN
        CREATE ROLE textsearch_compactor LOGIN NOSUPERUSER INHERIT;
    ELSE
        ALTER ROLE textsearch_compactor LOGIN NOSUPERUSER INHERIT;
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
END
$$;

DO $$
BEGIN
    IF EXISTS (
        SELECT 1
        FROM pg_catalog.pg_auth_members member
        WHERE member.roleid = (
            SELECT oid
            FROM pg_catalog.pg_roles
            WHERE rolname = 'textsearch_compactor'))
    THEN
        RAISE EXCEPTION
            'textsearch_compactor must not be granted to another role';
    END IF;
END
$$;

-- Grants schema USAGE on df plus the INSERT/SELECT privileges on
-- df.instances and df.nodes that df.start() needs.  include_http is
-- left false: compaction never makes HTTP calls.
SELECT df.grant_usage('textsearch_compactor');

-- The worker inherits the owner's privileges, but cannot become that
-- role explicitly.  Reject another grantor or membership path that
-- leaves SET ROLE available.
GRANT :"index_owner" TO textsearch_compactor
    WITH INHERIT TRUE, SET FALSE;

-- The owner grant can introduce a new transitive superuser path.
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
END
$$;

SELECT pg_catalog.pg_has_role(
           'textsearch_compactor',
           :'index_owner',
           'SET') AS compactor_can_set_owner
\gset

\if :compactor_can_set_owner
DO $$
BEGIN
    RAISE EXCEPTION
        'textsearch_compactor must not be able to SET ROLE to index_owner';
END
$$;
\endif

-- Grant only the pg_textsearch entry points used by the per-index
-- request and the periodic backstop.  Resolve the extension schema so
-- this remains correct when pg_textsearch is not installed in public.
SELECT n.nspname AS ext_schema
FROM pg_catalog.pg_extension e
     JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace
WHERE e.extname = 'pg_textsearch'
\gset

GRANT USAGE ON SCHEMA :"ext_schema" TO textsearch_compactor;
GRANT EXECUTE ON FUNCTION
    :"ext_schema".bm25_compact(regclass),
    :"ext_schema".bm25_compact_step(regclass),
    :"ext_schema".bm25_compact_step_if_current(oid, oid, oid, oid),
    :"ext_schema".bm25_level_counts(regclass),
    :"ext_schema".bm25_needs_compaction(regclass),
    :"ext_schema".bm25_needs_compaction_if_current(oid, oid, oid, oid),
    :"ext_schema".bm25_indexes_needing_compaction(),
    :"ext_schema".bm25_compact_pending()
TO textsearch_compactor;

COMMIT;

\echo 'textsearch_compactor ready.'
