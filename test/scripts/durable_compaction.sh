#!/bin/bash
#
# End-to-end test for background BM25 segment compaction driven by
# pg_durable (see docs/superpowers/plans/2026-08-26-background-compaction-
# pg-durable.md, Task 7, and scripts/durable_compaction/README.md for the
# mechanism this exercises).
#
# This test brings up its OWN PostgreSQL instance with both pg_durable and
# pg_textsearch preloaded, wires up the three glue scripts in
# scripts/durable_compaction/ exactly as an operator would, and then
# verifies every criterion from the plan's "acceptance criteria" section:
#
#   1. Atomicity        - a rolled-back spill enqueues nothing; a
#                          committed spill enqueues exactly one instance.
#   2. It actually compacts - background mode really drops segment counts.
#   3. Write latency     - inline vs background wall-clock is recorded.
#   4. Failure path       - a compactor without ownership fails the
#                          instance; restoring ownership self-heals on the
#                          next spill.
#   5. Cascade splits across transactions - a multi-level cascade shows up
#                          as more than one node execution in the server
#                          log, and a concurrent writer can commit while
#                          the cascade is still running.
#   6. Permissions        - a writer with ONLY EXECUTE on
#                          bm25_request_compaction (no df privileges at
#                          all) can still enqueue a task, attributed to
#                          textsearch_compactor.
#   7. Backstop           - the scheduled sweep compacts an index with
#                          compaction_mode = 'off' and no writer
#                          involvement at all.
#
# This test requires the pg_durable extension to be installed
# (module + control/SQL files discoverable via pg_config). It is NOT part
# of `make test-all`: CI has no pg_durable. Run it explicitly with
# `make test-durable`.
#
# Every pg_textsearch compaction GUC (segments_per_level, compaction_mode,
# compaction_request_function, memtable_pages_threshold) is PGC_SUSET, and
# critically is read by TWO different sessions: the writer's (to decide
# whether to enqueue) and the compactor's worker session (to decide
# whether df.loop's condition keeps stepping). This script always changes
# them via `ALTER SYSTEM ... ; SELECT pg_reload_conf()` as the superuser,
# which both sessions pick up -- never a plain per-session SET, which the
# worker session would never see. See scripts/durable_compaction/README.md
# ("Set the compaction threshold where the worker can see it").

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GLUE_DIR="${SCRIPT_DIR}/../../scripts/durable_compaction"
TEST_PORT=55447
TEST_DB=durable_compaction_test
DATA_DIR="${SCRIPT_DIR}/../tmp_durable_compaction"
LOGFILE="${DATA_DIR}/postgres.log"
KEEP_DIR="${SCRIPT_DIR}/../tmp_durable_compaction_logs"
TEST_SIZE_MULTIPLIER=${TEST_SIZE_MULTIPLIER:-1.0}

# Pin every binary to the installation pg_config points at, matching
# the convention in shutdown_spill.sh. This must be the same install
# `make install` targeted, because that is the only $libdir where
# pg_textsearch.so is discoverable -- relying on an ambient PATH
# silently picks up a system Postgres that has neither extension and
# fails much later with a bare "could not access file".
PGBINDIR="$(pg_config --bindir)"
PKGLIBDIR="$(pg_config --pkglibdir)"

# Scale a loop count by TEST_SIZE_MULTIPLIER (minimum 1).
scaled_count() {
    awk "BEGIN {n = int($1 * ${TEST_SIZE_MULTIPLIER} + 0.5);
                print (n < 1 ? 1 : n)}"
}

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
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
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
# psql helpers
# ---------------------------------------------------------------------

sql_as() {
    local role="$1"
    shift
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" -U "${role}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 "$@"
}

sql_super() { sql_as postgres "$@"; }

set_guc() {
    # ALTER SYSTEM cannot run inside a transaction block, and psql folds
    # multiple statements passed to one -c into an implicit transaction
    # -- so this must be two separate -c invocations, not one.
    sql_super -c "ALTER SYSTEM SET ${1} = '${2}';" >/dev/null
    sql_super -c "SELECT pg_reload_conf();" >/dev/null
}

reset_guc() {
    sql_super -c "ALTER SYSTEM RESET ${1};" >/dev/null
    sql_super -c "SELECT pg_reload_conf();" >/dev/null
}

# Exact label pg_textsearch/bm25_request_compaction uses for an index.
label_for() {
    sql_super -c "SELECT 'bm25-compact-' || '${1}'::regclass::oid;"
}

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" != "$actual" ]; then
        error "ASSERTION FAILED: ${desc} (expected '${expected}', got \
'${actual}')"
    fi
    log "PASS: ${desc} (= ${actual})"
}

