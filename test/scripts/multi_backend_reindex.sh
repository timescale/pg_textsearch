#!/bin/bash
#
# multi_backend_reindex.sh — regression test for issue #390:
#   "bug: BM25 index returns stale heap TIDs after heap rewrite"
#
# Reproduces the multi-backend scenario in which one backend has already
# touched the BM25 index (priming its private TpLocalIndexState cache)
# when a second backend runs a statement that rewrites the heap (e.g.
# `ALTER TABLE ... ADD COLUMN ... GENERATED ALWAYS AS (...) STORED`),
# which gives the index a new relfilenode and rebuilds it from scratch.
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
# reintroduced.
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
    # ALTER, hiding the bug being exercised.
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

run_sql_value() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc "$1" \
        2>/dev/null
}

# -------------------------------------------------------------------
# Test: ALTER TABLE ADD COLUMN ... GENERATED STORED with a primed
#       local-state cache in a second backend.
#
# Setup creates a bloated heap (large initial load + DELETE of most
# rows, with autovacuum disabled). This makes the OLD heap span many
# blocks while the NEW heap (post-ALTER) is small, so any stale CTID
# retained by a backend with a primed cache will point well beyond
# the new heap's EOF — surfacing the "could not read blocks" bug
# reliably on v1.2.0.
#
# We drive both backends from a single psql session (Backend A) that
# spawns Backend B via `\!`. This keeps Backend A's connection alive
# across the heap rewrite, exactly matching the in-production race.
# -------------------------------------------------------------------
test_alter_table_add_column_stored() {
    log "Test: ALTER TABLE ADD COLUMN ... GENERATED STORED (issue #390)"

    # Build a bloated heap: 30k rows, fillfactor=10 → many pages.
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

    # Delete most rows so the post-ALTER heap will be tiny.
    run_sql_quiet "DELETE FROM docs WHERE id > 1000;"

    local pre_heap_pages
    pre_heap_pages=$(run_sql_value \
        "SELECT pg_relation_size('docs') / 8192;")
    if ! [[ "${pre_heap_pages}" =~ ^[0-9]+$ ]]; then
        error "Failed to obtain pre-ALTER heap page count (got \
'${pre_heap_pages}'). Test setup is broken."
    fi
    info "Pre-ALTER heap pages: ${pre_heap_pages}"

    if [ "${pre_heap_pages}" -lt 1000 ]; then
        warn "Pre-ALTER heap is smaller than expected (${pre_heap_pages} \
pages). Test may still pass but stale-CTID detection is weakest when \
old heap is much larger than new heap."
    fi

    # Drive both backends from one psql session. Backend A primes its
    # cache via INSERT, then spawns Backend B (psql via \!) to do the
    # ALTER, then continues using its (potentially stale) cached state.
    # We use a bash heredoc (unquoted EOF) so ${DATA_DIR}, ${TEST_PORT},
    # and ${TEST_DB} expand at script-render time. The result is fed to
    # psql with ON_ERROR_STOP off so we collect every error in $output
    # for inspection.
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
SELECT 'A-primed' AS marker;

-- Backend B: heap rewrite + reindex. Runs in a *separate* psql
-- process via \! while Backend A's session stays open.
\! psql -h "${DATA_DIR}" -p ${TEST_PORT} -d "${TEST_DB}" -v ON_ERROR_STOP=1 -q -c "ALTER TABLE docs ADD COLUMN content_tsv tsvector GENERATED ALWAYS AS (to_tsvector('english', content)) STORED;" 2>&1

-- Backend A is still the same session; its TpLocalIndexState cache
-- in src/index/state.c still points at the pre-ALTER TpSharedIndexState
-- if no relcache invalidation fires.
SELECT 'A-post-alter' AS marker;

-- Backend A: write via the (potentially stale) cache.
INSERT INTO docs (content)
SELECT 'late ' || g || ' lorem moonlight ' || md5(('l'||g)::text)
FROM generate_series(1, 50) g;

-- Force a spill. On the v1.2.0 bug this is what materialises the
-- corruption: the OLD in-memory memtable (with OLD CTIDs) gets
-- written into the NEW index file as a stale L0 segment.
SELECT 'spill_count=' || bm25_spill_index('docs_bm25');
SELECT 'A-spilled' AS marker;
EOF
)

    info "Backend A session output: ${output}"

    if echo "${output}" | grep -qE "could not read blocks|invalid \
(page|segment)|XX001"; then
        error "BUG (issue #390): Backend A session produced a \
storage-corruption error (likely stale CTID). Output: ${output}"
    fi

    # Sanity-check the session reached every phase; otherwise an
    # unexpected non-corruption error or a hanging psql would silently
    # weaken later assertions.
    local marker
    for marker in "A-primed" "A-post-alter" "A-spilled"; do
        if ! echo "${output}" | grep -qF "${marker}"; then
            error "Backend A session never reached marker \
'${marker}'. Full output:\n${output}"
        fi
    done

    # Spill must have happened; otherwise the bug's key
    # materialization step is not being exercised.
    local spill_count
    spill_count=$(echo "${output}" \
        | sed -n 's/^.*spill_count=\([0-9]*\).*$/\1/p' | head -1)
    if [ -z "${spill_count}" ] || [ "${spill_count}" -le 0 ]; then
        error "Spill produced ${spill_count:-no} entries; expected \
> 0 (the late inserts should populate the memtable before the \
explicit spill)."
    fi
    info "Spill count: ${spill_count}"

    # --- Assertions from a fresh backend (no primed cache) ---

    # 1. Index doc total must equal live heap row count. The bug
    #    inflates the segment doc count above the actual row count
    #    because the stale memtable was spilled as a second L0
    #    segment whose entries reference long-dead CTIDs.
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
a stale segment was written from a pre-ALTER memtable.\n\nFull \
summary:\n$(run_sql "SELECT bm25_summarize_index('docs_bm25');")"
    fi

    # 2. Run an index scan with a small LIMIT. On the v1.2.0 bug
    #    the top-k results may include CTIDs from the stale segment
    #    that point past the new heap's EOF, producing:
    #      ERROR: could not read blocks N..N in file "base/.../<fno>"
    #    'lorem' is in every pre-ALTER row, so on the bug the stale
    #    segment's entries score it highly and would dominate the
    #    top-k. Assert (a) no error, and (b) the result count equals
    #    LIMIT (proves the scan ran through to completion).
    local scan_q
    scan_q=$(run_sql_value "
        SELECT count(*)::text FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('lorem', 'docs_bm25')
            LIMIT 100
        ) s;
    " 2>&1)

    if echo "${scan_q}" | grep -qiE "could not read blocks|invalid \
(page|segment)|error"; then
        error "BUG (issue #390): index scan for 'lorem' failed: \
${scan_q}"
    fi
    if [ "${scan_q}" != "100" ]; then
        error "BUG (issue #390): top-100 index scan for 'lorem' \
returned ${scan_q} rows, expected 100."
    fi

    # 3. BM25 query using a token ONLY present in the post-ALTER
    #    late inserts ('moonlight'). The 50 late rows were appended
    #    after the ALTER through Backend A's (potentially stale)
    #    cache, then materialised via bm25_spill_index above. They
    #    must be queryable, must return live heap IDs, and the row
    #    count must equal 50.
    local moonlight_q
    moonlight_q=$(run_sql "
        SELECT s.id FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('moonlight', 'docs_bm25')
            LIMIT 100
        ) s
        JOIN docs d ON d.id = s.id
        WHERE d.content LIKE '%moonlight%';
    " 2>&1)

    if echo "${moonlight_q}" | grep -qiE "could not read blocks|invalid \
(page|segment)|error"; then
        error "BUG (issue #390): query for post-ALTER 'moonlight' \
inserts failed: ${moonlight_q}"
    fi

    # 50 late 'moonlight' rows; LIMIT 100 captures them all + 50
    # extras (no other rows match the join LIKE filter). Inner join
    # against the live heap ensures we count only valid CTIDs.
    local moonlight_count
    moonlight_count=$(run_sql_value "
        SELECT count(*)::text FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('moonlight', 'docs_bm25')
            LIMIT 100
        ) s
        JOIN docs d ON d.id = s.id
        WHERE d.content LIKE '%moonlight%';
    ")
    if [ "${moonlight_count}" != "50" ]; then
        error "BUG (issue #390): expected 50 live 'moonlight' rows \
via BM25 index, got ${moonlight_count}. The post-ALTER late inserts \
were not correctly indexed in the new file."
    fi

    # 4. Sanity check: heap must have actually been compacted by the
    #    ALTER. If the heap is the same size or larger, the rewrite
    #    didn't happen and the test isn't meaningfully exercising the
    #    bug.
    local heap_size_now
    heap_size_now=$(run_sql_value "SELECT pg_relation_size('docs')/8192;")
    if ! [[ "${heap_size_now}" =~ ^[0-9]+$ ]]; then
        error "Failed to obtain post-ALTER heap size (got \
'${heap_size_now}')."
    fi
    if [ "${heap_size_now}" -ge "${pre_heap_pages}" ]; then
        error "Heap was not compacted by ALTER (pre=${pre_heap_pages}, \
post=${heap_size_now}). Test setup invalid — bloat was not large \
enough or ALTER did not rewrite the heap."
    fi

    run_sql_quiet "DROP TABLE docs CASCADE;"
    log "✅ Test passed: no stale CTIDs after heap rewrite + \
reindex with primed backend cache"
}

main() {
    log "Starting pg_textsearch multi-backend reindex test (#390)..."

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"

    setup_test_db
    test_alter_table_add_column_stored

    log "🎉 multi_backend_reindex.sh: all tests passed"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
