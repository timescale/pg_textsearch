#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VENV="${PGTS_SCALE_VENV:-/srv/searchbench/pg_textsearch-scale/venv}"
if [[ ! -x "$VENV/bin/python" ]]; then
  mkdir -p "$(dirname "$VENV")"
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q -r "$ROOT/benchmarks/scale/requirements.txt"
fi
exec "$VENV/bin/python" "$ROOT/benchmarks/scale/run.py" "$@"