assert_true() {
    local desc="$1" actual="$2"
    if [ "$actual" != "t" ]; then
        error "ASSERTION FAILED: ${desc} (expected t, got '${actual}')"
    fi
    log "PASS: ${desc}"
}

# Poll df.instances (as superuser, which bypasses its RLS) until the
# given instance id reaches a terminal state or the timeout elapses.
wait_for_instance() {
    local id="$1" timeout="$2"
    local waited=0
    local st=""
    while [ "$waited" -lt "$timeout" ]; do
        st=$(sql_super -c \
            "SELECT status FROM df.instances WHERE id = '${id}';")
        if [ "$st" = "completed" ] || [ "$st" = "failed" ]; then
            echo "$st"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "instance ${id} did not reach a terminal state within \
${timeout}s (last status: '${st}')"
}

# Count distinct orchestration generations (execution_id values) the
# server log recorded for a durable instance. df.nodes is NOT usable for
# this: pg_durable reuses node rows across continue_as_new generations, so
# it always shows one row per node type regardless of how many iterations
# ran (see scripts/durable_compaction/README.md, "Known limitations").
count_generations() {
    grep -F "instance_id=${1} execution_id=" "${LOGFILE}" \
        | grep -oE 'execution_id=[0-9]+' \
        | sort -u | wc -l
}

# ---------------------------------------------------------------------
# Cluster setup
# ---------------------------------------------------------------------

setup_test_cluster() {
    log "Setting up dedicated PostgreSQL instance on port ${TEST_PORT}..."

    # Fail early and legibly if either module is missing from the
    # install pg_config points at. Without this the cluster dies at
    # startup with only "could not access file" in a log the caller
    # has to go hunting for.
    local lib
    for lib in pg_durable pg_textsearch; do
        if [ ! -f "${PKGLIBDIR}/${lib}.so" ]; then
            error "${lib}.so not found in ${PKGLIBDIR}. This test \
needs both pg_durable and pg_textsearch installed into the Postgres \
that '$(command -v pg_config)' points at. Put the intended \
installation's bin directory first on PATH, or run 'make install'."
        fi
    done

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    # -U postgres: pg_durable.worker_role defaults to 'postgres' and is
    # used for the background worker's own internal connection, distinct
    # from the per-node submitted_by connection. Matching it to the
    # cluster's superuser avoids a "role does not exist" surprise.
    "${PGBINDIR}/initdb" -D "${DATA_DIR}" -U postgres \
        --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_durable,pg_textsearch'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
pg_durable.database = '${TEST_DB}'
EOF

    # Per scripts/durable_compaction/01_setup_role.sql: the worker opens a
    # fresh libpq connection as df.instances.submitted_by for every node,
    # with no password and no host -- it needs a passwordless route.
    # initdb's --auth-local=trust already makes the whole pg_hba.conf
    # trust-all for this throwaway cluster, so these lines are not load-
    # bearing here, but they document (and would keep working under) the
    # tighter, role-scoped hba the README actually recommends for a real
    # deployment -- prepended above the general rules, as instructed.
    cat > "${DATA_DIR}/pg_hba.conf.new" << EOF
local   all   textsearch_compactor   trust
host    all   textsearch_compactor   127.0.0.1/32   trust
EOF
    cat "${DATA_DIR}/pg_hba.conf" >> "${DATA_DIR}/pg_hba.conf.new"
    mv "${DATA_DIR}/pg_hba.conf.new" "${DATA_DIR}/pg_hba.conf"

    "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" -l "${LOGFILE}" -w -o "-p ${TEST_PORT}" \
        || error "Failed to start PostgreSQL"

    "${PGBINDIR}/createdb" -h "${DATA_DIR}" -p "${TEST_PORT}" -U postgres "${TEST_DB}"
    sql_super -c "CREATE EXTENSION pg_durable;"
    sql_super -c "CREATE EXTENSION pg_textsearch;"
    log "Test cluster ready (db=${TEST_DB})"
}

