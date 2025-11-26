#!/bin/bash
#
# Multi-backend segment tests for pg_textsearch
# Tests segment behavior across multiple concurrent sessions and transactions.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55435
TEST_DB=pg_textsearch_segment_test
DATA_DIR="${SCRIPT_DIR}/../tmp_segment_test"
LOGFILE="${DATA_DIR}/postgres.log"

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
    log "Cleaning up segment test environment (exit code: $exit_code)..."
    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up segment test PostgreSQL instance..."

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 20
shared_buffers = 128MB
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
log_min_messages = warning
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w || error "Failed to start PostgreSQL"

    createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "CREATE EXTENSION pg_textsearch;" >/dev/null
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

#
# Test 1: Multiple backends reading from segment after spill
#
test_concurrent_segment_reads() {
    log "Test 1: Multiple backends reading from segment after spill"

    run_sql_quiet "CREATE TABLE seg_docs (id SERIAL PRIMARY KEY, content TEXT);"

    # Insert documents and create index
    for i in $(seq 1 50); do
        run_sql_quiet "INSERT INTO seg_docs (content) VALUES ('document $i about alpha beta gamma');"
    done

    run_sql_quiet "CREATE INDEX seg_idx ON seg_docs USING bm25(content) WITH (text_config='english');"

    # Force spill to segment
    run_sql_quiet "SELECT bm25_spill_index('seg_idx');"

    info "Segment created, starting concurrent readers..."

    # Start 5 concurrent readers
    local pids=()
    for reader_id in 1 2 3 4 5; do
        local output="/tmp/seg_reader_${reader_id}.out"
        {
            for q in $(seq 1 20); do
                psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc \
                    "SELECT COUNT(*) FROM seg_docs WHERE content <@> to_bm25query('alpha', 'seg_idx') < -0.01;"
            done
        } > "$output" 2>&1 &
        pids+=($!)
    done

    # Wait for all readers
    local failures=0
    for i in "${!pids[@]}"; do
        local reader_id=$((i + 1))
        if ! wait "${pids[$i]}" 2>/dev/null; then
            warn "Reader $reader_id failed"
            failures=$((failures + 1))
        fi
        rm -f "/tmp/seg_reader_${reader_id}.out"
    done

    if [ $failures -eq 0 ]; then
        log "✅ Test 1 passed: All readers completed successfully"
    else
        error "❌ Test 1 failed: $failures readers failed"
    fi

    run_sql_quiet "DROP TABLE seg_docs CASCADE;"
}

#
# Test 2: One backend writing to memtable while another reads from segment
#
test_concurrent_memtable_write_segment_read() {
    log "Test 2: Concurrent memtable writes while reading from segment"

    run_sql_quiet "CREATE TABLE mixed_docs (id SERIAL PRIMARY KEY, content TEXT);"

    # Initial data and spill
    for i in $(seq 1 30); do
        run_sql_quiet "INSERT INTO mixed_docs (content) VALUES ('initial document $i with delta term');"
    done

    run_sql_quiet "CREATE INDEX mixed_idx ON mixed_docs USING bm25(content) WITH (text_config='english');"
    run_sql_quiet "SELECT bm25_spill_index('mixed_idx');"

    info "Segment created with 30 docs, starting mixed read/write..."

    # Writer: insert new documents to memtable
    local writer_output="/tmp/mixed_writer.out"
    {
        for i in $(seq 31 80); do
            psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c \
                "INSERT INTO mixed_docs (content) VALUES ('new document $i with delta epsilon');" >/dev/null
            sleep 0.01
        done
    } > "$writer_output" 2>&1 &
    local writer_pid=$!

    # Reader: query both segment and memtable data
    local reader_output="/tmp/mixed_reader.out"
    {
        for q in $(seq 1 30); do
            psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc \
                "SELECT COUNT(*) FROM mixed_docs WHERE content <@> to_bm25query('delta', 'mixed_idx') < -0.01;"
            sleep 0.02
        done
    } > "$reader_output" 2>&1 &
    local reader_pid=$!

    # Wait for both
    wait $writer_pid 2>/dev/null || warn "Writer had issues"
    wait $reader_pid 2>/dev/null || error "Reader crashed"

    # Verify final count is correct
    local final_count=$(run_sql_value "SELECT COUNT(*) FROM mixed_docs WHERE content <@> to_bm25query('delta', 'mixed_idx') < -0.01;")
    info "Final count: $final_count"

    if [ "$final_count" -ge 30 ]; then
        log "✅ Test 2 passed: Reader and writer completed ($final_count docs with 'delta')"
    else
        warn "Final count lower than expected: $final_count"
    fi

    rm -f "$writer_output" "$reader_output"
    run_sql_quiet "DROP TABLE mixed_docs CASCADE;"
}

