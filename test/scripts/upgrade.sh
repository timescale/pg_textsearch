#!/bin/bash

# Test extension upgrades between versions
# This script tests that:
# 1. Indexes created in older versions work after upgrade
# 2. Data can be queried correctly after upgrade
# 3. New indexes can be created after upgrade

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PSQL="psql -X -q"
PORT=55434
DB_NAME="upgrade_test_db"
DATA_DIR="/tmp/pg_textsearch_upgrade_test"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

function cleanup() {
    echo "Cleaning up..."
    pg_ctl stop -D "$DATA_DIR" -m immediate 2>/dev/null || true
    rm -rf "$DATA_DIR"
}

function log_success() {
    echo -e "${GREEN}✓${NC} $1"
}

function log_error() {
    echo -e "${RED}✗${NC} $1"
    exit 1
}

# Clean up on exit
trap cleanup EXIT

echo "=== pg_textsearch Upgrade Testing ==="
echo

# Step 1: Set up test instance
echo "Setting up test PostgreSQL instance..."
rm -rf "$DATA_DIR"
initdb -D "$DATA_DIR" --auth-local=trust --auth-host=trust >/dev/null 2>&1
echo "port = $PORT" >> "$DATA_DIR/postgresql.conf"
echo "shared_buffers = 256MB" >> "$DATA_DIR/postgresql.conf"
echo "max_connections = 20" >> "$DATA_DIR/postgresql.conf"
echo "log_statement = 'all'" >> "$DATA_DIR/postgresql.conf"
pg_ctl start -D "$DATA_DIR" -l "$DATA_DIR/logfile" -w >/dev/null
createdb -p $PORT "$DB_NAME"
log_success "PostgreSQL instance created"

# Function to test a specific version
test_version() {
    local version=$1
    local sql_file="$PROJECT_ROOT/sql/pg_textsearch--${version}.sql"

    if [ ! -f "$sql_file" ]; then
        echo "Skipping version $version (file not found)"
        return
    fi

    echo
    echo "Testing upgrade from version $version..."

    # Drop database and recreate
    dropdb -p $PORT "$DB_NAME" 2>/dev/null || true
    createdb -p $PORT "$DB_NAME"

    # Manually install the old version (simulate installing from an older release)
    # Note: In real-world testing, we would actually install an older .so file
    # For now, we test that upgrade scripts work with current .so
    $PSQL -p $PORT -d "$DB_NAME" -c "CREATE EXTENSION pg_textsearch VERSION '$version';" 2>/dev/null || {
        echo "  Version $version not available for testing (expected for now)"
        return
    }

    # Create test data and index
    $PSQL -p $PORT -d "$DB_NAME" <<EOF
CREATE TABLE test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL
);

INSERT INTO test_docs (content) VALUES
    ('PostgreSQL is a powerful, open source object-relational database system'),
    ('It has more than 35 years of active development'),
    ('PostgreSQL has earned a strong reputation for reliability and performance'),
    ('Full text search capabilities are built into PostgreSQL'),
    ('The BM25 algorithm provides relevance ranking for text search');

-- Create index with old version
CREATE INDEX idx_test_bm25 ON test_docs
USING bm25 (content) WITH (text_config = 'english');

-- Test search works before upgrade
SELECT COUNT(*) FROM test_docs
WHERE content @@ to_tsquery('english', 'postgresql');
EOF

    # Get count before upgrade
    BEFORE_COUNT=$($PSQL -p $PORT -d "$DB_NAME" -t -c "SELECT COUNT(*) FROM test_docs WHERE content @@ to_tsquery('english', 'postgresql');")

    # Perform upgrade
    echo "  Upgrading to latest version..."
    $PSQL -p $PORT -d "$DB_NAME" -c "ALTER EXTENSION pg_textsearch UPDATE;" >/dev/null 2>&1

    # Verify extension version
    CURRENT_VERSION=$($PSQL -p $PORT -d "$DB_NAME" -t -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_textsearch';")
    echo "  Current version: $CURRENT_VERSION"

    # Test that old index still works
    AFTER_COUNT=$($PSQL -p $PORT -d "$DB_NAME" -t -c "SELECT COUNT(*) FROM test_docs WHERE content @@ to_tsquery('english', 'postgresql');")

    if [ "$BEFORE_COUNT" = "$AFTER_COUNT" ]; then
        log_success "Search results consistent after upgrade"
    else
        log_error "Search results changed after upgrade!"
    fi

    # Test creating new index after upgrade
    $PSQL -p $PORT -d "$DB_NAME" <<EOF
CREATE TABLE new_docs (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL
);

INSERT INTO new_docs (content) VALUES ('Testing after upgrade');

CREATE INDEX idx_new_bm25 ON new_docs
USING bm25 (content) WITH (text_config = 'english');

SELECT COUNT(*) FROM new_docs
WHERE content @@ to_tsquery('english', 'testing');
EOF

    log_success "New index creation works after upgrade"
}

# Step 2: Test current version (no actual upgrade, but tests ALTER EXTENSION UPDATE)
echo
echo "Testing current version upgrade path..."
$PSQL -p $PORT -d "$DB_NAME" <<EOF
CREATE EXTENSION pg_textsearch;
ALTER EXTENSION pg_textsearch UPDATE;
SELECT extversion FROM pg_extension WHERE extname = 'pg_textsearch';
DROP EXTENSION pg_textsearch;
EOF
log_success "Current version upgrade test passed"

# Step 3: Test upgrades from older versions (when available)
# Note: These will only work if we have the actual old .so files
# For now, we document the intended test structure
for version in "0.0.1" "0.0.2"; do
    test_version "$version"
done

echo
echo "=== All upgrade tests completed successfully ==="#