setup_roles_and_glue() {
    log "Creating app_owner / app_writer and wiring the glue scripts..."
    sql_super -c "CREATE ROLE app_owner LOGIN;"
    sql_super -c "CREATE ROLE app_writer LOGIN;"

    sql_super -f "${GLUE_DIR}/01_setup_role.sql" -v index_owner=app_owner \
        >/dev/null
    sql_super -f "${GLUE_DIR}/02_wrapper.sql" -v writer_role=app_writer \
        >/dev/null

    # app_owner is used as the general spilling writer in most scenarios
    # below (it must own the index to call bm25_spill_index() at all);
    # it needs the same EXECUTE grant 02_wrapper.sql gave app_writer, or
    # its own PRE_COMMIT dispatch is denied.
    sql_super -c "GRANT EXECUTE ON FUNCTION \
public.bm25_request_compaction(regclass) TO app_owner;"

    set_guc pg_textsearch.compaction_request_function \
        'public.bm25_request_compaction'
    set_guc pg_textsearch.compaction_mode 'background'
    set_guc pg_textsearch.segments_per_level '2'
}

# The pg_durable background worker initializes ASYNCHRONOUSLY after
# CREATE EXTENSION. Submitting before it is ready leaves an instance
# stuck 'pending' forever (see README, "The background worker initializes
# asynchronously"). Retry a trivial probe submission until it both
# succeeds and actually reaches 'completed'.
wait_for_durable_worker() {
    log "Waiting for the pg_durable background worker..."
    local attempt id st
    for attempt in $(seq 1 60); do
        id=$(sql_as textsearch_compactor -c \
            "SELECT df.start('SELECT 1', label => 'probe-ready-${attempt}');" \
            2>/dev/null || true)
        if [ -n "$id" ]; then
            local i
            for i in $(seq 1 20); do
                st=$(sql_as textsearch_compactor -c \
                    "SELECT status FROM df.instances WHERE id = '${id}';" \
                    2>/dev/null || true)
                if [ "$st" = "completed" ]; then
                    log "pg_durable worker ready (probe ${id} completed)"
                    return 0
                fi
                [ "$st" = "failed" ] && break
                sleep 1
            done
        fi
        sleep 1
    done
    error "pg_durable background worker never became ready"
}

# Build ${1} rounds of INSERT + bm25_spill_index() on ${3} against index
# ${2} inside a single transaction, as ${4} (the index owner, since
# bm25_spill_index() requires ownership). Returns nothing; caller reads
# df.instances afterwards.
spill_rounds() {
    local role="$1" idx="$2" table="$3" rounds="$4"
    sql_as "$role" << SQL
SET client_min_messages = warning;
BEGIN;
DO \$body\$
BEGIN
    FOR n IN 1..${rounds} LOOP
        INSERT INTO ${table} (body)
        SELECT format('doc %s round %s filler filler filler', i, n)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('${idx}');
    END LOOP;
END
\$body\$;
COMMIT;
SQL
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

# ---------------------------------------------------------------------
# Test 1: Atomicity
# ---------------------------------------------------------------------

test_atomicity() {
    log "=== Test: atomicity ==="
    create_owned_table t_atomic t_atomic_idx
    local label
    label=$(label_for t_atomic_idx)

    sql_as app_owner << SQL >/dev/null
SET client_min_messages = warning;
BEGIN;
INSERT INTO t_atomic (body)
SELECT 'rollback doc ' || i || ' filler filler filler'
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_atomic_idx');
ROLLBACK;
SQL
    local rollback_count
    rollback_count=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "rollback enqueues nothing" "0" "$rollback_count"

    spill_rounds app_owner t_atomic_idx t_atomic 1
    local commit_count
    commit_count=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "commit enqueues exactly one instance" "1" "$commit_count"

    local id
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}';")
    wait_for_instance "$id" 30 >/dev/null
}

# ---------------------------------------------------------------------
# Test 2: it actually compacts
# ---------------------------------------------------------------------

test_actually_compacts() {
    log "=== Test: background mode actually compacts ==="
    create_owned_table t_actual t_actual_idx
    local label
    label=$(label_for t_actual_idx)

    spill_rounds app_owner t_actual_idx t_actual 2
    local id
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}';")

    local l0_before
    l0_before=$(sql_super -c \
        "SELECT (bm25_level_counts('t_actual_idx'::regclass))[1];")

    local st
    st=$(wait_for_instance "$id" 30)
    assert_eq "compaction instance completes" "completed" "$st"

    local l0_after
    l0_after=$(sql_super -c \
        "SELECT (bm25_level_counts('t_actual_idx'::regclass))[1];")

    # Never assert only "completed" for a compaction case: that alone
    # passes vacuously if the worker session never saw the lowered
    # threshold (the GUC-scope trap). Require the level counts to have
    # actually moved.
    if [ "$l0_after" -ge "$l0_before" ]; then
        error "ASSERTION FAILED: L0 count did not drop \
(before=${l0_before}, after=${l0_after})"
    fi
    log "PASS: L0 segment count dropped (${l0_before} -> ${l0_after})"
}

