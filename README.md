# pg_textsearch

![Tapir and Friends](images/tapir_and_friends.png)

[![CI](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml/badge.svg)](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml)

Open-source full-text search for Postgres.
Supports:

- BM25 ranking with configurable parameters (k1, b)
- PostgreSQL text search configurations (english, french, german, etc.)
- bm25 index and bm25query data type for fast ranked searches

🚀 **Development Status**: v0.0.3-dev - development version.  Memtable-based
implementation is in place and working. This is prerelease software and should not be used in production.

## Historical note

The original name of the project was Tapir - **T**extual **A**nalysis for **P**ostgres **I**nformation **R**etrieval.  We still use the tapir as our
mascot and the name occurs in various places in the source code.

## PostgreSQL Version Compatibility

pg_textsearch supports:
- PostgreSQL 17
- PostgreSQL 18

### New in PostgreSQL 18 Support

- **Embedded index name syntax**: Use `index_name:query` format in cast expressions for
  better compatibility with PG18's query planner
- **Improved ORDER BY optimization**: Full support for PG18's consistent ordering semantics
- **Query planner compatibility**: Works correctly with PG18's more eager expression evaluation

## Installation

### Linux and Mac

Compile and install the extension (requires PostgreSQL 17 or 18)

```sh
cd /tmp
git clone https://github.com/timescale/pg_textsearch
cd pg_textsearch
make
make install # may need sudo
```

## Getting Started

Enable the extension (do this once in each database where you want to use it)

```sql
CREATE EXTENSION pg_textsearch;
```

Create a table with text content

```sql
CREATE TABLE documents (id bigserial PRIMARY KEY, content text);
INSERT INTO documents (content) VALUES
    ('PostgreSQL is a powerful database system'),
    ('BM25 is an effective ranking function'),
    ('Full text search with custom scoring');
```

Create a pg_textsearch index on the text column

```sql
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

## Querying

Get the most relevant documents using the `<@>` operator

```sql
SELECT * FROM documents
ORDER BY content <@> to_bm25query('database system', 'docs_idx')
LIMIT 5;
```

Note: `<@>` returns the negative BM25 score since Postgres only supports `ASC` order index scans on operators. Lower scores indicate better matches.

For WHERE clause queries, use bm25query with index name:
```sql
SELECT * FROM documents
WHERE content <@> to_bm25query('database system', 'docs_idx') < -1.0;
```

Supported operations:
- `text <@> bm25query` - Score text against a query

### Verifying Index Usage

Check query plan with EXPLAIN:
```sql
EXPLAIN SELECT * FROM documents
ORDER BY content <@> to_bm25query('database system', 'docs_idx')
LIMIT 5;
```

For small datasets, PostgreSQL may prefer sequential scans. Force index usage:
```sql
SET enable_seqscan = off;
```

Note: Even if EXPLAIN shows a sequential scan, `<@>` and `to_bm25query` always use the index for corpus statistics (document counts, average length) required for BM25 scoring.

## Indexing

Create a BM25 index on your text columns:

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english');
```

### Index Options

- `text_config` - PostgreSQL text search configuration to use (required)
- `k1` - term frequency saturation parameter (1.2 by default)
- `b` - length normalization parameter (0.75 by default)

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english', k1=1.5, b=0.8);
```

Also supports different text search configurations:

```sql
-- English documents with stemming
CREATE INDEX docs_en_idx ON documents USING bm25(content) WITH (text_config='english');

-- Simple text processing without stemming
CREATE INDEX docs_simple_idx ON documents USING bm25(content) WITH (text_config='simple');

-- Language-specific configurations
CREATE INDEX docs_fr_idx ON french_docs USING bm25(content) WITH (text_config='french');
CREATE INDEX docs_de_idx ON german_docs USING bm25(content) WITH (text_config='german');
```

## Data Types

### bm25query

The `bm25query` type represents queries for BM25 scoring with optional index context:

```sql
-- Create a bm25query with index name (required for WHERE clause and standalone scoring)
SELECT to_bm25query('search query text', 'docs_idx');
-- Returns: docs_idx:search query text

-- Embedded index name syntax (alternative form using cast)
SELECT 'docs_idx:search query text'::bm25query;
-- Returns: docs_idx:search query text

-- Create a bm25query without index name (only works in ORDER BY with index scan)
SELECT to_bm25query('search query text');
-- Returns: search query text
```

**Note**: In PostgreSQL 18, the embedded index name syntax using single colon (`:`) allows the
query planner to determine the index name even when evaluating SELECT clause expressions early.
This ensures compatibility across different query evaluation strategies.

#### bm25query Functions

Function | Description
--- | ---
to_bm25query(text) → bm25query | Create bm25query without index name (for ORDER BY only)
to_bm25query(text, text) → bm25query | Create bm25query with query text and index name
text <@> bm25query → double precision | BM25 scoring operator (returns negative scores)
bm25query = bm25query → boolean | Equality comparison

## Performance

pg_textsearch indexes use a memtable architecture for efficient writes. Like other index types, it's faster to create an index after loading your data.

```sql
-- Load data first
INSERT INTO documents (content) VALUES (...);

