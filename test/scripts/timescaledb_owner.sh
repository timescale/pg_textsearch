#!/usr/bin/env bash
# Copyright (c) 2026 Tiger Data, Inc.
# Licensed under the PostgreSQL License. See LICENSE for details.
# Optional integration test: requires TimescaleDB installed for this PostgreSQL.
set -euo pipefail
PG_CONFIG=${PG_CONFIG:-pg_config}
bindir=$("$PG_CONFIG" --bindir)
work=$(mktemp -d)
cleanup() {
    "$bindir/pg_ctl" -D "$work/data" -m immediate stop >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT
"$bindir/initdb" -D "$work/data" -A trust --no-locale >"$work/init.log" 2>&1
# Loading pg_textsearch last makes it the outer utility hook, as in the HA image.
"$bindir/pg_ctl" -D "$work/data" -l "$work/server.log" \
    -o "-p 55439 -k $work -c listen_addresses='' -c shared_preload_libraries=timescaledb,pg_textsearch" \
    -w start >"$work/start.log" 2>&1 || {
    cat "$work/start.log" "$work/server.log"
    exit 1
}
"$bindir/psql" -X -h "$work" -p 55439 -d postgres -v ON_ERROR_STOP=1 <<'SQL'
\set VERBOSITY verbose
CREATE EXTENSION timescaledb;
-- Reproduce the failure even without CREATE EXTENSION pg_textsearch.
CREATE ROLE owner_target;
CREATE ROLE owner_unrelated;
CREATE TABLE metrics (time timestamptz NOT NULL, value integer);
SELECT create_hypertable('metrics', by_range('time'));
CREATE MATERIALIZED VIEW daily WITH (timescaledb.continuous) AS
  SELECT time_bucket('1 day', time) AS bucket, sum(value)
  FROM metrics GROUP BY 1 WITH NO DATA;
ALTER MATERIALIZED VIEW daily OWNER TO owner_target;
DO $$
DECLARE owners_checked integer;
BEGIN
  SELECT count(*) INTO owners_checked
  FROM _timescaledb_catalog.continuous_agg ca
  JOIN _timescaledb_catalog.hypertable h ON h.id = ca.mat_hypertable_id
  JOIN pg_class c ON c.oid IN (
    format('%I.%I', ca.user_view_schema, ca.user_view_name)::regclass,
    format('%I.%I', ca.partial_view_schema, ca.partial_view_name)::regclass,
    format('%I.%I', ca.direct_view_schema, ca.direct_view_name)::regclass,
    format('%I.%I', h.schema_name, h.table_name)::regclass)
  WHERE ca.user_view_name = 'daily'
    AND c.relowner = 'owner_target'::regrole;
  IF owners_checked <> 4 THEN
    RAISE EXCEPTION 'expected 4 transferred cagg objects, got %', owners_checked;
  END IF;
END $$;
CREATE EXTENSION pg_textsearch;
ALTER MATERIALIZED VIEW daily OWNER TO CURRENT_USER;
CREATE TABLE docs (body text);
CREATE INDEX docs_bm25 ON docs USING bm25 (body) WITH (text_config='english');
CREATE MATERIALIZED VIEW native_daily AS SELECT * FROM metrics;
CREATE VIEW plain_view AS SELECT * FROM docs;
ALTER TABLE docs OWNER TO owner_target;
ALTER MATERIALIZED VIEW native_daily OWNER TO owner_target;
DO $$
BEGIN
  IF (SELECT count(*) FROM pg_class
      WHERE oid IN ('docs'::regclass, 'docs_bm25'::regclass,
                    'native_daily'::regclass)
        AND relowner = 'owner_target'::regrole) <> 3 THEN
    RAISE EXCEPTION 'native relation/index ownership was not transferred';
  END IF;
  BEGIN
    ALTER MATERIALIZED VIEW plain_view OWNER TO owner_target;
    RAISE EXCEPTION 'ordinary view was incorrectly accepted';
  EXCEPTION WHEN wrong_object_type THEN NULL;
  END;
  BEGIN
    ALTER MATERIALIZED VIEW docs OWNER TO owner_target;
    RAISE EXCEPTION 'ordinary table was incorrectly accepted';
  EXCEPTION WHEN wrong_object_type THEN NULL;
  END;
END $$;
SET ROLE owner_unrelated;
DO $$
BEGIN
  BEGIN
    ALTER MATERIALIZED VIEW daily OWNER TO owner_unrelated;
    RAISE EXCEPTION 'unauthorized cagg ownership change was accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;
RESET ROLE;
SQL
printf '%s\n' 'TimescaleDB ownership integration test passed'
