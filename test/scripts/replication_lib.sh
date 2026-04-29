#!/bin/bash
#
# replication_lib.sh — shared helpers for physical-replication tests.
#
# Sourced by test/scripts/replication*.sh files. Provides two-node
# setup primitives, query helpers, catchup waits, and long-lived
# (persistent) psql session management used to expose the standby
# memtable-staleness bug.
#
# Required environment variables (set by the sourcing script BEFORE
# this file is sourced):
#
#   PRIMARY_PORT, STANDBY_PORT
#   PRIMARY_DIR, STANDBY_DIR
#   TEST_DB
#
# Optional:
#   STANDBY2_PORT, STANDBY2_DIR  — set by replication_cascading.sh
#

# Colors for output.
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"
}

warn() {
    echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"
}

error() {
    echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"
    exit 1
}

# -------------------------------------------------------------------
# Cleanup. Each script's main() should `trap repl_cleanup EXIT INT TERM`
# AFTER setting PRIMARY_DIR/STANDBY_DIR/STANDBY2_DIR.
# -------------------------------------------------------------------
repl_cleanup() {
    local exit_code=$?
    log "Cleaning up replication test environment..."
    if [ $exit_code -ne 0 ]; then
        warn "Test failed - dumping logs:"
        for d in "${PRIMARY_DIR:-}" "${STANDBY_DIR:-}" \
                 "${STANDBY2_DIR:-}"; do
            [ -z "$d" ] && continue
            if [ -f "${d}/log/postgres.log" ]; then
                echo "=== $(basename "$d") LOG (last 80 lines) ==="
                tail -80 "${d}/log/postgres.log" 2>/dev/null || true
            fi
        done
    fi
    for dir in "${STANDBY2_DIR:-}" "${STANDBY_DIR:-}" \
               "${PRIMARY_DIR:-}"; do
        [ -z "$dir" ] && continue
        if [ -f "${dir}/postmaster.pid" ]; then
            pg_ctl stop -D "${dir}" -m fast 2>/dev/null ||
                pg_ctl stop -D "${dir}" -m immediate 2>/dev/null || \
                true
        fi
    done
    for dir in "${PRIMARY_DIR:-}" "${STANDBY_DIR:-}" \
               "${STANDBY2_DIR:-}"; do
        [ -z "$dir" ] || rm -rf "${dir}"
    done
    exit $exit_code
}

