#!/bin/bash
# Simple validation script for test_docs table

# Install Python dependencies if needed
if ! python3 -c "import rank_bm25" 2>/dev/null; then
    echo "Installing Python dependencies..."
    pip3 install -r scripts/requirements.txt
fi

# Database name (default: contrib_regression)
DB=${1:-contrib_regression}

# Create a simple test if test_docs exists
psql -d $DB -c "SELECT 1 FROM test_docs LIMIT 1" >/dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "Validating test_docs table..."
    python3 scripts/bm25_validation.py \
        --dbname $DB \
        --table test_docs \
        --text-column content \
        --index-name docs_english_idx \
        --text-config english \
        --queries "quick brown fox" "postgresql database" "lazy dog" \
        --tolerance 0.01
else
    echo "test_docs table not found in $DB"
    echo "Usage: $0 [database_name]"
    echo ""
    echo "Make sure to create and populate a test table first:"
    echo "  CREATE TABLE test_docs (id SERIAL PRIMARY KEY, content TEXT);"
    echo "  INSERT INTO test_docs (content) VALUES ('quick brown fox'), ('lazy dog');"
    echo "  CREATE INDEX docs_english_idx ON test_docs USING tapir(content) WITH (text_config='english');"
fi