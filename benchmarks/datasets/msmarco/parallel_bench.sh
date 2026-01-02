#!/bin/bash
# MS MARCO Parallel Query Benchmark
# Runs pgbench with # clients = # CPUs for realistic throughput testing

set -e

# Configuration
DURATION=${BENCHMARK_DURATION:-30}  # seconds per test
PGBENCH_SCRIPT="$(dirname "$0")/parallel_query.pgbench"

# Detect number of CPUs
if [ -n "$BENCHMARK_CLIENTS" ]; then
    NUM_CPUS="$BENCHMARK_CLIENTS"
elif command -v nproc &> /dev/null; then
    NUM_CPUS=$(nproc)
elif command -v sysctl &> /dev/null; then
    NUM_CPUS=$(sysctl -n hw.ncpu)
else
    NUM_CPUS=4
fi

echo "=== MS MARCO Parallel Query Benchmark ==="
echo "CPUs detected: $NUM_CPUS"
echo "Duration per test: ${DURATION}s"
echo "pgbench script: $PGBENCH_SCRIPT"
echo ""

# Verify the script exists
if [ ! -f "$PGBENCH_SCRIPT" ]; then
    echo "ERROR: pgbench script not found: $PGBENCH_SCRIPT"
    exit 1
fi

# Find pgbench
if [ -n "$PG_BIN" ]; then
    PGBENCH="$PG_BIN/pgbench"
elif command -v pgbench &> /dev/null; then
    PGBENCH="pgbench"
else
    # Try common locations
    for path in /usr/lib/postgresql/*/bin /usr/pgsql-*/bin /opt/homebrew/bin; do
        if [ -f "$path/pgbench" ]; then
            PGBENCH="$path/pgbench"
            break
        fi
    done
fi

if [ -z "$PGBENCH" ] || ! command -v "$PGBENCH" &> /dev/null; then
    echo "ERROR: pgbench not found"
    exit 1
fi

echo "Using pgbench: $PGBENCH"
echo ""

# Warmup run (short, single client)
echo "=== Warmup (5s, 1 client) ==="
$PGBENCH -f "$PGBENCH_SCRIPT" -c 1 -j 1 -T 5 2>&1 | grep -E "tps|latency" || true
echo ""

# Run scaling tests: 1, 2, 4, ..., up to NUM_CPUS
echo "=== Parallel Scaling Test ==="
echo ""
printf "%-10s %-12s %-15s %-10s\n" "Clients" "TPS" "Latency (ms)" "Scaling"
echo "------------------------------------------------------"

BASE_TPS=""
for clients in 1 2 4 8 16 32; do
    # Stop if we exceed NUM_CPUS
    if [ "$clients" -gt "$NUM_CPUS" ]; then
        break
    fi

    # Use min(clients, NUM_CPUS) threads
    threads=$clients
    if [ "$threads" -gt "$NUM_CPUS" ]; then
        threads=$NUM_CPUS
    fi

    # Run benchmark
    result=$($PGBENCH -f "$PGBENCH_SCRIPT" -c "$clients" -j "$threads" \
        -T "$DURATION" 2>&1)

    tps=$(echo "$result" | grep "tps =" | awk '{print $3}')
    latency=$(echo "$result" | grep "latency average" | awk '{print $4}')

    # Calculate scaling factor
    if [ -z "$BASE_TPS" ]; then
        BASE_TPS="$tps"
        scaling="1.00x"
    else
        scaling=$(echo "scale=2; $tps / $BASE_TPS" | bc)
        scaling="${scaling}x"
    fi

    printf "%-10s %-12.2f %-15.3f %-10s\n" "$clients" "$tps" "$latency" "$scaling"
done

echo ""

# Final full-parallel test with all CPUs
echo "=== Full Parallel Test ($NUM_CPUS clients, ${DURATION}s) ==="
result=$($PGBENCH -f "$PGBENCH_SCRIPT" -c "$NUM_CPUS" -j "$NUM_CPUS" \
    -T "$DURATION" 2>&1)

tps=$(echo "$result" | grep "tps =" | awk '{print $3}')
latency=$(echo "$result" | grep "latency average" | awk '{print $4}')

echo ""
echo "PARALLEL_TPS: $tps"
echo "PARALLEL_LATENCY_MS: $latency"
echo "PARALLEL_CLIENTS: $NUM_CPUS"
echo ""

# Output for metrics extraction
echo "=== Parallel Benchmark Complete ==="
