#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../.."
root="${PGTS_SCALE_ROOT:-/srv/searchbench/pg_textsearch-scale}"
label="${1:-$(date -u +%Y%m%dT%H%M%SZ)}"
if [[ ! "$label" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "Invalid result label" >&2
    exit 2
fi
test "$(docker info --format '{{.DockerRootDir}}')" = /srv/searchbench/docker
name=pgts-tin-drain-validation
out="$root/tin-drain-$label"
mkdir -p "$out/k6"
cp benchmarks/scale/benchmarker/schema.yaml "$out/schema.yaml"
printf '%s\n' '[{"id":"slow","text":"fixture"}]' > "$out/k6/queries.json"
printf '%s\n' '[1,2]' > "$out/k6/update_targets.json"
printf '%s\n' \
    '{"kinds":["disjunction"],
      "sql":{"disjunction":"SELECT $1::text, pg_sleep(35)"}}' \
    > "$out/k6/scenario.json"
sha="$("$root/venv/bin/python" -c \
    'from benchmarks.scale.run import PG_TEXTSEARCH_SHA
print(PG_TEXTSEARCH_SHA)')"
docker run --rm -d --name "$name" \
    --label managed-by=pg_textsearch-tin --label purpose=driver-drain \
    --cpuset-cpus=0-7 --memory=16g --memory-swap=16g --shm-size=2g \
    -e POSTGRES_PASSWORD=postgres -p 127.0.0.1:15432:5432 \
    "pg_textsearch-scale:$sha" \
    -c shared_preload_libraries=pg_textsearch \
    -c shared_buffers=256MB > "$out/container-start.log"
trap 'docker stop --timeout 60 "$name" >/dev/null' EXIT
ready=0
for attempt in {1..60}; do
    if docker exec "$name" pg_isready -U postgres >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
test "$ready" = 1
docker exec "$name" psql -U postgres -v ON_ERROR_STOP=1 -c "
CREATE EXTENSION pg_textsearch;
CREATE TABLE documents(id bigint PRIMARY KEY,body text);
INSERT INTO documents VALUES(1,'fixture'),(2,'fixture');
CREATE INDEX documents_bm25 ON documents USING bm25(body)
WITH(text_config='english');
CREATE FUNCTION delay_write() RETURNS trigger LANGUAGE plpgsql AS
\$\$ BEGIN PERFORM pg_sleep(35); RETURN NEW; END \$\$;
CREATE TRIGGER delayed_update BEFORE UPDATE ON documents
FOR EACH ROW EXECUTE FUNCTION delay_write();" > "$out/setup.log" 2>&1
repo="$PWD"
cd "$out/k6"
env PG_TEXTSEARCH_URL=postgres://postgres:postgres@127.0.0.1:15432/postgres \
    ENGINE=pg_textsearch OPERATION=concurrent-writes QUERY_FAMILY=disjunction \
    CELL_ID=drain PACK=drain PROFILE=constrained16 ANALYZER=english-stopwords \
    PHASE=measured DURATION=1s VUS=1 \
    SCENARIO_FILE="$out/k6/scenario.json" QUERY_FILE="$out/k6/queries.json" \
    UPDATE_TARGETS_FILE="$out/k6/update_targets.json" UPDATE_RATE=1 \
    BACKENDS_DIR="$repo/benchmarks/scale/benchmarker/backends" \
    DASHBOARD_EXPORT_DIR="$out" DASHBOARD_EXPORT_PREFIX=drain \
    taskset -c 24-31 "$root/tools/benchmarker/k6" run \
    --summary-export "$out/summary.json" \
    --out dashboard=json "$repo/benchmarks/scale/benchmarker/k6/tin.js" \
    > "$out/k6.log" 2>&1
"$root/venv/bin/python" - "$out/summary.json" <<'PY'
import json
import sys

metrics = json.load(open(sys.argv[1]))["metrics"]
errors = metrics.get("benchmark_errors", {})
completed = errors.get("passes", 0) + errors.get("fails", 0)
writes = metrics.get("achieved_updates", {}).get("count", 0)
assert 1 <= writes <= 2, "long writes did not complete"
assert completed == 1 + writes, (
    f"long request interrupted before result accounting: {completed} callbacks"
)
assert errors["passes"] == 0, "drained request failed"
assert metrics["update_failures"]["fails"] == writes
assert metrics["update_failures"]["passes"] == 0
print("Long read and write drained and recorded without SQL errors")
PY
