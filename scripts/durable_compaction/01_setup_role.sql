-- 01_setup_role.sql -- create the textsearch_compactor role.
--
-- NOTE ON THE NAME: this role is called textsearch_compactor, not
-- pg_textsearch_compactor.  PostgreSQL reserves the entire "pg_"
-- role namespace, so CREATE ROLE pg_textsearch_compactor fails
-- outright with "role name ... is reserved".
--
-- Run as a superuser, in the database named by pg_durable.database
-- (that is the only database in which the df schema exists, and
-- df.start() must run in the writer's own transaction, so the BM25
-- indexes have to live there too).
--
--   psql -d postgres -f 01_setup_role.sql -v index_owner=app_owner
--
-- If -v index_owner=... is omitted the current user is used.
--
-- Requirements this script encodes, each enforced by pg_durable:
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
--   * INHERIT plus membership in the index owner's role.  The
--     compactor can never *be* the index owner, but bm25_compact()
--     and bm25_compact_step() gate on object_ownercheck(), which
--     resolves through has_privs_of_role() -- so inherited
--     membership is sufficient.  bm25_indexes_needing_compaction()
--     likewise filters on pg_has_role(relowner, 'USAGE'), so without
--     this grant the backstop sweep simply sees no indexes.
--
--   * pg_hba.conf must let this role connect WITHOUT a password.
--     connect_as_user() builds PgConnectOptions from username,
--     database and port only -- no password and no host, so it uses
--     the unix socket, where `peer` authentication fails for a role
--     that is not the OS user.  A `trust` line (or any
--     password-free method such as an ident map that resolves to
--     this role) is required, for example:
--
--       local   all   textsearch_compactor   trust
--       host    all   textsearch_compactor   127.0.0.1/32   trust
--
--     Placed above the general rules, and followed by
--     `SELECT pg_reload_conf();`.  Restricting the entry to this one
--     role keeps the blast radius small; a production deployment
--     would prefer a peer/ident map over trust.

\set ON_ERROR_STOP on

\if :{?index_owner}
\else
\set index_owner ''
\endif

SELECT coalesce(nullif(:'index_owner', ''), current_user) AS index_owner
\gset

-- LOGIN: required by require_login_privilege().
-- NOSUPERUSER: superuser submission is blocked by default.
-- INHERIT: the owner-role membership granted below must be automatic,
--          because the worker never issues SET ROLE.
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

-- Grants schema USAGE on df plus the INSERT/SELECT privileges on
-- df.instances and df.nodes that df.start() needs.  include_http is
-- left false: compaction never makes HTTP calls.
SELECT df.grant_usage('textsearch_compactor');

-- The compactor runs as itself, not as the writer, so it needs the
-- index owner's privileges on the heap and index it compacts.
GRANT :"index_owner" TO textsearch_compactor;

-- The compactor must be able to reach the pg_textsearch functions.
-- Adjust if the extension does not live in public.
GRANT USAGE ON SCHEMA public TO textsearch_compactor;

\echo 'textsearch_compactor ready.'
