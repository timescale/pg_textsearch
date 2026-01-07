#!/bin/bash
#
# Long-running stress tests for pg_textsearch
#
# Designed for nightly CI with memory leak detection. Each test is time-based
# to ensure sustained load over the configured duration.
#
# Default nightly configuration targets:
#   - 30 minutes total runtime
#   - 100,000+ documents inserted
#   - 100+ segments created
#   - 5+ concurrent database connections
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT="${TEST_PORT:-55436}"
TEST_DB=pg_textsearch_stress_test
DATA_DIR="${SCRIPT_DIR}/../tmp_stress_test"
LOGFILE="${DATA_DIR}/postgres.log"

# Test parameters - can be overridden via environment
STRESS_DURATION_MINUTES="${STRESS_DURATION_MINUTES:-5}"
SPILL_THRESHOLD="${SPILL_THRESHOLD:-1000}"  # Low threshold for more segments
DOCS_PER_BATCH="${DOCS_PER_BATCH:-100}"
CONCURRENT_READERS="${CONCURRENT_READERS:-6}"       # Readers per test
CONCURRENT_WRITERS="${CONCURRENT_WRITERS:-3}"       # Writers for concurrent tests
VALIDATION_INTERVAL="${VALIDATION_INTERVAL:-30}"    # seconds between validations

# Calculated durations for each test (in seconds)
# Test 1: 40% of time, Test 2: 25%, Test 3: 15%, Test 4: 10%, Test 5: 10%
TOTAL_SECONDS=$((STRESS_DURATION_MINUTES * 60))
TEST1_DURATION=$((TOTAL_SECONDS * 40 / 100))
TEST2_DURATION=$((TOTAL_SECONDS * 25 / 100))
TEST3_DURATION=$((TOTAL_SECONDS * 15 / 100))
TEST4_DURATION=$((TOTAL_SECONDS * 10 / 100))
TEST5_DURATION=$((TOTAL_SECONDS * 10 / 100))

# Metrics tracking
TOTAL_DOCS_INSERTED=0
TOTAL_SEGMENTS_CREATED=0
TOTAL_QUERIES_EXECUTED=0
TOTAL_ERRORS=0

# Colors for output
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
    log "Cleaning up stress test environment (exit code: $exit_code)..."

    # Kill any background processes
    jobs -p | xargs -r kill 2>/dev/null || true

    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up stress test PostgreSQL instance..."

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 30
shared_buffers = 256MB
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
log_min_messages = warning
log_statement = 'none'
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w || error "Failed to start PostgreSQL"

    createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "CREATE EXTENSION pg_textsearch;" >/dev/null

    # Set low spill threshold
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c \
        "ALTER SYSTEM SET pg_textsearch.memtable_spill_threshold = ${SPILL_THRESHOLD};" >/dev/null
    pg_ctl reload -D "${DATA_DIR}" >/dev/null

    log "PostgreSQL started with memtable_spill_threshold = ${SPILL_THRESHOLD}"
}

run_sql() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

run_sql_quiet() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" >/dev/null 2>&1
}

run_sql_value() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc "$1" 2>/dev/null
}

get_segment_count() {
    local idx_name=$1
    local summary=$(run_sql_value "SELECT bm25_summarize_index('${idx_name}');" 2>/dev/null || echo "")
    # Format is "Total: N segments, ..." - extract the number
    echo "$summary" | grep -o 'Total: [0-9]* segments' | grep -o '[0-9]*' || echo "0"
}

#
# Generate random text content for documents
#
generate_doc_content() {
    local doc_id=$1
    local words=("alpha" "beta" "gamma" "delta" "epsilon" "zeta" "eta" "theta"
                 "iota" "kappa" "lambda" "mu" "nu" "xi" "omicron" "pi" "rho"
                 "sigma" "tau" "upsilon" "phi" "chi" "psi" "omega" "database"
                 "search" "query" "index" "document" "text" "ranking" "score"
                 "postgres" "extension" "memory" "segment" "posting" "term")

    local content="document ${doc_id}"
    local num_words=$((RANDOM % 30 + 10))
    for ((i=0; i<num_words; i++)); do
        content="${content} ${words[$((RANDOM % ${#words[@]}))]}"
    done
    echo "$content"
}

