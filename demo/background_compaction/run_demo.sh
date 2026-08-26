#!/bin/bash
#
# Runnable demo for background BM25 segment compaction via pg_durable
# (see docs/superpowers/plans/2026-08-26-background-compaction-pg-durable.md,
# Task 9; docs/background_compaction.md for the architecture and the
# honest LW_EXCLUSIVE limitation; and test/scripts/durable_compaction.sh,
# whose cluster/role/glue setup this script's structure is modeled on).
#
# Tells the story in three acts:
#
#   Act 1 - a single-row INSERT that happens to cross the compaction
#           threshold blows through statement_timeout under
#           pg_textsearch.compaction_mode = 'inline'. The row is
#           rolled back, but -- measured on this machine, not assumed
#           -- the merge is NOT: segment merges are physical,
#           GenericXLog-logged page mutations with no undo log (like
#           a B-tree page split), so by the time the cancellation is
#           observed the cascade has already run to completion. The
#           client pays the full merge cost and still gets an error.
#   Act 2 - the identical insert, against a freshly rebuilt identical
#           corpus, under compaction_mode = 'background' commits well
#           inside the same statement_timeout. The merge itself still
#           happens -- out of band, driven by pg_durable -- and level
#           counts are shown to actually change.
#   Act 3 - a reader loop and a writer loop run concurrently with a
#           background cascade in flight, and every correctness
#           property is a hard assertion: ranking invariance, no torn
#           reads under REPEATABLE READ, transactional atomicity of
#           the enqueue, forward progress for a concurrent writer
#           (with its worst observed latency printed, unflattering or
#           not), and a final row-count sanity check.
#
# This demo is NOT part of `make test-all` or any CI target: it needs
# pg_durable installed (module + control/SQL files discoverable via
# pg_config), and it prints a narrated story rather than asserting a
# fixed pass/fail matrix the way test/scripts/durable_compaction.sh
# does. It DOES assert every correctness property in Act 3 and every
# expected outcome in Acts 1-2; a failed assertion exits non-zero.
#
# Run directly:
#
#   demo/background_compaction/run_demo.sh
#
# It brings up its own dedicated PostgreSQL instance (own data
# directory, own port -- neither 55447 nor 55448, which belong to
# test/scripts/durable_compaction.sh and
# benchmarks/durable_compaction_latency.sh respectively) and tears it
# down on exit via a trap.
#
# IMPORTANT: statement_timeout for Act 1 / Act 2 is NOT hardcoded. It
# is derived from a calibration run that measures the ACTUAL inline
# cascade time on this machine, right before Act 1 runs, so the demo
# stays honest on a slower or faster box. See calibrate_inline_cascade
# below.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_PORT=55449
DEMO_DB=background_compaction_demo
DATA_DIR="${SCRIPT_DIR}/tmp_demo_data"
LOGFILE="${DATA_DIR}/postgres.log"
KEEP_DIR="${SCRIPT_DIR}/tmp_demo_data_logs"

# Corpus shape. The task calibration measured 2.794s (inline) vs
# 0.097s (background) for a single triggering commit at exactly this
# scale (8 segments x 25,000 rows) with segments_per_level=2 -- this
# demo reproduces that effect at the same scale, but with a richer,
# per-document vocabulary (see demo_gen_body below) rather than one
# repeated phrase, so the merged term dictionary is larger and the
# effect is not an artifact of a trivially small dictionary.
DEMO_SEGMENTS=${DEMO_SEGMENTS:-8}
DEMO_ROWS_PER_SEGMENT=${DEMO_ROWS_PER_SEGMENT:-25000}
DEMO_SEGMENTS_PER_LEVEL=${DEMO_SEGMENTS_PER_LEVEL:-2}

# Pin every binary to the installation pg_config points at -- the
# same convention as test/scripts/durable_compaction.sh. An ambient
# PATH can silently resolve to a system Postgres with neither
# extension, failing much later with a bare "could not access file".
PGBINDIR="$(pg_config --bindir)"
PKGLIBDIR="$(pg_config --pkglibdir)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }
act() { echo -e "\n${CYAN}=== $1 ===${NC}"; }

cleanup() {
    local exit_code=$?
    log "Cleaning up (exit code: ${exit_code})..."
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
    exit "$exit_code"
}

trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------
# psql / GUC helpers (same shape as test/scripts/durable_compaction.sh)
# ---------------------------------------------------------------------

sql_as() {
    local role="$1"
    shift
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${DEMO_PORT}" -U "${role}" \
        -d "${DEMO_DB}" -qAt -v ON_ERROR_STOP=1 "$@"
}

sql_super() { sql_as postgres "$@"; }

