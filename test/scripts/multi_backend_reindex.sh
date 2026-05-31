#!/bin/bash
#
# multi_backend_reindex.sh — regression test for issue #390:
#   "bug: BM25 index returns stale heap TIDs after heap rewrite"
#
# Reproduces the multi-backend scenario in which one backend has already
# touched the BM25 index (priming its private TpLocalIndexState cache)
# when a second backend runs a statement that gives the index a new
# relfilenode and rebuilds it from scratch. Several distinct DDL paths
# trigger this:
#
#   * ALTER TABLE ... ADD COLUMN ... GENERATED ALWAYS AS (...) STORED
#   * VACUUM FULL <table>
#   * CLUSTER <table> USING <pkey>
#   * REINDEX INDEX <bm25_index>   (index relfilenode only; heap intact)
#
# On v1.2.0 (in-memory memtable in DSA) the first backend's
# local_state_cache (`src/index/state.c`, `tp_get_local_index_state`)
# was never invalidated because pg_textsearch registers no
# `CacheRegisterRelcacheCallback`. Subsequent writes from that backend
# went to the OLD memtable still living in DSA, and a later spill
# wrote a segment of OLD (now stale) heap CTIDs into the NEW index
# file. Queries returning those stale CTIDs then failed with:
#
#   ERROR: could not read blocks N..N in file "base/<db>/<heap_fno>":
#          read only 0 of 8192 bytes
#
# As of pg_textsearch 1.3.0 (#374/#375/#392) the memtable lives in
# the index relation's own on-disk pages, so the new relfilenode is
# automatically empty and stale local-state pointers become
# harmless. This test pins that behaviour so the bug cannot be
# reintroduced — for every member of the heap-rewrite / index-rebuild
# class.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Allow override but default to a port not used by any other test script.
# (cross-checked against test/scripts/*.sh: 55434–55450, 55454–55457,
# 55460–55462 are taken; 55458 is free as of this writing.)
TEST_PORT="${TEST_PORT:-55458}"
TEST_DB=pg_textsearch_reindex_test
# Unique per-PID data dir so concurrent invocations (e.g. accidental
# parallel make) cannot rm -rf each other's clusters.
DATA_DIR="${SCRIPT_DIR}/../tmp_reindex_test_${TEST_PORT}_$$"
LOGFILE="${DATA_DIR}/postgres.log"
# Preserved server-log destination if the test fails — kept outside
# DATA_DIR so it survives the cleanup `rm -rf`. CI can `cat` this on
# failure for post-mortem.
PRESERVED_LOGFILE="${SCRIPT_DIR}/../tmp_reindex_test_${TEST_PORT}_$$_postgres.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }
info() { echo -e "${BLUE}[$(date '+%H:%M:%S')] $1${NC}"; }

cleanup() {
    local exit_code=$?
    # Disarm the trap so a signal during cleanup doesn't re-enter us.
    trap - EXIT INT TERM
    log "Cleaning up reindex test environment (exit code: $exit_code)..."
    # On failure, preserve postgres.log outside DATA_DIR so CI can
    # surface it for debugging — DATA_DIR is about to be wiped.
    if [ "${exit_code}" -ne 0 ] && [ -f "${LOGFILE}" ]; then
        cp "${LOGFILE}" "${PRESERVED_LOGFILE}" 2>/dev/null || true
        if [ -f "${PRESERVED_LOGFILE}" ]; then
            warn "Preserved server log: ${PRESERVED_LOGFILE}"
        fi
    fi
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m fast -w &>/dev/null ||
            pg_ctl stop -D "${DATA_DIR}" -m immediate -w &>/dev/null ||
            true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up reindex test PostgreSQL instance..."

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1

    # Pin spill thresholds so the only memtable spill in this test is
    # the one we force via bm25_spill_index() — guards against future
    # default changes that might cause an implicit spill before the
    # rewrite, hiding the bug being exercised.
    # Both GUCs accept 0 as "disable": tp_bulk_load_threshold short-
    # circuits at src/index/state.c when <= 0, and the page-threshold
    # auto-spill returns early when == 0 at src/access/build.c.
    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 20
shared_buffers = 128MB
unix_socket_directories = '${DATA_DIR}'
listen_addresses = ''
log_min_messages = warning
shared_preload_libraries = 'pg_textsearch'
pg_textsearch.bulk_load_threshold = 0
pg_textsearch.memtable_pages_threshold = 0
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w \
        || error "Failed to start PostgreSQL"

    createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

run_sql() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

run_sql_quiet() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -c "$1" >/dev/null 2>&1
}

