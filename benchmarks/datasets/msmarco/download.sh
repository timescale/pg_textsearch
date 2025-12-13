#!/bin/bash
# Download MS MARCO Passage Ranking dataset
# Full dataset: ~8.8M passages, ~500K queries
# Source: https://microsoft.github.io/msmarco/

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"

mkdir -p "$DATA_DIR"
cd "$DATA_DIR"

echo "=== MS MARCO Passage Ranking Dataset Download ==="
echo "This will download approximately 2GB of compressed data"
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

echo ""
echo "=== Download Complete ==="
echo "Files in $DATA_DIR:"
ls -lh "$DATA_DIR"/*.tsv 2>/dev/null | head -10
echo ""
echo "Dataset statistics:"
echo "  Passages:  $(wc -l < collection.tsv)"
echo "  Dev queries: $(wc -l < queries.dev.small.tsv)"
echo "  Relevance judgments: $(wc -l < qrels.dev.small.tsv)"