set_guc() {
    # ALTER SYSTEM cannot run inside a transaction block, and psql
    # folds multiple -c statements passed to one invocation into an
    # implicit transaction -- so this must be two separate
    # invocations, not one. Every pg_textsearch compaction GUC is
    # PGC_SUSET, and critically is read by TWO sessions (the writer's,
    # to decide whether to enqueue, and the compactor worker's, to
    # decide whether to keep merging) -- ALTER SYSTEM is the only way
    # both see the same value. A plain per-session SET only reaches
    # the session that issues it.
    sql_super -c "ALTER SYSTEM SET ${1} = '${2}';" >/dev/null
    sql_super -c "SELECT pg_reload_conf();" >/dev/null
}

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

# Poll until df.instances.status leaves the pending/queued state (or
# reaches a terminal state first). Bounded so this cannot hang.
wait_for_running() {
    local id="$1" max_iters="$2"
    local i st=""
    for i in $(seq 1 "$max_iters"); do
        st=$(sql_super -c \
            "SELECT status FROM df.instances WHERE id = '${id}';")
        case "$st" in
            running|completed|failed) echo "$st"; return 0 ;;
        esac
        sleep 0.5
    done
    echo "$st"
}

# The pg_durable background worker initializes ASYNCHRONOUSLY after
# CREATE EXTENSION. Submitting before it is ready leaves an instance
# stuck 'pending' forever. Retry a trivial probe submission until it
# both succeeds and actually reaches 'completed'.
wait_for_durable_worker() {
    log "Waiting for the pg_durable background worker..."
    local attempt id st
    for attempt in $(seq 1 60); do
        id=$(sql_as textsearch_compactor -c \
            "SELECT df.start('SELECT 1', label => 'demo-probe-${attempt}');" \
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

# ---------------------------------------------------------------------
# Cluster setup
# ---------------------------------------------------------------------

setup_cluster() {
    log "Setting up dedicated PostgreSQL instance on port ${DEMO_PORT}..."

    local lib
    for lib in pg_durable pg_textsearch; do
        if [ ! -f "${PKGLIBDIR}/${lib}.so" ]; then
            error "${lib}.so not found in ${PKGLIBDIR}. This demo \
needs both pg_durable and pg_textsearch installed into the Postgres \
that '$(command -v pg_config)' points at. Put the intended \
installation's bin directory first on PATH, or run 'make install'."
        fi
    done

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    "${PGBINDIR}/initdb" -D "${DATA_DIR}" -U postgres \
        --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${DEMO_PORT}
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_durable,pg_textsearch'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
pg_durable.database = '${DEMO_DB}'
EOF

    # Passwordless route for the compactor: pg_durable's worker opens
    # a fresh libpq connection as df.instances.submitted_by with no
    # password and no host, so it hits the unix socket, where `peer`
    # fails for a role that is not the OS user.
    cat > "${DATA_DIR}/pg_hba.conf.new" << EOF
local   all   textsearch_compactor   trust
host    all   textsearch_compactor   127.0.0.1/32   trust
EOF
    cat "${DATA_DIR}/pg_hba.conf" >> "${DATA_DIR}/pg_hba.conf.new"
    mv "${DATA_DIR}/pg_hba.conf.new" "${DATA_DIR}/pg_hba.conf"

    "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" -l "${LOGFILE}" -w \
        -o "-p ${DEMO_PORT}" \
        || error "Failed to start PostgreSQL"

    "${PGBINDIR}/createdb" -h "${DATA_DIR}" -p "${DEMO_PORT}" \
        -U postgres "${DEMO_DB}"
    sql_super -c "CREATE EXTENSION pg_durable;"
    sql_super -c "CREATE EXTENSION pg_textsearch;"
    log "Demo cluster ready (db=${DEMO_DB})"
}

setup_roles_and_glue() {
    log "Creating demo_owner and wiring the glue scripts..."
    sql_super -c "CREATE ROLE demo_owner LOGIN;"

    local glue_dir="${SCRIPT_DIR}/../../scripts/durable_compaction"
    sql_super -f "${glue_dir}/01_setup_role.sql" -v index_owner=demo_owner \
        >/dev/null
    sql_super -f "${glue_dir}/02_wrapper.sql" -v writer_role=demo_owner \
        >/dev/null

    set_guc pg_textsearch.compaction_request_function \
        'public.bm25_request_compaction'
    set_guc pg_textsearch.segments_per_level "${DEMO_SEGMENTS_PER_LEVEL}"
    set_guc pg_textsearch.compaction_mode off

    # High-term-diversity document body generator. Each document is
    # 30 space-separated tokens drawn from a 64-word list combined
    # with a numeric suffix in [0, 400), so the merged dictionary can
    # have up to 64 * 400 = 25,600 distinct lexemes -- merge cost
    # scales with the term dictionary, not just byte count, and one
    # repeated phrase would badly understate the real cost. `seg`
    # varies the phase per spill round so segments do not all draw
    # from an identical sequence.
    sql_super << 'SQL' >/dev/null
CREATE OR REPLACE FUNCTION public.demo_gen_body(seg int, g int)
RETURNS text
LANGUAGE sql IMMUTABLE AS $genbody$
    SELECT string_agg(
        (ARRAY['alpha','bravo','charlie','delta','echo','foxtrot',
               'golf','hotel','india','juliet','kilo','lima','mike',
               'november','oscar','papa','quebec','romeo','sierra',
               'tango','uniform','victor','whiskey','xray','yankee',
               'zulu','amber','birch','cedar','denim','ember','frost',
               'glow','haze','ivory','jade','karma','lunar','mint',
               'nova','opal','pearl','quartz','ridge','slate','tidal',
               'umbra','vapor','willow','xenon','yield','zephyr',
               'arbor','blaze','crest','drift','ferry','grove',
               'hollow','inlet','jetty','knoll','lodge',
               'meadow'])[1 + ((g * 31 + k * 17 + seg * 97) % 64)]
        || '_' || ((g * 13 + k * 7 + seg * 53) % 400)::text,
        ' ')
    FROM generate_series(1, 30) k;
$genbody$;
SQL
}

create_owned_table() {
    local table="$1" idx="$2"
    sql_super << SQL
SET client_min_messages = warning;
CREATE TABLE ${table} (id serial PRIMARY KEY, body text);
CREATE INDEX ${idx} ON ${table}
    USING bm25(body) WITH (text_config = 'english');
ALTER TABLE ${table} OWNER TO demo_owner;
ALTER INDEX ${idx} OWNER TO demo_owner;
SQL
}

# Build ${3} L0 segments of ${4} rows each on a fresh table/index,
# with compaction fully disabled so the build itself never merges
# anything -- segment count is exactly what the caller asked for.
build_corpus() {
    local table="$1" idx="$2" segments="$3" rows="$4"
    create_owned_table "$table" "$idx"

    set_guc pg_textsearch.compaction_mode off
    set_guc pg_textsearch.memtable_pages_threshold 0
    set_guc pg_textsearch.bulk_load_threshold 0

    local seg
    for seg in $(seq 1 "$segments"); do
        sql_as demo_owner << SQL >/dev/null
SET client_min_messages = warning;
INSERT INTO ${table} (body)
SELECT public.demo_gen_body(${seg}, g)
FROM generate_series(1, ${rows}) g;
SELECT bm25_spill_index('${idx}');
SQL
    done

    local l0
    l0=$(sql_super -c \
        "SELECT (bm25_level_counts('${idx}'::regclass))[1];")
    if [ "${l0:-0}" -lt "$segments" ]; then
        error "corpus build for ${idx} produced only L0=${l0}, \
expected >= ${segments}"
    fi
    log "Built ${idx}: ${segments} segments x ${rows} rows (L0=${l0})"
}

# Run "INSERT one row, then spill" as a SINGLE statement (a DO
# block), so statement_timeout governs the insert-that-triggers-a-
# merge atomically, exactly as it would for a real application
# transaction that happens to be the unlucky one to cross the
# threshold. Echoes "<rc> <elapsed_seconds>". stderr goes to $5.
run_trigger() {
    local table="$1" idx="$2" marker="$3" timeout_ms="$4" stderr_file="$5"
    local start end rc
    start=$(date +%s.%N)
    set +e
    sql_as demo_owner << SQL 2> "$stderr_file"
SET statement_timeout = '${timeout_ms}ms';
BEGIN;
DO \$body\$
BEGIN
    INSERT INTO ${table} (body) VALUES ('${marker}');
    PERFORM bm25_spill_index('${idx}');
END
\$body\$;
COMMIT;
SQL
    rc=$?
    set -e
    end=$(date +%s.%N)
    echo "${rc} $(awk "BEGIN { printf \"%.3f\", ${end} - ${start} }")"
}

# Same shape as run_trigger, but with NO statement_timeout (this is
# the calibration measurement itself, so it must be allowed to run to
# completion). Echoes elapsed seconds.
calibrate_inline_cascade() {
    local table="$1" idx="$2"
    local start end
    start=$(date +%s.%N)
    sql_as demo_owner << SQL >/dev/null
BEGIN;
DO \$body\$
BEGIN
    INSERT INTO ${table} (body) VALUES ('calibration trigger row');
    PERFORM bm25_spill_index('${idx}');
END
\$body\$;
COMMIT;
SQL
    end=$(date +%s.%N)
    awk "BEGIN { printf \"%.3f\", ${end} - ${start} }"
}

# ---------------------------------------------------------------------
# Calibration
# ---------------------------------------------------------------------

calibrate() {
    act "Calibration: measuring the real inline cascade time on \
this machine"
    build_corpus cal_docs cal_idx "$DEMO_SEGMENTS" "$DEMO_ROWS_PER_SEGMENT"
    set_guc pg_textsearch.compaction_mode inline

    CALIBRATED_SECONDS=$(calibrate_inline_cascade cal_docs cal_idx)
    log "Measured inline cascade time: ${CALIBRATED_SECONDS}s"

    # statement_timeout must be COMFORTABLY under the measured time so
    # Act 1 reliably times out mid-cascade, on a slower or faster
    # machine than the one this was written on -- hence deriving it
    # here rather than hardcoding a constant. Half the measured time,
    # floored at 1s, lands solidly inside the cascade (levels 0..2 or
    # so of a multi-level merge) without being so tight that ordinary
    # scheduling noise could make it fire before any real merge work
    # has started.
    DEMO_TIMEOUT_SECONDS=$(awk \
        "BEGIN { t = ${CALIBRATED_SECONDS} / 2; \
                 if (t < 1.0) t = 1.0; printf \"%.2f\", t }")
    DEMO_TIMEOUT_MS=$(awk \
        "BEGIN { printf \"%.0f\", ${DEMO_TIMEOUT_SECONDS} * 1000 }")
    log "Derived statement_timeout for Acts 1-2: \
${DEMO_TIMEOUT_SECONDS}s (${DEMO_TIMEOUT_MS}ms)"

    sql_super -c "DROP TABLE cal_docs;" >/dev/null
    set_guc pg_textsearch.compaction_mode off
}

# ---------------------------------------------------------------------
# Act 1: inline mode blows through statement_timeout
# ---------------------------------------------------------------------

act1() {
    act "Act 1: a single-row INSERT under compaction_mode = 'inline'"
    build_corpus act1_docs act1_idx "$DEMO_SEGMENTS" "$DEMO_ROWS_PER_SEGMENT"
    set_guc pg_textsearch.compaction_mode inline

    local before
    before=$(sql_super -c \
        "SELECT bm25_level_counts('act1_idx'::regclass)::text;")
    log "Level counts before the triggering insert: ${before}"

    local stderr_file="${DATA_DIR}/act1_stderr.log"
    local result rc elapsed
    result=$(run_trigger act1_docs act1_idx 'act1 trigger row' \
        "$DEMO_TIMEOUT_MS" "$stderr_file")
    rc=${result%% *}
    elapsed=${result#* }
    log "Triggering insert ran for ${elapsed}s and exited with rc=${rc}"

    if [ "$rc" -eq 0 ]; then
        error "ASSERTION FAILED: Act 1's insert was expected to be \
cancelled by statement_timeout (${DEMO_TIMEOUT_SECONDS}s), but it \
succeeded in ${elapsed}s. The calibration margin may be too tight \
on this machine, or inline compaction got faster."
    fi
    if ! grep -q "canceling statement due to statement timeout" \
        "$stderr_file"; then
        error "ASSERTION FAILED: Act 1's insert failed, but not with \
a statement_timeout cancellation. See ${stderr_file}:
$(cat "$stderr_file")"
    fi
    log "PASS: insert was cancelled by statement_timeout \
(\"canceling statement due to statement timeout\")"

    local row_present
    row_present=$(sql_super -c \
        "SELECT EXISTS (SELECT 1 FROM act1_docs \
         WHERE body = 'act1 trigger row');")
    assert_eq "the row is NOT there after the abort" "f" "$row_present"

    # Measured, not assumed: pg_textsearch's segment merges are
    # physical page mutations, WAL-logged directly via GenericXLog
    # (see CLAUDE.md's "Physical replication" note -- there is no
    # custom rmgr and no undo log for these pages, exactly like a
    # B-tree page split surviving a ROLLBACK). ROLLBACK only undoes
    # the *heap tuple's* visibility, not merge work already flushed
    # to the index's pages. So a cancelled statement does NOT get
    # its compaction progress back "for free": every timing we
    # measured on this machine -- including timeouts far tighter
    # than the calibrated one -- showed the FULL cascade completing
    # before the cancellation was even observed, then rolling back
    # only the row. This is a genuine discrepancy from an earlier
    # assumption that CHECK_FOR_INTERRUPTS() would land mid-cascade
    # and leave a partially merged, undone state: in practice, for
    # this corpus size, it does not -- the client pays the FULL
    # merge cost AND still gets an error. See the closing summary.
    local after
    after=$(sql_super -c \
        "SELECT bm25_level_counts('act1_idx'::regclass)::text;")
    log "Level counts after the cancelled insert: ${after}"

    local l0_after
    l0_after=$(sql_super -c \
        "SELECT (bm25_level_counts('act1_idx'::regclass))[1];")
    if [ "$l0_after" -ge "$((DEMO_SEGMENTS + 1))" ]; then
        error "ASSERTION FAILED: expected the merge to have \
progressed (L0 dropped) despite the cancelled statement -- L0 is \
still ${l0_after}. See the comment above run_atomicity_check for \
why this is what we actually measure."
    fi
    log "PASS: L0 dropped from $((DEMO_SEGMENTS + 1)) to \
${l0_after} even though the triggering statement was cancelled --\
 the merge is not undone by the abort."

    log "Inline mode: the client blocked for the FULL merge \
duration and STILL got an error (statement timeout). Unlike an \
ordinary write, cancelling did not even save the merge cost: \
already-applied segment-page mutations are not transactional, so \
the work happened anyway. That is a worse outcome than either \
succeeding or failing fast."
}

# ---------------------------------------------------------------------
# Act 2: the identical insert in background mode
# ---------------------------------------------------------------------

act2() {
    act "Act 2: the identical INSERT under compaction_mode = \
'background'"
    build_corpus act2_docs act2_idx "$DEMO_SEGMENTS" "$DEMO_ROWS_PER_SEGMENT"
    set_guc pg_textsearch.compaction_mode background

    local before
    before=$(sql_super -c \
        "SELECT bm25_level_counts('act2_idx'::regclass)::text;")
    log "Level counts before the triggering insert: ${before}"

    local stderr_file="${DATA_DIR}/act2_stderr.log"
    local result rc elapsed
    result=$(run_trigger act2_docs act2_idx 'act2 trigger row' \
        "$DEMO_TIMEOUT_MS" "$stderr_file")
    rc=${result%% *}
    elapsed=${result#* }
    log "Triggering insert ran for ${elapsed}s and exited with rc=${rc}"

    if [ "$rc" -ne 0 ]; then
        error "ASSERTION FAILED: Act 2's insert was expected to \
commit well inside statement_timeout (${DEMO_TIMEOUT_SECONDS}s), but \
it failed. See ${stderr_file}:
$(cat "$stderr_file")"
    fi
    if ! awk "BEGIN { exit !(${elapsed} < ${DEMO_TIMEOUT_SECONDS}) }"
    then
        error "ASSERTION FAILED: Act 2's insert took ${elapsed}s, \
which is not comfortably under the ${DEMO_TIMEOUT_SECONDS}s timeout"
    fi
    log "PASS: insert committed in ${elapsed}s, well inside the \
${DEMO_TIMEOUT_SECONDS}s timeout that killed Act 1's identical insert"

    local row_present
    row_present=$(sql_super -c \
        "SELECT EXISTS (SELECT 1 FROM act2_docs \
         WHERE body = 'act2 trigger row');")
    assert_eq "the row IS there" "t" "$row_present"

    local label id
    label=$(label_for act2_idx)
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}';")
    if [ -z "$id" ]; then
        error "ASSERTION FAILED: no durable compaction instance was \
enqueued for act2_idx"
    fi
    log "PASS: durable compaction instance ${id} exists \
(df.instances, label=${label})"

    local st
    st=$(wait_for_instance "$id" 60)
    assert_eq "the background compaction instance completes" \
        "completed" "$st"

    local after
    after=$(sql_super -c \
        "SELECT bm25_level_counts('act2_idx'::regclass)::text;")
    log "Level counts after the cascade completed: ${after}"

    # Never assert only "completed": that alone passes vacuously if
    # the worker session never saw the lowered segments_per_level
    # threshold (the GUC-scope trap). Require L0 to have actually
    # dropped, proving real merge work happened, not a silent no-op.
    local l0_final
    l0_final=$(sql_super -c \
        "SELECT (bm25_level_counts('act2_idx'::regclass))[1];")
    if [ "$l0_final" -ge "$((DEMO_SEGMENTS + 1))" ]; then
        error "ASSERTION FAILED: L0 segment count did not drop after \
the background cascade completed (still ${l0_final})"
    fi
    log "PASS: L0 segment count dropped from $((DEMO_SEGMENTS + 1)) \
to ${l0_final} -- the merge is real, not skipped"

    log "Background mode: the write committed fast, and the merge \
still happened -- just off the triggering transaction."
}

# ---------------------------------------------------------------------
# Act 3: transactional correctness under concurrency
# ---------------------------------------------------------------------

ACT3_QUERY_TERM='alpha_100'

# Emits the top-20 ctids (order preserved) for ACT3_QUERY_TERM. The
# comparison used for ranking invariance is on ctid ORDER only, not
# on the raw score value: act3's writer_loop keeps inserting new
# rows into this same table throughout the check (deliberately --
# that's what makes the concurrent-writer-latency measurement real),
# so total_docs/avg document length keep drifting. Every demo
# document is exactly 30 words, so BM25's length-normalization term
# is identical across all of them; a drifting corpus-wide avgdl (or
# doc count) rescales every document's score by the same constant
# factor and does not reorder them. So ctid order is the right
# invariant to check; the raw score number is expected to drift for
# reasons unrelated to the merge, and comparing it verbatim would be
# comparing against a moving target, not a correctness signal.
top20_query() {
    local idx="$1" table="$2"
    echo "SELECT ctid FROM ${table} \
ORDER BY body <@> to_bm25query('${ACT3_QUERY_TERM}', '${idx}'), ctid \
LIMIT 20;"
}

# Rollback must enqueue nothing; commit must enqueue exactly one
# instance. Runs against its own small table so it cannot be confused
# with the main act3_docs cascade, which shares no label with it.
run_atomicity_check() {
    log "Atomicity check (own table, concurrently with the main \
cascade)..."
    build_corpus atomic_docs atomic_idx 2 50
    set_guc pg_textsearch.compaction_mode background

    local label before_rows before_instances
    label=$(label_for atomic_idx)
    before_rows=$(sql_super -c "SELECT count(*) FROM atomic_docs;")
    before_instances=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")

    sql_as demo_owner << 'SQL' >/dev/null
SET client_min_messages = warning;
BEGIN;
INSERT INTO atomic_docs (body) VALUES ('atomicity rollback probe');
SELECT bm25_spill_index('atomic_idx');
ROLLBACK;
SQL

    local after_rollback_rows after_rollback_instances
    after_rollback_rows=$(sql_super -c "SELECT count(*) FROM atomic_docs;")
    after_rollback_instances=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "atomicity: ROLLBACK leaves row count unchanged" \
        "$before_rows" "$after_rollback_rows"
    assert_eq "atomicity: ROLLBACK enqueues zero compaction instances" \
        "$before_instances" "$after_rollback_instances"

    sql_as demo_owner << 'SQL' >/dev/null
SET client_min_messages = warning;
BEGIN;
INSERT INTO atomic_docs (body) VALUES ('atomicity commit probe');
SELECT bm25_spill_index('atomic_idx');
COMMIT;
SQL

    local after_commit_rows after_commit_instances
    after_commit_rows=$(sql_super -c "SELECT count(*) FROM atomic_docs;")
    after_commit_instances=$(sql_super -c \
        "SELECT count(*) FROM df.instances WHERE label = '${label}';")
    assert_eq "atomicity: COMMIT makes the row visible" \
        "$((before_rows + 1))" "$after_commit_rows"
    assert_eq "atomicity: COMMIT enqueues exactly one compaction \
instance" "$((before_instances + 1))" "$after_commit_instances"

    local id
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}' \
         ORDER BY created_at DESC LIMIT 1;")
    [ -n "$id" ] && wait_for_instance "$id" 30 >/dev/null
    log "Atomicity check done."
}

# Background job: insert one row roughly every 100ms into $1 until
# $2 (a stop-file path) appears. Records per-insert latency to $3
# and a running failure count to $4.
writer_loop() {
    local table="$1" stopfile="$2" latfile="$3" failcount_file="$4"
    local stderr_log="$5"
    : > "$latfile"
    echo 0 > "$failcount_file"
    local fails=0
    while [ ! -f "$stopfile" ]; do
        local start end lat
        start=$(date +%s.%N)
        if sql_as demo_owner -c \
            "INSERT INTO ${table} (body) \
             VALUES ('writer row ' || clock_timestamp()::text);" \
            >/dev/null 2>>"$stderr_log"; then
            end=$(date +%s.%N)
            lat=$(awk "BEGIN { printf \"%.3f\", ${end} - ${start} }")
            echo "$lat" >> "$latfile"
        else
            fails=$((fails + 1))
            echo "$fails" > "$failcount_file"
        fi
        sleep 0.1
    done
}

# Background job: one continuous REPEATABLE READ transaction that
# samples the same top-k query three times, well spaced out, and
# writes each sample to its own file.
torn_reads_session() {
    local idx="$1" table="$2" out1="$3" out2="$4" out3="$5"
    local sleep1="$6" sleep2="$7"
    local q
    q="$(top20_query "$idx" "$table")"
    sql_as demo_owner << SQL
BEGIN ISOLATION LEVEL REPEATABLE READ;
\o ${out1}
${q}
\o
SELECT pg_sleep(${sleep1});
\o ${out2}
${q}
\o
SELECT pg_sleep(${sleep2});
\o ${out3}
${q}
\o
COMMIT;
SQL
}

act3() {
    act "Act 3: transactional correctness under concurrency"
    build_corpus act3_docs act3_idx "$DEMO_SEGMENTS" "$DEMO_ROWS_PER_SEGMENT"
    set_guc pg_textsearch.compaction_mode background

    local baseline_file="${DATA_DIR}/act3_rank_baseline.txt"
    sql_as demo_owner -c "$(top20_query act3_idx act3_docs)" \
        > "$baseline_file"
    if [ ! -s "$baseline_file" ]; then
        error "ASSERTION FAILED: baseline ranking query for term \
'${ACT3_QUERY_TERM}' returned no rows -- the invariance check would \
prove nothing against an empty result set"
    fi
    log "Captured baseline top-20 for '${ACT3_QUERY_TERM}' \
($(wc -l < "$baseline_file") rows)"

    local result rc elapsed
    result=$(run_trigger act3_docs act3_idx 'act3 trigger row' \
        60000 "${DATA_DIR}/act3_trigger_stderr.log")
    rc=${result%% *}
    elapsed=${result#* }
    if [ "$rc" -ne 0 ]; then
        error "ASSERTION FAILED: Act 3's triggering insert failed \
unexpectedly. See ${DATA_DIR}/act3_trigger_stderr.log:
$(cat "${DATA_DIR}/act3_trigger_stderr.log")"
    fi
    log "Triggering insert committed in ${elapsed}s"

    local label id
    label=$(label_for act3_idx)
    id=$(sql_super -c \
        "SELECT id FROM df.instances WHERE label = '${label}';")
    if [ -z "$id" ]; then
        error "ASSERTION FAILED: no durable compaction instance was \
enqueued for act3_idx"
    fi

    local st
    st=$(wait_for_running "$id" 30)
    if [ "$st" != "running" ]; then
        error "ASSERTION FAILED: cascade instance ${id} never \
reached 'running' within 15s (last status: '${st}') -- nothing to \
race against"
    fi
    log "Cascade instance ${id} is running -- launching concurrent \
readers and writers against it now"

    # --- launch background concurrent load -----------------------
    local stopfile="${DATA_DIR}/act3_stop"
    rm -f "$stopfile"
    local latfile="${DATA_DIR}/act3_writer_latencies.txt"
    local failcount_file="${DATA_DIR}/act3_writer_fails.txt"
    local writer_stderr="${DATA_DIR}/act3_writer_stderr.log"
    writer_loop act3_docs "$stopfile" "$latfile" "$failcount_file" \
        "$writer_stderr" &
    local writer_pid=$!

    local torn1="${DATA_DIR}/act3_torn_1.txt"
    local torn2="${DATA_DIR}/act3_torn_2.txt"
    local torn3="${DATA_DIR}/act3_torn_3.txt"
    local sleep1 sleep2
    sleep1=$(awk \
        "BEGIN { s = ${CALIBRATED_SECONDS} * 0.4; \
                 if (s < 1) s = 1; printf \"%.0f\", s }")
    sleep2=$(awk \
        "BEGIN { s = ${CALIBRATED_SECONDS} * 1.2; \
                 if (s < 3) s = 3; printf \"%.0f\", s }")
    torn_reads_session act3_idx act3_docs "$torn1" "$torn2" "$torn3" \
        "$sleep1" "$sleep2" >/dev/null &
    local torn_pid=$!

    # Atomicity runs concurrently too -- a fully independent index,
    # so it cannot interfere with the cascade above.
    run_atomicity_check

    # --- ranking-invariance loop (foreground) ---------------------
    local max_iters=150
    local mismatches=0
    local confirmed_running=0
    local i sample_file last_status="$st"
    for i in $(seq 1 "$max_iters"); do
        last_status=$(sql_super -c \
            "SELECT status FROM df.instances WHERE id = '${id}';")
        sample_file="${DATA_DIR}/act3_rank_sample_${i}.txt"
        sql_as demo_owner -c "$(top20_query act3_idx act3_docs)" \
            > "$sample_file"
        if ! diff -q "$baseline_file" "$sample_file" >/dev/null; then
            mismatches=$((mismatches + 1))
            warn "ranking mismatch at sample ${i} (status=\
${last_status}); see ${sample_file}"
        fi
        if [ "$last_status" = "running" ]; then
            confirmed_running=$((confirmed_running + 1))
        fi
        if [ "$last_status" = "completed" ] \
            || [ "$last_status" = "failed" ]; then
            break
        fi
        sleep 0.4
    done
    local ranking_samples=$i
    log "Ranking-invariance loop took ${ranking_samples} samples \
(${confirmed_running} confirmed while status='running'); final \
status observed: ${last_status}"

    # Stop the writer loop and wait for both background jobs.
    touch "$stopfile"
    wait "$writer_pid" || true
    wait "$torn_pid" || true

    local final_status
    final_status=$(wait_for_instance "$id" 60)
    assert_eq "the cascade instance completes" "completed" "$final_status"

    local final_counts
    final_counts=$(sql_super -c \
        "SELECT bm25_level_counts('act3_idx'::regclass)::text;")
    log "Final level counts after the cascade: ${final_counts}"

    # --- assertions ------------------------------------------------

    if [ "$mismatches" -ne 0 ]; then
        error "ASSERTION FAILED: ranking invariance -- ${mismatches} \
of ${ranking_samples} samples differed from the pre-cascade baseline"
    fi
    log "PASS: ranking invariance -- all ${ranking_samples} top-20 \
samples for '${ACT3_QUERY_TERM}' matched the pre-cascade baseline \
exactly"

    if [ "$confirmed_running" -lt 1 ]; then
        error "ASSERTION FAILED: ranking invariance proves nothing -- \
no sample was confirmed to run while the cascade instance was still \
'running' (it may have already finished before sampling began)"
    fi
    log "PASS: ${confirmed_running} of those samples were confirmed \
to run WHILE the cascade was still 'running' -- the check genuinely \
ran during the merge, not only after it"

    if ! diff -q "$torn1" "$torn2" >/dev/null \
        || ! diff -q "$torn2" "$torn3" >/dev/null; then
        error "ASSERTION FAILED: no-torn-reads -- the three samples \
taken inside one REPEATABLE READ transaction (before/during/after \
the cascade) did not all agree. See ${torn1}, ${torn2}, ${torn3}."
    fi
    log "PASS: no torn reads -- one REPEATABLE READ transaction saw \
identical results before, during, and after the cascade"

    local writer_success writer_fails max_latency
    writer_success=$(wc -l < "$latfile")
    writer_fails=$(cat "$failcount_file")
    if [ "$writer_fails" -ne 0 ]; then
        error "ASSERTION FAILED: ${writer_fails} of $((writer_success \
+ writer_fails)) concurrent writer inserts FAILED during the cascade \
-- a concurrent writer must always eventually commit. See \
${writer_stderr}."
    fi
    if [ "$writer_success" -lt 1 ]; then
        error "ASSERTION FAILED: the concurrent writer loop recorded \
zero successful inserts -- it never ran long enough to prove \
anything"
    fi
    max_latency=$(sort -n "$latfile" | tail -1)
    log "PASS: all ${writer_success} concurrent writer inserts \
eventually committed during the cascade (zero failures)"
    log "MEASURED max concurrent-writer insert latency during the \
cascade: ${max_latency}s (this is the honest cost of the per-index \
LW_EXCLUSIVE lock still held for one merge batch -- see the closing \
summary)"

    local expected_count actual_count
    expected_count=$((DEMO_SEGMENTS * DEMO_ROWS_PER_SEGMENT \
        + 1 + writer_success))
    actual_count=$(sql_super -c "SELECT count(*) FROM act3_docs;")
    assert_eq "crash-consistency: row count matches every committed \
insert (${DEMO_SEGMENTS}x${DEMO_ROWS_PER_SEGMENT} corpus + 1 trigger \
row + ${writer_success} writer rows)" \
        "$expected_count" "$actual_count"

    ACT3_MAX_WRITER_LATENCY="$max_latency"
}

# ---------------------------------------------------------------------
# Closing summary -- honest framing, non-negotiable
# ---------------------------------------------------------------------

closing_summary() {
    act "Summary"
    cat << SUMMARY
Background compaction (pg_durable) moves the merge OFF the triggering
transaction. That is what rescued Act 2's insert: the identical
single-row insert that blew through a ${DEMO_TIMEOUT_SECONDS}s
statement_timeout under inline mode committed in well under that
time under background mode, and the merge still happened -- level
counts really changed, out of band.

It does NOT make merges lock-free. bm25_compact_step() still takes
the per-index LW_EXCLUSIVE lock for the duration of one merge batch,
and concurrent readers and writers serialize behind it -- see the
comment at src/access/build.c:606, which confirms the scan and
append paths take LW_SHARED on that same lock. The stepped cascade
(one df.loop iteration per merge batch) shrinks the exclusive window
from "the whole multi-level cascade" to "one batch", which is why
Act 3's concurrent writer kept making progress instead of stalling
for the cascade's entire duration -- but it did NOT avoid blocking
during that one batch.

MEASURED max concurrent-writer insert latency during Act 3's cascade:
${ACT3_MAX_WRITER_LATENCY}s.
SUMMARY

    if awk "BEGIN { exit !(${ACT3_MAX_WRITER_LATENCY} > 0.5) }"; then
        cat << STALL
This is a real, measured stall, not a rounding artifact: a
concurrent writer was blocked for up to ${ACT3_MAX_WRITER_LATENCY}s
by a single merge batch's LW_EXCLUSIVE hold. This POC does not hide
that. It is exactly the motivation for the non-blocking-merge design
in docs/background_compaction.md ("Future work: non-blocking
merges"): segments are immutable, so a merge could build its output
out-of-band and take the exclusive lock only for the metapage
pointer swap, shrinking the window from "duration of one batch" to
"duration of one buffer update".
STALL
    fi

    cat << SUMMARY2

See docs/background_compaction.md ("Future work: non-blocking
merges") for the proposed fix, which is intentionally out of scope
for this POC.
SUMMARY2
}

# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------

setup_cluster
setup_roles_and_glue
wait_for_durable_worker

calibrate
act1
act2
act3
closing_summary

log "Demo complete. All assertions passed."
exit 0