-- Then create index
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

## Monitoring

```sql
-- Check index usage
SELECT schemaname, tablename, indexname, idx_scan, idx_tup_read, idx_tup_fetch
FROM pg_stat_user_indexes
WHERE indexrelid::regclass::text ~ 'pg_textsearch';

-- Debug index internal structure (shows term dictionary and posting lists)
SELECT bm25_debug_dump_index('index_name');
```

### Configuration

Optional settings in `postgresql.conf`:

```bash
# Per-index memory limit (can be changed without restart)
pg_textsearch.index_memory_limit = 64MB      # Memory limit per index, default 64MB

# Query limit when no LIMIT clause detected
pg_textsearch.default_limit = 1000           # default 1000
```

## Examples

### Basic Search

```sql
CREATE TABLE articles (id serial PRIMARY KEY, title text, content text);
CREATE INDEX articles_idx ON articles USING bm25(content) WITH (text_config='english');

INSERT INTO articles (title, content) VALUES
    ('Database Systems', 'PostgreSQL is a powerful relational database system'),
    ('Search Technology', 'Full text search enables finding relevant documents quickly'),
    ('Information Retrieval', 'BM25 is a ranking function used in search engines');

-- Find relevant documents
SELECT title, content <@> to_bm25query('database search', 'articles_idx') as score
FROM articles
ORDER BY content <@> to_bm25query('database search', 'articles_idx');
```

Also supports different languages and custom parameters:

```sql
-- Different languages
CREATE INDEX fr_idx ON french_articles USING bm25(content) WITH (text_config='french');
CREATE INDEX de_idx ON german_articles USING bm25(content) WITH (text_config='german');

-- Custom parameters
CREATE INDEX custom_idx ON documents USING bm25(content)
    WITH (text_config='english', k1=2.0, b=0.9);
```


## Troubleshooting

```sql
-- List available text search configurations
SELECT cfgname FROM pg_ts_config;

-- List BM25 indexes
SELECT indexname FROM pg_indexes WHERE indexdef LIKE '%USING bm25%';
```


## Installation Notes

If your machine has multiple Postgres installations, specify the path to `pg_config`:

```sh
export PG_CONFIG=/Library/PostgreSQL/18/bin/pg_config  # or 17
make clean && make && make install
```

If you get compilation errors, install Postgres development files:

```sh
# Ubuntu/Debian
sudo apt install postgresql-server-dev-17  # for PostgreSQL 17
sudo apt install postgresql-server-dev-18  # for PostgreSQL 18
```

## Reference

### Index Options

Option | Type | Default | Description
--- | --- | --- | ---
text_config | string | required | PostgreSQL text search configuration to use
k1 | real | 1.2 | Term frequency saturation parameter (0.1 to 10.0)
b | real | 0.75 | Length normalization parameter (0.0 to 1.0)

### Text Search Configurations

Available configurations depend on your Postgres installation:
```
# SELECT cfgname FROM pg_ts_config;
  cfgname
------------
 simple
 arabic
 armenian
 basque
 catalan
 danish
 dutch
 english
 finnish
 french
 german
 greek
 hindi
 hungarian
 indonesian
 irish
 italian
 lithuanian
 nepali
 norwegian
 portuguese
 romanian
 russian
 serbian
 spanish
 swedish
 tamil
 turkish
 yiddish
(29 rows)
```
Further language support is available via extensions such as [zhparser](https://github.com/amutu/zhparser).

## Development

### Building and Testing

```sh
make                   # build extension
make install           # install to PostgreSQL
make installcheck      # run regression tests
make test-concurrency  # run concurrency stress tests
test/scripts/recovery.sh    # run crash recovery tests

# Code formatting
make format            # format all source files with clang-format
make lint-format       # check formatting without changing files

# Install pre-commit hooks (optional)
brew install pre-commit && pre-commit install  # macOS
# pip install pre-commit && pre-commit install  # Linux
```

### Code Style

- 79-character line limit, tab indentation
- Opening braces on new lines (Allman style)
- Consecutive alignment for variables and macros
- 2 spaces before trailing comments
- Multiline function prototypes followed by blank line
- PostgreSQL naming conventions (snake_case)
- Format with: `make format`

## Bug Reports & Feature Requests

- **Bug Reports**: [Create an issue](https://github.com/timescale/pg_textsearch/issues/new?labels=bug&template=bug_report.md)
- **Feature Requests**: [Request a feature](https://github.com/timescale/pg_textsearch/issues/new?labels=enhancement&template=feature_request.md)
- **General Discussion**: [Start a discussion](https://github.com/timescale/pg_textsearch/discussions)

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make changes with tests
4. Ensure `make installcheck` and `make test-concurrency` pass
5. Submit a pull request

All pull requests are automatically tested against PostgreSQL 17 and 18.
