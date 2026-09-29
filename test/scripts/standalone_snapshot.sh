#!/bin/bash
#
# Standalone scoring caches IDF across heap rows. Cache-hit rows must not
# recapture every published segment root.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
export PATH="${PGBINDIR}:${PATH}"
TEST_PORT=55462
TEST_DB=standalone_snapshot_test
DATA_DIR="${SCRIPT_DIR}/../tmp_standalone_snapshot"
SOCKET_DIR="/tmp/pgts_standalone_snapshot"
LOGFILE="${DATA_DIR}/postgres.log"

cleanup() {
    local exit_code=$?

    trap - EXIT INT TERM
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate >/dev/null 2>&1 || true
    fi
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    exit "${exit_code}"
}

trap cleanup EXIT INT TERM

rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
mkdir -p "${DATA_DIR}" "${SOCKET_DIR}"

initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
    >/dev/null 2>&1
cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
unix_socket_directories = '${SOCKET_DIR}'
listen_addresses = ''
shared_preload_libraries = 'pg_textsearch'
autovacuum = off
EOF

if ! pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null; then
    cat "${LOGFILE}" >&2
    exit 1
fi
createdb -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"

PSQL=(
    psql
    -h "${SOCKET_DIR}"
    -p "${TEST_PORT}"
    -d "${TEST_DB}"
    -qAt
    -v ON_ERROR_STOP=1
)

"${PSQL[@]}" <<'SQL' >/dev/null
CREATE EXTENSION pg_textsearch;
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;

CREATE TABLE docs (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    body text NOT NULL
);
CREATE INDEX docs_bm25 ON docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');

DO $$
BEGIN
    FOR batch IN 1..8 LOOP
        INSERT INTO docs (body)
        SELECT CASE
                   WHEN batch = 8 AND gs IN (49, 50)
                       THEN (
                           SELECT string_agg(
                                      'cacheterm' || term_no::text, ' ')
                           FROM generate_series(1, 65) term_no
                       )
                   ELSE 'haystack standalone document ' || batch || ' ' || gs
               END
        FROM generate_series(1, 50) gs;
        PERFORM bm25_spill_index('docs_bm25');
    END LOOP;
END
$$;
SQL

graph=$(
    "${PSQL[@]}" -c \
        "SELECT bm25_level_counts('docs_bm25'::regclass)::text;"
)
if [ "${graph}" != "{8,0,0,0,0,0,0,0}" ]; then
    echo "standalone snapshot fixture built graph ${graph}, expected eight L0 segments" \
        >&2
    exit 1
fi

matches=$(
    "${PSQL[@]}" <<'SQL'
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET jit = off;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 1;
SELECT string_agg('cacheterm' || term_no::text, ' ') AS query_text
FROM generate_series(1, 65) term_no \gset
SELECT count(*)
FROM docs
WHERE body <@> to_bm25query(:'query_text', 'docs_bm25') < 0;
SELECT pg_sleep(0.1);
SQL
)
matches="$(head -1 <<<"${matches}")"
if [ "${matches}" != "2" ]; then
    echo "standalone scoring returned ${matches}/2 rows" >&2
    exit 1
fi

snapshot_count=$(
    grep -c \
        'pg_textsearch segment graph snapshot pause at after-unlock' \
        "${LOGFILE}" || true
)
if [ "${snapshot_count}" -ne 1 ]; then
    echo "standalone scoring captured ${snapshot_count} graph snapshots; expected 1" \
        >&2
    exit 1
fi