#
# Test 1: Extended insert/query cycle with continuous segment creation
#
# Target: Insert thousands of documents while running queries, creating many segments
# Duration: 40% of total time
# Expected: ~4000 docs/min at default settings, 40+ segments
#
test_extended_insert_query() {
    local duration=$TEST1_DURATION
    log "Test 1: Extended insert/query cycle (${duration} seconds)"

    run_sql_quiet "CREATE TABLE stress_docs (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "CREATE INDEX stress_idx ON stress_docs USING bm25(content) WITH (text_config='english');"

    local start_time=$(date +%s)
    local end_time=$((start_time + duration))
    local batch_num=0
    local docs_inserted=0
    local queries_run=0
    local errors=0
    local last_validation=$start_time
    local last_segment_count=0

    info "Duration: ${duration}s, target: ~$((duration * 70)) docs, ~$((duration * 70 / SPILL_THRESHOLD)) segments"

    while [ $(date +%s) -lt $end_time ]; do
        batch_num=$((batch_num + 1))

        # Insert a batch of documents
        local insert_sql="INSERT INTO stress_docs (content) VALUES"
        local first=true
        for ((i=1; i<=DOCS_PER_BATCH; i++)); do
            local content=$(generate_doc_content $((docs_inserted + i)))
            if [ "$first" = true ]; then
                insert_sql="${insert_sql} ('${content}')"
                first=false
            else
                insert_sql="${insert_sql}, ('${content}')"
            fi
        done
        insert_sql="${insert_sql};"

        if ! run_sql_quiet "$insert_sql"; then
            warn "Batch $batch_num insert failed"
            errors=$((errors + 1))
        fi
        docs_inserted=$((docs_inserted + DOCS_PER_BATCH))

        # Run queries - fetch actual rows, not just count
        local query_terms=("alpha" "database" "search" "document" "gamma delta" "postgres extension")
        for term in "${query_terms[@]}"; do
            # Heavy query: fetch top 50 results with scores
            local result=$(run_sql_value "
                SELECT COUNT(*) FROM (
                    SELECT id, content <@> to_bm25query('${term}', 'stress_idx') as score
                    FROM stress_docs
                    ORDER BY content <@> to_bm25query('${term}', 'stress_idx')
                    LIMIT 50
                ) t WHERE score < 0;" 2>/dev/null)
            queries_run=$((queries_run + 1))
            if [ -z "$result" ]; then
                errors=$((errors + 1))
            fi
        done

        # Report progress every 20 batches
        if [ $((batch_num % 20)) -eq 0 ]; then
            local segment_count=$(get_segment_count 'stress_idx')
            if [ "$segment_count" != "$last_segment_count" ]; then
                last_segment_count=$segment_count
            fi
            local elapsed=$(($(date +%s) - start_time))
            local rate=$((docs_inserted * 60 / (elapsed + 1)))
            info "Progress: ${docs_inserted} docs, ${segment_count} segments, ${rate} docs/min"
        fi

        # Periodic validation
        local now=$(date +%s)
        if [ $((now - last_validation)) -ge $VALIDATION_INTERVAL ]; then
            # Verify document count matches
            local table_count=$(run_sql_value "SELECT COUNT(*) FROM stress_docs;")
            if [ "$table_count" != "$docs_inserted" ]; then
                warn "Document count mismatch: expected $docs_inserted, got $table_count"
                errors=$((errors + 1))
            fi

            # Verify top results have valid negative scores
            local valid_scores=$(run_sql_value "
                SELECT COUNT(*) FROM (
                    SELECT content <@> to_bm25query('database', 'stress_idx') as score
                    FROM stress_docs
                    ORDER BY content <@> to_bm25query('database', 'stress_idx')
                    LIMIT 20
                ) t WHERE score < 0;")

            if [ -z "$valid_scores" ] || [ "$valid_scores" -eq 0 ]; then
                warn "Score validation failed: no valid negative scores"
                errors=$((errors + 1))
            fi

            last_validation=$now
        fi

        sleep 0.02
    done

    # Final statistics
    local final_segments=$(get_segment_count 'stress_idx')
    local final_count=$(run_sql_value "SELECT COUNT(*) FROM stress_docs;")

    info "Final: ${final_count} documents, ${final_segments} segments, ${queries_run} queries, ${errors} errors"

    # Update global metrics
    TOTAL_DOCS_INSERTED=$((TOTAL_DOCS_INSERTED + docs_inserted))
    TOTAL_SEGMENTS_CREATED=$((TOTAL_SEGMENTS_CREATED + final_segments))
    TOTAL_QUERIES_EXECUTED=$((TOTAL_QUERIES_EXECUTED + queries_run))
    TOTAL_ERRORS=$((TOTAL_ERRORS + errors))

    if [ $errors -eq 0 ]; then
        log "Test 1 PASSED"
    else
        warn "Test 1 completed with $errors errors"
    fi

    run_sql_quiet "DROP TABLE stress_docs CASCADE;"
}

#
# Test 2: Concurrent readers and writers
#
# Target: Multiple readers and writers operating simultaneously
# Duration: 25% of total time
# Expected: 6 readers + 3 writers = 9 concurrent connections, heavy load
#
test_concurrent_stress() {
    local duration=$TEST2_DURATION
    local total_connections=$((CONCURRENT_READERS + CONCURRENT_WRITERS))
    log "Test 2: Concurrent stress (${duration}s, ${CONCURRENT_READERS} readers + ${CONCURRENT_WRITERS} writers = ${total_connections} connections)"

    run_sql_quiet "CREATE TABLE conc_docs (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "CREATE INDEX conc_idx ON conc_docs USING bm25(content) WITH (text_config='english');"

    # Initial data load (500 docs)
    info "Loading 500 initial documents..."
    for ((batch=1; batch<=10; batch++)); do
        local insert_sql="INSERT INTO conc_docs (content) VALUES"
        local first=true
        for ((i=1; i<=50; i++)); do
            local content=$(generate_doc_content $((batch * 50 + i)))
            if [ "$first" = true ]; then
                insert_sql="${insert_sql} ('${content}')"
                first=false
            else
                insert_sql="${insert_sql}, ('${content}')"
            fi
        done
        run_sql_quiet "${insert_sql};"
    done
    run_sql_quiet "SELECT bm25_spill_index('conc_idx');"

    local start_time=$(date +%s)
    local end_time=$((start_time + duration))

    # Start concurrent readers
    local reader_pids=()
    for ((r=1; r<=CONCURRENT_READERS; r++)); do
        {
            local queries=0
            local rows_fetched=0
            local errors=0
            local search_terms=("alpha" "beta" "gamma" "database" "search" "document" "postgres" "index")
            while [ $(date +%s) -lt $end_time ]; do
                local term="${search_terms[$((RANDOM % ${#search_terms[@]}))]}"
                # Heavy query: fetch actual rows with scores
                local result=$(psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc "
                    SELECT COUNT(*) FROM (
                        SELECT id, content, content <@> to_bm25query('${term}', 'conc_idx') as score
                        FROM conc_docs
                        ORDER BY content <@> to_bm25query('${term}', 'conc_idx')
                        LIMIT 100
                    ) t;" 2>/dev/null)
                if [ -n "$result" ]; then
                    rows_fetched=$((rows_fetched + result))
                else
                    errors=$((errors + 1))
                fi
                queries=$((queries + 1))
                sleep 0.03
            done
            echo "Reader $r: $queries queries, $rows_fetched rows, $errors errors" >&2
        } &
        reader_pids+=($!)
    done

    # Start concurrent writers
    local writer_pids=()
    for ((w=1; w<=CONCURRENT_WRITERS; w++)); do
        {
            local docs=0
            local spills=0
            local errors=0
            local base_id=$((w * 100000))
            while [ $(date +%s) -lt $end_time ]; do
                local insert_sql="INSERT INTO conc_docs (content) VALUES"
                local first=true
                for ((i=1; i<=20; i++)); do
                    local content="writer${w} doc$((base_id + docs + i)) alpha beta gamma database search query index"
                    if [ "$first" = true ]; then
                        insert_sql="${insert_sql} ('${content}')"
                        first=false
                    else
                        insert_sql="${insert_sql}, ('${content}')"
                    fi
                done
                if psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "${insert_sql};" >/dev/null 2>&1; then
                    docs=$((docs + 20))
                else
                    errors=$((errors + 1))
                fi

                # Periodic spill from this writer
                if [ $((docs % 500)) -eq 0 ] && [ $docs -gt 0 ]; then
                    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "SELECT bm25_spill_index('conc_idx');" >/dev/null 2>&1
                    spills=$((spills + 1))
                fi

                sleep 0.02
            done
            echo "Writer $w: $docs docs, $spills spills, $errors errors" >&2
        } &
        writer_pids+=($!)
    done

    # Wait for all processes
    local failures=0
    for pid in "${reader_pids[@]}" "${writer_pids[@]}"; do
        if ! wait $pid 2>/dev/null; then
            failures=$((failures + 1))
        fi
    done

    local final_segments=$(get_segment_count 'conc_idx')
    local final_docs=$(run_sql_value "SELECT COUNT(*) FROM conc_docs;")

    info "Final: ${final_docs} documents, ${final_segments} segments"

    # Update global metrics
    TOTAL_DOCS_INSERTED=$((TOTAL_DOCS_INSERTED + final_docs))
    TOTAL_SEGMENTS_CREATED=$((TOTAL_SEGMENTS_CREATED + final_segments))
    TOTAL_ERRORS=$((TOTAL_ERRORS + failures))

    if [ $failures -eq 0 ]; then
        log "Test 2 PASSED"
    else
        warn "Test 2: $failures process failures"
    fi

    run_sql_quiet "DROP TABLE conc_docs CASCADE;"
}

#
# Test 3: Multiple indexes with interleaved operations (TIME-BASED)
#
# Target: Operate on multiple indexes simultaneously to test resource isolation
# Duration: 15% of total time
# Expected: 3 indexes, ~2000 docs each, 10+ segments each
#
test_multiple_indexes() {
    local duration=$TEST3_DURATION
    log "Test 3: Multiple indexes with interleaved operations (${duration} seconds)"

    # Create 3 tables with indexes
    for t in 1 2 3; do
        run_sql_quiet "CREATE TABLE multi_${t} (id SERIAL PRIMARY KEY, content TEXT);"
        run_sql_quiet "CREATE INDEX multi_idx_${t} ON multi_${t} USING bm25(content) WITH (text_config='english');"
    done

    local start_time=$(date +%s)
    local end_time=$((start_time + duration))
    local iteration=0
    local errors=0
    local docs_per_table=0

    while [ $(date +%s) -lt $end_time ]; do
        iteration=$((iteration + 1))

        # Insert batch to all tables
        for t in 1 2 3; do
            local insert_sql="INSERT INTO multi_${t} (content) VALUES"
            local first=true
            for ((i=1; i<=20; i++)); do
                local content="iteration ${iteration} table ${t} doc ${i} alpha beta gamma database search"
                if [ "$first" = true ]; then
                    insert_sql="${insert_sql} ('${content}')"
                    first=false
                else
                    insert_sql="${insert_sql}, ('${content}')"
                fi
            done
            run_sql_quiet "${insert_sql};"
        done
        docs_per_table=$((docs_per_table + 20))

        # Query from all tables
        for t in 1 2 3; do
            local count=$(run_sql_value "
                SELECT COUNT(*) FROM (
                    SELECT * FROM multi_${t}
                    ORDER BY content <@> to_bm25query('alpha', 'multi_idx_${t}')
                    LIMIT 100
                ) sub;")
            TOTAL_QUERIES_EXECUTED=$((TOTAL_QUERIES_EXECUTED + 1))
            if [ -z "$count" ] || [ "$count" -eq 0 ]; then
                errors=$((errors + 1))
            fi
        done

        # Rotating spills across indexes
        local spill_target=$(( (iteration % 3) + 1 ))
        if [ $((iteration % 10)) -eq 0 ]; then
            run_sql_quiet "SELECT bm25_spill_index('multi_idx_${spill_target}');"
        fi

        sleep 0.02
    done

    # Final validation
    local total_segments=0
    for t in 1 2 3; do
        local count=$(run_sql_value "SELECT COUNT(*) FROM multi_${t};")
        local segments=$(get_segment_count "multi_idx_${t}")
        total_segments=$((total_segments + segments))
        info "Table $t: ${count} docs, ${segments} segments"

        if [ "$count" != "$docs_per_table" ]; then
            warn "Table $t count mismatch: expected $docs_per_table, got $count"
            errors=$((errors + 1))
        fi
    done

    TOTAL_DOCS_INSERTED=$((TOTAL_DOCS_INSERTED + docs_per_table * 3))
    TOTAL_SEGMENTS_CREATED=$((TOTAL_SEGMENTS_CREATED + total_segments))
    TOTAL_ERRORS=$((TOTAL_ERRORS + errors))

    if [ $errors -eq 0 ]; then
        log "Test 3 PASSED"
    else
        warn "Test 3 completed with $errors errors"
    fi

    for t in 1 2 3; do
        run_sql_quiet "DROP TABLE multi_${t} CASCADE;"
    done
}

#
# Test 4: Segment creation stress test (TIME-BASED)
#
# Target: Create as many segments as possible to test segment handling
# Duration: 10% of total time
# Expected: 50+ segments with small batches and frequent spills
#
test_segment_stress() {
    local duration=$TEST4_DURATION
    log "Test 4: Segment creation stress (${duration} seconds)"

    run_sql_quiet "CREATE TABLE segment_test (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "CREATE INDEX segment_idx ON segment_test USING bm25(content) WITH (text_config='english');"

    local start_time=$(date +%s)
    local end_time=$((start_time + duration))
    local segment_num=0
    local total_docs=0
    local docs_per_segment=30  # Small batches = more segments

    while [ $(date +%s) -lt $end_time ]; do
        segment_num=$((segment_num + 1))

        # Insert small batch
        local insert_sql="INSERT INTO segment_test (content) VALUES"
        local first=true
        for ((i=1; i<=docs_per_segment; i++)); do
            local content="segment ${segment_num} document ${i} with unique terms seg${segment_num}word${i}"
            if [ "$first" = true ]; then
                insert_sql="${insert_sql} ('${content}')"
                first=false
            else
                insert_sql="${insert_sql}, ('${content}')"
            fi
        done
        run_sql_quiet "${insert_sql};"
        total_docs=$((total_docs + docs_per_segment))

        # Force spill after each batch to create many segments
        run_sql_quiet "SELECT bm25_spill_index('segment_idx');"

        # Verify searchability periodically
        if [ $((segment_num % 20)) -eq 0 ]; then
            local actual_segments=$(get_segment_count 'segment_idx')
            info "Created ${actual_segments} segments, ${total_docs} docs"

            # Query across all segments
            local search_count=$(run_sql_value "
                SELECT COUNT(*) FROM (
                    SELECT * FROM segment_test
                    ORDER BY content <@> to_bm25query('document', 'segment_idx')
                    LIMIT 1000
                ) sub;")
            TOTAL_QUERIES_EXECUTED=$((TOTAL_QUERIES_EXECUTED + 1))
        fi

        sleep 0.01
    done

    local final_segments=$(get_segment_count 'segment_idx')
    local final_docs=$(run_sql_value "SELECT COUNT(*) FROM segment_test;")

    # Verify all documents are searchable
    local searchable=$(run_sql_value "
        SELECT COUNT(*) FROM (
            SELECT * FROM segment_test
            ORDER BY content <@> to_bm25query('document', 'segment_idx')
            LIMIT 100000
        ) sub;")

    info "Final: ${final_docs} documents across ${final_segments} segments"

    TOTAL_DOCS_INSERTED=$((TOTAL_DOCS_INSERTED + total_docs))
    TOTAL_SEGMENTS_CREATED=$((TOTAL_SEGMENTS_CREATED + final_segments))

    if [ "$searchable" = "$final_docs" ]; then
        log "Test 4 PASSED: All ${final_docs} documents searchable across ${final_segments} segments"
    else
        warn "Test 4: Expected $final_docs searchable, found $searchable"
        TOTAL_ERRORS=$((TOTAL_ERRORS + 1))
    fi

    run_sql_quiet "DROP TABLE segment_test CASCADE;"
}

#
# Test 5: Resource cleanup stress (TIME-BASED)
#
# Target: Repeatedly create and drop indexes to test resource cleanup
# Duration: 10% of total time
# Expected: 20+ create/drop cycles, no resource leaks
#
# Checks for:
# - DSA/shared memory leaks (via reference index DSA size)
# - File descriptor leaks (via /proc or lsof)
#
test_resource_cleanup() {
    local duration=$TEST5_DURATION
    log "Test 5: Resource cleanup stress (${duration} seconds)"

    # Create a reference index to monitor DSA usage
    run_sql_quiet "CREATE TABLE ref_table (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "INSERT INTO ref_table (content) VALUES ('reference document');"
    run_sql_quiet "CREATE INDEX ref_idx ON ref_table USING bm25(content) WITH (text_config='english');"

    # Record baseline DSA size
    local baseline_dsa=$(run_sql_value "
        SELECT (regexp_matches(bm25_summarize_index('ref_idx'), 'DSA total size: ([0-9]+)'))[1]::bigint;" 2>/dev/null || echo "0")
    info "Baseline DSA: ${baseline_dsa} bytes"

    # Get postgres backend PID for FD and lock tracking
    local backend_pid=$(run_sql_value "SELECT pg_backend_pid();")
    local baseline_fds=0
    if [ -d "/proc/${backend_pid}/fd" ]; then
        baseline_fds=$(ls -1 /proc/${backend_pid}/fd 2>/dev/null | wc -l)
        info "Baseline FDs: ${baseline_fds}"
    fi

    # Record baseline advisory locks (should be 0)
    local baseline_advisory_locks=$(run_sql_value "
        SELECT COUNT(*) FROM pg_locks WHERE pid = ${backend_pid} AND locktype = 'advisory';")
    info "Baseline advisory locks: ${baseline_advisory_locks}"

    local start_time=$(date +%s)
    local end_time=$((start_time + duration))
    local cycle=0
    local docs_per_cycle=200

    while [ $(date +%s) -lt $end_time ]; do
        cycle=$((cycle + 1))

        # Create table and index
        run_sql_quiet "CREATE TABLE cleanup_${cycle} (id SERIAL PRIMARY KEY, content TEXT);"
        run_sql_quiet "INSERT INTO cleanup_${cycle} (content)
            SELECT 'document ' || g || ' alpha beta gamma database search query'
            FROM generate_series(1, ${docs_per_cycle}) g;"
        run_sql_quiet "CREATE INDEX cleanup_idx_${cycle} ON cleanup_${cycle} USING bm25(content) WITH (text_config='english');"
        run_sql_quiet "SELECT bm25_spill_index('cleanup_idx_${cycle}');"

        # Query multiple times
        for ((q=1; q<=5; q++)); do
            run_sql_quiet "
                SELECT COUNT(*) FROM (
                    SELECT * FROM cleanup_${cycle}
                    ORDER BY content <@> to_bm25query('alpha', 'cleanup_idx_${cycle}')
                    LIMIT 100
                ) sub;"
            TOTAL_QUERIES_EXECUTED=$((TOTAL_QUERIES_EXECUTED + 1))
        done

        # Drop table (cascades to index)
        run_sql_quiet "DROP TABLE cleanup_${cycle} CASCADE;"

        if [ $((cycle % 10)) -eq 0 ]; then
            info "Completed ${cycle} create/drop cycles"
        fi

        sleep 0.02
    done

    TOTAL_DOCS_INSERTED=$((TOTAL_DOCS_INSERTED + cycle * docs_per_cycle))

    # Check for DSA leaks - reference index DSA should not have grown significantly
    local final_dsa=$(run_sql_value "
        SELECT (regexp_matches(bm25_summarize_index('ref_idx'), 'DSA total size: ([0-9]+)'))[1]::bigint;" 2>/dev/null || echo "0")
    local dsa_growth=0
    if [ "$baseline_dsa" -gt 0 ] && [ "$final_dsa" -gt 0 ]; then
        dsa_growth=$(( (final_dsa - baseline_dsa) * 100 / baseline_dsa ))
    fi
    info "Final DSA: ${final_dsa} bytes (${dsa_growth}% growth)"

    # Check for FD leaks
    local final_fds=0
    if [ -d "/proc/${backend_pid}/fd" ]; then
        final_fds=$(ls -1 /proc/${backend_pid}/fd 2>/dev/null | wc -l)
        local fd_growth=$((final_fds - baseline_fds))
        info "Final FDs: ${final_fds} (${fd_growth} growth)"
        if [ $fd_growth -gt 10 ]; then
            warn "Possible FD leak: ${fd_growth} new file descriptors"
            TOTAL_ERRORS=$((TOTAL_ERRORS + 1))
        fi
    fi

    # DSA growth > 50% after cleanup would indicate a leak
    if [ $dsa_growth -gt 50 ]; then
        warn "Possible DSA leak: ${dsa_growth}% growth in reference index"
        TOTAL_ERRORS=$((TOTAL_ERRORS + 1))
    fi

    # Check for advisory lock leaks
    local final_advisory_locks=$(run_sql_value "
        SELECT COUNT(*) FROM pg_locks WHERE pid = ${backend_pid} AND locktype = 'advisory';")
    local lock_growth=$((final_advisory_locks - baseline_advisory_locks))
    info "Final advisory locks: ${final_advisory_locks} (${lock_growth} growth)"
    if [ $lock_growth -gt 0 ]; then
        warn "Possible lock leak: ${lock_growth} advisory locks not released"
        TOTAL_ERRORS=$((TOTAL_ERRORS + 1))
    fi

    run_sql_quiet "DROP TABLE ref_table CASCADE;"

    log "Test 5 PASSED: Completed ${cycle} create/drop cycles"
}

print_summary() {
    echo ""
    echo "============================================================"
    echo "                    STRESS TEST SUMMARY"
    echo "============================================================"
    echo ""
    echo "Configuration:"
    echo "  Duration:            ${STRESS_DURATION_MINUTES} minutes"
    echo "  Spill threshold:     ${SPILL_THRESHOLD} postings"
    echo "  Docs per batch:      ${DOCS_PER_BATCH}"
    echo "  Concurrent readers:  ${CONCURRENT_READERS}"
    echo "  Concurrent writers:  ${CONCURRENT_WRITERS}"
    echo ""
    echo "Results:"
    echo "  Total documents:     ${TOTAL_DOCS_INSERTED}"
    echo "  Total segments:      ${TOTAL_SEGMENTS_CREATED}"
    echo "  Total queries:       ${TOTAL_QUERIES_EXECUTED}"
    echo "  Total errors:        ${TOTAL_ERRORS}"
    echo ""
    echo "Time allocation:"
    echo "  Test 1 (insert/query):    ${TEST1_DURATION}s (40%)"
    echo "  Test 2 (concurrent):      ${TEST2_DURATION}s (25%)"
    echo "  Test 3 (multi-index):     ${TEST3_DURATION}s (15%)"
    echo "  Test 4 (segment stress):  ${TEST4_DURATION}s (10%)"
    echo "  Test 5 (cleanup):         ${TEST5_DURATION}s (10%)"
    echo ""
    if [ $TOTAL_ERRORS -eq 0 ]; then
        echo -e "${GREEN}STATUS: ALL TESTS PASSED${NC}"
    else
        echo -e "${YELLOW}STATUS: COMPLETED WITH ${TOTAL_ERRORS} ERRORS${NC}"
    fi
    echo "============================================================"
}

run_stress_tests() {
    log "Starting pg_textsearch stress tests"

    test_extended_insert_query
    test_concurrent_stress
    test_multiple_indexes
    test_segment_stress
    test_resource_cleanup

    print_summary
}

main() {
    log "pg_textsearch stress test suite"
    echo ""
    echo "Configuration:"
    echo "  Duration:           ${STRESS_DURATION_MINUTES} minutes (${TOTAL_SECONDS} seconds)"
    echo "  Spill threshold:    ${SPILL_THRESHOLD} postings"
    echo "  Docs per batch:     ${DOCS_PER_BATCH}"
    echo "  Concurrent readers: ${CONCURRENT_READERS}"
    echo "  Concurrent writers: ${CONCURRENT_WRITERS}"
    echo ""

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"

    setup_test_db
    run_stress_tests

    if [ $TOTAL_ERRORS -eq 0 ]; then
        log "Stress tests completed successfully!"
        exit 0
    else
        warn "Stress tests completed with ${TOTAL_ERRORS} errors"
        exit 1
    fi
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