# -------------------------------------------------------------------
# SQL helpers. Each takes either a port (for arbitrary node) or
# uses a default (PRIMARY_PORT / STANDBY_PORT).
# -------------------------------------------------------------------
primary_sql() {
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

primary_sql_quiet() {
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

standby_sql() {
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

standby_sql_quiet() {
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

standby2_sql_quiet() {
    psql -p "${STANDBY2_PORT}" -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

node_sql_quiet() {
    psql -p "$1" -d "${TEST_DB}" -tA -c "$2" 2>/dev/null
}

# Count search results using the index-scan pattern (CLAUDE.md says
# WHERE-with-<@>-<-0 is an anti-pattern; ORDER BY ... LIMIT is right).
search_count() {
    local port=$1
    local table=$2
    local column=$3
    local query=$4
    local index=$5
    psql -p "${port}" -d "${TEST_DB}" -tA -c "
        SELECT count(*) FROM (
            SELECT id FROM ${table}
            ORDER BY ${column} <@> to_bm25query('${query}',
                '${index}')
            LIMIT 100
        ) t;" 2>/dev/null
}

# -------------------------------------------------------------------
# Wait for a target node's pg_last_wal_replay_lsn to catch up to the
# primary's pg_current_wal_lsn.
# -------------------------------------------------------------------
wait_for_node_catchup() {
    local target_port=$1
    local max_wait=${2:-15}
    local i=0
    log "Waiting for node ${target_port} to catch up (max ${max_wait}s)..."
    while [ $i -lt $max_wait ]; do
        local primary_lsn
        primary_lsn=$(node_sql_quiet "${PRIMARY_PORT}" \
            "SELECT pg_current_wal_lsn();")
        local target_lsn
        target_lsn=$(node_sql_quiet "${target_port}" \
            "SELECT pg_last_wal_replay_lsn();")
        if [ -n "$primary_lsn" ] && [ -n "$target_lsn" ]; then
            local caught_up
            caught_up=$(node_sql_quiet "${target_port}" \
                "SELECT '${target_lsn}'::pg_lsn >= '${primary_lsn}'::pg_lsn;")
            if [ "$caught_up" = "t" ]; then
                log "Node ${target_port} caught up at LSN ${target_lsn}"
                return 0
            fi
        fi
        sleep 1
        i=$((i + 1))
    done
    warn "Node ${target_port} may not have caught up after ${max_wait}s"
    return 1
}

# Compatibility shim for the existing replication.sh.
wait_for_standby_catchup() {
    wait_for_node_catchup "${STANDBY_PORT}" "${1:-15}"
}

# -------------------------------------------------------------------
# Two-node setup helpers. setup_primary uses the script-supplied
# PRIMARY_DIR/PRIMARY_PORT.
# -------------------------------------------------------------------
setup_primary() {
    log "Setting up primary instance on port ${PRIMARY_PORT}..."
    rm -rf "${PRIMARY_DIR}"
    mkdir -p "${PRIMARY_DIR}"

    initdb -D "${PRIMARY_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1

    cat >> "${PRIMARY_DIR}/postgresql.conf" <<EOF
port = ${PRIMARY_PORT}
shared_buffers = 128MB
max_connections = 30
log_min_messages = notice
logging_collector = on
log_filename = 'postgres.log'
shared_preload_libraries = 'pg_textsearch'
wal_level = replica
max_wal_senders = 8
hot_standby = on
EOF

    echo "local replication all trust" >> "${PRIMARY_DIR}/pg_hba.conf"
    echo "host replication all 127.0.0.1/32 trust" >> \
        "${PRIMARY_DIR}/pg_hba.conf"

    pg_ctl start -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -w
    createdb -p "${PRIMARY_PORT}" "${TEST_DB}"
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

# Standby is built from primary via pg_basebackup. Caller must
# already have run setup_primary (and optionally loaded data).
setup_standby() {
    log "Creating standby via pg_basebackup..."
    rm -rf "${STANDBY_DIR}"

    pg_basebackup -D "${STANDBY_DIR}" -p "${PRIMARY_PORT}" -R \
        -X stream --checkpoint=fast >/dev/null 2>&1

    cat >> "${STANDBY_DIR}/postgresql.conf" <<EOF
port = ${STANDBY_PORT}
EOF

    pg_ctl start -D "${STANDBY_DIR}" \
        -l "${STANDBY_DIR}/postgres.log" -w

    local in_recovery
    in_recovery=$(node_sql_quiet "${STANDBY_PORT}" \
        "SELECT pg_is_in_recovery();")
    if [ "$in_recovery" != "t" ]; then
        error "Standby is not in recovery mode!"
    fi
    log "Standby running on port ${STANDBY_PORT} (in recovery)"
}

# Cascading: standby2 takes its basebackup from standby1 (which is
# in recovery). Postgres supports this since 9.x.
setup_standby2() {
    log "Creating standby2 via pg_basebackup from standby1..."
    rm -rf "${STANDBY2_DIR}"

    pg_basebackup -D "${STANDBY2_DIR}" -p "${STANDBY_PORT}" -R \
        -X stream --checkpoint=fast >/dev/null 2>&1

    cat >> "${STANDBY2_DIR}/postgresql.conf" <<EOF
port = ${STANDBY2_PORT}
EOF

    pg_ctl start -D "${STANDBY2_DIR}" \
        -l "${STANDBY2_DIR}/postgres.log" -w

    local in_recovery
    in_recovery=$(node_sql_quiet "${STANDBY2_PORT}" \
        "SELECT pg_is_in_recovery();")
    if [ "$in_recovery" != "t" ]; then
        error "Standby2 is not in recovery mode!"
    fi
    log "Standby2 running on port ${STANDBY2_PORT} (in recovery)"
}

# -------------------------------------------------------------------
# Long-lived (persistent) psql session.
#
# The standby memtable-staleness bug only manifests for backends that
# stay alive across primary inserts (one-shot psql -c reconnects
# each time and hits the rebuild path). These helpers manage a
# persistent psql process via FIFOs, with a sentinel-line protocol
# so we know when each query's output has fully arrived.
#
# Usage:
#   long_lived_open <port>
#   result=$(long_lived_query "SELECT count(*) FROM ...;")
#   long_lived_close
#
# Only one long-lived session per shell at a time.
# -------------------------------------------------------------------
long_lived_open() {
    local port=$1
    LL_TMP_DIR=$(mktemp -d -t pg_ts_repl.XXXXXX)
    LL_IN_FIFO="${LL_TMP_DIR}/in"
    LL_OUT_FIFO="${LL_TMP_DIR}/out"
    mkfifo "${LL_IN_FIFO}" "${LL_OUT_FIFO}"

    # -tA: tuples-only, unaligned. -q: quiet (no banners between cmds).
    psql -p "${port}" -d "${TEST_DB}" -tAq \
        < "${LL_IN_FIFO}" > "${LL_OUT_FIFO}" 2>&1 &
    LL_PID=$!

    # Hold input fifo open via fd 9 so psql doesn't see EOF.
    exec 9>"${LL_IN_FIFO}"

    # Open output FIFO on fd 8 so reads don't block on writer churn.
    exec 8<"${LL_OUT_FIFO}"
}

# Send SQL, read until sentinel. Returns query output (may be
# multiple lines, may be empty). Caller checks for ERROR: lines.
long_lived_query() {
    local sql=$1
    local sentinel="__pg_ts_sentinel_$$_${RANDOM}__"
    echo "$sql" >&9
    echo "SELECT '${sentinel}';" >&9

    local result=""
    local line
    while IFS= read -r line <&8; do
        if [ "$line" = "$sentinel" ]; then
            break
        fi
        if [ -n "$result" ]; then
            result="${result}"$'\n'"${line}"
        else
            result="$line"
        fi
    done
    printf '%s' "$result"
}

long_lived_close() {
    exec 9>&-
    exec 8<&-
    wait "${LL_PID}" 2>/dev/null || true
    rm -rf "${LL_TMP_DIR}"
    LL_PID=""
}

# Helper that wraps the full pattern: open standby, query before,
# do something on primary, query after on the SAME backend, return
# both counts via globals LL_BEFORE and LL_AFTER, close.
#
# Args:
#   $1: port to open long-lived session on
#   $2: query to run before+after (must return a single value)
#   $3: bash command to execute between the two queries
long_lived_before_after() {
    local port=$1
    local query=$2
    local between=$3

    long_lived_open "${port}"
    LL_BEFORE=$(long_lived_query "${query}")
    eval "${between}"
    LL_AFTER=$(long_lived_query "${query}")
    long_lived_close
}

# -------------------------------------------------------------------
# Misc utilities.
# -------------------------------------------------------------------
check_required_tools() {
    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"
    command -v pg_basebackup >/dev/null 2>&1 || \
        error "pg_basebackup not found"
    command -v initdb >/dev/null 2>&1 || error "initdb not found"
}
