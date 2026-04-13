#!/bin/bash
#
# Multi-index, multi-user, multi-schema concurrency tests.
# Verifies that concurrent memtable operations work correctly
# across privilege boundaries, schemas, and memory pressure.
#
# Test scenarios:
#   1. Multiple bm25 indexes on the same table
#   2. Cross-schema indexes with concurrent operations
#   3. Non-owner INSERT triggering auto-spill
#   4. Cross-user memory pressure spill
#   5. Concurrent spills from multiple backends
#   6. Query during cross-index spill
#   7. Mixed workload across many indexes
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55440
TEST_DB=pg_textsearch_multi_index_test
DATA_DIR="${SCRIPT_DIR}/../tmp_multi_index_test"
LOGFILE="${DATA_DIR}/postgres.log"

PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$($PG_CONFIG --bindir)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()   { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn()  { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }
info()  { echo -e "${BLUE}[$(date '+%H:%M:%S')] $1${NC}"; }

PASS_COUNT=0
FAIL_COUNT=0

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    log "PASS: $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo -e "${RED}[$(date '+%H:%M:%S')] FAIL: $1${NC}"
}

cleanup() {
    local exit_code=$?
    log "Cleaning up multi-index test (exit code: $exit_code)..."
    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m fast \
            &>/dev/null ||
        "${PGBINDIR}/pg_ctl" stop -D "${DATA_DIR}" -m immediate \
            &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up test PostgreSQL instance..."

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    "${PGBINDIR}/initdb" -D "${DATA_DIR}" \
        --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 40
shared_buffers = 128MB
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
log_min_messages = warning
shared_preload_libraries = 'pg_textsearch'
pg_textsearch.memory_limit = '256MB'
pg_textsearch.bulk_load_threshold = 0
pg_textsearch.memtable_spill_threshold = 0
EOF

    "${PGBINDIR}/pg_ctl" start -D "${DATA_DIR}" -l "${LOGFILE}" \
        -w || error "Failed to start PostgreSQL"

    "${PGBINDIR}/createdb" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        "${TEST_DB}"

    run_sql_quiet "CREATE EXTENSION pg_textsearch
                       VERSION '1.0.0-dev';"
}

run_sql() {
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "$1" 2>&1
}

run_sql_quiet() {
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "$1" >/dev/null 2>&1
}

run_sql_value() {
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -tAc "$1" 2>/dev/null
}

# Run SQL as a specific role.
run_sql_as() {
    local role="$1"
    local sql="$2"
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -U "${role}" -d "${TEST_DB}" -c "$sql" 2>&1
}

run_sql_as_quiet() {
    local role="$1"
    local sql="$2"
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -U "${role}" -d "${TEST_DB}" -c "$sql" >/dev/null 2>&1
}

run_sql_as_value() {
    local role="$1"
    local sql="$2"
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -U "${role}" -d "${TEST_DB}" -tAc "$sql" 2>/dev/null
}

# Reload server configuration after ALTER SYSTEM.
reload_conf() {
    run_sql_quiet "SELECT pg_reload_conf();"
    # Short pause for backends to pick up new GUC values.
    sleep 0.2
}

# Return the number of on-disk segments for an index.
# Always runs as superuser (bm25_summarize_index is revoked
# from PUBLIC).  Returns 0 when no segments exist.
get_segment_count() {
    local index_name="$1"
    local summary
    summary=$(run_sql_value \
        "SELECT bm25_summarize_index('$index_name');")
    local count
    count=$(echo "$summary" \
        | grep -oE 'Total: [0-9]+ segments' \
        | grep -oE '[0-9]+' || echo 0)
    echo "${count:-0}"
}

# Assert that an index has at least one on-disk segment,
# proving that an auto-spill occurred.
assert_spilled() {
    local label="$1"
    local index_name="$2"
    local seg_ct
    seg_ct=$(get_segment_count "$index_name")
    if [ "$seg_ct" -gt 0 ]; then
        pass "${label}: auto-spill confirmed (${seg_ct} segments)"
    else
        fail "${label}: expected segments > 0 but got ${seg_ct}"
    fi
}