# ---------------------------------------------------------------------
# Test 3: write latency, inline vs background
# ---------------------------------------------------------------------

time_rounds() {
    local mode="$1" idx="$2" table="$3" rounds="$4"
    set_guc pg_textsearch.compaction_mode "$mode"
    local start end
    start=$(date +%s.%N)
    spill_rounds app_owner "$idx" "$table" "$rounds"
    end=$(date +%s.%N)
    awk "BEGIN { printf \"%.3f\", ${end} - ${start} }"
}

test_write_latency() {
    log "=== Test: write latency, inline vs background ==="
    create_owned_table t_latency_inline t_latency_inline_idx
    create_owned_table t_latency_bg t_latency_bg_idx
    local rounds
    rounds=$(scaled_count 10)

    local inline_seconds
    inline_seconds=$(time_rounds inline t_latency_inline_idx \
        t_latency_inline "$rounds")
    local bg_seconds
    bg_seconds=$(time_rounds background t_latency_bg_idx \
        t_latency_bg "$rounds")

    # Restore the mode every other test relies on.
    set_guc pg_textsearch.compaction_mode background

    log "RESULT: ${rounds} spill rounds, inline=${inline_seconds}s \
background=${bg_seconds}s"

    # A meaningful, non-flaky assertion: both numbers must actually have
    # been measured (not empty, not zero/negative) -- the comparison
    # itself is recorded above for human review rather than asserted,
    # since relative timing under shared-CI load is inherently noisy.
    for pair in "inline:${inline_seconds}" "background:${bg_seconds}"; do
        local name="${pair%%:*}" val="${pair#*:}"
        if ! awk "BEGIN { exit !(${val} > 0) }"; then
            error "ASSERTION FAILED: ${name} duration not a positive \
number (got '${val}')"
        fi
    done
    log "PASS: both inline and background durations were measured"

    # Let the background instance for t_latency_bg finish draining so it
    # does not linger into later log-grep-based assertions.
    local label id
    label=$(label_for t_latency_bg_idx)
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    [ -n "$id" ] && wait_for_instance "$id" 30 >/dev/null
}

# ---------------------------------------------------------------------
# Test 4: failure path
# ---------------------------------------------------------------------

test_failure_path() {
    log "=== Test: failure path (ownership revoked) ==="
    create_owned_table t_failure t_failure_idx
    local label
    label=$(label_for t_failure_idx)

    # bm25_compact_step() gates on object_ownercheck(); revoking the
    # compactor's inherited membership in app_owner makes that check
    # fail inside the worker's own session, with no debug GUC needed.
    sql_super -c "REVOKE app_owner FROM textsearch_compactor;"

    spill_rounds app_owner t_failure_idx t_failure 2
    local id st
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    st=$(wait_for_instance "$id" 30)
    assert_eq "instance fails without ownership" "failed" "$st"

    local l0_unchanged
    l0_unchanged=$(sql_super -c \
        "SELECT (bm25_level_counts('t_failure_idx'::regclass))[1];")
    if [ "$l0_unchanged" -lt 2 ]; then
        error "ASSERTION FAILED: a failed compact_step must not have \
merged anything (L0 = ${l0_unchanged})"
    fi
    log "PASS: failed instance left level counts untouched (L0 = \
${l0_unchanged})"

    # Restore ownership; the next spill self-heals (README, "Known
    # limitations: No retry" -- next-spill retry is layer 1).
    sql_super -c "GRANT app_owner TO textsearch_compactor;"

    spill_rounds app_owner t_failure_idx t_failure 1
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    st=$(wait_for_instance "$id" 30)
    assert_eq "next instance succeeds after re-grant" "completed" "$st"

    local l0_after
    l0_after=$(sql_super -c \
        "SELECT (bm25_level_counts('t_failure_idx'::regclass))[1];")
    if [ "$l0_after" -ge "$l0_unchanged" ]; then
        error "ASSERTION FAILED: level counts did not change on the \
successful retry (still ${l0_after})"
    fi
    log "PASS: retry actually compacted (L0 ${l0_unchanged} -> \
${l0_after})"
}

# ---------------------------------------------------------------------
# Test 5: cascade splits across transactions
# ---------------------------------------------------------------------