# run_sql_value: scalar fetch. Captures stderr (2>&1) so that any
# server-side ERROR surfaces in the returned string and downstream
# regex checks can detect storage corruption. Numeric callers must
# validate the format (e.g. `[[ $val =~ ^[0-9]+$ ]]`) because the
# returned value may be an error message rather than a number.
run_sql_value() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc "$1" 2>&1
}

# prepare_docs_table — fresh, intentionally-bloated docs table.
#
# 30k rows + fillfactor=10 → ~4000-6000 heap pages. DELETE of the
# tail leaves ~1000 live rows but the heap retains its page count
# until rewrite (autovacuum is disabled at table level). This makes
# the OLD heap span many blocks while the NEW heap (post-rewrite) is
# small — so any stale CTID retained by a backend with a primed
# cache will point well beyond the new heap's EOF, surfacing the
# "could not read blocks" bug deterministically on v1.2.0.
prepare_docs_table() {
    run_sql_quiet "
        DROP TABLE IF EXISTS docs CASCADE;
        CREATE TABLE docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        ) WITH (autovacuum_enabled=false, fillfactor=10);

        INSERT INTO docs (content)
        SELECT 'row ' || g || ' lorem ipsum dolor sit amet '
               || md5(g::text)
        FROM generate_series(1, 30000) g;

        CREATE INDEX docs_bm25 ON docs USING bm25 (content)
            WITH (text_config='english');
    "
    run_sql_quiet "DELETE FROM docs WHERE id > 1000;"
}

