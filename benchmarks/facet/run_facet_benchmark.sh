#!/bin/bash
# Faceted MS-MARCO benchmark runner.
#
# Measures the performance impact of the pg_textsearch faceted-search filter
# pushdown (PR #408) and compares three systems on identical data and queries:
#
#   pg_textsearch, pushdown ON  (enable_facet_pushdown=on, gate wide open)
#   pg_textsearch, pushdown OFF (enable_facet_pushdown=off  == pre-PR behavior)
#   ParadeDB pg_search          (native faceted BM25 search)
#
# across a facet-selectivity sweep (1%..50%). See README.md for the design.
#
# Usage:
#   run_facet_benchmark.sh [options]
#
#   --scale v1|v2        Dataset: v1 = MS-MARCO v1 (8.8M), v2 = v2 (138M)
#   --systems LIST       Comma list of: pg, paradedb        (default: pg,paradedb)
#   --nq N               Queries per (mode, selectivity) cell (default: 200)
#   --load               (Re)load data + build indexes before querying
#   --pg-conn "..."      psql conn args for pg_textsearch (default from env)
#   --pdb-conn "..."     psql conn args for ParadeDB      (default from env)
#   --data-dir DIR       Dataset dir containing collection.tsv
#   --out DIR            Results dir (default: benchmarks/facet/results)
#
# Connection defaults can also be set via env:
#   PG_PSQL   e.g. "psql -h /tmp -p 5408 -U postgres -d postgres"
#   PDB_PSQL  e.g. "docker exec -i paradedb_bench408 psql -U postgres"

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

SCALE=v1
SYSTEMS=pg,paradedb
NQ=200
DO_LOAD=0
OUT="$REPO_ROOT/benchmarks/facet/results"
DATA_DIR="${DATA_DIR:-}"
PG_PSQL="${PG_PSQL:-psql -h /tmp -p 5408 -U postgres -d postgres}"
PDB_PSQL="${PDB_PSQL:-docker exec -i paradedb_bench408 psql -U postgres}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale)     SCALE="$2"; shift 2 ;;
    --systems)   SYSTEMS="$2"; shift 2 ;;
    --nq)        NQ="$2"; shift 2 ;;
    --load)      DO_LOAD=1; shift ;;
    --pg-conn)   PG_PSQL="$2"; shift 2 ;;
    --pdb-conn)  PDB_PSQL="$2"; shift 2 ;;
    --data-dir)  DATA_DIR="$2"; shift 2 ;;
    --out)       OUT="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

if [[ -z "$DATA_DIR" ]]; then
  if [[ "$SCALE" == "v2" ]]; then
    DATA_DIR="$REPO_ROOT/benchmarks/datasets/msmarco-v2/data"
  else
    DATA_DIR="$REPO_ROOT/benchmarks/datasets/msmarco/data"
  fi
fi

mkdir -p "$OUT"
TS="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$OUT/${SCALE}_${TS}"
mkdir -p "$RUN_DIR"
echo "Results dir: $RUN_DIR"
echo "Scale=$SCALE systems=$SYSTEMS nq=$NQ data_dir=$DATA_DIR"

has_system() { [[ ",$SYSTEMS," == *",$1,"* ]]; }

# ---- pg_textsearch -------------------------------------------------------
if has_system pg; then
  if [[ "$DO_LOAD" == "1" ]]; then
    echo "[pg] loading + indexing ($SCALE)..."
    DATA_DIR="$DATA_DIR" $PG_PSQL -v ON_ERROR_STOP=1 \
        -f benchmarks/facet/pg_textsearch/load.sql \
        > "$RUN_DIR/pg_load.log" 2>&1
    grep -E 'INDEX_SIZE|TABLE_SIZE|rows ' "$RUN_DIR/pg_load.log" || true
  fi
  echo "[pg] running query sweep (nq=$NQ)..."
  $PG_PSQL -v ON_ERROR_STOP=1 -v nq="$NQ" \
      -f benchmarks/facet/pg_textsearch/queries.sql \
      > "$RUN_DIR/pg_queries.log" 2>&1
  grep -E 'FACET_RESULT|FACET_SWEEP_ROW|PARITY_CHECK' "$RUN_DIR/pg_queries.log" || true
fi

# ---- ParadeDB ------------------------------------------------------------
if has_system paradedb; then
  if [[ "$DO_LOAD" == "1" ]]; then
    echo "[paradedb] loading + indexing ($SCALE)..."
    # ParadeDB load: \copy FROM PROGRAM runs client-side, so DATA_DIR must be
    # visible to the psql client. For the docker exec form we stream the SQL in.
    DATA_DIR="$DATA_DIR" $PDB_PSQL -v ON_ERROR_STOP=1 \
        < benchmarks/facet/paradedb/load.sql \
        > "$RUN_DIR/pdb_load.log" 2>&1 || {
          echo "[paradedb] load failed; see $RUN_DIR/pdb_load.log"; }
    grep -E 'INDEX_SIZE|TABLE_SIZE|rows ' "$RUN_DIR/pdb_load.log" || true
  fi
  echo "[paradedb] running query sweep (nq=$NQ)..."
  $PDB_PSQL -v ON_ERROR_STOP=1 -v nq="$NQ" \
      < benchmarks/facet/paradedb/queries.sql \
      > "$RUN_DIR/pdb_queries.log" 2>&1 || {
        echo "[paradedb] queries failed; see $RUN_DIR/pdb_queries.log"; }
  grep -E 'FACET_RESULT|FACET_SWEEP_ROW' "$RUN_DIR/pdb_queries.log" || true
fi

# ---- Build a comparison report ------------------------------------------
REPORT="$RUN_DIR/summary.md"
{
  echo "# Faceted MS-MARCO benchmark ($SCALE)"
  echo ""
  echo "- Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "- Queries per cell: $NQ"
  echo "- Systems: $SYSTEMS"
  echo ""
  echo "Average ms/query (lower is better), by facet selectivity:"
  echo ""
  echo "| selectivity | pg OFF | pg ON | ParadeDB | ON speedup vs OFF |"
  echo "|------------:|-------:|------:|---------:|------------------:|"
} > "$REPORT"

python3 - "$RUN_DIR" >> "$REPORT" <<'PY'
import os, re, sys
run_dir = sys.argv[1]
# mode -> sel -> avg
data = {}
pat = re.compile(r'FACET_SWEEP_ROW mode=(\S+) sel=(\d+) .*? avg=([\d.]+)')
for fn in ('pg_queries.log', 'pdb_queries.log'):
    p = os.path.join(run_dir, fn)
    if not os.path.exists(p):
        continue
    for line in open(p):
        m = pat.search(line)
        if m:
            mode, sel, avg = m.group(1), int(m.group(2)), float(m.group(3))
            data.setdefault(sel, {})[mode] = avg
for sel in sorted(data):
    d = data[sel]
    off = d.get('off')
    on  = d.get('on_forced')
    pdb = d.get('paradedb')
    def f(x): return f"{x:.2f}" if x is not None else "-"
    speed = f"{off/on:.2f}x" if (off and on) else "-"
    print(f"| {sel}% | {f(off)} | {f(on)} | {f(pdb)} | {speed} |")
PY

echo ""
echo "=== Report ==="
cat "$REPORT"
echo ""
echo "Full logs + report in: $RUN_DIR"
