# Tapir Validation Scripts

BM25 scoring validation tools for Tapir.

## Setup

```bash
pip3 install -r requirements.txt
```

## Scripts

### `bm25_validation.py`

Validates Tapir BM25 scores against Python reference implementation.

```bash
python3 bm25_validation.py \
    --dbname your_database \
    --table your_table \
    --text-column content_column \
    --index-name your_tapir_index \
    --queries "search query"
```

Key options:
- `--k1`: BM25 k1 parameter (default: 1.2)
- `--b`: BM25 b parameter (default: 0.75)
- `--tolerance`: Acceptable score difference (default: 0.001)
- `--text-config`: PostgreSQL text config (default: english)

### `validate_all_tests.py`

Runs validation on all Tapir test datasets.

```bash
python3 validate_all_tests.py
```

### `validate_aerodocs.sh`

Validates aerodocs test dataset.

```bash
./validate_aerodocs.sh
```

## Output

Shows score comparison, statistics, and correlation analysis:
- Side-by-side score comparison
- Pearson and Spearman correlations
- Average/max differences
- Match rate