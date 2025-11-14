# BM25 Validation System

This directory contains the Python-based validation system for BM25 scoring in pg_textsearch. It generates expected output files for regression tests while validating BM25 scores against a ground-truth implementation.

## Overview

The validation system:
1. Executes SQL test files against a running PostgreSQL instance
2. Computes ground-truth BM25 scores using a Python implementation that matches Tantivy's formula
3. Generates complete expected output files with validated scores
4. Ensures tokenization consistency by using PostgreSQL's text search configuration

## Components

- `bm25_tantivy.py` - BM25 implementation using Tantivy's IDF formula
- `sql_parser.py` - Parses SQL files and extracts validation markers
- `tokenizer.py` - Uses PostgreSQL's tokenization for consistency
- `postgres_executor.py` - Executes SQL and captures output
- `generate_expected.py` - Main script that generates expected outputs

## Installation

```bash
# Install Python dependencies
pip3 install -r test/python/requirements.txt

# Or use the Makefile
make install-validation-deps
```

## Usage

### Adding Validation Markers to SQL Tests

Add special comments to SQL test files to mark validation points:

```sql
-- VALIDATE_BM25: table=mytable query="search terms" index=myindex
SELECT * FROM mytable WHERE content <@> to_bm25query('search terms', 'myindex');
```

Parameters:
- `table` - The table being queried
- `query` - The search query (must match the SQL query)
- `index` - The BM25 index name
- `text_config` (optional) - Override text search configuration

### Generating Expected Outputs

#### For a Single Test File

```bash
# Generate expected output for one test
python3 test/python/generate_expected.py test/sql/vector.sql

# Or use the Makefile
make generate-expected SQL=test/sql/vector.sql
```

#### For All Test Files

```bash
# Regenerate all expected outputs
python3 test/python/generate_expected.py --all

# Or use the Makefile
make regenerate-all-expected
```

### Command-Line Options

```bash
python3 test/python/generate_expected.py [options] [sql_file]

Options:
  --all               Process all SQL test files
  --output FILE       Specify output file (default: test/expected/<name>.out)
  --validate-only     Validate without regenerating
  --decimal-places N  Number of decimal places for scores (default: 6)
  --verbose          Enable debug logging
  --host HOST        PostgreSQL host (default: localhost)
  --port PORT        PostgreSQL port (default: 5432)
  --database DB      PostgreSQL database (default: postgres)
  --user USER        PostgreSQL user (default: postgres)
```

## How It Works

### 1. SQL Parsing

The system parses SQL files to:
- Split into executable chunks at statement boundaries
- Extract validation markers
- Track table operations (CREATE, INSERT, etc.)

### 2. Tokenization Consistency

To ensure Python and PostgreSQL use identical tokenization:

```sql
-- PostgreSQL extracts tokens using:
SELECT to_tsvector('english', content) FROM table;

-- Python uses these exact tokens for BM25 calculation
```

### 3. Score Calculation

The BM25Tantivy class uses Tantivy's IDF formula:
```
IDF = ln(1 + (N - n + 0.5) / (n + 0.5))
```

This formula never produces negative values, unlike the standard BM25 formula.

### 4. Output Generation

The system:
- Executes SQL using psql to get exact output format
- Validates scores at marked points
- Updates scores with ground-truth values
- Preserves all other output exactly

## Validation Workflow

1. **Development Phase**: Add validation markers to SQL tests
2. **Testing Phase**: Run `make test-validation` to verify the system works
3. **Generation Phase**: Run `make regenerate-all-expected` to update all outputs
4. **CI Phase**: Tests run normally using the generated expected files

## Troubleshooting

### Database Connection Issues

Ensure PostgreSQL is running and accessible:
```bash
psql -h localhost -U postgres -d postgres
```

### Tokenization Mismatches

Check the text configuration:
```sql
-- See what tokens PostgreSQL generates
SELECT to_tsvector('english', 'your text here');
```

### Score Differences

Enable verbose mode to see detailed calculations:
```bash
python3 test/python/generate_expected.py test/sql/vector.sql --verbose
```

## Example

Here's a complete example SQL test with validation:

```sql
-- test/sql/example.sql

CREATE EXTENSION pg_textsearch;

CREATE TABLE docs (id int, content text);
INSERT INTO docs VALUES
    (1, 'PostgreSQL database'),
    (2, 'Database systems');

CREATE INDEX docs_idx ON docs USING bm25 (content)
    WITH (text_config='simple');

-- VALIDATE_BM25: table=docs query="database" index=docs_idx
SELECT id, content,
       content <@> to_bm25query('database', 'docs_idx') AS score
FROM docs
ORDER BY score DESC;

DROP TABLE docs CASCADE;
```

Generate expected output:
```bash
make generate-expected SQL=test/sql/example.sql
```

This creates `test/expected/example.out` with validated BM25 scores.

## Notes

- Scores are rounded to 6 decimal places by default (configurable)
- PostgreSQL uses negative scores for DESC ordering compatibility
- The system handles INFO messages from EXPLAIN output
- All tokenization happens in PostgreSQL to ensure consistency
