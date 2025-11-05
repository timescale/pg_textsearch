#!/bin/bash

# Large-scale benchmark for pg_textsearch with real Wikipedia data
# Tests with datasets from 10K to 100K+ articles

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

echo "=================================================="
echo "  Large-Scale Wikipedia Benchmark"
echo "  for pg_textsearch (Tapir)"
echo "=================================================="
echo ""

# Check what data files are available
echo "Available Wikipedia datasets:"
ls -lh "$DATA_DIR"/*.tsv 2>/dev/null | awk '{print "  " $NF " (" $5 ")"}'
echo ""

# Select dataset
if [ -z "$1" ]; then
    echo "Usage: $0 <data_file.tsv>"
    echo ""
    echo "Available files:"
    ls "$DATA_DIR"/*.tsv 2>/dev/null | xargs -n1 basename
    exit 1
fi

DATA_FILE="$1"
if [ ! -f "$DATA_FILE" ]; then
    DATA_FILE="$DATA_DIR/$1"
    if [ ! -f "$DATA_FILE" ]; then
        log_error "File not found: $1"
        exit 1
    fi
fi

# Count articles
ARTICLE_COUNT=$(wc -l < "$DATA_FILE")
FILE_SIZE=$(du -sh "$DATA_FILE" | cut -f1)

log_info "Dataset: $(basename $DATA_FILE)"
log_info "Articles: $ARTICLE_COUNT"
log_info "File size: $FILE_SIZE"
echo ""

# PostgreSQL setup
psql -q -c "CREATE EXTENSION IF NOT EXISTS pg_textsearch;"

# Show configuration
echo "Configuration:"
psql -t -c "SELECT 'Memory limit: ' || COALESCE(current_setting('pg_textsearch.index_memory_limit', true), '16') || 'MB'" 2>/dev/null
psql -t -c "SELECT 'Shared buffers: ' || current_setting('shared_buffers')"
psql -t -c "SELECT 'Work mem: ' || current_setting('work_mem')"
echo ""

# Phase 1: Data Loading
echo "=================================================="
echo "Phase 1: Data Loading"
echo "=================================================="

log_info "Creating table and loading $ARTICLE_COUNT articles..."
start_time=$(date +%s)

psql -q << EOF
DROP TABLE IF EXISTS wikipedia_scale CASCADE;
CREATE TABLE wikipedia_scale (
    id SERIAL PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL,
    doc_length INTEGER
);

-- Load data
\copy wikipedia_scale (title, content) FROM '$DATA_FILE' DELIMITER E'\t'

-- Calculate lengths
UPDATE wikipedia_scale SET doc_length = array_length(string_to_array(content, ' '), 1);
ANALYZE wikipedia_scale;
EOF

end_time=$(date +%s)
LOAD_TIME=$((end_time - start_time))

# Get statistics
TABLE_SIZE=$(psql -t -c "SELECT pg_size_pretty(pg_total_relation_size('wikipedia_scale'))")
AVG_LENGTH=$(psql -t -c "SELECT ROUND(AVG(doc_length)) FROM wikipedia_scale")

log_info "Data loaded in ${LOAD_TIME} seconds"
log_info "Table size: $TABLE_SIZE"
log_info "Average document: $AVG_LENGTH words"
echo ""

# Phase 2: Index Creation
echo "=================================================="
echo "Phase 2: BM25 Index Creation"
echo "=================================================="

log_info "Creating BM25 index..."
log_info "This may take several minutes for large datasets..."

start_time=$(date +%s)

psql << EOF
CREATE INDEX idx_wikipedia_scale_bm25
ON wikipedia_scale
USING bm25 (content)
WITH (text_config = 'english');
EOF

end_time=$(date +%s)
INDEX_TIME=$((end_time - start_time))

INDEX_SIZE=$(psql -t -c "SELECT pg_size_pretty(pg_relation_size('idx_wikipedia_scale_bm25'))")

log_info "Index created in ${INDEX_TIME} seconds"

# Get DSA memory usage (actual index) and on-disk size (metadata)
DSA_OUTPUT=$(psql -t -c "SELECT bm25_debug_dump_index('idx_wikipedia_scale_bm25')" 2>/dev/null | grep "DSA total size:")
DSA_SIZE_MB=$(echo "$DSA_OUTPUT" | sed 's/.*(\(.*MB\)).*/\1/')
if [ -n "$DSA_SIZE_MB" ]; then
    DSA_SIZE="$DSA_SIZE_MB"
else
    DSA_SIZE_BYTES=$(echo "$DSA_OUTPUT" | sed 's/.*DSA total size: \([0-9]*\) bytes.*/\1/')
    DSA_SIZE=$(psql -t -c "SELECT pg_size_pretty(${DSA_SIZE_BYTES}::bigint)" 2>/dev/null || echo "$DSA_SIZE_BYTES bytes")
