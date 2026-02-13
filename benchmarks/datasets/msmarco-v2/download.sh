#!/bin/bash
# Download MS MARCO v2 Passage Ranking dataset
# Full dataset: ~138M passages, ~3,903 dev queries
# Source: https://microsoft.github.io/msmarco/TREC-Deep-Learning
#
# Usage: ./download.sh
#
# This downloads the full 138M passage collection (no subsetting).
# The tar file contains 70 gzipped JSONL files which are streamed
# to a single TSV, deleting each .gz after processing to save disk.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"

mkdir -p "$DATA_DIR"
cd "$DATA_DIR"

echo "=== MS MARCO v2 Passage Ranking Dataset Download ==="
echo "Full dataset: ~138M passages"
echo ""

# Collection (passages) - 20.3GB tar of 70 gzipped JSONL files
if [ ! -f "collection.tsv" ]; then
    if [ ! -d "msmarco_v2_passage" ]; then
        if [ ! -f "msmarco_v2_passage.tar" ]; then
            echo "Downloading passage collection (~20GB)..."
            wget -q --show-progress \
                https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco_v2_passage.tar
        fi
        echo "Extracting tar archive..."
        tar xf msmarco_v2_passage.tar
        rm msmarco_v2_passage.tar
        echo "Tar extracted, converting JSONL files to TSV..."
    fi

    # Convert 70 gzipped JSONL files to single TSV
    # Each JSONL line: {"pid": "msmarco_passage_XX_NNNNN", "passage": "..."}
    # Delete each .gz after processing to save disk space
    echo "Converting JSONL files to collection.tsv..."
    > collection.tsv
    file_count=0
    total_files=$(ls -1 msmarco_v2_passage/*.gz 2>/dev/null | wc -l)

    for f in msmarco_v2_passage/*.gz; do
        file_count=$((file_count + 1))
        echo "  Processing $f ($file_count/$total_files)..."
        zcat "$f" | python3 -c "
import sys, json
for line in sys.stdin:
    obj = json.loads(line)
    pid = obj['pid']
    text = obj['passage'].replace('\t', ' ').replace('\n', ' ')
    print(f'{pid}\t{text}')
" >> collection.tsv
        rm "$f"
    done

    # Clean up empty directory
    rmdir msmarco_v2_passage 2>/dev/null || true

    echo "Collection converted: $(wc -l < collection.tsv) passages"
else
    echo "Collection already exists: $(wc -l < collection.tsv) passages"
fi

# Dev1 queries (3,903 queries)
if [ ! -f "passv2_dev_queries.tsv" ]; then
    echo "Downloading dev queries..."
    wget -q --show-progress \
        https://msmarco.z22.web.core.windows.net/msmarcoranking/passv2_dev_queries.tsv
    echo "Dev queries downloaded: $(wc -l < passv2_dev_queries.tsv) queries"
else
    echo "Dev queries already exist: $(wc -l < passv2_dev_queries.tsv) queries"
fi

# Dev1 relevance judgments (qrels)
if [ ! -f "passv2_dev_qrels.tsv" ]; then
    echo "Downloading relevance judgments..."
    wget -q --show-progress \
        https://msmarco.z22.web.core.windows.net/msmarcoranking/passv2_dev_qrels.tsv
    echo "Relevance judgments downloaded: $(wc -l < passv2_dev_qrels.tsv) qrels"
else
    echo "Relevance judgments already exist: $(wc -l < passv2_dev_qrels.tsv) qrels"
fi

echo ""
echo "=== Download Complete ==="
echo "Files in $DATA_DIR:"
ls -lh "$DATA_DIR"/*.tsv 2>/dev/null | head -10
echo ""
echo "Dataset statistics:"
echo "  Passages:  $(wc -l < collection.tsv)"
echo "  Dev queries: $(wc -l < passv2_dev_queries.tsv 2>/dev/null || echo 'N/A')"
echo "  Relevance judgments: $(wc -l < passv2_dev_qrels.tsv 2>/dev/null || echo 'N/A')"