# Capturing exactly once says nothing about what the single root walk
# aggregated.  A walk that undercounts segment document frequency still
# caches every term, emits one marker and returns negative scores, so
# compare the standalone scores against index scan scores over the same
# eight-segment graph.
score_matches=$(
    "${PSQL[@]}" <<'SQL'
SET jit = off;
SELECT string_agg('cacheterm' || term_no::text, ' ') AS query_text
FROM generate_series(1, 65) term_no \gset
CREATE TEMP TABLE idx_scores AS
SELECT id,
       round((body <@> to_bm25query(:'query_text', 'docs_bm25'))::numeric,
             6) AS score
FROM docs
ORDER BY body <@> to_bm25query(:'query_text', 'docs_bm25')
LIMIT 2;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE standalone_scores AS
SELECT id,
       round((body <@> to_bm25query(:'query_text', 'docs_bm25'))::numeric,
             6) AS score
FROM docs
WHERE body <@> to_bm25query(:'query_text', 'docs_bm25') < 0;
SELECT count(*) FROM idx_scores JOIN standalone_scores USING (id, score);
SQL
)
score_matches="$(tail -1 <<<"${score_matches}")"
if [ "${score_matches}" != "2" ]; then
    echo "standalone scores matched index scan scores for ${score_matches}/2 rows" \
        >&2
    exit 1
fi

"${PSQL[@]}" <<'SQL' >/dev/null
CREATE TABLE inherited_docs (
    id bigint NOT NULL,
    body text NOT NULL
);
CREATE TABLE inherited_docs_child () INHERITS (inherited_docs);
INSERT INTO inherited_docs_child (id, body)
VALUES (1, 'inheritcache alpha'), (2, 'inheritcache beta');
CREATE INDEX inherited_docs_bm25
    ON inherited_docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');
CREATE INDEX inherited_docs_child_bm25
    ON inherited_docs_child USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');
SQL

inherit_snapshot_before=$(
    grep -c \
        'pg_textsearch segment graph snapshot pause at after-unlock' \
        "${LOGFILE}" || true
)
inherit_matches=$(
    "${PSQL[@]}" <<'SQL'
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET jit = off;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 1;
SELECT count(*)
FROM inherited_docs
WHERE body <@>
          to_bm25query('inheritcache', 'inherited_docs_bm25') < 0;
SELECT pg_sleep(0.1);
SQL
)
inherit_matches="$(head -1 <<<"${inherit_matches}")"
if [ "${inherit_matches}" != "2" ]; then
    echo "inherited standalone scoring returned ${inherit_matches}/2 rows" >&2
    exit 1
fi

inherit_snapshot_after=$(
    grep -c \
        'pg_textsearch segment graph snapshot pause at after-unlock' \
        "${LOGFILE}" || true
)
inherit_snapshot_count=$((inherit_snapshot_after - inherit_snapshot_before))
if [ "${inherit_snapshot_count}" -ne 2 ]; then
    echo "inherited scoring captured ${inherit_snapshot_count} snapshots; expected parent and child" \
        >&2
    exit 1
fi

# A committed INSERT must not split an Incremental Sort tie group or
# change standalone scores, including a term first scored after the INSERT.
"${PSQL[@]}" <<'SQL'
CREATE EXTENSION dblink;
CREATE TABLE stable_docs (id int PRIMARY KEY, body text);
CREATE INDEX stable_docs_idx ON stable_docs USING bm25(body)
    WITH (text_config = 'simple');
CREATE SEQUENCE stable_row;
CREATE FUNCTION stable_insert_at(n bigint) RETURNS boolean
LANGUAGE plpgsql VOLATILE AS $$
BEGIN
    IF nextval('stable_row') = n THEN
        PERFORM dblink_exec(
            format('host=%s port=%s dbname=%s user=%s',
                   current_setting('unix_socket_directories'),
                   current_setting('port'), current_database(), current_user),
            'INSERT INTO stable_docs VALUES (4001, ''beta'')');
    END IF;
    RETURN true;
END
$$;
SQL

for use_cache in off on; do
    for use_index in off on; do
        for insert_at in 0 1 500 1500; do
            "${PSQL[@]}" -v use_cache="${use_cache}" \
                -v use_index="${use_index}" -v insert_at="${insert_at}" \
                <<'SQL' >/dev/null