# run_rewrite_test test_name rewrite_sql expect_heap_shrink
#
# Drive both backends from one psql session: Backend A primes its
# TpLocalIndexState cache via INSERT, samples pre-rewrite heap size,
# then spawns Backend B (psql via `\!`) to perform the supplied
# rewrite, then continues using its (potentially stale) cached state
# to INSERT 50 'moonlight' rows and force a spill.
#
# After the heredoc completes, all SETUP INVARIANTS are checked
# BEFORE any bug-class assertion (so a degraded test environment
# can't masquerade as a bug-fix regression):
#   - no storage-corruption errors in session output
#   - all session markers were emitted
#   - bm25_spill_index returned a real segment root
#   - pre_heap_pages >= 1000 (bloat sufficient)
#   - heap actually shrank (skipped when expect_heap_shrink=0)
#
# Then the BUG-CLASS ASSERTIONS run:
#   - docs_persisted == live heap row count
#   - top-100 'lorem' index scan returns exactly 100 (no errors)
#   - exactly 50 live 'moonlight' rows are returned through the index
#
# Args:
#   $1: human-readable test name
#   $2: SQL Backend B should run (the heap/index rewrite)
#   $3: 1 if the rewrite is expected to compact the heap, 0 otherwise
#       (REINDEX INDEX only touches the index relfilenode)
run_rewrite_test() {
    local test_name="$1"
    local rewrite_sql="$2"
    local expect_heap_shrink="$3"

    log "Test: ${test_name} (issue #390)"

    prepare_docs_table

    # Bash heredoc with unquoted EOF so ${DATA_DIR}, ${TEST_PORT},
    # ${TEST_DB}, ${rewrite_sql} all expand at script-render time.
    # ON_ERROR_STOP=0 so every server-side error reaches $output,
    # not just the first.
    #
    # NOTE on shell quoting: ${rewrite_sql} is interpolated INSIDE
    # double quotes after `\! psql ... -c`. Any SQL string passed
    # here must therefore contain no unescaped double quotes. All
    # four current callers use only single-quoted SQL literals — if
    # a future case needs double quotes, switch the inner -c to a
    # here-string fed to psql instead.
    local output
    output=$(psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=0 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
-- Backend A: prime local_state_cache by inserting into the index.
-- The 'unicorn' term is unique to these rows so we can isolate them
-- later. 500 rows extend the heap further, so any CTID captured here
-- will reference a high block number.
INSERT INTO docs (content)
SELECT 'pre ' || g || ' unicorn rainbow sparkle ' || md5(('h'||g)::text)
FROM generate_series(1, 500) g;

-- Sample pre-rewrite heap pages HERE, immediately before the
-- rewrite, so the post-rewrite "heap shrank" invariant has zero
-- slack (matches the exact moment Backend B is about to act on).
SELECT 'pre_heap_pages=' || (pg_relation_size('docs')::bigint / 8192);
SELECT 'A-primed' AS marker;

-- Backend B: heap/index rewrite. Runs in a *separate* psql
-- process via \! while Backend A's session stays open.
\! psql -h "${DATA_DIR}" -p ${TEST_PORT} -d "${TEST_DB}" -v ON_ERROR_STOP=1 -q -c "${rewrite_sql}" 2>&1

-- Backend A is still the same session; on v1.2.0 its
-- TpLocalIndexState cache still points at the pre-rewrite
-- TpSharedIndexState because no relcache invalidation fires.
SELECT 'A-post-rewrite' AS marker;

-- Backend A: write via the (potentially stale) cache.
INSERT INTO docs (content)
SELECT 'late ' || g || ' lorem moonlight ' || md5(('l'||g)::text)
FROM generate_series(1, 50) g;

-- Force a spill. On the v1.2.0 bug this is what materialises the
-- corruption: the OLD in-memory memtable (with OLD CTIDs) gets
-- written into the NEW index file as a stale L0 segment.
-- bm25_spill_index returns the new L0 segment's root block number
-- (always > 0 because block 0 is the metapage) or NULL if nothing
-- was spilled.
SELECT 'spill_segment_root=' || bm25_spill_index('docs_bm25');
SELECT 'A-spilled' AS marker;
EOF
)

    info "Backend A session output: ${output}"

    # ----- SETUP INVARIANT CHECKS (must pass before bug assertions) -----

    # 1. Corruption regex — fail fast on the production error symptom.
    if echo "${output}" | grep -qE "could not read blocks|invalid \
(page|segment)|XX001"; then
        error "BUG (issue #390): Backend A session produced a \
storage-corruption error (likely stale CTID). Output: ${output}"
    fi

    # 2. Session must have reached every marker; otherwise an
    #    unexpected non-corruption error or a hanging psql would
    #    silently weaken later assertions.
    local marker
    for marker in "A-primed" "A-post-rewrite" "A-spilled"; do
        if ! echo "${output}" | grep -qF "${marker}"; then
            error "Backend A session never reached marker \
'${marker}'. Full output:\n${output}"
        fi
    done

    # 3. Spill must have produced a real segment. bm25_spill_index
    #    returns a BlockNumber (always > 0 for a non-empty spill) or
    #    NULL when no segment was written.
    local spill_segment_root
    spill_segment_root=$(echo "${output}" \
        | sed -n 's/^.*spill_segment_root=\([0-9]*\).*$/\1/p' | head -1)
    if [ -z "${spill_segment_root}" ] \
        || [ "${spill_segment_root}" -le 0 ]; then
        error "bm25_spill_index returned no segment root \
(got '${spill_segment_root:-empty}'); the late inserts should have \
populated the memtable before the explicit spill."
    fi
    info "Spill segment root block: ${spill_segment_root}"

    # 4. Pre-rewrite heap page count (parsed from session output).
    local pre_heap_pages
    pre_heap_pages=$(echo "${output}" \
        | sed -n 's/^.*pre_heap_pages=\([0-9]*\).*$/\1/p' | head -1)
    if ! [[ "${pre_heap_pages}" =~ ^[0-9]+$ ]]; then
        error "Failed to obtain pre-rewrite heap page count from \
session output (got '${pre_heap_pages}'). Test setup broken."
    fi
    info "Pre-rewrite heap pages: ${pre_heap_pages}"

    # 5. Bloat threshold. Promoted from warn to error: the entire
    #    test thesis depends on the OLD heap dwarfing the NEW one.
    #    Without enough bloat, stale CTIDs may still land on valid
    #    blocks and the bug becomes silently invisible.
    if [ "${pre_heap_pages}" -lt 1000 ]; then
        error "Pre-rewrite heap is too small (${pre_heap_pages} \
pages, need >= 1000). Test setup is degenerate — bloat is insufficient \
for stale-CTID detection."
    fi

    # 6. Heap-rewrite invariant: confirms the rewrite actually
    #    happened. Skipped for REINDEX INDEX (touches only the index
    #    relfilenode, not the heap).
    if [ "${expect_heap_shrink}" = "1" ]; then
        local heap_size_now
        heap_size_now=$(run_sql_value \
            "SELECT pg_relation_size('docs')/8192;")
        if ! [[ "${heap_size_now}" =~ ^[0-9]+$ ]]; then
            error "Failed to obtain post-rewrite heap size \
(got '${heap_size_now}')."
        fi
        info "Post-rewrite heap pages: ${heap_size_now}"
        if [ "${heap_size_now}" -ge "${pre_heap_pages}" ]; then
            error "Heap was not compacted by '${test_name}' \
(pre=${pre_heap_pages}, post=${heap_size_now}). Test setup invalid — \
either bloat was insufficient or the rewrite did not happen."
        fi
    fi

    # ----- BUG-CLASS ASSERTIONS (only after setup is verified) -----

    # A1. docs_persisted must equal live heap row count. The bug
    #     inflates the segment doc count above the actual row count
    #     because the stale memtable was spilled as a second L0
    #     segment whose entries reference long-dead CTIDs.
    local heap_rows seg_docs
    heap_rows=$(run_sql_value "SELECT count(*) FROM docs;")
    seg_docs=$(run_sql_value "
        SELECT (regexp_match(
            bm25_summarize_index('docs_bm25'),
            'docs_persisted: ([0-9]+)'
        ))[1]::bigint;
    ")
    info "Heap live rows: ${heap_rows}, docs_persisted: ${seg_docs}"
    if [ -z "${seg_docs}" ]; then
        error "Failed to parse docs_persisted from \
bm25_summarize_index output:\n$(run_sql \
"SELECT bm25_summarize_index('docs_bm25');")"
    fi
    if [ "${seg_docs}" != "${heap_rows}" ]; then
        error "BUG (issue #390): docs_persisted (${seg_docs}) does \
not match live heap row count (${heap_rows}). Excess docs indicate \
a stale segment was written from a pre-rewrite memtable.\n\nFull \
summary:\n$(run_sql "SELECT bm25_summarize_index('docs_bm25');")"
    fi

    # A2. Top-100 'lorem' scan. The bug manifests as "could not read
    #     blocks" when the stale segment's top-k results point past
    #     the new heap's EOF. Wrap in `set +e/-e` so psql's non-zero
    #     exit on the corruption error does NOT abort before we
    #     inspect the captured stderr.
    local scan_q
    set +e
    scan_q=$(run_sql_value "
        SELECT count(*)::text FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('lorem', 'docs_bm25')
            LIMIT 100
        ) s;
    ")
    set -e
    if echo "${scan_q}" | grep -qiE "could not read blocks|invalid \
(page|segment)|error"; then
        error "BUG (issue #390): top-100 'lorem' index scan failed: \
${scan_q}"
    fi
    if [ "${scan_q}" != "100" ]; then
        error "BUG (issue #390): top-100 index scan for 'lorem' \
returned '${scan_q}' rows, expected 100."
    fi

    # A3. Post-rewrite late-insert visibility: 'moonlight' appears
    #     only in the 50 late rows Backend A inserted via the
    #     (potentially stale) cache. md5-hex output uses only [0-9a-f]
    #     so 'moonlight' (m,n,i,g,h,t) cannot accidentally appear in
    #     any pre-rewrite row's md5 suffix — the inner-join LIKE
    #     filter is therefore provably tight. Single query handles
    #     both the error-detection and count-verification roles
    #     (no double-scan).
    local moonlight_count
    set +e
    moonlight_count=$(run_sql_value "
        SELECT count(*)::text FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('moonlight', 'docs_bm25')
            LIMIT 100
        ) s
        JOIN docs d ON d.id = s.id
        WHERE d.content LIKE '%moonlight%';
    ")
    set -e
    if echo "${moonlight_count}" | grep -qiE "could not read blocks\
|invalid (page|segment)|error"; then
        error "BUG (issue #390): post-rewrite 'moonlight' query \
failed: ${moonlight_count}"
    fi
    if [ "${moonlight_count}" != "50" ]; then
        error "BUG (issue #390): expected 50 live 'moonlight' rows \
via BM25 index, got '${moonlight_count}'. The post-rewrite late \
inserts were not correctly indexed in the new file."
    fi

    run_sql_quiet "DROP TABLE docs CASCADE;"
    log "✅ ${test_name}: no stale CTIDs after rewrite"
}

main() {
    log "Starting pg_textsearch multi-backend reindex test (#390)..."

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"

    setup_test_db

    # All four heap/index rewrite paths covered by the issue #390
    # fix (#374/#375/#392) as a class. The first three rewrite the
    # heap (and reindex); REINDEX INDEX touches only the index
    # relfilenode. All four exercise the same TpLocalIndexState
    # invalidation gap.
    run_rewrite_test \
        "ALTER TABLE ADD COLUMN ... GENERATED STORED" \
        "ALTER TABLE docs ADD COLUMN content_tsv tsvector GENERATED \
ALWAYS AS (to_tsvector('english', content)) STORED;" \
        1

    run_rewrite_test \
        "VACUUM FULL docs" \
        "VACUUM FULL docs;" \
        1

    run_rewrite_test \
        "CLUSTER docs USING docs_pkey" \
        "CLUSTER docs USING docs_pkey;" \
        1

    run_rewrite_test \
        "REINDEX INDEX docs_bm25" \
        "REINDEX INDEX docs_bm25;" \
        0

    log "🎉 multi_backend_reindex.sh: all tests passed"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