test_cascade() {
    log "=== Test: cascade splits across transactions ==="
    create_owned_table t_cascade t_cascade_idx
    local label
    label=$(label_for t_cascade_idx)

    # 8 L0 segments at segments_per_level=2 cascades L0 -> L1 -> L2 -> L3
    # over 7 df.loop iterations (verified interactively: {8,0,...} ->
    # {0,0,0,1,0,0,0,0}, execution_id 1..7 in the server log).
    spill_rounds app_owner t_cascade_idx t_cascade 8

    local id
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")

    # Wait until the cascade is actually running (it starts almost
    # immediately), then fire a concurrent writer against the SAME index
    # while the cascade is still in flight. Each compact_step only holds
    # the per-index LW_EXCLUSIVE for the duration of one merge batch and
    # releases it between df.loop iterations (~1s apart, 7 iterations),
    # so this commit should land in well under a second -- proving the
    # writer was not blocked for the whole ~7s+ cascade.
    local w=0
    local st="pending"
    while [ "$w" -lt 10 ]; do
        st=$(sql_super -c \
            "SELECT status FROM df.instances WHERE id = '${id}';")
        [ "$st" = "running" ] && break
        sleep 0.3
        w=$((w + 1))
    done

    local concurrent_start concurrent_end
    concurrent_start=$(date +%s.%N)
    sql_as app_owner << SQL >/dev/null
SET client_min_messages = warning;
INSERT INTO t_cascade (body)
SELECT 'concurrent doc ' || i || ' filler filler filler'
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('t_cascade_idx');
SQL
    concurrent_end=$(date +%s.%N)

    local status_right_after
    status_right_after=$(sql_super -c \
        "SELECT status FROM df.instances WHERE id = '${id}';")

    local concurrent_seconds
    concurrent_seconds=$(awk \
        "BEGIN { printf \"%.3f\", ${concurrent_end} - ${concurrent_start} }")
    log "Concurrent writer commit took ${concurrent_seconds}s; cascade \
status right after was '${status_right_after}'"

    if [ "$status_right_after" = "completed" ] \
        || [ "$status_right_after" = "failed" ]; then
        error "ASSERTION FAILED: concurrent writer only committed after \
the cascade had already finished -- this proves nothing about \
non-blocking behavior"
    fi
    log "PASS: a concurrent writer committed while the cascade instance \
was still '${status_right_after}' (not blocked for the whole cascade)"

    local final_status
    final_status=$(wait_for_instance "$id" 60)
    assert_eq "cascade instance completes" "completed" "$final_status"

    local generations
    generations=$(count_generations "$id")
    if [ "$generations" -le 1 ]; then
        error "ASSERTION FAILED: expected more than one node execution \
for the cascade, server log shows ${generations}"
    fi
    log "PASS: cascade recorded ${generations} distinct node executions \
(execution_id generations) in the server log"

    local level_counts
    level_counts=$(sql_super -c \
        "SELECT bm25_level_counts('t_cascade_idx'::regclass)::text;")
    log "Final level counts after cascade: ${level_counts}"
    # {0,...} at L0/L1/L2 with something promoted at L3+ proves the
    # cascade crossed more than one level, not just a single L0->L1 hop.
    local l3_or_higher
    l3_or_higher=$(sql_super -c \
        "SELECT (bm25_level_counts('t_cascade_idx'::regclass))[4] > 0;")
    assert_true "cascade promoted a segment to L3 or higher" \
        "$l3_or_higher"
}

# ---------------------------------------------------------------------
# Test 6: permissions
# ---------------------------------------------------------------------

