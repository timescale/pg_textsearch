# BM25 Test Validation System

This directory contains Python scripts for validating BM25 scoring in pg_textsearch tests using a template-based approach.

## Files

- `template_processor.py` - Main template processing logic for generating validation SQL
- `process_templates.py` - Command-line interface for processing SQL templates
- `bm25_tantivy.py` - BM25 implementation matching the IDF formula
- `tokenizer.py` - PostgreSQL-compatible text tokenization
- `postgres_executor.py` - PostgreSQL database connection and query execution
- `test_template.py` - Simple test script for the template system
- `requirements.txt` - Python package dependencies

## Template Processing

The system processes `.sql.template` files to generate regular `.sql` test files with BM25 score validation.

### Usage

Process a single template:
```bash
cd test/python
python process_templates.py ../sql/scoring1.sql.template ../sql/scoring1.sql
```

Process all templates via Makefile:
```bash
make process-templates
```

## Template Format

Templates use `VALIDATE_BM25` markers to indicate where BM25 validation should occur:

```sql
-- VALIDATE_BM25: table=docs query="search terms" index=idx_name
SELECT * FROM docs ORDER BY score LIMIT 5;
```

The processor will:
1. Extract document data from the specified table
2. Calculate expected BM25 scores
3. Generate a temp table with expected results
4. Validate that actual results match expected scores

## BM25 Formula

Uses the formula: `log(1 + (N - df + 0.5) / (df + 0.5))`

This ensures IDF is always non-negative, preventing issues with very common terms.
