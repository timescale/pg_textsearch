#!/bin/bash

# Wikipedia Real Data Benchmark for pg_textsearch
# Uses actual Wikipedia articles fetched via API

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCALE="${1:-small}"

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

echo "=================================================="
echo "  Real Wikipedia Data Benchmark"
echo "  for pg_textsearch (Tapir)"
echo "=================================================="
echo ""

# Determine dataset size
case "$SCALE" in
    "micro")
        DOC_COUNT=100
        DESC="100 articles (quick test)"
        ;;
    "small")
        DOC_COUNT=1000
        DESC="1K articles"
        ;;
    "medium")
        DOC_COUNT=10000
        DESC="10K articles"
        ;;
    "large")
        DOC_COUNT=100000
        DESC="100K articles (will take time)"
        ;;
    *)
        echo "Usage: $0 [micro|small|medium|large]"
        exit 1
        ;;
esac

log_info "Scale: $SCALE - $DESC"

# Create data directory
DATA_DIR="$SCRIPT_DIR/data/wikipedia_real"
mkdir -p "$DATA_DIR"

DATA_FILE="$DATA_DIR/wikipedia_${SCALE}.tsv"

# Check extension
psql -q -c "CREATE EXTENSION IF NOT EXISTS pg_textsearch;"

# Show configuration
echo ""
echo "Configuration:"
psql -t -c "SELECT 'Memory limit: ' || COALESCE(current_setting('pg_textsearch.index_memory_limit', true), '16') || 'MB'" 2>/dev/null
echo ""

# Phase 1: Fetch Wikipedia data (if not cached)
echo "Phase 1: Wikipedia Data"
echo "-----------------------"

if [ -f "$DATA_FILE" ]; then
    log_info "Using cached Wikipedia data: $DATA_FILE"
    ARTICLE_COUNT=$(wc -l < "$DATA_FILE")
    log_info "Found $ARTICLE_COUNT cached articles"
else
    log_info "Fetching $DOC_COUNT real Wikipedia articles..."
    log_info "This may take a few minutes due to API rate limits..."

    python3 "$SCRIPT_DIR/fetch_wikipedia_simple.py" $DOC_COUNT "$DATA_FILE"

    if [ ! -f "$DATA_FILE" ] || [ ! -s "$DATA_FILE" ]; then
        log_error "Failed to fetch Wikipedia data"
        exit 1
    fi

    ARTICLE_COUNT=$(wc -l < "$DATA_FILE")
    log_info "Successfully fetched $ARTICLE_COUNT articles"
fi

# Calculate data size
DATA_SIZE=$(du -sh "$DATA_FILE" | cut -f1)
log_info "Data file size: $DATA_SIZE"
echo ""

# Phase 2: Load data into PostgreSQL
echo "Phase 2: Data Loading"
echo "--------------------"
echo -n "Creating table and loading articles... "

start_time=$(date +%s)

psql -q << EOF
DROP TABLE IF EXISTS wikipedia_real CASCADE;
CREATE TABLE wikipedia_real (
    id SERIAL PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL,
    doc_length INTEGER
);
EOF

# Load data with COPY
psql -q -c "COPY wikipedia_real (title, content) FROM '$DATA_FILE' DELIMITER E'\t'"

# Update document lengths
psql -q -c "UPDATE wikipedia_real SET doc_length = array_length(string_to_array(content, ' '), 1);"
psql -q -c "ANALYZE wikipedia_real;"

end_time=$(date +%s)
LOAD_TIME=$((end_time - start_time))
echo "done (${LOAD_TIME}s)"

# Get table info
TABLE_SIZE=$(psql -t -c "SELECT pg_size_pretty(pg_total_relation_size('wikipedia_real'))")
AVG_DOC_LENGTH=$(psql -t -c "SELECT ROUND(AVG(doc_length)) FROM wikipedia_real")
MAX_DOC_LENGTH=$(psql -t -c "SELECT MAX(doc_length) FROM wikipedia_real")

echo "Table size: $TABLE_SIZE"
echo "Avg document: $AVG_DOC_LENGTH words, Max: $MAX_DOC_LENGTH words"
echo ""

# Phase 3: Create BM25 index
echo "Phase 3: BM25 Index Creation"
echo "---------------------------"
echo -n "Creating BM25 index... "

start_time=$(date +%s)

psql << EOF
CREATE INDEX idx_wikipedia_real_bm25
ON wikipedia_real
USING bm25 (content)
WITH (text_config = 'english');
EOF

end_time=$(date +%s)
INDEX_TIME=$((end_time - start_time))
echo "done (${INDEX_TIME}s)"

