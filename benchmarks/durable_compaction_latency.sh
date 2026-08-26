#!/bin/bash
#
# Insert-latency benchmark for background BM25 compaction via
# pg_durable (see docs/background_compaction.md and
# scripts/durable_compaction/). Measures the wall-clock cost of an
# INSERT-that-triggers-a-spill transaction under
# pg_textsearch.compaction_mode = 'inline' vs 'background', at one
# or more corpus scales, and reports p50/p99 per scale.
#
# This is modeled directly on test/scripts/durable_compaction.sh: it
# brings up its own dedicated PostgreSQL instance with both
# pg_durable and pg_textsearch preloaded, wires up the same three
# glue scripts an operator would use, then times individual
# spill-round transactions instead of asserting behavior.
#
# This is NOT part of `make test-all` or any CI target: it requires
# pg_durable to be installed (module + control/SQL files discoverable
# via pg_config) and reports numbers, it does not assert pass/fail.
# Run it directly:
#
#   benchmarks/durable_compaction_latency.sh
#
# Override the round counts measured at each scale (space-separated,
# each run separately for inline and background):
#
#   BENCH_ROUNDS="10 50 200" benchmarks/durable_compaction_latency.sh
#
# IMPORTANT -- read before quoting these numbers: p50/p99 here are
# the cost of ONE spill-round transaction (20-row INSERT plus the
# spill it triggers), not raw single-row INSERT latency, and they
# reflect this machine's disk/CPU at the time of the run. Do not
# extrapolate the reported numbers to a different scale than the one
# actually measured -- rerun at the scale you care about instead.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GLUE_DIR="${SCRIPT_DIR}/../scripts/durable_compaction"
BENCH_PORT=55448
BENCH_DB=durable_compaction_bench
DATA_DIR="${SCRIPT_DIR}/tmp_durable_compaction_bench"
LOGFILE="${DATA_DIR}/postgres.log"
KEEP_DIR="${SCRIPT_DIR}/tmp_durable_compaction_bench_logs"
BENCH_ROUNDS="${BENCH_ROUNDS:-10 50 200}"
BENCH_SEGMENTS_PER_LEVEL="${BENCH_SEGMENTS_PER_LEVEL:-4}"

# Pin every binary to the installation pg_config points at -- the
# same convention as test/scripts/durable_compaction.sh and
# shutdown_spill.sh. An ambient PATH can silently resolve to a system
# Postgres with neither extension.
PGBINDIR="$(pg_config --bindir)"
PKGLIBDIR="$(pg_config --pkglibdir)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    local exit_code=$?
    log "Cleaning up (exit code: $exit_code)..."
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate \
            &>/dev/null || true
    fi
    if [ "$exit_code" -ne 0 ] && [ -d "${DATA_DIR}" ]; then
        rm -rf "${KEEP_DIR}"
        mkdir -p "${KEEP_DIR}"
        cp "${LOGFILE}" "${KEEP_DIR}/" 2>/dev/null || true
        warn "Preserved server log in ${KEEP_DIR}"
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------
# psql / GUC helpers (same shape as test/scripts/durable_compaction.sh)
# ---------------------------------------------------------------------

sql_as() {
    local role="$1"
    shift
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${BENCH_PORT}" -U "${role}" \
        -d "${BENCH_DB}" -qAt -v ON_ERROR_STOP=1 "$@"
}

sql_super() { sql_as postgres "$@"; }

set_guc() {
    # ALTER SYSTEM cannot run inside a transaction block, and psql
    # folds multiple -c statements into an implicit transaction, so
    # this must be two separate invocations.
    sql_super -c "ALTER SYSTEM SET ${1} = '${2}';" >/dev/null
    sql_super -c "SELECT pg_reload_conf();" >/dev/null
}

# ---------------------------------------------------------------------
# Cluster setup -- mirrors test/scripts/durable_compaction.sh
# ---------------------------------------------------------------------

