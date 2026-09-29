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
if [ "${inherit_snapshot_count}" -ne 1 ]; then
    echo "inherited standalone scoring captured ${inherit_snapshot_count} graph snapshots; expected 1" \
        >&2
    exit 1
fi

echo "Standalone snapshot test passed"
