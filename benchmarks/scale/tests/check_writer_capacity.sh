#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../.."
root="${PGTS_SCALE_ROOT:-/srv/searchbench/pg_textsearch-scale}"
name=pgts-scale-writer-validation
label="${1:-$(date -u +%Y%m%dT%H%M%SZ)}"
if [[ ! "$label" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "Invalid result label" >&2
    exit 2
fi
out="$root/writer-capacity-$label"
test "$(docker info --format '{{.DockerRootDir}}')" = /srv/searchbench/docker
mkdir -p "$out"
"$root/venv/bin/python" - "$root" <<'PY'
from pathlib import Path
import sys
from benchmarks.scale.run import Runner
runner = Runner(
    Path.cwd(), Path(sys.argv[1]),
    True, "writer-capacity-validation", "msmarco-v2", None,
)
runner.prepare()
PY
sha="$("$root/venv/bin/python" -c \
    'from benchmarks.scale.run import PG_TEXTSEARCH_SHA; print(PG_TEXTSEARCH_SHA)')"
docker run --rm -d --name "$name" --label purpose=writer-validation \
    --cpuset-cpus=0-23 --memory=16g --shm-size=2g \
    -e POSTGRES_PASSWORD=postgres -p 127.0.0.1:15432:5432 \
    "pg_textsearch-scale:$sha" \
    -c shared_preload_libraries=pg_textsearch \
    -c shared_buffers=512MB -c max_connections=128
trap 'docker stop --timeout 30 "$name" >/dev/null' EXIT
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
export PG_TEXTSEARCH_URL=postgres://postgres:postgres@127.0.0.1:15432/postgres
dataset="$root/smoke-dataset"
taskset -c 24-31 "$root/tools/benchmarker/bin/loader" load \
    --backend pg_textsearch --backends-dir "$dataset/backends" \
    --workers 8 --batch-size 10000 "$dataset" > "$out/load.log" 2>&1
docker exec "$name" psql -U postgres -v ON_ERROR_STOP=1 -c '
CREATE FUNCTION delay_write() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  PERFORM pg_sleep(0.01);
  RETURN NEW;
END
$$;
CREATE TRIGGER delayed_update BEFORE UPDATE ON documents
FOR EACH ROW EXECUTE FUNCTION delay_write();'
cp "$dataset/k6/search.js" "$out/search.js"
cd "$dataset/k6"
env ENGINE=pg_textsearch MODE=mixed LIMIT=10 QUERY_FILE=./natural.json \
    PHASE=measured DURATION=5s VUS=8 UPDATE_RATE=1000 \
    BACKENDS_DIR="$dataset/backends" DASHBOARD_EXPORT_DIR="$out" \
    taskset -c 24-31 "$root/tools/benchmarker/k6" run \
    --summary-export "$out/summary.json" \
    --out dashboard=json ./search.js > "$out/k6.log" 2>&1
"$root/venv/bin/python" - "$out/summary.json" <<'PY'
import json
import sys
metrics = json.load(open(sys.argv[1]))["metrics"]
count = metrics["achieved_updates"]["count"]
print("Successful updates:", count, "scheduled target:", 5000)
print("Dropped iterations:", metrics.get("dropped_iterations"))
assert count >= 4500, "Writer client cannot sustain the intended offered load"
PY
