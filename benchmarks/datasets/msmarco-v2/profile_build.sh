#!/usr/bin/env bash
#
# profile_build.sh - Profile parallel index build with perf + flamegraph
#
# Usage:
#   ./profile_build.sh [database] [table] [column]
#
# Defaults: msmarco_v2 / passages / passage
#
# Prerequisites:
#   - perf installed (linux-tools-common / linux-tools-$(uname -r))
#   - Postgres running, data loaded
#   - Run as a user with perf_event access (or root)
#
# Output:
#   - flamegraph_build.svg   Flame graph of the build
#   - perf.data              Raw perf recording
#   - pidstat_build.log      CPU/IO stats during build

set -euo pipefail

DB="${1:-msmarco_v2}"
TABLE="${2:-passages}"
COLUMN="${3:-passage}"
INDEX_NAME="idx_${TABLE}_bm25"
FLAMEGRAPH_DIR="./FlameGraph"

# Clone FlameGraph if not present
if [ ! -d "$FLAMEGRAPH_DIR" ]; then
    echo "Cloning FlameGraph repository..."
    git clone --depth 1 \
        https://github.com/brendangregg/FlameGraph.git \
        "$FLAMEGRAPH_DIR"
fi

# Check for perf
if ! command -v perf &>/dev/null; then
    echo "ERROR: perf not found. Install with:"
    echo "  sudo apt install linux-tools-common linux-tools-\$(uname -r)"
    exit 1
fi

# Drop existing index if present
echo "Dropping existing index (if any)..."
psql -d "$DB" -c "DROP INDEX IF EXISTS $INDEX_NAME;" 2>/dev/null || true

# System-wide perf recording captures the leader backend AND all
# parallel worker processes (workers are forked by the postmaster,
# not by the backend, so -p <pid> --inherit misses them).
echo "Starting system-wide perf record..."
sudo perf record -ag --call-graph dwarf,32768 \
    -o perf.data &
PERF_PID=$!
sleep 1

# Start pidstat for all postgres processes
pidstat -C postgres -dru 5 > pidstat_build.log 2>&1 &
PIDSTAT_PID=$!

# Run CREATE INDEX
echo "Running CREATE INDEX..."
START_TIME=$(date +%s)

psql -d "$DB" -c "
SET maintenance_work_mem = '8GB';
SET max_parallel_maintenance_workers = 4;
CREATE INDEX $INDEX_NAME ON $TABLE
    USING bm25 ($COLUMN) WITH (text_config = 'english');
"

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
echo "CREATE INDEX completed in ${ELAPSED}s"

# Stop perf and pidstat
sudo kill -INT "$PERF_PID" 2>/dev/null || true
wait "$PERF_PID" 2>/dev/null || true
kill "$PIDSTAT_PID" 2>/dev/null || true
wait "$PIDSTAT_PID" 2>/dev/null || true

# Generate flame graph (filtered to postgres processes only)
echo "Generating flame graph..."
sudo perf script -i perf.data --comms=postgres \
    | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" \
    | "$FLAMEGRAPH_DIR/flamegraph.pl" \
        --title "Parallel Index Build (${ELAPSED}s)" \
        --width 1600 \
    > flamegraph_build.svg

echo ""
echo "=== Output files ==="
echo "  flamegraph_build.svg      - Flame graph (postgres only)"
echo "  flamegraph_build_all.svg  - Flame graph (all processes)"
echo "  perf.data                 - Raw perf data"
echo "  pidstat_build.log         - CPU/IO stats"
echo ""
echo "View: open flamegraph_build.svg"