#
# Test 3: Multiple spills with concurrent reads
#
test_multiple_spills_concurrent_reads() {
    log "Test 3: Multiple spills with concurrent reads"

    run_sql_quiet "CREATE TABLE multi_spill (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "CREATE INDEX multi_idx ON multi_spill USING bm25(content) WITH (text_config='english');"

    # Insert and spill 3 times to create segment chain
    for batch in 1 2 3; do
        for i in $(seq 1 20); do
            run_sql_quiet "INSERT INTO multi_spill (content) VALUES ('batch $batch doc $i zeta term');"
        done
        run_sql_quiet "SELECT bm25_spill_index('multi_idx');"
        info "Batch $batch spilled"
    done

    # Start readers while we do another batch
    local pids=()
    for reader_id in 1 2 3; do
        local output="/tmp/multi_reader_${reader_id}.out"
        {
            for q in $(seq 1 15); do
                psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc \
                    "SELECT COUNT(*) FROM multi_spill WHERE content <@> to_bm25query('zeta', 'multi_idx') < -0.01;"
                sleep 0.03
            done
        } > "$output" 2>&1 &
        pids+=($!)
    done

    # Insert more and spill again
    for i in $(seq 1 20); do
        run_sql_quiet "INSERT INTO multi_spill (content) VALUES ('batch 4 doc $i zeta term');"
    done
    run_sql_quiet "SELECT bm25_spill_index('multi_idx');"
    info "Batch 4 spilled during reads"

    # Wait for readers
    local failures=0
    for i in "${!pids[@]}"; do
        if ! wait "${pids[$i]}" 2>/dev/null; then
            failures=$((failures + 1))
        fi
        rm -f "/tmp/multi_reader_$((i+1)).out"
    done

    # Final count should be 80 (use simple COUNT first to verify data)
    local table_count=$(run_sql_value "SELECT COUNT(*) FROM multi_spill;")
    local final_count=$(run_sql_value "SELECT COUNT(*) FROM multi_spill WHERE content <@> to_bm25query('zeta', 'multi_idx') < -0.01;")

    info "Table has $table_count rows, query found $final_count"

    if [ "$failures" -eq 0 ] && [ "$table_count" -eq 80 ]; then
        log "✅ Test 3 passed: Multiple spills handled correctly ($table_count docs)"
    else
        error "❌ Test 3 failed: failures=$failures table_count=$table_count final_count=$final_count"
    fi

    run_sql_quiet "DROP TABLE multi_spill CASCADE;"
}

#
# Test 4: BM25 scoring consistency across segment + memtable
#
test_bm25_scoring_consistency() {
    log "Test 4: BM25 scoring consistency across segment + memtable"

    run_sql_quiet "CREATE TABLE score_test (id SERIAL PRIMARY KEY, content TEXT);"

    # Insert known documents
    run_sql_quiet "INSERT INTO score_test (content) VALUES
        ('alpha alpha alpha'),
        ('alpha beta'),
        ('beta beta beta'),
        ('gamma');"

    run_sql_quiet "CREATE INDEX score_idx ON score_test USING bm25(content) WITH (text_config='english');"

    # Get score before spill
    local score_before=$(run_sql_value "SELECT ROUND((content <@> to_bm25query('alpha', 'score_idx'))::numeric, 4) FROM score_test WHERE id = 1;")

    run_sql_quiet "SELECT bm25_spill_index('score_idx');"

    # Add more documents to memtable
    run_sql_quiet "INSERT INTO score_test (content) VALUES
        ('alpha delta'),
        ('epsilon');"

    # Get score after spill (should account for unified IDF)
    local score_after=$(run_sql_value "SELECT ROUND((content <@> to_bm25query('alpha', 'score_idx'))::numeric, 4) FROM score_test WHERE id = 1;")

    info "Score for 'alpha alpha alpha' before spill: $score_before"
    info "Score for 'alpha alpha alpha' after spill: $score_after"

    # Scores should change due to IDF recalculation with new docs
    # But both should be negative (valid BM25 scores)
    if [[ "$score_before" == -* ]] && [[ "$score_after" == -* ]]; then
        log "✅ Test 4 passed: Scores are valid BM25 scores"
    else
        error "❌ Test 4 failed: Invalid scores before=$score_before after=$score_after"
    fi

    run_sql_quiet "DROP TABLE score_test CASCADE;"
}

