#!/usr/bin/env bash

set -euo pipefail

PG_CONFIG="${PG_CONFIG:-pg_config}"
export PATH="$("$PG_CONFIG" --bindir):$PATH"
export PGDATABASE="${PGDATABASE:-upgrade_test}"

psql -X -v ON_ERROR_STOP=1 <<'SQL'
DO $$
DECLARE
    expected_version text;
    installed_version text;
    loaded_version text;
BEGIN
    SELECT default_version INTO expected_version
    FROM pg_available_extensions WHERE name = 'pg_textsearch';
    SELECT extversion INTO installed_version
    FROM pg_extension WHERE extname = 'pg_textsearch';
    loaded_version := current_setting('pg_textsearch.library_version', true);
    IF expected_version IS NULL OR
       installed_version IS DISTINCT FROM expected_version OR
       loaded_version IS DISTINCT FROM expected_version THEN
        RAISE EXCEPTION
            'Version mismatch: expected %, installed %, loaded %',
            expected_version, installed_version, loaded_version;
    END IF;
END $$;
SQL

extension_schema="$(psql -XAt -v ON_ERROR_STOP=1 -c \
    "SELECT n.nspname FROM pg_extension e
     JOIN pg_namespace n ON n.oid = e.extnamespace
     WHERE e.extname = 'pg_textsearch';")"
temp_dir="$(mktemp -d)"
reference_db="pg_textsearch_upgrade_reference_$$"
reference_created=0
cleanup() {
    local status=$?
    trap - EXIT
    if [ "$reference_created" -eq 1 ]; then
        dropdb "$reference_db" || status=1
    fi
    rm -f "$temp_dir/upgraded.objects" "$temp_dir/clean.objects" || status=1
    rmdir "$temp_dir" || status=1
    exit "$status"
}
trap cleanup EXIT

createdb --template=template0 "$reference_db"
reference_created=1
psql -X -v ON_ERROR_STOP=1 -v extension_schema="$extension_schema" \
    -d "$reference_db" <<'SQL'
CREATE SCHEMA IF NOT EXISTS :"extension_schema";
CREATE EXTENSION pg_textsearch WITH SCHEMA :"extension_schema";
SQL

catalog_query="$(cat <<'SQL'
SELECT pg_describe_object(d.classid, d.objid, d.objsubid),
       CASE WHEN p.oid IS NOT NULL THEN
           jsonb_build_object(
               'result', pg_get_function_result(p.oid),
               'language', l.lanname, 'strict', p.proisstrict,
               'volatility', p.provolatile, 'parallel', p.proparallel,
               'cost', p.procost, 'rows', p.prorows, 'symbol', p.prosrc)
       END
FROM pg_depend d
LEFT JOIN pg_proc p ON d.classid = 'pg_proc'::regclass AND p.oid = d.objid
LEFT JOIN pg_language l ON l.oid = p.prolang
WHERE d.refclassid = 'pg_extension'::regclass
  AND d.refobjid = (SELECT oid FROM pg_extension
                   WHERE extname = 'pg_textsearch')
  AND d.deptype = 'e'
ORDER BY 1, 2;
SQL
)"
psql -XAt -v ON_ERROR_STOP=1 -c "$catalog_query" \
    >"$temp_dir/upgraded.objects"
psql -XAt -v ON_ERROR_STOP=1 -d "$reference_db" -c "$catalog_query" \
    >"$temp_dir/clean.objects"
diff -u "$temp_dir/clean.objects" "$temp_dir/upgraded.objects"

psql -X -v ON_ERROR_STOP=1 -v extension_schema="$extension_schema" <<'SQL'
BEGIN;
SET LOCAL search_path TO :"extension_schema", pg_catalog;
SET LOCAL enable_indexscan = off;
SET LOCAL enable_bitmapscan = off;
CREATE TEMP TABLE upgrade_verify_docs (
    tenant int, id int, body text, tags text[]
) PARTITION BY LIST (tenant);
CREATE TEMP TABLE upgrade_verify_one PARTITION OF upgrade_verify_docs
    FOR VALUES IN (1);
CREATE TEMP TABLE upgrade_verify_two PARTITION OF upgrade_verify_docs
    FOR VALUES IN (2);
INSERT INTO upgrade_verify_docs
SELECT 1, g, body, ARRAY[body]
FROM (
    SELECT g, CASE WHEN g = 1 THEN 'apple orchard'
                  ELSE 'banana orchard' END AS body
    FROM generate_series(1, 10) g
) s;
INSERT INTO upgrade_verify_docs
SELECT 2, g, 'apple orchard', ARRAY['apple orchard']
FROM generate_series(1, 10) g;
CREATE INDEX upgrade_verify_text ON upgrade_verify_docs USING bm25 (body)
    WITH (text_config = 'english');
CREATE INDEX upgrade_verify_array ON upgrade_verify_docs USING bm25 (tags)
    WITH (text_config = 'english');

-- Standalone parent scoring requires both planner-only overloads.
DO $$
DECLARE
    correct boolean;
BEGIN
    SELECT count(*) = 2 AND bool_and((abs(
        (body <@> to_bm25query('apple', 'upgrade_verify_text')) +
        CASE WHEN tenant = 1 THEN ln(1 + 9.5 / 1.5)
             ELSE ln(1 + 0.5 / 10.5) END) < 0.000001) IS TRUE)
    INTO correct FROM upgrade_verify_docs WHERE id = 1;
    IF correct IS DISTINCT FROM true THEN
        RAISE EXCEPTION 'Partition-local text scoring failed after upgrade';
    END IF;

    SELECT count(*) = 2 AND bool_and((abs(
        (tags <@> to_bm25query('apple', 'upgrade_verify_array')) +
        CASE WHEN tenant = 1 THEN ln(1 + 9.5 / 1.5)
             ELSE ln(1 + 0.5 / 10.5) END) < 0.000001) IS TRUE)
    INTO correct FROM upgrade_verify_docs WHERE id = 1;
    IF correct IS DISTINCT FROM true THEN
        RAISE EXCEPTION 'Partition-local text[] scoring failed after upgrade';
    END IF;
END $$;
ROLLBACK;
SQL

echo "Upgrade versions, catalog parity, and partition scoring verified."
