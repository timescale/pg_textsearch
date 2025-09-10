# Tapir BM25 Validation Scripts

This directory contains tools for validating Tapir's BM25 scoring implementation against a reference Python implementation.

## Setup

Install the required Python dependencies:

```bash
pip3 install -r requirements.txt
```

## Main Validation Tool

### `bm25_validation.py`

This is the core validation tool that:
1. Connects to your PostgreSQL database
2. Fetches documents and applies the same text processing (stemming via `to_tsvector`)
3. Calculates BM25 scores using the Python `rank-bm25` library
4. Fetches Tapir's BM25 scores via SQL
5. Compares the scores and reports differences

### Usage

```bash
python3 bm25_validation.py \
    --dbname your_database \
    --table your_table \
    --text-column content_column \
    --index-name your_tapir_index \
    --queries "your search query" "another query" \
    --k1 1.2 \
    --b 0.75
```

### Options

- `--host`: Database host (default: localhost)
- `--port`: Database port (default: 5432)
- `--dbname`: Database name (required)
- `--user`: Database user (default: postgres)
- `--password`: Database password
- `--table`: Table name (required)
- `--text-column`: Name of the text column (required)
- `--index-name`: Name of the Tapir index (required)
- `--id-column`: ID column name (default: id)
- `--text-config`: PostgreSQL text search configuration (default: english)
- `--where`: Optional WHERE clause for filtering
- `--k1`: BM25 k1 parameter (default: 1.2)
- `--b`: BM25 b parameter (default: 0.75)
- `--queries`: One or more queries to test (required)
- `--tolerance`: Acceptable score difference (default: 0.001)

## Example Scripts

### `validate_aerodocs.sh`

Validates the aerodocs test data (used in Tapir's test suite):

```bash
./validate_aerodocs.sh
```

### `validate_simple.sh`

Validates a simple test_docs table:

```bash
./validate_simple.sh [database_name]
```

## Output

The validation tool provides:
- Side-by-side score comparison for top results
- Statistics on score differences
- Correlation analysis (Pearson and Spearman rank correlation)
- Detailed reporting of any mismatches

Example output:

```
Query: 'aerodynamic flow analysis'
------------------------------------------------------------

Top 10 Results:
+--------+--------------------------------+-------------+------------+------------+-------+
| doc_id | document                       | python_score| tapir_score| difference | match |
+--------+--------------------------------+-------------+------------+------------+-------+
|      1 | Aerodynamic Flow Over Wings... |    1.021100 |   1.021100 |   0.000000 |   ✓   |
|      3 | Supersonic Aircraft Design...  |    0.765100 |   0.765100 |   0.000000 |   ✓   |
...

Statistics:
  Total documents: 15
  Matching scores (diff < 0.001): 15
  Mismatched scores: 0
  Average difference: 0.000000
  Max difference: 0.000000
  Score correlation: 1.000000
  Rank correlation (Spearman): 1.000000
```

## How It Works

1. **Text Processing**: Both systems use PostgreSQL's `to_tsvector` for consistent stemming
2. **BM25 Calculation**: Python uses the standard BM25 formula with the same k1 and b parameters
3. **Score Normalization**: Tapir returns negative scores for PostgreSQL ordering; the tool converts these back to positive
4. **Comparison**: Scores are compared with a configurable tolerance level

## Troubleshooting

- If scores don't match, check that:
  - The same text_config is used for both index creation and validation
  - The k1 and b parameters match between Tapir index and validation
  - The corpus statistics (document count, average length) are consistent

- For debugging, you can use `--where` to test on a subset of documents

## Dependencies

- `psycopg2-binary`: PostgreSQL database adapter
- `rank-bm25`: Reference BM25 implementation
- `pandas`: Data manipulation and analysis
- `numpy`: Numerical computations
- `tabulate`: Pretty-printing tables