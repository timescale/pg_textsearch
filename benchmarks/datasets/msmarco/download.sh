#!/bin/bash
# Download MS MARCO Passage Ranking dataset
# Full dataset: ~8.8M passages, ~500K queries
# Source: https://microsoft.github.io/msmarco/
#
# Usage: ./download.sh [size]
#   size: 100K, 500K, 1M, or full (default: full)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
SIZE="${1:-full}"

mkdir -p "$DATA_DIR"
cd "$DATA_DIR"

echo "=== MS MARCO Passage Ranking Dataset Download ==="
echo "Requested size: $SIZE"
echo ""

# Collection (passages)
if [ ! -f "collection.tsv" ]; then
    echo "Downloading passage collection..."
    if [ ! -f "collection.tar.gz" ]; then
        wget -q --show-progress \
            https://msmarco.z22.web.core.windows.net/msmarcoranking/collection.tar.gz
    fi
    echo "Extracting collection..."
    tar -xzf collection.tar.gz
    rm collection.tar.gz
    echo "Collection extracted: $(wc -l < collection.tsv) passages"
else
    echo "Collection already exists: $(wc -l < collection.tsv) passages"
fi

# Queries (dev set - 6980 queries with relevance judgments)
if [ ! -f "queries.dev.small.tsv" ]; then
    echo "Downloading dev queries..."
    wget -q --show-progress \
        https://msmarco.z22.web.core.windows.net/msmarcoranking/queries.tar.gz
    tar -xzf queries.tar.gz
    rm queries.tar.gz
    echo "Queries extracted"
else
    echo "Queries already exist"
fi

# Relevance judgments (qrels) for dev set
if [ ! -f "qrels.dev.small.tsv" ]; then
    echo "Downloading relevance judgments..."
    wget -q --show-progress \
        https://msmarco.z22.web.core.windows.net/msmarcoranking/qrels.dev.small.tsv
    echo "Relevance judgments downloaded"
else
    echo "Relevance judgments already exist"
fi

# Create subset if requested
if [ "$SIZE" != "full" ]; then
    echo ""
    echo "=== Creating $SIZE passage subset ==="

    case "$SIZE" in
        100K) LIMIT=100000 ;;
        500K) LIMIT=500000 ;;
        1M)   LIMIT=1000000 ;;
        *)    echo "Unknown size: $SIZE, using full dataset"; LIMIT=0 ;;
    esac

    if [ "$LIMIT" -gt 0 ]; then
        # Keep original as collection_full.tsv
        if [ ! -f "collection_full.tsv" ]; then
            mv collection.tsv collection_full.tsv
        fi
        # Create subset
        head -n "$LIMIT" collection_full.tsv > collection.tsv
        echo "Created subset: $(wc -l < collection.tsv) passages"

        # Clean up full version to save space
        rm -f collection_full.tsv
    fi
fi

echo ""
echo "=== Download Complete ==="
echo "Files in $DATA_DIR:"
ls -lh "$DATA_DIR"/*.tsv 2>/dev/null | head -10
echo ""
echo "Dataset statistics:"
echo "  Passages:  $(wc -l < collection.tsv)"
echo "  Dev queries: $(wc -l < queries.dev.small.tsv 2>/dev/null || echo 'N/A')"
echo "  Relevance judgments: $(wc -l < qrels.dev.small.tsv 2>/dev/null || echo 'N/A')"