setup_bench_cluster() {
    log "Setting up dedicated PostgreSQL instance on port \
${BENCH_PORT}..."

    local lib
    for lib in pg_durable pg_textsearch; do
        if [ ! -f "${PKGLIBDIR}/${lib}.so" ]; then
            error "${lib}.so not found in ${PKGLIBDIR}. This \
benchmark needs both pg_durable and pg_textsearch installed into \
the Postgres that '$(command -v pg_config)' points at. Put the \
intended installation's bin directory first on PATH, or run \
'make install'."
        fi
    done

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    "${PGBINDIR}/initdb" -D "${DATA_DIR}" -U postgres \
        --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${BENCH_PORT}
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_durable,pg_textsearch'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
pg_durable.database = '${BENCH_DB}'
EOF

    cat > "${DATA_DIR}/pg_hba.conf.new" << EOF
local   all   textsearch_compactor   trust
host    all   textsearch_compactor   127.0.0.1/32   trust
EOF
    cat "${DATA_DIR}/pg_hba.conf" >> "${DATA_DIR}/pg_hba.conf.new"
    mv "${DATA_DIR}/pg_hba.conf.new" "${DATA_DIR}/pg_hba.conf"

    "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" -l "${LOGFILE}" -w \
        -o "-p ${BENCH_PORT}" \
        || error "Failed to start PostgreSQL"

    "${PGBINDIR}/createdb" -h "${DATA_DIR}" -p "${BENCH_PORT}" \
        -U postgres "${BENCH_DB}"
    sql_super -c "CREATE EXTENSION pg_durable;"
    sql_super -c "CREATE EXTENSION pg_textsearch;"
    log "Bench cluster ready (db=${BENCH_DB})"
}

setup_roles_and_glue() {
    log "Creating app_owner and wiring the glue scripts..."
    sql_super -c "CREATE ROLE app_owner LOGIN;"

    sql_super -f "${GLUE_DIR}/01_setup_role.sql" -v index_owner=app_owner \
        >/dev/null
    sql_super -f "${GLUE_DIR}/02_wrapper.sql" >/dev/null
    sql_super -c "GRANT EXECUTE ON FUNCTION \
public.bm25_request_compaction(regclass) TO app_owner;"

    set_guc pg_textsearch.compaction_request_function \
        'public.bm25_request_compaction'
    set_guc pg_textsearch.segments_per_level "${BENCH_SEGMENTS_PER_LEVEL}"
    # Set on the compactor role too -- see docs/background_compaction.md
    # ("The GUC-scope trap"). The writer's ALTER SYSTEM above already
    # covers this cluster-wide, but this documents the required extra
    # step for a deployment that instead uses a per-session SET.
    sql_super -c "ALTER ROLE textsearch_compactor SET \
pg_textsearch.segments_per_level = ${BENCH_SEGMENTS_PER_LEVEL};" \
        >/dev/null
}

# The pg_durable background worker initializes ASYNCHRONOUSLY after
# CREATE EXTENSION. Retry a trivial probe submission until it both
# succeeds and reaches 'completed', bounded so this cannot hang.
wait_for_durable_worker() {
    log "Waiting for the pg_durable background worker..."
    local attempt id st
    for attempt in $(seq 1 60); do
        id=$(sql_as textsearch_compactor -c \
            "SELECT df.start('SELECT 1', \
                label => 'bench-probe-${attempt}');" \
            2>/dev/null || true)
        if [ -n "$id" ]; then
            local i
            for i in $(seq 1 20); do
                st=$(sql_as textsearch_compactor -c \
                    "SELECT status FROM df.instances WHERE id = \
'${id}';" 2>/dev/null || true)
                [ "$st" = "completed" ] && \
                    log "pg_durable worker ready" && return 0
                [ "$st" = "failed" ] && break
                sleep 1
            done
        fi
        sleep 1
    done
    error "pg_durable background worker never became ready"
}

create_owned_table() {
    local table="$1" idx="$2"
    sql_super << SQL
SET client_min_messages = warning;
CREATE TABLE ${table} (id serial PRIMARY KEY, body text);
CREATE INDEX ${idx} ON ${table}
    USING bm25(body) WITH (text_config = 'english');
ALTER TABLE ${table} OWNER TO app_owner;
ALTER INDEX ${idx} OWNER TO app_owner;
SQL
}