fi

log_info "DSA memory (actual index): $DSA_SIZE"
log_info "On-disk size (metadata): $INDEX_SIZE"

if [ $INDEX_TIME -gt 0 ]; then
    THROUGHPUT=$((ARTICLE_COUNT / INDEX_TIME))
    log_info "Throughput: ${THROUGHPUT} docs/sec"
fi
echo ""

# Phase 3: Query Performance
echo "=================================================="
echo "Phase 3: Query Performance Tests"
echo "=================================================="

psql << 'EOF'
\timing on

-- Warmup query
SELECT COUNT(*) FROM wikipedia_scale WHERE content @@ to_tsquery('english', 'wikipedia');

\echo ''
\echo 'Test 1: Single-term queries'
\echo '----------------------------'

SELECT 'science' as term, COUNT(*) as matches
FROM wikipedia_scale WHERE content @@ to_tsquery('english', 'science');

SELECT 'technology' as term, COUNT(*) as matches
FROM wikipedia_scale WHERE content @@ to_tsquery('english', 'technology');

SELECT 'history' as term, COUNT(*) as matches
FROM wikipedia_scale WHERE content @@ to_tsquery('english', 'history');

\echo ''
\echo 'Test 2: Multi-term queries'
\echo '-------------------------'

SELECT 'computer science' as terms, COUNT(*) as matches
FROM wikipedia_scale WHERE content @@ to_tsquery('english', 'computer & science');

SELECT 'machine learning' as terms, COUNT(*) as matches
FROM wikipedia_scale WHERE content @@ to_tsquery('english', 'machine & learning');

SELECT 'united states' as terms, COUNT(*) as matches
FROM wikipedia_scale WHERE content @@ to_tsquery('english', 'united & states');

\echo ''
\echo 'Test 3: BM25 Ranking (top 5 for "artificial intelligence")'
\echo '----------------------------------------------------------'

SELECT id,
       title,
       LEFT(content, 60) || '...' as content_preview,
       ROUND(CAST(content <@> to_bm25query('artificial intelligence', 'idx_wikipedia_scale_bm25') AS numeric), 4) as score
FROM wikipedia_scale
WHERE content @@ to_tsquery('english', 'artificial & intelligence')
ORDER BY content <@> to_bm25query('artificial intelligence', 'idx_wikipedia_scale_bm25')
LIMIT 5;

\echo ''
\echo 'Test 4: Complex queries'
\echo '----------------------'

WITH query_results AS (
    SELECT
        'data AND structure AND algorithm' as query,
        COUNT(*) as matches,
        AVG(CAST(content <@> to_bm25query('data structure algorithm', 'idx_wikipedia_scale_bm25') AS numeric)) as avg_score
    FROM wikipedia_scale
    WHERE content @@ to_tsquery('english', 'data & structure & algorithm')
)
SELECT query, matches, ROUND(avg_score::numeric, 4) as avg_score FROM query_results;

\timing off
EOF

echo ""

# Phase 4: Concurrent Performance
echo "=================================================="
echo "Phase 4: Concurrent Query Test"
echo "=================================================="

log_info "Running 10 concurrent queries..."

start_time=$(date +%s)

for i in {1..10}; do
    psql -c "SELECT COUNT(*) FROM wikipedia_scale WHERE content @@ to_tsquery('english', 'research')" > /dev/null 2>&1 &
done

wait

end_time=$(date +%s)
CONCURRENT_TIME=$((end_time - start_time))

log_info "Concurrent queries completed in ${CONCURRENT_TIME} seconds"
echo ""

# Summary
echo "=================================================="
echo "                BENCHMARK SUMMARY"
echo "=================================================="
echo "Dataset:             $ARTICLE_COUNT articles"
echo "File size:           $FILE_SIZE"
echo "Table size:          $TABLE_SIZE"
echo "DSA memory:          $DSA_SIZE (actual inverted index)"
echo "On-disk metadata:    $INDEX_SIZE (recovery pages)"
echo ""
echo "Performance Metrics:"
echo "  Data loading:      ${LOAD_TIME}s"
echo "  Index creation:    ${INDEX_TIME}s"
if [ $INDEX_TIME -gt 0 ]; then
    echo "  Index throughput:  ${THROUGHPUT} docs/sec"
    echo "  Time per doc:      $((INDEX_TIME * 1000 / ARTICLE_COUNT))ms"
fi
echo "  Concurrent test:   ${CONCURRENT_TIME}s (10 queries)"
echo ""
echo "=================================================="
echo ""

# Cleanup
read -p "Keep test data? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    psql -q -c "DROP TABLE wikipedia_scale CASCADE;"
    log_info "Table dropped."
else
    log_info "Table 'wikipedia_scale' preserved for further testing."
fi