SET jit = off;
SET statement_timeout = '30s';
SET pg_textsearch.memtable_cache_enabled = :'use_cache';
SET client_min_messages = warning;
SET enable_indexscan = :'use_index';
SELECT set_config('enable_seqscan',
                  CASE WHEN :'use_index' = 'on' THEN 'off' ELSE 'on' END,
                  false);
TRUNCATE stable_docs;
INSERT INTO stable_docs
SELECT g, 'alpha beta' FROM generate_series(3000, 1, -1) g;
SELECT setval('stable_row', 1, false);
CREATE TEMP TABLE stable_result AS
SELECT array_agg(id ORDER BY id) AS ids
FROM (
    SELECT id, body <@> to_bm25query('alpha', 'stable_docs_idx') AS score
    FROM stable_docs WHERE stable_insert_at(:insert_at)
    ORDER BY score, id LIMIT 10
) s;
DO $$
BEGIN
    IF (SELECT ids FROM stable_result)
       IS DISTINCT FROM ARRAY[1,2,3,4,5,6,7,8,9,10] THEN
        RAISE EXCEPTION 'concurrent INSERT changed top-k: %',
            (SELECT ids FROM stable_result);
    END IF;
END
$$;
SQL
        done
    done
done

"${PSQL[@]}" <<'SQL' >/dev/null
SET client_min_messages = warning;
SET enable_indexscan = off;
SET jit = off;
SET statement_timeout = '30s';
TRUNCATE stable_docs;
INSERT INTO stable_docs
SELECT g, 'alpha beta' FROM generate_series(3000, 1, -1) g;
SELECT setval('stable_row', 1, false);
CREATE TEMP TABLE late_term_result AS
SELECT count(DISTINCT body <@> to_bm25query(
           CASE WHEN id > 2501 THEN 'alpha' ELSE 'beta' END,
           'stable_docs_idx')) AS scores
FROM stable_docs WHERE stable_insert_at(500);
DO $$
BEGIN
    IF (SELECT scores FROM late_term_result) <> 1 THEN
        RAISE EXCEPTION 'late term observed different corpus statistics';
    END IF;
END
$$;

-- A prepared execution must refresh statistics, not retain its last run.
PREPARE stable_score AS
SELECT body <@> to_bm25query('alpha', 'stable_docs_idx') AS score
FROM stable_docs WHERE id = 1;
EXECUTE stable_score \gset before_
INSERT INTO stable_docs VALUES (4002, 'gamma');
EXECUTE stable_score \gset after_
SELECT :'before_score'::float8 <> :'after_score'::float8 AS refreshed \gset
\if :refreshed
\else
    \quit 1
\endif

-- A suspended cursor keeps its generation across FETCH and nested SQL.
BEGIN;
DECLARE stable_cursor CURSOR FOR
SELECT body <@> to_bm25query('alpha', 'stable_docs_idx') AS score
FROM stable_docs WHERE id <= 3000;
FETCH stable_cursor \gset first_
SELECT dblink_exec(
    format('host=%s port=%s dbname=%s user=%s',
           current_setting('unix_socket_directories'),
           current_setting('port'), current_database(), current_user),
    'INSERT INTO stable_docs VALUES (4003, ''gamma'')');
SELECT body <@> to_bm25query('alpha', 'stable_docs_idx')
FROM stable_docs WHERE id = 1;
FETCH stable_cursor \gset next_
SELECT :'first_score'::float8 = :'next_score'::float8 AS stable \gset
\if :stable
\else
    \quit 1
\endif
CLOSE stable_cursor;
COMMIT;

-- An error in nested execution must not leave a dangling snapshot cache.
DO $$
BEGIN
    FOR attempt IN 1..3 LOOP
        BEGIN
            PERFORM body <@> to_bm25query('alpha', 'stable_docs_idx')
            FROM stable_docs WHERE id = 1;
            RAISE EXCEPTION 'discard this execution';
        EXCEPTION WHEN raise_exception THEN
            NULL;
        END;
        IF NOT EXISTS (
            SELECT 1 FROM stable_docs
            ORDER BY body <@> to_bm25query('alpha', 'stable_docs_idx')
            LIMIT 1
        ) THEN
            RAISE EXCEPTION 'scoring failed after subtransaction rollback';
        END IF;
    END LOOP;
