#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../.."
root="${PGTS_SCALE_ROOT:-/srv/searchbench/pg_textsearch-scale}"
name=pgts-scale-phrase-validation
label="${1:-$(date -u +%Y%m%dT%H%M%SZ)}"
if [[ ! "$label" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "Invalid result label" >&2
    exit 2
fi
out="$root/phrase-semantics-$label"
test "$(docker info --format '{{.DockerRootDir}}')" = /srv/searchbench/docker
mkdir -p "$out"
"$root/venv/bin/python" - "$root" <<'PY'
from pathlib import Path
import sys
from benchmarks.scale.run import Runner
runner = Runner(
    Path.cwd(), Path(sys.argv[1]),
    True, "phrase-semantics-validation", "msmarco-v2", None,
)
runner.prepare()
PY
image="${PGTS_PARADEDB_IMAGE:-}"
if [[ -z "$image" ]]; then
    image="$("$root/venv/bin/python" - "$root/images.lock.json" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1]))["paradedb"])
PY
)"
fi
docker run --rm -d --name "$name" --label purpose=phrase-validation \
    --cpuset-cpus=0-23 --memory=16g --shm-size=2g \
    -e POSTGRES_PASSWORD=postgres -p 127.0.0.1:15432:5432 \
    "$image" -c shared_buffers=512MB -c max_connections=128
trap 'docker stop --timeout 30 "$name" >/dev/null' EXIT
docker inspect "$name" > "$out/container-inspect.json"
ready=0
for attempt in {1..60}; do
    if docker exec "$name" pg_isready -h 127.0.0.1 -U postgres \
        >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
test "$ready" = 1
docker exec -i "$name" psql -U postgres -v ON_ERROR_STOP=1 <<'SQL'
CREATE EXTENSION IF NOT EXISTS pg_search;
CREATE TABLE documents (id bigint PRIMARY KEY, body text NOT NULL);
INSERT INTO documents VALUES
    (1, 'alpha the beta'),
    (2, 'alpha beta'),
    (3, 'alpha beta');
SQL
dataset="$root/smoke-dataset"
docker exec -i "$name" psql -U postgres -v ON_ERROR_STOP=1 \
    < "$dataset/paradedb/post.sql"
docker exec -i "$name" psql -U postgres -v ON_ERROR_STOP=1 <<'SQL' \
    | tee "$out/sql-probe.log"
SELECT count(*) AS phrase_operator_hits
FROM documents WHERE body ### 'alpha the beta';
SELECT count(*) AS quoted_parser_hits
FROM documents WHERE body @@@ '"alpha the beta"';
SELECT count(*) AS postgres_phrase_hits
FROM documents WHERE to_tsvector('english', body)
    @@ phraseto_tsquery('english', 'alpha the beta');
DO $$
BEGIN
    IF ARRAY(SELECT id FROM documents
             WHERE body @@@ '"alpha the beta"') <> ARRAY[1::bigint]
    THEN
        RAISE EXCEPTION 'Phrase parser did not exclude adjacent-term controls';
    END IF;
END;
$$;
SQL
printf '[{"id":"stopword-gap","text":"alpha the beta"}]\n' \
    > "$out/queries.json"
export PARADEDB_URL=postgres://postgres:postgres@127.0.0.1:15432/postgres
cd "$dataset/k6"
env ENGINE=paradedb MODE=phrase LIMIT=10 QUERY_FILE="$out/queries.json" \
    PHASE=measured DURATION=2s VUS=1 BACKENDS_DIR="$dataset/backends" \
    DASHBOARD_EXPORT_DIR="$out" DASHBOARD_EXPORT_PREFIX=phrase-validation \
    taskset -c 24-31 "$root/tools/benchmarker/k6" run \
    --summary-export "$out/summary.json" \
    --out dashboard=json ./search.js > "$out/k6.log" 2>&1
"$root/venv/bin/python" - "$out" <<'PY'
import json
from pathlib import Path
import sys
out = Path(sys.argv[1])
files = list(out.glob("phrase-validation*.json"))
assert len(files) == 1, files
dashboard = json.loads(files[0].read_text())
run, = dashboard["runs"].values()
hits = run["queries"]["search"]["hitCounts"]
assert hits and all(count == 1 for count in hits), (
    "The phrase driver failed the stopword-gap and adjacency controls",
    min(hits) if hits else None,
    max(hits) if hits else None,
)
print("Exact stopword-containing phrase matched in every driver request.")
PY