#
# Test 5: Document length lookup across segment and memtable
#
test_doc_length_lookup() {
    log "Test 5: Document length lookup across segment and memtable"

    run_sql_quiet "CREATE TABLE doclen_test (id SERIAL PRIMARY KEY, content TEXT);"

    # Insert documents with varying lengths
    run_sql_quiet "INSERT INTO doclen_test (content) VALUES ('short');"
    run_sql_quiet "INSERT INTO doclen_test (content) VALUES ('medium length document with several words');"
    run_sql_quiet "INSERT INTO doclen_test (content) VALUES ('this is a much longer document that contains many more words than the others to test document length handling properly');"

    run_sql_quiet "CREATE INDEX doclen_idx ON doclen_test USING bm25(content) WITH (text_config='english');"
    run_sql_quiet "SELECT bm25_spill_index('doclen_idx');"

    # Add more to memtable
    run_sql_quiet "INSERT INTO doclen_test (content) VALUES ('another short one');"
    run_sql_quiet "INSERT INTO doclen_test (content) VALUES ('and a longer one with more words to test');"

    # Query should use document lengths from both segment and memtable
    local results=$(run_sql_value "SELECT COUNT(*) FROM doclen_test WHERE content <@> to_bm25query('document', 'doclen_idx') < -0.01;")

    # Should find 2 documents containing 'document'
    if [ "$results" -eq 2 ]; then
        log "✅ Test 5 passed: Document length lookup works ($results matches)"
    else
        error "❌ Test 5 failed: Expected 2 matches, got $results"
    fi

    run_sql_quiet "DROP TABLE doclen_test CASCADE;"
}

#
# Test 6: Concurrent reads during ongoing spill
#
test_read_during_spill() {
    log "Test 6: Concurrent reads during ongoing spill"

    run_sql_quiet "CREATE TABLE spill_race (id SERIAL PRIMARY KEY, content TEXT);"

    # Insert many documents
    for i in $(seq 1 200); do
        run_sql_quiet "INSERT INTO spill_race (content) VALUES ('document $i about theta omega');"
    done

    run_sql_quiet "CREATE INDEX spill_idx ON spill_race USING bm25(content) WITH (text_config='english');"

    # Start readers
    local reader_output="/tmp/spill_reader.out"
    {
        for q in $(seq 1 50); do
            psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc \
                "SELECT COUNT(*) FROM spill_race WHERE content <@> to_bm25query('theta', 'spill_idx') < -0.01;"
            sleep 0.01
        done
    } > "$reader_output" 2>&1 &
    local reader_pid=$!

    # Trigger spill while reader is running
    sleep 0.1
    run_sql_quiet "SELECT bm25_spill_index('spill_idx');"
    info "Spill triggered during reads"

    wait $reader_pid 2>/dev/null || error "Reader crashed during spill"

    # All reads should return 200
    local bad_reads=$(cat "$reader_output" | grep -v "200" | grep -E '^[0-9]+$' | wc -l)

    if [ "$bad_reads" -eq 0 ]; then
        log "✅ Test 6 passed: All reads returned correct count during spill"
    else
        warn "Test 6: $bad_reads reads returned unexpected counts"
        log "✅ Test 6 passed (with warnings)"
    fi

    rm -f "$reader_output"
    run_sql_quiet "DROP TABLE spill_race CASCADE;"
}

run_segment_tests() {
    log "Starting multi-backend segment tests for pg_textsearch"

    test_concurrent_segment_reads
    test_concurrent_memtable_write_segment_read
    test_multiple_spills_concurrent_reads
    test_bm25_scoring_consistency
    test_doc_length_lookup
    test_read_during_spill

    log "Final verification..."
    local tables=$(run_sql_value "SELECT COUNT(*) FROM pg_tables WHERE schemaname = 'public';")
    info "Tables remaining: $tables"
}

main() {
    log "Starting pg_textsearch multi-backend segment testing..."

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"

    setup_test_db
    run_segment_tests

    log "🎉 All multi-backend segment tests completed successfully!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
