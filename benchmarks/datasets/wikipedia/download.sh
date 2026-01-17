#!/bin/bash
# Download and process Wikipedia dump for benchmarking
# Uses the Wikimedia Enterprise HTML dumps (cleaner than XML)
# English Wikipedia: ~6.7M articles, ~21GB compressed
#
# For faster testing, we also support downloading a subset

set -e

is_py310() {
    "$1" <<'EOF' >/dev/null 2>&1
import sys
sys.exit(0 if (sys.version_info.major, sys.version_info.minor) == (3, 10) else 1)
EOF
}

if is_py310 python3.10; then
    PYTHON_BIN=python3.10
elif is_py310 python3; then
    PYTHON_BIN=python3
elif is_py310 python; then
    PYTHON_BIN=python
else
    echo "Error: Python 3.10 is required. Install python3.10 and retry."
    exit 1
fi

echo "Using Python interpreter: $PYTHON_BIN"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
SUBSET_SIZE="${1:-full}"  # "full", "1M", "100K", "10K"

mkdir -p "$DATA_DIR"
cd "$DATA_DIR"

echo "=== Wikipedia Dataset Download ==="
echo "Subset: $SUBSET_SIZE"
echo ""

# We'll use the Simple Wikipedia dump for a more manageable size,
# or the BEIR Wikipedia subset which is pre-processed for IR benchmarks

if [ "$SUBSET_SIZE" = "full" ]; then
    # Full English Wikipedia - articles only (no talk pages, etc.)
    # Latest dump from dumps.wikimedia.org
    DUMP_DATE="latest"
    DUMP_URL="https://dumps.wikimedia.org/enwiki/$DUMP_DATE/enwiki-$DUMP_DATE-pages-articles.xml.bz2"

    echo "Downloading full English Wikipedia dump..."
    echo "WARNING: This is ~21GB compressed, ~80GB uncompressed"
    echo "Consider using a subset for initial testing: ./download.sh 1M"
    echo ""

    if [ ! -f "enwiki-pages-articles.xml.bz2" ]; then
        # Get the actual latest dump date
        DUMP_DATE=$(curl -s https://dumps.wikimedia.org/enwiki/ | \
            grep -oE '20[0-9]{6}' | sort -r | head -1)
        DUMP_URL="https://dumps.wikimedia.org/enwiki/$DUMP_DATE/enwiki-$DUMP_DATE-pages-articles.xml.bz2"
        echo "Latest dump: $DUMP_DATE"
        wget -q --show-progress -O enwiki-pages-articles.xml.bz2 "$DUMP_URL"
    fi

    echo "Processing Wikipedia XML dump..."
    echo "This requires WikiExtractor: python3.10 -m pip install wikiextractor"

    if ! "$PYTHON_BIN" -c "import wikiextractor" &> /dev/null; then
        echo "Installing wikiextractor..."
        "$PYTHON_BIN" -m pip install wikiextractor
    fi

    # Extract to JSON format
    if [ ! -d "extracted" ]; then
        "$PYTHON_BIN" -m wikiextractor.WikiExtractor \
            --json \
            --output extracted \
            --compress \
            enwiki-pages-articles.xml.bz2
    fi

    # Convert to TSV format for COPY
    echo "Converting to TSV format..."
    "$PYTHON_BIN" << 'PYTHON_SCRIPT'
import json
import gzip
import os
from pathlib import Path

output_file = open("wikipedia.tsv", "w")
doc_id = 0

for subdir in sorted(Path("extracted").iterdir()):
    if not subdir.is_dir():
        continue
    for wiki_file in sorted(subdir.iterdir()):
        if wiki_file.suffix == '.gz':
            opener = gzip.open
        else:
            opener = open
        with opener(wiki_file, 'rt', encoding='utf-8') as f:
            for line in f:
                doc = json.loads(line)
                title = doc.get('title', '').replace('\t', ' ').replace('\n', ' ')
                text = doc.get('text', '').replace('\t', ' ').replace('\n', ' ')
                if title and text and len(text) > 100:
                    doc_id += 1
                    output_file.write(f"{doc_id}\t{title}\t{text}\n")
                    if doc_id % 100000 == 0:
                        print(f"Processed {doc_id} articles...")

output_file.close()
print(f"Total articles: {doc_id}")
PYTHON_SCRIPT

else
    # Use pre-processed subsets for faster testing
    # BEIR provides a Wikipedia subset, or we can use HuggingFace datasets

    echo "Downloading Wikipedia subset ($SUBSET_SIZE articles)..."

    # For subsets, we'll generate from Simple Wikipedia or use a seed + generate
    case "$SUBSET_SIZE" in
        "1M")
            NUM_ARTICLES=1000000
            ;;
        "100K")
            NUM_ARTICLES=100000
            ;;
        "10K")
            NUM_ARTICLES=10000
            ;;
        *)
            echo "Unknown subset size: $SUBSET_SIZE"
            echo "Use: full, 1M, 100K, or 10K"
            exit 1
            ;;
    esac

    # Download Simple English Wikipedia (smaller, ~200K articles)
    # and truncate to desired size
    if [ ! -f "simplewiki-latest-pages-articles.xml.bz2" ]; then
        echo "Downloading Simple English Wikipedia..."
        wget -q --show-progress \
            https://dumps.wikimedia.org/simplewiki/latest/simplewiki-latest-pages-articles.xml.bz2
    fi

    if ! "$PYTHON_BIN" -c "import wikiextractor" &> /dev/null; then
        echo "Installing wikiextractor..."
        "$PYTHON_BIN" -m pip install wikiextractor
    fi

    if [ ! -d "simple_extracted" ]; then
        echo "Extracting Simple Wikipedia..."
        "$PYTHON_BIN" -m wikiextractor.WikiExtractor \
            --json \
            --output simple_extracted \
            simplewiki-latest-pages-articles.xml.bz2
    fi

    echo "Converting to TSV (limiting to $NUM_ARTICLES articles)..."
    "$PYTHON_BIN" << PYTHON_SCRIPT
import json
import os
from pathlib import Path

output_file = open("wikipedia.tsv", "w")
doc_id = 0
max_docs = $NUM_ARTICLES

for subdir in sorted(Path("simple_extracted").iterdir()):
    if not subdir.is_dir():
        continue
    for wiki_file in sorted(subdir.iterdir()):
        with open(wiki_file, 'r', encoding='utf-8') as f:
            for line in f:
                if doc_id >= max_docs:
                    break
                doc = json.loads(line)
                title = doc.get('title', '').replace('\t', ' ').replace('\n', ' ')
                text = doc.get('text', '').replace('\t', ' ').replace('\n', ' ')
                if title and text and len(text) > 100:
                    doc_id += 1
                    output_file.write(f"{doc_id}\t{title}\t{text}\n")
        if doc_id >= max_docs:
            break
    if doc_id >= max_docs:
        break

output_file.close()
print(f"Total articles: {doc_id}")
PYTHON_SCRIPT

fi

echo ""
echo "=== Download Complete ==="
if [ -f "wikipedia.tsv" ]; then
    echo "Articles: $(wc -l < wikipedia.tsv)"
    echo "File size: $(du -h wikipedia.tsv | cut -f1)"
fi