test_permissions() {
    log "=== Test: writer permissions (EXECUTE-only) ==="
    create_owned_table t_perm t_perm_idx
    sql_super << SQL
GRANT INSERT, SELECT ON t_perm TO app_writer;
GRANT USAGE ON SEQUENCE t_perm_id_seq TO app_writer;
SQL
    local label
    label=$(label_for t_perm_idx)

    # Confirm app_writer genuinely has no df privileges at all: a direct
    # df.* call must fail with a permission error.
    if sql_as app_writer -c "SELECT df.start('SELECT 1');" \
        >/dev/null 2>&1; then
        error "ASSERTION FAILED: app_writer must NOT be able to call \
df.start() directly (it holds no df schema privileges)"
    fi
    log "PASS: app_writer cannot call df.* directly (permission denied)"

    # app_writer is not the index owner, so it cannot call
    # bm25_spill_index() directly either -- it must rely on ordinary
    # auto-spill during a plain INSERT. tp_aminsert's auto-spill check
    # re-fires per tuple, so an overly low page threshold (e.g. 1) makes
    # a single large INSERT spill dozens of times over (each spill
    # resets the chain and immediately starts refilling it), producing
    # a pile of tiny L0 segments whose df.loop cascade does not converge
    # inside any reasonable timeout. threshold=4 was measured to cross
    # exactly once for this batch, giving one predictable L0 segment
    # (well under segments_per_level=2, so the enqueued instance is a
    # cheap single-iteration no-op) -- this test only cares whether a
    # request is enqueued and correctly attributed, not whether real
    # compaction work happens.
    set_guc pg_textsearch.memtable_pages_threshold '4'

    sql_as app_writer << SQL
SET client_min_messages = warning;
BEGIN;
INSERT INTO t_perm (body)
SELECT 'writer doc ' || i || ' filler filler filler filler filler'
FROM generate_series(1, 400) i;
COMMIT;
SQL

    reset_guc pg_textsearch.memtable_pages_threshold

    local id submitted_by
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    if [ -z "$id" ]; then
        error "ASSERTION FAILED: no compaction instance was enqueued \
for a writer that only holds EXECUTE on bm25_request_compaction"
    fi
    log "PASS: unprivileged writer's insert still enqueued a task \
(${id})"

    submitted_by=$(sql_super -c \
        "SELECT submitted_by::text FROM df.instances WHERE id = '${id}';")
    assert_eq "instance is attributed to textsearch_compactor" \
        "textsearch_compactor" "$submitted_by"

    wait_for_instance "$id" 30 >/dev/null
}

# ---------------------------------------------------------------------
# Test 7: backstop (run LAST -- its per-minute sweep would otherwise
# compact every pending index in the database, corrupting the carefully
# staged segment counts the other tests depend on).
# ---------------------------------------------------------------------

test_backstop() {
    log "=== Test: backstop sweep (compaction_mode = 'off') ==="
    create_owned_table t_backstop t_backstop_idx
    local label
    label=$(label_for t_backstop_idx)

    set_guc pg_textsearch.compaction_mode 'off'

    spill_rounds app_owner t_backstop_idx t_backstop 2

    local writer_side_count
    writer_side_count=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "compaction_mode=off enqueues nothing from the write path" \
        "0" "$writer_side_count"

    local l0_before
    l0_before=$(sql_super -c \
        "SELECT (bm25_level_counts('t_backstop_idx'::regclass))[1];")
    if [ "$l0_before" -lt 2 ]; then
        error "ASSERTION FAILED: setup did not leave the index over \
threshold (L0 = ${l0_before})"
    fi

    sql_as textsearch_compactor -f "${GLUE_DIR}/03_backstop.sql" \
        -v cron='* * * * *' >/dev/null

    local backstop_id backstop_status
    backstop_id=$(sql_super -c \
        "SELECT id FROM df.instances \
         WHERE label = 'bm25-compaction-backstop' \
         ORDER BY created_at DESC LIMIT 1;")
    backstop_status=$(sql_super -c \
        "SELECT status FROM df.instances WHERE id = '${backstop_id}';")
    if [ "$backstop_status" != "running" ] \
        && [ "$backstop_status" != "pending" ]; then
        error "ASSERTION FAILED: backstop instance is not live \
(status=${backstop_status})"
    fi
    log "PASS: long-lived backstop instance ${backstop_id} is \
${backstop_status}"

    local waited=0
    local l0_after="$l0_before"
    while [ "$waited" -lt 130 ]; do
        l0_after=$(sql_super -c \
            "SELECT (bm25_level_counts('t_backstop_idx'::regclass))[1];")
        [ "$l0_after" -lt "$l0_before" ] && break
        sleep 5
        waited=$((waited + 5))
    done

    if [ "$l0_after" -ge "$l0_before" ]; then
        error "ASSERTION FAILED: backstop sweep never compacted \
t_backstop_idx within 130s (L0 stayed at ${l0_before})"
    fi
    log "PASS: backstop sweep compacted with no writer involvement \
(L0 ${l0_before} -> ${l0_after}) after waiting ${waited}s"

    sql_as textsearch_compactor -c \
        "SELECT df.cancel('${backstop_id}');" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------

setup_test_cluster
setup_roles_and_glue
wait_for_durable_worker

test_atomicity
test_actually_compacts
test_write_latency
test_failure_path
test_cascade
test_permissions
test_backstop

log "All tests passed!"
exit 0
