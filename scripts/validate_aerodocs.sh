#!/bin/bash
# Example script to validate BM25 scores for the aerodocs test data

# Install Python dependencies if needed
if ! python3 -c "import rank_bm25" 2>/dev/null; then
    echo "Installing Python dependencies..."
    pip3 install -r scripts/requirements.txt
fi

# Run validation for aerodocs test data
python3 scripts/bm25_validation.py \
    --dbname contrib_regression \
    --table aerodocs_documents \
    --text-column full_text \
    --index-name cranfield_tapir_idx \
    --id-column doc_id \
    --text-config english \
    --k1 1.2 \
    --b 0.75 \
    --queries "aerodynamic flow analysis" "computational fluid dynamics" "wing design" \
    --tolerance 0.01

# You can also run with specific WHERE clause to test subsets:
# python3 scripts/bm25_validation.py \
#     --dbname contrib_regression \
#     --table aerodocs_documents \
#     --text-column full_text \
#     --index-name cranfield_tapir_idx \
#     --id-column doc_id \
#     --text-config english \
#     --where "doc_id <= 10" \
#     --queries "aerodynamic flow" \
#     --tolerance 0.01