END
$$;
SQL

for use_cache in off on; do
    "${PSQL[@]}" -v use_cache="${use_cache}" <<'SQL' >/dev/null
SET client_min_messages = warning;
SET pg_textsearch.memtable_cache_enabled = :'use_cache';
CREATE TABLE rewrite_docs (body text);
CREATE INDEX rewrite_idx ON rewrite_docs USING bm25(body)
    WITH (text_config = 'simple');
INSERT INTO rewrite_docs VALUES ('alpha'), ('beta');

-- Simple PL/pgSQL expressions share the outer SELECT's executor context.
CREATE FUNCTION rewrite_and_score() RETURNS void
LANGUAGE plpgsql VOLATILE AS $$
DECLARE
    q bm25query := to_bm25query('alpha', 'rewrite_idx');
    score float8;
    n integer;
BEGIN
    score := 'alpha'::text <@> q;
    FOR n IN 1..3 LOOP
        REINDEX INDEX rewrite_idx;
        score := 'alpha'::text <@> q;
        IF abs(score + ln((2 * n + 1)::float8 / 1.5)) > 0.000001 THEN
            RAISE EXCEPTION 'stale score after REINDEX: %', score;
        END IF;
        INSERT INTO rewrite_docs VALUES ('beta'), ('beta');
    END LOOP;
    TRUNCATE rewrite_docs;
    INSERT INTO rewrite_docs VALUES ('beta');
    score := 'alpha'::text <@> q;
    IF score <> 0 THEN
        RAISE EXCEPTION 'stale score after TRUNCATE: %', score;
    END IF;
END
$$;
SELECT rewrite_and_score();
DROP FUNCTION rewrite_and_score();
DROP TABLE rewrite_docs;
SQL
done

startup_oid=$(
    "${PSQL[@]}" <<'SQL'
SET client_min_messages = warning;
SET pg_textsearch.bulk_load_threshold = 0;
SET pg_textsearch.memtable_pages_threshold = 0;
SET enable_seqscan = off;
CREATE TABLE startup_docs (id int, body text);
CREATE INDEX startup_idx ON startup_docs USING bm25(body)
    WITH (text_config = 'simple');
INSERT INTO startup_docs
SELECT g, 'alpha beta gamma' FROM generate_series(1, 5000) g;
INSERT INTO startup_docs VALUES (5001, 'needle');
SET pg_textsearch.memtable_cache_enabled = off;
DO $$
DECLARE
    plan json;
    buffers int;
    pages int;
BEGIN
    EXECUTE $q$EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON, TIMING OFF)
        SELECT id FROM startup_docs
        ORDER BY body <@> to_bm25query('needle', 'startup_idx')
        LIMIT 1$q$ INTO plan;
    buffers := (plan->0->'Plan'->>'Shared Hit Blocks')::int
             + (plan->0->'Plan'->>'Shared Read Blocks')::int;
    pages := pg_relation_size('startup_idx')
             / current_setting('block_size')::int;
    IF buffers > pages + 16 THEN
        RAISE EXCEPTION 'memtable startup used % buffers for % pages',
            buffers, pages;
    END IF;
END
$$;
SET pg_textsearch.memtable_cache_enabled = on;
SET pg_textsearch.log_cache_state = on;
SELECT id FROM startup_docs
ORDER BY body <@> to_bm25query('needle', 'startup_idx') LIMIT 1;
SELECT 'startup_idx'::regclass::oid;
SQL
)
startup_oid="$(tail -1 <<<"${startup_oid}")"
opens=$(grep -c "cache_source: opened (oid=${startup_oid}," \
    "${LOGFILE}" || true)
if [ "${opens}" -ne 1 ]; then
    echo "first ranked pass opened ${opens} memtable sources; expected 1" >&2
    exit 1
fi

echo "Standalone snapshot test passed"
