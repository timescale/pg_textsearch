#!/bin/bash
# Generate benchmark query samples bucketed by token count
#
# This script creates benchmark_queries.tsv with deterministically-sampled
# queries from MS-MARCO, bucketed by the number of search tokens after
# stop word removal and stemming (via to_tsvector).
#
# Output format: query_id<TAB>query_text<TAB>token_count
#
# Usage: ./generate_benchmark_queries.sh
#
# Requires: PostgreSQL running locally

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
OUTPUT_FILE="$SCRIPT_DIR/benchmark_queries.tsv"
INPUT_FILE="$DATA_DIR/queries.dev.tsv"
TEMP_FILE=$(mktemp)
trap "rm -f $TEMP_FILE" EXIT

# Check if queries file exists
if [ ! -f "$INPUT_FILE" ]; then
    echo "Error: $INPUT_FILE not found"
    echo "Run ./download.sh first to get the MS-MARCO dataset"
    exit 1
fi

echo "Generating benchmark queries with 100 queries per token bucket..."
echo "Loading and tokenizing $(wc -l < "$INPUT_FILE") queries..."

# Preprocess: convert to CSV format to handle special characters
echo "Preprocessing queries..."
awk -F'\t' 'BEGIN {OFS=","} {gsub(/"/, "\"\"", $2); print $1, "\"" $2 "\""}' "$INPUT_FILE" > "$TEMP_FILE"

# Run query in psql - output to stdout, redirect to file
# Use -q to suppress notices, and filter out COPY/CREATE lines
echo "Running tokenization and bucketing..."
psql -U postgres -d postgres -p 5432 -t -A -F $'	' -q -v ON_ERROR_STOP=1 << EOSQL | grep -v "^CREATE\|^COPY" > "$OUTPUT_FILE"
CREATE TEMP TABLE all_queries (query_id int, query_text text);
\copy all_queries FROM '$TEMP_FILE' WITH (FORMAT csv)

WITH tokenized AS (
    SELECT
        query_id,
        query_text,
        array_length(string_to_array(to_tsvector('english', query_text)::text, ' '), 1) as token_count
    FROM all_queries
),
bucketed AS (
    SELECT
        query_id,
        query_text,
        CASE WHEN token_count >= 8 THEN 8 ELSE token_count END as token_bucket,
        row_number() OVER (
            PARTITION BY CASE WHEN token_count >= 8 THEN 8 ELSE token_count END
            ORDER BY query_id
        ) as rn
    FROM tokenized
    WHERE token_count IS NOT NULL
)
SELECT query_id, query_text, token_bucket
FROM bucketed
WHERE rn <= 100
ORDER BY token_bucket, query_id;
EOSQL

# Count results
echo ""
echo "Generated $OUTPUT_FILE"
TOTAL=$(wc -l < "$OUTPUT_FILE")
echo "Total queries: $TOTAL"
echo ""
echo "Distribution by token bucket:"
cut -f3 "$OUTPUT_FILE" | sort | uniq -c | sort -k2 -n

# Show sample queries from each bucket
echo ""
echo "Sample queries per bucket:"
for bucket in 1 2 3 4 5 6 7 8; do
    echo "  Bucket $bucket: $(grep -P "\t${bucket}$" "$OUTPUT_FILE" | head -1 | cut -f2 | head -c 60)..."
done