# -------------------------------------------------------
# Test 1: Multiple bm25 indexes on the same table
#
# Two indexes on different columns of the same table.
# Insert, spill one, query both, spill the other.
# -------------------------------------------------------
test_multi_index_same_table() {
    log "Test 1: Multiple bm25 indexes on the same table"

    run_sql_quiet "CREATE TABLE multi_col (
        id serial PRIMARY KEY,
        title text,
        body text
    );"
    run_sql_quiet "CREATE INDEX idx_title ON multi_col
        USING bm25(title) WITH (text_config='english');"
    run_sql_quiet "CREATE INDEX idx_body ON multi_col
        USING bm25(body) WITH (text_config='english');"

    # Insert documents with distinct terms per column.
    run_sql_quiet "INSERT INTO multi_col (title, body)
        SELECT 'alpha_' || i || ' bravo_' || (i % 10),
               'charlie_' || i || ' delta_' || (i % 10)
        FROM generate_series(1, 500) i;"

    # Both indexes should be queryable before any spill.
    local title_ct
    title_ct=$(run_sql_value \
        "SELECT count(*) FROM multi_col
             WHERE title <@> 'bravo_3'::bm25query < 0;")
    if [ "$title_ct" -gt 0 ]; then
        pass "Test 1a: title index returns results before spill"
    else
        fail "Test 1a: title index returned 0 rows"
    fi

    local body_ct
    body_ct=$(run_sql_value \
        "SELECT count(*) FROM multi_col
             WHERE body <@> 'delta_3'::bm25query < 0;")
    if [ "$body_ct" -gt 0 ]; then
        pass "Test 1b: body index returns results before spill"
    else
        fail "Test 1b: body index returned 0 rows"
    fi

    # Spill one index, verify both still work.
    run_sql_quiet "SELECT bm25_spill_index('idx_title');"

    title_ct=$(run_sql_value \
        "SELECT count(*) FROM multi_col
             WHERE title <@> 'bravo_3'::bm25query < 0;")
    body_ct=$(run_sql_value \
        "SELECT count(*) FROM multi_col
             WHERE body <@> 'delta_3'::bm25query < 0;")

    if [ "$title_ct" -gt 0 ] && [ "$body_ct" -gt 0 ]; then
        pass "Test 1c: both indexes work after spilling title"
    else
        fail "Test 1c: title=$title_ct body=$body_ct after title spill"
    fi

    # Spill the other index, then insert more, verify consistency.
    run_sql_quiet "SELECT bm25_spill_index('idx_body');"
    run_sql_quiet "INSERT INTO multi_col (title, body)
        SELECT 'echo_' || i, 'foxtrot_' || i
        FROM generate_series(1, 200) i;"

    local echo_ct
    echo_ct=$(run_sql_value \
        "SELECT count(*) FROM multi_col
             WHERE title <@> 'echo'::bm25query < 0;")
    local fox_ct
    fox_ct=$(run_sql_value \
        "SELECT count(*) FROM multi_col
             WHERE body <@> 'foxtrot'::bm25query < 0;")

    if [ "$echo_ct" -eq 200 ] && [ "$fox_ct" -eq 200 ]; then
        pass "Test 1d: post-spill inserts visible in both indexes"
    else
        fail "Test 1d: echo=$echo_ct foxtrot=$fox_ct (expected 200)"
    fi

    run_sql_quiet "DROP TABLE multi_col CASCADE;"
    log "Test 1 complete"
}

# -------------------------------------------------------
# Test 2: Cross-schema indexes with concurrent operations
#
# Indexes in two different schemas; concurrent inserts
# from parallel backends, then verify query results.
# -------------------------------------------------------
test_cross_schema() {
    log "Test 2: Cross-schema concurrent operations"

    run_sql_quiet "CREATE SCHEMA schema_a;"
    run_sql_quiet "CREATE SCHEMA schema_b;"

    run_sql_quiet "CREATE TABLE schema_a.docs (
        id serial PRIMARY KEY, content text);"
    run_sql_quiet "CREATE TABLE schema_b.docs (
        id serial PRIMARY KEY, content text);"

    run_sql_quiet "CREATE INDEX idx_a ON schema_a.docs
        USING bm25(content) WITH (text_config='english');"
    run_sql_quiet "CREATE INDEX idx_b ON schema_b.docs
        USING bm25(content) WITH (text_config='english');"

    # Concurrent inserts into both schemas from parallel backends.
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "
        INSERT INTO schema_a.docs (content)
        SELECT 'schema_a_term_' || i || ' common_' || (i % 20)
        FROM generate_series(1, 1000) i;
    " >/dev/null 2>&1 &
    local pid_a=$!

    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "
        INSERT INTO schema_b.docs (content)
        SELECT 'schema_b_term_' || i || ' common_' || (i % 20)
        FROM generate_series(1, 1000) i;
    " >/dev/null 2>&1 &
    local pid_b=$!

    wait $pid_a || error "Schema A insert failed"
    wait $pid_b || error "Schema B insert failed"

    # Verify both indexes return correct results.
    local ct_a
    ct_a=$(run_sql_value \
        "SELECT count(*) FROM schema_a.docs
             WHERE content <@> to_bm25query('common_5',
                 'schema_a.idx_a') < 0;")
    local ct_b
    ct_b=$(run_sql_value \
        "SELECT count(*) FROM schema_b.docs
             WHERE content <@> to_bm25query('common_5',
                 'schema_b.idx_b') < 0;")

    if [ "$ct_a" -gt 0 ] && [ "$ct_b" -gt 0 ]; then
        pass "Test 2a: both schemas return results ($ct_a, $ct_b)"
    else
        fail "Test 2a: schema_a=$ct_a schema_b=$ct_b"
    fi

    # Spill one schema's index, verify the other is unaffected.
    run_sql_quiet "SELECT bm25_spill_index('schema_a.idx_a');"

    ct_b=$(run_sql_value \
        "SELECT count(*) FROM schema_b.docs
             WHERE content <@> to_bm25query('common_5',
                 'schema_b.idx_b') < 0;")
    if [ "$ct_b" -gt 0 ]; then
        pass "Test 2b: schema_b unaffected by schema_a spill"
    else
        fail "Test 2b: schema_b returned 0 after schema_a spill"
    fi

    # Insert more into schema_b while schema_a is on disk.
    run_sql_quiet "INSERT INTO schema_b.docs (content)
        SELECT 'xylophone_' || i
        FROM generate_series(1, 200) i;"

    local extra_ct
    extra_ct=$(run_sql_value \
        "SELECT count(*) FROM schema_b.docs
             WHERE content <@> to_bm25query('xylophone',
                 'schema_b.idx_b') < 0;")

    if [ "$extra_ct" -eq 200 ]; then
        pass "Test 2c: cross-schema post-spill inserts work"
    else
        fail "Test 2c: xylophone count=$extra_ct (expected 200)"
    fi

    run_sql_quiet "DROP SCHEMA schema_a CASCADE;"
    run_sql_quiet "DROP SCHEMA schema_b CASCADE;"
    log "Test 2 complete"
}

# -------------------------------------------------------
# Test 3: Non-owner INSERT triggers auto-spill
#
# Owner creates table + index. A non-owner role with
# INSERT privilege inserts enough data to trigger the
# per-index auto-spill. Verify data integrity for both
# owner and non-owner queries.
# -------------------------------------------------------
test_non_owner_spill() {
    log "Test 3: Non-owner INSERT triggers auto-spill"

    # Tighten per-index limit so spill triggers quickly.
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '4MB';"
    reload_conf

    # Create roles.
    run_sql_quiet "DO \$\$
    BEGIN
        IF EXISTS (SELECT 1 FROM pg_roles
                       WHERE rolname = 'idx_owner') THEN
            REASSIGN OWNED BY idx_owner TO CURRENT_USER;
            DROP OWNED BY idx_owner CASCADE;
            DROP ROLE idx_owner;
        END IF;
        IF EXISTS (SELECT 1 FROM pg_roles
                       WHERE rolname = 'inserter') THEN
            REASSIGN OWNED BY inserter TO CURRENT_USER;
            DROP OWNED BY inserter CASCADE;
            DROP ROLE inserter;
        END IF;
    END \$\$;"

    run_sql_quiet "CREATE ROLE idx_owner LOGIN;"
    run_sql_quiet "CREATE ROLE inserter LOGIN;"

    # Owner creates table and index.
    run_sql_quiet "GRANT CREATE ON SCHEMA public TO idx_owner;"
    run_sql_as_quiet idx_owner \
        "CREATE TABLE owned_docs (id serial PRIMARY KEY,
                                  content text);"
    run_sql_as_quiet idx_owner \
        "CREATE INDEX idx_owned ON owned_docs
             USING bm25(content)
             WITH (text_config='english');"

    # Grant INSERT and SELECT to the inserter role.
    run_sql_as_quiet idx_owner \
        "GRANT INSERT, SELECT ON owned_docs TO inserter;"
    run_sql_as_quiet idx_owner \
        "GRANT USAGE, SELECT ON SEQUENCE owned_docs_id_seq
             TO inserter;"

    # Inserter loads enough data to trigger per-index soft limit.
    # With 4MB limit, per-index soft = 512kB.
    run_sql_as_quiet inserter \
        "INSERT INTO owned_docs (content)
         SELECT 'nonowner_' || i || ' term_' || (i % 50)
                || ' ' || repeat('pad_', 10)
         FROM generate_series(1, 3000) i;"

    assert_spilled "Test 3" idx_owned

    # Both roles should see correct results.
    local owner_ct
    owner_ct=$(run_sql_as_value idx_owner \
        "SELECT count(*) FROM owned_docs
             WHERE content <@> 'term_7'::bm25query < 0;")
    local inserter_ct
    inserter_ct=$(run_sql_as_value inserter \
        "SELECT count(*) FROM owned_docs
             WHERE content <@> 'term_7'::bm25query < 0;")

    if [ "$owner_ct" -gt 0 ] && [ "$owner_ct" = "$inserter_ct" ]; then
        pass "Test 3a: owner and inserter see same results ($owner_ct)"
    else
        fail "Test 3a: owner=$owner_ct inserter=$inserter_ct"
    fi

    # Verify inserter cannot call bm25_spill_index (owner check).
    local spill_err
    spill_err=$(run_sql_as inserter \
        "SELECT bm25_spill_index('idx_owned');" 2>&1 || true)
    if echo "$spill_err" | grep -qi "must be owner"; then
        pass "Test 3b: non-owner blocked from explicit spill"
    else
        fail "Test 3b: expected 'must be owner', got: $spill_err"
    fi

    # Insert more as inserter, verify auto-spill keeps working.
    run_sql_as_quiet inserter \
        "INSERT INTO owned_docs (content)
         SELECT 'wave2_' || i
         FROM generate_series(1, 500) i;"

    local wave2_ct
    wave2_ct=$(run_sql_as_value inserter \
        "SELECT count(*) FROM owned_docs
             WHERE content <@> 'wave2'::bm25query < 0;")
    if [ "$wave2_ct" -eq 500 ]; then
        pass "Test 3c: second insert wave visible after auto-spill"
    else
        fail "Test 3c: wave2 count=$wave2_ct (expected 500)"
    fi

    # Cleanup.
    run_sql_quiet "DROP TABLE owned_docs CASCADE;"
    run_sql_quiet "DROP OWNED BY idx_owner CASCADE;"
    run_sql_quiet "DROP OWNED BY inserter CASCADE;"
    run_sql_quiet "DROP ROLE idx_owner;"
    run_sql_quiet "DROP ROLE inserter;"
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '256MB';"
    reload_conf
    log "Test 3 complete"
}

# -------------------------------------------------------
# Test 4: Cross-user memory pressure spill
#
# Two users each own an index. User A inserts heavily.
# Global memory pressure triggers spill of User B's index.
# Verify User B's index is intact afterward.
# -------------------------------------------------------
test_cross_user_pressure_spill() {
    log "Test 4: Cross-user memory pressure spill"

    # Tight global limit: 4MB total, per-index soft = 512kB.
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '4MB';"
    reload_conf

    # Create two owner roles.
    run_sql_quiet "DO \$\$
    BEGIN
        IF EXISTS (SELECT 1 FROM pg_roles
                       WHERE rolname = 'user_a') THEN
            REASSIGN OWNED BY user_a TO CURRENT_USER;
            DROP OWNED BY user_a CASCADE;
            DROP ROLE user_a;
        END IF;
        IF EXISTS (SELECT 1 FROM pg_roles
                       WHERE rolname = 'user_b') THEN
            REASSIGN OWNED BY user_b TO CURRENT_USER;
            DROP OWNED BY user_b CASCADE;
            DROP ROLE user_b;
        END IF;
    END \$\$;"

    run_sql_quiet "CREATE ROLE user_a LOGIN;"
    run_sql_quiet "CREATE ROLE user_b LOGIN;"
    run_sql_quiet "GRANT CREATE ON SCHEMA public TO user_a;"
    run_sql_quiet "GRANT CREATE ON SCHEMA public TO user_b;"

    # Each user creates their own table and index.
    run_sql_as_quiet user_b \
        "CREATE TABLE docs_b (id serial PRIMARY KEY,
                              content text);"
    run_sql_as_quiet user_b \
        "CREATE INDEX idx_b ON docs_b
             USING bm25(content)
             WITH (text_config='english');"

    run_sql_as_quiet user_a \
        "CREATE TABLE docs_a (id serial PRIMARY KEY,
                              content text);"
    run_sql_as_quiet user_a \
        "CREATE INDEX idx_a ON docs_a
             USING bm25(content)
             WITH (text_config='english');"

    # User B seeds their index (small load, stays in memtable).
    run_sql_as_quiet user_b \
        "INSERT INTO docs_b (content)
         SELECT 'userb_doc_' || i || ' topic_' || (i % 30)
         FROM generate_series(1, 500) i;"

    # Verify User B's data is queryable.
    local b_before
    b_before=$(run_sql_as_value user_b \
        "SELECT count(*) FROM docs_b
             WHERE content <@> 'topic_5'::bm25query < 0;")
    if [ "$b_before" -le 0 ]; then
        fail "Test 4 setup: user_b has no results"
        # Continue anyway to see if spill path works.
    fi

    # User A floods their index to blow through global soft limit.
    # This should trigger eviction of the largest memtable
    # (possibly user_b's), or spill user_a's own.
    run_sql_as_quiet user_a \
        "INSERT INTO docs_a (content)
         SELECT 'usera_flood_' || i || ' ' || repeat('pad_', 15)
         FROM generate_series(1, 5000) i;"

    # At least one index must have been auto-spilled.
    local seg_a seg_b
    seg_a=$(get_segment_count idx_a)
    seg_b=$(get_segment_count idx_b)
    if [ "$((seg_a + seg_b))" -gt 0 ]; then
        pass "Test 4: auto-spill confirmed (idx_a=${seg_a}, idx_b=${seg_b} segments)"
    else
        fail "Test 4: no auto-spill occurred (idx_a=${seg_a}, idx_b=${seg_b})"
    fi

    # Verify User B's index is still intact and queryable.
    local b_after
    b_after=$(run_sql_as_value user_b \
        "SELECT count(*) FROM docs_b
             WHERE content <@> 'topic_5'::bm25query < 0;")

    if [ "$b_after" = "$b_before" ]; then
        pass "Test 4a: user_b results intact after user_a pressure ($b_after)"
    else
        fail "Test 4a: user_b before=$b_before after=$b_after"
    fi

    # User A's data should also be fully queryable.
    local a_ct
    a_ct=$(run_sql_as_value user_a \
        "SELECT count(*) FROM docs_a
             WHERE content <@> 'usera_flood'::bm25query < 0;")
    if [ "$a_ct" -eq 5000 ]; then
        pass "Test 4b: user_a data intact ($a_ct rows)"
    else
        fail "Test 4b: user_a count=$a_ct (expected 5000)"
    fi

    # User B inserts more after the pressure event.
    run_sql_as_quiet user_b \
        "INSERT INTO docs_b (content)
         SELECT 'post_pressure_' || i
         FROM generate_series(1, 200) i;"

    local post_ct
    post_ct=$(run_sql_as_value user_b \
        "SELECT count(*) FROM docs_b
             WHERE content <@> 'post_pressure'::bm25query < 0;")
    if [ "$post_ct" -eq 200 ]; then
        pass "Test 4c: user_b inserts work after pressure spill"
    else
        fail "Test 4c: post_pressure count=$post_ct (expected 200)"
    fi

    # Cleanup.
    run_sql_quiet "DROP TABLE docs_a CASCADE;"
    run_sql_quiet "DROP TABLE docs_b CASCADE;"
    run_sql_quiet "DROP OWNED BY user_a CASCADE;"
    run_sql_quiet "DROP OWNED BY user_b CASCADE;"
    run_sql_quiet "DROP ROLE user_a;"
    run_sql_quiet "DROP ROLE user_b;"
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '256MB';"
    reload_conf
    log "Test 4 complete"
}

# -------------------------------------------------------
# Test 5: Concurrent spills from multiple backends
#
# Multiple backends insert into different indexes and
# hit the spill threshold around the same time. Verify
# no deadlocks and data integrity.
# -------------------------------------------------------
test_concurrent_spills() {
    log "Test 5: Concurrent spills from multiple backends"

    # Tight per-index limit so each backend spills.
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '4MB';"
    reload_conf

    local N=4
    for i in $(seq 1 $N); do
        run_sql_quiet "CREATE TABLE spill_t${i} (
            id serial PRIMARY KEY, content text);"
        run_sql_quiet "CREATE INDEX idx_spill_${i} ON spill_t${i}
            USING bm25(content) WITH (text_config='english');"
    done

    # Launch parallel inserts — each backend floods its own index.
    local pids=()
    for i in $(seq 1 $N); do
        "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
            -d "${TEST_DB}" -c "
            INSERT INTO spill_t${i} (content)
            SELECT 'idx${i}_term_' || j || ' '
                   || repeat('fill_', 8)
            FROM generate_series(1, 2000) j;
        " >/dev/null 2>&1 &
        pids+=($!)
    done

    local any_fail=0
    for pid in "${pids[@]}"; do
        if ! wait "$pid"; then
            any_fail=1
        fi
    done

    if [ "$any_fail" -eq 1 ]; then
        fail "Test 5a: one or more concurrent inserts failed"
    else
        pass "Test 5a: all concurrent inserts completed"
    fi

    # Verify each index has all its data AND was auto-spilled.
    local all_ok=1
    local total_segments=0
    for i in $(seq 1 $N); do
        local ct
        ct=$(run_sql_value \
            "SELECT count(*) FROM spill_t${i}
                 WHERE content <@> 'idx${i}_term'::bm25query < 0;")
        if [ "$ct" -ne 2000 ]; then
            fail "Test 5b: spill_t${i} count=$ct (expected 2000)"
            all_ok=0
        fi
        local seg
        seg=$(get_segment_count "idx_spill_${i}")
        total_segments=$((total_segments + seg))
    done
    if [ "$all_ok" -eq 1 ]; then
        pass "Test 5b: all $N indexes have correct counts"
    fi
    if [ "$total_segments" -ge "$N" ]; then
        pass "Test 5c: auto-spills confirmed ($total_segments total segments across $N indexes)"
    else
        fail "Test 5c: expected >= $N segments, got $total_segments"
    fi

    # Cleanup.
    for i in $(seq 1 $N); do
        run_sql_quiet "DROP TABLE spill_t${i} CASCADE;"
    done
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '256MB';"
    reload_conf
    log "Test 5 complete"
}

# -------------------------------------------------------
# Test 6: Query during cross-index spill
#
# Backend A continuously queries index_1 while Backend B
# inserts into index_2 under tight memory, potentially
# triggering cross-index spill of index_1.
# -------------------------------------------------------
test_query_during_spill() {
    log "Test 6: Query during cross-index spill"

    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '4MB';"
    reload_conf

    # Set up two tables with indexes.
    run_sql_quiet "CREATE TABLE query_target (
        id serial PRIMARY KEY, content text);"
    run_sql_quiet "CREATE INDEX idx_qt ON query_target
        USING bm25(content) WITH (text_config='english');"
    run_sql_quiet "INSERT INTO query_target (content)
        SELECT 'target_' || i || ' keyword_' || (i % 20)
        FROM generate_series(1, 1000) i;"

    run_sql_quiet "CREATE TABLE spill_driver (
        id serial PRIMARY KEY, content text);"
    run_sql_quiet "CREATE INDEX idx_sd ON spill_driver
        USING bm25(content) WITH (text_config='english');"

    # Expected result count for validation.
    local expected_ct
    expected_ct=$(run_sql_value \
        "SELECT count(*) FROM query_target
             WHERE content <@> 'keyword_5'::bm25query < 0;")

    # Backend A: query loop.
    local query_ok_file="${DATA_DIR}/query_ok"
    echo "0" > "$query_ok_file"
    (
        local failures=0
        for round in $(seq 1 20); do
            local ct
            ct=$("${PGBINDIR}/psql" -h "${DATA_DIR}" \
                -p "${TEST_PORT}" -d "${TEST_DB}" -tAc \
                "SELECT count(*) FROM query_target
                     WHERE content <@> 'keyword_5'::bm25query
                     < 0;" 2>/dev/null)
            if [ "$ct" != "$expected_ct" ]; then
                failures=$((failures + 1))
            fi
            sleep 0.05
        done
        echo "$failures" > "$query_ok_file"
    ) &
    local query_pid=$!

    # Backend B: flood spill_driver to create memory pressure.
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "
        INSERT INTO spill_driver (content)
        SELECT 'driver_' || i || ' ' || repeat('bulk_', 15)
        FROM generate_series(1, 5000) i;
    " >/dev/null 2>&1 &
    local insert_pid=$!

    wait $insert_pid || warn "spill_driver insert had an error"
    wait $query_pid

    local query_failures
    query_failures=$(cat "$query_ok_file")
    if [ "$query_failures" -eq 0 ]; then
        pass "Test 6a: queries returned correct results during spill"
    else
        fail "Test 6a: $query_failures query rounds returned wrong count"
    fi

    # Verify that memory pressure actually triggered spills.
    local seg_qt seg_sd
    seg_qt=$(get_segment_count idx_qt)
    seg_sd=$(get_segment_count idx_sd)
    if [ "$((seg_qt + seg_sd))" -gt 0 ]; then
        pass "Test 6b: auto-spill confirmed (qt=${seg_qt}, sd=${seg_sd} segments)"
    else
        fail "Test 6b: no auto-spill occurred (qt=${seg_qt}, sd=${seg_sd})"
    fi

    run_sql_quiet "DROP TABLE query_target CASCADE;"
    run_sql_quiet "DROP TABLE spill_driver CASCADE;"
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '256MB';"
    reload_conf
    log "Test 6 complete"
}

# -------------------------------------------------------
# Test 7: Mixed workload across many indexes
#
# Create several indexes across different schemas and
# roles, then run a mixed workload of inserts, queries,
# and explicit spills from multiple backends.
# -------------------------------------------------------
test_mixed_workload() {
    log "Test 7: Mixed workload across many indexes"

    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '4MB';"
    reload_conf

    # Create schemas and roles.
    run_sql_quiet "DO \$\$
    BEGIN
        IF EXISTS (SELECT 1 FROM pg_roles
                       WHERE rolname = 'mix_owner') THEN
            REASSIGN OWNED BY mix_owner TO CURRENT_USER;
            DROP OWNED BY mix_owner CASCADE;
            DROP ROLE mix_owner;
        END IF;
        IF EXISTS (SELECT 1 FROM pg_roles
                       WHERE rolname = 'mix_writer') THEN
            REASSIGN OWNED BY mix_writer TO CURRENT_USER;
            DROP OWNED BY mix_writer CASCADE;
            DROP ROLE mix_writer;
        END IF;
    END \$\$;"

    run_sql_quiet "CREATE SCHEMA mix_s1;"
    run_sql_quiet "CREATE SCHEMA mix_s2;"
    run_sql_quiet "CREATE ROLE mix_owner LOGIN;"
    run_sql_quiet "CREATE ROLE mix_writer LOGIN;"
    run_sql_quiet "GRANT ALL ON SCHEMA mix_s1 TO mix_owner;"
    run_sql_quiet "GRANT USAGE ON SCHEMA mix_s1 TO mix_writer;"
    run_sql_quiet "GRANT USAGE ON SCHEMA mix_s2 TO mix_writer;"

    # Owner creates tables and indexes in different schemas.
    run_sql_as_quiet mix_owner \
        "CREATE TABLE mix_s1.t1 (id serial PRIMARY KEY,
                                 content text);"
    run_sql_as_quiet mix_owner \
        "CREATE INDEX idx_m1 ON mix_s1.t1
             USING bm25(content) WITH (text_config='english');"

    # Superuser creates a table in mix_s2.
    run_sql_quiet "CREATE TABLE mix_s2.t2 (id serial PRIMARY KEY,
                                           content text);"
    run_sql_quiet "CREATE INDEX idx_m2 ON mix_s2.t2
        USING bm25(content) WITH (text_config='english');"

    # Public schema table.
    run_sql_quiet "CREATE TABLE mix_pub (id serial PRIMARY KEY,
                                         content text);"
    run_sql_quiet "CREATE INDEX idx_mp ON mix_pub
        USING bm25(content) WITH (text_config='english');"

    # Grant insert privileges.
    run_sql_as_quiet mix_owner \
        "GRANT INSERT, SELECT ON mix_s1.t1 TO mix_writer;"
    run_sql_as_quiet mix_owner \
        "GRANT USAGE, SELECT ON SEQUENCE mix_s1.t1_id_seq
             TO mix_writer;"
    run_sql_quiet "GRANT INSERT, SELECT ON mix_s2.t2
                       TO mix_writer;"
    run_sql_quiet "GRANT USAGE, SELECT ON SEQUENCE mix_s2.t2_id_seq
                       TO mix_writer;"
    run_sql_quiet "GRANT INSERT, SELECT ON mix_pub TO mix_writer;"
    run_sql_quiet "GRANT USAGE, SELECT ON SEQUENCE mix_pub_id_seq
                       TO mix_writer;"

    # Launch concurrent workload: 3 backends inserting into
    # different tables, 1 backend querying.
    # Each backend inserts 3000 rows with padding to exceed
    # the 512kB per-index soft limit (4MB / 8).
    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -U mix_writer -d "${TEST_DB}" -c "
        INSERT INTO mix_s1.t1 (content)
        SELECT 'schema1_' || i || ' ' || repeat('w_', 10)
        FROM generate_series(1, 3000) i;
    " >/dev/null 2>&1 &
    local pid1=$!

    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -U mix_writer -d "${TEST_DB}" -c "
        INSERT INTO mix_s2.t2 (content)
        SELECT 'schema2_' || i || ' ' || repeat('x_', 10)
        FROM generate_series(1, 3000) i;
    " >/dev/null 2>&1 &
    local pid2=$!

    "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "
        INSERT INTO mix_pub (content)
        SELECT 'public_' || i || ' ' || repeat('y_', 10)
        FROM generate_series(1, 3000) i;
    " >/dev/null 2>&1 &
    local pid3=$!

    # Concurrent query loop while inserts are running.
    local query_results="${DATA_DIR}/mix_query_results"
    (
        for round in $(seq 1 15); do
            "${PGBINDIR}/psql" -h "${DATA_DIR}" -p "${TEST_PORT}" \
                -d "${TEST_DB}" -tAc "
                SELECT count(*) FROM mix_pub
                    WHERE content <@> 'public'::bm25query < 0;
            " >> "$query_results" 2>/dev/null
            sleep 0.1
        done
    ) &
    local qpid=$!

    wait $pid1 || warn "mix_s1 insert had issues"
    wait $pid2 || warn "mix_s2 insert had issues"
    wait $pid3 || warn "mix_pub insert had issues"
    wait $qpid

    # Verify final counts across all indexes.
    local ct1
    ct1=$(run_sql_value \
        "SELECT count(*) FROM mix_s1.t1
             WHERE content <@> to_bm25query('schema1',
                 'mix_s1.idx_m1') < 0;")
    local ct2
    ct2=$(run_sql_value \
        "SELECT count(*) FROM mix_s2.t2
             WHERE content <@> to_bm25query('schema2',
                 'mix_s2.idx_m2') < 0;")
    local ct3
    ct3=$(run_sql_value \
        "SELECT count(*) FROM mix_pub
             WHERE content <@> 'public'::bm25query < 0;")

    if [ "$ct1" -eq 3000 ] && [ "$ct2" -eq 3000 ] \
       && [ "$ct3" -eq 3000 ]; then
        pass "Test 7a: all three indexes have 3000 rows"
    else
        fail "Test 7a: s1=$ct1 s2=$ct2 pub=$ct3 (expected 3000)"
    fi

    # Verify auto-spills happened under the 8MB global limit.
    local seg_m1 seg_m2 seg_mp
    seg_m1=$(get_segment_count 'mix_s1.idx_m1')
    seg_m2=$(get_segment_count 'mix_s2.idx_m2')
    seg_mp=$(get_segment_count idx_mp)
    local total_seg=$((seg_m1 + seg_m2 + seg_mp))
    if [ "$total_seg" -gt 0 ]; then
        pass "Test 7b: auto-spill confirmed (m1=${seg_m1}, m2=${seg_m2}, mp=${seg_mp})"
    else
        fail "Test 7b: no auto-spill occurred across 3 indexes"
    fi

    # Verify query results were monotonically non-decreasing
    # (the count should never go backward).
    if [ -f "$query_results" ]; then
        local prev=0
        local monotonic=1
        while IFS= read -r line; do
            if [ -n "$line" ] && [ "$line" -lt "$prev" ]; then
                monotonic=0
                break
            fi
            prev="$line"
        done < "$query_results"

        if [ "$monotonic" -eq 1 ]; then
            pass "Test 7c: query counts monotonically non-decreasing"
        else
            fail "Test 7c: query count went backward"
        fi
    else
        warn "Test 7b: no query results file"
    fi

    # Cleanup.
    run_sql_quiet "DROP TABLE mix_pub CASCADE;"
    run_sql_quiet "DROP SCHEMA mix_s1 CASCADE;"
    run_sql_quiet "DROP SCHEMA mix_s2 CASCADE;"
    run_sql_quiet "DROP OWNED BY mix_owner CASCADE;"
    run_sql_quiet "DROP OWNED BY mix_writer CASCADE;"
    run_sql_quiet "DROP ROLE mix_owner;"
    run_sql_quiet "DROP ROLE mix_writer;"
    run_sql_quiet "ALTER SYSTEM SET pg_textsearch.memory_limit
                       = '256MB';"
    reload_conf
    log "Test 7 complete"
}

# -------------------------------------------------------
# Main
# -------------------------------------------------------

log "=========================================="
log "Multi-index / multi-user / multi-schema"
log "concurrency tests for pg_textsearch"
log "=========================================="

setup_test_db

test_multi_index_same_table
test_cross_schema
test_non_owner_spill
test_cross_user_pressure_spill
test_concurrent_spills
test_query_during_spill
test_mixed_workload

log "=========================================="
if [ "$FAIL_COUNT" -gt 0 ]; then
    error "$FAIL_COUNT test(s) FAILED, $PASS_COUNT passed"
else
    log "All tests passed ($PASS_COUNT assertions)"
fi