# Get both DSA memory usage (actual index) and on-disk size (metadata)
# Extract DSA size in bytes and MB from debug output
DSA_OUTPUT=$(psql -t -c "SELECT bm25_debug_dump_index('idx_wikipedia_real_bm25')" 2>/dev/null | grep "DSA total size:")
DSA_SIZE_BYTES=$(echo "$DSA_OUTPUT" | sed 's/.*DSA total size: \([0-9]*\) bytes.*/\1/')
DSA_SIZE_MB=$(echo "$DSA_OUTPUT" | sed 's/.*(\(.*MB\)).*/\1/')

# Format DSA size nicely
if [ -n "$DSA_SIZE_MB" ]; then
    DSA_SIZE="$DSA_SIZE_MB"
else
    # Fallback to manual calculation if extraction fails
    DSA_SIZE=$(psql -t -c "SELECT pg_size_pretty(${DSA_SIZE_BYTES}::bigint)" 2>/dev/null || echo "$DSA_SIZE_BYTES bytes")
fi

# Get on-disk size (metadata pages only)
ONDISK_SIZE=$(psql -t -c "SELECT pg_size_pretty(pg_relation_size('idx_wikipedia_real_bm25'))")

echo "DSA memory (actual index): $DSA_SIZE"
echo "On-disk size (metadata):  $ONDISK_SIZE"

if [ $INDEX_TIME -gt 0 ]; then
    THROUGHPUT=$((ARTICLE_COUNT / INDEX_TIME))
    echo "Throughput: ~${THROUGHPUT} docs/sec"
fi
echo ""

# Phase 4: Query Tests
echo "Phase 4: Query Performance"
echo "-------------------------"

# Test queries that are likely to appear in Wikipedia
psql << 'EOF'
\timing on

-- Single word queries
\echo 'Query 1: "history"'
SELECT COUNT(*) as matches FROM wikipedia_real
WHERE content @@ to_tsquery('english', 'history');

\echo ''
\echo 'Query 2: "science"'
WITH results AS (
    SELECT id, title,
           ts_rank(to_tsvector('english', content),
                   to_tsquery('english', 'science')) as rank
    FROM wikipedia_real
    WHERE content @@ to_tsquery('english', 'science')
)
SELECT COUNT(*) as matches, ROUND(AVG(rank)::numeric, 4) as avg_rank
FROM results;

-- Multi-word queries
\echo ''
\echo 'Query 3: "united states"'
SELECT COUNT(*) as matches FROM wikipedia_real
WHERE content @@ to_tsquery('english', 'united & states');

\echo ''
\echo 'Query 4: Top results for "computer science"'
SELECT id, title,
       ROUND(ts_rank(to_tsvector('english', content),
                     to_tsquery('english', 'computer & science'))::numeric, 4) as rank
FROM wikipedia_real
WHERE content @@ to_tsquery('english', 'computer & science')
ORDER BY rank DESC
LIMIT 5;

-- BM25 scoring test
\echo ''
\echo 'Query 5: BM25 scoring for "wikipedia"'
SELECT id, title,
       ROUND(CAST(content <@> to_bm25query('wikipedia', 'idx_wikipedia_real_bm25') AS numeric), 4) as bm25_score
FROM wikipedia_real
WHERE content @@ to_tsquery('english', 'wikipedia')
ORDER BY content <@> to_bm25query('wikipedia', 'idx_wikipedia_real_bm25')
LIMIT 5;

\timing off
EOF

echo ""

# Phase 5: Sample content display
echo "Phase 5: Sample Articles"
echo "-----------------------"

psql -q << EOF
SELECT title, LEFT(content, 100) || '...' as content_preview
FROM wikipedia_real
ORDER BY RANDOM()
LIMIT 3;
EOF

echo ""

# Summary
echo "=================================================="
echo "Summary"
echo "=================================================="
echo "Articles:          $ARTICLE_COUNT"
echo "Load time:         ${LOAD_TIME}s"
echo "Index time:        ${INDEX_TIME}s"
echo "Table size:        $TABLE_SIZE"
echo "DSA memory:        $DSA_SIZE (actual inverted index)"
echo "On-disk metadata:  $ONDISK_SIZE (recovery pages)"
echo "Avg doc size:      $AVG_DOC_LENGTH words"
echo ""

# Cleanup option
read -p "Keep Wikipedia data for further testing? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    psql -q -c "DROP TABLE wikipedia_real CASCADE;"
    log_info "Table removed."
    echo "Cached data file kept at: $DATA_FILE"
else
    log_info "Data kept in table 'wikipedia_real'"
    echo "To remove later: DROP TABLE wikipedia_real CASCADE;"
fi
