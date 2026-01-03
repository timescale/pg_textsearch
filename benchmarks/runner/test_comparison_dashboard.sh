#!/bin/bash
# Test the comparison dashboard locally
# Usage: ./test_comparison_dashboard.sh
#
# This creates a temporary directory with the dashboard and sample data,
# then starts a Python HTTP server to view it in a browser.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_DIR=$(mktemp -d)

echo "Setting up test dashboard in $TEST_DIR"

# Copy dashboard and test data
cp "$SCRIPT_DIR/index_comparison.html" "$TEST_DIR/index.html"
cp "$SCRIPT_DIR/test_data.js" "$TEST_DIR/data.js"

# Create empty branch_info.js (test data includes it)
echo "// Branch info included in test_data.js" > "$TEST_DIR/branch_info.js"

# Create profiles directory placeholder
mkdir -p "$TEST_DIR/profiles"
echo "<html><body>No profiles in test mode</body></html>" > "$TEST_DIR/profiles/index.html"

echo ""
echo "Dashboard ready at $TEST_DIR"
echo ""
echo "Starting HTTP server on port 8080..."
echo "Open http://localhost:8080 in your browser"
echo ""
echo "Press Ctrl+C to stop"
echo ""

cd "$TEST_DIR"
python3 -m http.server 8080
