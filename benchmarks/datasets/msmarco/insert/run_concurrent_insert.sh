#!/usr/bin/env bash
#
# Concurrent INSERT benchmark for MS MARCO dataset.
#
# Runs pgbench at multiple concurrency levels (1, 2, 4, 8, 16 clients)
# for a configurable duration per level. Reports TPS and latency.
#
# The script does NOT reset between concurrency levels — the index
# accumulates data across runs, which is more realistic (the memtable
# grows, spills happen, segments compact under load).
#
# Prerequisites:
#   - Postgres running with the target extension installed
#   - msmarco staging data loaded via concurrent-setup.sql (or the
#     ParadeDB equivalent in paradedb/concurrent-setup.sql)
#
# Usage:
#   # pg_textsearch (default):
#   ./run_concurrent_insert.sh
#
#   # ParadeDB:
#   ./run_concurrent_insert.sh --engine paradedb
#
#   # Custom port/duration:
#   PGPORT=5434 DURATION=30 ./run_concurrent_insert.sh
#
# Environment variables:
#   PGPORT      Postgres port (default: 5434)
#   PGDATABASE  Database name (default: postgres)
#   DURATION    Seconds per concurrency level (default: 60)
#   CLIENTS     Space-separated client counts (default: "1 2 4 8 16")
#   SKIP_SETUP  Set to 1 to skip setup phase (reuse existing staging)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MSMARCO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="${DATA_DIR:-$MSMARCO_DIR/data}"

PGPORT="${PGPORT:-5434}"
PGDATABASE="${PGDATABASE:-postgres}"
DURATION="${DURATION:-60}"
CLIENTS="${CLIENTS:-1 2 4 8 16}"
SKIP_SETUP="${SKIP_SETUP:-0}"
ENGINE="${1:-tapir}"

# Strip leading -- from engine flag
ENGINE="${ENGINE#--engine=}"
ENGINE="${ENGINE#--engine }"
if [[ "$ENGINE" == "--engine" ]]; then
    ENGINE="${2:-tapir}"
fi

# Resolve paths based on engine
case "$ENGINE" in
    tapir|pg_textsearch)
        ENGINE_LABEL="pg_textsearch"
        SETUP_SQL="$SCRIPT_DIR/concurrent-setup.sql"
        PGBENCH_SQL="$SCRIPT_DIR/pgbench-insert.sql"
        COUNT_TABLE="msmarco_passages"
        ;;
    paradedb|pg_search)
        ENGINE_LABEL="ParadeDB"
        SETUP_SQL="$MSMARCO_DIR/paradedb/concurrent-setup.sql"
        PGBENCH_SQL="$MSMARCO_DIR/paradedb/pgbench-insert.sql"
        COUNT_TABLE="msmarco_passages_paradedb"
        ;;
    *)
        echo "Unknown engine: $ENGINE" >&2
        echo "Usage: $0 [--engine tapir|paradedb]" >&2
        exit 1
        ;;
esac

export PGPORT PGDATABASE

echo "=============================================="
echo " Concurrent INSERT Benchmark: $ENGINE_LABEL"
echo "=============================================="
echo "Port:     $PGPORT"
echo "Database: $PGDATABASE"
echo "Duration: ${DURATION}s per level"
echo "Clients:  $CLIENTS"
echo "Date:     $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

# --- Setup phase ---
if [[ "$SKIP_SETUP" != "1" ]]; then
    echo "--- Setup: loading staging data ---"
    (cd "$MSMARCO_DIR" && DATA_DIR="$DATA_DIR" \
        psql -f "$SETUP_SQL" 2>&1) | tail -5
    echo ""
fi

# --- Benchmark runs ---
echo "--- Results ---"
printf "%-10s %10s %12s %12s %15s\n" \
    "CLIENTS" "TPS" "AVG_LAT_MS" "ROWS_60S" "CUMULATIVE"

for c in $CLIENTS; do
    output=$(pgbench -n -c "$c" -j "$c" -T "$DURATION" \
        -f "$PGBENCH_SQL" 2>&1)

    tps=$(echo "$output" | grep "^tps" | \
        grep -oE '[0-9]+\.[0-9]+' | head -1)
    lat=$(echo "$output" | grep "latency average" | \
        grep -oE '[0-9]+\.[0-9]+')
    txns=$(echo "$output" | \
        grep "number of transactions actually processed" | \
        grep -oE '[0-9]+' | head -1)
    cumulative=$(psql -qtAc \
        "SELECT count(*) FROM $COUNT_TABLE;")

    printf "%-10s %10s %12s %12s %15s\n" \
        "$c" "$tps" "$lat" "$txns" "$cumulative"
done

echo ""
echo "--- Done ---"
