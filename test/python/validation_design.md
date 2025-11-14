# BM25 Validation System Design

## Overview
This system generates expected output files for PostgreSQL regression tests by:
1. Executing SQL test files against a running PostgreSQL instance
2. Computing ground-truth BM25 scores using Python
3. Generating complete expected output files with validated scores

## Validation Markers

SQL test files contain special comment markers to indicate validation points:

```sql
-- !VALIDATE_BM25: table=test_table query="search terms" index=idx_name
SELECT * FROM test_table WHERE content <@> to_bm25query('search terms', 'idx_name');

-- !VALIDATE_BM25_SCAN: table=test_table query="search terms" index=idx_name
-- For validating INFO output from index scans
SET enable_seqscan = off;
EXPLAIN (ANALYZE, BUFFERS) SELECT ...;
```

## Marker Format

Each marker contains:
- `table`: The table being queried
- `query`: The search query (exact text)
- `index`: The index name (for fetching config)
- Optional `text_config`: Override text search configuration

## Processing Flow

1. **Parse SQL File**
   - Split into chunks at statement boundaries
   - Identify validation markers
   - Track table state (INSERTs, DELETEs, UPDATEs)

2. **Execute SQL Chunks**
   - Connect to PostgreSQL
   - Execute each chunk sequentially
   - Capture output exactly as psql would

3. **At Validation Points**
   - Query table contents using Postgres tokenization:
     ```sql
     SELECT ctid, to_tsvector('config', column) FROM table
     ```
   - Compute ground-truth BM25 scores using BM25Tantivy
   - Validate scores in query output
   - Replace/verify scores match ground truth

4. **Generate Expected Output**
   - Combine all outputs
   - Format exactly as PostgreSQL regression tests expect
   - Write to test/expected/*.out

## Tokenization Consistency

To ensure Python and PostgreSQL use identical tokenization:
```python
# Get tokenized documents from PostgreSQL
cursor.execute("""
    SELECT ctid,
           to_tsvector(%s, content) as tsvector,
           content as raw_text
    FROM %s
""", (text_config, table_name))

# Extract tokens from tsvector
tokens = parse_tsvector(tsvector)  # Returns ['token1', 'token2', ...]
```

## Score Validation

For each result row:
1. Extract ctid
2. Look up ground-truth score for that ctid
3. Validate score matches to N decimal places
4. For INFO output, validate intermediate calculations

## Example Usage

```bash
# Generate expected output for a single test
python3 test/python/generate_expected.py test/sql/vector.sql

# Regenerate all expected outputs
python3 test/python/generate_expected.py --all

# Validate without regenerating
python3 test/python/generate_expected.py --validate test/sql/vector.sql
```

## Implementation Components

1. **sql_parser.py**: Parse SQL files and extract chunks
2. **postgres_executor.py**: Execute SQL and capture output
3. **tokenizer.py**: Extract tokens using Postgres
4. **bm25_tantivy.py**: Ground-truth BM25 implementation
5. **validator.py**: Validate scores at marker points
6. **generate_expected.py**: Main entry point

## Error Handling

- Clear error messages when validation fails
- Show expected vs actual scores
- Point to exact line in SQL file
- Option to update expected values automatically

## Configuration

Config file `test/python/validation_config.yaml`:
```yaml
database:
  host: localhost
  port: 5432
  dbname: postgres
  user: postgres

validation:
  decimal_places: 6
  validate_info: true
  validate_output: true

defaults:
  text_config: english
  k1: 1.2
  b: 0.75
```
