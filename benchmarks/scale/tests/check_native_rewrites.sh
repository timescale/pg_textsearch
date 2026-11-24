#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../.."
root="${PGTS_SCALE_ROOT:-/srv/searchbench/pg_textsearch-scale}"
name=pgts-scale-native-rewrite-validation
label="${1:-$(date -u +%Y%m%dT%H%M%SZ)}"
if [[ ! "$label" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "Invalid result label" >&2
    exit 2
fi
out="$root/native-rewrites-$label"
test "$(docker info --format '{{.DockerRootDir}}')" = /srv/searchbench/docker
mkdir -p "$out"
python="$root/venv/bin/python"
sha="$("$python" -c \
    'from benchmarks.scale.run import PG_TEXTSEARCH_SHA
print(PG_TEXTSEARCH_SHA)')"
docker run --rm -d --name "$name" --label purpose=native-rewrite-validation \
    --cpuset-cpus=0-7 --memory=8g --shm-size=2g \
    -e POSTGRES_PASSWORD=postgres "pg_textsearch-scale:$sha" \
    -c shared_preload_libraries=pg_textsearch -c shared_buffers=256MB
trap 'docker stop --timeout 30 "$name" >/dev/null' EXIT
ready=0
for attempt in {1..60}; do
    if docker exec "$name" pg_isready -U postgres >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
test "$ready" = 1
docker exec -i "$name" psql -U postgres -X -v ON_ERROR_STOP=1 \
    > "$out/setup.log" <<'SQL'
CREATE EXTENSION pg_textsearch;
SET default_text_search_config='pg_catalog.english';
CREATE TABLE documents (id bigint PRIMARY KEY, body text NOT NULL);
INSERT INTO documents
SELECT n, repeat(CASE WHEN n <= 1000 THEN 'alpha ' ELSE 'beta ' END, 16)
FROM generate_series(1, 2000) n;
INSERT INTO documents VALUES
    (2001, 'alpha beta ' || repeat('padding ', 50)),
    (2002, 'alpha beta ' || repeat('padding ', 100)),
    (2003, 'alpha the beta ' || repeat('padding ', 150));
CREATE INDEX documents_bm25 ON documents USING bm25(body)
WITH (text_config='english');
INSERT INTO documents VALUES
    (2004, 'alpha beta ' || repeat('padding ', 200));
VACUUM (ANALYZE) documents;
SQL
export PGTS_REWRITE_TEST_CONTAINER="$name"
PGTS_REWRITE_PLAN_OUTPUT="$out/english" \
    "$python" -m unittest discover -s benchmarks/scale/tests \
    -p test_native_rewrites.py -v > "$out/english-tests.log" 2>&1
analyzer_sql="$("$python" - <<'PY'
from benchmarks.scale.tin import analyzer_sql
print(analyzer_sql("pg_textsearch", "english-no-stopwords"))
PY
)"
docker exec "$name" psql -U postgres -X -v ON_ERROR_STOP=1 \
    -c "$analyzer_sql" > "$out/no-stopword-setup.log"
docker exec -i "$name" psql -U postgres -X -v ON_ERROR_STOP=1 \
    >> "$out/no-stopword-setup.log" <<'SQL'
INSERT INTO documents VALUES
    (2005, 'alpha beta ' || repeat('padding ', 220));
VACUUM (ANALYZE) documents;
SQL
PGTS_REWRITE_TEST_CONFIG=public.tin_english_no_stopwords \
    PGTS_REWRITE_PLAN_OUTPUT="$out/no-stopwords" \
    "$python" -m unittest discover -s benchmarks/scale/tests \
    -p test_native_rewrites.py -v > "$out/no-stopword-tests.log" 2>&1
cat "$out/english-tests.log" "$out/no-stopword-tests.log"