label_for() {
    sql_super -c "SELECT 'bm25-compact-' || '${1}'::regclass::oid;"
}

# Run one spill-round transaction (20-row INSERT + bm25_spill_index())
# and echo its wall-clock duration in seconds, high resolution.
time_one_round() {
    local idx="$1" table="$2" n="$3"
    local start end
    start=$(date +%s.%N)
    sql_as app_owner << SQL >/dev/null
SET client_min_messages = warning;
BEGIN;
INSERT INTO ${table} (body)
SELECT format('doc %s round %s filler filler filler', i, ${n})
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('${idx}');
COMMIT;
SQL
    end=$(date +%s.%N)
    awk "BEGIN { printf \"%.6f\", ${end} - ${start} }"
}

# Compute a percentile (0-100) over a newline-separated list of
# numbers read from stdin, using nearest-rank on the sorted list.
percentile() {
    local p="$1"
    sort -n | awk -v p="$p" '
        { a[NR] = $1 }
        END {
            if (NR == 0) { print "nan"; exit }
            idx = int((p / 100.0) * NR + 0.9999)
            if (idx < 1) idx = 1
            if (idx > NR) idx = NR
            printf "%.6f", a[idx]
        }'
}

# Time $rounds spill-round transactions under the given mode against
# a fresh table, and report p50/p99 (seconds).
bench_mode() {
    local mode="$1" rounds="$2"
    local table="bench_${mode}_${rounds}"
    local idx="${table}_idx"
    create_owned_table "$table" "$idx"
    set_guc pg_textsearch.compaction_mode "$mode"

    local samples_file="${DATA_DIR}/samples_${mode}_${rounds}.txt"
    : > "$samples_file"

    local n
    for n in $(seq 1 "$rounds"); do
        time_one_round "$idx" "$table" "$n" >> "$samples_file"
        echo >> "$samples_file"
    done

    local p50 p99
    p50=$(percentile 50 < "$samples_file")
    p99=$(percentile 99 < "$samples_file")
    echo "${p50} ${p99}"

    # Drain any background compaction this table's spills queued, so
    # it does not bleed CPU/IO into the next mode's measurement.
    if [ "$mode" = "background" ]; then
        local label id st waited=0
        label=$(label_for "$idx")
        id=$(sql_super -c \
            "SELECT id FROM df.instances WHERE label = '${label}' \
             ORDER BY created_at DESC LIMIT 1;")
        if [ -n "$id" ]; then
            while [ "$waited" -lt 30 ]; do
                st=$(sql_super -c \
                    "SELECT status FROM df.instances WHERE id = \
'${id}';")
                [ "$st" = "completed" ] || [ "$st" = "failed" ] && break
                sleep 1
                waited=$((waited + 1))
            done
        fi
    fi
}

# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------

setup_bench_cluster
setup_roles_and_glue
wait_for_durable_worker

log "pg_textsearch.segments_per_level = ${BENCH_SEGMENTS_PER_LEVEL}"
log "Round counts to measure: ${BENCH_ROUNDS}"
echo ""
printf "%-8s %-10s %-12s %-12s\n" "rounds" "mode" "p50 (s)" "p99 (s)"
printf "%-8s %-10s %-12s %-12s\n" "------" "----" "-------" "-------"

for rounds in $BENCH_ROUNDS; do
    read -r inline_p50 inline_p99 <<< "$(bench_mode inline "$rounds")"
    printf "%-8s %-10s %-12s %-12s\n" "$rounds" "inline" \
        "$inline_p50" "$inline_p99"

    read -r bg_p50 bg_p99 <<< "$(bench_mode background "$rounds")"
    printf "%-8s %-10s %-12s %-12s\n" "$rounds" "background" \
        "$bg_p50" "$bg_p99"
done

echo ""
log "Done. These numbers are from THIS run, on this machine, at the \
round counts above -- do not extrapolate to other scales without \
rerunning."
